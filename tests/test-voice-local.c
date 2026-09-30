/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <gst/gst.h>
#include <glib/gstdio.h>
#include <math.h>

/* The local transport runs a voice session on this machine's microphone and
 * speaker: a small device next to someone, rather than a call. These tests
 * use test sources and a file sink, so they need no sound card. */

typedef struct {
	gsize bytes;
	gboolean loud;
	gchar *speaker;
	GError *error;
	gboolean done;
} Heard;

static gboolean
available(void)
{
	gst_init(NULL, NULL);
	return gst_element_factory_find("audiotestsrc") != NULL &&
		   gst_element_factory_find("filesink") != NULL;
}
static void
wait_for(gboolean *flag, guint ms)
{
	gint64 limit = g_get_monotonic_time() + (gint64)ms * 1000;
	while (!*flag && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
}
static void
pump(guint ms)
{
	gboolean never = FALSE;
	wait_for(&never, ms);
}
static void
joined(GObject *source, GAsyncResult *result, gpointer data)
{
	Heard *h = data;
	ai_audio_transport_join_finish(AI_AUDIO_TRANSPORT(source), result, &h->error);
	h->done = TRUE;
}
static void
left(GObject *source, GAsyncResult *result, gpointer data)
{
	ai_audio_transport_leave_finish(AI_AUDIO_TRANSPORT(source), result, NULL);
	*(gboolean *)data = TRUE;
}
static void
on_joined(AiAudioTransport *t, const gchar *speaker, const gchar *name, gpointer data)
{
	Heard *h = data;
	g_free(h->speaker);
	h->speaker = g_strdup(speaker);
}
static void
on_audio(AiAudioTransport *t, const gchar *speaker, GBytes *pcm, gpointer data)
{
	Heard *h = data;
	gsize size, i;
	const gint16 *s = g_bytes_get_data(pcm, &size);
	g_assert_cmpstr(speaker, ==, "local");
	g_assert_cmpuint(size % 2, ==, 0);
	h->bytes += size;
	for (i = 0; i < size / 2; i++)
		if (ABS(s[i]) > 1000)
			h->loud = TRUE;
}
static AiLocalAudioTransport *
start(const gchar *input, const gchar *output, gboolean echo_cancel, Heard *h)
{
	AiLocalAudioTransport *t = g_object_new(AI_TYPE_LOCAL_AUDIO_TRANSPORT, "input", input,
											"output", output, "echo-cancel", echo_cancel,
											NULL);
	g_signal_connect(t, "participant-joined", G_CALLBACK(on_joined), h);
	g_signal_connect(t, "audio", G_CALLBACK(on_audio), h);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(t), NULL, NULL, NULL, joined, h);
	wait_for(&h->done, 5000);
	g_assert_true(h->done);
	g_assert_no_error(h->error);
	return t;
}
static void
stop(AiLocalAudioTransport *t)
{
	gboolean done = FALSE;
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(t), NULL, left, &done);
	wait_for(&done, 5000);
	g_assert_true(done);
	g_object_unref(t);
}
/* The microphone reaches the session as 16 kHz mono from one local speaker. */
static void
test_capture(gconstpointer data)
{
	gboolean echo_cancel = GPOINTER_TO_INT(data);
	Heard h = {0};
	AiLocalAudioTransport *t;
	if (!available() || (echo_cancel && gst_element_factory_find("webrtcdsp") == NULL)) {
		g_test_skip("GStreamer test elements unavailable");
		return;
	}
	t = start("audiotestsrc is-live=true wave=sine freq=440 volume=0.5", "fakesink",
			  echo_cancel, &h);
	g_assert_cmpstr(h.speaker, ==, "local");
	pump(600);
	/* Roughly 0.6 s at 32000 bytes per second; generous either way. */
	g_assert_cmpuint(h.bytes, >, 8000);
	g_assert_cmpuint(h.bytes, <, 40000);
	g_assert_true(h.loud);
	stop(t);
	g_free(h.speaker);
}
typedef struct {
	gboolean done;
	GError *error;
	gint64 at;
} Write;
static void
written(GObject *source, GAsyncResult *result, gpointer data)
{
	Write *w = data;
	ai_audio_transport_write_pcm_finish(AI_AUDIO_TRANSPORT(source), result, &w->error);
	w->at = g_get_monotonic_time();
	w->done = TRUE;
}
static GBytes *
tone(guint rate, guint ms)
{
	gsize n = (gsize)rate * ms / 1000, i;
	gint16 *s = g_new(gint16, n);
	for (i = 0; i < n; i++)
		s[i] = (gint16)(8000 * sin(2 * G_PI * 440 * i / rate));
	return g_bytes_new_take(s, n * 2);
}
static gsize
file_size(const gchar *path)
{
	GStatBuf st;
	return g_stat(path, &st) == 0 ? (gsize)st.st_size : 0;
}
/* Speech plays at the speed it was spoken, and a write completes when it has
 * been played, not when it was queued: the session relies on that for what
 * the caller heard. */
