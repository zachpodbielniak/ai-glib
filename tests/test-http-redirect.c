/*
 * test-http-redirect.c - Provider redirects must not disclose credentials or prompts
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ai-glib.h"
#include "convenience/ai-search-http.h"
#include "core/ai-http-error.h"
#include "core/ai-client-private.h"
#include "core/ai-http-redirect-private.h"
#include "test-server.h"

typedef struct
{
    AiProviderType provider;
    const gchar *name;
    GType (*get_type) (void);
    const gchar *header;
    const gchar *credential;
} RedirectProvider;

static const RedirectProvider providers[] = {
    { AI_PROVIDER_CLAUDE, "claude", ai_claude_client_get_type, "x-api-key", "redirect-test-key" },
    { AI_PROVIDER_GEMINI, "gemini", ai_gemini_client_get_type, "x-goog-api-key", "redirect-test-key" },
    { AI_PROVIDER_OPENAI, "openai", ai_openai_client_get_type, "Authorization", "Bearer redirect-test-key" },
    { AI_PROVIDER_GROK, "grok", ai_grok_client_get_type, "Authorization", "Bearer redirect-test-key" },
    { AI_PROVIDER_OLLAMA, "ollama", ai_ollama_client_get_type, "Authorization", NULL }
};

typedef struct
{
    gboolean done;
    AiResponse *response;
    GError *error;
} ChatResult;

static void
chat_finished (GObject *source, GAsyncResult *result, gpointer user_data)
{
    ChatResult *chat = user_data;

    chat->response = ai_provider_chat_finish (AI_PROVIDER (source), result, &chat->error);
    chat->done = TRUE;
}

static void
check_redirect (const RedirectProvider *provider, gboolean same_origin, gboolean async,
                guint status)
{
    TServer *origin = tserver_new ();
    TServer *target = same_origin ? origin : tserver_new ();
    g_autoptr(AiConfig) config = ai_config_new ();
    g_autoptr(AiClient) client = NULL;
    g_autoptr(AiMessage) message = ai_message_new_user ("private conversation");
    g_autofree gchar *auth = NULL;
    GList messages = { message, NULL, NULL };
    ChatResult chat = { FALSE, NULL, NULL };

    /* A body accepted by all five response parsers. */
    tserver_set_response (target, 200,
        "{\"id\":\"reply\",\"model\":\"test\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],"
        "\"choices\":[{\"message\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}],"
        "\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"ok\"}]}}],"
        "\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"done\":true}");
    g_mutex_lock (&origin->lock);
    origin->redirect_to = g_strconcat (target->base_url, "/redirect-target", NULL);
    origin->redirect_status = status;
    g_mutex_unlock (&origin->lock);

    ai_config_set_base_url (config, provider->provider, origin->base_url);
    ai_config_set_api_key (config, provider->provider, "redirect-test-key");
    ai_config_set_max_retries (config, 0);
    client = g_object_new (provider->get_type (), "config", config, NULL);

    if (async)
    {
        ai_provider_chat_async (AI_PROVIDER (client), &messages, NULL, 64,
                                NULL, NULL, chat_finished, &chat);
        while (!chat.done)
            g_main_context_iteration (NULL, TRUE);
    }
    else
        chat.response = ai_client_chat_sync (client, &messages, NULL, &chat.error);

    if (same_origin && provider->credential != NULL)
    {
        /*
         * A request with a key is not redirected even to its own origin:
         * a 307/308 would replay the key and the prompt to a path nobody
         * configured.  The caller is told so rather than "HTTP 307".
         */
        g_assert_null (chat.response);
        g_assert_error (chat.error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
        g_assert_nonnull (strstr (chat.error->message, "Redirect refused"));
        g_assert_cmpuint (tserver_hits (origin), ==, 1);
    }
    else if (same_origin && status >= 307)
    {
        /* libsoup itself does not resend a POST on a 307/308. */
        g_assert_null (chat.response);
        g_assert_cmpuint (tserver_hits (origin), ==, 1);
    }
    else if (same_origin)
    {
        /* Nothing to leak: a same-origin hop is still followed. */
        g_assert_no_error (chat.error);
        g_assert_nonnull (chat.response);
        g_assert_cmpuint (tserver_hits (target), ==, 2);
        auth = tserver_dup_header (target, provider->header);
        g_assert_cmpstr (auth, ==, provider->credential);
    }
    else
    {
        auth = tserver_dup_header (target, provider->header);
        g_assert_null (auth);
        g_assert_cmpuint (tserver_hits (target), ==, 0);
        g_assert_cmpuint (tserver_hits (origin), ==, 1);
        auth = tserver_dup_header (origin, provider->header);
        g_assert_cmpstr (auth, ==, provider->credential);
        g_assert_null (chat.response);
        g_assert_nonnull (chat.error);
    }

    g_clear_object (&chat.response);
    g_clear_error (&chat.error);
    if (!same_origin)
        tserver_free (target);
    tserver_free (origin);
}

