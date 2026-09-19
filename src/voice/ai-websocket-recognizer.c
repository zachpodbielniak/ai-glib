/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-websocket-recognizer.h"
#include "core/ai-json-util.h"
#include <libsoup/soup.h>

/* One independently cancellable connection per utterance/participant. */
typedef struct {
	grefcount refs;
	GWeakRef owner;
	gchar *speaker;
	GCancellable *cancel;
	GSource *deadline;
	SoupWebsocketConnection *ws;
	GQueue queued;
	gsize queued_bytes;
	gboolean ready, ended, active;
} Recognition;

struct _AiWebsocketRecognizer {
	GObject parent_instance;
	gchar *url;
	SoupSession *http;
	GHashTable *streams;
	guint timeout_ms;
};
static void
recognizer_iface(AiSpeechRecognizerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiWebsocketRecognizer, ai_websocket_recognizer, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_SPEECH_RECOGNIZER,
											  recognizer_iface))

static void
recognition_unref(Recognition *r)
{
	if (!g_ref_count_dec(&r->refs))
		return;
	g_weak_ref_clear(&r->owner);
	g_clear_object(&r->cancel);
	g_clear_object(&r->ws);
	g_queue_clear_full(&r->queued, (GDestroyNotify)g_bytes_unref);
	g_free(r->speaker);
	g_free(r);
}

static void
recognition_stop(gpointer data)
{
	Recognition *r = data;
	r->active = FALSE;
	if (r->deadline != NULL) {
		g_source_destroy(r->deadline);
		g_clear_pointer(&r->deadline, g_source_unref);
	}
	g_cancellable_cancel(r->cancel);
	if (r->ws != NULL) {
		g_signal_handlers_disconnect_by_data(r->ws, r);
		if (soup_websocket_connection_get_state(r->ws) == SOUP_WEBSOCKET_STATE_OPEN)
			soup_websocket_connection_close(r->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
	}
	recognition_unref(r);
}

static void
recognition_error(Recognition *r, const GError *error)
{
	g_autoptr(AiWebsocketRecognizer) self = g_weak_ref_get(&r->owner);
	g_autofree gchar *speaker = g_strdup(r->speaker);
	if (!r->active || self == NULL)
		return;
	g_hash_table_remove(self->streams, speaker);
	g_signal_emit_by_name(self, "error", speaker, error);
}

static gboolean
recognition_timeout(gpointer data)
{
	Recognition *r = data;
	g_autoptr(GError) error =
		g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
							"STT timed out waiting for ready or final transcript");
	recognition_error(r, error);
	return G_SOURCE_REMOVE;
}
static void
arm_deadline(Recognition *r, guint milliseconds)
{
	if (r->deadline != NULL) {
		g_source_destroy(r->deadline);
		g_clear_pointer(&r->deadline, g_source_unref);
	}
	r->deadline = g_timeout_source_new(milliseconds);
	g_ref_count_inc(&r->refs);
	g_source_set_callback(r->deadline, recognition_timeout, r,
						  (GDestroyNotify)recognition_unref);
	g_source_attach(r->deadline, g_main_context_get_thread_default());
}
static void
on_closed(SoupWebsocketConnection *ws, gpointer data)
{
	g_autoptr(GError) error = g_error_new_literal(
		G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED, "STT closed before a final transcript");
	recognition_error(data, error);
}

static void
on_ws_error(SoupWebsocketConnection *ws, GError *error, gpointer data)
{
	recognition_error(data, error);
}

