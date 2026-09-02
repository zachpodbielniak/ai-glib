/*
 * ai-http-error.c - Mapping an HTTP failure onto a GError (private)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include <json-glib/json-glib.h>

#include "core/ai-http-error.h"
#include "core/ai-error.h"
#include "core/ai-json-util.h"

/*
 * A bounded, whitespace-stripped copy of the raw body, used when the body
 * is not JSON or has no message in it. Returns NULL rather than an empty
 * string so callers have one thing to test.
 */
static gchar *
ai_http_error__excerpt(const gchar *body, gsize body_len)
{
    gchar *excerpt;

    if (body == NULL || body_len == 0)
        return NULL;

    excerpt = g_strndup(body, MIN(body_len, (gsize) AI_HTTP_ERROR_EXCERPT));
    g_strstrip(excerpt);

    if (excerpt[0] == '\0')
    {
        g_free(excerpt);
        return NULL;
    }

    return excerpt;
}

gchar *
ai_http_error_extract_message(
    const gchar *body,
    gsize        body_len
){
    g_autoptr(JsonParser) parser = NULL;
    JsonObject           *obj;
    const gchar          *msg = NULL;

    if (body == NULL || body_len == 0)
        return NULL;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, (gssize) body_len, NULL))
        return ai_http_error__excerpt(body, body_len);

    obj = ai_json_root_object(parser);
    if (obj == NULL)
        return ai_http_error__excerpt(body, body_len);

    /*
     * Presence, not shape, decides an error envelope: a server answering
     * {"error": "boom"} has still said it failed, and reading that as an
     * object to find out would be the critical this file exists to avoid.
     */
    if (json_object_has_member(obj, "error"))
    {
        JsonNode *err = json_object_get_member(obj, "error");

        /*
         * Two shapes under the same key. OpenAI, Grok, Gemini and
         * ollama's OpenAI-compatible endpoint nest an object with a
         * `message'; ollama's native endpoints put a bare string there.
         */
        if (JSON_NODE_HOLDS_OBJECT(err))
        {
            msg = ai_json_get_string(json_node_get_object(err),
                                     "message", NULL);
        }
        else if (JSON_NODE_HOLDS_VALUE(err) &&
                 json_node_get_value_type(err) == G_TYPE_STRING)
        {
            msg = json_node_get_string(err);
        }
    }

    /* Anthropic sends {"type":"error","error":{...}}, already handled
     * above; a few services put the message at the top level instead. */
    if (msg == NULL)
        msg = ai_json_get_string(obj, "message", NULL);

    if (msg == NULL || msg[0] == '\0')
        return ai_http_error__excerpt(body, body_len);

    /*
     * Bounded like the raw excerpt: a provider is free to return a
     * multi-kilobyte message and it should not become the error string.
     */
    {
        gchar *out = g_strndup(msg, AI_HTTP_ERROR_EXCERPT);

        g_strstrip(out);
        if (out[0] == '\0')
        {
            g_free(out);
            return ai_http_error__excerpt(body, body_len);
        }
        return out;
    }
}

void
ai_http_error_set(
    GError      **error,
    const gchar  *what,
    guint         status,
    const gchar  *body,
    gsize         body_len
){
    g_autofree gchar *detail = NULL;
    const gchar      *sep;
    const gchar      *text;

    detail = ai_http_error_extract_message(body, body_len);
    sep    = (detail != NULL) ? ": " : "";
    text   = (detail != NULL) ? detail : "";

    /*
     * The status picks the code, not the body. Callers switch on
     * AI_ERROR_INVALID_API_KEY to decide whether to re-prompt for a key,
     * and a provider wording its 401 unusually must not change that.
     */
    if (status == 401 || status == 403)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                    "Authentication failed (HTTP %u)%s%s", status, sep, text);
    }
    else if (status == 429)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED,
                    "Rate limited (HTTP %u)%s%s", status, sep, text);
    }
    else if (status >= 500)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                    "%s (HTTP %u)%s%s",
                    what != NULL ? what : "Server error", status, sep, text);
    }
    else
    {
        g_set_error(error, AI_ERROR, AI_ERROR_NETWORK_ERROR,
                    "%s (HTTP %u)%s%s",
                    what != NULL ? what : "Request failed", status, sep, text);
    }
}

void
ai_http_error_set_from_bytes(
    GError      **error,
    const gchar  *what,
    guint         status,
    GBytes       *body
){
    gconstpointer data = NULL;
    gsize         len  = 0;

    if (body != NULL)
        data = g_bytes_get_data(body, &len);

    ai_http_error_set(error, what, status, (const gchar *) data, len);
}

void
ai_http_error_set_from_stream(
    GError       **error,
    const gchar   *what,
    guint          status,
    GInputStream  *stream,
    GCancellable  *cancellable
){
    gchar   buf[AI_HTTP_ERROR_EXCERPT * 2];
    gssize  got = 0;

    /*
     * One read, not a drain. The body of an error response is small and
     * has already arrived by the time the status line has been parsed,
     * so a single read gets all of it in practice; looping to EOF would
     * hand a server that sends a byte and stalls the ability to block
     * this path indefinitely. A truncated message still names the
     * problem, which is the whole point.
     */
    if (stream != NULL)
    {
        got = g_input_stream_read(stream, buf, sizeof buf, cancellable, NULL);
        if (got < 0)
            got = 0;
    }

    ai_http_error_set(error, what, status, got > 0 ? buf : NULL, (gsize) got);
}
