/*
 * ai-http-error.h - Mapping an HTTP failure onto a GError (private)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib. It is INTERNAL: it is compiled into the
 * library but not installed and not part of the public umbrella header.
 *
 * Every provider used to map a status code onto an AiError and throw the
 * response body away, so a caller got "Request failed (HTTP 500)" and no
 * way to tell a broken model from a broken key. The body is where the
 * answer actually is -- ollama reports a model that crashes llama-server
 * as `llama-server process has terminated: GGML_ASSERT(...) failed', and
 * that names the problem where the status code only says something went
 * wrong somewhere.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-http-error.h is private to ai-glib and cannot be included directly."
#endif

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * How much of an unparseable body to quote. Long enough for a real
 * message, short enough that an HTML error page does not become the
 * error string.
 */
#define AI_HTTP_ERROR_EXCERPT 400

/**
 * ai_http_error_extract_message:
 * @body: (nullable): the raw response body
 * @body_len: its length in bytes
 *
 * Digs the provider's own error message out of @body.
 *
 * Understands the shapes the providers actually send -- `{"error":
 * {"message": ...}}` (OpenAI, Grok, Ollama, Gemini), `{"error": "..."}`
 * (Ollama's native endpoints), and a top-level `{"message": ...}` -- and
 * falls back to a stripped, bounded excerpt of the raw bytes when the
 * body is not JSON or carries no message. An HTML error page from a
 * proxy therefore still says something, rather than nothing.
 *
 * Returns: (transfer full) (nullable): the message, or %NULL when @body
 *   is empty or yields nothing worth showing
 */
gchar *
ai_http_error_extract_message(
    const gchar *body,
    gsize        body_len
);

/**
 * ai_http_error_set:
 * @error: (out) (optional): return location for the error
 * @what: (nullable): what was being attempted, e.g. "Model listing failed"
 * @status: the HTTP status code
 * @body: (nullable): the raw response body
 * @body_len: its length in bytes
 *
 * Sets @error from @status, appending the provider's own message from
 * @body when there is one.
 *
 * The status still picks the #AiError code, because callers switch on it
 * -- 401 stays %AI_ERROR_INVALID_API_KEY whatever the body says. The body
 * only ever adds text.
 *
 * @what names the request for the failure branches, so a model listing
 * says so instead of reporting a bare "Request failed". %NULL gets the
 * generic wording. Authentication and rate-limit failures ignore it:
 * what failed there is the key or the quota, not the particular call.
 */
void
ai_http_error_set(
    GError      **error,
    const gchar  *what,
    guint         status,
    const gchar  *body,
    gsize         body_len
);

/**
 * ai_http_error_set_from_bytes:
 * @error: (out) (optional): return location for the error
 * @status: the HTTP status code
 * @body: (nullable): the response body
 *
 * #GBytes convenience for ai_http_error_set(). A %NULL @body is the same
 * as an empty one.
 */
void
ai_http_error_set_from_bytes(
    GError      **error,
    const gchar  *what,
    guint         status,
    GBytes       *body
);

/**
 * ai_http_error_set_from_stream:
 * @error: (out) (optional): return location for the error
 * @status: the HTTP status code
 * @stream: (nullable): the response body stream, positioned at the start
 * @cancellable: (nullable): a #GCancellable
 *
 * ai_http_error_set() for the streaming paths, which hold an open
 * #GInputStream rather than a body they have already read.
 *
 * This is where a streamed chat reports a failure, so it is the one that
 * matters most in practice -- a model that crashes the server fails here,
 * not on the non-streaming path. Does a single bounded read rather than
 * draining to EOF: an error body is small and already complete by the
 * time the status is known, and one read cannot be held open by a server
 * that stops sending.
 */
void
ai_http_error_set_from_stream(
    GError       **error,
    const gchar   *what,
    guint          status,
    GInputStream  *stream,
    GCancellable  *cancellable
);

G_END_DECLS
