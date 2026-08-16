/*
 * ai-view-blocks.c - The straightforward block kinds
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "view/ai-view-blocks.h"

/* The agent block reads state out of live #AiAgent objects. The view
 * layer already reaches into the agent layer through #AiConversation,
 * which drives a brigade; this is the same direction. */
#include "agent/ai-agent.h"

/* ================================================================
 * The user's turn
 * ================================================================ */

struct _AiViewTurnBlock
{
    AiViewBlock parent_instance;
    gchar      *text;
};

G_DEFINE_TYPE(AiViewTurnBlock, ai_view_turn_block, AI_TYPE_VIEW_BLOCK)

static AiViewBlockKind
turn_get_kind(AiViewBlock *block)
{
    (void)block;
    return AI_VIEW_BLOCK_TURN;
}

static AiRenderedText *
turn_render(AiViewBlock *block)
{
    AiViewTurnBlock *self = AI_VIEW_TURN_BLOCK(block);
    AiRenderedText *out = ai_rendered_text_new();

    /*
     * The marker is part of the text rather than a decoration the frontend
     * adds, so every frontend shows the same thing and none of them has to
     * know what a user turn looks like.
     */
    ai_rendered_text_append(out, "> ", AI_STYLE_DIM);
    ai_rendered_text_append(out, self->text != NULL ? self->text : "",
                            AI_STYLE_USER_PROMPT);

    return out;
}

static void
ai_view_turn_block_finalize(GObject *object)
{
    AiViewTurnBlock *self = AI_VIEW_TURN_BLOCK(object);

    g_clear_pointer(&self->text, g_free);

    G_OBJECT_CLASS(ai_view_turn_block_parent_class)->finalize(object);
}

static void
ai_view_turn_block_class_init(AiViewTurnBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_turn_block_finalize;
    block_class->render = turn_render;
    block_class->get_kind = turn_get_kind;
}

static void
ai_view_turn_block_init(AiViewTurnBlock *self)
{
    (void)self;
}

/**
 * ai_view_turn_block_new:
 * @text: (nullable): what the user said
 *
 * Creates a block for the user's turn. Complete on arrival --- the user
 * finished typing before it existed.
 *
 * Returns: (transfer full): a new #AiViewBlock
 */
AiViewBlock *
ai_view_turn_block_new(const gchar *text)
{
    AiViewTurnBlock *self = g_object_new(AI_TYPE_VIEW_TURN_BLOCK, NULL);

    self->text = g_strdup(text);
    ai_view_block_set_complete(AI_VIEW_BLOCK(self), TRUE);

    return AI_VIEW_BLOCK(self);
}

/**
 * ai_view_turn_block_get_text:
 * @self: an #AiViewTurnBlock
 *
 * Returns: (transfer none) (nullable): what the user said
 */
const gchar *
ai_view_turn_block_get_text(AiViewTurnBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_TURN_BLOCK(self), NULL);

    return self->text;
}

/* ================================================================
 * Assistant prose
 * ================================================================ */

struct _AiViewTextBlock
{
    AiViewBlock parent_instance;
    GString    *text;
};

G_DEFINE_TYPE(AiViewTextBlock, ai_view_text_block, AI_TYPE_VIEW_BLOCK)

static AiViewBlockKind
text_get_kind(AiViewBlock *block)
{
    (void)block;
    return AI_VIEW_BLOCK_TEXT;
}

static AiRenderedText *
text_render(AiViewBlock *block)
{
    AiViewTextBlock *self = AI_VIEW_TEXT_BLOCK(block);
    AiRenderedText *out = ai_rendered_text_new();

    ai_rendered_text_append(out, self->text->str, AI_STYLE_DEFAULT);

    return out;
}

static void
ai_view_text_block_finalize(GObject *object)
{
    AiViewTextBlock *self = AI_VIEW_TEXT_BLOCK(object);

    g_string_free(self->text, TRUE);

    G_OBJECT_CLASS(ai_view_text_block_parent_class)->finalize(object);
}

static void
ai_view_text_block_class_init(AiViewTextBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_text_block_finalize;
    block_class->render = text_render;
    block_class->get_kind = text_get_kind;
}

static void
ai_view_text_block_init(AiViewTextBlock *self)
{
    self->text = g_string_new(NULL);
}

