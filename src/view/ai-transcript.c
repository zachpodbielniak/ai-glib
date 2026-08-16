/*
 * ai-transcript.c - The buffer: an ordered, observable list of blocks
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "view/ai-transcript.h"

struct _AiTranscript
{
    GObject    parent_instance;
    GPtrArray *blocks;   /* AiViewBlock, owned */
};

static void ai_transcript_list_model_init(GListModelInterface *iface);

/*
 * GListModel rather than a bare GList, which is what the rest of ai-glib
 * uses for collections.
 *
 * Two reasons. ::items-changed gives insertion and removal notification for
 * free, in the form every GLib consumer already knows. And a stable integer
 * position is exactly what a frontend that maps blocks onto its own
 * addressing -- regions of an Emacs buffer, rows of a pad -- needs to key
 * on. A GList would have neither.
 */
G_DEFINE_TYPE_WITH_CODE(AiTranscript, ai_transcript, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              ai_transcript_list_model_init))

enum
{
    SIGNAL_BLOCK_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_block_changed(
    AiViewBlock *block,
    gpointer     user_data
){
    AiTranscript *self = user_data;
    guint position = 0;

    /*
     * The position is looked up rather than captured at connect time,
     * because a block's position moves when earlier ones are removed and a
     * stale index would update the wrong region.
     */
    if (ai_transcript_find_block(self, block, &position))
    {
        g_signal_emit(self, signals[SIGNAL_BLOCK_CHANGED], 0, position, block);
    }
}

static GType
ai_transcript_get_item_type(GListModel *model)
{
    (void)model;
    return AI_TYPE_VIEW_BLOCK;
}

static guint
ai_transcript_get_n_items(GListModel *model)
{
    return AI_TRANSCRIPT(model)->blocks->len;
}

static gpointer
ai_transcript_get_item(GListModel *model, guint position)
{
    AiTranscript *self = AI_TRANSCRIPT(model);

    if (position >= self->blocks->len)
    {
        return NULL;
    }

    return g_object_ref(g_ptr_array_index(self->blocks, position));
}

static void
ai_transcript_list_model_init(GListModelInterface *iface)
{
    iface->get_item_type = ai_transcript_get_item_type;
    iface->get_n_items = ai_transcript_get_n_items;
    iface->get_item = ai_transcript_get_item;
}

static void
ai_transcript_finalize(GObject *object)
{
    AiTranscript *self = AI_TRANSCRIPT(object);
    guint i;

    for (i = 0; i < self->blocks->len; i++)
    {
        g_signal_handlers_disconnect_by_data(
            g_ptr_array_index(self->blocks, i), self);
    }

    g_clear_pointer(&self->blocks, g_ptr_array_unref);

    G_OBJECT_CLASS(ai_transcript_parent_class)->finalize(object);
}

static void
ai_transcript_class_init(AiTranscriptClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_transcript_finalize;

    /**
     * AiTranscript::block-changed:
     * @self: the transcript
     * @position: where the block is
     * @block: the block that changed
     *
     * Emitted when a block's content changed in place.
     *
     * #GListModel::items-changed covers insertion and removal, but a block
     * being streamed into is neither --- prose grows a delta at a time and a
     * tool group's calls change state, all without the list changing shape.
     * A frontend that only watched items-changed would show the first delta
     * of a reply and nothing after it.
     *
     * The position is current as of the emission; act on it before
     * returning, or look the block up again by id.
     */
    signals[SIGNAL_BLOCK_CHANGED] =
        g_signal_new("block-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 2,
                     G_TYPE_UINT, AI_TYPE_VIEW_BLOCK);
}

static void
ai_transcript_init(AiTranscript *self)
{
    self->blocks = g_ptr_array_new_with_free_func(g_object_unref);
}

/**
 * ai_transcript_new:
 *
 * Creates an empty transcript.
 *
 * Returns: (transfer full): a new #AiTranscript
 */
AiTranscript *
ai_transcript_new(void)
{
    return g_object_new(AI_TYPE_TRANSCRIPT, NULL);
}

/**
 * ai_transcript_append:
 * @self: an #AiTranscript
 * @block: (transfer none): the block to add
 *
 * Adds @block to the end, taking a reference.
 *
 * The transcript subscribes to the block's ::changed so it can relay it as
 * ::block-changed with a position. A frontend therefore only ever has to
 * watch the transcript, never the individual blocks.
 */
void
ai_transcript_append(
    AiTranscript *self,
    AiViewBlock  *block
){
    guint position;

    g_return_if_fail(AI_IS_TRANSCRIPT(self));
    g_return_if_fail(AI_IS_VIEW_BLOCK(block));

    position = self->blocks->len;

    g_ptr_array_add(self->blocks, g_object_ref(block));
    g_signal_connect(block, "changed", G_CALLBACK(on_block_changed), self);

    g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
}

/**
 * ai_transcript_get_block:
 * @self: an #AiTranscript
 * @position: which block
 *
 * Like g_list_model_get_item() but without taking a reference, for the
 * common case of looking at a block and being done with it.
 *
 * Returns: (transfer none) (nullable): the block, or %NULL if out of range
 */
AiViewBlock *
ai_transcript_get_block(
    AiTranscript *self,
    guint         position
){
    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), NULL);

    if (position >= self->blocks->len)
    {
        return NULL;
    }

    return g_ptr_array_index(self->blocks, position);
}

