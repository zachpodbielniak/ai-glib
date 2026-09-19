/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-http-synthesizer.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

struct _AiHttpSynthesizer {
	GObject parent_instance;
	gchar *url;
	SoupSession *http;
	gboolean busy;
};

typedef struct {
	SoupMessage *message;
	GInputStream *input;
	GByteArray *pending;
	guint rate;
	guint32 frame_size;
	guint64 input_samples, next_position;
	gint16 previous;
	gboolean header, length;
} Synthesis;

static void
synth_iface(AiSpeechSynthesizerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiHttpSynthesizer, ai_http_synthesizer, G_TYPE_OBJECT,
						G_IMPLEMENT_INTERFACE(AI_TYPE_SPEECH_SYNTHESIZER, synth_iface))

static void
synthesis_free(gpointer data)
{
	Synthesis *s = data;
	g_clear_object(&s->input);
	g_clear_object(&s->message);
	g_byte_array_unref(s->pending);
	g_free(s);
}

static void
complete(GTask *task, GError *error)
{
	AI_HTTP_SYNTHESIZER(g_task_get_source_object(task))->busy = FALSE;
	if (error != NULL)
		g_task_return_error(task, error);
	else
		g_task_return_boolean(task, TRUE);
	g_object_unref(task);
}

/* Integer linear interpolation, preserving phase across HTTP frames. */
static void
emit_pcm(GTask *task, const guint8 *data, guint len)
{
	Synthesis *s = g_task_get_task_data(task);
	g_autoptr(GByteArray) output = g_byte_array_new();
	g_autoptr(GBytes) pcm = NULL;
	guint i;
	for (i = 0; i < len; i += 2) {
		gint16 current = (gint16)((guint16)data[i] | ((guint16)data[i + 1] << 8));
		guint64 position = s->input_samples * 16000;
		while (s->next_position <= position) {
			gint64 value = current;
			guint8 encoded[2];
			if (s->input_samples != 0) {
				guint64 fraction = s->next_position - (s->input_samples - 1) * 16000;
				value = s->previous + ((gint64)current - s->previous) * fraction / 16000;
			}
			encoded[0] = (guint16)value & 255;
			encoded[1] = (guint16)value >> 8;
			g_byte_array_append(output, encoded, 2);
			s->next_position += s->rate;
		}
		s->previous = current;
		s->input_samples++;
	}
	if (output->len != 0 && !g_cancellable_is_cancelled(g_task_get_cancellable(task))) {
		pcm = g_byte_array_free_to_bytes(g_steal_pointer(&output));
		g_signal_emit_by_name(g_task_get_source_object(task), "audio", pcm);
	}
}

/* 1 = sentinel, 0 = need bytes, -1 = malformed. */
static gint
parse(GTask *task)
{
	Synthesis *s = g_task_get_task_data(task);
	GByteArray *b = s->pending;
	if (!s->header) {
		guint i;
		for (i = 0; i < b->len && b->data[i] != '\n'; i++)
			;
		if (i == b->len)
			return i > 16 ? -1 : 0;
		if (i < 4 || i > 16 || memcmp(b->data, "SR=", 3) != 0)
			return -1;
		{
			guint j;
			guint64 rate = 0;
			for (j = 3; j < i; j++) {
				if (!g_ascii_isdigit(b->data[j]))
					return -1;
				rate = rate * 10 + b->data[j] - '0';
				if (rate > 192000)
					return -1;
			}
			if (rate < 8000)
				return -1;
			s->rate = rate;
		}
		g_byte_array_remove_range(b, 0, i + 1);
		s->header = TRUE;
	}
	for (;;) {
		if (!s->length) {
			if (b->len < 4)
				return 0;
			s->frame_size = ((guint32)b->data[0] << 24) | ((guint32)b->data[1] << 16) |
							((guint32)b->data[2] << 8) | b->data[3];
			g_byte_array_remove_range(b, 0, 4);
			if (s->frame_size == 0)
				return b->len == 0 ? 1 : -1;
			if (s->frame_size > 1024 * 1024 || s->frame_size % 2 != 0)
				return -1;
			s->length = TRUE;
		}
		if (b->len < s->frame_size)
			return 0;
		emit_pcm(task, b->data, s->frame_size);
		g_byte_array_remove_range(b, 0, s->frame_size);
		s->length = FALSE;
	}
}

