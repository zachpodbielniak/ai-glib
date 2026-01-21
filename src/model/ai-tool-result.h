/*
 * ai-tool-result.h - Tool result content block
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

#include "model/ai-content-block.h"

G_BEGIN_DECLS

#define AI_TYPE_TOOL_RESULT (ai_tool_result_get_type())

G_DECLARE_FINAL_TYPE(AiToolResult, ai_tool_result, AI, TOOL_RESULT, AiContentBlock)

/**
 * ai_tool_result_new:
 * @tool_use_id: the ID of the tool use this is responding to
 * @content: the result content
 * @is_error: whether this result indicates an error
 *
 * Creates a new #AiToolResult with the given content.
 *
 * Returns: (transfer full): a new #AiToolResult
 */
AiToolResult *
ai_tool_result_new(
    const gchar *tool_use_id,
    const gchar *content,
    gboolean     is_error
);

/**
 * ai_tool_result_get_tool_use_id:
 * @self: an #AiToolResult
 *
 * Gets the tool use ID this result corresponds to.
 *
 * Returns: (transfer none): the tool use ID
 */
const gchar *
ai_tool_result_get_tool_use_id(AiToolResult *self);

/**
 * ai_tool_result_get_content:
 * @self: an #AiToolResult
 *
 * Gets the result content.
 *
 * Returns: (transfer none): the result content
 */
const gchar *
ai_tool_result_get_content(AiToolResult *self);

/**
 * ai_tool_result_get_is_error:
 * @self: an #AiToolResult
 *
 * Gets whether this result indicates an error.
 *
 * Returns: %TRUE if this is an error result
 */
gboolean
ai_tool_result_get_is_error(AiToolResult *self);

G_END_DECLS
