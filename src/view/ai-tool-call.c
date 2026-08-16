/*
 * ai-tool-call.c - One tool call, as a transcript needs to show it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "view/ai-tool-call.h"
#include "view/ai-tool-style.h"

struct _AiToolCall
{
    GObject          parent_instance;

    gchar           *id;
    gchar           *name;
    AiToolUse       *tool_use;
    AiToolCallState  state;
    gchar           *result;
    gboolean         is_error;

    gchar           *target;         /* derived, cached */
    gboolean         target_resolved;

    guint            lines_added;
    guint            lines_removed;

    gint64           started_us;
    gint64           finished_us;
};

G_DEFINE_TYPE(AiToolCall, ai_tool_call, G_TYPE_OBJECT)

static void ai_tool_call_derive_diff(AiToolCall *self);

static void
ai_tool_call_finalize(GObject *object)
{
    AiToolCall *self = AI_TOOL_CALL(object);

    g_clear_pointer(&self->id, g_free);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->result, g_free);
    g_clear_pointer(&self->target, g_free);
    g_clear_object(&self->tool_use);

    G_OBJECT_CLASS(ai_tool_call_parent_class)->finalize(object);
}

static void
ai_tool_call_class_init(AiToolCallClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_tool_call_finalize;
}

static void
ai_tool_call_init(AiToolCall *self)
{
    self->state = AI_TOOL_CALL_PENDING;
    self->started_us = g_get_monotonic_time();
}

/**
 * ai_tool_call_new:
 * @tool_use: (transfer none) (nullable): what the model asked for
 *
 * Creates a call in the %AI_TOOL_CALL_PENDING state.
 *
 * Returns: (transfer full): a new #AiToolCall
 */
AiToolCall *
ai_tool_call_new(AiToolUse *tool_use)
{
    g_autoptr(AiToolCall) self = g_object_new(AI_TYPE_TOOL_CALL, NULL);

    ai_tool_call_set_tool_use(self, tool_use);

    return (AiToolCall *)g_steal_pointer(&self);
}

/**
 * ai_tool_call_set_tool_use:
 * @self: an #AiToolCall
 * @tool_use: (transfer none) (nullable): the request
 *
 * Replaces what is known about the request.
 *
 * Called more than once for one call, because a streamed tool announces its
 * name before its arguments exist --- the first #AiEvent carries an empty
 * input and a later one carries the assembled arguments. Anything derived
 * from the input is recomputed here rather than at construction, so a call
 * whose arguments arrive late still gets its target and diff counts.
 */
void
ai_tool_call_set_tool_use(
    AiToolCall *self,
    AiToolUse  *tool_use
){
    g_return_if_fail(AI_IS_TOOL_CALL(self));

    if (tool_use == NULL)
    {
        return;
    }

    g_set_object(&self->tool_use, tool_use);

    if (self->id == NULL || self->id[0] == '\0')
    {
        g_free(self->id);
        self->id = g_strdup(ai_tool_use_get_id(tool_use));
    }

    if (ai_tool_use_get_name(tool_use) != NULL)
    {
        g_free(self->name);
        self->name = g_strdup(ai_tool_use_get_name(tool_use));
    }

    /* The arguments may have grown, so any derivation from them is stale. */
    g_clear_pointer(&self->target, g_free);
    self->target_resolved = FALSE;
    ai_tool_call_derive_diff(self);
}

/**
 * ai_tool_call_get_id:
 * @self: an #AiToolCall
 *
 * Returns: (transfer none) (nullable): the tool use id
 */
const gchar *
ai_tool_call_get_id(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), NULL);

    return self->id;
}

/**
 * ai_tool_call_get_name:
 * @self: an #AiToolCall
 *
 * Returns: (transfer none) (nullable): the tool name
 */
const gchar *
ai_tool_call_get_name(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), NULL);

    return self->name;
}

