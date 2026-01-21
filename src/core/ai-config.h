/*
 * ai-config.h - Configuration management for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "core/ai-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_CONFIG (ai_config_get_type())

G_DECLARE_FINAL_TYPE(AiConfig, ai_config, AI, CONFIG, GObject)

/**
 * AI_CONFIG_DEFAULT_TIMEOUT:
 *
 * Default timeout in seconds for API requests.
 */
#define AI_CONFIG_DEFAULT_TIMEOUT (120)

/**
 * AI_CONFIG_DEFAULT_MAX_RETRIES:
 *
 * Default maximum number of retry attempts for failed requests.
 */
#define AI_CONFIG_DEFAULT_MAX_RETRIES (3)

/**
 * ai_config_new:
 *
 * Creates a new #AiConfig instance with default settings.
 * Configuration values will be read from environment variables.
 *
 * Returns: (transfer full): a new #AiConfig
 */
AiConfig *
ai_config_new(void);

/**
 * ai_config_get_default:
 *
 * Gets the default shared #AiConfig instance.
 * This is a singleton that persists for the lifetime of the application.
 *
 * Returns: (transfer none): the default #AiConfig
 */
AiConfig *
ai_config_get_default(void);

/**
 * ai_config_get_api_key:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to get the key for
 *
 * Gets the API key for the specified provider.
 * First checks for an explicitly set key, then falls back to environment
 * variables.
 *
 * Returns: (transfer none) (nullable): the API key, or %NULL if not set
 */
const gchar *
ai_config_get_api_key(
    AiConfig       *self,
    AiProviderType  provider
);

/**
 * ai_config_set_api_key:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to set the key for
 * @api_key: (nullable): the API key to set
 *
 * Sets the API key for the specified provider.
 * This overrides any environment variable setting.
 */
void
ai_config_set_api_key(
    AiConfig       *self,
    AiProviderType  provider,
    const gchar    *api_key
);

/**
 * ai_config_get_base_url:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to get the URL for
 *
 * Gets the base URL for the specified provider.
 * Some providers (like OpenAI and Ollama) support custom base URLs.
 *
 * Returns: (transfer none): the base URL
 */
const gchar *
ai_config_get_base_url(
    AiConfig       *self,
    AiProviderType  provider
);

/**
 * ai_config_set_base_url:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to set the URL for
 * @base_url: (nullable): the base URL to set, or %NULL to use default
 *
 * Sets the base URL for the specified provider.
 */
void
ai_config_set_base_url(
    AiConfig       *self,
    AiProviderType  provider,
    const gchar    *base_url
);

/**
 * ai_config_get_timeout:
 * @self: an #AiConfig
 *
 * Gets the timeout in seconds for API requests.
 *
 * Returns: the timeout in seconds
 */
guint
ai_config_get_timeout(AiConfig *self);

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
);

/**
 * ai_config_get_max_retries:
 * @self: an #AiConfig
 *
 * Gets the maximum number of retry attempts for failed requests.
 *
 * Returns: the maximum retry count
 */
guint
ai_config_get_max_retries(AiConfig *self);

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
);

/**
 * ai_config_validate:
 * @self: an #AiConfig
 * @provider: the #AiProviderType to validate for
 * @error: (out) (optional): return location for a #GError
 *
 * Validates that the configuration is complete for the specified provider.
 * This checks that required settings (like API keys) are present.
 *
 * Returns: %TRUE if the configuration is valid, %FALSE otherwise
 */
gboolean
ai_config_validate(
    AiConfig        *self,
    AiProviderType   provider,
    GError         **error
);

G_END_DECLS
