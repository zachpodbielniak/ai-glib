/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-local-audio-transport.h"
#include "voice/ai-voice-input-private.h"
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <string.h>

/* A voice session on this machine's microphone and speaker, with no call in
 * between: a small device on a desk, or a laptop. There is one participant,
 * the person in the room, and it joins as soon as the devices are open.
 *
 * Playback is paced here rather than handed to the sink in one piece, so a
 * write completes when it has been played (the session treats completion as
 * "the caller heard it") and a flush stops speech within a tick. */

/* How far ahead of the pipeline clock audio is pushed, and how long the sink
 * is assumed to hold it before it is heard. */
#define LEAD (60 * GST_MSECOND)
#define SINK_LATENCY (40 * GST_MSECOND)
#define CHUNK_MS 10

typedef struct {
	GTask *task;
	GBytes *pcm;
	guint rate;
	gsize offset;
	GstClockTime end; /* running time at which its last sample has been heard */
} Output;

struct _AiLocalAudioTransport {
	GObject parent_instance;
	gchar *input, *output, *speaker, *speaker_name, *noise_suppression;
	gboolean echo_cancel;
	GMainContext *context;
	GstElement *pipeline, *appsrc, *appsink;
	GSource *bus_source, *clock;
	GQueue playback;
	/* The speaker path is one unbroken stream: speech or silence, stamped
	 * from a sample count at the current rate, never from the time a buffer
	 * happened to be pushed. */
	guint appsrc_rate;
	GstClockTime origin;
	guint64 samples;
	guint64 generation;
	gboolean leaving;
};
enum {
	PROP_0,
	PROP_INPUT,
	PROP_OUTPUT,
	PROP_SPEAKER,
	PROP_SPEAKER_NAME,
	PROP_ECHO_CANCEL,
	PROP_NOISE_SUPPRESSION
};
static void
transport_iface(AiAudioTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiLocalAudioTransport, ai_local_audio_transport, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_AUDIO_TRANSPORT, transport_iface))

static void
output_free(Output *o)
{
	g_object_unref(o->task);
	g_bytes_unref(o->pcm);
	g_free(o);
}
/* Captured audio is emitted on the transport's own context, never on the
 * streaming thread, and only for the pipeline that captured it. */