static void
on_message(SoupWebsocketConnection *ws, gint kind, GBytes *bytes, gpointer data)
{
	Recognition *r = data;
	g_autoptr(AiWebsocketRecognizer) self = g_weak_ref_get(&r->owner);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *obj;
	const gchar *type, *text;
	gsize len;
	const gchar *raw = g_bytes_get_data(bytes, &len);
	if (self == NULL || !r->active)
		return;
	if (kind != SOUP_WEBSOCKET_DATA_TEXT || len > 65536 ||
		!json_parser_load_from_data(parser, raw, len, NULL) ||
		(obj = ai_json_root_object(parser)) == NULL)
		goto malformed;
	type = ai_json_get_string(obj, "type", NULL);
	if (g_strcmp0(type, "ready") == 0 && !r->ready) {
		GBytes *frame;
		r->ready = TRUE;
		if (r->deadline != NULL) {
			g_source_destroy(r->deadline);
			g_clear_pointer(&r->deadline, g_source_unref);
		}
		if (r->ended)
			arm_deadline(r, self->timeout_ms);
		while ((frame = g_queue_pop_head(&r->queued)) != NULL) {
			soup_websocket_connection_send_message(ws, SOUP_WEBSOCKET_DATA_BINARY, frame);
			g_bytes_unref(frame);
		}
		r->queued_bytes = 0;
		if (r->ended)
			soup_websocket_connection_send_text(ws, "EOS");
		return;
	}
	if (g_strcmp0(type, "error") == 0) {
		const gchar *detail =
			ai_json_get_string(obj, "detail", "Speech recognition failed");
		error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, detail);
		recognition_error(r, error);
		return;
	}
	text = ai_json_get_string(obj, "text", NULL);
	if (text == NULL || !r->ready || !g_utf8_validate(text, -1, NULL))
		goto malformed;
	if (g_strcmp0(type, "partial") == 0) {
		g_signal_emit_by_name(self, "transcript", r->speaker, text, FALSE);
		return;
	}
	if (g_strcmp0(type, "final") == 0 && r->ended) {
		g_autofree gchar *speaker = g_strdup(r->speaker);
		g_hash_table_remove(self->streams, speaker);
		g_signal_emit_by_name(self, "transcript", speaker, text, TRUE);
		return;
	}
malformed:
	error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
								"Invalid STT protocol message");
	recognition_error(r, error);
}

static void
on_connected(GObject *source, GAsyncResult *result, gpointer data)
{
	Recognition *r = data;
	g_autoptr(GError) error = NULL;
	r->ws = soup_session_websocket_connect_finish(SOUP_SESSION(source), result, &error);
	if (r->active && r->ws != NULL) {
		soup_websocket_connection_set_max_incoming_payload_size(r->ws, 65536);
		g_signal_connect(r->ws, "message", G_CALLBACK(on_message), r);
		g_signal_connect(r->ws, "closed", G_CALLBACK(on_closed), r);
		g_signal_connect(r->ws, "error", G_CALLBACK(on_ws_error), r);
	} else if (r->active)
		recognition_error(r, error);
	else if (r->ws != NULL)
		soup_websocket_connection_close(r->ws, SOUP_WEBSOCKET_CLOSE_NORMAL, NULL);
	recognition_unref(r);
}

static gboolean
begin(AiSpeechRecognizer *recognizer, const gchar *speaker, GError **error)
{
	AiWebsocketRecognizer *self = AI_WEBSOCKET_RECOGNIZER(recognizer);
	g_autoptr(SoupMessage) msg = NULL;
	Recognition *r;
	if (speaker == NULL || speaker[0] == '\0' ||
		g_hash_table_contains(self->streams, speaker)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING,
							"STT stream already active or invalid id");
		return FALSE;
	}
	msg = soup_message_new("GET", self->url);
	if (msg == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
							"Invalid STT URL");
		return FALSE;
	}
	r = g_new0(Recognition, 1);
	g_ref_count_init(&r->refs);
	g_weak_ref_init(&r->owner, self);
	r->speaker = g_strdup(speaker);
	r->cancel = g_cancellable_new();
	r->active = TRUE;
	g_hash_table_insert(self->streams, g_strdup(speaker), r);
	arm_deadline(r, self->timeout_ms);
	g_ref_count_inc(&r->refs);
	soup_session_websocket_connect_async(self->http, msg, NULL, NULL, G_PRIORITY_DEFAULT,
										 r->cancel, on_connected, r);
	return TRUE;
}

