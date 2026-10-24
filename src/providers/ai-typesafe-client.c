/*
 * ai-typesafe-client.c - TypeSafe System One client (Jev)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "ai-typesafe-client.h"
#include "ai-systemone.h"

struct _AiTypesafeClient
{
	GObject parent_instance;
	gchar *base_url;
	gchar *model;
	gchar *api_key;
	guint timeout_ms;
	gboolean long_context;
};

static void decider_init(AiDeciderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiTypesafeClient, ai_typesafe_client, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_DECIDER, decider_init))

enum { PROP_0, PROP_BASE_URL, PROP_MODEL, PROP_API_KEY, PROP_TIMEOUT_MS, PROP_LONG_CONTEXT, N_PROPS };

static GParamSpec *properties[N_PROPS];

static void
typesafe_decide_async(AiDecider *decider, AiDecisionRequest *request,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	AiTypesafeClient *self = AI_TYPESAFE_CLIENT(decider);
	AiSystemOneTarget target;

	target.kind = AI_SYSTEMONE_TYPESAFE;
	target.base_url = self->base_url;
	target.model = self->model;
	target.api_key = self->api_key;
	target.timeout_ms = self->timeout_ms;
	target.long_context = self->long_context;
	ai_systemone_decide_async(G_OBJECT(self), &target, request, cancellable,
		callback, user_data, typesafe_decide_async);
}

static AiDecisionResponse *
typesafe_decide_finish(AiDecider *self, GAsyncResult *result, GError **error)
{
	return ai_systemone_decide_finish(G_OBJECT(self), result, typesafe_decide_async, error);
}

static void
decider_init(AiDeciderInterface *iface)
{
	iface->decide_async = typesafe_decide_async;
	iface->decide_finish = typesafe_decide_finish;
}

static void
ai_typesafe_client_finalize(GObject *object)
{
	AiTypesafeClient *self = AI_TYPESAFE_CLIENT(object);

	g_free(self->base_url);
	g_free(self->model);
	g_free(self->api_key);
	G_OBJECT_CLASS(ai_typesafe_client_parent_class)->finalize(object);
}

static void
ai_typesafe_client_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiTypesafeClient *self = AI_TYPESAFE_CLIENT(object);

	switch (id)
	{
		case PROP_BASE_URL: g_value_set_string(value, self->base_url); break;
		case PROP_MODEL: g_value_set_string(value, self->model); break;
		case PROP_API_KEY: g_value_set_string(value, self->api_key); break;
		case PROP_TIMEOUT_MS: g_value_set_uint(value, self->timeout_ms); break;
		case PROP_LONG_CONTEXT: g_value_set_boolean(value, self->long_context); break;
		default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}

static void
ai_typesafe_client_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiTypesafeClient *self = AI_TYPESAFE_CLIENT(object);
	gchar **target = NULL;

	switch (id)
	{
		case PROP_BASE_URL: target = &self->base_url; break;
		case PROP_MODEL: target = &self->model; break;
		case PROP_API_KEY: target = &self->api_key; break;
		case PROP_TIMEOUT_MS: self->timeout_ms = g_value_get_uint(value); return;
		case PROP_LONG_CONTEXT: self->long_context = g_value_get_boolean(value); return;
		default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec); return;
	}
	g_free(*target);
	*target = g_value_dup_string(value);
}

static void
ai_typesafe_client_class_init(AiTypesafeClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = ai_typesafe_client_finalize;
	object_class->get_property = ai_typesafe_client_get_property;
	object_class->set_property = ai_typesafe_client_set_property;
	/**
	 * AiTypesafeClient:base-url:
	 *
	 * API origin. `TYPESAFE_BASE_URL` supplies the initial value.
	 * Point this at an Ollama server only when you want the TypeSafe
	 * request shape against it; AiOllamaClient is the local client.
	 */
	properties[PROP_BASE_URL] = g_param_spec_string("base-url", NULL, NULL,
		"https://api.typesafe.ai", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiTypesafeClient:model:
	 *
	 * Default model when the request does not name one. `jev-latest`
	 * unless `TYPESAFE_DEFAULT_MODEL` is set.
	 */
	properties[PROP_MODEL] = g_param_spec_string("model", NULL, NULL,
		AI_TYPESAFE_MODEL_JEV_LATEST, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiTypesafeClient:api-key:
	 *
	 * Bearer token. Initial value is `TYPESAFE_API_KEY`. Never placed in the URL.
	 */
	properties[PROP_API_KEY] = g_param_spec_string("api-key", NULL, NULL,
		NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiTypesafeClient:timeout-ms:
	 *
	 * Whole-operation deadline in milliseconds. There is no retry.
	 */
	properties[PROP_TIMEOUT_MS] = g_param_spec_uint("timeout-ms", NULL, NULL,
		1, G_MAXUINT, 30000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiTypesafeClient:long-context:
	 *
	 * Sends `TypeSafe-Danger-Zone-Long-Context: true`. Off unless set.
	 * Longer context is slower and less accurate; turn it on for a request
	 * that does not fit the default window.
	 */
	properties[PROP_LONG_CONTEXT] = g_param_spec_boolean("long-context", NULL, NULL,
		FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static const gchar *
env_nonempty(const gchar *name)
{
	const gchar *value = g_getenv(name);

	if (value == NULL || *value == '\0')
		return NULL;
	return value;
}

static void
ai_typesafe_client_init(AiTypesafeClient *self)
{
	const gchar *url = env_nonempty("TYPESAFE_BASE_URL");
	const gchar *model = env_nonempty("TYPESAFE_DEFAULT_MODEL");

	self->base_url = g_strdup(url != NULL ? url : "https://api.typesafe.ai");
	self->model = g_strdup(model != NULL ? model : AI_TYPESAFE_MODEL_JEV_LATEST);
	self->api_key = g_strdup(env_nonempty("TYPESAFE_API_KEY"));
	self->timeout_ms = 30000;
	self->long_context = FALSE;
}

AiTypesafeClient *
ai_typesafe_client_new(void)
{
	g_autoptr(AiTypesafeClient) self = g_object_new(AI_TYPE_TYPESAFE_CLIENT, NULL);

	return g_steal_pointer(&self);
}
