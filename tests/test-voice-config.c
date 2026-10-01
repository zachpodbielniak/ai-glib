/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>
static void
voice_config(void)
{
	g_autoptr(AiConfig) config = ai_config_new();
	g_autofree gchar *dir = g_dir_make_tmp("ai-voice-config-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "config.yaml", NULL);
	g_autofree gchar *stt = NULL, *model = NULL;
	g_autoptr(GError) error = NULL;
	AiProviderType provider;
	g_assert_true(g_file_set_contents(
		path,
		"apps:\n  ai-call:\n    default_provider: ollama\n    default_model: "
		"example-model\nvoice:\n  stt_url: ws://fixture/stt/stream\n  identity_file: "
		"/fixture/identity\n",
		-1, NULL));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_no_error(error);
	g_object_get(config, "voice-stt-url", &stt, NULL);
	g_assert_cmpstr(stt, ==, "ws://fixture/stt/stream");
	g_assert_true(ai_provider_factory_resolve_defaults(config, "ai-call", "default",
													   NULL, &provider, &model, &error));
	g_assert_cmpint(provider, ==, AI_PROVIDER_OLLAMA);
	g_assert_cmpstr(model, ==, "example-model");
	g_setenv("AI_VOICE_STT_URL", "ws://environment/stt/stream", TRUE);
	g_clear_pointer(&stt, g_free);
	g_object_get(config, "voice-stt-url", &stt, NULL);
	g_assert_cmpstr(stt, ==, "ws://environment/stt/stream");
	g_object_set(config, "voice-stt-url", "ws://explicit/stt/stream", NULL);
	g_clear_pointer(&stt, g_free);
	g_object_get(config, "voice-stt-url", &stt, NULL);
	g_assert_cmpstr(stt, ==, "ws://explicit/stt/stream");
	g_unsetenv("AI_VOICE_STT_URL");
	g_assert_true(g_file_set_contents(path, "voice:\n  stt_url: [invalid]\n", -1, NULL));
	g_assert_false(ai_config_load_from_file(config, path, &error));
	g_assert_nonnull(error);
	g_clear_pointer(&stt, g_free);
	g_object_get(config, "voice-stt-url", &stt, NULL);
	g_assert_cmpstr(stt, ==, "ws://explicit/stt/stream");
	g_unlink(path);
	g_rmdir(dir);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
	g_test_add_func("/voice/config/scoped-defaults", voice_config);
	return g_test_run();
}