static gboolean
feed(AiSpeechRecognizer *recognizer, const gchar *speaker, GBytes *pcm, GError **error)
{
	AiWebsocketRecognizer *self = AI_WEBSOCKET_RECOGNIZER(recognizer);
	Recognition *r = g_hash_table_lookup(self->streams, speaker);
	gsize n = g_bytes_get_size(pcm);
	if (r == NULL || r->ended || n % 2 != 0 || n > 32000 ||
		r->queued_bytes + n > 320000) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Invalid PCM, closed STT stream, or STT queue full");
		return FALSE;
	}
	if (r->ready)
		soup_websocket_connection_send_message(r->ws, SOUP_WEBSOCKET_DATA_BINARY, pcm);
	else {
		g_queue_push_tail(&r->queued, g_bytes_ref(pcm));
		r->queued_bytes += n;
	}
	return TRUE;
}

static void
end(AiSpeechRecognizer *recognizer, const gchar *speaker)
{
	Recognition *r =
		g_hash_table_lookup(AI_WEBSOCKET_RECOGNIZER(recognizer)->streams, speaker);
	if (r == NULL || r->ended)
		return;
	r->ended = TRUE;
	arm_deadline(r, AI_WEBSOCKET_RECOGNIZER(recognizer)->timeout_ms);
	if (r->ready)
		soup_websocket_connection_send_text(r->ws, "EOS");
}

static void
cancel(AiSpeechRecognizer *recognizer, const gchar *speaker)
{
	g_hash_table_remove(AI_WEBSOCKET_RECOGNIZER(recognizer)->streams, speaker);
}

static void
recognizer_iface(AiSpeechRecognizerInterface *iface)
{
	iface->begin = begin;
	iface->feed = feed;
	iface->end = end;
	iface->cancel = cancel;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		g_value_set_string(value, AI_WEBSOCKET_RECOGNIZER(object)->url);
	else if (id == 2)
		g_value_set_uint(value, AI_WEBSOCKET_RECOGNIZER(object)->timeout_ms);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		AI_WEBSOCKET_RECOGNIZER(object)->url = g_value_dup_string(value);
	else if (id == 2)
		AI_WEBSOCKET_RECOGNIZER(object)->timeout_ms = g_value_get_uint(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
dispose(GObject *object)
{
	AiWebsocketRecognizer *self = AI_WEBSOCKET_RECOGNIZER(object);
	g_hash_table_remove_all(self->streams);
	g_clear_object(&self->http);
	G_OBJECT_CLASS(ai_websocket_recognizer_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
	AiWebsocketRecognizer *self = AI_WEBSOCKET_RECOGNIZER(object);
	g_hash_table_unref(self->streams);
	g_free(self->url);
	G_OBJECT_CLASS(ai_websocket_recognizer_parent_class)->finalize(object);
}

static void
ai_websocket_recognizer_class_init(AiWebsocketRecognizerClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->dispose = dispose;
	oc->finalize = finalize;
	oc->get_property = get_property;
	oc->set_property = set_property;
	g_object_class_install_property(
		oc, 2,
		g_param_spec_uint("timeout-ms", "Timeout", "Ready/final response deadline", 1,
						  300000, 30000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, 1,
		g_param_spec_string("url", "URL", "STT websocket endpoint", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
								G_PARAM_STATIC_STRINGS));
}

static void
ai_websocket_recognizer_init(AiWebsocketRecognizer *self)
{
	self->timeout_ms = 30000;
	self->http = soup_session_new_with_options("timeout", 30, NULL);
	self->streams =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, recognition_stop);
}

/**
 * ai_websocket_recognizer_new:
 * @url: websocket endpoint
 *
 * Returns: (transfer full): a streaming recognizer
 */
AiWebsocketRecognizer *
ai_websocket_recognizer_new(const gchar *url)
{
	g_autoptr(AiWebsocketRecognizer) self =
		g_object_new(AI_TYPE_WEBSOCKET_RECOGNIZER, "url", url, NULL);
	return g_steal_pointer(&self);
}
