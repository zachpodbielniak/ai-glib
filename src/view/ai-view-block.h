/*
 * ai-view-block.h - One renderable unit of a transcript
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

#include "view/ai-style.h"

G_BEGIN_DECLS

/**
 * AiViewBlockKind:
 * @AI_VIEW_BLOCK_TURN: something the user said
 * @AI_VIEW_BLOCK_TEXT: assistant prose
 * @AI_VIEW_BLOCK_THINKING: reasoning
 * @AI_VIEW_BLOCK_TOOL: a group of tool calls
 * @AI_VIEW_BLOCK_STATUS: a note, a token count, or a failure
 * @AI_VIEW_BLOCK_TODO: the current todo list
 * @AI_VIEW_BLOCK_AGENT: background agents and what they are doing
 *
 * What a block is, for a frontend that would rather switch than downcast.
 */
typedef enum
{
    AI_VIEW_BLOCK_TURN = 0,
    AI_VIEW_BLOCK_TEXT,
    AI_VIEW_BLOCK_THINKING,
    AI_VIEW_BLOCK_TOOL,
    AI_VIEW_BLOCK_STATUS,
    AI_VIEW_BLOCK_TODO,
    AI_VIEW_BLOCK_AGENT
} AiViewBlockKind;

#define AI_TYPE_VIEW_BLOCK (ai_view_block_get_type())

G_DECLARE_DERIVABLE_TYPE(AiViewBlock, ai_view_block, AI, VIEW_BLOCK, GObject)

/**
 * AiViewBlockClass:
 * @parent_class: the parent class
 * @render: produces the block's text and spans, unwrapped
 * @get_kind: what this block is
 * @_reserved: reserved for future expansion
 *
 * Class structure for #AiViewBlock.
 *
 * @render is asked for the block *unwrapped*; wrapping to a width is done
 * once, by the base class, so no subclass has to reimplement it and no two
 * of them can disagree about where a line breaks.
 */
struct _AiViewBlockClass
{
    GObjectClass parent_class;

    AiRenderedText * (*render)   (AiViewBlock *self);
    AiViewBlockKind  (*get_kind) (AiViewBlock *self);

    /*< private >*/
    gpointer _reserved[8];
};

guint64
ai_view_block_get_id(AiViewBlock *self);

AiViewBlockKind
ai_view_block_get_kind(AiViewBlock *self);

gboolean
ai_view_block_get_expanded(AiViewBlock *self);

void
ai_view_block_set_expanded(
    AiViewBlock *self,
    gboolean     expanded
);

gboolean
ai_view_block_get_complete(AiViewBlock *self);

void
ai_view_block_set_complete(
    AiViewBlock *self,
    gboolean     complete
);

AiRenderedText *
ai_view_block_render(
    AiViewBlock *self,
    guint        width
);

AiRenderedText *
ai_view_block_render_expanded(
    AiViewBlock *self,
    guint        width
);

gchar *
ai_view_block_render_text(
    AiViewBlock *self,
    guint        width
);

void
ai_view_block_changed(AiViewBlock *self);

G_END_DECLS
