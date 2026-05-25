/*
 * ai-claude-code-client.h - Claude Code CLI client
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

#define AI_TYPE_CLAUDE_CODE_CLIENT (ai_claude_code_client_get_type())

G_DECLARE_FINAL_TYPE(AiClaudeCodeClient, ai_claude_code_client, AI, CLAUDE_CODE_CLIENT, AiCliClient)

/**
 * AI_CLAUDE_CODE_DEFAULT_MODEL:
 *
 * The default model for Claude Code clients.
 */
#define AI_CLAUDE_CODE_DEFAULT_MODEL "sonnet"

/*
 * Claude Code CLI model aliases.
 * These map to the --model argument of the claude CLI.
 */
#define AI_CLAUDE_CODE_MODEL_OPUS       "opus"
#define AI_CLAUDE_CODE_MODEL_SONNET     "sonnet"
#define AI_CLAUDE_CODE_MODEL_HAIKU      "haiku"


AiClaudeCodeClient *
ai_claude_code_client_new(void);

AiClaudeCodeClient *
ai_claude_code_client_new_with_config(AiConfig *config);

gdouble
ai_claude_code_client_get_total_cost(AiClaudeCodeClient *self);

gboolean
ai_claude_code_client_get_skip_permissions(AiClaudeCodeClient *self);

void
ai_claude_code_client_set_skip_permissions(
    AiClaudeCodeClient *self,
    gboolean            skip
);

G_END_DECLS
