/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include "providers/ai-openai-compatible-client.h"
#include "core/ai-image-generator.h"
#include "core/ai-embedder.h"

/* The transport and wire parsers belong to AiOpenAIClient. This class
 * supplies only endpoint, credentials, identity and deployment metadata. */
struct _AiOpenAICompatibleClient
{
	AiOpenAIClient parent_instance;
	gchar *base_url;
	gchar *api_key;
	gchar *image_model;
	gchar *embedding_model;
	AiImageModelInfo *image_info;
};

static void provider_init(AiProviderInterface *iface);
static void image_init(AiImageGeneratorInterface *iface);
static void embedder_init(AiEmbedderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiOpenAICompatibleClient, ai_openai_compatible_client,
	AI_TYPE_OPENAI_CLIENT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, provider_init)
	G_IMPLEMENT_INTERFACE(AI_TYPE_IMAGE_GENERATOR, image_init)
	G_IMPLEMENT_INTERFACE(AI_TYPE_EMBEDDER, embedder_init))

enum
{
	PROP_0,
	PROP_BASE_URL,
	PROP_API_KEY,
	PROP_IMAGE_MODEL,
	PROP_EMBEDDING_MODEL,
	PROP_IMAGE_MODEL_INFO
};

static const gchar *
base_url(AiOpenAICompatibleClient *self)
{
	return self->base_url != NULL ? self->base_url : ai_config_get_base_url(
		ai_client_get_config(AI_CLIENT(self)), AI_PROVIDER_OPENAI_COMPATIBLE);
}

static const gchar *
api_key(AiOpenAICompatibleClient *self)
{
	return self->api_key != NULL ? self->api_key : ai_config_get_api_key(
		ai_client_get_config(AI_CLIENT(self)), AI_PROVIDER_OPENAI_COMPATIBLE);
}

static gchar *
build_api_url(AiOpenAIClient *client, const gchar *path)
{
	AiOpenAICompatibleClient *self = AI_OPENAI_COMPATIBLE_CLIENT(client);
	const gchar *base = base_url(self);
	const gchar *key = api_key(self);
	const gchar *scheme;
	g_autoptr(GUri) uri = NULL;
	g_autofree gchar *trimmed = NULL;
	gsize len;

	/* Reject ambiguous roots and header injection before constructing any
	 * SoupMessage. Never include credentials or the supplied URL in errors. */
	if (base == NULL || base[0] == '\0' ||
	    strpbrk(base, " \t\r\n") != NULL ||
	    (key != NULL && strpbrk(key, "\r\n") != NULL))
		return NULL;
	uri = g_uri_parse(base, G_URI_FLAGS_NONE, NULL);
	if (uri == NULL)
		return NULL;
	scheme = g_uri_get_scheme(uri);
	if ((g_strcmp0(scheme, "http") != 0 && g_strcmp0(scheme, "https") != 0) ||
	    g_uri_get_host(uri) == NULL || g_uri_get_host(uri)[0] == '\0' ||
	    g_uri_get_userinfo(uri) != NULL || g_uri_get_query(uri) != NULL ||
	    g_uri_get_fragment(uri) != NULL)
		return NULL;

	len = strlen(base);
	while (len > 0 && base[len - 1] == '/')
		len--;
	trimmed = g_strndup(base, len);
	/* An origin is shorthand for /v1. A path is the exact API root,
	 * including custom gateway prefixes or alternative API versions. */
	return g_strconcat(trimmed,
		g_uri_get_path(uri)[0] == '\0' || strcmp(g_uri_get_path(uri), "/") == 0
		? path : path + strlen("/v1"), NULL);
}

static void
add_auth_headers(AiClient *client, SoupMessage *msg)
{
	const gchar *key = api_key(AI_OPENAI_COMPATIBLE_CLIENT(client));
	g_autofree gchar *header = NULL;

	if (key != NULL && key[0] != '\0')
	{
		header = g_strconcat("Bearer ", key, NULL);
		soup_message_headers_replace(soup_message_get_request_headers(msg),
		                             "Authorization", header);
	}
}

static JsonNode *
build_request(AiClient *client, GList *messages, const gchar *system_prompt,
              gint max_tokens, GList *tools)
{
	const gchar *model = ai_client_get_model(client);

	if (model == NULL || model[0] == '\0')
		return NULL;
	return AI_CLIENT_CLASS(ai_openai_compatible_client_parent_class)->build_request(
		client, messages, system_prompt, max_tokens, tools);
}

static AiProviderType
get_provider_type(AiProvider *provider)
{
	(void)provider;
	return AI_PROVIDER_OPENAI_COMPATIBLE;
}

static const gchar *
get_name(AiProvider *provider)
{
	(void)provider;
	return "OpenAI-compatible";
}

static const gchar *
get_default_model(AiProvider *provider)
{
	return ai_client_get_model(AI_CLIENT(provider));
}

static void
provider_init(AiProviderInterface *iface)
{
	iface->get_provider_type = get_provider_type;
	iface->get_name = get_name;
	iface->get_default_model = get_default_model;
}

static const gchar *
get_image_model(AiImageGenerator *generator)
{
	return AI_OPENAI_COMPATIBLE_CLIENT(generator)->image_model;
}

