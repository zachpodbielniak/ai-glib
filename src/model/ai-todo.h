/*
 * ai-todo.h - One item on an agent's todo list
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

G_BEGIN_DECLS

/**
 * AiTodoState:
 * @AI_TODO_PENDING: not started
 * @AI_TODO_IN_PROGRESS: being worked on now
 * @AI_TODO_COMPLETED: finished
 *
 * Where one item stands.
 */
typedef enum
{
    AI_TODO_PENDING = 0,
    AI_TODO_IN_PROGRESS,
    AI_TODO_COMPLETED
} AiTodoState;

/**
 * AiTodo:
 * @content: the task, as the model wrote it --- "Add the parser"
 * @active_form: the same task in progress --- "Adding the parser"
 * @state: where it stands
 *
 * One item on an agent's todo list.
 *
 * @active_form exists because a list is read while it is being worked
 * on: the item in progress reads better as "Adding the parser" than as
 * "Add the parser", and only the model knows how to say it. It is
 * optional; a renderer falls back to @content.
 */
typedef struct
{
    gchar       *content;
    gchar       *active_form;
    AiTodoState  state;
} AiTodo;

#define AI_TYPE_TODO (ai_todo_get_type())

GType
ai_todo_get_type(void) G_GNUC_CONST;

AiTodo *
ai_todo_new(
    const gchar *content,
    const gchar *active_form,
    AiTodoState  state
);

AiTodo *
ai_todo_copy(const AiTodo *self);

void
ai_todo_free(AiTodo *self);

const gchar *
ai_todo_get_label(const AiTodo *self);

const gchar *
ai_todo_state_to_string(AiTodoState state);

AiTodoState
ai_todo_state_from_string(const gchar *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiTodo, ai_todo_free)

G_END_DECLS
