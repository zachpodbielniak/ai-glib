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
/* Samples in the file louder than silence: the speaker path runs continuously,
 * so its size says how long the test ran, not how much was spoken. */
static gsize
loud_samples(const gchar *path)
{
	g_autofree gchar *raw = NULL;
	gsize size = 0, i, n = 0;
	if (!g_file_get_contents(path, &raw, &size, NULL))
		return 0;
	for (i = 0; i + 1 < size; i += 2) {
		gint16 v = (gint16)((guint8)raw[i] | ((guint8)raw[i + 1] << 8));
		if (ABS(v) > 200)
			n++;
	}
	return n;
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
	g_assert_cmpuint(loud_samples(path), >, 14000);
	g_assert_cmpuint(loud_samples(path), <, 18000);
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
	g_assert_cmpuint(loud_samples(path), <, 16000);
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
typedef struct {
	GstClockTime next;
	guint buffers, breaks;
} Continuity;
static gboolean
on_handoff(GSignalInvocationHint *hint, guint n, const GValue *values, gpointer data)
{
	Continuity *c = data;
	GstBuffer *buffer = g_value_get_boxed(&values[1]);
	GstClockTime pts = GST_BUFFER_PTS(buffer), duration = GST_BUFFER_DURATION(buffer);
	if (c->buffers > 0 && GST_CLOCK_TIME_IS_VALID(c->next) &&
		(pts > c->next + GST_MSECOND || pts + GST_MSECOND < c->next))
		c->breaks++;
	c->buffers++;
	c->next = GST_CLOCK_TIME_IS_VALID(duration) ? pts + duration : GST_CLOCK_TIME_NONE;
	return TRUE;
}
/* Speech reaches the speaker as one unbroken stream. Stamped with the time
 * each piece was pushed, scheduling jitter became gaps and overlaps, and every
 * one reset the resampler: on a small board that was choppy, quiet speech. */
static void
test_continuous(void)
{
	g_autoptr(GBytes) pcm = tone(24000, 1000);
	GstElement *probe;
	Continuity c = {GST_CLOCK_TIME_NONE, 0, 0};
	Heard h = {0};
	Write w = {0};
	AiLocalAudioTransport *t;
	guint signal;
	gulong hook;
	if (!available()) {
		g_test_skip("GStreamer test elements unavailable");
		return;
	}
	probe = gst_element_factory_make("fakesink", NULL);
	signal = g_signal_lookup("handoff", G_OBJECT_TYPE(probe));
	gst_object_unref(probe);
	hook = g_signal_add_emission_hook(signal, 0, on_handoff, &c, NULL);
	t = start("audiotestsrc is-live=true wave=silence",
			  "audio/x-raw,format=S16LE,rate=16000,channels=1 ! fakesink "
			  "signal-handoffs=true sync=true",
			  FALSE, &h);
	ai_audio_transport_write_pcm_async(AI_AUDIO_TRANSPORT(t), pcm, 24000, NULL, written, &w);
	wait_for(&w.done, 5000);
	pump(300);
	stop(t);
	g_signal_remove_emission_hook(signal, hook);
	g_assert_no_error(w.error);
	g_assert_cmpuint(c.buffers, >, 20);
	g_assert_cmpuint(c.breaks, ==, 0);
	g_free(h.speaker);
}
static void
leave_when_written(GObject *source, GAsyncResult *result, gpointer data)
{
	Write *w = data;
	ai_audio_transport_write_pcm_finish(AI_AUDIO_TRANSPORT(source), result, &w->error);
	/* What ai-call does when its goodbye finishes playing. */
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(source), NULL, NULL, NULL);
	w->done = TRUE;
}
/* The last write's completion is where a program hangs up. The pipeline is
 * gone when the callback returns, and the playback tick that completed the
 * write must not go on pushing into it. */
static void
test_leave_from_completion(void)
{
	g_autoptr(GBytes) pcm = tone(16000, 200);
	Heard h = {0};
	Write w = {0};
	AiLocalAudioTransport *t;
	if (!available()) {
		g_test_skip("GStreamer test elements unavailable");
		return;
	}
	t = start("audiotestsrc is-live=true wave=silence", "fakesink sync=true", FALSE, &h);
	ai_audio_transport_write_pcm_async(AI_AUDIO_TRANSPORT(t), pcm, 16000, NULL,
									   leave_when_written, &w);
	wait_for(&w.done, 5000);
	g_assert_true(w.done);
	g_assert_no_error(w.error);
	pump(200);
	g_object_unref(t);
	g_free(h.speaker);
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
static void
count_joined(AiAudioTransport *t, const gchar *speaker, const gchar *name, gpointer data)
{
	(*(guint *)data)++;
}
/* Hanging up while the devices are still opening: the stop can run before the
 * start, which must then not leave a pipeline playing into a freed transport. */
static void
test_leave_while_opening(void)
{
	guint i;
	if (!available()) {
		g_test_skip("GStreamer test elements unavailable");
		return;
	}
	for (i = 0; i < 50; i++) {
		AiLocalAudioTransport *t = g_object_new(
			AI_TYPE_LOCAL_AUDIO_TRANSPORT, "input", "audiotestsrc is-live=true", "output",
			"fakesink sync=true", NULL);
		Heard h = {0};
		gboolean done = FALSE;
		ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(t), NULL, NULL, NULL, joined, &h);
		ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(t), NULL, left, &done);
		g_object_unref(t);
		wait_for(&h.done, 5000);
		wait_for(&done, 5000);
		g_assert_true(h.done && done);
		if (h.error != NULL)
			g_assert_error(h.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		g_clear_error(&h.error);
	}
	pump(100);
}
/* Opened, closed and opened again before the first open finished: one join
 * each, and the first is not reported as the second's success. */
static void
test_rejoin_while_opening(void)
{
	AiLocalAudioTransport *t;
	Heard a = {0}, b = {0};
	gboolean done = FALSE;
	guint joins = 0;
	if (!available()) {
		g_test_skip("GStreamer test elements unavailable");
		return;
	}
	t = g_object_new(AI_TYPE_LOCAL_AUDIO_TRANSPORT, "input", "audiotestsrc is-live=true",
					 "output", "fakesink sync=true", NULL);
	g_signal_connect(t, "participant-joined", G_CALLBACK(count_joined), &joins);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(t), NULL, NULL, NULL, joined, &a);
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(t), NULL, left, &done);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(t), NULL, NULL, NULL, joined, &b);
	wait_for(&a.done, 5000);
	wait_for(&b.done, 5000);
	wait_for(&done, 5000);
	g_assert_error(a.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_no_error(b.error);
	g_assert_cmpuint(joins, ==, 1);
	g_clear_error(&a.error);
	stop(t);
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
	g_test_add_func("/voice/local/continuous", test_continuous);
	g_test_add_func("/voice/local/leave-from-completion", test_leave_from_completion);
	g_test_add_func("/voice/local/bad-device", test_bad_device);
	g_test_add_func("/voice/local/leave-while-opening", test_leave_while_opening);
	g_test_add_func("/voice/local/rejoin-while-opening", test_rejoin_while_opening);
	return g_test_run();
}