static GList *
list_image_models(AiImageGenerator *generator)
{
	AiImageModelInfo *info = AI_OPENAI_COMPATIBLE_CLIENT(generator)->image_info;
	return info != NULL ? g_list_append(NULL, ai_image_model_info_copy(info)) : NULL;
}

static void
image_init(AiImageGeneratorInterface *iface)
{
	iface->get_default_model = get_image_model;
	iface->list_image_models = list_image_models;
}

static const gchar *
get_embedding_model(AiEmbedder *embedder)
{
	return AI_OPENAI_COMPATIBLE_CLIENT(embedder)->embedding_model;
}

static GList *
list_embedding_models(AiEmbedder *embedder)
{
	(void)embedder;
	/* /models does not describe dimensions or embedding capabilities. */
	return NULL;
}

static void
embedder_init(AiEmbedderInterface *iface)
{
	iface->get_default_embedding_model = get_embedding_model;
	iface->list_embedding_models = list_embedding_models;
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiOpenAICompatibleClient *self = AI_OPENAI_COMPATIBLE_CLIENT(object);
	gchar **target;

	switch (id)
	{
	case PROP_BASE_URL: target = &self->base_url; break;
	case PROP_API_KEY: target = &self->api_key; break;
	case PROP_IMAGE_MODEL: target = &self->image_model; break;
	case PROP_EMBEDDING_MODEL: target = &self->embedding_model; break;
	case PROP_IMAGE_MODEL_INFO:
		g_clear_pointer(&self->image_info, ai_image_model_info_free);
		self->image_info = g_value_dup_boxed(value);
		return;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
		return;
	}
	g_free(*target);
	*target = g_value_dup_string(value);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiOpenAICompatibleClient *self = AI_OPENAI_COMPATIBLE_CLIENT(object);

	switch (id)
	{
	case PROP_BASE_URL: g_value_set_string(value, base_url(self)); break;
	case PROP_API_KEY: g_value_set_string(value, api_key(self)); break;
	case PROP_IMAGE_MODEL: g_value_set_string(value, self->image_model); break;
	case PROP_EMBEDDING_MODEL: g_value_set_string(value, self->embedding_model); break;
	case PROP_IMAGE_MODEL_INFO: g_value_set_boxed(value, self->image_info); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}

static void
finalize(GObject *object)
{
	AiOpenAICompatibleClient *self = AI_OPENAI_COMPATIBLE_CLIENT(object);

	g_free(self->base_url);
	g_free(self->api_key);
	g_free(self->image_model);
	g_free(self->embedding_model);
	g_clear_pointer(&self->image_info, ai_image_model_info_free);
	G_OBJECT_CLASS(ai_openai_compatible_client_parent_class)->finalize(object);
}

static void
ai_openai_compatible_client_class_init(AiOpenAICompatibleClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->set_property = set_property;
	object_class->get_property = get_property;
	object_class->finalize = finalize;
	AI_OPENAI_CLIENT_CLASS(klass)->build_api_url = build_api_url;
	AI_CLIENT_CLASS(klass)->add_auth_headers = add_auth_headers;
	AI_CLIENT_CLASS(klass)->build_request = build_request;

	g_object_class_install_property(object_class, PROP_BASE_URL,
		g_param_spec_string("base-url", "Base URL", "API root; NULL uses AiConfig",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_API_KEY,
		g_param_spec_string("api-key", "API key", "Bearer token; empty disables authentication",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_IMAGE_MODEL,
		g_param_spec_string("image-model", "Image model", "Default image model ID",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_EMBEDDING_MODEL,
		g_param_spec_string("embedding-model", "Embedding model", "Default embedding model ID",
		                    NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(object_class, PROP_IMAGE_MODEL_INFO,
		g_param_spec_boxed("image-model-info", "Image model information",
		                   "Optional deployment-specific image capabilities",
		                   AI_TYPE_IMAGE_MODEL_INFO,
		                   G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
ai_openai_compatible_client_init(AiOpenAICompatibleClient *self)
{
	ai_client_set_model(AI_CLIENT(self), NULL);
}

/**
 * ai_openai_compatible_client_new:
 *
 * Creates a client for an OpenAI-compatible API. Set base-url and model
 * before chatting, and optionally api-key. Unset URL and token properties
 * use the OPENAI_COMPATIBLE_BASE_URL and OPENAI_COMPATIBLE_API_KEY environment.
 *
 * Returns: (transfer full): a new client
 */
AiOpenAICompatibleClient *
ai_openai_compatible_client_new(void)
{
	g_autoptr(AiOpenAICompatibleClient) self =
		g_object_new(AI_TYPE_OPENAI_COMPATIBLE_CLIENT, NULL);
	return g_steal_pointer(&self);
}

/**
 * ai_openai_compatible_client_new_with_config:
 * @config: configuration for the client
 *
 * Creates a client using the configuration's OPENAI_COMPATIBLE settings.
 * Per-client base-url and api-key properties override those settings.
 *
 * Returns: (transfer full): a new client
 */
AiOpenAICompatibleClient *
ai_openai_compatible_client_new_with_config(AiConfig *config)
{
	g_autoptr(AiOpenAICompatibleClient) self = NULL;

	g_return_val_if_fail(AI_IS_CONFIG(config), NULL);
	self = g_object_new(AI_TYPE_OPENAI_COMPATIBLE_CLIENT, "config", config, NULL);
	return g_steal_pointer(&self);
}
