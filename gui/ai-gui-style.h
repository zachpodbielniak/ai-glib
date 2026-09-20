/*
 * ai-gui-style.h - Turning an AiStyleTag into something Pango understands
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The library says "this run of bytes is a tool target". Deciding that
 * means blue, on this display, at this colour scheme, is this file's job
 * and nobody else's -- exactly as ai-tui decides it means a cyan ncurses
 * pair. Neither of them re-derives the structure.
 */

#pragma once

#include <gtk/gtk.h>

#include <ai-glib.h>

G_BEGIN_DECLS

/**
 * ai_gui_style_init:
 *
 * Loads the application stylesheet onto the default display and starts
 * tracking the system colour scheme.
 */
void
ai_gui_style_init(void);

/**
 * ai_gui_style_set_dark:
 * @dark: whether the dark palette is in use
 *
 * Called from the #AdwStyleManager notification. Kept separate from
 * ai_gui_style_init() so a widget can ask for attributes before the
 * first notification has arrived.
 */
void
ai_gui_style_set_dark(gboolean dark);

gboolean
ai_gui_style_get_dark(void);

/**
 * ai_gui_style_attributes:
 * @rendered: a block's rendered text
 *
 * Builds the Pango attributes for @rendered's spans.
 *
 * Span offsets are byte offsets into UTF-8 and #PangoAttribute wants byte
 * indices too, so this is a direct translation with no conversion step
 * to get wrong.
 *
 * Returns: (transfer full): attributes for a #GtkLabel
 */
PangoAttrList *
ai_gui_style_attributes(AiRenderedText *rendered);

/**
 * ai_gui_style_tag_colour:
 * @tag: a style role
 *
 * Returns: (nullable): the hex colour for @tag at the current scheme, or
 *   %NULL when @tag inherits the label's colour
 */
const gchar *
ai_gui_style_tag_colour(AiStyleTag tag);

G_END_DECLS
