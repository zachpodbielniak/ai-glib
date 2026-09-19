/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-livekit-transport.h"
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <json-glib/json-glib.h>
#include "core/ai-json-util.h"

typedef struct {
	GTask *task;
	GBytes *pcm;
	gsize offset;
} Output;
typedef struct {
	GWeakRef owner;
	gchar *speaker, *name;
} Track;
typedef struct {
	AiLivekitTransport *self;
	gchar *speaker, *name;
	GBytes *pcm;
	guint kind;
} Delivery;
struct _AiLivekitTransport {
	GObject parent_instance;
	gchar *url, *receive_token;
	GMainContext *context;
	GstElement *pipeline, *input, *output, *appsrc;
	GSource *bus_source, *clock;
	GQueue playback;
	GTask *joining;
	gint64 join_deadline;
	gsize queued_bytes;
	gboolean leaving;
};
static void
transport_iface(AiAudioTransportInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiLivekitTransport, ai_livekit_transport, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_AUDIO_TRANSPORT, transport_iface))
static void
clear_source(GSource **source)
{
	if (*source != NULL) {
		g_source_destroy(*source);
		g_clear_pointer(source, g_source_unref);
	}
}
static void
output_free(Output *o)
{
	g_object_unref(o->task);
	g_bytes_unref(o->pcm);
	g_free(o);
}
static void
flush(AiAudioTransport *transport)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(transport);
	Output *o;
	while ((o = g_queue_pop_head(&self->playback)) != NULL) {
		g_task_return_new_error(o->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
								"Audio flushed");
		output_free(o);
	}
	self->queued_bytes = 0;
}
static gboolean
deliver(gpointer data)
{
	Delivery *d = data;
	if (d->self->pipeline == NULL || d->self->leaving)
		return G_SOURCE_REMOVE;
	if (d->kind == 0)
		g_signal_emit_by_name(d->self, "participant-joined", d->speaker, d->name);
	else if (d->kind == 1)
		g_signal_emit_by_name(d->self, "audio", d->speaker, d->pcm);
	else
		g_signal_emit_by_name(d->self, "participant-left", d->speaker);
	return G_SOURCE_REMOVE;
}
static void
delivery_free(gpointer data)
{
	Delivery *d = data;
	g_object_unref(d->self);
	g_free(d->speaker);
	g_free(d->name);
	g_clear_pointer(&d->pcm, g_bytes_unref);
	g_free(d);
}
static void
post(Track *track, guint kind, GBytes *pcm)
{
	AiLivekitTransport *self = g_weak_ref_get(&track->owner);
	Delivery *d;
	GSource *source;
	if (self == NULL)
		return;
	d = g_new0(Delivery, 1);
	d->self = self;
	d->speaker = g_strdup(track->speaker);
	d->name = g_strdup(track->name);
	d->kind = kind;
	d->pcm = pcm != NULL ? g_bytes_ref(pcm) : NULL;
	source = g_idle_source_new();
	g_source_set_callback(source, deliver, d, delivery_free);
	g_source_attach(source, self->context);
	g_source_unref(source);
}
static void
track_free(gpointer data)
{
	Track *t = data;
	g_weak_ref_clear(&t->owner);
	g_free(t->speaker);
	g_free(t->name);
	g_free(t);
}
static GstFlowReturn
sample(GstAppSink *sink, gpointer data)
{
	Track *t = data;
	GstSample *sample = gst_app_sink_pull_sample(sink);
	GstBuffer *buffer;
	GstMapInfo map;
	if (sample == NULL)
		return GST_FLOW_EOS;
	buffer = gst_sample_get_buffer(sample);
	if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
		g_autoptr(GBytes) bytes = g_bytes_new(map.data, map.size);
		post(t, 1, bytes);
		gst_buffer_unmap(buffer, &map);
	}
	gst_sample_unref(sample);
	return GST_FLOW_OK;
}
static void
pad_added(GstElement *input, GstPad *pad, gpointer data)
{
	AiLivekitTransport *self = data;
	GstStructure *info = NULL;
	GstElement *bin, *sink;
	GstObject *pipeline;
	GstPad *target;
	Track *track;
	g_autoptr(GError) error = NULL;
	if (!g_str_has_prefix(GST_PAD_NAME(pad), "audio_"))
		return;
	g_object_get(pad, "participant-info", &info, NULL);
	if (info == NULL)
		return;
	track = g_new0(Track, 1);
	g_weak_ref_init(&track->owner, self);
	track->speaker = g_strdup(gst_structure_get_string(info, "sid"));
	track->name = g_strdup(gst_structure_get_string(info, "name"));
	if (track->name == NULL || *track->name == '\0') {
		g_free(track->name);
		track->name = g_strdup(gst_structure_get_string(info, "identity"));
	}
	gst_structure_free(info);
	if (track->speaker == NULL) {
		track_free(track);
		return;
	}
	bin = gst_parse_bin_from_description(
		"queue max-size-time=200000000 ! audioconvert ! audioresample ! "
		"audio/x-raw,format=S16LE,rate=16000,channels=1,layout=interleaved ! appsink "
		"name=pcm emit-signals=true sync=false max-buffers=20 drop=true",
		TRUE, &error);
	if (bin == NULL) {
		track_free(track);
		return;
	}
	sink = gst_bin_get_by_name(GST_BIN(bin), "pcm");
	g_object_set_data_full(G_OBJECT(sink), "voice-track", track, track_free);
	g_object_set_data_full(G_OBJECT(pad), "voice-speaker", g_strdup(track->speaker),
						   g_free);
	g_signal_connect(sink, "new-sample", G_CALLBACK(sample), track);
	pipeline = gst_object_get_parent(GST_OBJECT(input));
	if (pipeline == NULL) {
		gst_object_unref(sink);
		gst_object_unref(bin);
		return;
	}
	gst_bin_add(GST_BIN(pipeline), bin);
	gst_object_unref(pipeline);
	target = gst_element_get_static_pad(bin, "sink");
	if (gst_pad_link(pad, target) == GST_PAD_LINK_OK) {
		post(track, 0, NULL);
		gst_element_sync_state_with_parent(bin);
	}
	gst_object_unref(target);
	gst_object_unref(sink);
}
static void
pad_removed(GstElement *input, GstPad *pad, gpointer data)
{
	const gchar *speaker = g_object_get_data(G_OBJECT(pad), "voice-speaker");
	Track track = {0};
	if (speaker == NULL)
		return;
	g_weak_ref_init(&track.owner, data);
	track.speaker = (gchar *)speaker;
	post(&track, 2, NULL);
	g_weak_ref_clear(&track.owner);
}
static gboolean
bus_message(GstBus *bus, GstMessage *message, gpointer data)
{
	AiLivekitTransport *self = data;
	if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
		g_autoptr(GError) error = NULL;
		g_autofree gchar *debug = NULL;
		gst_message_parse_error(message, &error, &debug);
		if (self->joining != NULL) {
			g_task_return_error(self->joining, g_error_copy(error));
			g_clear_object(&self->joining);
		}
		g_signal_emit_by_name(self, "error", error);
	}
	return G_SOURCE_CONTINUE;
}
static gboolean
tick(gpointer data)
{
	AiLivekitTransport *self = data;
	Output *o;
	if (self->joining != NULL) {
		GObject *rx = NULL, *tx = NULL;
		gint rx_state = 0, tx_state = 0;
		g_object_get(self->input, "signaller", &rx, NULL);
		g_object_get(self->output, "signaller", &tx, NULL);
		g_object_get(rx, "connection-state", &rx_state, NULL);
		g_object_get(tx, "connection-state", &tx_state, NULL);
		g_object_unref(rx);
		g_object_unref(tx);
		if (g_task_return_error_if_cancelled(self->joining))
			g_clear_object(&self->joining);
		else if (rx_state != 0 && tx_state >= 3) {
			g_task_return_boolean(self->joining, TRUE);
			g_clear_object(&self->joining);
		} else if (g_get_monotonic_time() >= self->join_deadline) {
			g_task_return_new_error(self->joining, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
									"LiveKit join timed out");
			g_clear_object(&self->joining);
		}
	}
	/* Retire completed writes without inserting a silent tick between frames. */
	while ((o = g_queue_peek_head(&self->playback)) != NULL) {
		gsize size = g_bytes_get_size(o->pcm);
		if (!g_cancellable_is_cancelled(g_task_get_cancellable(o->task)) &&
			o->offset < size)
			break;
		g_queue_pop_head(&self->playback);
		self->queued_bytes -= size;
		g_task_return_boolean(o->task, TRUE);
		output_free(o);
	}
	if (o != NULL) {
		gsize size;
		const guint8 *raw = g_bytes_get_data(o->pcm, &size);
		gsize count = MIN((gsize)320, size - o->offset);
		GstBuffer *buffer = gst_buffer_new_allocate(NULL, count, NULL);
		GstFlowReturn flow;
		gst_buffer_fill(buffer, 0, raw + o->offset, count);
		GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(count / 2, GST_SECOND, 16000);
		flow = gst_app_src_push_buffer(GST_APP_SRC(self->appsrc), buffer);
		o->offset += count;
		if (flow != GST_FLOW_OK) {
			g_queue_pop_head(&self->playback);
			self->queued_bytes -= size;
			g_task_return_new_error(o->task, G_IO_ERROR, G_IO_ERROR_FAILED,
									"LiveKit PCM push failed");
			output_free(o);
		}
	} else if (self->appsrc != NULL) {
		GstBuffer *silence = gst_buffer_new_allocate(NULL, 320, NULL);
		gst_buffer_memset(silence, 0, 0, 320);
		GST_BUFFER_DURATION(silence) = 10 * GST_MSECOND;
		gst_app_src_push_buffer(GST_APP_SRC(self->appsrc), silence);
	}
	return G_SOURCE_CONTINUE;
}
static void
start_pipeline(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	GstElement *pipeline = data;
	if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
								"Cannot start LiveKit pipeline");
	else
		g_task_return_boolean(task, TRUE);
}
static void
started(GObject *source, GAsyncResult *result, gpointer data)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(source);
	g_autoptr(GError) error = NULL;
	if (!g_task_propagate_boolean(G_TASK(result), &error) && self->joining != NULL) {
		g_task_return_error(self->joining, g_steal_pointer(&error));
		g_clear_object(&self->joining);
	}
}
/* JWT payload is used only to exclude our own publisher, never to authenticate. */
static gchar *
token_identity(const gchar *token)
{
	g_auto(GStrv) parts = g_strsplit(token, ".", 3);
	g_autofree guchar *decoded = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	gsize size;
	gchar *p;
	if (g_strv_length(parts) != 3)
		return NULL;
	for (p = parts[1]; *p; p++) {
		if (*p == '-')
			*p = '+';
		else if (*p == '_')
			*p = '/';
	}
	decoded = g_base64_decode(parts[1], &size);
	if (!json_parser_load_from_data(parser, (gchar *)decoded, size, NULL))
		return NULL;
	return g_strdup(ai_json_get_string(ai_json_root_object(parser), "sub", NULL));
}
/* LiveKit supplies ICE traversal. UPnP probes on every local interface add
 * latency and fail on addressless VPN interfaces; do not request router maps. */