static void
test_playback(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("voice-local-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "out.raw", NULL);
	g_autofree gchar *output = g_strdup_printf(
		"audio/x-raw,format=S16LE,rate=16000,channels=1 ! filesink location=%s", path);
	g_autoptr(GBytes) pcm = tone(24000, 1000);
	Heard h = {0};
	Write w = {0};
	AiLocalAudioTransport *t;
	gint64 begin;
	if (!available()) {
		g_test_skip("GStreamer test elements unavailable");
		return;
	}
	t = start("audiotestsrc is-live=true wave=silence", output, FALSE, &h);
	begin = g_get_monotonic_time();
	ai_audio_transport_write_pcm_async(AI_AUDIO_TRANSPORT(t), pcm, 24000, NULL, written, &w);
	wait_for(&w.done, 5000);
	g_assert_true(w.done);
	g_assert_no_error(w.error);
	g_assert_cmpint(w.at - begin, >=, 800000);
	stop(t);
	/* One second at 16 kHz, resampled from 24 kHz, give or take a buffer. */
	g_assert_cmpuint(file_size(path), >, 28000);
	g_assert_cmpuint(file_size(path), <, 40000);
	g_remove(path);
	g_rmdir(dir);
	g_free(h.speaker);
}
/* A flush is how an interruption stops speech: the write is cancelled at
 * once, and the rest of it is never played. */
static void
test_flush(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("voice-local-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "out.raw", NULL);
	g_autofree gchar *output = g_strdup_printf(
		"audio/x-raw,format=S16LE,rate=16000,channels=1 ! filesink location=%s", path);
	g_autoptr(GBytes) pcm = tone(16000, 3000);
	Heard h = {0};
	Write w = {0};
	AiLocalAudioTransport *t;
	gint64 flushed;
	if (!available()) {
		g_test_skip("GStreamer test elements unavailable");
		return;
	}
	t = start("audiotestsrc is-live=true wave=silence", output, FALSE, &h);
	ai_audio_transport_write_pcm_async(AI_AUDIO_TRANSPORT(t), pcm, 16000, NULL, written, &w);
	pump(300);
	g_assert_false(w.done);
	flushed = g_get_monotonic_time();
	ai_audio_transport_flush(AI_AUDIO_TRANSPORT(t));
	wait_for(&w.done, 1000);
	g_assert_true(w.done);
	g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_cmpint(w.at - flushed, <, 200000);
	pump(300);
	stop(t);
	/* About 0.3 s of the 3 s reached the speaker. */
	g_assert_cmpuint(file_size(path), <, 32000);
	g_clear_error(&w.error);
	g_remove(path);
	g_rmdir(dir);
	g_free(h.speaker);
}
/* A program with no other GStreamer user must not have to initialise it:
 * ai-call --local crashed parsing the first caps for want of gst_init(). */
static void
test_initialises_gstreamer(void)
{
	Heard h = {0};
	AiLocalAudioTransport *t;
	g_assert_false(gst_is_initialized());
	t = g_object_new(AI_TYPE_LOCAL_AUDIO_TRANSPORT, "input",
					 "audiotestsrc is-live=true wave=silence", "output", "fakesink", NULL);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(t), NULL, NULL, NULL, joined, &h);
	wait_for(&h.done, 5000);
	g_assert_true(h.done);
	if (h.error != NULL && g_error_matches(h.error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
		g_test_skip("GStreamer test elements unavailable");
		g_clear_error(&h.error);
	} else {
		g_assert_no_error(h.error);
		stop(t);
		t = NULL;
	}
	g_clear_object(&t);
}
/* A description that does not parse is a join error, not a crash. */
static void
test_bad_device(void)
{
	gst_init(NULL, NULL);
	AiLocalAudioTransport *t = g_object_new(AI_TYPE_LOCAL_AUDIO_TRANSPORT, "input",
											"no-such-element-here", NULL);
	Heard h = {0};
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(t), NULL, NULL, NULL, joined, &h);
	wait_for(&h.done, 5000);
	g_assert_true(h.done);
	g_assert_nonnull(h.error);
	g_clear_error(&h.error);
	g_object_unref(t);
}
int
main(int argc, char **argv)
{
	/* No gst_init() here: the first test proves the transport does it. */
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/local/initialises-gstreamer", test_initialises_gstreamer);
	g_test_add_data_func("/voice/local/capture", GINT_TO_POINTER(FALSE), test_capture);
	g_test_add_data_func("/voice/local/capture-echo-cancel", GINT_TO_POINTER(TRUE),
						 test_capture);
	g_test_add_func("/voice/local/playback-paced", test_playback);
	g_test_add_func("/voice/local/flush", test_flush);
	g_test_add_func("/voice/local/bad-device", test_bad_device);
	return g_test_run();
}
