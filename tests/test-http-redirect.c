/*
 * test-http-redirect.c - Provider redirects must not disclose credentials or prompts
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ai-glib.h"
#include "convenience/ai-search-http.h"
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
check_redirect (const RedirectProvider *provider, gboolean same_origin, gboolean async)
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

    if (same_origin)
    {
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

static void cross_sync (gconstpointer p) { check_redirect (p, FALSE, FALSE); }
static void cross_async (gconstpointer p) { check_redirect (p, FALSE, TRUE); }
static void same_sync (gconstpointer p) { check_redirect (p, TRUE, FALSE); }
static void same_async (gconstpointer p) { check_redirect (p, TRUE, TRUE); }

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

    /* Relative redirects on the same origin must retain authentication. */
    g_mutex_lock (&origin->lock);
    g_free (origin->redirect_to);
    origin->redirect_to = g_strdup ("/redirect-target");
    g_mutex_unlock (&origin->lock);
    result = ai_search_http_get_json (session, origin->base_url, headers, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (result);
    auth = tserver_dup_header (origin, header);
    g_assert_cmpstr (auth, ==, "search-test-key");
    g_assert_cmpuint (tserver_hits (origin), ==, 3);
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
    for (i = 0; i < G_N_ELEMENTS (providers); i++)
    {
        g_autofree gchar *a = g_strdup_printf ("/ai-glib/redirect/%s/cross-sync", providers[i].name);
        g_autofree gchar *b = g_strdup_printf ("/ai-glib/redirect/%s/cross-async", providers[i].name);
        g_autofree gchar *c = g_strdup_printf ("/ai-glib/redirect/%s/same-sync", providers[i].name);
        g_autofree gchar *d = g_strdup_printf ("/ai-glib/redirect/%s/same-async", providers[i].name);

        g_test_add_data_func (a, &providers[i], cross_sync);
        g_test_add_data_func (b, &providers[i], cross_async);
        g_test_add_data_func (c, &providers[i], same_sync);
        g_test_add_data_func (d, &providers[i], same_async);
    }
    return g_test_run ();
}