/**
 * ai_tool_call_get_tool_use:
 * @self: an #AiToolCall
 *
 * Returns: (transfer none) (nullable): the request
 */
AiToolUse *
ai_tool_call_get_tool_use(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), NULL);

    return self->tool_use;
}

/**
 * ai_tool_call_get_state:
 * @self: an #AiToolCall
 *
 * Returns: where the call has got to
 */
AiToolCallState
ai_tool_call_get_state(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), AI_TOOL_CALL_PENDING);

    return self->state;
}

/**
 * ai_tool_call_set_state:
 * @self: an #AiToolCall
 * @state: the new state
 *
 * Moves the call to @state.
 */
void
ai_tool_call_set_state(
    AiToolCall      *self,
    AiToolCallState  state
){
    g_return_if_fail(AI_IS_TOOL_CALL(self));

    self->state = state;
}

/**
 * ai_tool_call_get_category:
 * @self: an #AiToolCall
 *
 * Which bucket this groups into when a transcript summarises several calls.
 *
 * Returns: the category, %AI_TOOL_CATEGORY_OTHER for an unknown tool
 */
AiToolCategory
ai_tool_call_get_category(AiToolCall *self)
{
    const AiToolStyle *style;

    g_return_val_if_fail(AI_IS_TOOL_CALL(self), AI_TOOL_CATEGORY_OTHER);

    style = ai_tool_style_lookup(self->name);

    return style != NULL ? style->category : AI_TOOL_CATEGORY_OTHER;
}

/**
 * ai_tool_call_get_target:
 * @self: an #AiToolCall
 *
 * What the call acted on, as a transcript should show it.
 *
 * A path is reduced to its basename: `Edited ai-style.c` is what a reader
 * needs, and the full path would push everything else off the line. A
 * command is shown whole but with newlines flattened, since a multi-line
 * command would break the one-line summary it appears in.
 *
 * Returns: (transfer none) (nullable): the target, or %NULL when the tool
 *   has none or its arguments have not arrived yet
 */
const gchar *
ai_tool_call_get_target(AiToolCall *self)
{
    const AiToolStyle *style;
    const gchar *raw;

    g_return_val_if_fail(AI_IS_TOOL_CALL(self), NULL);

    if (self->target_resolved)
    {
        return self->target;
    }

    self->target_resolved = TRUE;

    style = ai_tool_style_lookup(self->name);

    if (style == NULL || style->target_key == NULL || self->tool_use == NULL)
    {
        return NULL;
    }

    raw = ai_tool_use_get_input_string(self->tool_use, style->target_key);

    if (raw == NULL || raw[0] == '\0')
    {
        return NULL;
    }

    if (style->category == AI_TOOL_CATEGORY_FILE_READ ||
        style->category == AI_TOOL_CATEGORY_FILE_WRITE)
    {
        self->target = g_path_get_basename(raw);
    }
    else
    {
        gchar **lines = g_strsplit(raw, "\n", -1);

        self->target = g_strjoinv(" ", lines);
        g_strfreev(lines);
        g_strstrip(self->target);
    }

    return self->target;
}

/**
 * ai_tool_call_get_result:
 * @self: an #AiToolCall
 *
 * Returns: (transfer none) (nullable): what the tool produced
 */
const gchar *
ai_tool_call_get_result(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), NULL);

    return self->result;
}

/**
 * ai_tool_call_get_is_error:
 * @self: an #AiToolCall
 *
 * Returns: whether the call failed or was refused
 */
gboolean
ai_tool_call_get_is_error(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), FALSE);

    return self->is_error;
}

/**
 * ai_tool_call_finish:
 * @self: an #AiToolCall
 * @result: (transfer none) (nullable): what the tool produced
 *
 * Records the outcome and moves the call to %AI_TOOL_CALL_OK or
 * %AI_TOOL_CALL_FAILED.
 */
