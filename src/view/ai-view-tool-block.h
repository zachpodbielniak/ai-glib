/*
 * ai-view-tool-block.h - A group of tool calls, summarised
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

#include "view/ai-view-block.h"
#include "view/ai-tool-call.h"

G_BEGIN_DECLS

#define AI_TYPE_VIEW_TOOL_BLOCK (ai_view_tool_block_get_type())

G_DECLARE_FINAL_TYPE(AiViewToolBlock, ai_view_tool_block,
                     AI, VIEW_TOOL_BLOCK, AiViewBlock)

AiViewBlock *
ai_view_tool_block_new(void);

AiToolCall *
ai_view_tool_block_add_call(
    AiViewToolBlock *self,
    AiToolUse       *tool_use
);

AiToolCall *
ai_view_tool_block_find_call(
    AiViewToolBlock *self,
    const gchar     *tool_use_id
);

AiToolCall *
ai_view_tool_block_get_call(
    AiViewToolBlock *self,
    guint            index_
);

guint
ai_view_tool_block_get_n_calls(AiViewToolBlock *self);

guint
ai_view_tool_block_get_lines_added(AiViewToolBlock *self);

guint
ai_view_tool_block_get_lines_removed(AiViewToolBlock *self);

gchar *
ai_view_tool_block_get_summary(AiViewToolBlock *self);

void
ai_view_tool_block_call_changed(AiViewToolBlock *self);

G_END_DECLS
