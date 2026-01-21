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

/**
 * ai_response_new:
 * @id: (nullable): the response ID
 * @model: (nullable): the model used
 *
 * Creates a new #AiResponse.
 *
 * Returns: (transfer full): a new #AiResponse
 */
AiResponse *
ai_response_new(
    const gchar *id,
    const gchar *model
);

/**
 * ai_response_get_id:
 * @self: an #AiResponse
 *
 * Gets the response ID.
 *
 * Returns: (transfer none) (nullable): the response ID
 */
const gchar *
ai_response_get_id(AiResponse *self);

/**
 * ai_response_get_model:
 * @self: an #AiResponse
 *
 * Gets the model used for this response.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_response_get_model(AiResponse *self);

/**
 * ai_response_get_stop_reason:
 * @self: an #AiResponse
 *
 * Gets the reason generation stopped.
 *
 * Returns: the #AiStopReason
 */
AiStopReason
ai_response_get_stop_reason(AiResponse *self);

/**
 * ai_response_set_stop_reason:
 * @self: an #AiResponse
 * @reason: the stop reason
 *
 * Sets the stop reason.
 */
void
ai_response_set_stop_reason(
    AiResponse   *self,
    AiStopReason  reason
);

/**
 * ai_response_get_usage:
 * @self: an #AiResponse
 *
 * Gets the token usage information.
 *
 * Returns: (transfer none) (nullable): the #AiUsage
 */
AiUsage *
ai_response_get_usage(AiResponse *self);

/**
 * ai_response_set_usage:
 * @self: an #AiResponse
 * @usage: (nullable): the usage information
 *
 * Sets the usage information.
 */
void
ai_response_set_usage(
    AiResponse *self,
    AiUsage    *usage
);

/**
 * ai_response_get_content_blocks:
 * @self: an #AiResponse
 *
 * Gets the list of content blocks in this response.
 *
 * Returns: (transfer none) (element-type AiContentBlock): the content blocks
 */
GList *
ai_response_get_content_blocks(AiResponse *self);

/**
 * ai_response_add_content_block:
 * @self: an #AiResponse
 * @block: (transfer full): the content block to add
 *
 * Adds a content block to the response.
 * The response takes ownership of the block.
 */
void
ai_response_add_content_block(
    AiResponse     *self,
    AiContentBlock *block
);

/**
 * ai_response_get_text:
 * @self: an #AiResponse
 *
 * Gets the concatenated text content of the response.
 * This combines all text content blocks into a single string.
 *
 * Returns: (transfer full) (nullable): the text content, free with g_free()
 */
gchar *
ai_response_get_text(AiResponse *self);

/**
 * ai_response_has_tool_use:
 * @self: an #AiResponse
 *
 * Checks if the response contains any tool use requests.
 *
 * Returns: %TRUE if there are tool use blocks
 */
gboolean
ai_response_has_tool_use(AiResponse *self);

/**
 * ai_response_get_tool_uses:
 * @self: an #AiResponse
 *
 * Gets the list of tool use requests in this response.
 *
 * Returns: (transfer container) (element-type AiToolUse): tool use blocks
 */
GList *
ai_response_get_tool_uses(AiResponse *self);

G_END_DECLS
