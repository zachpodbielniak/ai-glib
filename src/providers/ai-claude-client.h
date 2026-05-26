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
#define AI_CLAUDE_DEFAULT_MODEL "claude-sonnet-4-6"

/*
 * Claude 4.7 Models (Latest)
 *
 * Starting with Claude 4.6, IDs are dateless but still pinned snapshots
 * (per Anthropic's model-versioning docs); no date suffix is required.
 */
#define AI_CLAUDE_MODEL_OPUS_4_7        "claude-opus-4-7"

/*
 * Claude 4.6 Models
 */
#define AI_CLAUDE_MODEL_SONNET_4_6      "claude-sonnet-4-6"
#define AI_CLAUDE_MODEL_OPUS_4_6        "claude-opus-4-6"

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
#define AI_CLAUDE_MODEL_OPUS            AI_CLAUDE_MODEL_OPUS_4_7
#define AI_CLAUDE_MODEL_SONNET          AI_CLAUDE_MODEL_SONNET_4_6
#define AI_CLAUDE_MODEL_HAIKU           AI_CLAUDE_MODEL_HAIKU_4_5

/**
 * AI_CLAUDE_API_VERSION:
 *
 * The default Anthropic API version.
 */
#define AI_CLAUDE_API_VERSION "2023-06-01"

AiClaudeClient *
ai_claude_client_new(void);

AiClaudeClient *
ai_claude_client_new_with_config(AiConfig *config);

AiClaudeClient *
ai_claude_client_new_with_key(const gchar *api_key);

const gchar *
ai_claude_client_get_api_version(AiClaudeClient *self);

void
ai_claude_client_set_api_version(
    AiClaudeClient *self,
    const gchar    *version
);

G_END_DECLS
