/*
 * ai-todo.c - One item on an agent's todo list
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-todo.h"

G_DEFINE_BOXED_TYPE(AiTodo, ai_todo, ai_todo_copy, ai_todo_free)

/**
 * ai_todo_new:
 * @content: the task, as the model wrote it
 * @active_form: (nullable): the same task phrased as in progress
 * @state: where it stands
 *
 * Returns: (transfer full): a new #AiTodo
 */
AiTodo *
ai_todo_new(
    const gchar *content,
    const gchar *active_form,
    AiTodoState  state
){
    AiTodo *self = g_slice_new0(AiTodo);

    self->content = g_strdup(content);
    self->active_form = g_strdup(active_form);
    self->state = state;

    return self;
}

/**
 * ai_todo_copy:
 * @self: (nullable): an #AiTodo
 *
 * Returns: (transfer full) (nullable): a copy
 */
AiTodo *
ai_todo_copy(const AiTodo *self)
{
    if (self == NULL)
    {
        return NULL;
    }

    return ai_todo_new(self->content, self->active_form, self->state);
}

/**
 * ai_todo_free:
 * @self: (nullable): an #AiTodo
 *
 * Frees @self.
 */
void
ai_todo_free(AiTodo *self)
{
    if (self == NULL)
    {
        return;
    }

    g_free(self->content);
    g_free(self->active_form);
    g_slice_free(AiTodo, self);
}

/**
 * ai_todo_get_label:
 * @self: an #AiTodo
 *
 * What to show for this item.
 *
 * The active phrasing while it is in progress, the plain one otherwise,
 * so a renderer does not have to know the rule. Falls back to @content
 * when the model did not supply an active form.
 *
 * Returns: (transfer none): the label, never %NULL
 */
const gchar *
ai_todo_get_label(const AiTodo *self)
{
    g_return_val_if_fail(self != NULL, "");

    if (self->state == AI_TODO_IN_PROGRESS &&
        self->active_form != NULL && self->active_form[0] != '\0')
    {
        return self->active_form;
    }

    return self->content != NULL ? self->content : "";
}

/**
 * ai_todo_state_to_string:
 * @state: an #AiTodoState
 *
 * The name the wire format uses, which is also the one claude-code's
 * TodoWrite uses --- a model that has seen either will write the same
 * strings.
 *
 * Returns: (transfer none): the name, never %NULL
 */
const gchar *
ai_todo_state_to_string(AiTodoState state)
{
    switch (state)
    {
        case AI_TODO_IN_PROGRESS:
            return "in_progress";

        case AI_TODO_COMPLETED:
            return "completed";

        case AI_TODO_PENDING:
        default:
            return "pending";
    }
}

/**
 * ai_todo_state_from_string:
 * @name: (nullable): a state name
 *
 * The inverse of ai_todo_state_to_string().
 *
 * An unrecognised name is %AI_TODO_PENDING rather than an error: this
 * reads model output, and a run should not stop because a model wrote
 * "done" instead of "completed".
 *
 * Returns: the state
 */
AiTodoState
ai_todo_state_from_string(const gchar *name)
{
    if (name == NULL)
    {
        return AI_TODO_PENDING;
    }

    if (g_strcmp0(name, "in_progress") == 0 ||
        g_strcmp0(name, "in-progress") == 0 ||
        g_strcmp0(name, "active") == 0)
    {
        return AI_TODO_IN_PROGRESS;
    }

    if (g_strcmp0(name, "completed") == 0 ||
        g_strcmp0(name, "complete") == 0 ||
        g_strcmp0(name, "done") == 0)
    {
        return AI_TODO_COMPLETED;
    }

    return AI_TODO_PENDING;
}