/**
 * ai_view_text_block_new:
 *
 * Creates an empty prose block, to be grown with
 * ai_view_text_block_append().
 *
 * Returns: (transfer full): a new #AiViewBlock
 */
AiViewBlock *
ai_view_text_block_new(void)
{
    return AI_VIEW_BLOCK(g_object_new(AI_TYPE_VIEW_TEXT_BLOCK, NULL));
}

/**
 * ai_view_text_block_append:
 * @self: an #AiViewTextBlock
 * @text: (nullable): the chunk to add
 *
 * Appends a chunk of prose.
 *
 * This is how streaming reaches the transcript: one call per
 * %AI_EVENT_TEXT_DELTA. Each one invalidates the cached rendering and
 * emits ::changed, so a frontend redraws just this block.
 */
void
ai_view_text_block_append(
    AiViewTextBlock *self,
    const gchar     *text
){
    g_return_if_fail(AI_IS_VIEW_TEXT_BLOCK(self));

    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    g_string_append(self->text, text);
    ai_view_block_changed(AI_VIEW_BLOCK(self));
}

/**
 * ai_view_text_block_get_text:
 * @self: an #AiViewTextBlock
 *
 * Returns: (transfer none): the prose so far
 */
const gchar *
ai_view_text_block_get_text(AiViewTextBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_TEXT_BLOCK(self), "");

    return self->text->str;
}

/* ================================================================
 * Reasoning
 * ================================================================ */

struct _AiViewThinkingBlock
{
    AiViewBlock parent_instance;
    GString    *text;
};

G_DEFINE_TYPE(AiViewThinkingBlock, ai_view_thinking_block, AI_TYPE_VIEW_BLOCK)

static AiViewBlockKind
thinking_get_kind(AiViewBlock *block)
{
    (void)block;
    return AI_VIEW_BLOCK_THINKING;
}

static AiRenderedText *
thinking_render(AiViewBlock *block)
{
    AiViewThinkingBlock *self = AI_VIEW_THINKING_BLOCK(block);
    AiRenderedText *out = ai_rendered_text_new();

    /*
     * Collapsed by default: reasoning is worth having available and rarely
     * worth reading, and left expanded it drowns the answer.
     */
    if (!ai_view_block_get_expanded(block))
    {
        ai_rendered_text_append(out, "\xe2\x9c\xb3 thinking", AI_STYLE_THINKING);
        ai_rendered_text_append(out, " \xe2\x80\xba", AI_STYLE_MARKER);
        return out;
    }

    ai_rendered_text_append(out, "\xe2\x9c\xb3 thinking", AI_STYLE_THINKING);
    ai_rendered_text_append(out, " \xe2\x8c\x84\n", AI_STYLE_MARKER);
    ai_rendered_text_append(out, self->text->str, AI_STYLE_THINKING);

    return out;
}

static void
ai_view_thinking_block_finalize(GObject *object)
{
    AiViewThinkingBlock *self = AI_VIEW_THINKING_BLOCK(object);

    g_string_free(self->text, TRUE);

    G_OBJECT_CLASS(ai_view_thinking_block_parent_class)->finalize(object);
}

static void
ai_view_thinking_block_class_init(AiViewThinkingBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_thinking_block_finalize;
    block_class->render = thinking_render;
    block_class->get_kind = thinking_get_kind;
}

static void
ai_view_thinking_block_init(AiViewThinkingBlock *self)
{
    self->text = g_string_new(NULL);
}

/**
 * ai_view_thinking_block_new:
 *
 * Creates an empty reasoning block, collapsed.
 *
 * Returns: (transfer full): a new #AiViewBlock
 */
AiViewBlock *
ai_view_thinking_block_new(void)
{
    return AI_VIEW_BLOCK(g_object_new(AI_TYPE_VIEW_THINKING_BLOCK, NULL));
}

/**
 * ai_view_thinking_block_append:
 * @self: an #AiViewThinkingBlock
 * @text: (nullable): the chunk to add
 *
 * Appends a chunk of reasoning.
 */
void
ai_view_thinking_block_append(
    AiViewThinkingBlock *self,
    const gchar         *text
){
    g_return_if_fail(AI_IS_VIEW_THINKING_BLOCK(self));

    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    g_string_append(self->text, text);
    ai_view_block_changed(AI_VIEW_BLOCK(self));
}

