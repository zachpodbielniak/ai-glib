/*
 * ai-claude-client.h - Anthropic Claude client
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

#define AI_TYPE_CLAUDE_CLIENT (ai_claude_client_get_type())

G_DECLARE_FINAL_TYPE(AiClaudeClient, ai_claude_client, AI, CLAUDE_CLIENT, AiClient)

/**
 * AI_CLAUDE_DEFAULT_MODEL:
 *
 * The default model for Claude clients.
 */
#define AI_CLAUDE_DEFAULT_MODEL "claude-sonnet-4-20250514"

/*
 * Claude 4.5 Models
 */
#define AI_CLAUDE_MODEL_OPUS_4_5        "claude-opus-4-5-20251101"
#define AI_CLAUDE_MODEL_SONNET_4_5      "claude-sonnet-4-5-20250929"
#define AI_CLAUDE_MODEL_HAIKU_4_5       "claude-haiku-4-5-20251001"

/*
 * Claude 4.1 Models
 */
#define AI_CLAUDE_MODEL_OPUS_4_1        "claude-opus-4-1-20250805"

/*
 * Claude 4 Models
 */
#define AI_CLAUDE_MODEL_OPUS_4          "claude-opus-4-20250514"
#define AI_CLAUDE_MODEL_SONNET_4        "claude-sonnet-4-20250514"

/*
 * Claude 3.7 Models
 */
#define AI_CLAUDE_MODEL_SONNET_3_7      "claude-3-7-sonnet-20250219"

/*
 * Claude 3.5 Models
 */
#define AI_CLAUDE_MODEL_HAIKU_3_5       "claude-3-5-haiku-20241022"

/*
 * Claude 3 Models
 */
#define AI_CLAUDE_MODEL_HAIKU_3         "claude-3-haiku-20240307"

/*
 * Convenience aliases
 */
#define AI_CLAUDE_MODEL_OPUS            AI_CLAUDE_MODEL_OPUS_4_5
#define AI_CLAUDE_MODEL_SONNET          AI_CLAUDE_MODEL_SONNET_4
#define AI_CLAUDE_MODEL_HAIKU           AI_CLAUDE_MODEL_HAIKU_4_5

/**
 * AI_CLAUDE_API_VERSION:
 *
 * The default Anthropic API version.
 */
#define AI_CLAUDE_API_VERSION "2023-06-01"

/**
 * ai_claude_client_new:
 *
 * Creates a new #AiClaudeClient using the default configuration.
 * The API key will be read from the ANTHROPIC_API_KEY environment variable.
 *
 * Returns: (transfer full): a new #AiClaudeClient
 */
AiClaudeClient *
ai_claude_client_new(void);

/**
 * ai_claude_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiClaudeClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiClaudeClient
 */
AiClaudeClient *
ai_claude_client_new_with_config(AiConfig *config);

/**
 * ai_claude_client_new_with_key:
 * @api_key: the Anthropic API key
 *
 * Creates a new #AiClaudeClient with the specified API key.
 *
 * Returns: (transfer full): a new #AiClaudeClient
 */
AiClaudeClient *
ai_claude_client_new_with_key(const gchar *api_key);

/**
 * ai_claude_client_get_api_version:
 * @self: an #AiClaudeClient
 *
 * Gets the Anthropic API version being used.
 *
 * Returns: (transfer none): the API version string
 */
const gchar *
ai_claude_client_get_api_version(AiClaudeClient *self);

/**
 * ai_claude_client_set_api_version:
 * @self: an #AiClaudeClient
 * @version: the API version string
 *
 * Sets the Anthropic API version to use.
 */
void
ai_claude_client_set_api_version(
    AiClaudeClient *self,
    const gchar    *version
);

G_END_DECLS
