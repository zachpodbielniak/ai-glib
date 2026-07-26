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
#define AI_OPENCODE_DEFAULT_MODEL "anthropic/claude-sonnet-5"

/*
 * OpenCode CLI model identifiers.
 * These map to the --model argument of the opencode CLI, in
 * provider/model-id form.
 */

/* Anthropic models — Claude 5 (current) */
#define AI_OPENCODE_MODEL_CLAUDE_FABLE_5    "anthropic/claude-fable-5"
#define AI_OPENCODE_MODEL_CLAUDE_OPUS_5     "anthropic/claude-opus-5"
#define AI_OPENCODE_MODEL_CLAUDE_SONNET_5   "anthropic/claude-sonnet-5"
#define AI_OPENCODE_MODEL_CLAUDE_HAIKU_4_5  "anthropic/claude-haiku-4-5"

/*
 * Anthropic models — older generations.
 *
 * SONNET_4, OPUS_4 and HAIKU_3_5 name IDs that Anthropic has already
 * retired upstream (2026-06-15 for the two Claude 4 IDs, 2026-02-19 for
 * Haiku 3.5); a request naming one of them will fail. The defines are
 * kept so existing source keeps compiling — do not use them in new code.
 */
#define AI_OPENCODE_MODEL_CLAUDE_OPUS_4_5   "anthropic/claude-opus-4-5-20251101"
#define AI_OPENCODE_MODEL_CLAUDE_SONNET_4   "anthropic/claude-sonnet-4-20250514"
#define AI_OPENCODE_MODEL_CLAUDE_OPUS_4     "anthropic/claude-opus-4-20250514"
#define AI_OPENCODE_MODEL_CLAUDE_HAIKU_3_5  "anthropic/claude-3-5-haiku-20241022"

/*
 * Unversioned Anthropic alias, retargeted to the newest tier on each
 * model refresh. Previously pointed at the now-retired Haiku 3.5.
 */
#define AI_OPENCODE_MODEL_CLAUDE_HAIKU      AI_OPENCODE_MODEL_CLAUDE_HAIKU_4_5

/* OpenAI models */
#define AI_OPENCODE_MODEL_GPT_4O            "openai/gpt-4o"
#define AI_OPENCODE_MODEL_GPT_4O_MINI       "openai/gpt-4o-mini"
#define AI_OPENCODE_MODEL_O3                "openai/o3"
#define AI_OPENCODE_MODEL_O3_MINI           "openai/o3-mini"

/* Google models */
#define AI_OPENCODE_MODEL_GEMINI_2_FLASH    "google/gemini-2.0-flash"
#define AI_OPENCODE_MODEL_GEMINI_2_5_PRO    "google/gemini-2.5-pro-preview-05-06"

AiOpenCodeClient *
ai_opencode_client_new(void);

AiOpenCodeClient *
ai_opencode_client_new_with_config(AiConfig *config);

gboolean
ai_opencode_client_get_skip_permissions(AiOpenCodeClient *self);

void
ai_opencode_client_set_skip_permissions(
    AiOpenCodeClient *self,
    gboolean          skip
);

G_END_DECLS
