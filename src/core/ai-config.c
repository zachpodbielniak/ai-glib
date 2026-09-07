/*
 * ai-config.c - Configuration management for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-config.h"
#include "core/ai-error.h"

#include <yaml-glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <yaml.h>

/* libyaml retains quoted empty/null strings that yaml-glib currently loses. */
static void
config_yaml_free(yaml_document_t *document)
{
	yaml_document_delete(document);
	g_free(document);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(yaml_document_t, config_yaml_free)

static gboolean config_yaml_validate(yaml_document_t *document);

/* Require a single mapping and consume the entire stream before any write. */
static yaml_document_t *
config_yaml_parse(const gchar *data, gsize length, GError **error)
{
	yaml_parser_t parser;
	yaml_document_t extra;
	g_autoptr(yaml_document_t) document = g_new0(yaml_document_t, 1);
	yaml_node_t *root;
	gboolean valid = FALSE;

	if (!yaml_parser_initialize(&parser))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR, "Cannot initialize YAML parser");
		return NULL;
	}
	yaml_parser_set_input_string(&parser, (const unsigned char *)data, length);
	if (!yaml_parser_load(&parser, document))
		goto done;
	root = yaml_document_get_root_node(document);
	if (root == NULL || root->type != YAML_MAPPING_NODE)
		goto done;
	if (!yaml_parser_load(&parser, &extra))
		goto done;
	valid = yaml_document_get_root_node(&extra) == NULL && config_yaml_validate(document);
	yaml_document_delete(&extra);
done:
	if (!valid)
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		            "Config must contain one valid YAML mapping: %s",
		            parser.problem != NULL ? parser.problem : "invalid document structure");
	yaml_parser_delete(&parser);
	return valid ? (yaml_document_t *)g_steal_pointer(&document) : NULL;
}

/* The emitter writes to memory, leaving atomic replacement to GIO. */
static int
config_yaml_output(void *data, unsigned char *buffer, size_t size)
{
	g_string_append_len((GString *)data, (const gchar *)buffer, size);
	return 1;
}

/* The enum parser's Claude fallback must not hide invalid configuration. */
static AiProviderType
config_parse_provider(const gchar *name)
{
	AiProviderType provider;

	if (name == NULL || name[0] == '\0')
		return (AiProviderType)-1;
	provider = ai_provider_type_from_string(name);
	if (provider == AI_PROVIDER_CLAUDE &&
	    g_ascii_strcasecmp(name, "claude") != 0 &&
	    g_ascii_strcasecmp(name, "anthropic") != 0)
		return (AiProviderType)-1;
	return provider;
}

/* Return node ids, which remain valid when libyaml reallocates its node array. */
static gint
config_yaml_member(yaml_document_t *document, gint mapping, const gchar *name)
{
	yaml_node_t *node;
	yaml_node_pair_t *pair;

	if (mapping == 0)
		return 0;
	node = yaml_document_get_node(document, mapping);
	if (node == NULL || node->type != YAML_MAPPING_NODE)
		return 0;
	for (pair = node->data.mapping.pairs.start; pair < node->data.mapping.pairs.top; pair++)
	{
		yaml_node_t *key = yaml_document_get_node(document, pair->key);
		if (key->type == YAML_SCALAR_NODE &&
		    g_str_equal((const gchar *)key->data.scalar.value, name))
			return pair->value;
	}
	return 0;
}

/* Keep quoted null strings literal while empty and YAML null models mean native. */
static const gchar *
config_yaml_model(yaml_node_t *node)
{
	const gchar *value = (const gchar *)node->data.scalar.value;

	if (g_str_equal((const gchar *)node->tag, YAML_NULL_TAG) ||
	    (node->data.scalar.style == YAML_PLAIN_SCALAR_STYLE &&
	     (g_ascii_strcasecmp(value, "null") == 0 || g_str_equal(value, "~"))))
		return "";
	return value;
}

/* Visit each node once; active nodes detect cycles, and the depth bound protects
 * both this walk and the recursive yaml-glib conversion that follows it. */
static gboolean
config_yaml_acyclic(yaml_document_t *document, gint id, guint8 *state, guint depth)
{
	yaml_node_t *node;
	yaml_node_pair_t *pair;
	yaml_node_item_t *item;

	if (depth > 128 || state[id] == 1)
		return FALSE;
	if (state[id] == 2)
		return TRUE;
	state[id] = 1;
	node = yaml_document_get_node(document, id);
	if (node->type == YAML_MAPPING_NODE)
	{
		for (pair = node->data.mapping.pairs.start; pair < node->data.mapping.pairs.top; pair++)
		{
			if (!config_yaml_acyclic(document, pair->key, state, depth + 1) ||
			    !config_yaml_acyclic(document, pair->value, state, depth + 1))
				return FALSE;
		}
	}
	else if (node->type == YAML_SEQUENCE_NODE)
	{
		for (item = node->data.sequence.items.start; item < node->data.sequence.items.top; item++)
		{
			if (!config_yaml_acyclic(document, *item, state, depth + 1))
				return FALSE;
		}
	}
	state[id] = 2;
	return TRUE;
}