static void cross_sync (gconstpointer p) { check_redirect (p, FALSE, FALSE, 0); }
static void cross_async (gconstpointer p) { check_redirect (p, FALSE, TRUE, 0); }
static void same_sync (gconstpointer p) { check_redirect (p, TRUE, FALSE, 0); }
static void same_async (gconstpointer p) { check_redirect (p, TRUE, TRUE, 0); }
/* 307 and 308 resend the body: the prompt goes with the key. */
static void same_307 (gconstpointer p) { check_redirect (p, TRUE, TRUE, 307); }
static void same_308 (gconstpointer p) { check_redirect (p, TRUE, FALSE, 308); }

static void
test_search_redirect (gconstpointer data)
{
    const gchar *header = data;
    const gchar *headers[] = { header, "search-test-key", NULL };
    TServer *origin = tserver_new ();
    TServer *target = tserver_new ();
    g_autoptr(SoupSession) session = soup_session_new ();
    g_autoptr(JsonNode) result = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *auth = NULL;

    g_mutex_lock (&origin->lock);
    origin->redirect_to = g_strconcat (target->base_url, "/redirect-target", NULL);
    g_mutex_unlock (&origin->lock);
    result = ai_search_http_get_json (session, origin->base_url, headers, NULL, &error);
    auth = tserver_dup_header (target, header);
    g_assert_null (auth);
    g_assert_cmpuint (tserver_hits (target), ==, 0);
    g_assert_null (result);
    g_assert_nonnull (error);
    g_clear_error (&error);

    /*
     * A relative redirect on the same origin is refused too while the
     * request carries its key, and the error says why.
     */
    g_mutex_lock (&origin->lock);
    g_free (origin->redirect_to);
    origin->redirect_to = g_strdup ("/redirect-target");
    g_mutex_unlock (&origin->lock);
    result = ai_search_http_get_json (session, origin->base_url, headers, NULL, &error);
    g_assert_null (result);
    g_assert_error (error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
    g_assert_nonnull (strstr (error->message, "never redirected"));
    g_clear_error (&error);
    g_assert_cmpuint (tserver_hits (origin), ==, 2);

    /* Without a key the same hop is followed. */
    result = ai_search_http_get_json (session, origin->base_url, NULL, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (result);
    g_clear_pointer (&result, json_node_unref);
    g_assert_cmpuint (tserver_hits (origin), ==, 4);
    tserver_free (target);
    tserver_free (origin);
}

/* ------------------------------------------------------------------ */
/* The decision, without a network                                     */
/* ------------------------------------------------------------------ */

static void
test_decision (void)
{
    g_autoptr(GUri) https = g_uri_parse ("https://api.example.com/v1/chat", SOUP_HTTP_URI_FLAGS, NULL);
    g_autoptr(GUri) http = g_uri_parse ("http://127.0.0.1:8087/api", SOUP_HTTP_URI_FLAGS, NULL);

    /* With credentials: never, whatever the target. */
    g_assert_false (ai_http_redirect_allowed (https, "/v1/chat/", TRUE));
    g_assert_false (ai_http_redirect_allowed (https, "https://api.example.com/v2", TRUE));
    g_assert_false (ai_http_redirect_allowed (https, "https://evil.example/", TRUE));

    /* Without: same scheme, host and port only. */
    g_assert_true (ai_http_redirect_allowed (https, "/v1/chat/", FALSE));
    g_assert_true (ai_http_redirect_allowed (https, "https://API.example.com:443/v2", FALSE));
    g_assert_false (ai_http_redirect_allowed (https, "http://api.example.com/v1", FALSE));
    g_assert_false (ai_http_redirect_allowed (https, "https://api.example.com:8443/", FALSE));
    g_assert_false (ai_http_redirect_allowed (https, "https://evil.example/", FALSE));
    g_assert_true (ai_http_redirect_allowed (http, "http://127.0.0.1:8087/x", FALSE));
    g_assert_false (ai_http_redirect_allowed (http, "http://127.0.0.1:8088/x", FALSE));
    g_assert_false (ai_http_redirect_allowed (http, "http://[::1", FALSE));
    g_assert_false (ai_http_redirect_allowed (http, NULL, FALSE));
}

static void
test_credentials_detected (void)
{
    static const gchar *credential_headers[] = {
        "Authorization", "Proxy-Authorization", "Cookie", "x-api-key",
        "x-goog-api-key", "api-key", "X-Subscription-Token",
        "Ocp-Apim-Subscription-Key", "X-Auth-Token", "X-Client-Secret", NULL
    };
    static const gchar *plain_headers[] = {
        "User-Agent", "Accept", "Content-Type", "anthropic-version",
        "TypeSafe-Danger-Zone-Long-Context", NULL
    };
    g_autoptr(GUri) plain = g_uri_parse ("https://h.example/", SOUP_HTTP_URI_FLAGS, NULL);
    g_autoptr(GUri) userinfo = g_uri_parse ("https://u:pw@h.example/", SOUP_HTTP_URI_FLAGS, NULL);
    guint i;

    for (i = 0; credential_headers[i] != NULL; i++)
    {
        SoupMessageHeaders *h = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);

        soup_message_headers_replace (h, credential_headers[i], "secret");
        g_assert_true (ai_http_request_has_credentials (h, plain));
        soup_message_headers_unref (h);
    }

    {
        SoupMessageHeaders *h = soup_message_headers_new (SOUP_MESSAGE_HEADERS_REQUEST);

        for (i = 0; plain_headers[i] != NULL; i++)
            soup_message_headers_replace (h, plain_headers[i], "value");
        g_assert_false (ai_http_request_has_credentials (h, plain));
        g_assert_true (ai_http_request_has_credentials (h, userinfo));
        soup_message_headers_unref (h);
    }

    g_assert_false (ai_http_request_has_credentials (NULL, NULL));
}

/* The status-only error path providers use names the refusal. */
static void
test_status_error_names_refusal (void)
{
    g_autoptr(GError) error = NULL;

    ai_http_error_set (&error, NULL, 307, NULL, 0);
    g_assert_error (error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
    g_assert_nonnull (strstr (error->message, "Redirect refused (HTTP 307)"));
}

/*
 * A credential the headers do not show (ai-call's OpenID token in a JSON
 * body) is declared, and the guarded session then refuses even a
 * same-origin 307 for it.
 */
static void
test_marked_message_is_refused (void)
{
    TServer *origin = tserver_new ();
    g_autoptr(SoupSession) session = soup_session_new ();
    g_autoptr(SoupMessage) msg = NULL;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GBytes) reply = NULL;
    g_autoptr(GError) error = NULL;

    ai_http_guard_session (session);
    tserver_set_response (origin, 200, "{}");
    g_mutex_lock (&origin->lock);
    origin->redirect_to = g_strdup ("/redirect-target");
    origin->redirect_status = 307;
    g_mutex_unlock (&origin->lock);

    msg = soup_message_new ("GET", origin->base_url);
    body = g_bytes_new_static ("{\"token\":\"openid\"}", 18);
    soup_message_set_request_body_from_bytes (msg, "application/json", body);
    ai_http_message_mark_credentials (msg);
    reply = soup_session_send_and_read (session, msg, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (soup_message_get_status (msg), ==, 307);
    g_assert_true (ai_http_redirect_refused (msg, &error));
    g_assert_error (error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
    g_assert_nonnull (strstr (error->message, "never redirected"));
    g_assert_cmpuint (tserver_hits (origin), ==, 1);
    g_clear_error (&error);

    /*
     * Unmarked and keyless, the same hop is followed (a GET: libsoup
     * itself declines to follow a 307 for a POST).
     */
    g_clear_object (&msg);
    g_clear_pointer (&reply, g_bytes_unref);
    msg = soup_message_new ("GET", origin->base_url);
    reply = soup_session_send_and_read (session, msg, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (soup_message_get_status (msg), ==, 200);
    g_assert_false (ai_http_redirect_refused (msg, NULL));
    g_assert_cmpuint (tserver_hits (origin), ==, 3);
    tserver_free (origin);
}

/* ------------------------------------------------------------------ */
/* libresearch's async requests                                        */
/* ------------------------------------------------------------------ */

typedef struct
{
    gboolean  done;
    JsonNode *node;
    GError   *error;
} LrResult;

static void
lr_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    LrResult *lr = user_data;

    lr->node = ai_libresearch_search_request_finish (AI_LIBRESEARCH_SEARCH (source),
                                                     result, &lr->error);
    lr->done = TRUE;
}

static void
lr_search (TServer *origin, const gchar *key, LrResult *lr)
{
    g_autoptr(AiLibresearchSearch) lrs = ai_libresearch_search_new (origin->base_url, key);

    lr->done = FALSE;
    ai_libresearch_search_web_async (lrs, "q", NULL, NULL, NULL, lr_done, lr);
    while (!lr->done)
        g_main_context_iteration (NULL, TRUE);
}

static void
test_libresearch_async (gconstpointer data)
{
    guint status = GPOINTER_TO_UINT (data);
    TServer *origin = tserver_new ();
    TServer *target = tserver_new ();
    LrResult lr = { FALSE, NULL, NULL };
    g_autofree gchar *auth = NULL;

    tserver_set_response (target, 200, "{\"results\":[]}");
    tserver_set_response (origin, 200, "{\"results\":[]}");

    /* Cross-origin with the bearer: refused, nothing reaches the target. */
    g_mutex_lock (&origin->lock);
    origin->redirect_to = g_strconcat (target->base_url, "/redirect-target", NULL);
    origin->redirect_status = status;
    g_mutex_unlock (&origin->lock);
    lr_search (origin, "lr-test-token", &lr);
    g_assert_null (lr.node);
    g_assert_error (lr.error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
    g_assert_nonnull (strstr (lr.error->message, "Redirect refused"));
    g_clear_error (&lr.error);
    g_assert_cmpuint (tserver_hits (target), ==, 0);
    auth = tserver_dup_header (target, "Authorization");
    g_assert_null (auth);

    /* Same origin with the bearer: refused as well. */
    g_mutex_lock (&origin->lock);
    g_free (origin->redirect_to);
    origin->redirect_to = g_strdup ("/redirect-target");
    g_mutex_unlock (&origin->lock);
    lr_search (origin, "lr-test-token", &lr);
    g_assert_null (lr.node);
    g_assert_error (lr.error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
    g_assert_nonnull (strstr (lr.error->message, "never redirected"));
    g_clear_error (&lr.error);
    g_assert_cmpuint (tserver_hits (origin), ==, 2);

    /* Same origin without one: followed. */
    lr_search (origin, NULL, &lr);
    g_assert_no_error (lr.error);
    g_assert_nonnull (lr.node);
    g_clear_pointer (&lr.node, json_node_unref);
    g_assert_cmpuint (tserver_hits (origin), ==, 4);

    tserver_free (target);
    tserver_free (origin);
}

/* ------------------------------------------------------------------ */
/* The constructors every call site goes through                        */
/* ------------------------------------------------------------------ */

/* Points @origin's redirect at @target (or at its own path when NULL). */
static void
point_redirect (TServer *origin, TServer *target, guint status)
{
    g_mutex_lock (&origin->lock);
    g_free (origin->redirect_to);
    origin->redirect_to = target != NULL
        ? g_strconcat (target->base_url, "/redirect-target", NULL)
        : g_strdup ("/redirect-target");
    origin->redirect_status = status;
    g_mutex_unlock (&origin->lock);
}

/* Sends @msg on @session (sync GET) and returns the status. */
static guint
send_get (SoupSession *session, SoupMessage *msg)
{
    g_autoptr(GBytes) reply = NULL;
    g_autoptr(GError) error = NULL;

    reply = soup_session_send_and_read (session, msg, NULL, &error);
    g_assert_no_error (error);
    return soup_message_get_status (msg);
}

/*
 * AI_HTTP_CARRIES_CREDENTIALS is decided before the request has any
 * header: the flag is on the message the constructor returns, and the
 * request is refused even on a session nobody guarded.  Zero is that
 * value, so a forgotten argument refuses.
 */
static void
test_constructor_carries_credentials (void)
{
    TServer *origin = tserver_new ();
    g_autoptr(SoupSession) bare = soup_session_new ();
    g_autoptr(SoupMessage) msg = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_cmpint (AI_HTTP_CARRIES_CREDENTIALS, ==, 0);
    tserver_set_response (origin, 200, "{}");
    point_redirect (origin, NULL, 302);

    msg = ai_http_message_new ("GET", origin->base_url, AI_HTTP_CARRIES_CREDENTIALS);
    g_assert_nonnull (msg);
    g_assert_true ((soup_message_get_flags (msg) & SOUP_MESSAGE_NO_REDIRECT) != 0);

    g_assert_cmpuint (send_get (bare, msg), ==, 302);
    g_assert_true (ai_http_redirect_refused (msg, &error));
    g_assert_nonnull (strstr (error->message, "never redirected"));
    g_assert_cmpuint (tserver_hits (origin), ==, 1);
    tserver_free (origin);
}

/* AI_HTTP_NO_CREDENTIALS: followed on its origin, refused off it. */
static void
test_constructor_no_credentials (void)
{
    TServer *origin = tserver_new ();
    TServer *target = tserver_new ();
    g_autoptr(SoupSession) session = ai_http_session_new ();
    g_autoptr(SoupMessage) msg = NULL;
    g_autoptr(GError) error = NULL;

    tserver_set_response (origin, 200, "{}");
    tserver_set_response (target, 200, "{}");

    point_redirect (origin, NULL, 302);
    msg = ai_http_message_new ("GET", origin->base_url, AI_HTTP_NO_CREDENTIALS);
    g_assert_true ((soup_message_get_flags (msg) & SOUP_MESSAGE_NO_REDIRECT) == 0);
    g_assert_cmpuint (send_get (session, msg), ==, 200);
    g_assert_cmpuint (tserver_hits (origin), ==, 2);

    g_clear_object (&msg);
    point_redirect (origin, target, 302);
    msg = ai_http_message_new ("GET", origin->base_url, AI_HTTP_NO_CREDENTIALS);
    g_assert_cmpuint (send_get (session, msg), ==, 302);
    g_assert_true (ai_http_redirect_refused (msg, &error));
    g_assert_nonnull (strstr (error->message, "leaves the request's origin"));
    g_assert_cmpuint (tserver_hits (target), ==, 0);

    tserver_free (target);
    tserver_free (origin);
}

/*
 * AI_HTTP_PUBLIC_URL follows a hop to another host -- a download link
 * to its CDN -- unless a credential header is on it when it is sent.
 */
static void
test_constructor_public_url (void)
{
    TServer *origin = tserver_new ();
    TServer *target = tserver_new ();
    g_autoptr(SoupSession) session = ai_http_session_new ();
    g_autoptr(SoupMessage) msg = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *auth = NULL;

    tserver_set_response (origin, 200, "{}");
    tserver_set_response (target, 200, "{}");
    point_redirect (origin, target, 302);

    msg = ai_http_message_new ("GET", origin->base_url, AI_HTTP_PUBLIC_URL);
    g_assert_cmpuint (send_get (session, msg), ==, 200);
    g_assert_cmpuint (tserver_hits (target), ==, 1);
    g_assert_false (ai_http_redirect_refused (msg, NULL));

    /* The same URL with a key added after construction is refused. */
    g_clear_object (&msg);
    msg = ai_http_message_new ("GET", origin->base_url, AI_HTTP_PUBLIC_URL);
    soup_message_headers_replace (soup_message_get_request_headers (msg),
                                  "Authorization", "Bearer public-test-key");
    g_assert_cmpuint (send_get (session, msg), ==, 302);
    g_assert_true (ai_http_redirect_refused (msg, &error));
    g_assert_nonnull (strstr (error->message, "never redirected"));
    g_assert_cmpuint (tserver_hits (target), ==, 1);
    auth = tserver_dup_header (target, "Authorization");
    g_assert_null (auth);

    tserver_free (target);
    tserver_free (origin);
}

/* An unparseable URL is NULL, as from soup_message_new(). */
static void
test_constructor_bad_url (void)
{
    g_autoptr(GUri) uri = g_uri_parse ("https://example.invalid/x", G_URI_FLAGS_NONE, NULL);
    g_autoptr(SoupMessage) msg = NULL;

    g_assert_null (ai_http_message_new ("GET", "not a url", AI_HTTP_NO_CREDENTIALS));

    msg = ai_http_message_new_from_uri ("POST", uri, AI_HTTP_CARRIES_CREDENTIALS);
    g_assert_nonnull (msg);
    g_assert_true ((soup_message_get_flags (msg) & SOUP_MESSAGE_NO_REDIRECT) != 0);
}

/*
 * ai_http_session_new() is guarded: a message built some other way --
 * by a caller, by a test -- is judged by its headers when it is queued.
 */
static void
test_session_new_is_guarded (void)
{
    TServer *origin = tserver_new ();
    TServer *target = tserver_new ();
    g_autoptr(SoupSession) session = ai_http_session_new ();
    g_autoptr(SoupMessage) msg = NULL;

    tserver_set_response (origin, 200, "{}");
    tserver_set_response (target, 200, "{}");
    point_redirect (origin, target, 302);

    msg = soup_message_new ("GET", origin->base_url);
    soup_message_headers_replace (soup_message_get_request_headers (msg),
                                  "x-api-key", "session-test-key");
    g_assert_cmpuint (send_get (session, msg), ==, 302);
    g_assert_true ((soup_message_get_flags (msg) & SOUP_MESSAGE_NO_REDIRECT) != 0);
    g_assert_true (ai_http_redirect_refused (msg, NULL));
    g_assert_cmpuint (tserver_hits (target), ==, 0);

    tserver_free (target);
    tserver_free (origin);
}

/*
 * A provider's requests are declared from what its own add_auth_headers()
 * attaches: credentialed with a key, keyless without one.  A new provider
 * whose header the detector misses would fail here, not in production.
 */
static void
test_client_http_credentials (gconstpointer data)
{
    const RedirectProvider *provider = data;
    g_autoptr(AiConfig) config = ai_config_new ();
    g_autoptr(AiClient) client = NULL;

    ai_config_set_api_key (config, provider->provider, "declared-test-key");
    client = g_object_new (provider->get_type (), "config", config, NULL);

    g_assert_cmpint (ai_client_http_credentials (client), ==,
                     provider->credential != NULL
                     ? AI_HTTP_CARRIES_CREDENTIALS
                     : AI_HTTP_NO_CREDENTIALS);
}

/* The OpenAI-compatible client has its own add_auth_headers(). */
static void
test_compatible_http_credentials (void)
{
    g_autoptr(AiConfig) config = ai_config_new ();
    g_autoptr(AiClient) keyed = NULL;
    g_autoptr(AiClient) keyless = NULL;

    /* "" rather than NULL, so no environment variable stands in. */
    keyed = g_object_new (AI_TYPE_OPENAI_COMPATIBLE_CLIENT, "config", config,
                          "base-url", "http://127.0.0.1:1", "api-key", "compat-key", NULL);
    keyless = g_object_new (AI_TYPE_OPENAI_COMPATIBLE_CLIENT, "config", config,
                            "base-url", "http://127.0.0.1:1", "api-key", "", NULL);
    g_assert_cmpint (ai_client_http_credentials (AI_CLIENT (keyed)), ==,
                     AI_HTTP_CARRIES_CREDENTIALS);
    g_assert_cmpint (ai_client_http_credentials (AI_CLIENT (keyless)), ==,
                     AI_HTTP_NO_CREDENTIALS);
}

/* ------------------------------------------------------------------ */
/* Call sites that had no guard before                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
    gboolean  done;
    GBytes   *bytes;
    GError   *error;
} LoadResult;

static void
image_loaded (GObject *source, GAsyncResult *result, gpointer user_data)
{
    LoadResult *load = user_data;

    load->bytes = ai_generated_image_load_bytes_finish (NULL, result, &load->error);
    load->done = TRUE;
}

/* A generated image's URL is public: its CDN hop is followed. */
static void
test_generated_image_follows (void)
{
    TServer *origin = tserver_new ();
    TServer *target = tserver_new ();
    g_autoptr(AiGeneratedImage) image = NULL;
    LoadResult load = { FALSE, NULL, NULL };

    tserver_set_response (target, 200, "PNGBYTES");
    point_redirect (origin, target, 302);
    image = ai_generated_image_new_from_url (origin->base_url);
    ai_generated_image_load_bytes_async (image, NULL, image_loaded, &load);
    while (!load.done)
        g_main_context_iteration (NULL, TRUE);

    g_assert_no_error (load.error);
    g_assert_nonnull (load.bytes);
    g_assert_cmpuint (g_bytes_get_size (load.bytes), ==, 8);
    g_assert_cmpuint (tserver_hits (target), ==, 1);
    g_bytes_unref (load.bytes);

    tserver_free (target);
    tserver_free (origin);
}

/*
 * DuckDuckGo is keyless but configured: its session used to follow a
 * redirect anywhere; now it stays on the endpoint's origin.
 */
static void
test_duckduckgo_stays_on_origin (void)
{
    TServer *origin = tserver_new ();
    TServer *target = tserver_new ();
    g_autoptr(AiDuckDuckGoSearch) ddg = NULL;
    g_autoptr(GError) error = NULL;
    GList *results;

    tserver_set_response (target, 200, "<html></html>");
    point_redirect (origin, target, 302);
    ddg = g_object_new (AI_TYPE_DUCKDUCKGO_SEARCH, "endpoint", origin->base_url, NULL);
    results = ai_search_provider_search (AI_SEARCH_PROVIDER (ddg), "q", NULL, NULL, &error);
    g_assert_no_error (error);
    g_assert_null (results);
    g_assert_cmpuint (tserver_hits (target), ==, 0);
    g_assert_cmpuint (tserver_hits (origin), >=, 1);

    tserver_free (target);
    tserver_free (origin);
}

int
main (int argc, char **argv)
{
    guint i;

    g_test_init (&argc, &argv, NULL);
    g_test_add_data_func ("/ai-glib/redirect/search/bing", "Ocp-Apim-Subscription-Key", test_search_redirect);
    g_test_add_data_func ("/ai-glib/redirect/search/brave", "X-Subscription-Token", test_search_redirect);
    g_test_add_func ("/ai-glib/redirect/decision", test_decision);
    g_test_add_func ("/ai-glib/redirect/credentials", test_credentials_detected);
    g_test_add_func ("/ai-glib/redirect/status-error", test_status_error_names_refusal);
    g_test_add_func ("/ai-glib/redirect/marked-message", test_marked_message_is_refused);
    g_test_add_data_func ("/ai-glib/redirect/libresearch/302", GUINT_TO_POINTER (302), test_libresearch_async);
    g_test_add_data_func ("/ai-glib/redirect/libresearch/307", GUINT_TO_POINTER (307), test_libresearch_async);
    g_test_add_func ("/ai-glib/redirect/constructor/carries-credentials", test_constructor_carries_credentials);
    g_test_add_func ("/ai-glib/redirect/constructor/no-credentials", test_constructor_no_credentials);
    g_test_add_func ("/ai-glib/redirect/constructor/public-url", test_constructor_public_url);
    g_test_add_func ("/ai-glib/redirect/constructor/bad-url", test_constructor_bad_url);
    g_test_add_func ("/ai-glib/redirect/session-new", test_session_new_is_guarded);
    g_test_add_func ("/ai-glib/redirect/client-credentials/openai-compatible", test_compatible_http_credentials);
    g_test_add_func ("/ai-glib/redirect/site/generated-image", test_generated_image_follows);
    g_test_add_func ("/ai-glib/redirect/site/duckduckgo", test_duckduckgo_stays_on_origin);
    for (i = 0; i < G_N_ELEMENTS (providers); i++)
    {
        g_autofree gchar *a = g_strdup_printf ("/ai-glib/redirect/%s/cross-sync", providers[i].name);
        g_autofree gchar *b = g_strdup_printf ("/ai-glib/redirect/%s/cross-async", providers[i].name);
        g_autofree gchar *c = g_strdup_printf ("/ai-glib/redirect/%s/same-sync", providers[i].name);
        g_autofree gchar *d = g_strdup_printf ("/ai-glib/redirect/%s/same-async", providers[i].name);
        g_autofree gchar *e = g_strdup_printf ("/ai-glib/redirect/%s/same-307", providers[i].name);
        g_autofree gchar *f = g_strdup_printf ("/ai-glib/redirect/%s/same-308", providers[i].name);

        g_test_add_data_func (a, &providers[i], cross_sync);
        g_test_add_data_func (b, &providers[i], cross_async);
        g_test_add_data_func (c, &providers[i], same_sync);
        g_test_add_data_func (d, &providers[i], same_async);
        g_test_add_data_func (e, &providers[i], same_307);
        g_test_add_data_func (f, &providers[i], same_308);
        {
            g_autofree gchar *g = g_strdup_printf ("/ai-glib/redirect/client-credentials/%s", providers[i].name);

            g_test_add_data_func (g, &providers[i], test_client_http_credentials);
        }
    }
    return g_test_run ();
}