typedef struct {
	AiLocalAudioTransport *self;
	guint64 generation;
	GBytes *pcm;
} Captured;
static gboolean
deliver(gpointer data)
{
	Captured *c = data;
	if (c->self->pipeline != NULL && !c->self->leaving &&
		c->generation == c->self->generation)
		g_signal_emit_by_name(c->self, "audio", c->self->speaker, c->pcm);
	return G_SOURCE_REMOVE;
}
static void
captured_free(gpointer data)
{
	Captured *c = data;
	g_object_unref(c->self);
	g_bytes_unref(c->pcm);
	g_free(c);
}
static GstFlowReturn
new_sample(GstAppSink *sink, gpointer data)
{
	AiLocalAudioTransport *self = data;
	GstSample *sample = gst_app_sink_pull_sample(sink);
	GstMapInfo map;
	Captured *c;
	GSource *idle;
	if (sample == NULL)
		return GST_FLOW_EOS;
	c = g_new0(Captured, 1);
	c->self = g_object_ref(self);
	c->generation = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(sink), "voice-generation"));
	gst_buffer_map(gst_sample_get_buffer(sample), &map, GST_MAP_READ);
	c->pcm = g_bytes_new(map.data, map.size);
	gst_buffer_unmap(gst_sample_get_buffer(sample), &map);
	gst_sample_unref(sample);
	idle = g_idle_source_new();
	g_source_set_callback(idle, deliver, c, captured_free);
	g_source_attach(idle, self->context);
	g_source_unref(idle);
	return GST_FLOW_OK;
}
static gchar *
describe(AiLocalAudioTransport *self)
{
	GString *d = g_string_new(NULL);
	gboolean dsp = self->echo_cancel || self->noise_suppression != NULL;
	/* Capture: the session wants 16 kHz mono S16LE. The canceller runs at the
	 * probe's format, 48 kHz mono, since it refuses any mismatch between the two. */
	g_string_append_printf(d, "%s ! queue ! audioconvert ! audioresample ! ", self->input);
	if (dsp)
		g_string_append_printf(d,
							   "audio/x-raw,format=S16LE,rate=48000,channels=1 ! "
							   "webrtcdsp echo-cancel=%s gain-control=false voice-detection=false "
							   "high-pass-filter=true noise-suppression=%s%s%s ! "
							   "audioconvert ! audioresample ! ",
							   self->echo_cancel ? "true" : "false",
							   self->noise_suppression != NULL ? "true" : "false",
							   self->noise_suppression != NULL ? " noise-suppression-level=" : "",
							   self->noise_suppression != NULL ? self->noise_suppression : "");
	g_string_append(d, "audio/x-raw,format=S16LE,rate=16000,channels=1,layout=interleaved ! "
					   "appsink name=voice-in sync=false max-buffers=100 drop=true ");
	/* Playback: the echo probe sees exactly what the speaker plays, which is
	 * what the canceller subtracts from the microphone. */
	/* The queue gives the sink its processing deadline; without one it warns
	 * that the pipeline cannot meet it, and a slow board drops audio. */
	g_string_append(d, "appsrc name=voice-out is-live=true format=time do-timestamp=false ! "
					   "queue ! audioconvert ! audioresample ! ");
	if (self->echo_cancel)
		g_string_append(d, "audio/x-raw,format=S16LE,rate=48000,channels=1 ! "
						   "webrtcechoprobe ! audioconvert ! audioresample ! ");
	g_string_append(d, self->output);
	return g_string_free(d, FALSE);
}
static gboolean
bus_message(GstBus *bus, GstMessage *message, gpointer data)
{
	AiLocalAudioTransport *self = data;
	if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR && !self->leaving) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *debug = NULL;
		gst_message_parse_error(message, &error, &debug);
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Local audio error: element=%s message=%s debug=%s",
			  GST_MESSAGE_SRC(message) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : "unknown",
			  error->message, debug != NULL ? debug : "(none)");
		g_signal_emit_by_name(self, "error", error);
	}
	return G_SOURCE_CONTINUE;
}
static void
set_rate(AiLocalAudioTransport *self, guint rate)
{
	GstCaps *caps;
	if (self->appsrc_rate == rate)
		return;
	self->appsrc_rate = rate;
	caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S16LE", "rate",
							   G_TYPE_INT, (gint)rate, "channels", G_TYPE_INT, 1, "layout",
							   G_TYPE_STRING, "interleaved", NULL);
	gst_app_src_set_caps(GST_APP_SRC(self->appsrc), caps);
	gst_caps_unref(caps);
}
static GstClockTime
stream_time(AiLocalAudioTransport *self)
{
	return self->origin + gst_util_uint64_scale(self->samples, GST_SECOND, self->appsrc_rate);
}
/* Push CHUNK_MS of audio at a time, speech when there is some and silence
 * otherwise, until the stream is LEAD ahead of the clock. A write completes
 * once its last sample should have been heard. */
