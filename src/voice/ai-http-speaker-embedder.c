/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-http-speaker-embedder.h"
#include "core/ai-http-redirect-private.h"
#include "core/ai-json-util.h"
#include <libsoup/soup.h>

/*
 * A voiceprint service over HTTP: POST raw 16 kHz mono S16LE, answered with
 * {"model":"...","embedding":[...]}. The service keeps nothing; less than
 * half a second may be refused with 422, which is "too short to tell".
 *
 * Two limits hold whatever the server does. timeout-ms bounds the whole
 * request, not just a silence on the socket: a server trickling one byte a
 * second would otherwise hold a naming forever. And the reply is read into
 * a fixed buffer one byte larger than AI_HTTP_SPEAKER_EMBEDDER_MAX_REPLY, so
 * an oversized body is refused without being buffered whole.
 */
typedef struct {
	gchar *url;
	guint timeout_ms;
	SoupSession *http;
} AiHttpSpeakerEmbedderPrivate;

enum { PROP_0, PROP_URL, PROP_TIMEOUT, N_PROPS };
static GParamSpec *props[N_PROPS];

static void embedder_iface_init(AiSpeakerEmbedderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiHttpSpeakerEmbedder, ai_http_speaker_embedder, G_TYPE_OBJECT,
						G_ADD_PRIVATE(AiHttpSpeakerEmbedder)
							G_IMPLEMENT_INTERFACE(AI_TYPE_SPEAKER_EMBEDDER, embedder_iface_init))

