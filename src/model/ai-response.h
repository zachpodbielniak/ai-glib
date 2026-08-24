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

/**
 * ai_response_get_cost_micros:
 * @self: an #AiResponse
 *
 * The cost of this turn in millionths of a US dollar, as reported by the
 * provider itself, or -1 when it did not report one.
 *
 * This is not derivable from #AiUsage.  A CLI backend bills for cache
 * reads and cache writes as well as for the input and output tokens it
 * reports, so a cost recomputed from a price table over those two
 * numbers understates the real one -- by a factor that grows with the
 * size of the context, which is exactly the case anybody watching a bill
 * cares about.  When the provider states a figure, it is the figure.
 *
 * Returns: the cost in micro-dollars, or -1 if unknown
 */
gint64
ai_response_get_cost_micros(AiResponse *self);

/**
 * ai_response_set_cost_micros:
 * @self: an #AiResponse
 * @cost_micros: cost in millionths of a US dollar, or -1 for unknown
 */
void
ai_response_set_cost_micros(
    AiResponse *self,
    gint64      cost_micros
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