/* Validate all scopes before either loading or rewriting any of their values. */
static gboolean
config_yaml_validate(yaml_document_t *document)
{
	const gchar *apps[] = {NULL, "ai", "ai-tui"};
	yaml_node_t *node;
	gint apps_id;
	guint i;
	g_autofree guint8 *state = g_new0(guint8, document->nodes.top - document->nodes.start + 1);

	if (!config_yaml_acyclic(document, 1, state, 0))
		return FALSE;
	for (node = document->nodes.start; node < document->nodes.top; node++)
	{
		yaml_node_pair_t *pair;
		g_autoptr(GHashTable) keys = NULL;

		/* Escaped NULs are legal YAML but cannot round-trip through C strings. */
		if (node->type == YAML_SCALAR_NODE &&
		    memchr(node->data.scalar.value, '\0', node->data.scalar.length) != NULL)
			return FALSE;
		if (node->type != YAML_MAPPING_NODE)
			continue;
		keys = g_hash_table_new(g_str_hash, g_str_equal);
		for (pair = node->data.mapping.pairs.start; pair < node->data.mapping.pairs.top; pair++)
		{
			yaml_node_t *key = yaml_document_get_node(document, pair->key);
			if (key->type != YAML_SCALAR_NODE ||
			    !g_hash_table_add(keys, key->data.scalar.value))
				return FALSE;
		}
	}
	apps_id = config_yaml_member(document, 1, "apps");
	if (apps_id != 0 && yaml_document_get_node(document, apps_id)->type != YAML_MAPPING_NODE)
		return FALSE;
	for (i = 0; i < G_N_ELEMENTS(apps); i++)
	{
		gint app_id = apps[i] == NULL ? 1 : config_yaml_member(document, apps_id, apps[i]);
		gint value_id;

		if (app_id == 0)
			continue;
		if (yaml_document_get_node(document, app_id)->type != YAML_MAPPING_NODE)
			return FALSE;
		value_id = config_yaml_member(document, app_id, "default_provider");
		if (value_id != 0)
		{
			node = yaml_document_get_node(document, value_id);
			if (node->type != YAML_SCALAR_NODE ||
			    config_parse_provider((const gchar *)node->data.scalar.value) == (AiProviderType)-1)
				return FALSE;
		}
		value_id = config_yaml_member(document, app_id, "default_model");
		if (value_id != 0 && yaml_document_get_node(document, value_id)->type != YAML_SCALAR_NODE)
			return FALSE;
	}
	return TRUE;
}

/*
 * Default base URLs for each provider.
 * These are used when no custom URL is configured.
 */
#define CLAUDE_BASE_URL "https://api.anthropic.com"
#define OPENAI_BASE_URL "https://api.openai.com"
#define GEMINI_BASE_URL "https://generativelanguage.googleapis.com"
#define GROK_BASE_URL   "https://api.x.ai"
#define OLLAMA_BASE_URL "http://localhost:11434"

/*
 * Environment variable names for API keys and configuration.
 * Primary env vars are checked first, then alternatives.
 */
#define ANTHROPIC_API_KEY_ENV "ANTHROPIC_API_KEY"
#define CLAUDE_API_KEY_ENV    "CLAUDE_API_KEY"       /* Alternative for Claude */
#define OPENAI_API_KEY_ENV    "OPENAI_API_KEY"
#define OPENAI_BASE_URL_ENV   "OPENAI_BASE_URL"
#define CLAUDE_BASE_URL_ENV   "ANTHROPIC_BASE_URL"
#define GEMINI_BASE_URL_ENV   "GEMINI_BASE_URL"
#define GROK_BASE_URL_ENV     "XAI_BASE_URL"
#define GEMINI_API_KEY_ENV    "GEMINI_API_KEY"
#define XAI_API_KEY_ENV       "XAI_API_KEY"
#define GROK_API_KEY_ENV      "GROK_API_KEY"         /* Alternative for Grok */
#define OLLAMA_API_KEY_ENV    "OLLAMA_API_KEY"       /* Optional Ollama auth */
#define OLLAMA_HOST_ENV       "OLLAMA_HOST"

/* Environment variables for default provider/model selection.
 * These override config file values but are overridden by programmatic set_*(). */
#define AI_GLIB_DEFAULT_PROVIDER_ENV "AI_GLIB_DEFAULT_PROVIDER"
#define AI_GLIB_DEFAULT_MODEL_ENV    "AI_GLIB_DEFAULT_MODEL"

/*
 * Private data structure for AiConfig.
 * Stores API keys, base URLs, and other configuration options.
 */
struct _AiConfig
{
    GObject parent_instance;

    /* API keys for each provider (overrides env vars) */
    gchar *claude_api_key;
    gchar *compatible_api_key;
    gchar *openai_api_key;
    gchar *gemini_api_key;
    gchar *grok_api_key;
    gchar *ollama_api_key;   /* Optional - Ollama may require auth in some setups */

    /*
     * Custom base URLs (override the defaults).
     *
     * Every provider gets one, not just the two whose vendors advertise
     * compatible gateways: pointing a client at a local address is how a
     * caller inspects what ai-glib actually sends, routes through a
     * corporate proxy, or stands a provider up against a mock in tests.
     * Leaving them hardcoded made those providers untestable offline.
     */
    gchar *claude_base_url;
    gchar *compatible_base_url;
    gchar *openai_base_url;
    gchar *gemini_base_url;
    gchar *grok_base_url;
    gchar *ollama_base_url;

    /* Request settings */
    guint timeout_seconds;
    guint max_retries;

    /* Default provider and model from config file */
    AiProviderType default_provider;
    gboolean       default_provider_set;       /* TRUE if set from file */
    gboolean       default_provider_programmatic; /* TRUE if set via set_default_provider() */
    gchar         *default_model;
    gboolean       default_model_programmatic;  /* TRUE if set via set_default_model() */
	AiProviderType app_providers[2];
	gchar *app_models[2];
};

G_DEFINE_TYPE(AiConfig, ai_config, G_TYPE_OBJECT)

/*
 * Property IDs for GObject properties.
 */
enum
{
    PROP_0,
    PROP_TIMEOUT,
    PROP_MAX_RETRIES,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/* Singleton instance for get_default() */
static AiConfig *default_config = NULL;

/*
 * ai_config_finalize:
 *
 * Releases all memory owned by the AiConfig instance.
 */
static void
ai_config_finalize(GObject *object)
{
    AiConfig *self = AI_CONFIG(object);

    g_clear_pointer(&self->claude_api_key, g_free);
    g_clear_pointer(&self->compatible_api_key, g_free);
    g_clear_pointer(&self->compatible_base_url, g_free);
    g_clear_pointer(&self->openai_api_key, g_free);
    g_clear_pointer(&self->gemini_api_key, g_free);
    g_clear_pointer(&self->grok_api_key, g_free);
    g_clear_pointer(&self->ollama_api_key, g_free);
    g_clear_pointer(&self->claude_base_url, g_free);
    g_clear_pointer(&self->openai_base_url, g_free);
    g_clear_pointer(&self->gemini_base_url, g_free);
    g_clear_pointer(&self->grok_base_url, g_free);
    g_clear_pointer(&self->ollama_base_url, g_free);
    g_clear_pointer(&self->default_model, g_free);
	g_clear_pointer(&self->app_models[0], g_free);
	g_clear_pointer(&self->app_models[1], g_free);

    G_OBJECT_CLASS(ai_config_parent_class)->finalize(object);
}

static void
ai_config_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiConfig *self = AI_CONFIG(object);