/**
 * ai_transcript_get_last:
 * @self: an #AiTranscript
 *
 * The most recent block.
 *
 * This is what the event folding keys on: whether a text delta continues
 * the current prose or starts a new one after a tool group is decided by
 * looking at what is currently last.
 *
 * Returns: (transfer none) (nullable): the last block, or %NULL when empty
 */
AiViewBlock *
ai_transcript_get_last(AiTranscript *self)
{
    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), NULL);

    if (self->blocks->len == 0)
    {
        return NULL;
    }

    return g_ptr_array_index(self->blocks, self->blocks->len - 1);
}

/**
 * ai_transcript_get_n_blocks:
 * @self: an #AiTranscript
 *
 * Returns: how many blocks there are
 */
guint
ai_transcript_get_n_blocks(AiTranscript *self)
{
    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), 0);

    return self->blocks->len;
}

/**
 * ai_transcript_find_block:
 * @self: an #AiTranscript
 * @block: (transfer none): the block to locate
 * @out_position: (out) (optional): where it is
 *
 * Finds a block's current position.
 *
 * Returns: %TRUE if @block is in this transcript
 */
gboolean
ai_transcript_find_block(
    AiTranscript *self,
    AiViewBlock  *block,
    guint        *out_position
){
    guint i;

    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), FALSE);

    for (i = 0; i < self->blocks->len; i++)
    {
        if (g_ptr_array_index(self->blocks, i) == block)
        {
            if (out_position != NULL)
            {
                *out_position = i;
            }

            return TRUE;
        }
    }

    return FALSE;
}

/**
 * ai_transcript_clear:
 * @self: an #AiTranscript
 *
 * Removes every block.
 *
 * Handlers are disconnected first: a block a caller still holds a reference
 * to may go on changing, and it must not still be reporting those changes
 * to a transcript it is no longer part of.
 */
void
ai_transcript_clear(AiTranscript *self)
{
    guint removed;
    guint i;

    g_return_if_fail(AI_IS_TRANSCRIPT(self));

    removed = self->blocks->len;

    if (removed == 0)
    {
        return;
    }

    for (i = 0; i < self->blocks->len; i++)
    {
        g_signal_handlers_disconnect_by_data(
            g_ptr_array_index(self->blocks, i), self);
    }

    g_ptr_array_set_size(self->blocks, 0);
    g_list_model_items_changed(G_LIST_MODEL(self), 0, removed, 0);
}

/**
 * ai_transcript_to_text:
 * @self: an #AiTranscript
 * @width: the width to wrap to, or 0 for no wrapping
 *
 * The whole transcript as plain text, one blank line between blocks.
 *
 * What makes the layer testable and pipeable: `ai-tui --dump` is this
 * function and a printf. It is also the fallback for a frontend that cannot
 * do styling at all.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_transcript_to_text(
    AiTranscript *self,
    guint         width
){
    g_autoptr(GString) out = NULL;
    guint i;

    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), g_strdup(""));

    out = g_string_new(NULL);

    for (i = 0; i < self->blocks->len; i++)
    {
        AiViewBlock *block = g_ptr_array_index(self->blocks, i);
        g_autofree gchar *text = ai_view_block_render_text(block, width);

        if (text == NULL || text[0] == '\0')
        {
            continue;
        }

        if (out->len > 0)
        {
            g_string_append(out, "\n\n");
        }

        g_string_append(out, text);
    }

    if (out->len > 0)
    {
        g_string_append_c(out, '\n');
    }

    return g_strdup(out->str);
}
