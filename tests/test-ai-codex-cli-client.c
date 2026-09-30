/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>

static void
models_done(GObject *source, GAsyncResult *result, gpointer data)
{
	const gchar *expected[] = {
		"gpt-6-astra", "gpt-6-sol", "gpt-6-luna", "gpt-5.6-sol",
		"gpt-5.6-terra", "gpt-5.6-luna", "gpt-5.5", "gpt-5.4-mini",
		"gpt-5.3-codex-spark"
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
test_lists_sol_and_luna(void)
{
	g_autoptr(GObject) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	g_assert_cmpstr(AI_CODEX_CLI_MODEL_GPT_6_SOL, ==, "gpt-6-sol");
	g_assert_cmpstr(AI_CODEX_CLI_MODEL_GPT_6_LUNA, ==, "gpt-6-luna");
	client = ai_provider_factory_new_from_string("codex-cli", NULL, &error);
	g_assert_no_error(error);
	ai_provider_list_models_async(AI_PROVIDER(client), NULL, models_done, loop);
	g_main_loop_run(loop);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/codex-cli/lists-gpt-6-sol-luna", test_lists_sol_and_luna);
	return g_test_run();
}
