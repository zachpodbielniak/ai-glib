/*
 * ai-opencode-client.h - OpenCode CLI client
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

#include "core/ai-cli-client.h"
#include "core/ai-config.h"

G_BEGIN_DECLS

#define AI_TYPE_OPENCODE_CLIENT (ai_opencode_client_get_type())

G_DECLARE_FINAL_TYPE(AiOpenCodeClient, ai_opencode_client, AI, OPENCODE_CLIENT, AiCliClient)

/**
 * AI_OPENCODE_DEFAULT_MODEL:
 *
 * The default model for OpenCode clients.
 */
#define AI_OPENCODE_DEFAULT_MODEL "anthropic/claude-sonnet-4-20250514"

/*
 * OpenCode CLI model identifiers.
 * These map to the --model argument of the opencode CLI.
 */

/* Anthropic models */
#define AI_OPENCODE_MODEL_CLAUDE_SONNET_4   "anthropic/claude-sonnet-4-20250514"
#define AI_OPENCODE_MODEL_CLAUDE_OPUS_4     "anthropic/claude-opus-4-20250514"
#define AI_OPENCODE_MODEL_CLAUDE_OPUS_4_5   "anthropic/claude-opus-4-5-20251101"
#define AI_OPENCODE_MODEL_CLAUDE_HAIKU      "anthropic/claude-3-5-haiku-20241022"

/* OpenAI models */
#define AI_OPENCODE_MODEL_GPT_4O            "openai/gpt-4o"
#define AI_OPENCODE_MODEL_GPT_4O_MINI       "openai/gpt-4o-mini"
#define AI_OPENCODE_MODEL_O3                "openai/o3"
#define AI_OPENCODE_MODEL_O3_MINI           "openai/o3-mini"

/* Google models */
#define AI_OPENCODE_MODEL_GEMINI_2_FLASH    "google/gemini-2.0-flash"
#define AI_OPENCODE_MODEL_GEMINI_2_5_PRO    "google/gemini-2.5-pro-preview-05-06"

/**
 * ai_opencode_client_new:
 *
 * Creates a new #AiOpenCodeClient.
 * The opencode CLI must be available in PATH or specified via
 * %OPENCODE_PATH environment variable.
 *
 * Returns: (transfer full): a new #AiOpenCodeClient
 */
AiOpenCodeClient *
ai_opencode_client_new(void);

/**
 * ai_opencode_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiOpenCodeClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiOpenCodeClient
 */
AiOpenCodeClient *
ai_opencode_client_new_with_config(AiConfig *config);

G_END_DECLS
