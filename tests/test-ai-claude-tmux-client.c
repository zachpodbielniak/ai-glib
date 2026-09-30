/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>

static void
models_done(GObject *source, GAsyncResult *result, gpointer data)
{
	const gchar *expected[] = {
		"fable", "opus", "sonnet", "haiku",
		"claude-fable-5", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5"
	};
	g_autoptr(GError) error = NULL;
	GList *models = ai_provider_list_models_finish(AI_PROVIDER(source), result, &error);
	GList *item;
	guint i = 0;

	g_assert_no_error(error);
	g_assert_cmpuint(g_list_length(models), ==, G_N_ELEMENTS(expected));
	for (item = models; item != NULL; item = item->next)
		g_assert_cmpstr(item->data, ==, expected[i++]);
	g_list_free_full(models, g_free);
	g_main_loop_quit(data);
}

static void
test_lists_opus_5_5(void)
{
	g_autoptr(AiClaudeTmuxClient) client = ai_claude_tmux_client_new();
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	g_assert_cmpstr(AI_CLAUDE_TMUX_MODEL_OPUS_5_5, ==, "claude-opus-5-5");
	ai_provider_list_models_async(AI_PROVIDER(client), NULL, models_done, loop);
	g_main_loop_run(loop);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/claude-tmux/lists-opus-5-5", test_lists_opus_5_5);
	return g_test_run();
}
