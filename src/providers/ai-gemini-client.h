/*
 * ai-gemini-client.h - Google Gemini client
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

#define AI_TYPE_GEMINI_CLIENT (ai_gemini_client_get_type())

G_DECLARE_FINAL_TYPE(AiGeminiClient, ai_gemini_client, AI, GEMINI_CLIENT, AiClient)

/**
 * AI_GEMINI_DEFAULT_MODEL:
 *
 * The default model for Gemini clients.
 */
#define AI_GEMINI_DEFAULT_MODEL "gemini-2.5-flash"

/*
 * Gemini 3 Models (Preview)
 */
#define AI_GEMINI_MODEL_3_FLASH_PREVIEW     "gemini-3-flash-preview"
#define AI_GEMINI_MODEL_3_PRO_PREVIEW       "gemini-3-pro-preview"

/*
 * Gemini 2.5 Models
 */
#define AI_GEMINI_MODEL_2_5_FLASH               "gemini-2.5-flash"
#define AI_GEMINI_MODEL_2_5_FLASH_LITE          "gemini-2.5-flash-lite"
#define AI_GEMINI_MODEL_2_5_FLASH_LITE_PREVIEW  "gemini-2.5-flash-lite-preview-09-2025"
#define AI_GEMINI_MODEL_2_5_FLASH_PREVIEW       "gemini-2.5-flash-preview-09-2025"
#define AI_GEMINI_MODEL_2_5_PRO                 "gemini-2.5-pro"

/*
 * Gemini 2.0 Models
 */
#define AI_GEMINI_MODEL_2_0_FLASH               "gemini-2.0-flash"
#define AI_GEMINI_MODEL_2_0_FLASH_001           "gemini-2.0-flash-001"
#define AI_GEMINI_MODEL_2_0_FLASH_EXP           "gemini-2.0-flash-exp"
#define AI_GEMINI_MODEL_2_0_FLASH_LITE          "gemini-2.0-flash-lite"
#define AI_GEMINI_MODEL_2_0_FLASH_LITE_001      "gemini-2.0-flash-lite-001"
#define AI_GEMINI_MODEL_2_0_FLASH_LITE_PREVIEW  "gemini-2.0-flash-lite-preview"

/*
 * Gemini Latest Aliases
 */
#define AI_GEMINI_MODEL_FLASH_LATEST        "gemini-flash-latest"
#define AI_GEMINI_MODEL_FLASH_LITE_LATEST   "gemini-flash-lite-latest"
#define AI_GEMINI_MODEL_PRO_LATEST          "gemini-pro-latest"

/*
 * Gemini Experimental
 */
#define AI_GEMINI_MODEL_EXP_1206            "gemini-exp-1206"

/*
 * Gemini Special Purpose
 */
#define AI_GEMINI_MODEL_DEEP_RESEARCH       "deep-research-pro-preview-12-2025"

/*
 * Gemma 3 Models (via Gemini API)
 */
#define AI_GEMINI_MODEL_GEMMA_3_27B         "gemma-3-27b-it"
#define AI_GEMINI_MODEL_GEMMA_3_12B         "gemma-3-12b-it"
#define AI_GEMINI_MODEL_GEMMA_3_4B          "gemma-3-4b-it"
#define AI_GEMINI_MODEL_GEMMA_3_1B          "gemma-3-1b-it"

/*
 * Convenience aliases
 */
#define AI_GEMINI_MODEL_FLASH               AI_GEMINI_MODEL_2_5_FLASH
#define AI_GEMINI_MODEL_PRO                 AI_GEMINI_MODEL_2_5_PRO

/*
 * Image Generation Models (Nano Banana / Native Gemini Image)
 *
 * Nano Banana is the codename for Gemini's native image generation.
 * - Nano Banana (gemini-2.5-flash-image): Fast, efficient, up to 1K resolution
 * - Nano Banana Pro (gemini-3-pro-image-preview): Professional, up to 4K resolution
 */
#define AI_GEMINI_IMAGE_MODEL_NANO_BANANA       "gemini-2.5-flash-image"
#define AI_GEMINI_IMAGE_MODEL_NANO_BANANA_PRO   "gemini-3-pro-image-preview"

/*
 * Image Generation Models (Imagen - Legacy)
 */
#define AI_GEMINI_IMAGE_MODEL_IMAGEN_4      "imagen-4.0-generate-001"
#define AI_GEMINI_IMAGE_MODEL_IMAGEN_3      "imagen-3.0-generate-001"

/*
 * Default image model (Nano Banana for native generation)
 */
#define AI_GEMINI_IMAGE_DEFAULT_MODEL       AI_GEMINI_IMAGE_MODEL_NANO_BANANA

/**
 * ai_gemini_client_new:
 *
 * Creates a new #AiGeminiClient using the default configuration.
 * The API key will be read from the GEMINI_API_KEY environment variable.
 *
 * Returns: (transfer full): a new #AiGeminiClient
 */
AiGeminiClient *
ai_gemini_client_new(void);

/**
 * ai_gemini_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiGeminiClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiGeminiClient
 */
AiGeminiClient *
ai_gemini_client_new_with_config(AiConfig *config);

/**
 * ai_gemini_client_new_with_key:
 * @api_key: the Google AI API key
 *
 * Creates a new #AiGeminiClient with the specified API key.
 *
 * Returns: (transfer full): a new #AiGeminiClient
 */
AiGeminiClient *
ai_gemini_client_new_with_key(const gchar *api_key);

G_END_DECLS
