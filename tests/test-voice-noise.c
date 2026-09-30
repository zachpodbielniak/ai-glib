/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <math.h>
#include "../src/voice/ai-voice-input-private.h"

/* Runs 3 s of pink noise through the incoming-audio chain and returns the RMS
 * of the last second, after the suppressor has had time to learn the noise. */
static gdouble
tail_rms(const gchar *level)
{
	g_autofree gchar *chain = ai_voice_input_description(level);
	g_autofree gchar *desc = g_strconcat(
		"audiotestsrc wave=pink-noise volume=0.2 num-buffers=300 samplesperbuffer=480 ! "
		"audio/x-raw,format=S16LE,rate=48000,channels=1 ! ",
		chain, NULL);
	g_autoptr(GError) error = NULL;
	GstElement *pipeline = gst_parse_launch(desc, &error);
	GstElement *sink;
	GByteArray *pcm = g_byte_array_new();
	gdouble sum = 0;
	gsize i, n, start;
	g_assert_no_error(error);
	sink = gst_bin_get_by_name(GST_BIN(pipeline), "pcm");
	g_object_set(sink, "drop", FALSE, "max-buffers", 0, NULL);
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	for (;;) {
		GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
		GstMapInfo map;
		if (sample == NULL)
			break;
		gst_buffer_map(gst_sample_get_buffer(sample), &map, GST_MAP_READ);
		g_byte_array_append(pcm, map.data, map.size);
		gst_buffer_unmap(gst_sample_get_buffer(sample), &map);
		gst_sample_unref(sample);
	}
	gst_element_set_state(pipeline, GST_STATE_NULL);
	gst_object_unref(sink);
	gst_object_unref(pipeline);
	/* 16 kHz mono S16LE out: the last second is the last 16000 samples. */
	n = pcm->len / 2;
	g_assert_cmpuint(n, >=, 32000);
	start = n - 16000;
	for (i = start; i < n; i++) {
		gdouble v = ((gint16 *)pcm->data)[i];
		sum += v * v;
	}
	g_byte_array_unref(pcm);
	return sqrt(sum / 16000.0);
}
static void
noise_is_suppressed(void)
{
	gdouble off = tail_rms(NULL), high = tail_rms("high");
	g_test_message("pink noise rms: off=%.1f high=%.1f", off, high);
	g_assert_cmpfloat(off, >, 500);
	/* At least a 4x (12 dB) drop once the suppressor has converged. */
	g_assert_cmpfloat(high, <, off / 4);
}
static void
off_is_unchanged(void)
{
	g_autofree gchar *off = ai_voice_input_description(NULL);
	g_autofree gchar *named = ai_voice_input_description("off");
	g_assert_cmpstr(off, ==, named);
	g_assert_null(strstr(off, "webrtcdsp"));
	g_assert_nonnull(strstr(off, "rate=16000"));
}
static void
levels(void)
{
	static const gchar *const good[] = {"off", "low", "moderate", "high", "very-high", NULL};
	guint i;
	for (i = 0; good[i] != NULL; i++)
		g_assert_true(ai_voice_input_level_valid(good[i]));
	g_assert_true(ai_voice_input_level_valid(NULL));
	g_assert_false(ai_voice_input_level_valid("loud"));
	g_assert_false(ai_voice_input_level_valid(""));
}
static void
transport_property(void)
{
	g_autoptr(AiLivekitTransport) transport = NULL;
	gchar *level = NULL;
	if (!ai_livekit_transport_is_available()) {
		g_test_skip("LiveKit plugin not installed");
		return;
	}
	transport = ai_livekit_transport_new("ws://127.0.0.1:1", "token");
	g_object_get(transport, "noise-suppression", &level, NULL);
	g_assert_null(level);
	g_object_set(transport, "noise-suppression", "moderate", NULL);
	g_object_get(transport, "noise-suppression", &level, NULL);
	g_assert_cmpstr(level, ==, "moderate");
	g_free(level);
}
int
main(int argc, char **argv)
{
	gst_init(&argc, &argv);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/noise/levels", levels);
	g_test_add_func("/voice/noise/off-is-unchanged", off_is_unchanged);
	g_test_add_func("/voice/noise/suppressed", noise_is_suppressed);
	g_test_add_func("/voice/noise/transport-property", transport_property);
	return g_test_run();
}