static void
configure_ice(GstBin *bin, GstBin *subbin, GstElement *element, gpointer data)
{
	GObject *ice = NULL, *agent = NULL;
	if (g_strcmp0(G_OBJECT_TYPE_NAME(element), "GstWebRTCBin") != 0)
		return;
	g_object_get(element, "ice-agent", &ice, NULL);
	if (ice != NULL &&
		g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent") != NULL)
		g_object_get(ice, "agent", &agent, NULL);
	if (agent != NULL &&
		g_object_class_find_property(G_OBJECT_GET_CLASS(agent), "upnp") != NULL)
		g_object_set(agent, "upnp", FALSE, NULL);
	g_clear_object(&agent);
	g_clear_object(&ice);
}
static void
join_async(AiAudioTransport *transport, const gchar *room, const gchar *token,
		   GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(transport);
	g_autoptr(GTask) task = g_task_new(self, cancel, cb, data);
	g_autoptr(GTask) starter = NULL;
	g_autofree gchar *identity = NULL;
	GObject *rx = NULL, *tx = NULL;
	GstCaps *caps;
	GstBus *bus;
	GValue excluded = G_VALUE_INIT, value = G_VALUE_INIT;
	if (self->pipeline != NULL || !ai_livekit_transport_is_available() ||
		self->receive_token == NULL) {
		g_task_return_new_error(
			task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
			"LiveKit plugin missing, receive-token missing, or already joined");
		return;
	}
	self->leaving = FALSE;
	self->pipeline = gst_pipeline_new(NULL);
	g_signal_connect(self->pipeline, "deep-element-added", G_CALLBACK(configure_ice),
					 NULL);
	self->input = gst_element_factory_make("livekitwebrtcsrc", NULL);
	self->output = gst_element_factory_make("livekitwebrtcsink", NULL);
	self->appsrc = gst_element_factory_make("appsrc", NULL);
	g_object_set(self->input, "stun-server", NULL, NULL);
	g_object_set(self->output, "stun-server", NULL, NULL);
	gst_bin_add_many(GST_BIN(self->pipeline), self->input, self->appsrc, self->output,
					 NULL);
	caps = gst_caps_from_string(
		"audio/x-raw,format=S16LE,rate=16000,channels=1,layout=interleaved");
	g_object_set(self->appsrc, "caps", caps, "is-live", TRUE, "format", GST_FORMAT_TIME,
				 "do-timestamp", TRUE, "block", FALSE, NULL);
	gst_caps_unref(caps);
	gst_element_link_pads(self->appsrc, "src", self->output, "audio_%u");
	g_object_get(self->input, "signaller", &rx, NULL);
	g_object_get(self->output, "signaller", &tx, NULL);
	g_object_set(rx, "ws-url", self->url, "auth-token", self->receive_token, "room-name",
				 room, NULL);
	g_object_set(tx, "ws-url", self->url, "auth-token", token, "room-name", room, NULL);
	identity = token_identity(token);
	if (identity != NULL) {
		g_value_init(&excluded, GST_TYPE_ARRAY);
		g_value_init(&value, G_TYPE_STRING);
		g_value_set_string(&value, identity);
		gst_value_array_append_value(&excluded, &value);
		g_object_set_property(rx, "excluded-producer-peer-ids", &excluded);
		g_value_unset(&value);
		g_value_unset(&excluded);
	}
	g_object_unref(rx);
	g_object_unref(tx);
	g_signal_connect_object(self->input, "pad-added", G_CALLBACK(pad_added), self, 0);
	g_signal_connect_object(self->input, "pad-removed", G_CALLBACK(pad_removed), self, 0);
	bus = gst_element_get_bus(self->pipeline);
	self->bus_source = gst_bus_create_watch(bus);
	gst_object_unref(bus);
	g_source_set_callback(self->bus_source, G_SOURCE_FUNC(bus_message), self, NULL);
	g_source_attach(self->bus_source, self->context);
	self->joining = g_steal_pointer(&task);
	self->join_deadline = g_get_monotonic_time() + 15000000;
	self->clock = g_timeout_source_new(10);
	g_source_set_callback(self->clock, tick, self, NULL);
	g_source_attach(self->clock, self->context);
	starter = g_task_new(self, cancel, started, NULL);
	g_task_set_task_data(starter, gst_object_ref(self->pipeline),
						 (GDestroyNotify)gst_object_unref);
	g_task_run_in_thread(starter, start_pipeline);
}
static void
stop_pipeline(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	gst_element_set_state(data, GST_STATE_NULL);
	g_task_return_boolean(task, TRUE);
}
static GstElement *
detach(AiLivekitTransport *self)
{
	GstElement *pipeline = g_steal_pointer(&self->pipeline);
	clear_source(&self->clock);
	clear_source(&self->bus_source);
	flush(AI_AUDIO_TRANSPORT(self));
	if (self->joining != NULL) {
		g_task_return_new_error(self->joining, G_IO_ERROR, G_IO_ERROR_CANCELLED,
								"Left while joining");
		g_clear_object(&self->joining);
	}
	if (self->input != NULL)
		g_signal_handlers_disconnect_by_data(self->input, self);
	self->input = self->output = self->appsrc = NULL;
	return pipeline;
}
static void
leave_async(AiAudioTransport *transport, GCancellable *cancel, GAsyncReadyCallback cb,
			gpointer data)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(transport);
	g_autoptr(GTask) task = g_task_new(self, cancel, cb, data);
	GstElement *pipeline = detach(self);
	self->leaving = TRUE;
	if (pipeline == NULL) {
		g_task_return_boolean(task, TRUE);
		return;
	}
	g_task_set_task_data(task, pipeline, (GDestroyNotify)gst_object_unref);
	g_task_run_in_thread(task, stop_pipeline);
}
static gboolean
finish(AiAudioTransport *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
	return g_task_propagate_boolean(G_TASK(result), error);
}
static void
write_async(AiAudioTransport *transport, GBytes *pcm, GCancellable *cancel,
			GAsyncReadyCallback cb, gpointer data)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(transport);
	g_autoptr(GTask) task = g_task_new(self, cancel, cb, data);
	gsize size = g_bytes_get_size(pcm);
	Output *o;
	if (self->pipeline == NULL || size % 2 != 0 ||
		size + self->queued_bytes > 4 * 1024 * 1024) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"Transport not joined or invalid/oversized PCM");
		return;
	}
	o = g_new0(Output, 1);
	o->task = g_steal_pointer(&task);
	o->pcm = g_bytes_ref(pcm);
	self->queued_bytes += size;
	g_queue_push_tail(&self->playback, o);
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
	iface->flush = flush;
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(object);
	if (id == 1)
		g_value_set_string(value, self->url);
	else if (id == 2)
		g_value_set_string(value, self->receive_token);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(object);
	if (id == 1)
		self->url = g_value_dup_string(value);
	else if (id == 2)
		self->receive_token = g_value_dup_string(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
dispose(GObject *object)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(object);
	GstElement *pipeline = detach(self);
	if (pipeline != NULL) {
		g_autoptr(GTask) task = g_task_new(NULL, NULL, NULL, NULL);
		g_task_set_task_data(task, pipeline, (GDestroyNotify)gst_object_unref);
		g_task_run_in_thread(task, stop_pipeline);
	}
	G_OBJECT_CLASS(ai_livekit_transport_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiLivekitTransport *self = AI_LIVEKIT_TRANSPORT(object);
	g_free(self->url);
	g_free(self->receive_token);
	g_main_context_unref(self->context);
	G_OBJECT_CLASS(ai_livekit_transport_parent_class)->finalize(object);
}
static void
ai_livekit_transport_class_init(AiLivekitTransportClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->dispose = dispose;
	oc->finalize = finalize;
	g_object_class_install_property(
		oc, 1,
		g_param_spec_string("url", "URL", "LiveKit websocket endpoint", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
								G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, 2,
		g_param_spec_string(
			"receive-token", "Receive token", "JWT for a distinct receiving identity",
			NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}
static void
ai_livekit_transport_init(AiLivekitTransport *self)
{
	self->context = g_main_context_ref_thread_default();
}
/**
 * ai_livekit_transport_is_available:
 *
 * Returns: whether GStreamer has both required LiveKit elements
 */
gboolean
ai_livekit_transport_is_available(void)
{
	GstElementFactory *src, *sink;
	gboolean available;
	if (!gst_init_check(NULL, NULL, NULL))
		return FALSE;
	src = gst_element_factory_find("livekitwebrtcsrc");
	sink = gst_element_factory_find("livekitwebrtcsink");
	available = src != NULL && sink != NULL;
	if (src != NULL)
		gst_object_unref(src);
	if (sink != NULL)
		gst_object_unref(sink);
	return available;
}
/**
 * ai_livekit_transport_new:
 * @url: LiveKit websocket URL
 * @receive_token: JWT with a different identity from the publishing token
 *
 * Returns: (transfer full): a GStreamer transport
 */
AiLivekitTransport *
ai_livekit_transport_new(const gchar *url, const gchar *receive_token)
{
	g_autoptr(AiLivekitTransport) self = g_object_new(
		AI_TYPE_LIVEKIT_TRANSPORT, "url", url, "receive-token", receive_token, NULL);
	return g_steal_pointer(&self);
}
