/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
static void
invalid_frame(void)
{
	g_autoptr(AiWebrtcVoiceActivity) vad = ai_webrtc_voice_activity_new();
	g_autoptr(GBytes) bytes = g_bytes_new_static("bad", 3);
	g_autoptr(GError) error = NULL;
	g_assert_cmpint(
		ai_voice_activity_process(AI_VOICE_ACTIVITY(vad), "caller", bytes, &error), ==, -1);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}
static void
independent_silence(void)
{
	g_autoptr(AiWebrtcVoiceActivity) vad = ai_webrtc_voice_activity_new();
	g_autoptr(GBytes) bytes = NULL;
	guint8 silence[320] = {0};
	guint i, window;
	g_object_get(vad, "trailing-silence-ms", &window, NULL);
	g_assert_cmpuint(window, ==, 600);
	bytes = g_bytes_new(silence, sizeof(silence));
	for (i = 0; i < 100; i++) {
		g_assert_cmpint(
			ai_voice_activity_process(AI_VOICE_ACTIVITY(vad), "caller", bytes, NULL), ==, 0);
		g_assert_cmpint(
			ai_voice_activity_process(AI_VOICE_ACTIVITY(vad), "sam", bytes, NULL), ==, 0);
	}
	ai_voice_activity_reset(AI_VOICE_ACTIVITY(vad), "caller");
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/vad/invalid-frame", invalid_frame);
	g_test_add_func("/voice/vad/independent-silence", independent_silence);
	return g_test_run();
}
