/*
 * test-http-error.c - Surfacing the server's own error message
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * These all exist because "Request failed (HTTP 500)" was reported as a
 * bug in the *client*. The status code says something went wrong; the
 * body says what, and every provider was throwing it away.
 */

#include <glib.h>
#include <gio/gio.h>

#include <string.h>

#include "core/ai-http-error.h"
#include "core/ai-error.h"

/* ------------------------------------------------------------------ */
/* Extraction                                                          */
/* ------------------------------------------------------------------ */

/*
 * The shape OpenAI, Grok, Gemini and ollama's OpenAI-compatible endpoint
 * all send. This is the body that started it: a model whose GGUF crashes
 * llama-server, reported by ollama as a 500 whose message names the
 * assert. Without it the user sees a bare 500 and reasonably concludes
 * the editor is broken.
 */
static void
test_extract_nested_message (void)
{
  const gchar *body =
    "{\"error\":{\"message\":\"llama-server process has terminated: "
    "GGML_ASSERT(a->ne[2] == b->ne[2]) failed\",\"type\":\"api_error\","
    "\"param\":null,\"code\":null}}";
  g_autofree gchar *msg = ai_http_error_extract_message (body, strlen (body));

  g_assert_nonnull (msg);
  g_assert_true (g_str_has_prefix (msg, "llama-server process has terminated"));
  /* The JSON scaffolding is not part of the message. */
  g_assert_null (strstr (msg, "api_error"));
  g_assert_null (strstr (msg, "{"));
}

/* ollama's native endpoints put a bare string under the same key. */
static void
test_extract_bare_string_error (void)
{
  const gchar *body = "{\"error\":\"model 'nope:9b' not found\"}";
  g_autofree gchar *msg = ai_http_error_extract_message (body, strlen (body));

  g_assert_cmpstr (msg, ==, "model 'nope:9b' not found");
}

static void
test_extract_anthropic_shape (void)
{
  const gchar *body =
    "{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
    "\"message\":\"max_tokens: must be greater than 0\"}}";
  g_autofree gchar *msg = ai_http_error_extract_message (body, strlen (body));

  g_assert_cmpstr (msg, ==, "max_tokens: must be greater than 0");
}

static void
test_extract_top_level_message (void)
{
  const gchar *body = "{\"message\":\"upstream connect error\"}";
  g_autofree gchar *msg = ai_http_error_extract_message (body, strlen (body));

  g_assert_cmpstr (msg, ==, "upstream connect error");
}

/*
 * A proxy or load balancer in front of a provider answers HTML, not JSON.
 * Saying nothing would be worse than saying something ugly -- the point is
 * to tell the user where the failure came from.
 */
static void
test_extract_falls_back_to_excerpt (void)
{
  const gchar *body = "<html><head><title>502 Bad Gateway</title></head>";
  g_autofree gchar *msg = ai_http_error_extract_message (body, strlen (body));

  g_assert_nonnull (msg);
  g_assert_nonnull (strstr (msg, "502 Bad Gateway"));
}

static void
test_extract_empty_is_null (void)
{
  g_assert_null (ai_http_error_extract_message (NULL, 0));
  g_assert_null (ai_http_error_extract_message ("", 0));
  /* Whitespace only is nothing worth showing, not "   ". */
  g_assert_null (ai_http_error_extract_message ("   \n\t ", 6));
}

/*
 * An error body is untrusted input. json-glib's *_with_default() accessors
 * emit a CRITICAL on a type mismatch, which is fatal under
 * G_DEBUG=fatal-warnings -- so a provider answering with the wrong type
 * could abort the process on the way to reporting its own error.
 */
static void
test_extract_survives_wrong_types (void)
{
  const gchar *cases[] = {
    "{\"error\":{\"message\":500}}",
    "{\"error\":{\"message\":null}}",
    "{\"error\":[1,2,3]}",
    "{\"error\":{}}",
    "null",
    "[]",
    "{\"message\":false}",
  };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      /* Must not abort; the value itself is allowed to be anything. */
      g_autofree gchar *msg =
        ai_http_error_extract_message (cases[i], strlen (cases[i]));
      (void) msg;
    }
}

/* A provider is free to return a novel; it must not become the error. */
static void
test_extract_is_bounded (void)
{
  g_autofree gchar *big = g_strnfill (8000, 'x');
  g_autofree gchar *body = g_strdup_printf ("{\"error\":{\"message\":\"%s\"}}", big);
  g_autofree gchar *msg = ai_http_error_extract_message (body, strlen (body));

  g_assert_nonnull (msg);
  g_assert_cmpuint (strlen (msg), <=, AI_HTTP_ERROR_EXCERPT);
}

/* ------------------------------------------------------------------ */
/* Status mapping                                                      */
/* ------------------------------------------------------------------ */

