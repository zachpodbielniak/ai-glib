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

AiToolResult *
ai_tool_result_new(
    const gchar *tool_use_id,
    const gchar *content,
    gboolean     is_error
);

AiToolResult *
ai_tool_result_new_with_name(
    const gchar *tool_use_id,
    const gchar *tool_name,
    const gchar *content,
    gboolean     is_error
);

const gchar *
ai_tool_result_get_tool_use_id(AiToolResult *self);

const gchar *
ai_tool_result_get_tool_name(AiToolResult *self);

const gchar *
ai_tool_result_get_content(AiToolResult *self);

gboolean
ai_tool_result_get_is_error(AiToolResult *self);

G_END_DECLS
