/*
 * ai-view-blocks.h - The straightforward block kinds
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Four small variations on #AiViewBlock: a user turn, assistant prose,
 * reasoning, and a status note. They share a file because each is a
 * dozen lines of "hold a string, render it under one tag", and four files
 * saying that would obscure rather than clarify. The tool group, which
 * carries real logic, lives on its own in ai-view-tool-block.h.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "view/ai-view-block.h"
#include "model/ai-usage.h"
#include "model/ai-todo.h"

G_BEGIN_DECLS

/* --- The user's turn --- */

#define AI_TYPE_VIEW_TURN_BLOCK (ai_view_turn_block_get_type())

G_DECLARE_FINAL_TYPE(AiViewTurnBlock, ai_view_turn_block,
                     AI, VIEW_TURN_BLOCK, AiViewBlock)

AiViewBlock *
ai_view_turn_block_new(const gchar *text);

const gchar *
ai_view_turn_block_get_text(AiViewTurnBlock *self);

/* --- Assistant prose --- */

#define AI_TYPE_VIEW_TEXT_BLOCK (ai_view_text_block_get_type())

G_DECLARE_FINAL_TYPE(AiViewTextBlock, ai_view_text_block,
                     AI, VIEW_TEXT_BLOCK, AiViewBlock)

AiViewBlock *
ai_view_text_block_new(void);

void
ai_view_text_block_append(
    AiViewTextBlock *self,
    const gchar     *text
);

const gchar *
ai_view_text_block_get_text(AiViewTextBlock *self);

/* --- Reasoning --- */

#define AI_TYPE_VIEW_THINKING_BLOCK (ai_view_thinking_block_get_type())

G_DECLARE_FINAL_TYPE(AiViewThinkingBlock, ai_view_thinking_block,
                     AI, VIEW_THINKING_BLOCK, AiViewBlock)

AiViewBlock *
ai_view_thinking_block_new(void);

void
ai_view_thinking_block_append(
    AiViewThinkingBlock *self,
    const gchar         *text
);

const gchar *
ai_view_thinking_block_get_text(AiViewThinkingBlock *self);

/* --- A note, a token count, or a failure --- */

/**
 * AiViewStatusKind:
 * @AI_VIEW_STATUS_INFO: something worth saying
 * @AI_VIEW_STATUS_USAGE: token counts and cost
 * @AI_VIEW_STATUS_ERROR: the turn failed
 *
 * What sort of note a status block carries.
 */
typedef enum
{
    AI_VIEW_STATUS_INFO = 0,
    AI_VIEW_STATUS_USAGE,
    AI_VIEW_STATUS_ERROR
} AiViewStatusKind;

#define AI_TYPE_VIEW_STATUS_BLOCK (ai_view_status_block_get_type())

G_DECLARE_FINAL_TYPE(AiViewStatusBlock, ai_view_status_block,
                     AI, VIEW_STATUS_BLOCK, AiViewBlock)

AiViewBlock *
ai_view_status_block_new(
    AiViewStatusKind  kind,
    const gchar      *text
);

AiViewBlock *
ai_view_status_block_new_usage(
    AiUsage *usage,
    gint64   cost_micros
);

AiViewStatusKind
ai_view_status_block_get_status_kind(AiViewStatusBlock *self);

const gchar *
ai_view_status_block_get_text(AiViewStatusBlock *self);

/* ---- The todo list ---- */

#define AI_TYPE_VIEW_TODO_BLOCK (ai_view_todo_block_get_type())

G_DECLARE_FINAL_TYPE(AiViewTodoBlock, ai_view_todo_block,
                     AI, VIEW_TODO_BLOCK, AiViewBlock)

AiViewTodoBlock *
ai_view_todo_block_new(void);

void
ai_view_todo_block_set_todos(
    AiViewTodoBlock *self,
    GPtrArray       *todos
);

guint
ai_view_todo_block_get_n_todos(AiViewTodoBlock *self);

G_END_DECLS