/* One request: the message, the capped buffer, and the deadline. */
typedef struct {
	SoupMessage *message;
	guint8 *buffer;
	GCancellable *cancel; /* ours: the caller's, or the deadline's */
	gulong caller_cancel_id;
	GCancellable *caller;
	GSource *deadline;
	gboolean timed_out;
} Request;
static void
request_free(gpointer data)
{
	Request *r = data;
	if (r->deadline != NULL) {
		g_source_destroy(r->deadline);
		g_source_unref(r->deadline);
	}
	if (r->caller_cancel_id != 0)
		g_cancellable_disconnect(r->caller, r->caller_cancel_id);
	g_clear_object(&r->caller);
	g_clear_object(&r->message);
	g_clear_object(&r->cancel);
	g_free(r->buffer);
	g_free(r);
}
static gboolean
deadline_passed(gpointer data)
{
	Request *r = data;
	r->timed_out = TRUE;
	g_cancellable_cancel(r->cancel);
	return G_SOURCE_REMOVE;
}
/* The caller's cancel, forwarded to ours. Any thread. */
static void
caller_cancelled(GCancellable *caller, gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
}
/* Turn our own cancellation into the error the caller expects to see. */
static void
return_error(GTask *task, Request *r, GError *error)
{
	if (r->timed_out && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_clear_error(&error);
		error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
									"Voiceprint service did not answer in time");
	}
	g_task_return_error(task, error);
	g_object_unref(task);
}
static AiVoiceprint *
default_parse_reply(AiHttpSpeakerEmbedder *self, GBytes *body, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	gsize size;
	const gchar *raw = g_bytes_get_data(body, &size);
	if (size == 0 || !json_parser_load_from_data(parser, raw, (gssize)size, NULL)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Voiceprint service answered with something other than JSON");
		return NULL;
	}
	return ai_voiceprint_new_from_json(json_parser_get_root(parser), error);
}
static void
body_read(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	AiHttpSpeakerEmbedder *self = g_task_get_source_object(task);
	Request *r = g_task_get_task_data(task);
	GError *error = NULL;
	g_autoptr(GBytes) body = NULL;
	AiVoiceprint *print;
	gsize got = 0;
	guint status;
	if (!g_input_stream_read_all_finish(G_INPUT_STREAM(source), result, &got, &error)) {
		return_error(task, r, error);
		return;
	}
	g_input_stream_close(G_INPUT_STREAM(source), NULL, NULL);
	if (got > AI_HTTP_SPEAKER_EMBEDDER_MAX_REPLY) {
		return_error(task, r,
					 g_error_new(G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
								 "Voiceprint service reply is larger than %" G_GSIZE_FORMAT
								 " bytes",
								 AI_HTTP_SPEAKER_EMBEDDER_MAX_REPLY));
		return;
	}
	body = g_bytes_new(r->buffer, got);
	status = soup_message_get_status(r->message);
	if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *o = NULL;
		const gchar *why;
		if (got > 0 && json_parser_load_from_data(parser, (const gchar *)r->buffer,
												  (gssize)got, NULL))
			o = ai_json_root_object(parser);
		why = ai_json_get_string(o, "error", NULL);
		return_error(task, r,
					 g_error_new(G_IO_ERROR,
								 status == 422 ? G_IO_ERROR_INVALID_DATA : G_IO_ERROR_FAILED,
								 "Voiceprint service: HTTP %u%s%s", status,
								 why != NULL ? ": " : "", why != NULL ? why : ""));
		return;
	}
	print = AI_HTTP_SPEAKER_EMBEDDER_GET_CLASS(self)->parse_reply(self, body, &error);
	if (print == NULL) {
		return_error(task, r, error);
		return;
	}
	g_task_return_pointer(task, print, g_object_unref);
	g_object_unref(task);
}
static void
sent(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	Request *r = g_task_get_task_data(task);
	GError *error = NULL;
	g_autoptr(GInputStream) stream =
		soup_session_send_finish(SOUP_SESSION(source), result, &error);
	if (stream == NULL || ai_http_redirect_refused(r->message, &error)) {
		return_error(task, r, error);
		return;
	}
	/* One byte past the cap: reading it means the body was too large. */
	r->buffer = g_malloc(AI_HTTP_SPEAKER_EMBEDDER_MAX_REPLY + 1);
	g_input_stream_read_all_async(stream, r->buffer, AI_HTTP_SPEAKER_EMBEDDER_MAX_REPLY + 1,
								  G_PRIORITY_DEFAULT, r->cancel, body_read, task);
}
static void
embed_async(AiSpeakerEmbedder *embedder, GBytes *pcm, GCancellable *cancellable,
			GAsyncReadyCallback callback, gpointer user_data)
{
	AiHttpSpeakerEmbedder *self = AI_HTTP_SPEAKER_EMBEDDER(embedder);
	AiHttpSpeakerEmbedderPrivate *priv = ai_http_speaker_embedder_get_instance_private(self);
	GTask *task = g_task_new(self, cancellable, callback, user_data);
	Request *r = g_new0(Request, 1);
	g_task_set_task_data(task, r, request_free);
	if (priv->url == NULL ||
		(r->message = ai_http_message_new("POST", priv->url, AI_HTTP_NO_CREDENTIALS)) == NULL) {
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"No usable voiceprint service URL");
		g_object_unref(task);
		return;
	}
	soup_message_set_request_body_from_bytes(r->message, "application/octet-stream", pcm);
	if (AI_HTTP_SPEAKER_EMBEDDER_GET_CLASS(self)->prepare_message != NULL)
		AI_HTTP_SPEAKER_EMBEDDER_GET_CLASS(self)->prepare_message(self, r->message);
	/* A header a subclass added may carry a credential: judge it again. */
	ai_http_guard_message(r->message);
	r->cancel = g_cancellable_new();
	if (cancellable != NULL) {
		r->caller = g_object_ref(cancellable);
		r->caller_cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(caller_cancelled),
													g_object_ref(r->cancel), g_object_unref);
	}
	if (priv->timeout_ms > 0) {
		r->deadline = g_timeout_source_new(priv->timeout_ms);
		g_source_set_callback(r->deadline, deadline_passed, r, NULL);
		g_source_attach(r->deadline, g_main_context_get_thread_default());
	}
	soup_session_send_async(priv->http, r->message, G_PRIORITY_DEFAULT, r->cancel, sent, task);
}
static AiVoiceprint *
embed_finish(AiSpeakerEmbedder *embedder, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, embedder), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
static void
embedder_iface_init(AiSpeakerEmbedderInterface *iface)
{
	iface->embed_async = embed_async;
	iface->embed_finish = embed_finish;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiHttpSpeakerEmbedderPrivate *priv =
		ai_http_speaker_embedder_get_instance_private(AI_HTTP_SPEAKER_EMBEDDER(object));
	if (id == PROP_URL)
		g_value_set_string(value, priv->url);
	else if (id == PROP_TIMEOUT)
		g_value_set_uint(value, priv->timeout_ms);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiHttpSpeakerEmbedderPrivate *priv =
		ai_http_speaker_embedder_get_instance_private(AI_HTTP_SPEAKER_EMBEDDER(object));
	if (id == PROP_URL) {
		g_free(priv->url);
		priv->url = g_value_dup_string(value);
	} else if (id == PROP_TIMEOUT)
		priv->timeout_ms = g_value_get_uint(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
finalize(GObject *object)
{
	AiHttpSpeakerEmbedderPrivate *priv =
		ai_http_speaker_embedder_get_instance_private(AI_HTTP_SPEAKER_EMBEDDER(object));
	g_free(priv->url);
	g_clear_object(&priv->http);
	G_OBJECT_CLASS(ai_http_speaker_embedder_parent_class)->finalize(object);
}
static void
ai_http_speaker_embedder_class_init(AiHttpSpeakerEmbedderClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	klass->parse_reply = default_parse_reply;
	/**
	 * AiHttpSpeakerEmbedder:url:
	 *
	 * The service's embedding endpoint. It follows a redirect only to the
	 * same scheme, host and port.
	 */
	props[PROP_URL] = g_param_spec_string("url", "URL", "Voiceprint service embedding endpoint",
										  NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
													G_PARAM_STATIC_STRINGS);
	/**
	 * AiHttpSpeakerEmbedder:timeout-ms:
	 *
	 * The longest one request may take, start to finish. 0 never gives up.
	 */
	props[PROP_TIMEOUT] = g_param_spec_uint("timeout-ms", "Timeout", "Longest request", 0,
											600000, 5000,
											G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
												G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
}
static void
ai_http_speaker_embedder_init(AiHttpSpeakerEmbedder *self)
{
	AiHttpSpeakerEmbedderPrivate *priv = ai_http_speaker_embedder_get_instance_private(self);
	priv->http = ai_http_session_new();
}

/**
 * ai_http_speaker_embedder_new:
 * @url: the voiceprint service's embedding endpoint
 *
 * Returns: (transfer full): a new embedder
 */
AiHttpSpeakerEmbedder *
ai_http_speaker_embedder_new(const gchar *url)
{
	return g_object_new(AI_TYPE_HTTP_SPEAKER_EMBEDDER, "url", url, NULL);
}

/**
 * ai_http_speaker_embedder_get_url:
 * @self: an embedder
 *
 * Returns: (nullable): the service URL
 */
const gchar *
ai_http_speaker_embedder_get_url(AiHttpSpeakerEmbedder *self)
{
	g_return_val_if_fail(AI_IS_HTTP_SPEAKER_EMBEDDER(self), NULL);
	return ((AiHttpSpeakerEmbedderPrivate *)ai_http_speaker_embedder_get_instance_private(self))
		->url;
}
