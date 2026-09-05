/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "core/ai-cli-client.h"
G_BEGIN_DECLS
#define AI_TYPE_CODEX_CLI_CLIENT (ai_codex_cli_client_get_type())
G_DECLARE_FINAL_TYPE(AiCodexCliClient, ai_codex_cli_client, AI, CODEX_CLI_CLIENT, AiCliClient)
#define AI_CODEX_CLI_MODEL_GPT_6_ASTRA "gpt-6-astra"
#define AI_CODEX_CLI_MODEL_GPT_5_6_SOL "gpt-5.6-sol"
#define AI_CODEX_CLI_MODEL_GPT_5_6_TERRA "gpt-5.6-terra"
#define AI_CODEX_CLI_MODEL_GPT_5_6_LUNA "gpt-5.6-luna"
#define AI_CODEX_CLI_MODEL_GPT_5_5 "gpt-5.5"
#define AI_CODEX_CLI_MODEL_GPT_5_4_MINI "gpt-5.4-mini"
#define AI_CODEX_CLI_MODEL_GPT_5_3_CODEX_SPARK "gpt-5.3-codex-spark"
#define AI_CODEX_CLI_DEFAULT_MODEL AI_CODEX_CLI_MODEL_GPT_6_ASTRA
AiCodexCliClient *ai_codex_cli_client_new(void);
AiCodexCliClient *ai_codex_cli_client_new_with_config(AiConfig *config);
const gchar *ai_codex_cli_client_get_sandbox(AiCodexCliClient *self);
void ai_codex_cli_client_set_sandbox(AiCodexCliClient *self, const gchar *sandbox);
gboolean ai_codex_cli_client_get_skip_permissions(AiCodexCliClient *self);
void ai_codex_cli_client_set_skip_permissions(AiCodexCliClient *self, gboolean skip);
G_END_DECLS
