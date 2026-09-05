/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include "providers/ai-openai-client.h"

G_BEGIN_DECLS

#define AI_TYPE_OPENAI_COMPATIBLE_CLIENT (ai_openai_compatible_client_get_type())
G_DECLARE_FINAL_TYPE(AiOpenAICompatibleClient, ai_openai_compatible_client,
                     AI, OPENAI_COMPATIBLE_CLIENT, AiOpenAIClient)

AiOpenAICompatibleClient *ai_openai_compatible_client_new(void);
AiOpenAICompatibleClient *ai_openai_compatible_client_new_with_config(AiConfig *config);

G_END_DECLS
