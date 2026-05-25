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
 * AI_CONFIG_SYSTEM_DIR:
 *
 * System-wide config directory for distro/image defaults.
 * Lowest priority in the fallback chain.
 */
#define AI_CONFIG_SYSTEM_DIR "/usr/share/ai-glib"

/**
 * AI_CONFIG_ADMIN_DIR:
 *
 * Admin config directory for system-level overrides.
 * Medium priority in the fallback chain.
 */
#define AI_CONFIG_ADMIN_DIR "/etc/ai-glib"

/**
 * AI_CONFIG_FILENAME:
 *
 * Config file name searched for in each config directory.
 */
#define AI_CONFIG_FILENAME "config.yaml"

AiConfig *
ai_config_new(void);

AiConfig *
ai_config_get_default(void);

const gchar *
ai_config_get_api_key(
    AiConfig       *self,
    AiProviderType  provider
);

void
ai_config_set_api_key(
    AiConfig       *self,
    AiProviderType  provider,
    const gchar    *api_key
);

const gchar *
ai_config_get_base_url(
    AiConfig       *self,
    AiProviderType  provider
);

void
ai_config_set_base_url(
    AiConfig       *self,
    AiProviderType  provider,
    const gchar    *base_url
);

guint
ai_config_get_timeout(AiConfig *self);

void
ai_config_set_timeout(
    AiConfig *self,
    guint     timeout_seconds
);

guint
ai_config_get_max_retries(AiConfig *self);

void
ai_config_set_max_retries(
    AiConfig *self,
    guint     max_retries
);

gboolean
ai_config_validate(
    AiConfig        *self,
    AiProviderType   provider,
    GError         **error
);

gboolean
ai_config_load_from_file(
    AiConfig     *self,
    const gchar  *path,
    GError      **error
);

AiProviderType
ai_config_get_default_provider(AiConfig *self);

void
ai_config_set_default_provider(
    AiConfig       *self,
    AiProviderType  provider
);

const gchar *
ai_config_get_default_model(AiConfig *self);

void
ai_config_set_default_model(
    AiConfig    *self,
    const gchar *model
);

G_END_DECLS
