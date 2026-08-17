/*
 * ai-view-block.c - One renderable unit of a transcript
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "view/ai-view-block.h"

typedef struct
{
    guint64         id;
    gboolean        expanded;
    gboolean        complete;

    /*
     * Rendering is cached against the width it was made for and the
     * revision it was made from. A transcript is re-rendered on every
     * keystroke and every terminal resize; re-deriving five hundred blocks
     * each time is work nobody asked for.
     */
    AiRenderedText *cached;
    guint           cached_width;
    guint64         cached_revision;
    guint64         revision;
} AiViewBlockPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(AiViewBlock, ai_view_block, G_TYPE_OBJECT)

enum
{
    SIGNAL_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/* Ids are unique for the process, so a frontend can key a map on one. */
static guint64 next_block_id = 1;

static void
ai_view_block_finalize(GObject *object)
{
    AiViewBlock *self = AI_VIEW_BLOCK(object);
    AiViewBlockPrivate *priv = ai_view_block_get_instance_private(self);

    g_clear_pointer(&priv->cached, ai_rendered_text_unref);

    G_OBJECT_CLASS(ai_view_block_parent_class)->finalize(object);
}

static void
ai_view_block_class_init(AiViewBlockClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_view_block_finalize;

    klass->render = NULL;
    klass->get_kind = NULL;

    /**
     * AiViewBlock::changed:
     * @self: the block
     *
     * Emitted when the block's content changed and its rendering is stale.
     *
     * Streaming mutates a block in place --- prose grows a delta at a time,
     * a tool group gains calls and its calls change state --- and none of
     * that is an insertion or a removal, so #GListModel::items-changed does
     * not cover it. #AiTranscript relays this as its own ::block-changed
     * with the position, which is what a frontend needs to update one
     * region rather than redraw everything.
     */
    signals[SIGNAL_CHANGED] =
        g_signal_new("changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 0);
}

static void
ai_view_block_init(AiViewBlock *self)
{
    AiViewBlockPrivate *priv = ai_view_block_get_instance_private(self);

    priv->id = next_block_id++;
    priv->expanded = FALSE;
    priv->complete = FALSE;
    priv->revision = 1;
}

/**
 * ai_view_block_get_id:
 * @self: an #AiViewBlock
 *
 * A stable identifier, unique for the lifetime of the process.
 *
 * A frontend that renders a transcript into something with its own
 * addressing --- an Emacs buffer, say --- keys its map from this. Positions
 * shift when blocks are removed; ids do not.
 *
 * Returns: the block id
 */
guint64
ai_view_block_get_id(AiViewBlock *self)
{
    AiViewBlockPrivate *priv;

    g_return_val_if_fail(AI_IS_VIEW_BLOCK(self), 0);

    priv = ai_view_block_get_instance_private(self);

    return priv->id;
}

/**
 * ai_view_block_get_kind:
 * @self: an #AiViewBlock
 *
 * Returns: what kind of block this is
 */
AiViewBlockKind
ai_view_block_get_kind(AiViewBlock *self)
{
    AiViewBlockClass *klass;

    g_return_val_if_fail(AI_IS_VIEW_BLOCK(self), AI_VIEW_BLOCK_STATUS);

    klass = AI_VIEW_BLOCK_GET_CLASS(self);

    if (klass->get_kind == NULL)
    {
        return AI_VIEW_BLOCK_STATUS;
    }

    return klass->get_kind(self);
}

/**
 * ai_view_block_get_expanded:
 * @self: an #AiViewBlock
 *
 * Returns: whether the block is showing its detail
 */
gboolean
ai_view_block_get_expanded(AiViewBlock *self)
{
    AiViewBlockPrivate *priv;

    g_return_val_if_fail(AI_IS_VIEW_BLOCK(self), FALSE);

    priv = ai_view_block_get_instance_private(self);

    return priv->expanded;
}

/**
 * ai_view_block_set_expanded:
 * @self: an #AiViewBlock
 * @expanded: %TRUE to show the detail
 *
 * Shows or hides the block's detail.
 *
 * Only some kinds render differently either way --- a tool group collapses
 * to one line and expands to one line per call --- but the flag lives on the
 * base so a frontend can offer the same affordance everywhere without
 * asking what it is looking at.
 */
void
ai_view_block_set_expanded(
    AiViewBlock *self,
    gboolean     expanded
){
    AiViewBlockPrivate *priv;

    g_return_if_fail(AI_IS_VIEW_BLOCK(self));

    priv = ai_view_block_get_instance_private(self);
    expanded = !!expanded;

    if (priv->expanded == expanded)
    {
        return;
    }

    priv->expanded = expanded;
    ai_view_block_changed(self);
}

/**
 * ai_view_block_get_complete:
 * @self: an #AiViewBlock
 *
 * Whether the block is finished, as opposed to still being streamed into.
 *
 * A frontend uses this to decide whether to show a cursor or a spinner
 * against it.
 *
 * Returns: %TRUE when nothing more will be added
 */
gboolean
ai_view_block_get_complete(AiViewBlock *self)
{
    AiViewBlockPrivate *priv;

    g_return_val_if_fail(AI_IS_VIEW_BLOCK(self), TRUE);

    priv = ai_view_block_get_instance_private(self);

    return priv->complete;
}

/**
 * ai_view_block_set_complete:
 * @self: an #AiViewBlock
 * @complete: %TRUE when nothing more will be added
 *
 * Marks the block finished or unfinished.
 */
void
ai_view_block_set_complete(
    AiViewBlock *self,
    gboolean     complete
){
    AiViewBlockPrivate *priv;

    g_return_if_fail(AI_IS_VIEW_BLOCK(self));

    priv = ai_view_block_get_instance_private(self);
    complete = !!complete;

    if (priv->complete == complete)
    {
        return;
    }

    priv->complete = complete;
    ai_view_block_changed(self);
}

/**
 * ai_view_block_changed:
 * @self: an #AiViewBlock
 *
 * Invalidates the cached rendering and emits #AiViewBlock::changed.
 *
 * A subclass calls this whenever it mutates. Doing both in one place is
 * deliberate: a subclass that emitted the signal without dropping the cache
 * would have every subscriber re-render and get the previous content back.
 */
void
ai_view_block_changed(AiViewBlock *self)
{
    AiViewBlockPrivate *priv;

    g_return_if_fail(AI_IS_VIEW_BLOCK(self));

    priv = ai_view_block_get_instance_private(self);
    priv->revision++;

    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

/**
 * ai_view_block_render:
 * @self: an #AiViewBlock
 * @width: the width to wrap to in terminal columns, or 0 for no wrapping
 *
 * Renders the block to text and styled spans.
 *
 * Width 0 means "do not wrap", which is what an Emacs frontend wants --- it
 * fills text itself, and pre-wrapped content would fight with that. A
 * terminal passes its column count.
 *
 * The result is cached against @width and the block's revision, so asking
 * repeatedly at one width is cheap and asking after a mutation is correct.
 *
 * Returns: (transfer full): the rendering
 */
AiRenderedText *
ai_view_block_render(
    AiViewBlock *self,
    guint        width
){
    AiViewBlockClass *klass;
    AiViewBlockPrivate *priv;
    g_autoptr(AiRenderedText) raw = NULL;

    g_return_val_if_fail(AI_IS_VIEW_BLOCK(self), NULL);

    priv = ai_view_block_get_instance_private(self);

    if (priv->cached != NULL &&
        priv->cached_width == width &&
        priv->cached_revision == priv->revision)
    {
        return ai_rendered_text_ref(priv->cached);
    }

    klass = AI_VIEW_BLOCK_GET_CLASS(self);

    if (klass->render == NULL)
    {
        return ai_rendered_text_new();
    }

    raw = klass->render(self);

    if (raw == NULL)
    {
        raw = ai_rendered_text_new();
    }

    g_clear_pointer(&priv->cached, ai_rendered_text_unref);
    priv->cached = ai_rendered_text_wrap(raw, width);
    priv->cached_width = width;
    priv->cached_revision = priv->revision;

    return ai_rendered_text_ref(priv->cached);
}

/**
 * ai_view_block_render_expanded:
 * @self: an #AiViewBlock
 * @width: the width to wrap to in terminal columns, or 0 for no wrapping
 *
 * Renders the block as though it were expanded, whatever its expand flag
 * says, and without disturbing that flag.
 *
 * For an export, not for a display. A thinking block and a tool group both
 * collapse to one summary line by default --- which is right on screen and
 * wrong in a file, where the whole point is to keep the record. Rendering a
 * collapsed transcript would produce a document that silently omits every
 * tool call it made.
 *
 * The obvious alternative --- set the flag, render, set it back --- emits
 * #AiViewBlock::changed twice per block, so exporting a live session would
 * make every attached frontend redraw the whole transcript. This neither
 * emits nor caches: the display cache stays keyed to what the display is
 * actually showing.
 *
 * Returns: (transfer full): the rendering
 */
AiRenderedText *
ai_view_block_render_expanded(
    AiViewBlock *self,
    guint        width
){
    AiViewBlockClass *klass;
    AiViewBlockPrivate *priv;
    g_autoptr(AiRenderedText) raw = NULL;
    gboolean saved;

    g_return_val_if_fail(AI_IS_VIEW_BLOCK(self), NULL);

    klass = AI_VIEW_BLOCK_GET_CLASS(self);

    if (klass->render == NULL)
    {
        return ai_rendered_text_new();
    }

    priv = ai_view_block_get_instance_private(self);

    /* Swapped around the vfunc rather than passed into it: the flag is
     * read by every subclass's render, and threading an override through
     * the vtable would change a signature four subclasses implement. */
    saved = priv->expanded;
    priv->expanded = TRUE;
    raw = klass->render(self);
    priv->expanded = saved;

    if (raw == NULL)
    {
        raw = ai_rendered_text_new();
    }

    return ai_rendered_text_wrap(raw, width);
}

/**
 * ai_view_block_render_text:
 * @self: an #AiViewBlock
 * @width: the width to wrap to, or 0 for no wrapping
 *
 * The block's text without its spans, for callers that only want the words.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_view_block_render_text(
    AiViewBlock *self,
    guint        width
){
    g_autoptr(AiRenderedText) rendered = NULL;

    g_return_val_if_fail(AI_IS_VIEW_BLOCK(self), g_strdup(""));

    rendered = ai_view_block_render(self, width);

    return g_strdup(ai_rendered_text_get_text(rendered));
}
