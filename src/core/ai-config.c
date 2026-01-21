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
#define GEMINI_API_KEY_ENV    "GEMINI_API_KEY"
#define XAI_API_KEY_ENV       "XAI_API_KEY"
#define GROK_API_KEY_ENV      "GROK_API_KEY"         /* Alternative for Grok */
#define OLLAMA_API_KEY_ENV    "OLLAMA_API_KEY"       /* Optional Ollama auth */
#define OLLAMA_HOST_ENV       "OLLAMA_HOST"

/*
 * Private data structure for AiConfig.
 * Stores API keys, base URLs, and other configuration options.
 */
struct _AiConfig
{
    GObject parent_instance;

    /* API keys for each provider (overrides env vars) */
    gchar *claude_api_key;
    gchar *openai_api_key;
    gchar *gemini_api_key;
    gchar *grok_api_key;
    gchar *ollama_api_key;   /* Optional - Ollama may require auth in some setups */

    /* Custom base URLs (overrides defaults) */
    gchar *openai_base_url;
    gchar *ollama_base_url;

    /* Request settings */
    guint timeout_seconds;
    guint max_retries;
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
    g_clear_pointer(&self->openai_api_key, g_free);
    g_clear_pointer(&self->gemini_api_key, g_free);
    g_clear_pointer(&self->grok_api_key, g_free);
    g_clear_pointer(&self->ollama_api_key, g_free);
    g_clear_pointer(&self->openai_base_url, g_free);
    g_clear_pointer(&self->ollama_base_url, g_free);

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
}

/**
 * ai_config_new:
 *
 * Creates a new #AiConfig instance with default settings.
 * Configuration values will be read from environment variables when
 * accessed via ai_config_get_api_key() and similar functions.
 *
 * Returns: (transfer full): a new #AiConfig
 */
AiConfig *
ai_config_new(void)
{
    g_autoptr(AiConfig) self = g_object_new(AI_TYPE_CONFIG, NULL);

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
 * Returns: (transfer none): the base URL
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
            return CLAUDE_BASE_URL;

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
            return GEMINI_BASE_URL;

        case AI_PROVIDER_GROK:
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
 * Sets the base URL for the specified provider.
 * Only OpenAI and Ollama support custom base URLs.
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
        case AI_PROVIDER_OPENAI:
            g_clear_pointer(&self->openai_base_url, g_free);
            self->openai_base_url = g_strdup(base_url);
            break;

        case AI_PROVIDER_OLLAMA:
            g_clear_pointer(&self->ollama_base_url, g_free);
            self->ollama_base_url = g_strdup(base_url);
            break;

        case AI_PROVIDER_CLAUDE:
        case AI_PROVIDER_GEMINI:
        case AI_PROVIDER_GROK:
        default:
            /* These providers don't support custom base URLs */
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

    /* Ollama doesn't require an API key (but supports optional auth) */
    if (provider == AI_PROVIDER_OLLAMA)
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