/**
 * ai_view_thinking_block_get_text:
 * @self: an #AiViewThinkingBlock
 *
 * Returns: (transfer none): the reasoning so far
 */
const gchar *
ai_view_thinking_block_get_text(AiViewThinkingBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_THINKING_BLOCK(self), "");

    return self->text->str;
}

/* ================================================================
 * A note, a token count, or a failure
 * ================================================================ */

struct _AiViewStatusBlock
{
    AiViewBlock       parent_instance;
    AiViewStatusKind  kind;
    gchar            *text;
};

G_DEFINE_TYPE(AiViewStatusBlock, ai_view_status_block, AI_TYPE_VIEW_BLOCK)

static AiViewBlockKind
status_get_kind(AiViewBlock *block)
{
    (void)block;
    return AI_VIEW_BLOCK_STATUS;
}

static AiRenderedText *
status_render(AiViewBlock *block)
{
    AiViewStatusBlock *self = AI_VIEW_STATUS_BLOCK(block);
    AiRenderedText *out = ai_rendered_text_new();
    AiStyleTag tag;
    const gchar *marker;

    switch (self->kind)
    {
        case AI_VIEW_STATUS_ERROR:
            tag = AI_STYLE_ERROR;
            marker = "\xe2\x9c\x96 ";
            break;
        case AI_VIEW_STATUS_USAGE:
            tag = AI_STYLE_DIM;
            marker = "";
            break;
        default:
            tag = AI_STYLE_STATUS;
            marker = "\xe2\x97\x8b ";
            break;
    }

    ai_rendered_text_append(out, marker, tag);
    ai_rendered_text_append(out, self->text != NULL ? self->text : "", tag);

    return out;
}

static void
ai_view_status_block_finalize(GObject *object)
{
    AiViewStatusBlock *self = AI_VIEW_STATUS_BLOCK(object);

    g_clear_pointer(&self->text, g_free);

    G_OBJECT_CLASS(ai_view_status_block_parent_class)->finalize(object);
}

static void
ai_view_status_block_class_init(AiViewStatusBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_status_block_finalize;
    block_class->render = status_render;
    block_class->get_kind = status_get_kind;
}

static void
ai_view_status_block_init(AiViewStatusBlock *self)
{
    self->kind = AI_VIEW_STATUS_INFO;
}

/**
 * ai_view_status_block_new:
 * @kind: what sort of note this is
 * @text: (nullable): the note
 *
 * Creates a status block.
 *
 * Returns: (transfer full): a new #AiViewBlock
 */
AiViewBlock *
ai_view_status_block_new(
    AiViewStatusKind  kind,
    const gchar      *text
){
    AiViewStatusBlock *self = g_object_new(AI_TYPE_VIEW_STATUS_BLOCK, NULL);

    self->kind = kind;
    self->text = g_strdup(text);
    ai_view_block_set_complete(AI_VIEW_BLOCK(self), TRUE);

    return AI_VIEW_BLOCK(self);
}

/**
 * ai_view_status_block_new_usage:
 * @usage: (nullable): the token counts
 * @cost_micros: cost in micro-dollars, or -1 when unpriced
 *
 * Creates a status block reporting what a turn cost.
 *
 * The cost is omitted entirely when @cost_micros is negative rather than
 * shown as $0.00 --- most providers do not report cost, and claiming a turn
 * was free is worse than saying nothing about it.
 *
 * Returns: (transfer full): a new #AiViewBlock
 */
AiViewBlock *
ai_view_status_block_new_usage(
    AiUsage *usage,
    gint64   cost_micros
){
    g_autoptr(GString) text = g_string_new(NULL);

    if (usage != NULL)
    {
        g_string_append_printf(text, "%d in / %d out",
                               ai_usage_get_input_tokens(usage),
                               ai_usage_get_output_tokens(usage));
    }

    if (cost_micros >= 0)
    {
        if (text->len > 0)
        {
            g_string_append(text, "  ");
        }

        g_string_append_printf(text, "$%.4f", cost_micros / 1000000.0);
    }

    return ai_view_status_block_new(AI_VIEW_STATUS_USAGE, text->str);
}

/**
 * ai_view_status_block_get_status_kind:
 * @self: an #AiViewStatusBlock
 *
 * Returns: what sort of note this is
 */