static void
test_set_appends_the_message (void)
{
  g_autoptr (GError) error = NULL;
  const gchar *body = "{\"error\":{\"message\":\"model 'x' not found\"}}";

  ai_http_error_set (&error, NULL, 404, body, strlen (body));

  g_assert_error (error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
  /* Both halves: the status for triage, the message for the cause. */
  g_assert_nonnull (strstr (error->message, "404"));
  g_assert_nonnull (strstr (error->message, "model 'x' not found"));
}

/*
 * The body only ever adds text. Callers switch on the code to decide
 * whether to re-prompt for a key, so a provider wording its 401 unusually
 * must not be able to change what the error *is*.
 */
static void
test_status_still_picks_the_code (void)
{
  struct { guint status; gint code; } cases[] = {
    { 401, AI_ERROR_INVALID_API_KEY },
    { 403, AI_ERROR_INVALID_API_KEY },
    { 429, AI_ERROR_RATE_LIMITED },
    { 500, AI_ERROR_SERVER_ERROR },
    { 503, AI_ERROR_SERVER_ERROR },
    { 400, AI_ERROR_NETWORK_ERROR },
    { 404, AI_ERROR_NETWORK_ERROR },
  };
  gsize i;
  const gchar *body = "{\"error\":{\"message\":\"whatever it says\"}}";

  for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      g_autoptr (GError) error = NULL;

      ai_http_error_set (&error, NULL, cases[i].status, body, strlen (body));
      g_assert_error (error, AI_ERROR, cases[i].code);
      g_assert_nonnull (strstr (error->message, "whatever it says"));
    }
}

/* No body is the old behaviour exactly, with no dangling separator. */
static void
test_set_without_a_body (void)
{
  g_autoptr (GError) error = NULL;

  ai_http_error_set (&error, NULL, 500, NULL, 0);

  g_assert_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR);
  g_assert_cmpstr (error->message, ==, "Server error (HTTP 500)");
}

/*
 * `what' says which request died, so a model listing does not report a
 * bare "Request failed" -- but an auth failure is about the key, not the
 * call, so it keeps its own wording.
 */
static void
test_what_names_the_request (void)
{
  g_autoptr (GError) listing = NULL;
  g_autoptr (GError) auth = NULL;

  ai_http_error_set (&listing, "Model listing failed", 500, NULL, 0);
  g_assert_cmpstr (listing->message, ==, "Model listing failed (HTTP 500)");

  ai_http_error_set (&auth, "Model listing failed", 401, NULL, 0);
  g_assert_nonnull (strstr (auth->message, "Authentication failed"));
}

static void
test_set_from_bytes_handles_null (void)
{
  g_autoptr (GError) error = NULL;

  ai_http_error_set_from_bytes (&error, NULL, 500, NULL);

  g_assert_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR);
}

static void
test_set_from_stream (void)
{
  const gchar *body = "{\"error\":{\"message\":\"it broke\"}}";
  g_autoptr (GInputStream) stream =
    g_memory_input_stream_new_from_data (body, strlen (body), NULL);
  g_autoptr (GError) error = NULL;

  ai_http_error_set_from_stream (&error, NULL, 500, stream, NULL);

  g_assert_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR);
  g_assert_nonnull (strstr (error->message, "it broke"));
}

/* The streaming paths pass whatever they have, including nothing. */
static void
test_set_from_null_stream (void)
{
  g_autoptr (GError) error = NULL;

  ai_http_error_set_from_stream (&error, NULL, 502, NULL, NULL);

  g_assert_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR);
  g_assert_cmpstr (error->message, ==, "Server error (HTTP 502)");
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/http-error/extract/nested", test_extract_nested_message);
  g_test_add_func ("/http-error/extract/bare-string", test_extract_bare_string_error);
  g_test_add_func ("/http-error/extract/anthropic", test_extract_anthropic_shape);
  g_test_add_func ("/http-error/extract/top-level", test_extract_top_level_message);
  g_test_add_func ("/http-error/extract/excerpt", test_extract_falls_back_to_excerpt);
  g_test_add_func ("/http-error/extract/empty", test_extract_empty_is_null);
  g_test_add_func ("/http-error/extract/wrong-types", test_extract_survives_wrong_types);
  g_test_add_func ("/http-error/extract/bounded", test_extract_is_bounded);

  g_test_add_func ("/http-error/set/appends", test_set_appends_the_message);
  g_test_add_func ("/http-error/set/code-from-status", test_status_still_picks_the_code);
  g_test_add_func ("/http-error/set/no-body", test_set_without_a_body);
  g_test_add_func ("/http-error/set/what", test_what_names_the_request);
  g_test_add_func ("/http-error/set/bytes-null", test_set_from_bytes_handles_null);
  g_test_add_func ("/http-error/set/stream", test_set_from_stream);
  g_test_add_func ("/http-error/set/null-stream", test_set_from_null_stream);

  return g_test_run ();
}
