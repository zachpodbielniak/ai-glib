/*
 * ai-openai-client.h - OpenAI GPT client
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

#define AI_TYPE_OPENAI_CLIENT (ai_openai_client_get_type())

G_DECLARE_FINAL_TYPE(AiOpenAIClient, ai_openai_client, AI, OPENAI_CLIENT, AiClient)

/**
 * AI_OPENAI_DEFAULT_MODEL:
 *
 * The default model for OpenAI clients.
 */
#define AI_OPENAI_DEFAULT_MODEL "gpt-4o"

/*
 * GPT-5.2 Models (Latest)
 */
#define AI_OPENAI_MODEL_GPT_5_2             "gpt-5.2"
#define AI_OPENAI_MODEL_GPT_5_2_PRO         "gpt-5.2-pro"
#define AI_OPENAI_MODEL_GPT_5_2_CODEX       "gpt-5.2-codex"

/*
 * GPT-5.1 Models
 */
#define AI_OPENAI_MODEL_GPT_5_1             "gpt-5.1"
#define AI_OPENAI_MODEL_GPT_5_1_CODEX       "gpt-5.1-codex"
#define AI_OPENAI_MODEL_GPT_5_1_CODEX_MAX   "gpt-5.1-codex-max"
#define AI_OPENAI_MODEL_GPT_5_1_CODEX_MINI  "gpt-5.1-codex-mini"

/*
 * GPT-5 Models
 */
#define AI_OPENAI_MODEL_GPT_5               "gpt-5"
#define AI_OPENAI_MODEL_GPT_5_MINI          "gpt-5-mini"
#define AI_OPENAI_MODEL_GPT_5_NANO          "gpt-5-nano"
#define AI_OPENAI_MODEL_GPT_5_PRO           "gpt-5-pro"
#define AI_OPENAI_MODEL_GPT_5_CODEX         "gpt-5-codex"

/*
 * GPT-4.1 Models
 */
#define AI_OPENAI_MODEL_GPT_4_1             "gpt-4.1"
#define AI_OPENAI_MODEL_GPT_4_1_MINI        "gpt-4.1-mini"
#define AI_OPENAI_MODEL_GPT_4_1_NANO        "gpt-4.1-nano"

/*
 * GPT-4o Models
 */
#define AI_OPENAI_MODEL_GPT_4O              "gpt-4o"
#define AI_OPENAI_MODEL_GPT_4O_MINI         "gpt-4o-mini"
#define AI_OPENAI_MODEL_CHATGPT_4O_LATEST   "chatgpt-4o-latest"

/*
 * GPT-4 Turbo Models
 */
#define AI_OPENAI_MODEL_GPT_4_TURBO         "gpt-4-turbo"
#define AI_OPENAI_MODEL_GPT_4_TURBO_PREVIEW "gpt-4-turbo-preview"

/*
 * GPT-4 Models
 */
#define AI_OPENAI_MODEL_GPT_4               "gpt-4"
#define AI_OPENAI_MODEL_GPT_4_0613          "gpt-4-0613"

/*
 * GPT-3.5 Models
 */
#define AI_OPENAI_MODEL_GPT_3_5_TURBO       "gpt-3.5-turbo"
#define AI_OPENAI_MODEL_GPT_3_5_TURBO_16K   "gpt-3.5-turbo-16k"
#define AI_OPENAI_MODEL_GPT_3_5_INSTRUCT    "gpt-3.5-turbo-instruct"

/*
 * O4 Series (Latest Reasoning)
 */
#define AI_OPENAI_MODEL_O4_MINI             "o4-mini"
#define AI_OPENAI_MODEL_O4_MINI_DEEP_RESEARCH "o4-mini-deep-research"

/*
 * O3 Series
 */
#define AI_OPENAI_MODEL_O3                  "o3"
#define AI_OPENAI_MODEL_O3_MINI             "o3-mini"

/*
 * O1 Series
 */
#define AI_OPENAI_MODEL_O1                  "o1"
#define AI_OPENAI_MODEL_O1_PRO              "o1-pro"

/*
 * Convenience aliases
 */
#define AI_OPENAI_MODEL_LATEST              AI_OPENAI_MODEL_GPT_5_2
#define AI_OPENAI_MODEL_FAST                AI_OPENAI_MODEL_GPT_4O_MINI
#define AI_OPENAI_MODEL_REASONING           AI_OPENAI_MODEL_O3

/**
 * ai_openai_client_new:
 *
 * Creates a new #AiOpenAIClient using the default configuration.
 * The API key will be read from the OPENAI_API_KEY environment variable.
 *
 * Returns: (transfer full): a new #AiOpenAIClient
 */
AiOpenAIClient *
ai_openai_client_new(void);

/**
 * ai_openai_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiOpenAIClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiOpenAIClient
 */
AiOpenAIClient *
ai_openai_client_new_with_config(AiConfig *config);

/**
 * ai_openai_client_new_with_key:
 * @api_key: the OpenAI API key
 *
 * Creates a new #AiOpenAIClient with the specified API key.
 *
 * Returns: (transfer full): a new #AiOpenAIClient
 */
AiOpenAIClient *
ai_openai_client_new_with_key(const gchar *api_key);

G_END_DECLS