AiViewStatusKind
ai_view_status_block_get_status_kind(AiViewStatusBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_STATUS_BLOCK(self), AI_VIEW_STATUS_INFO);

    return self->kind;
}

/**
 * ai_view_status_block_get_text:
 * @self: an #AiViewStatusBlock
 *
 * Returns: (transfer none) (nullable): the note
 */
const gchar *
ai_view_status_block_get_text(AiViewStatusBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_STATUS_BLOCK(self), NULL);

    return self->text;
}

/* ================================================================
 * The todo list
 * ================================================================ */

struct _AiViewTodoBlock
{
    AiViewBlock  parent_instance;
    GPtrArray   *todos;      /* AiTodo*, owned */
};

G_DEFINE_TYPE(AiViewTodoBlock, ai_view_todo_block, AI_TYPE_VIEW_BLOCK)

static AiViewBlockKind
todo_get_kind(AiViewBlock *block)
{
    (void)block;
    return AI_VIEW_BLOCK_TODO;
}

/* The box in front of an item. Chosen for the same reason the tool
 * markers are: one glyph, aligned, and legible in a terminal that has no
 * colour at all. */
static const gchar *
todo_marker(AiTodoState state)
{
    switch (state)
    {
        case AI_TODO_IN_PROGRESS:
            return "◐ ";   /* half-filled circle */

        case AI_TODO_COMPLETED:
            return "☑ ";   /* ballot box with check */

        case AI_TODO_PENDING:
        default:
            return "☐ ";   /* empty ballot box */
    }
}

static AiStyleTag
todo_tag(AiTodoState state)
{
    switch (state)
    {
        case AI_TODO_IN_PROGRESS:
            return AI_STYLE_TODO_ACTIVE;

        case AI_TODO_COMPLETED:
            return AI_STYLE_TODO_DONE;

        case AI_TODO_PENDING:
        default:
            return AI_STYLE_TODO_PENDING;
    }
}

static AiRenderedText *
todo_render(AiViewBlock *block)
{
    AiViewTodoBlock *self = AI_VIEW_TODO_BLOCK(block);
    AiRenderedText  *out = ai_rendered_text_new();
    guint            done = 0;
    guint            i;

    if (self->todos->len == 0)
    {
        ai_rendered_text_append(out, "No todos", AI_STYLE_DIM);
        return out;
    }

    for (i = 0; i < self->todos->len; i++)
    {
        const AiTodo *todo = g_ptr_array_index(self->todos, i);
        AiStyleTag    tag = todo_tag(todo->state);

        if (todo->state == AI_TODO_COMPLETED)
        {
            done++;
        }

        if (i > 0)
        {
            ai_rendered_text_append(out, "\n", AI_STYLE_DEFAULT);
        }

        ai_rendered_text_append(out, todo_marker(todo->state), tag);
        ai_rendered_text_append(out, ai_todo_get_label(todo), tag);
    }

    /*
     * A count, because the list is often longer than the screen and
     * "three of nine" is the thing a reader actually wants from it.
     */
    {
        g_autofree gchar *tally =
            g_strdup_printf("\n%u/%u done", done, self->todos->len);

        ai_rendered_text_append(out, tally, AI_STYLE_DIM);
    }

    return out;
}

static void
ai_view_todo_block_finalize(GObject *object)
{
    AiViewTodoBlock *self = AI_VIEW_TODO_BLOCK(object);

    g_clear_pointer(&self->todos, g_ptr_array_unref);

    G_OBJECT_CLASS(ai_view_todo_block_parent_class)->finalize(object);
}

static void
ai_view_todo_block_class_init(AiViewTodoBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_todo_block_finalize;
    block_class->render = todo_render;
    block_class->get_kind = todo_get_kind;
}

static void
ai_view_todo_block_init(AiViewTodoBlock *self)
{
    self->todos = g_ptr_array_new_with_free_func((GDestroyNotify)ai_todo_free);
}

/**
 * ai_view_todo_block_new:
 *
 * Creates an empty todo block.
 *
 * Returns: (transfer full): a new #AiViewTodoBlock
 */
AiViewTodoBlock *
ai_view_todo_block_new(void)
{
    return g_object_new(AI_TYPE_VIEW_TODO_BLOCK, NULL);
}

