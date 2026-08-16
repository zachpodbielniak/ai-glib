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
