/*
 * ai-tool-call.h - One tool call, as a transcript needs to show it
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

#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

G_BEGIN_DECLS

/**
 * AiToolCallState:
 * @AI_TOOL_CALL_PENDING: announced, not started
 * @AI_TOOL_CALL_RUNNING: in flight
 * @AI_TOOL_CALL_OK: finished successfully
 * @AI_TOOL_CALL_FAILED: finished with an error
 * @AI_TOOL_CALL_DENIED: refused before it ran
 *
 * Where a call has got to.
 */
typedef enum
{
    AI_TOOL_CALL_PENDING = 0,
    AI_TOOL_CALL_RUNNING,
    AI_TOOL_CALL_OK,
    AI_TOOL_CALL_FAILED,
    AI_TOOL_CALL_DENIED
} AiToolCallState;

/**
 * AiToolCategory:
 * @AI_TOOL_CATEGORY_OTHER: anything not covered below
 * @AI_TOOL_CATEGORY_FILE_READ: reading a file
 * @AI_TOOL_CATEGORY_FILE_WRITE: creating or changing a file
 * @AI_TOOL_CATEGORY_COMMAND: running a command
 * @AI_TOOL_CATEGORY_SEARCH: looking for something
 * @AI_TOOL_CATEGORY_NETWORK: fetching from the network
 * @AI_TOOL_CATEGORY_TASK: bookkeeping --- todo lists, sub-agents
 *
 * What kind of thing a tool does.
 *
 * A transcript groups calls by category to summarise them, which is what
 * turns five calls into "Edited 3 files, ran 2 commands".
 */
typedef enum
{
    AI_TOOL_CATEGORY_OTHER = 0,
    AI_TOOL_CATEGORY_FILE_READ,
    AI_TOOL_CATEGORY_FILE_WRITE,
    AI_TOOL_CATEGORY_COMMAND,
    AI_TOOL_CATEGORY_SEARCH,
    AI_TOOL_CATEGORY_NETWORK,
    AI_TOOL_CATEGORY_TASK,

    /*< private >*/
    AI_TOOL_N_CATEGORIES
} AiToolCategory;

#define AI_TYPE_TOOL_CALL (ai_tool_call_get_type())

G_DECLARE_FINAL_TYPE(AiToolCall, ai_tool_call, AI, TOOL_CALL, GObject)

AiToolCall *
ai_tool_call_new(AiToolUse *tool_use);

const gchar *
ai_tool_call_get_id(AiToolCall *self);

const gchar *
ai_tool_call_get_name(AiToolCall *self);

AiToolUse *
ai_tool_call_get_tool_use(AiToolCall *self);

void
ai_tool_call_set_tool_use(
    AiToolCall *self,
    AiToolUse  *tool_use
);

AiToolCallState
ai_tool_call_get_state(AiToolCall *self);

void
ai_tool_call_set_state(
    AiToolCall      *self,
    AiToolCallState  state
);

AiToolCategory
ai_tool_call_get_category(AiToolCall *self);

const gchar *
ai_tool_call_get_target(AiToolCall *self);

const gchar *
ai_tool_call_get_result(AiToolCall *self);

gboolean
ai_tool_call_get_is_error(AiToolCall *self);

void
ai_tool_call_finish(
    AiToolCall   *self,
    AiToolResult *result
);

void
ai_tool_call_deny(AiToolCall *self);

guint
ai_tool_call_get_lines_added(AiToolCall *self);

guint
ai_tool_call_get_lines_removed(AiToolCall *self);

gint64
ai_tool_call_get_duration_us(AiToolCall *self);

G_END_DECLS
