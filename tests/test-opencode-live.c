/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Opt-in contract test against the installed OpenCode binary. The model is
 * served on loopback; all OpenCode state lives in a temporary directory. */
#include <glib.h>
#include <glib/gstdio.h>
#include "ai-glib.h"
#include "test-server.h"

/* Remove only the private tree created by this test, without following links. */
static void
remove_tree(const gchar *path)
{
	if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
	{
		g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
		const gchar *name;
		if (dir != NULL)
			while ((name = g_dir_read_name(dir)) != NULL)
			{
				g_autofree gchar *child = g_build_filename(path, name, NULL);
				remove_tree(child);
			}
		g_rmdir(path);
	}
	else
		g_remove(path);
}

/* Keep the callback result until the main context has dispatched completion. */
static void
live_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GAsyncResult **completed = data;
	(void)source;
	*completed = g_object_ref(result);
}

static void
live_contract(gconstpointer mode_data)
{
	const gchar *binary = g_getenv("AI_GLIB_TEST_OPENCODE");
	g_autofree gchar *dir = NULL;
	g_autofree gchar *config = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *request = NULL;
	g_autoptr(AiOpenCodeClient) client = NULL;
	g_autoptr(AiMessage) message = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GAsyncResult) completed = NULL;
	gint mode = GPOINTER_TO_INT(mode_data);
	GList messages = { NULL, NULL, NULL };
	TServer *server;
	const gchar *envs[] = { "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", NULL };
	guint i;

	if (binary == NULL || binary[0] == '\0')
	{
		g_test_skip("Set AI_GLIB_TEST_OPENCODE to an absolute OpenCode binary path");
		return;
	}
	server = tserver_new();
	tserver_set_response_full(server, SOUP_STATUS_OK, "text/event-stream",
		"data: {\"id\":\"test\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"loopback answer\"},\"finish_reason\":null}]}\n\n"
		"data: {\"id\":\"test\",\"object\":\"chat.completion.chunk\",\"created\":1,\"model\":\"fixture\",\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":3,\"total_tokens\":15}}\n\n"
		"data: [DONE]\n\n");
	dir = g_dir_make_tmp("ai-opencode-live-XXXXXX", NULL);
	g_assert_nonnull(dir);
	config = g_strdup_printf(
		"{\"provider\":{\"audit\":{\"npm\":\"@ai-sdk/openai-compatible\",\"name\":\"Audit\","
		"\"options\":{\"baseURL\":\"%s/v1\",\"apiKey\":\"fixture\"},"
		"\"models\":{\"fixture\":{\"name\":\"fixture\",\"limit\":{\"context\":32768,\"output\":1024}}}}},"
		"\"enabled_providers\":[\"audit\"],\"model\":\"audit/fixture\",\"small_model\":\"audit/fixture\","
		"\"permission\":\"deny\",\"share\":\"disabled\",\"autoupdate\":false}", server->base_url);
	client = ai_opencode_client_new();
	g_object_set(client, "executable-path", binary, "working-directory", dir,
		"model", "audit/fixture", "pure", TRUE, "process-timeout-ms", 30000, NULL);
	for (i = 0; envs[i] != NULL; i++)
		ai_cli_client_set_env(AI_CLI_CLIENT(client), envs[i], dir);
	ai_cli_client_set_env(AI_CLI_CLIENT(client), "OPENCODE_CONFIG_CONTENT", config);
	ai_cli_client_set_env(AI_CLI_CLIENT(client), "OPENCODE_DISABLE_MODELS_FETCH", "true");
	ai_cli_client_set_env(AI_CLI_CLIENT(client), "OPENCODE_DISABLE_DEFAULT_PLUGINS", "true");
	message = ai_message_new_user("Respond with the fixture answer.");
	messages.data = message;
	if (mode == 0)
		response = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), &messages, NULL, &error);
	else
	{
		if (mode == 1)
			ai_provider_chat_async(AI_PROVIDER(client), &messages, NULL, 1024, NULL, NULL, live_done, &completed);
		else
			ai_streamable_chat_stream_async(AI_STREAMABLE(client), &messages, NULL, 1024, NULL, NULL, live_done, &completed);
		while (completed == NULL)
			g_main_context_iteration(NULL, TRUE);
		response = mode == 1
			? ai_provider_chat_finish(AI_PROVIDER(client), completed, &error)
			: ai_streamable_chat_stream_finish(AI_STREAMABLE(client), completed, &error);
	}
	if (error != NULL)
		g_test_message("OpenCode: %s", error->message);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	text = ai_response_get_text(response);
	g_assert_cmpstr(text, ==, "loopback answer");
	g_assert_nonnull(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));
	g_assert_cmpint(ai_usage_get_input_tokens(ai_response_get_usage(response)), ==, 12);
	g_assert_cmpint(ai_usage_get_output_tokens(ai_response_get_usage(response)), ==, 3);
	g_mutex_lock(&server->lock);
	request = g_strdup(server->last_body);
	g_mutex_unlock(&server->lock);
	g_assert_nonnull(strstr(request, "Respond with the fixture answer."));
	tserver_free(server);
	remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/ai-glib/opencode-live/sync", GINT_TO_POINTER(0), live_contract);
	g_test_add_data_func("/ai-glib/opencode-live/async", GINT_TO_POINTER(1), live_contract);
	g_test_add_data_func("/ai-glib/opencode-live/stream", GINT_TO_POINTER(2), live_contract);
	return g_test_run();
}
