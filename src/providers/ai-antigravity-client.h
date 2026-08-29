/*
 * ai-antigravity-client.h - Google Antigravity CLI client (agy)
 *
 * Copyright (C) 2026
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

#define AI_TYPE_ANTIGRAVITY_CLIENT (ai_antigravity_client_get_type())

G_DECLARE_FINAL_TYPE(AiAntigravityClient, ai_antigravity_client,
                     AI, ANTIGRAVITY_CLIENT, AiCliClient)

/**
 * AI_ANTIGRAVITY_DEFAULT_MODEL:
 *
 * The default model for Antigravity clients.
 */
#define AI_ANTIGRAVITY_DEFAULT_MODEL "gemini-3.7-flash-high"

/*
 * Model IDs accepted by the `agy` CLI's --model argument.
 *
 * Run `agy models` for the authoritative list; the CLI rejects unknown
 * ids rather than falling back.
 */

/* Gemini 3.7 Flash */
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_HIGH   "gemini-3.7-flash-high"
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_MEDIUM "gemini-3.7-flash-medium"
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_LOW    "gemini-3.7-flash-low"

/* Gemini 3.6 Flash */
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_6_FLASH_HIGH   "gemini-3.6-flash-high"
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_6_FLASH_MEDIUM "gemini-3.6-flash-medium"
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_6_FLASH_LOW    "gemini-3.6-flash-low"

/* Gemini 3.5 Flash */
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_5_FLASH_HIGH   "gemini-3.5-flash-high"
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_5_FLASH_MEDIUM "gemini-3.5-flash-medium"
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_5_FLASH_LOW    "gemini-3.5-flash-low"

/* Gemini 3.1 Pro */
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_1_PRO_HIGH     "gemini-3.1-pro-high"
#define AI_ANTIGRAVITY_MODEL_GEMINI_3_1_PRO_LOW      "gemini-3.1-pro-low"

/* Anthropic, via Antigravity */
#define AI_ANTIGRAVITY_MODEL_CLAUDE_SONNET_4_6       "claude-sonnet-4-6"
#define AI_ANTIGRAVITY_MODEL_CLAUDE_OPUS_4_6_THINKING "claude-opus-4-6-thinking"

/* Open-weight */
#define AI_ANTIGRAVITY_MODEL_GPT_OSS_120B_MEDIUM     "gpt-oss-120b-medium"

/*
 * Floating alias for the newest Gemini Flash (High) offering.
 */
#define AI_ANTIGRAVITY_MODEL_LATEST AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_HIGH

AiAntigravityClient *
ai_antigravity_client_new(void);

AiAntigravityClient *
ai_antigravity_client_new_with_config(AiConfig *config);

gboolean
ai_antigravity_client_get_skip_permissions(AiAntigravityClient *self);

void
ai_antigravity_client_set_skip_permissions(
    AiAntigravityClient *self,
    gboolean             skip
);

G_END_DECLS