static void
push_chunk(AiLocalAudioTransport *self, Output *o)
{
	gsize count = 2 * (self->appsrc_rate * CHUNK_MS / 1000);
	GstBuffer *buffer;
	GstClockTime pts = stream_time(self);
	if (o != NULL)
		count = MIN(count, g_bytes_get_size(o->pcm) - o->offset);
	buffer = gst_buffer_new_allocate(NULL, count, NULL);
	if (o != NULL) {
		gst_buffer_fill(buffer, 0, (const guint8 *)g_bytes_get_data(o->pcm, NULL) + o->offset,
						count);
		o->offset += count;
	} else
		gst_buffer_memset(buffer, 0, 0, count);
	self->samples += count / 2;
	GST_BUFFER_PTS(buffer) = pts;
	GST_BUFFER_DTS(buffer) = pts;
	GST_BUFFER_DURATION(buffer) = stream_time(self) - pts;
	if (o != NULL && o->offset == g_bytes_get_size(o->pcm))
		o->end = stream_time(self) + SINK_LATENCY;
	gst_app_src_push_buffer(GST_APP_SRC(self->appsrc), buffer);
}
static gboolean
tick(gpointer data)
{
	AiLocalAudioTransport *self = data;
	GstClock *clock;
	GstClockTime now, base, target;
	Output *o;
	GList *l;
	guint chunks;
	if (self->pipeline == NULL)
		return G_SOURCE_CONTINUE;
	clock = gst_element_get_clock(self->pipeline);
	if (clock == NULL)
		return G_SOURCE_CONTINUE;
	now = gst_clock_get_time(clock);
	gst_object_unref(clock);
	base = gst_element_get_base_time(self->pipeline);
	if (!GST_CLOCK_TIME_IS_VALID(base) || now < base)
		return G_SOURCE_CONTINUE;
	now -= base;
	while ((o = g_queue_peek_head(&self->playback)) != NULL &&
		   o->offset == g_bytes_get_size(o->pcm) && now >= o->end) {
		g_queue_pop_head(&self->playback);
		g_task_return_boolean(o->task, TRUE);
		output_free(o);
	}
	target = now + LEAD;
	/* Bounded catch-up after a stalled main loop; the sink drops what is late. */
	for (chunks = 0; chunks < 40 && stream_time(self) < target; chunks++) {
		o = NULL;
		for (l = self->playback.head; l != NULL; l = l->next)
			if (((Output *)l->data)->offset < g_bytes_get_size(((Output *)l->data)->pcm)) {
				o = l->data;
				break;
			}
		if (o != NULL && o->rate != self->appsrc_rate) {
			/* A new rate starts where the old one ended, not at zero. */
			self->origin = stream_time(self);
			self->samples = 0;
			set_rate(self, o->rate);
		}
		push_chunk(self, o);
	}
	return G_SOURCE_CONTINUE;
}
static void
started(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *join = data;
	AiLocalAudioTransport *self = g_task_get_source_object(join);
	g_autoptr(GError) error = NULL;
	if (!g_task_propagate_boolean(G_TASK(result), &error)) {
		g_task_return_error(join, g_steal_pointer(&error));
	} else if (self->pipeline == NULL || self->leaving) {
		g_task_return_new_error(join, G_IO_ERROR, G_IO_ERROR_CANCELLED,
								"Local audio stopped while opening");
	} else {
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Local audio open: input=%s output=%s echo-cancel=%d",
			  self->input, self->output, self->echo_cancel);
		g_signal_emit_by_name(self, "participant-joined", self->speaker, self->speaker_name);
		g_task_return_boolean(join, TRUE);
	}
	g_object_unref(join);
}
static void
start_pipeline(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	/* Not waiting for preroll: the speaker side has nothing to play until the
	 * first reply, and a device that fails later reports it on the bus. */
	if (gst_element_set_state(data, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
								"Could not open the local audio devices");
	else
		g_task_return_boolean(task, TRUE);
}
static void
stop_pipeline(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	gst_element_set_state(data, GST_STATE_NULL);
	g_task_return_boolean(task, TRUE);
}
static void
flush(AiAudioTransport *transport)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(transport);
	Output *o;
	while ((o = g_queue_pop_head(&self->playback)) != NULL) {
		g_task_return_new_error(o->task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Audio flushed");
		output_free(o);
	}
}
/* Detaches the pipeline from this object; the caller stops it. */
static GstElement *
detach(AiLocalAudioTransport *self)
{
	self->generation++;
	flush(AI_AUDIO_TRANSPORT(self));
	if (self->clock != NULL) {
		g_source_destroy(self->clock);
		g_clear_pointer(&self->clock, g_source_unref);
	}
	if (self->bus_source != NULL) {
		g_source_destroy(self->bus_source);
		g_clear_pointer(&self->bus_source, g_source_unref);
	}
	g_clear_object(&self->appsrc);
	g_clear_object(&self->appsink);
	self->appsrc_rate = 0;
	return g_steal_pointer(&self->pipeline);
}
static void
join_async(AiAudioTransport *transport, const gchar *room, const gchar *token,
		   GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(transport);
	GTask *task = g_task_new(self, cancel, callback, data);
	g_autoptr(GTask) starter = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *description = NULL;
	GstAppSinkCallbacks callbacks;
	GstBus *bus;
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.new_sample = new_sample;
	if (self->pipeline != NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_EXISTS, "Local audio already open");
		g_object_unref(task);
		return;
	}
	/* A program whose only GStreamer user is this transport never called it. */
	if (!gst_init_check(NULL, NULL, &error)) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"GStreamer unavailable: %s", error->message);
		g_object_unref(task);
		return;
	}
	description = describe(self);
	self->pipeline = gst_parse_launch_full(description, NULL, GST_PARSE_FLAG_FATAL_ERRORS, &error);
	if (self->pipeline == NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Local audio pipeline did not parse: %s", error->message);
		g_object_unref(task);
		return;
	}
	self->leaving = FALSE;
	self->generation++;
	self->appsrc = gst_bin_get_by_name(GST_BIN(self->pipeline), "voice-out");
	self->appsink = gst_bin_get_by_name(GST_BIN(self->pipeline), "voice-in");
	g_object_set_data(G_OBJECT(self->appsink), "voice-generation",
					  GUINT_TO_POINTER((guint)self->generation));
	gst_app_sink_set_callbacks(GST_APP_SINK(self->appsink), &callbacks, self, NULL);
	bus = gst_element_get_bus(self->pipeline);
	self->bus_source = gst_bus_create_watch(bus);
	gst_object_unref(bus);
	g_source_set_callback(self->bus_source, G_SOURCE_FUNC(bus_message), self, NULL);
	g_source_attach(self->bus_source, self->context);
	self->clock = g_timeout_source_new(CHUNK_MS / 2);
	g_source_set_callback(self->clock, tick, self, NULL);
	g_source_attach(self->clock, self->context);
	/* A sink waits for its first buffer before the pipeline may play: prime
	 * the stream with silence from running time zero. */
	self->origin = 0;
	self->samples = 0;
	set_rate(self, 16000);
	push_chunk(self, NULL);
	starter = g_task_new(self, cancel, started, task);
	g_task_set_task_data(starter, gst_object_ref(self->pipeline),
						 (GDestroyNotify)gst_object_unref);
	g_task_run_in_thread(starter, start_pipeline);
}
static gboolean
finish(AiAudioTransport *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
	return g_task_propagate_boolean(G_TASK(result), error);
}
static void
leave_async(AiAudioTransport *transport, GCancellable *cancel, GAsyncReadyCallback callback,
			gpointer data)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(transport);
	g_autoptr(GTask) task = g_task_new(self, cancel, callback, data);
	GstElement *pipeline;
	self->leaving = TRUE;
	pipeline = detach(self);
	if (pipeline == NULL) {
		g_task_return_boolean(task, TRUE);
		return;
	}
	g_task_set_task_data(task, pipeline, (GDestroyNotify)gst_object_unref);
	g_task_run_in_thread(task, stop_pipeline);
}
static void
write_pcm_async(AiAudioTransport *transport, GBytes *pcm, guint sample_rate,
				GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(transport);
	GTask *task = g_task_new(self, cancel, callback, data);
	Output *o;
	if (self->pipeline == NULL || self->leaving) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
								"Local audio is not open");
		g_object_unref(task);
		return;
	}
	if (sample_rate == 0 || g_bytes_get_size(pcm) % 2 != 0) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
								"PCM must be 16-bit mono at a known rate");
		g_object_unref(task);
		return;
	}
	o = g_new0(Output, 1);
	o->task = task;
	o->pcm = g_bytes_ref(pcm);
	o->rate = sample_rate;
	g_queue_push_tail(&self->playback, o);
}
static void
write_async(AiAudioTransport *transport, GBytes *pcm, GCancellable *cancel,
			GAsyncReadyCallback callback, gpointer data)
{
	write_pcm_async(transport, pcm, 16000, cancel, callback, data);
}
static void
transport_iface(AiAudioTransportInterface *iface)
{
	iface->join_async = join_async;
	iface->join_finish = finish;
	iface->leave_async = leave_async;
	iface->leave_finish = finish;
	iface->write_async = write_async;
	iface->write_finish = finish;
	iface->write_pcm_async = write_pcm_async;
	iface->write_pcm_finish = finish;
	iface->flush = flush;
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(object);
	if (id == PROP_INPUT)
		g_value_set_string(value, self->input);
	else if (id == PROP_OUTPUT)
		g_value_set_string(value, self->output);
	else if (id == PROP_SPEAKER)
		g_value_set_string(value, self->speaker);
	else if (id == PROP_SPEAKER_NAME)
		g_value_set_string(value, self->speaker_name);
	else if (id == PROP_ECHO_CANCEL)
		g_value_set_boolean(value, self->echo_cancel);
	else if (id == PROP_NOISE_SUPPRESSION)
		g_value_set_string(value, self->noise_suppression);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
replace(gchar **field, const GValue *value, const gchar *fallback)
{
	const gchar *text = g_value_get_string(value);
	g_free(*field);
	*field = g_strdup(text != NULL && *text != '\0' ? text : fallback);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(object);
	if (id == PROP_INPUT)
		replace(&self->input, value, "autoaudiosrc");
	else if (id == PROP_OUTPUT)
		replace(&self->output, value, "autoaudiosink");
	else if (id == PROP_SPEAKER)
		replace(&self->speaker, value, "local");
	else if (id == PROP_SPEAKER_NAME)
		replace(&self->speaker_name, value, "Local");
	else if (id == PROP_ECHO_CANCEL)
		self->echo_cancel = g_value_get_boolean(value);
	else if (id == PROP_NOISE_SUPPRESSION) {
		const gchar *level = g_value_get_string(value);
		if (!ai_voice_input_level_valid(level)) {
			g_message("Ignoring unknown noise-suppression level '%s'", level);
			return;
		}
		g_free(self->noise_suppression);
		self->noise_suppression =
			level != NULL && *level != '\0' && g_strcmp0(level, "off") != 0 ? g_strdup(level)
																			 : NULL;
	} else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
dispose(GObject *object)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(object);
	GstElement *pipeline;
	self->leaving = TRUE;
	pipeline = detach(self);
	if (pipeline != NULL) {
		gst_element_set_state(pipeline, GST_STATE_NULL);
		gst_object_unref(pipeline);
	}
	G_OBJECT_CLASS(ai_local_audio_transport_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiLocalAudioTransport *self = AI_LOCAL_AUDIO_TRANSPORT(object);
	g_free(self->input);
	g_free(self->output);
	g_free(self->speaker);
	g_free(self->speaker_name);
	g_free(self->noise_suppression);
	g_main_context_unref(self->context);
	G_OBJECT_CLASS(ai_local_audio_transport_parent_class)->finalize(object);
}
static void
ai_local_audio_transport_class_init(AiLocalAudioTransportClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->dispose = dispose;
	oc->finalize = finalize;
	/**
	 * AiLocalAudioTransport:input:
	 *
	 * GStreamer description of the microphone, such as "autoaudiosrc" or
	 * "alsasrc device=hw:1". Takes effect at the next join.
	 */
	g_object_class_install_property(
		oc, PROP_INPUT,
		g_param_spec_string("input", "Input", "GStreamer description of the microphone",
							"autoaudiosrc",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiLocalAudioTransport:output:
	 *
	 * GStreamer description of the speaker, such as "autoaudiosink" or
	 * "alsasink device=hw:1". Takes effect at the next join.
	 */
	g_object_class_install_property(
		oc, PROP_OUTPUT,
		g_param_spec_string("output", "Output", "GStreamer description of the speaker",
							"autoaudiosink",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_SPEAKER,
		g_param_spec_string("speaker", "Speaker", "Identity of the person at the microphone",
							"local",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_SPEAKER_NAME,
		g_param_spec_string("speaker-name", "Speaker name",
							"Display name of the person at the microphone", "Local",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiLocalAudioTransport:echo-cancel:
	 *
	 * Subtract what the speaker plays from what the microphone hears. Without
	 * it, a device whose speaker sits next to its microphone hears its own
	 * replies. Needs the webrtcdsp plugin.
	 */
	g_object_class_install_property(
		oc, PROP_ECHO_CANCEL,
		g_param_spec_boolean("echo-cancel", "Echo cancel",
							 "Remove the speaker's own output from the microphone", FALSE,
							 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_NOISE_SUPPRESSION,
		g_param_spec_string("noise-suppression", "Noise suppression",
							"low, moderate, high or very-high; unset or off: none", NULL,
							G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}
static void
ai_local_audio_transport_init(AiLocalAudioTransport *self)
{
	self->context = g_main_context_ref_thread_default();
}
/**
 * ai_local_audio_transport_new:
 *
 * Creates a transport on the default microphone and speaker.
 *
 * Returns: (transfer full): a new #AiLocalAudioTransport
 */
AiLocalAudioTransport *
ai_local_audio_transport_new(void)
{
	return g_object_new(AI_TYPE_LOCAL_AUDIO_TRANSPORT, NULL);
}