/**
 * ai_view_todo_block_set_todos:
 * @self: an #AiViewTodoBlock
 * @todos: (nullable) (element-type AiTodo): the current list, copied
 *
 * Replaces the list and marks the block changed.
 *
 * The block is meant to be updated in place rather than appended anew
 * each time. A model rewrites its plan eight times over a long task, and
 * eight copies of a nine-line list is not a transcript anybody can read
 * --- so this emits #AiViewBlock::changed, which a frontend answers by
 * redrawing one block, and #AiTranscript reports as ::block-changed
 * rather than ::items-changed.
 *
 * The items are copied, so the executor is free to replace its own list
 * on the next `todo_write` without disturbing what is on screen.
 */
void
ai_view_todo_block_set_todos(
    AiViewTodoBlock *self,
    GPtrArray       *todos
){
    guint i;

    g_return_if_fail(AI_IS_VIEW_TODO_BLOCK(self));

    g_ptr_array_set_size(self->todos, 0);

    if (todos != NULL)
    {
        for (i = 0; i < todos->len; i++)
        {
            g_ptr_array_add(self->todos,
                            ai_todo_copy(g_ptr_array_index(todos, i)));
        }
    }

    ai_view_block_changed(AI_VIEW_BLOCK(self));
}

/**
 * ai_view_todo_block_get_n_todos:
 * @self: an #AiViewTodoBlock
 *
 * Returns: how many items the block is showing
 */
guint
ai_view_todo_block_get_n_todos(AiViewTodoBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_TODO_BLOCK(self), 0);

    return self->todos->len;
}

/* ================================================================
 * Background agents
 * ================================================================ */

/*
 * What the block remembers about one agent.
 *
 * Copied rather than referenced. A brigade forgets an agent the moment
 * its answer is collected, and a panel that emptied itself as each
 * result came back would be at its least informative exactly when the
 * reader wants to look at it.
 */
typedef struct
{
    gchar        *id;
    gchar        *description;
    AiAgentState  state;
    gint64        elapsed_ms;
} AgentRow;

static void
agent_row_free(gpointer data)
{
    AgentRow *row = data;

    if (row == NULL) return;

    g_free(row->id);
    g_free(row->description);
    g_free(row);
}

struct _AiViewAgentBlock
{
    AiViewBlock  parent_instance;
    GPtrArray   *rows;      /* AgentRow*, owned */
};

G_DEFINE_TYPE(AiViewAgentBlock, ai_view_agent_block, AI_TYPE_VIEW_BLOCK)

static AiViewBlockKind
agent_get_kind(AiViewBlock *block)
{
    (void)block;
    return AI_VIEW_BLOCK_AGENT;
}

/* One glyph per state, legible with no colour at all --- the same rule
 * the todo and tool markers follow. */
static const gchar *
agent_marker(AiAgentState state)
{
    switch (state)
    {
        case AI_AGENT_STATE_DONE:
            return "✔ ";

        case AI_AGENT_STATE_FAILED:
        case AI_AGENT_STATE_OVER_BUDGET:
            return "✘ ";

        case AI_AGENT_STATE_CANCELLED:
        case AI_AGENT_STATE_INTERRUPTED:
            return "⊘ ";

        case AI_AGENT_STATE_QUEUED:
        case AI_AGENT_STATE_IDLE:
            return "· ";

        default:
            return "▸ ";   /* starting, running, waiting, blocked */
    }
}

/*
 * Agent state reuses the tool tags rather than adding three of its own.
 * They already mean "in flight", "succeeded" and "did not", which is
 * exactly the distinction here --- and a frontend that has chosen colours
 * for those gets this block right without being changed at all.
 */
static AiStyleTag
agent_tag(AiAgentState state)
{
    switch (state)
    {
        case AI_AGENT_STATE_DONE:
            return AI_STYLE_TOOL_OK;

        case AI_AGENT_STATE_FAILED:
        case AI_AGENT_STATE_CANCELLED:
        case AI_AGENT_STATE_OVER_BUDGET:
        case AI_AGENT_STATE_INTERRUPTED:
            return AI_STYLE_TOOL_FAILED;

        default:
            return AI_STYLE_TOOL_PENDING;
    }
}