void
ai_tool_call_finish(
    AiToolCall   *self,
    AiToolResult *result
){
    g_return_if_fail(AI_IS_TOOL_CALL(self));

    self->finished_us = g_get_monotonic_time();

    if (result != NULL)
    {
        g_free(self->result);
        self->result = g_strdup(ai_tool_result_get_content(result));
        self->is_error = ai_tool_result_get_is_error(result);
    }

    self->state = self->is_error ? AI_TOOL_CALL_FAILED : AI_TOOL_CALL_OK;
}

/**
 * ai_tool_call_deny:
 * @self: an #AiToolCall
 *
 * Marks the call as refused before it ran.
 *
 * Distinct from %AI_TOOL_CALL_FAILED: a refusal is a decision, not a
 * malfunction, and a transcript should not report the two the same way.
 */
void
ai_tool_call_deny(AiToolCall *self)
{
    g_return_if_fail(AI_IS_TOOL_CALL(self));

    self->finished_us = g_get_monotonic_time();
    self->state = AI_TOOL_CALL_DENIED;
    self->is_error = TRUE;
}

/**
 * ai_tool_call_get_lines_added:
 * @self: an #AiToolCall
 *
 * Returns: lines this call added, or 0
 */
guint
ai_tool_call_get_lines_added(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), 0);

    return self->lines_added;
}

/**
 * ai_tool_call_get_lines_removed:
 * @self: an #AiToolCall
 *
 * Returns: lines this call removed, or 0
 */
guint
ai_tool_call_get_lines_removed(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), 0);

    return self->lines_removed;
}

/**
 * ai_tool_call_get_duration_us:
 * @self: an #AiToolCall
 *
 * How long the call took, in microseconds, or -1 if it has not finished.
 *
 * From the monotonic clock, so a clock adjustment mid-run cannot make a
 * tool appear to have taken negative time.
 *
 * Returns: the duration in microseconds, or -1
 */
gint64
ai_tool_call_get_duration_us(AiToolCall *self)
{
    g_return_val_if_fail(AI_IS_TOOL_CALL(self), -1);

    if (self->finished_us == 0)
    {
        return -1;
    }

    return self->finished_us - self->started_us;
}

/* Count the lines in a string, treating "" as zero rather than one. */
static guint
count_lines(const gchar *text)
{
    const gchar *p;
    guint lines;

    if (text == NULL || text[0] == '\0')
    {
        return 0;
    }

    lines = 1;

    for (p = text; *p != '\0'; p++)
    {
        if (*p == '\n')
        {
            lines++;
        }
    }

    /* A trailing newline does not start another line. */
    if (text[strlen(text) - 1] == '\n')
    {
        lines--;
    }

    return lines;
}

/*
 * Work out the +N-M for this call from its arguments.
 *
 * Not a real diff: an edit replaces one exact string with another, so the
 * line counts of the two are the answer, and a write adds the lines of what
 * it wrote. That is what produces the "+9-12" in a collapsed summary, and it
 * is derived from the request rather than from the result because the result
 * is prose the tool chose to print.
 *
 * Tools whose style says counts_diff is FALSE contribute nothing, which is
 * why running a command never inflates the figure.
 */
static void
ai_tool_call_derive_diff(AiToolCall *self)
{
    const AiToolStyle *style;

    self->lines_added = 0;
    self->lines_removed = 0;

    style = ai_tool_style_lookup(self->name);

    if (style == NULL || !style->counts_diff || self->tool_use == NULL)
    {
        return;
    }

    {
        const gchar *old_string =
            ai_tool_use_get_input_string(self->tool_use, "old_string");
        const gchar *new_string =
            ai_tool_use_get_input_string(self->tool_use, "new_string");
        const gchar *content =
            ai_tool_use_get_input_string(self->tool_use, "content");

        if (old_string != NULL || new_string != NULL)
        {
            self->lines_removed = count_lines(old_string);
            self->lines_added = count_lines(new_string);
        }
        else if (content != NULL)
        {
            self->lines_added = count_lines(content);
        }
    }
}
