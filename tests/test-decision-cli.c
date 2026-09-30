/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include "test-server.h"

static gchar *ai_binary, *tui_binary;
static const gchar *response_json = "{\"model\":\"laya-test\",\"answers\":{\"answer\":{\"type\":\"noul\",\"noul\":0.93}}}";

static gchar *
run(const gchar *binary, const gchar *const *args, const gchar *input,
	const gchar *url, gint expected_status)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL, *errors = NULL;
	g_autofree gchar *home_dir = g_dir_make_tmp("ai-decision-cli-XXXXXX", &error);
	guint i;
	g_assert_no_error(error);
	g_ptr_array_add(argv, (gpointer)binary);
	for (i = 0; args[i] != NULL; i++) g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_set_cwd(launcher, home_dir);
	g_subprocess_launcher_setenv(launcher, "HOME", home_dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", home_dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_STATE_HOME", home_dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CACHE_HOME", home_dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "LAYA_BASE_URL", url, TRUE);
	g_subprocess_launcher_unsetenv(launcher, "LAYA_API_KEY");
	g_subprocess_launcher_unsetenv(launcher, "AI_PROVIDER");
	g_subprocess_launcher_unsetenv(launcher, "AI_GLIB_DEFAULT_PROVIDER");
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(process, input, NULL, &output, &errors, &error));
	g_assert_no_error(error);
	g_test_message("stderr: %s", errors);
	g_assert_true(g_subprocess_get_if_exited(process));
	g_assert_cmpint(g_subprocess_get_exit_status(process), ==, expected_status);
	/* The fixture owns this directory, including TUI history files. */
	{
		g_autoptr(GSubprocess) cleanup = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error, "rm", "-rf", "--", home_dir, NULL);
		g_assert_no_error(error);
		g_assert_true(g_subprocess_wait_check(cleanup, NULL, &error));
		g_assert_no_error(error);
	}
	return g_steal_pointer(&output);
}

static void
test_cli_stdin(void)
{
	TServer *server = tserver_new();
	const gchar *args[] = { "decide", "--question", "Is this spam?", "--json", NULL };
	g_autofree gchar *output = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	tserver_set_response(server, 200, response_json);
	output = run(ai_binary, args, "A free prize", server->base_url, 0);
	g_assert_true(json_parser_load_from_data(parser, output, -1, NULL));
	g_assert_nonnull(strstr(output, "0.93"));
	g_assert_nonnull(strstr(server->last_body, "A free prize"));
	tserver_free(server);
}
static void
test_cli_request(void)
{
	TServer *server = tserver_new();
	const gchar *args[] = { "decide", "--request", "-", NULL };
	g_autofree gchar *output = NULL;
	tserver_set_response(server, 200, response_json);
	output = run(ai_binary, args, "{\"state\":\"text\",\"questions\":{\"answer\":{\"type\":\"noul\",\"instructions\":\"Spam?\"}}}", server->base_url, 0);
	g_assert_nonnull(strstr(output, "answer: 0.9300"));
	tserver_free(server);
}
static void
test_cli_errors(void)
{
	TServer *server = tserver_new();
	const gchar *bad[] = { "decide", "--provider", "wrong", "--question", "Spam?", "text", NULL };
	const gchar *valid[] = { "decide", "--question", "Spam?", "text", NULL };
	const gchar *help[] = { "decide", "--help", NULL };
	g_autofree gchar *output = run(ai_binary, bad, NULL, server->base_url, 2);
	g_assert_cmpuint(server->hits, ==, 0);
	g_clear_pointer(&output, g_free);
	output = run(ai_binary, help, NULL, server->base_url, 0);
	g_assert_nonnull(strstr(output, "Never downloads weights"));
	g_assert_cmpuint(server->hits, ==, 0);
	g_clear_pointer(&output, g_free);
	tserver_set_response(server, 200, "{}");
	output = run(ai_binary, valid, NULL, server->base_url, 1);
	g_assert_cmpstr(output, ==, "");
	tserver_free(server);
}
static void
test_tui_dump(void)
{
	TServer *server = tserver_new();
	const gchar *args[] = { "-p", "ollama", "--dump", "/decide --question 'Is this spam?' 'A free prize'", NULL };
	g_autofree gchar *output = NULL;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE)) { g_test_skip("TUI not built"); tserver_free(server); return; }
	tserver_set_response(server, 200, response_json);
	output = run(tui_binary, args, NULL, server->base_url, 0);
	g_assert_nonnull(strstr(output, "Decision result:"));
	g_assert_nonnull(strstr(output, "answer: 0.9300"));
	g_assert_cmpuint(server->hits, ==, 1);
	tserver_free(server);
}
int main(int argc, char **argv)
{
	g_autofree gchar *directory = g_path_get_dirname(argv[0]);
	g_autofree gchar *base = g_canonicalize_filename(directory, NULL);
	gint result;
	g_test_init(&argc, &argv, NULL);
	ai_binary = g_build_filename(base, "..", "bin", "ai", NULL);
	tui_binary = g_build_filename(base, "..", "bin", "ai-tui", NULL);
	g_test_add_func("/decision-cli/stdin", test_cli_stdin);
	g_test_add_func("/decision-cli/request", test_cli_request);
	g_test_add_func("/decision-cli/errors", test_cli_errors);
	g_test_add_func("/decision-cli/tui", test_tui_dump);
	result = g_test_run();
	g_free(ai_binary); g_free(tui_binary);
	return result;
}
