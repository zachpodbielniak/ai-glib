/*
 * ai-grok-client.h - xAI Grok client
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

#include "core/ai-client.h"
#include "core/ai-config.h"

G_BEGIN_DECLS

#define AI_TYPE_GROK_CLIENT (ai_grok_client_get_type())

G_DECLARE_FINAL_TYPE(AiGrokClient, ai_grok_client, AI, GROK_CLIENT, AiClient)

/**
 * AI_GROK_DEFAULT_MODEL:
 *
 * The default model for Grok clients.
 */
#define AI_GROK_DEFAULT_MODEL "grok-4-1-fast-reasoning"

/*
 * Grok 4.1 Models (Latest)
 */
#define AI_GROK_MODEL_4_1_FAST_REASONING     "grok-4-1-fast-reasoning"
#define AI_GROK_MODEL_4_1_FAST_NON_REASONING "grok-4-1-fast-non-reasoning"

/*
 * Grok 4 Models
 */
#define AI_GROK_MODEL_4_0709                "grok-4-0709"
#define AI_GROK_MODEL_4_FAST_REASONING      "grok-4-fast-reasoning"
#define AI_GROK_MODEL_4_FAST_NON_REASONING  "grok-4-fast-non-reasoning"

/*
 * Grok 3 Models
 */
#define AI_GROK_MODEL_3                     "grok-3"
#define AI_GROK_MODEL_3_MINI                "grok-3-mini"

/*
 * Grok 2 Models (Vision)
 */
#define AI_GROK_MODEL_2_VISION_1212         "grok-2-vision-1212"
#define AI_GROK_MODEL_2_IMAGE_1212          "grok-2-image-1212"

/*
 * Grok Code Models
 */
#define AI_GROK_MODEL_CODE_FAST_1           "grok-code-fast-1"

/*
 * Convenience aliases
 */
#define AI_GROK_MODEL_LATEST                AI_GROK_MODEL_4_1_FAST_REASONING
#define AI_GROK_MODEL_FAST                  AI_GROK_MODEL_4_1_FAST_NON_REASONING
#define AI_GROK_MODEL_CODE                  AI_GROK_MODEL_CODE_FAST_1

/**
 * ai_grok_client_new:
 *
 * Creates a new #AiGrokClient using the default configuration.
 * The API key will be read from the XAI_API_KEY environment variable.
 *
 * Returns: (transfer full): a new #AiGrokClient
 */
AiGrokClient *
ai_grok_client_new(void);

/**
 * ai_grok_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiGrokClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiGrokClient
 */
AiGrokClient *
ai_grok_client_new_with_config(AiConfig *config);

/**
 * ai_grok_client_new_with_key:
 * @api_key: the xAI API key
 *
 * Creates a new #AiGrokClient with the specified API key.
 *
 * Returns: (transfer full): a new #AiGrokClient
 */
AiGrokClient *
ai_grok_client_new_with_key(const gchar *api_key);

G_END_DECLS
