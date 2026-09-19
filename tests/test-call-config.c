/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../bin/call/ai-call-config.h"
#include <glib/gstdio.h>
static void
settings(void)
{
	g_autoptr(AiCallConfig) config = ai_call_config_new();
	g_autofree gchar *path = NULL, *text = NULL;
	g_autoptr(GError) error = NULL;
	guint deadline, debounce;
	g_autofree gchar *goodbye = NULL;
	gint fd = g_file_open_tmp("call-config-XXXXXX", &path, NULL);
	g_close(fd, NULL);
	g_object_get(config, "goodbye-message", &goodbye, NULL);
	g_assert_cmpstr(goodbye, ==, "Goodbye.");
	g_assert_true(
		ai_call_config_set_text(config, "goodbye-message", "Until next time.", &error));
	g_clear_pointer(&goodbye, g_free);
	g_object_get(config, "goodbye-message", &goodbye, NULL);
	g_assert_cmpstr(goodbye, ==, "Until next time.");
	g_object_get(config, "barge-in-ms", &debounce, NULL);
	g_assert_cmpuint(debounce, ==, 250);
	g_assert_true(
		ai_call_config_set_text(config, "media-reconnect-attempts", "2", &error));
	g_assert_true(
		ai_call_config_set_text(config, "media-reconnect-delay-ms", "100", &error));
	g_assert_true(ai_call_config_set_text(config, "opus-bitrate", "96000", &error));
	g_assert_false(ai_call_config_set_text(config, "opus-bitrate", "32000", &error));
	g_clear_error(&error);
	g_assert_true(
		g_file_set_contents(path,
							"ai_call:\n  greeting: 'Hello caller'\n  turn-deadline-ms: "
							"1234\n  barge-in-ms: 300\n  jwt-url: https://fixture/jwt\n",
							-1, NULL));
	g_assert_true(ai_call_config_load(config, path, &error));
	g_object_get(config, "greeting", &text, "turn-deadline-ms", &deadline, NULL);
	g_assert_cmpstr(text, ==, "Hello caller");
	g_assert_cmpuint(deadline, ==, 1234);
	g_object_get(config, "barge-in-ms", &debounce, NULL);
	g_assert_cmpuint(debounce, ==, 300);
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
	g_setenv("AI_VOICE_BARGE_IN_MS", "400", TRUE);
	g_assert_true(ai_call_config_apply_environment(config, &error));
	g_unsetenv("AI_VOICE_BARGE_IN_MS");
	g_object_get(config, "barge-in-ms", &debounce, NULL);
	g_assert_cmpuint(debounce, ==, 400);
	g_assert_true(ai_call_config_set_text(config, "barge-in-ms", "200", &error));
	g_object_get(config, "barge-in-ms", &debounce, NULL);
	g_assert_cmpuint(debounce, ==, 200);
	g_assert_false(ai_call_config_set_text(config, "barge-in-ms", "0", &error));
	g_clear_error(&error);
	g_unlink(path);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/voice/call-config/validated-overlays", settings);
	return g_test_run();
}