static AiRenderedText *
agent_render(AiViewBlock *block)
{
    AiViewAgentBlock *self = AI_VIEW_AGENT_BLOCK(block);
    AiRenderedText   *out = ai_rendered_text_new();
    guint             live = 0;
    guint             i;

    if (self->rows->len == 0)
    {
        ai_rendered_text_append(out, "No background agents", AI_STYLE_DIM);
        return out;
    }

    for (i = 0; i < self->rows->len; i++)
    {
        const AgentRow   *row = g_ptr_array_index(self->rows, i);
        AiStyleTag        tag = agent_tag(row->state);
        g_autofree gchar *timing = NULL;

        if (ai_agent_state_is_live(row->state)) live++;

        if (i > 0)
        {
            ai_rendered_text_append(out, "\n", AI_STYLE_DEFAULT);
        }

        ai_rendered_text_append(out, agent_marker(row->state), tag);
        ai_rendered_text_append(out, row->id, AI_STYLE_TOOL_NAME);

        if (row->description != NULL && row->description[0] != '\0')
        {
            ai_rendered_text_append(out, "  ", AI_STYLE_DEFAULT);
            ai_rendered_text_append(out, row->description,
                                    AI_STYLE_TOOL_TARGET);
        }

        /*
         * The state word and the clock, dim. A reader scanning the panel
         * is looking for the marker and the name; the numbers matter
         * only once one of those has caught the eye.
         */
        timing = g_strdup_printf("  %s %" G_GINT64_FORMAT "s",
                                 ai_agent_state_to_string(row->state),
                                 row->elapsed_ms / 1000);
        ai_rendered_text_append(out, timing, AI_STYLE_DIM);
    }

    {
        g_autofree gchar *tally =
            g_strdup_printf("\n%u running, %u total", live, self->rows->len);

        ai_rendered_text_append(out, tally, AI_STYLE_DIM);
    }

    return out;
}

static void
ai_view_agent_block_finalize(GObject *object)
{
    AiViewAgentBlock *self = AI_VIEW_AGENT_BLOCK(object);

    g_clear_pointer(&self->rows, g_ptr_array_unref);

    G_OBJECT_CLASS(ai_view_agent_block_parent_class)->finalize(object);
}

static void
ai_view_agent_block_class_init(AiViewAgentBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_agent_block_finalize;
    block_class->render = agent_render;
    block_class->get_kind = agent_get_kind;
}

static void
ai_view_agent_block_init(AiViewAgentBlock *self)
{
    self->rows = g_ptr_array_new_with_free_func(agent_row_free);
}

/**
 * ai_view_agent_block_new:
 *
 * Creates an empty background-agent block.
 *
 * Returns: (transfer full): a new #AiViewAgentBlock
 */
AiViewAgentBlock *
ai_view_agent_block_new(void)
{
    return g_object_new(AI_TYPE_VIEW_AGENT_BLOCK, NULL);
}

void
ai_view_agent_block_set_agents(
    AiViewAgentBlock *self,
    GList            *agents
){
    GList *iter;

    g_return_if_fail(AI_IS_VIEW_AGENT_BLOCK(self));

    g_ptr_array_set_size(self->rows, 0);

    for (iter = agents; iter != NULL; iter = iter->next)
    {
        AiAgent  *agent = iter->data;
        AgentRow *row;

        if (!AI_IS_AGENT(agent)) continue;

        row = g_new0(AgentRow, 1);
        row->id          = g_strdup(ai_agent_get_id(agent));
        row->description = g_strdup(ai_agent_get_description(agent));
        row->state       = ai_agent_get_state(agent);
        row->elapsed_ms  = ai_agent_get_elapsed_ms(agent);

        g_ptr_array_add(self->rows, row);
    }

    ai_view_block_changed(AI_VIEW_BLOCK(self));
}

/**
 * ai_view_agent_block_get_n_agents:
 * @self: an #AiViewAgentBlock
 *
 * Returns: how many agents the block is showing
 */
guint
ai_view_agent_block_get_n_agents(AiViewAgentBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_AGENT_BLOCK(self), 0);

    return self->rows->len;
}

guint
ai_view_agent_block_get_n_live(AiViewAgentBlock *self)
{
    guint live = 0;
    guint i;

    g_return_val_if_fail(AI_IS_VIEW_AGENT_BLOCK(self), 0);

    for (i = 0; i < self->rows->len; i++)
    {
        const AgentRow *row = g_ptr_array_index(self->rows, i);

        if (ai_agent_state_is_live(row->state)) live++;
    }

    return live;
}
