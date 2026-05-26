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
#define AI_GROK_DEFAULT_MODEL "grok-4.3"

/*
 * Grok 4.3 Models (Latest)
 *
 * xAI changed its naming convention from dash-separated (grok-4-1) to
 * dot-separated (grok-4.3) starting with the 4.20 generation.
 */
#define AI_GROK_MODEL_4_3                          "grok-4.3"

/*
 * Grok 4.20 Models
 */
#define AI_GROK_MODEL_4_20_REASONING               "grok-4.20-0309-reasoning"
#define AI_GROK_MODEL_4_20_NON_REASONING           "grok-4.20-0309-non-reasoning"
#define AI_GROK_MODEL_4_20_MULTI_AGENT             "grok-4.20-multi-agent-0309"

/*
 * Grok Build Models (Coding Agent CLI)
 */
#define AI_GROK_MODEL_BUILD_0_1                    "grok-build-0.1"

/*
 * Grok 4.1 Models
 *
 * Note: retired upstream 2026-05-15 — server redirects to grok-4.3.
 */
#define AI_GROK_MODEL_4_1_FAST_REASONING     "grok-4-1-fast-reasoning"
#define AI_GROK_MODEL_4_1_FAST_NON_REASONING "grok-4-1-fast-non-reasoning"

/*
 * Grok 4 Models
 *
 * Note: retired upstream 2026-05-15 — server redirects to grok-4.3.
 */
#define AI_GROK_MODEL_4_0709                "grok-4-0709"
#define AI_GROK_MODEL_4_FAST_REASONING      "grok-4-fast-reasoning"
#define AI_GROK_MODEL_4_FAST_NON_REASONING  "grok-4-fast-non-reasoning"

/*
 * Grok 3 Models
 *
 * Note: retired upstream 2026-05-15 — server redirects to grok-4.3.
 */
#define AI_GROK_MODEL_3                     "grok-3"
#define AI_GROK_MODEL_3_MINI                "grok-3-mini"

/*
 * Grok 2 Models (Vision)
 *
 * Note: retired upstream — server redirects to current Grok models.
 */
#define AI_GROK_MODEL_2_VISION_1212         "grok-2-vision-1212"
#define AI_GROK_MODEL_2_IMAGE_1212          "grok-2-image-1212"

/*
 * Grok Code Models
 *
 * Note: grok-code-fast-1 retired upstream 2026-05-15 — server redirects
 * to grok-4.3. Use AI_GROK_MODEL_BUILD_0_1 for the current coding tier.
 */
#define AI_GROK_MODEL_CODE_FAST_1           "grok-code-fast-1"

/*
 * Convenience aliases
 */
#define AI_GROK_MODEL_LATEST                AI_GROK_MODEL_4_3
#define AI_GROK_MODEL_FAST                  AI_GROK_MODEL_4_20_NON_REASONING
#define AI_GROK_MODEL_CODE                  AI_GROK_MODEL_BUILD_0_1

/*
 * Image / Video Generation Models (Grok Imagine)
 */
#define AI_GROK_IMAGE_MODEL_GROK_IMAGINE         "grok-imagine-image"
#define AI_GROK_IMAGE_MODEL_GROK_IMAGINE_QUALITY "grok-imagine-image-quality"
#define AI_GROK_VIDEO_MODEL_GROK_IMAGINE         "grok-imagine-video"

/*
 * Legacy image model (retired upstream).
 */
#define AI_GROK_IMAGE_MODEL_GROK_2_IMAGE    "grok-2-image"
#define AI_GROK_IMAGE_DEFAULT_MODEL         AI_GROK_IMAGE_MODEL_GROK_IMAGINE

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