    switch (prop_id)
    {
        case PROP_TIMEOUT:
            g_value_set_uint(value, self->timeout_seconds);
            break;
        case PROP_MAX_RETRIES:
            g_value_set_uint(value, self->max_retries);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_config_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiConfig *self = AI_CONFIG(object);

    switch (prop_id)
    {
        case PROP_TIMEOUT:
            self->timeout_seconds = g_value_get_uint(value);
            break;
        case PROP_MAX_RETRIES:
            self->max_retries = g_value_get_uint(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_config_class_init(AiConfigClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_config_finalize;
    object_class->get_property = ai_config_get_property;
    object_class->set_property = ai_config_set_property;

    /**
     * AiConfig:timeout:
     *
     * The timeout in seconds for API requests.
     */
    properties[PROP_TIMEOUT] =
        g_param_spec_uint("timeout",
                          "Timeout",
                          "Timeout in seconds for API requests",
                          0, G_MAXUINT, AI_CONFIG_DEFAULT_TIMEOUT,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiConfig:max-retries:
     *
     * The maximum number of retry attempts for failed requests.
     */
    properties[PROP_MAX_RETRIES] =
        g_param_spec_uint("max-retries",
                          "Max Retries",
                          "Maximum number of retry attempts",
                          0, G_MAXUINT, AI_CONFIG_DEFAULT_MAX_RETRIES,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_config_init(AiConfig *self)
{
    self->timeout_seconds = AI_CONFIG_DEFAULT_TIMEOUT;
    self->max_retries = AI_CONFIG_DEFAULT_MAX_RETRIES;
	self->app_providers[0] = AI_PROVIDER_CLAUDE;
	self->app_providers[1] = AI_PROVIDER_CLAUDE;
}

/* Forward declaration for use in ai_config_new */
static void ai_config_load_files(AiConfig *self);

/**
 * ai_config_new:
 *
 * Creates a new #AiConfig instance with default settings.
 * Configuration files are loaded from the standard fallback chain:
 * /usr/share/ai-glib/config.yaml, /etc/ai-glib/config.yaml,
 * ~/.config/ai-glib/config.yaml (each overrides the previous).
 * Environment variables override file values at access time.
 *
 * Returns: (transfer full): a new #AiConfig
 */
AiConfig *
ai_config_new(void)
{
    g_autoptr(AiConfig) self = g_object_new(AI_TYPE_CONFIG, NULL);

    /* Load config files (lowest to highest priority) */
    ai_config_load_files(self);

    return (AiConfig *)g_steal_pointer(&self);
}

/**
 * ai_config_get_default:
 *
 * Gets the default shared #AiConfig instance.
 * This is a singleton that persists for the lifetime of the application.
 * The returned reference should not be freed.
 *
 * Returns: (transfer none): the default #AiConfig
 */
AiConfig *
ai_config_get_default(void)
{
    if (g_once_init_enter(&default_config))
    {
        AiConfig *config = ai_config_new();
        g_once_init_leave(&default_config, config);
    }

    return default_config;
}

/**
 * ai_config_get_api_key:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to get the key for
 *
 * Gets the API key for the specified provider.
 * First checks for an explicitly set key, then falls back to environment
 * variables. Environment variables checked (in order of precedence):
 * - Claude: ANTHROPIC_API_KEY, CLAUDE_API_KEY
 * - OpenAI: OPENAI_API_KEY
 * - Gemini: GEMINI_API_KEY
 * - Grok: XAI_API_KEY, GROK_API_KEY
 * - Ollama: OLLAMA_API_KEY (optional)
 *
 * Returns: (transfer none) (nullable): the API key, or %NULL if not set
 */
const gchar *
ai_config_get_api_key(
    AiConfig       *self,
    AiProviderType  provider
){
    const gchar *key = NULL;
    const gchar *env_key = NULL;

    g_return_val_if_fail(AI_IS_CONFIG(self), NULL);

    /* Check for explicitly set key first, then env vars with fallbacks */
    switch (provider)
    {
        case AI_PROVIDER_CLAUDE:
            key = self->claude_api_key;
            if (key != NULL && key[0] != '\0')
            {
                return key;
            }
            /* Check primary env var, then alternative */
            env_key = g_getenv(ANTHROPIC_API_KEY_ENV);
            if (env_key != NULL && env_key[0] != '\0')
            {
                return env_key;
            }
            return g_getenv(CLAUDE_API_KEY_ENV);

        case AI_PROVIDER_OPENAI_COMPATIBLE:
            return self->compatible_api_key != NULL ? self->compatible_api_key :
                   g_getenv("OPENAI_COMPATIBLE_API_KEY");
        case AI_PROVIDER_OPENAI:
            key = self->openai_api_key;
            if (key != NULL && key[0] != '\0')
            {
                return key;
            }
            return g_getenv(OPENAI_API_KEY_ENV);

        case AI_PROVIDER_GEMINI:
            key = self->gemini_api_key;
            if (key != NULL && key[0] != '\0')
            {
                return key;
            }
            return g_getenv(GEMINI_API_KEY_ENV);

        case AI_PROVIDER_GROK:
            key = self->grok_api_key;
            if (key != NULL && key[0] != '\0')
            {
                return key;
            }
            /* Check primary env var, then alternative */
            env_key = g_getenv(XAI_API_KEY_ENV);
            if (env_key != NULL && env_key[0] != '\0')
            {
                return env_key;
            }
            return g_getenv(GROK_API_KEY_ENV);

        case AI_PROVIDER_OLLAMA:
            /* Ollama API key is optional but supported */
            key = self->ollama_api_key;
            if (key != NULL && key[0] != '\0')
            {
                return key;
            }
            return g_getenv(OLLAMA_API_KEY_ENV);

        default:
            return NULL;
    }
}

/**
 * ai_config_set_api_key:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to set the key for
 * @api_key: (nullable): the API key to set, or %NULL to clear
 *
 * Sets the API key for the specified provider.
 * This overrides any environment variable setting.
 */
void
ai_config_set_api_key(
    AiConfig       *self,
    AiProviderType  provider,
    const gchar    *api_key
){
    gchar **target = NULL;

    g_return_if_fail(AI_IS_CONFIG(self));

    switch (provider)
    {
        case AI_PROVIDER_CLAUDE:
            target = &self->claude_api_key;
            break;
        case AI_PROVIDER_OPENAI_COMPATIBLE:
            target = &self->compatible_api_key;
            break;
        case AI_PROVIDER_OPENAI:
            target = &self->openai_api_key;
            break;
        case AI_PROVIDER_GEMINI:
            target = &self->gemini_api_key;
            break;
        case AI_PROVIDER_GROK:
            target = &self->grok_api_key;
            break;
        case AI_PROVIDER_OLLAMA:
            target = &self->ollama_api_key;
            break;
        default:
            return;
    }

    g_clear_pointer(target, g_free);
    *target = g_strdup(api_key);
}

/**
 * ai_config_get_base_url:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to get the URL for
 *
 * Gets the base URL for the specified provider.
 * Checks for explicit settings, then environment variables, then returns
 * the default URL.
 *
 * Returns: (transfer none) (nullable): the base URL; NULL if the
 *   OpenAI-compatible provider has no configured URL
 */
const gchar *
ai_config_get_base_url(
    AiConfig       *self,
    AiProviderType  provider
){
    const gchar *url = NULL;

    g_return_val_if_fail(AI_IS_CONFIG(self), NULL);

    switch (provider)
    {
        case AI_PROVIDER_CLAUDE:
            /* Explicit setting, then environment, then the default. */
            if (self->claude_base_url != NULL && self->claude_base_url[0] != '\0')
            {
                return self->claude_base_url;
            }
            url = g_getenv(CLAUDE_BASE_URL_ENV);
            if (url != NULL && url[0] != '\0')
            {
                return url;
            }
            return CLAUDE_BASE_URL;

        case AI_PROVIDER_OPENAI_COMPATIBLE:
            return self->compatible_base_url != NULL ? self->compatible_base_url :
                   g_getenv("OPENAI_COMPATIBLE_BASE_URL");
        case AI_PROVIDER_OPENAI:
            /* Check explicit setting first */
            if (self->openai_base_url != NULL && self->openai_base_url[0] != '\0')
            {
                return self->openai_base_url;
            }
            /* Check environment variable */
            url = g_getenv(OPENAI_BASE_URL_ENV);
            if (url != NULL && url[0] != '\0')
            {
                return url;
            }
            return OPENAI_BASE_URL;

        case AI_PROVIDER_GEMINI:
            if (self->gemini_base_url != NULL && self->gemini_base_url[0] != '\0')
            {
                return self->gemini_base_url;
            }
            url = g_getenv(GEMINI_BASE_URL_ENV);
            if (url != NULL && url[0] != '\0')
            {
                return url;
            }
            return GEMINI_BASE_URL;

        case AI_PROVIDER_GROK:
            if (self->grok_base_url != NULL && self->grok_base_url[0] != '\0')
            {
                return self->grok_base_url;
            }
            url = g_getenv(GROK_BASE_URL_ENV);
            if (url != NULL && url[0] != '\0')
            {
                return url;
            }
            return GROK_BASE_URL;

        case AI_PROVIDER_OLLAMA:
            /* Check explicit setting first */
            if (self->ollama_base_url != NULL && self->ollama_base_url[0] != '\0')
            {
                return self->ollama_base_url;
            }
            /* Check environment variable */
            url = g_getenv(OLLAMA_HOST_ENV);
            if (url != NULL && url[0] != '\0')
            {
                return url;
            }
            return OLLAMA_BASE_URL;

        default:
            return NULL;
    }
}

/**
 * ai_config_set_base_url:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to set the URL for
 * @base_url: (nullable): the base URL to set, or %NULL to use default
 *
 * Sets the base URL for the specified provider, overriding both the
 * environment variable and the built-in default.
 *
 * Supported for every HTTP provider.  Pointing a client at a local
 * address is how a caller inspects the requests ai-glib actually sends,
 * routes through a proxy or a compatible gateway, or stands a provider up
 * against a mock server in tests.
 *
 * The CLI-backed providers spawn a subprocess rather than making HTTP
 * requests, so this does nothing for them.
 */
void
ai_config_set_base_url(
    AiConfig       *self,
    AiProviderType  provider,
    const gchar    *base_url
){
    g_return_if_fail(AI_IS_CONFIG(self));

    switch (provider)
    {
        case AI_PROVIDER_CLAUDE:
            g_clear_pointer(&self->claude_base_url, g_free);
            self->claude_base_url = g_strdup(base_url);
            break;

        case AI_PROVIDER_OPENAI_COMPATIBLE:
            g_free(self->compatible_base_url);
            self->compatible_base_url = g_strdup(base_url);
            break;
        case AI_PROVIDER_OPENAI:
            g_clear_pointer(&self->openai_base_url, g_free);
            self->openai_base_url = g_strdup(base_url);
            break;

        case AI_PROVIDER_GEMINI:
            g_clear_pointer(&self->gemini_base_url, g_free);
            self->gemini_base_url = g_strdup(base_url);
            break;

        case AI_PROVIDER_GROK:
            g_clear_pointer(&self->grok_base_url, g_free);
            self->grok_base_url = g_strdup(base_url);
            break;

        case AI_PROVIDER_OLLAMA:
            g_clear_pointer(&self->ollama_base_url, g_free);
            self->ollama_base_url = g_strdup(base_url);
            break;

        default:
            /* CLI providers spawn a subprocess and have no base URL. */
            break;
    }
}

/**
 * ai_config_get_timeout:
 * @self: an #AiConfig
 *
 * Gets the timeout in seconds for API requests.
 *
 * Returns: the timeout in seconds
 */
guint
ai_config_get_timeout(AiConfig *self)
{
    g_return_val_if_fail(AI_IS_CONFIG(self), AI_CONFIG_DEFAULT_TIMEOUT);

    return self->timeout_seconds;
}

/**
 * ai_config_set_timeout:
 * @self: an #AiConfig
 * @timeout_seconds: the timeout in seconds
 *
 * Sets the timeout for API requests.
 */
void
ai_config_set_timeout(
    AiConfig *self,
    guint     timeout_seconds
){
    g_return_if_fail(AI_IS_CONFIG(self));

    self->timeout_seconds = timeout_seconds;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TIMEOUT]);
}

/**
 * ai_config_get_max_retries:
 * @self: an #AiConfig
 *
 * Gets the maximum number of retry attempts for failed requests.
 *
 * Returns: the maximum retry count
 */
guint
ai_config_get_max_retries(AiConfig *self)
{
    g_return_val_if_fail(AI_IS_CONFIG(self), AI_CONFIG_DEFAULT_MAX_RETRIES);

    return self->max_retries;
}

/**
 * ai_config_set_max_retries:
 * @self: an #AiConfig
 * @max_retries: the maximum retry count
 *
 * Sets the maximum number of retry attempts for failed requests.
 */
void
ai_config_set_max_retries(
    AiConfig *self,
    guint     max_retries
){
    g_return_if_fail(AI_IS_CONFIG(self));

    self->max_retries = max_retries;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAX_RETRIES]);
}

/**
 * ai_config_validate:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to validate for
 * @error: (out) (optional): return location for a #GError
 *
 * Validates that the configuration is complete for the specified provider.
 * Checks that required settings (like API keys) are present.
 *
 * Note: Ollama does not require an API key, so validation always passes
 * for that provider. However, if OLLAMA_API_KEY is set, it will be used
 * for authentication with Ollama instances that require it.
 *
 * Returns: %TRUE if the configuration is valid, %FALSE otherwise
 */
gboolean
ai_config_validate(
    AiConfig        *self,
    AiProviderType   provider,
    GError         **error
){
    const gchar *api_key;
    const gchar *provider_name;

    g_return_val_if_fail(AI_IS_CONFIG(self), FALSE);

    /* Local and compatible servers may not require authentication. */
    if (provider == AI_PROVIDER_OLLAMA || provider == AI_PROVIDER_OPENAI_COMPATIBLE)
    {
        return TRUE;
    }

    api_key = ai_config_get_api_key(self, provider);
    provider_name = ai_provider_type_to_string(provider);

    if (api_key == NULL || api_key[0] == '\0')
    {
        g_set_error(error,
                    AI_ERROR,
                    AI_ERROR_INVALID_API_KEY,
                    "No API key configured for provider '%s'",
                    provider_name);
        return FALSE;
    }

    return TRUE;
}

/*
 * ai_config_apply_provider_mapping:
 * @self: an #AiConfig
 * @provider: the provider type to apply settings for
 * @provider_map: the YAML mapping for this provider's settings
 *
 * Extracts api_key and base_url from a provider's YAML mapping
 * and applies them to the config. Only sets values that are present
 * in the mapping — missing keys are silently skipped.
 */
static void
ai_config_apply_provider_mapping(
    AiConfig       *self,
    AiProviderType  provider,
    YamlMapping    *provider_map
){
    const gchar *val;

    /* Apply api_key if present */
    if (yaml_mapping_has_member(provider_map, "api_key"))
    {
        val = yaml_mapping_get_string_member(provider_map, "api_key");
        if (val != NULL && val[0] != '\0')
        {
            ai_config_set_api_key(self, provider, val);
        }
    }

    /* Apply base_url if present */
    if (yaml_mapping_has_member(provider_map, "base_url"))
    {
        val = yaml_mapping_get_string_member(provider_map, "base_url");
        if (val != NULL && val[0] != '\0')
        {
            ai_config_set_base_url(self, provider, val);
        }
    }
}

/*
 * Provider name to AiProviderType mapping table.
 * Used when parsing the "providers" section of config files.
 */
static const struct {
    const gchar    *name;
    AiProviderType  type;
} provider_name_map[] = {
    { "claude",      AI_PROVIDER_CLAUDE },
    { "openai",      AI_PROVIDER_OPENAI },
    { "openai-compatible", AI_PROVIDER_OPENAI_COMPATIBLE },
    { "gemini",      AI_PROVIDER_GEMINI },
    { "grok",        AI_PROVIDER_GROK },
    { "ollama",      AI_PROVIDER_OLLAMA },
    { "claude_code", AI_PROVIDER_CLAUDE_CODE },
    { "opencode",    AI_PROVIDER_OPENCODE },
    { "grok_build",  AI_PROVIDER_GROK_BUILD },
    { "antigravity", AI_PROVIDER_ANTIGRAVITY },
    { "agy",         AI_PROVIDER_ANTIGRAVITY },
    { "codex_cli",   AI_PROVIDER_CODEX_CLI },
    { "codex-cli",   AI_PROVIDER_CODEX_CLI },
    { "codex",       AI_PROVIDER_CODEX_CLI },
    { "cursor",      AI_PROVIDER_CURSOR },
    { NULL,          0 }
};

/**
 * ai_config_load_from_file:
 * @self: an #AiConfig
 * @path: path to a YAML config file
 * @error: (out) (optional): return location for a #GError
 *
 * Loads configuration from a YAML file. Values from the file are
 * applied to the config, overriding any previously loaded file values.
 * Programmatic set calls and environment variables still take priority
 * (env vars are checked at access time in the getter functions).
 * Empty or YAML null models clear the scoped file default; quoted `null`
 * remains a literal model name. Scalars containing embedded NULs, cyclic
 * aliases and nesting deeper than 128 edges are rejected.
 *
 * Returns: %TRUE on success, %FALSE on parse error
 */
gboolean
ai_config_load_from_file(
    AiConfig     *self,
    const gchar  *path,
    GError      **error
){
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(yaml_document_t) document = NULL;
    g_autofree gchar *contents = NULL;
    gsize length;
    yaml_node_pair_t *pair;
    const gchar *provider_value = NULL;
    const gchar *model_value = NULL;
    gboolean has_provider = FALSE;
    gboolean has_model = FALSE;
    YamlNode              *root;
    YamlMapping           *root_map;
    const gchar           *str_val;
    guint                  i;

    g_return_val_if_fail(AI_IS_CONFIG(self), FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    /* Check if file exists before attempting to parse */
    if (!g_file_test(path, G_FILE_TEST_EXISTS))
    {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_NOENT,
                    "Config file not found: %s", path);
        return FALSE;
    }

    parser = yaml_parser_new();

    if (!g_file_get_contents(path, &contents, &length, error))
        return FALSE;
    document = config_yaml_parse(contents, length, error);
    if (document == NULL)
        return FALSE;
    /* Read defaults losslessly, including an explicitly quoted empty model. */
    for (pair = yaml_document_get_root_node(document)->data.mapping.pairs.start;
         pair < yaml_document_get_root_node(document)->data.mapping.pairs.top; pair++)
    {
        yaml_node_t *key = yaml_document_get_node(document, pair->key);
        yaml_node_t *value = yaml_document_get_node(document, pair->value);

        if (key->type != YAML_SCALAR_NODE)
            continue;
        if (g_str_equal((const gchar *)key->data.scalar.value, "default_provider"))
        {
            has_provider = TRUE;
            provider_value = value->type == YAML_SCALAR_NODE ? (const gchar *)value->data.scalar.value : NULL;
        }
        if (g_str_equal((const gchar *)key->data.scalar.value, "default_model"))
        {
            has_model = TRUE;
            model_value = config_yaml_model(value);
        }
    }
    if (!yaml_parser_load_from_data(parser, contents, length, error))
    {
        return FALSE;
    }

    root = yaml_parser_get_root(parser);
    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
    {
        g_set_error(error,
                    G_FILE_ERROR,
                    G_FILE_ERROR_FAILED,
                    "Config file root must be a YAML mapping: %s", path);
        return FALSE;
    }

    root_map = yaml_node_get_mapping(root);

	/* App overlays are independent of both library fields and environment values. */
	for (i = 0; i < G_N_ELEMENTS(self->app_providers); i++)
	{
		gint apps_id = config_yaml_member(document, 1, "apps");
		gint app_id = config_yaml_member(document, apps_id, i == 0 ? "ai" : "ai-tui");
		gint value_id = config_yaml_member(document, app_id, "default_provider");
		yaml_node_t *value;

		if (value_id != 0)
		{
			value = yaml_document_get_node(document, value_id);
			self->app_providers[i] = config_parse_provider((const gchar *)value->data.scalar.value);
		}
		value_id = config_yaml_member(document, app_id, "default_model");
		if (value_id != 0)
		{
			value = yaml_document_get_node(document, value_id);
			str_val = config_yaml_model(value);
			g_clear_pointer(&self->app_models[i], g_free);
			if (str_val[0] != '\0')
				self->app_models[i] = g_strdup(str_val);
		}
	}

    /* default_provider */
    if (has_provider)
    {
        self->default_provider = config_parse_provider(provider_value);
        self->default_provider_set = TRUE;
    }

    /* default_model */
    if (has_model)
    {
        str_val = model_value;
        if (str_val != NULL)
        {
            g_clear_pointer(&self->default_model, g_free);
            self->default_model = str_val[0] != '\0' ? g_strdup(str_val) : NULL;
        }
    }

    /* timeout */
    if (yaml_mapping_has_member(root_map, "timeout"))
    {
        self->timeout_seconds = (guint)yaml_mapping_get_int_member(
            root_map, "timeout");
    }

    /* max_retries */
    if (yaml_mapping_has_member(root_map, "max_retries"))
    {
        self->max_retries = (guint)yaml_mapping_get_int_member(
            root_map, "max_retries");
    }

    /* providers section — per-provider api_key and base_url */
    if (yaml_mapping_has_member(root_map, "providers"))
    {
        YamlNode    *providers_node;
        YamlMapping *providers_map;

        providers_node = yaml_mapping_get_member(root_map, "providers");
        if (providers_node != NULL &&
            yaml_node_get_node_type(providers_node) == YAML_NODE_MAPPING)
        {
            providers_map = yaml_node_get_mapping(providers_node);

            for (i = 0; provider_name_map[i].name != NULL; i++)
            {
                if (yaml_mapping_has_member(providers_map,
                                            provider_name_map[i].name))
                {
                    YamlMapping *pmap;

                    pmap = yaml_mapping_get_mapping_member(
                        providers_map, provider_name_map[i].name);
                    if (pmap != NULL)
                    {
                        ai_config_apply_provider_mapping(
                            self, provider_name_map[i].type, pmap);
                    }
                }
            }
        }
    }

    return TRUE;
}

/*
 * ai_config_load_files:
 * @self: an #AiConfig
 *
 * Loads config files from the standard fallback chain.
 * Files are loaded in order from lowest to highest priority:
 * 1. /usr/share/ai-glib/config.yaml (distro/image defaults)
 * 2. /etc/ai-glib/config.yaml (system admin overrides)
 * 3. ~/.config/ai-glib/config.yaml (user preferences)
 *
 * Each file overlays the previous — later files override earlier ones.
 * Missing files are silently skipped (not an error).
 */
static void
ai_config_load_files(AiConfig *self)
{
    const gchar *paths[3];
    g_autofree gchar *user_path = NULL;
    guint i;

    user_path = g_build_filename(
        g_get_user_config_dir(), "ai-glib", AI_CONFIG_FILENAME, NULL);

    paths[0] = AI_CONFIG_SYSTEM_DIR "/" AI_CONFIG_FILENAME;
    paths[1] = AI_CONFIG_ADMIN_DIR "/" AI_CONFIG_FILENAME;
    paths[2] = user_path;

    for (i = 0; i < G_N_ELEMENTS(paths); i++)
    {
        if (g_file_test(paths[i], G_FILE_TEST_EXISTS))
        {
			g_autoptr(GError) error = NULL;

			if (!ai_config_load_from_file(self, paths[i], &error))
				g_debug("Ignoring config file %s: %s", paths[i], error->message);
        }
    }
}

/**
 * ai_config_get_default_provider:
 * @self: an #AiConfig
 *
 * Gets the default provider type. Priority order:
 * 1. Programmatic — value set via ai_config_set_default_provider()
 * 2. Environment — `AI_GLIB_DEFAULT_PROVIDER` env var
 * 3. Config file — `default_provider` key from YAML
 * 4. Built-in default — %AI_PROVIDER_CLAUDE
 *
 * Returns: the default #AiProviderType, or -1 if the effective environment
 *   override is not a recognized provider name
 */
AiProviderType
ai_config_get_default_provider(AiConfig *self)
{
    const gchar *env_val;

    g_return_val_if_fail(AI_IS_CONFIG(self), AI_PROVIDER_CLAUDE);

    /* 1. Programmatic override takes highest priority */
    if (self->default_provider_programmatic)
    {
        return self->default_provider;
    }

    /* 2. Environment variable overrides config file */
    env_val = g_getenv(AI_GLIB_DEFAULT_PROVIDER_ENV);
    if (env_val != NULL && env_val[0] != '\0')
    {
        return config_parse_provider(env_val);
    }

    /* 3. Config file value */
    if (self->default_provider_set)
    {
        return self->default_provider;
    }

    /* 4. Built-in default */
    return AI_PROVIDER_CLAUDE;
}

/**
 * ai_config_set_default_provider:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to use as default
 *
 * Sets the default provider type programmatically. This takes the
 * highest priority, overriding both environment variables and
 * config file values.
 */
void
ai_config_set_default_provider(
    AiConfig       *self,
    AiProviderType  provider
){
    g_return_if_fail(AI_IS_CONFIG(self));

    self->default_provider = provider;
    self->default_provider_set = TRUE;
    self->default_provider_programmatic = TRUE;
}

/**
 * ai_config_get_default_model:
 * @self: an #AiConfig
 *
 * Gets the default model name. Priority order:
 * 1. Programmatic — value set via ai_config_set_default_model()
 * 2. Environment — `AI_GLIB_DEFAULT_MODEL` env var
 * 3. Config file — `default_model` key from YAML
 * 4. Built-in default — %NULL
 *
 * Returns: (transfer none) (nullable): the default model name
 */
const gchar *
ai_config_get_default_model(AiConfig *self)
{
    const gchar *env_val;

    g_return_val_if_fail(AI_IS_CONFIG(self), NULL);

    /* 1. Programmatic override takes highest priority */
    if (self->default_model_programmatic)
    {
        return self->default_model;
    }

    /* 2. Environment variable overrides config file */
    env_val = g_getenv(AI_GLIB_DEFAULT_MODEL_ENV);
    if (env_val != NULL && env_val[0] != '\0')
    {
        return env_val;
    }

    /* 3. Config file value (may be NULL) */
    return self->default_model;
}

/**
 * ai_config_set_default_model:
 * @self: an #AiConfig
 * @model: (nullable): the model name to use as default
 *
 * Sets the default model name programmatically. This takes the
 * highest priority, overriding both environment variables and
 * config file values.
 */
void
ai_config_set_default_model(
    AiConfig    *self,
    const gchar *model
){
    g_return_if_fail(AI_IS_CONFIG(self));

    g_clear_pointer(&self->default_model, g_free);
    self->default_model = g_strdup(model);
    self->default_model_programmatic = TRUE;
}

/**
 * ai_config_get_app_provider:
 * @self: an #AiConfig
 * @app: application name, `ai` or `ai-tui`
 *
 * Reads only apps.@app.default_provider, never library or environment defaults.
 *
 * Returns: the app provider, or %AI_PROVIDER_CLAUDE when absent
 */
AiProviderType
ai_config_get_app_provider(AiConfig *self, const gchar *app)
{
	g_return_val_if_fail(AI_IS_CONFIG(self), AI_PROVIDER_CLAUDE);
	g_return_val_if_fail(g_strcmp0(app, "ai") == 0 || g_strcmp0(app, "ai-tui") == 0, AI_PROVIDER_CLAUDE);
	return self->app_providers[g_str_equal(app, "ai") ? 0 : 1];
}

/**
 * ai_config_get_app_model:
 * @self: an #AiConfig
 * @app: application name, `ai` or `ai-tui`
 *
 * Reads only apps.@app.default_model, never library or environment defaults.
 *
 * Returns: (transfer none) (nullable): the saved app model, or %NULL for native defaults
 */
const gchar *
ai_config_get_app_model(AiConfig *self, const gchar *app)
{
	g_return_val_if_fail(AI_IS_CONFIG(self), NULL);
	g_return_val_if_fail(g_strcmp0(app, "ai") == 0 || g_strcmp0(app, "ai-tui") == 0, NULL);
	return self->app_models[g_str_equal(app, "ai") ? 0 : 1];
}

/* Copy each mapping on the edited path so YAML aliases outside it stay unchanged. */
static gint
config_yaml_copy_mapping(yaml_document_t *document, gint parent, const gchar *name)
{
	gint old_id = config_yaml_member(document, parent, name);
	gint new_id = yaml_document_add_mapping(document, (const yaml_char_t *)YAML_MAP_TAG,
	                                        YAML_BLOCK_MAPPING_STYLE);
	yaml_node_t *node;
	yaml_node_pair_t *pair;

	if (old_id != 0)
	{
		node = yaml_document_get_node(document, old_id);
		for (pair = node->data.mapping.pairs.start; pair < node->data.mapping.pairs.top; pair++)
			yaml_document_append_mapping_pair(document, new_id, pair->key, pair->value);
	}
	node = yaml_document_get_node(document, parent);
	for (pair = node->data.mapping.pairs.start; pair < node->data.mapping.pairs.top; pair++)
	{
		yaml_node_t *key = yaml_document_get_node(document, pair->key);
		if (g_str_equal((const gchar *)key->data.scalar.value, name))
		{
			pair->value = new_id;
			return new_id;
		}
	}
	old_id = yaml_document_add_scalar(document, (const yaml_char_t *)YAML_STR_TAG,
	                                  (const yaml_char_t *)name, -1, YAML_PLAIN_SCALAR_STYLE);
	yaml_document_append_mapping_pair(document, parent, old_id, new_id);
	return new_id;
}

/**
 * ai_config_save_defaults:
 * @self: an #AiConfig
 * @app: (nullable): `ai` or `ai-tui`, or %NULL for library defaults
 * @provider: the provider to save
 * @model: (nullable): model name, or %NULL for the provider's native default
 * @error: (out) (optional): return location for a #GError
 *
 * Updates only default_provider and default_model in
 * `$XDG_CONFIG_HOME/ai-glib/config.yaml` (using GLib's user config directory).
 * With @app, only the apps.@app mapping is updated; %NULL updates the existing
 * top-level library keys. These scopes never inherit from one another.
 * Unrelated YAML data is retained, though formatting may change. Existing
 * malformed, multi-document or non-mapping YAML is rejected without writing.
 * A missing parent directory is created with mode 0700; existing directory
 * permissions are unchanged. The file is atomically replaced with mode 0600.
 * A %NULL model is stored as an empty string to mask lower-priority file models.
 * Invalid UTF-8 model names are rejected with a configuration error.
 * On success, the scoped defaults are updated on @self. Library saves set
 * programmatic overrides; app saves leave library values untouched.
 * On failure, @self is unchanged.
 *
 * Returns: %TRUE on success, %FALSE on validation or I/O failure
 */
gboolean
ai_config_save_defaults(
	AiConfig       *self,
	const gchar    *app,
	AiProviderType  provider,
	const gchar    *model,
	GError        **error
){
	g_autoptr(yaml_document_t) document = NULL;
	g_autoptr(GString) output = g_string_new(NULL);
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) read_error = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *saved_model = g_strdup(model != NULL ? model : "");
	yaml_emitter_t emitter;
	yaml_node_pair_t *pair;
	yaml_node_t *root;
	const gchar *keys[] = {"default_provider", "default_model"};
	const gchar *values[2];
	guint i;
	gboolean emitted;
	const gchar *name;
	gsize length;
	gint mapping_id = 1;

	g_return_val_if_fail(AI_IS_CONFIG(self), FALSE);
	if (model != NULL && !g_utf8_validate(model, -1, NULL))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		                    "Model must be valid UTF-8");
		return FALSE;
	}
	if (app != NULL && !g_str_equal(app, "ai") && !g_str_equal(app, "ai-tui"))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR, "Unknown app '%s'", app);
		return FALSE;
	}
	name = ai_provider_type_to_string(provider);
	if (g_str_equal(name, "unknown"))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		            "Unknown provider type %d", (gint)provider);
		return FALSE;
	}
	directory = g_build_filename(g_get_user_config_dir(), "ai-glib", NULL);
	path = g_build_filename(directory, AI_CONFIG_FILENAME, NULL);
	if (!g_file_get_contents(path, &contents, &length, &read_error))
	{
		if (!g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
		{
			g_propagate_error(error, (GError *)g_steal_pointer(&read_error));
			return FALSE;
		}
		contents = g_strdup("{}\n");
		length = 3;
	}
	document = config_yaml_parse(contents, length, error);
	if (document == NULL)
		return FALSE;
	if (app != NULL)
	{
		mapping_id = config_yaml_copy_mapping(document, 1, "apps");
		mapping_id = config_yaml_copy_mapping(document, mapping_id, app);
	}
	values[0] = name;
	values[1] = saved_model;
	for (i = 0; i < G_N_ELEMENTS(keys); i++)
	{
		gint value_id;
		gboolean found = FALSE;

		/* New nodes avoid mutating unrelated aliases of an old default value. */
		value_id = yaml_document_add_scalar(document, (const yaml_char_t *)YAML_STR_TAG,
			(const yaml_char_t *)values[i], -1, YAML_DOUBLE_QUOTED_SCALAR_STYLE);
		root = yaml_document_get_node(document, mapping_id);
		for (pair = root->data.mapping.pairs.start; pair < root->data.mapping.pairs.top; pair++)
		{
			yaml_node_t *key = yaml_document_get_node(document, pair->key);
			if (key->type == YAML_SCALAR_NODE &&
			    g_str_equal((const gchar *)key->data.scalar.value, keys[i]))
			{
				pair->value = value_id;
				found = TRUE;
			}
		}
		if (!found)
		{
			gint key_id = yaml_document_add_scalar(document, (const yaml_char_t *)YAML_STR_TAG,
				(const yaml_char_t *)keys[i], -1, YAML_PLAIN_SCALAR_STYLE);
			yaml_document_append_mapping_pair(document, mapping_id, key_id, value_id);
		}
	}
	if (!yaml_emitter_initialize(&emitter))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR, "Cannot initialize YAML emitter");
		return FALSE;
	}
	yaml_emitter_set_output(&emitter, config_yaml_output, output);
	/* dump consumes the document contents even on failure, not its allocation. */
	emitted = yaml_emitter_open(&emitter);
	if (emitted)
	{
		emitted = yaml_emitter_dump(&emitter, document);
		g_free(g_steal_pointer(&document));
	}
	if (emitted)
		emitted = yaml_emitter_close(&emitter);
	if (!emitted)
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR, "Cannot emit config YAML: %s",
		            emitter.problem != NULL ? emitter.problem : "emitter failure");
	yaml_emitter_delete(&emitter);
	if (!emitted)
		return FALSE;
	if (g_mkdir_with_parents(directory, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot create config directory %s: %s", directory, g_strerror(errno));
		return FALSE;
	}
	/* Replace the inode, not its contents: old permissive modes must not survive. */
	file = g_file_new_for_path(path);
	if (!g_file_replace_contents(file, output->str, output->len, NULL, FALSE,
	                             G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
	                             NULL, NULL, error))
		return FALSE;
	if (app == NULL)
	{
		ai_config_set_default_provider(self, provider);
		ai_config_set_default_model(self, saved_model[0] != '\0' ? saved_model : NULL);
	}
	else
	{
		i = g_str_equal(app, "ai") ? 0 : 1;
		self->app_providers[i] = provider;
		g_free(self->app_models[i]);
		self->app_models[i] = saved_model[0] != '\0' ? g_strdup(saved_model) : NULL;
	}
	return TRUE;
}
