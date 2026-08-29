/*
 * ai-cursor-client.h - Cursor Agent CLI client (cursor-agent)
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

#define AI_TYPE_CURSOR_CLIENT (ai_cursor_client_get_type())

G_DECLARE_FINAL_TYPE(AiCursorClient, ai_cursor_client, AI, CURSOR_CLIENT, AiCliClient)

/**
 * AI_CURSOR_DEFAULT_MODEL:
 *
 * The default model for Cursor clients: the CLI's "auto" selector.
 */
#define AI_CURSOR_DEFAULT_MODEL "auto"

/*
 * Model IDs accepted by the `cursor-agent` CLI's --model argument.
 *
 * Run `cursor-agent models` for the authoritative list; the CLI rejects
 * unknown ids rather than falling back. Parameterized models also accept
 * quoted bracket overrides, e.g.
 * 'claude-opus-4-8[context=1m,effort=high,fast=false]' -- see
 * #AiCursorClient:model-params.
 */

#define AI_CURSOR_MODEL_AUTO                                 "auto"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX_LOW                    "gpt-5.3-codex-low"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX_LOW_FAST               "gpt-5.3-codex-low-fast"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX                        "gpt-5.3-codex"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX_FAST                   "gpt-5.3-codex-fast"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX_HIGH                   "gpt-5.3-codex-high"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX_HIGH_FAST              "gpt-5.3-codex-high-fast"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX_XHIGH                  "gpt-5.3-codex-xhigh"
#define AI_CURSOR_MODEL_GPT_5_3_CODEX_XHIGH_FAST             "gpt-5.3-codex-xhigh-fast"
#define AI_CURSOR_MODEL_GPT_5_2                              "gpt-5.2"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_HIGH_FAST            "cursor-grok-4.6-high-fast"
#define AI_CURSOR_MODEL_COMPOSER_2_5                         "composer-2.5"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_HIGH          "claude-opus-5-thinking-high"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_HIGH_FAST     "claude-opus-5-thinking-high-fast"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_HIGH                     "gpt-5.6-sol-high"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_HIGH_FAST                "gpt-5.6-sol-high-fast"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_XHIGH                    "gpt-5.6-sol-xhigh"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_XHIGH_FAST               "gpt-5.6-sol-xhigh-fast"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_THINKING_HIGH         "claude-fable-5-thinking-high"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_THINKING_XHIGH        "claude-fable-5-thinking-xhigh"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_5_HIGH                 "cursor-grok-4.5-high"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_5_HIGH_FAST            "cursor-grok-4.5-high-fast"
#define AI_CURSOR_MODEL_GEMINI_3_7_FLASH_HIGH                "gemini-3.7-flash-high"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_THINKING_HIGH        "claude-sonnet-5-thinking-high"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_THINKING_XHIGH       "claude-sonnet-5-thinking-xhigh"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_HIGH                    "gpt-5.6-luna-high"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_LOW                  "cursor-grok-4.6-low"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_LOW_FAST             "cursor-grok-4.6-low-fast"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_MEDIUM               "cursor-grok-4.6-medium"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_MEDIUM_FAST          "cursor-grok-4.6-medium-fast"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_HIGH                 "cursor-grok-4.6-high"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_XHIGH                "cursor-grok-4.6-xhigh"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_6_XHIGH_FAST           "cursor-grok-4.6-xhigh-fast"
#define AI_CURSOR_MODEL_COMPOSER_2_5_FAST                    "composer-2.5-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_LOW                    "claude-opus-5-low"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_LOW_FAST               "claude-opus-5-low-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_MEDIUM                 "claude-opus-5-medium"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_MEDIUM_FAST            "claude-opus-5-medium-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_HIGH                   "claude-opus-5-high"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_HIGH_FAST              "claude-opus-5-high-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_LOW           "claude-opus-5-thinking-low"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_LOW_FAST      "claude-opus-5-thinking-low-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_MEDIUM        "claude-opus-5-thinking-medium"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_MEDIUM_FAST   "claude-opus-5-thinking-medium-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_XHIGH         "claude-opus-5-thinking-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_XHIGH_FAST    "claude-opus-5-thinking-xhigh-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_MAX           "claude-opus-5-thinking-max"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_MAX_FAST      "claude-opus-5-thinking-max-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_LOW                  "claude-opus-4-8-low"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_LOW_FAST             "claude-opus-4-8-low-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_MEDIUM               "claude-opus-4-8-medium"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_MEDIUM_FAST          "claude-opus-4-8-medium-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_HIGH                 "claude-opus-4-8-high"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_HIGH_FAST            "claude-opus-4-8-high-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_XHIGH                "claude-opus-4-8-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_XHIGH_FAST           "claude-opus-4-8-xhigh-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_MAX                  "claude-opus-4-8-max"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_MAX_FAST             "claude-opus-4-8-max-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_LOW         "claude-opus-4-8-thinking-low"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_LOW_FAST    "claude-opus-4-8-thinking-low-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_MEDIUM      "claude-opus-4-8-thinking-medium"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_MEDIUM_FAST "claude-opus-4-8-thinking-medium-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_HIGH        "claude-opus-4-8-thinking-high"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_HIGH_FAST   "claude-opus-4-8-thinking-high-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_XHIGH       "claude-opus-4-8-thinking-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_XHIGH_FAST  "claude-opus-4-8-thinking-xhigh-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_MAX         "claude-opus-4-8-thinking-max"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_THINKING_MAX_FAST    "claude-opus-4-8-thinking-max-fast"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_NONE                     "gpt-5.6-sol-none"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_NONE_FAST                "gpt-5.6-sol-none-fast"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_LOW                      "gpt-5.6-sol-low"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_LOW_FAST                 "gpt-5.6-sol-low-fast"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_MEDIUM                   "gpt-5.6-sol-medium"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_MEDIUM_FAST              "gpt-5.6-sol-medium-fast"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_MAX                      "gpt-5.6-sol-max"
#define AI_CURSOR_MODEL_GPT_5_6_SOL_MAX_FAST                 "gpt-5.6-sol-max-fast"
#define AI_CURSOR_MODEL_GPT_5_5_NONE                         "gpt-5.5-none"
#define AI_CURSOR_MODEL_GPT_5_5_NONE_FAST                    "gpt-5.5-none-fast"
#define AI_CURSOR_MODEL_GPT_5_5_LOW                          "gpt-5.5-low"
#define AI_CURSOR_MODEL_GPT_5_5_LOW_FAST                     "gpt-5.5-low-fast"
#define AI_CURSOR_MODEL_GPT_5_5_MEDIUM                       "gpt-5.5-medium"
#define AI_CURSOR_MODEL_GPT_5_5_MEDIUM_FAST                  "gpt-5.5-medium-fast"
#define AI_CURSOR_MODEL_GPT_5_5_HIGH                         "gpt-5.5-high"
#define AI_CURSOR_MODEL_GPT_5_5_HIGH_FAST                    "gpt-5.5-high-fast"
#define AI_CURSOR_MODEL_GPT_5_5_EXTRA_HIGH                   "gpt-5.5-extra-high"
#define AI_CURSOR_MODEL_GPT_5_5_EXTRA_HIGH_FAST              "gpt-5.5-extra-high-fast"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_LOW                   "claude-fable-5-low"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_MEDIUM                "claude-fable-5-medium"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_HIGH                  "claude-fable-5-high"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_XHIGH                 "claude-fable-5-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_MAX                   "claude-fable-5-max"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_THINKING_LOW          "claude-fable-5-thinking-low"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_THINKING_MEDIUM       "claude-fable-5-thinking-medium"
#define AI_CURSOR_MODEL_CLAUDE_FABLE_5_THINKING_MAX          "claude-fable-5-thinking-max"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_5_LOW                  "cursor-grok-4.5-low"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_5_LOW_FAST             "cursor-grok-4.5-low-fast"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_5_MEDIUM               "cursor-grok-4.5-medium"
#define AI_CURSOR_MODEL_CURSOR_GROK_4_5_MEDIUM_FAST          "cursor-grok-4.5-medium-fast"
#define AI_CURSOR_MODEL_GEMINI_3_7_FLASH_LOW                 "gemini-3.7-flash-low"
#define AI_CURSOR_MODEL_GEMINI_3_7_FLASH_MEDIUM              "gemini-3.7-flash-medium"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_NONE                   "gpt-5.6-terra-none"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_NONE_FAST              "gpt-5.6-terra-none-fast"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_LOW                    "gpt-5.6-terra-low"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_LOW_FAST               "gpt-5.6-terra-low-fast"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_MEDIUM                 "gpt-5.6-terra-medium"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_MEDIUM_FAST            "gpt-5.6-terra-medium-fast"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_HIGH                   "gpt-5.6-terra-high"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_HIGH_FAST              "gpt-5.6-terra-high-fast"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_XHIGH                  "gpt-5.6-terra-xhigh"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_XHIGH_FAST             "gpt-5.6-terra-xhigh-fast"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_MAX                    "gpt-5.6-terra-max"
#define AI_CURSOR_MODEL_GPT_5_6_TERRA_MAX_FAST               "gpt-5.6-terra-max-fast"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_LOW                  "claude-sonnet-5-low"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_MEDIUM               "claude-sonnet-5-medium"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_HIGH                 "claude-sonnet-5-high"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_XHIGH                "claude-sonnet-5-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_MAX                  "claude-sonnet-5-max"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_THINKING_LOW         "claude-sonnet-5-thinking-low"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_THINKING_MEDIUM      "claude-sonnet-5-thinking-medium"
#define AI_CURSOR_MODEL_CLAUDE_SONNET_5_THINKING_MAX         "claude-sonnet-5-thinking-max"
#define AI_CURSOR_MODEL_CLAUDE_4_6_SONNET_MEDIUM             "claude-4.6-sonnet-medium"
#define AI_CURSOR_MODEL_CLAUDE_4_6_SONNET_MEDIUM_THINKING    "claude-4.6-sonnet-medium-thinking"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_LOW                  "claude-opus-4-7-low"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_LOW_FAST             "claude-opus-4-7-low-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_MEDIUM               "claude-opus-4-7-medium"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_MEDIUM_FAST          "claude-opus-4-7-medium-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_HIGH                 "claude-opus-4-7-high"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_HIGH_FAST            "claude-opus-4-7-high-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_XHIGH                "claude-opus-4-7-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_XHIGH_FAST           "claude-opus-4-7-xhigh-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_MAX                  "claude-opus-4-7-max"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_MAX_FAST             "claude-opus-4-7-max-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_LOW         "claude-opus-4-7-thinking-low"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_LOW_FAST    "claude-opus-4-7-thinking-low-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_MEDIUM      "claude-opus-4-7-thinking-medium"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_MEDIUM_FAST "claude-opus-4-7-thinking-medium-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_HIGH        "claude-opus-4-7-thinking-high"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_HIGH_FAST   "claude-opus-4-7-thinking-high-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_XHIGH       "claude-opus-4-7-thinking-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_XHIGH_FAST  "claude-opus-4-7-thinking-xhigh-fast"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_MAX         "claude-opus-4-7-thinking-max"
#define AI_CURSOR_MODEL_CLAUDE_OPUS_4_7_THINKING_MAX_FAST    "claude-opus-4-7-thinking-max-fast"
#define AI_CURSOR_MODEL_GPT_5_4_LOW                          "gpt-5.4-low"
#define AI_CURSOR_MODEL_GPT_5_4_MEDIUM                       "gpt-5.4-medium"
#define AI_CURSOR_MODEL_GPT_5_4_MEDIUM_FAST                  "gpt-5.4-medium-fast"
#define AI_CURSOR_MODEL_GPT_5_4_HIGH                         "gpt-5.4-high"
#define AI_CURSOR_MODEL_GPT_5_4_HIGH_FAST                    "gpt-5.4-high-fast"
#define AI_CURSOR_MODEL_GPT_5_4_XHIGH                        "gpt-5.4-xhigh"
#define AI_CURSOR_MODEL_GPT_5_4_XHIGH_FAST                   "gpt-5.4-xhigh-fast"
#define AI_CURSOR_MODEL_CLAUDE_4_6_OPUS_HIGH                 "claude-4.6-opus-high"
#define AI_CURSOR_MODEL_CLAUDE_4_6_OPUS_MAX                  "claude-4.6-opus-max"
#define AI_CURSOR_MODEL_CLAUDE_4_6_OPUS_HIGH_THINKING        "claude-4.6-opus-high-thinking"
#define AI_CURSOR_MODEL_CLAUDE_4_6_OPUS_MAX_THINKING         "claude-4.6-opus-max-thinking"
#define AI_CURSOR_MODEL_CLAUDE_4_5_OPUS_HIGH                 "claude-4.5-opus-high"
#define AI_CURSOR_MODEL_CLAUDE_4_5_OPUS_HIGH_THINKING        "claude-4.5-opus-high-thinking"
#define AI_CURSOR_MODEL_GPT_5_2_LOW                          "gpt-5.2-low"
#define AI_CURSOR_MODEL_GPT_5_2_LOW_FAST                     "gpt-5.2-low-fast"
#define AI_CURSOR_MODEL_GPT_5_2_FAST                         "gpt-5.2-fast"
#define AI_CURSOR_MODEL_GPT_5_2_HIGH                         "gpt-5.2-high"
#define AI_CURSOR_MODEL_GPT_5_2_HIGH_FAST                    "gpt-5.2-high-fast"
#define AI_CURSOR_MODEL_GPT_5_2_XHIGH                        "gpt-5.2-xhigh"
#define AI_CURSOR_MODEL_GPT_5_2_XHIGH_FAST                   "gpt-5.2-xhigh-fast"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_NONE                    "gpt-5.6-luna-none"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_NONE_FAST               "gpt-5.6-luna-none-fast"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_LOW                     "gpt-5.6-luna-low"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_LOW_FAST                "gpt-5.6-luna-low-fast"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_MEDIUM                  "gpt-5.6-luna-medium"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_MEDIUM_FAST             "gpt-5.6-luna-medium-fast"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_HIGH_FAST               "gpt-5.6-luna-high-fast"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_XHIGH                   "gpt-5.6-luna-xhigh"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_XHIGH_FAST              "gpt-5.6-luna-xhigh-fast"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_MAX                     "gpt-5.6-luna-max"
#define AI_CURSOR_MODEL_GPT_5_6_LUNA_MAX_FAST                "gpt-5.6-luna-max-fast"
#define AI_CURSOR_MODEL_GEMINI_3_6_FLASH_MINIMAL             "gemini-3.6-flash-minimal"
#define AI_CURSOR_MODEL_GEMINI_3_6_FLASH_LOW                 "gemini-3.6-flash-low"
#define AI_CURSOR_MODEL_GEMINI_3_6_FLASH_MEDIUM              "gemini-3.6-flash-medium"
#define AI_CURSOR_MODEL_GEMINI_3_6_FLASH_HIGH                "gemini-3.6-flash-high"
#define AI_CURSOR_MODEL_GEMINI_3_1_PRO                       "gemini-3.1-pro"
#define AI_CURSOR_MODEL_GPT_5_4_MINI_NONE                    "gpt-5.4-mini-none"
#define AI_CURSOR_MODEL_GPT_5_4_MINI_LOW                     "gpt-5.4-mini-low"
#define AI_CURSOR_MODEL_GPT_5_4_MINI_MEDIUM                  "gpt-5.4-mini-medium"
#define AI_CURSOR_MODEL_GPT_5_4_MINI_HIGH                    "gpt-5.4-mini-high"
#define AI_CURSOR_MODEL_GPT_5_4_MINI_XHIGH                   "gpt-5.4-mini-xhigh"
#define AI_CURSOR_MODEL_GPT_5_4_NANO_NONE                    "gpt-5.4-nano-none"
#define AI_CURSOR_MODEL_GPT_5_4_NANO_LOW                     "gpt-5.4-nano-low"
#define AI_CURSOR_MODEL_GPT_5_4_NANO_MEDIUM                  "gpt-5.4-nano-medium"
#define AI_CURSOR_MODEL_GPT_5_4_NANO_HIGH                    "gpt-5.4-nano-high"
#define AI_CURSOR_MODEL_GPT_5_4_NANO_XHIGH                   "gpt-5.4-nano-xhigh"
#define AI_CURSOR_MODEL_CLAUDE_4_5_SONNET                    "claude-4.5-sonnet"
#define AI_CURSOR_MODEL_CLAUDE_4_5_SONNET_THINKING           "claude-4.5-sonnet-thinking"
#define AI_CURSOR_MODEL_GPT_5_1_LOW                          "gpt-5.1-low"
#define AI_CURSOR_MODEL_GPT_5_1                              "gpt-5.1"
#define AI_CURSOR_MODEL_GPT_5_1_HIGH                         "gpt-5.1-high"
#define AI_CURSOR_MODEL_GEMINI_3_5_FLASH                     "gemini-3.5-flash"
#define AI_CURSOR_MODEL_CLAUDE_4_SONNET                      "claude-4-sonnet"
#define AI_CURSOR_MODEL_CLAUDE_4_SONNET_THINKING             "claude-4-sonnet-thinking"
#define AI_CURSOR_MODEL_GPT_5_MINI                           "gpt-5-mini"
#define AI_CURSOR_MODEL_KIMI_K3_LOW                          "kimi-k3-low"
#define AI_CURSOR_MODEL_KIMI_K3_HIGH                         "kimi-k3-high"
#define AI_CURSOR_MODEL_KIMI_K3_MAX                          "kimi-k3-max"
#define AI_CURSOR_MODEL_KIMI_K2_7_CODE                       "kimi-k2.7-code"
#define AI_CURSOR_MODEL_GLM_5_2_HIGH                         "glm-5.2-high"
#define AI_CURSOR_MODEL_GLM_5_2_MAX                          "glm-5.2-max"
#define AI_CURSOR_MODEL_GEMINI_3_FLASH                       "gemini-3-flash"

#define AI_CURSOR_MODEL_LATEST AI_CURSOR_MODEL_AUTO

AiCursorClient *
ai_cursor_client_new(void);

AiCursorClient *
ai_cursor_client_new_with_config(AiConfig *config);

gboolean
ai_cursor_client_get_skip_permissions(AiCursorClient *self);

void
ai_cursor_client_set_skip_permissions(
    AiCursorClient *self,
    gboolean        skip
);

G_END_DECLS