static void
read_next(GTask *task);

static void
read_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	Synthesis *s = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes =
		g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
	gsize n;
	const guint8 *p;
	gint status;
	if (bytes == NULL) {
		complete(task, g_steal_pointer(&error));
		return;
	}
	p = g_bytes_get_data(bytes, &n);
	if (n == 0) {
		complete(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
										   "TTS ended without a zero sentinel"));
		return;
	}
	g_byte_array_append(s->pending, p, n);
	status = parse(task);
	if (status != 0) {
		complete(task, status < 0
						   ? g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
												 "Invalid TTS frame or sample rate")
						   : NULL);
		return;
	}
	read_next(task);
}

static void
read_next(GTask *task)
{
	Synthesis *s = g_task_get_task_data(task);
	g_input_stream_read_bytes_async(s->input, 4096, G_PRIORITY_DEFAULT,
									g_task_get_cancellable(task), read_done, task);
}

static void
sent(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	Synthesis *s = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	s->input = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	if (s->input == NULL) {
		complete(task, g_steal_pointer(&error));
		return;
	}
	if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(s->message))) {
		complete(task, g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "TTS HTTP status %u",
								   soup_message_get_status(s->message)));
		return;
	}
	read_next(task);
}

static void
synthesize(AiSpeechSynthesizer *synth, const gchar *text, GCancellable *cancel,
		   GAsyncReadyCallback callback, gpointer user_data)
{
	AiHttpSynthesizer *self = AI_HTTP_SYNTHESIZER(synth);
	GTask *task = g_task_new(self, cancel, callback, user_data);
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *json = NULL;
	Synthesis *s;
	if (self->busy || text == NULL || !g_utf8_validate(text, -1, NULL)) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PENDING,
								"Synthesizer busy or invalid text");
		g_object_unref(task);
		return;
	}
	self->busy = TRUE;
	s = g_new0(Synthesis, 1);
	s->pending = g_byte_array_new();
	s->message = soup_message_new("POST", self->url);
	g_task_set_task_data(task, s, synthesis_free);
	if (s->message == NULL) {
		complete(task, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
										   "Invalid TTS URL"));
		return;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "text");
	json_builder_add_string_value(builder, text);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json = json_to_string(root, FALSE);
	body = g_bytes_new(json, strlen(json));
	soup_message_set_request_body_from_bytes(s->message, "application/json", body);
	soup_session_send_async(self->http, s->message, G_PRIORITY_DEFAULT, cancel, sent,
							task);
}

static gboolean
synthesize_finish(AiSpeechSynthesizer *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
	return g_task_propagate_boolean(G_TASK(result), error);
}

static void
synth_iface(AiSpeechSynthesizerInterface *iface)
{
	iface->synthesize_async = synthesize;
	iface->synthesize_finish = synthesize_finish;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		g_value_set_string(value, AI_HTTP_SYNTHESIZER(object)->url);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	if (id == 1)
		AI_HTTP_SYNTHESIZER(object)->url = g_value_dup_string(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
finalize(GObject *object)
{
	AiHttpSynthesizer *self = AI_HTTP_SYNTHESIZER(object);
	g_clear_object(&self->http);
	g_free(self->url);
	G_OBJECT_CLASS(ai_http_synthesizer_parent_class)->finalize(object);
}

static void
ai_http_synthesizer_class_init(AiHttpSynthesizerClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->finalize = finalize;
	oc->get_property = get_property;
	oc->set_property = set_property;
	g_object_class_install_property(
		oc, 1,
		g_param_spec_string("url", "URL", "Streaming TTS endpoint", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
								G_PARAM_STATIC_STRINGS));
}

static void
ai_http_synthesizer_init(AiHttpSynthesizer *self)
{
	self->http = soup_session_new_with_options("timeout", 30, NULL);
}

/**
 * ai_http_synthesizer_new:
 * @url: HTTP streaming synthesis endpoint
 *
 * Returns: (transfer full): a synthesizer emitting 16 kHz S16LE PCM
 */
AiHttpSynthesizer *
ai_http_synthesizer_new(const gchar *url)
{
	g_autoptr(AiHttpSynthesizer) self =
		g_object_new(AI_TYPE_HTTP_SYNTHESIZER, "url", url, NULL);
	return g_steal_pointer(&self);
}
