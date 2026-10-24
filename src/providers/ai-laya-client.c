/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-laya-client.h"
#include "ai-systemone.h"
#include "core/ai-error.h"

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

static void
laya_decide_async(AiDecider *decider, AiDecisionRequest *request,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	AiLayaClient *self = AI_LAYA_CLIENT(decider);
	AiSystemOneTarget target;

	target.kind = AI_SYSTEMONE_LAYA;
	target.base_url = self->base_url;
	target.model = self->model;
	target.api_key = self->api_key;
	target.timeout_ms = self->timeout_ms;
	target.long_context = FALSE;
	ai_systemone_decide_async(G_OBJECT(self), &target, request, cancellable, callback, user_data, laya_decide_async);
}
static AiDecisionResponse *
laya_decide_finish(AiDecider *self, GAsyncResult *result, GError **error)
{
	return ai_systemone_decide_finish(G_OBJECT(self), result, laya_decide_async, error);
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
