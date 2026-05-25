/*
 * ai-response.h - API response
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

#include "core/ai-enums.h"
#include "model/ai-usage.h"
#include "model/ai-content-block.h"
#include "model/ai-tool-use.h"

G_BEGIN_DECLS

#define AI_TYPE_RESPONSE (ai_response_get_type())

G_DECLARE_FINAL_TYPE(AiResponse, ai_response, AI, RESPONSE, GObject)

AiResponse *
ai_response_new(
    const gchar *id,
    const gchar *model
);

const gchar *
ai_response_get_id(AiResponse *self);

const gchar *
ai_response_get_model(AiResponse *self);

AiStopReason
ai_response_get_stop_reason(AiResponse *self);

void
ai_response_set_stop_reason(
    AiResponse   *self,
    AiStopReason  reason
);

AiUsage *
ai_response_get_usage(AiResponse *self);

void
ai_response_set_usage(
    AiResponse *self,
    AiUsage    *usage
);

GList *
ai_response_get_content_blocks(AiResponse *self);

void
ai_response_add_content_block(
    AiResponse     *self,
    AiContentBlock *block
);

gchar *
ai_response_get_text(AiResponse *self);

gboolean
ai_response_has_tool_use(AiResponse *self);

GList *
ai_response_get_tool_uses(AiResponse *self);

G_END_DECLS
