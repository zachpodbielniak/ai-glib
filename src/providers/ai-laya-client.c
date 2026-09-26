/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-laya-client.h"
#include "ai-image-shared.h"
#include "core/ai-json-util.h"
#include "core/ai-error.h"
#include <string.h>

struct _AiLayaClient
{
	GObject parent_instance;
	gchar *base_url, *model, *api_key;
	guint timeout_ms;
};
static void decider_init(AiDeciderInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiLayaClient, ai_laya_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_DECIDER, decider_init))
enum { PROP_0, PROP_BASE_URL, PROP_MODEL, PROP_API_KEY, PROP_TIMEOUT_MS, N_PROPS };
static GParamSpec *properties[N_PROPS];

typedef struct
{
	SoupSession *session;
	AiDecisionRequest *request;
	GCancellable *cancel, *parent_cancel;
	gulong cancel_id;
	GSource *deadline;
	gint64 started;
	gboolean timed_out;
} Operation;

static void
operation_free(Operation *op)
{
	if (op->deadline != NULL) { g_source_destroy(op->deadline); g_source_unref(op->deadline); }
	if (op->cancel_id != 0) g_cancellable_disconnect(op->parent_cancel, op->cancel_id);
	g_clear_object(&op->parent_cancel);
	g_clear_object(&op->cancel);
	g_clear_object(&op->session);
	g_clear_object(&op->request);
	g_free(op);
}
static void forward_cancel(GCancellable *parent, gpointer data)
{
	(void)parent;
	g_cancellable_cancel(data);
}
static gboolean deadline_expired(gpointer data)
{
	Operation *op = data;
	op->timed_out = TRUE;
	g_clear_pointer(&op->deadline, g_source_unref);
	g_cancellable_cancel(op->cancel);
	return G_SOURCE_REMOVE;
}
static void
on_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
	g_autoptr(GTask) task = user_data;
	Operation *op = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = ai_image_shared_send_finish(result, &error);
	g_autoptr(AiDecisionResponse) response = NULL;
	g_autofree gchar *json = NULL;
	gconstpointer data;
	gsize size;
	(void)source;
	if (op->deadline != NULL)
	{
		g_source_destroy(op->deadline);
		g_clear_pointer(&op->deadline, g_source_unref);
	}
	if (g_task_return_error_if_cancelled(task)) return;
	if (op->timed_out)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_TIMEOUT, "Laya decision deadline expired");
		return;
	}
	if (bytes == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	data = g_bytes_get_data(bytes, &size);
	if (size > 4 * 1024 * 1024 || memchr(data, '\0', size) != NULL || !g_utf8_validate(data, size, NULL))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_RESPONSE, "Invalid Laya response encoding or size");
		return;
	}
	json = g_strndup(data, size);
	response = ai_decision_response_new_from_json(op->request, json,
		(g_get_monotonic_time() - op->started) / 1000.0, &error);
	if (response == NULL) g_task_return_error(task, g_steal_pointer(&error));
	else g_task_return_pointer(task, g_steal_pointer(&response), g_object_unref);
}
static gboolean known_model(const gchar *model)
{
	return model == NULL || g_str_equal(model, AI_LAYA_MODEL_ENGLISH) ||
		g_str_equal(model, AI_LAYA_MODEL_MULTILINGUAL) || g_str_equal(model, AI_LAYA_MODEL_TYPED_DECISIONS);
}
static void
laya_decide_async(AiDecider *decider, AiDecisionRequest *request,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	AiLayaClient *self = AI_LAYA_CLIENT(decider);
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *json = ai_decision_request_dup_json(request);
	g_autofree gchar *url = NULL;
	g_autofree gchar *encoded = NULL;
	g_autofree gchar *authorization = NULL;
	const gchar *model, *scheme;
	JsonObject *root;
	Operation *op;
	gsize length;
	g_task_set_source_tag(task, laya_decide_async);
	if (g_task_return_error_if_cancelled(task)) return;
	json_parser_load_from_data(parser, json, -1, NULL);
	root = ai_json_root_object(parser);
	model = ai_json_get_string(root, "model", self->model);
	if (!known_model(model))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_MODEL_NOT_FOUND, "Unknown Laya checkpoint: %s", model);
		return;
	}
	if (model != NULL) json_object_set_string_member(root, "model", model);
	if (self->base_url != NULL) uri = g_uri_parse(self->base_url, G_URI_FLAGS_NONE, NULL);
	scheme = uri != NULL ? g_uri_get_scheme(uri) : NULL;
	if (uri == NULL || (g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0) ||
		g_uri_get_host(uri) == NULL || g_uri_get_userinfo(uri) != NULL ||
		g_uri_get_query(uri) != NULL || g_uri_get_fragment(uri) != NULL ||
		(self->api_key != NULL && strpbrk(self->api_key, "\r\n") != NULL))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR, "Laya needs an HTTP(S) base URL without credentials, query or fragment and a valid API key");
		return;
	}
	length = strlen(self->base_url);
	while (length > 0 && self->base_url[length - 1] == '/') length--;
	url = g_strdup_printf("%.*s/v1/systemone", (gint)length, self->base_url);
	message = soup_message_new("POST", url);
	if (message == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR, "Invalid Laya URL");
		return;
	}
	/* Keep classified text and credentials at the configured endpoint. */
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (self->api_key != NULL && *self->api_key != '\0')
	{
		authorization = g_strconcat("Bearer ", self->api_key, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(message), "Authorization", authorization);
	}
	encoded = json_to_string(json_parser_get_root(parser), FALSE);
	if (strlen(encoded) > 2 * 1024 * 1024)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Laya request exceeds 2 MiB");
		return;
	}
	body = g_bytes_new(encoded, strlen(encoded));
	soup_message_set_request_body_from_bytes(message, "application/json", body);
	op = g_new0(Operation, 1);
	op->session = soup_session_new();
	op->request = g_object_ref(request);
	op->cancel = g_cancellable_new();
	op->started = g_get_monotonic_time();
	if (cancellable != NULL)
	{
		op->parent_cancel = g_object_ref(cancellable);
		op->cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(forward_cancel), g_object_ref(op->cancel), g_object_unref);
	}
	op->deadline = g_timeout_source_new(self->timeout_ms);
	g_source_set_callback(op->deadline, deadline_expired, op, NULL);
	g_source_attach(op->deadline, g_main_context_get_thread_default());
	g_task_set_task_data(task, op, (GDestroyNotify)operation_free);
	/* A classification is latency-sensitive. Do not retry implicitly. */
	ai_image_shared_send_async(op->session, message, body, 0, op->cancel, on_reply, g_steal_pointer(&task));
}
static AiDecisionResponse *
laya_decide_finish(AiDecider *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	g_return_val_if_fail(g_async_result_is_tagged(result, laya_decide_async), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
static void decider_init(AiDeciderInterface *iface)
{
	iface->decide_async = laya_decide_async;
	iface->decide_finish = laya_decide_finish;
}
static void
ai_laya_client_finalize(GObject *object)
{
	AiLayaClient *self = AI_LAYA_CLIENT(object);
	g_free(self->base_url); g_free(self->model); g_free(self->api_key);
	G_OBJECT_CLASS(ai_laya_client_parent_class)->finalize(object);
}
static void
ai_laya_client_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiLayaClient *self = AI_LAYA_CLIENT(object);
	switch (id)
	{
		case PROP_BASE_URL: g_value_set_string(value, self->base_url); break;
		case PROP_MODEL: g_value_set_string(value, self->model); break;
		case PROP_API_KEY: g_value_set_string(value, self->api_key); break;
		case PROP_TIMEOUT_MS: g_value_set_uint(value, self->timeout_ms); break;
		default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
ai_laya_client_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiLayaClient *self = AI_LAYA_CLIENT(object);
	gchar **target = NULL;
	switch (id)
	{
		case PROP_BASE_URL: target = &self->base_url; break;
		case PROP_MODEL: target = &self->model; break;
		case PROP_API_KEY: target = &self->api_key; break;
		case PROP_TIMEOUT_MS: self->timeout_ms = g_value_get_uint(value); return;
		default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); return;
	}
	g_free(*target); *target = g_value_dup_string(value);
}
static void
ai_laya_client_class_init(AiLayaClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->finalize = ai_laya_client_finalize;
	object_class->get_property = ai_laya_client_get_property;
	object_class->set_property = ai_laya_client_set_property;
	/**
	 * AiLayaClient:base-url:
	 * Base URL of an independently managed Laya server. No service is started.
	 */
	properties[PROP_BASE_URL] = g_param_spec_string("base-url", NULL, NULL, "http://127.0.0.1:8000", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiLayaClient:model:
	 * Default checkpoint: english, multilingual, typed-decisions, or NULL for routing.
	 */
	properties[PROP_MODEL] = g_param_spec_string("model", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiLayaClient:api-key:
	 * Optional bearer token. Never sent as a URL parameter.
	 */
	properties[PROP_API_KEY] = g_param_spec_string("api-key", NULL, NULL, NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiLayaClient:timeout-ms:
	 * Whole-operation deadline in milliseconds.
	 */
	properties[PROP_TIMEOUT_MS] = g_param_spec_uint("timeout-ms", NULL, NULL, 1, G_MAXUINT, 30000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, properties);
}
static void
ai_laya_client_init(AiLayaClient *self)
{
	self->base_url = g_strdup("http://127.0.0.1:8000");
	self->timeout_ms = 30000;
}
/**
 * ai_laya_client_new:
 *
 * Creates an HTTP client without I/O, dependencies on inference runtimes,
 * subprocesses or weight downloads. Set properties before calling AiDecider.
 * Returns: (transfer full): a Laya client
 */
AiLayaClient *ai_laya_client_new(void)
{
	g_autoptr(AiLayaClient) self = g_object_new(AI_TYPE_LAYA_CLIENT, NULL);
	return g_steal_pointer(&self);
}
