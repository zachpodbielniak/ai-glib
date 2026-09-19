/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../bin/call/ai-call-config.h"
#include <glib/gstdio.h>
static void
settings(void)
{
	g_autoptr(AiCallConfig) config = ai_call_config_new();
	g_autofree gchar *path = NULL, *text = NULL;
	g_autoptr(GError) error = NULL;
	guint deadline;
	gint fd = g_file_open_tmp("call-config-XXXXXX", &path, NULL);
	g_close(fd, NULL);
	g_assert_true(
		g_file_set_contents(path,
							"ai_call:\n  greeting: 'Hello caller'\n  turn-deadline-ms: "
							"1234\n  jwt-url: https://fixture/jwt\n",
							-1, NULL));
	g_assert_true(ai_call_config_load(config, path, &error));
	g_object_get(config, "greeting", &text, "turn-deadline-ms", &deadline, NULL);
	g_assert_cmpstr(text, ==, "Hello caller");
	g_assert_cmpuint(deadline, ==, 1234);
	g_assert_true(g_file_set_contents(
		path, "ai_call:\n  greeting: changed\n  turn-deadline-ms: -1\n", -1, NULL));
	g_assert_false(ai_call_config_load(config, path, &error));
	g_clear_error(&error);
	g_clear_pointer(&text, g_free);
	g_object_get(config, "greeting", &text, NULL);
	g_assert_cmpstr(text, ==, "Hello caller");
	g_assert_false(ai_call_config_set_text(config, "unknown", "x", &error));
	g_clear_error(&error);
	g_setenv("AI_CALL_GREETING", "Environment caller", TRUE);
	g_assert_true(ai_call_config_apply_environment(config, &error));
	g_unsetenv("AI_CALL_GREETING");
	g_assert_true(ai_call_config_set_text(config, "greeting", "CLI caller", &error));
	g_clear_pointer(&text, g_free);
	g_object_get(config, "greeting", &text, NULL);
	g_assert_cmpstr(text, ==, "CLI caller");
	g_unlink(path);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/voice/call-config/validated-overlays", settings);
	return g_test_run();
}
