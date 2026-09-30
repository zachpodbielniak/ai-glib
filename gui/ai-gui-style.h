/*
 * ai-gui-style.h - Turning an AiStyleTag into something Pango understands
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The library says "this run of bytes is a tool target". Which palette
 * entry that means is decided once, in src/core/ai-theme.h, so the
 * window and the terminal agree; what is left here is turning that
 * entry into Pango attributes and libadwaita named colours.
 */

#pragma once

#include <gtk/gtk.h>

#include <ai-glib.h>

G_BEGIN_DECLS

/**
 * ai_gui_style_init:
 * @theme: (nullable): the palette to start in, or %NULL for the default
 * @color_scheme: (nullable): `system`, `light` or `dark`
 *
 * Loads the application stylesheet onto the default display and applies
 * the starting appearance.
 */
void
ai_gui_style_init(
	const gchar *theme,
	const gchar *color_scheme
);

/**
 * ai_gui_style_set_theme:
 * @name: a palette name, as `--theme` spells it
 *
 * Recolours every open window.
 *
 * A named palette is applied by overriding libadwaita's own colours
 * rather than by styling each widget: that is how a libadwaita
 * application is meant to be recoloured, and it is the difference
 * between a themed transcript inside default-blue chrome and a window
 * that is one thing.
 *
 * Returns: %FALSE if @name is not a palette this build has
 */
gboolean
ai_gui_style_set_theme(const gchar *name);

/**
 * ai_gui_style_get_theme:
 *
 * Returns: (transfer none): the current palette's name
 */
const gchar *
ai_gui_style_get_theme(void);

/**
 * ai_gui_style_cycle_theme:
 *
 * Moves to the next palette, wrapping — the same thing `^T` does in
 * ai-tui, and for the same reason: trying them is faster than reading
 * about them.
 *
 * Returns: (transfer none): the new palette's name
 */
const gchar *
ai_gui_style_cycle_theme(void);

/**
 * ai_gui_style_set_color_scheme:
 * @name: `system`, `light` or `dark`
 *
 * Chooses light or dark, or follows the desktop.
 *
 * Ignored while a palette that names its own colours is active: a dark
 * palette rendered in light chrome is not a light theme, it is a broken
 * one. The preference is remembered and applies again the moment the
 * palette goes back to `terminal` or `monochrome`.
 *
 * Returns: %FALSE if @name is not one of the three
 */
gboolean
ai_gui_style_set_color_scheme(const gchar *name);

/**
 * ai_gui_style_get_color_scheme:
 *
 * Returns: (transfer none): the requested scheme, not the resolved one
 */
const gchar *
ai_gui_style_get_color_scheme(void);

/**
 * AiGuiStyleChangedFunc:
 * @user_data: what was registered with it
 *
 * Called when the palette or the resolved light/dark has changed.
 */
typedef void (*AiGuiStyleChangedFunc)(gpointer user_data);

/**
 * ai_gui_style_add_changed:
 * @func: what to call
 * @user_data: passed back
 *
 * Registers interest in appearance changes.
 *
 * Widget chrome follows the stylesheet on its own, but the transcript
 * does not: a span's colour is a #PangoAttribute baked into a label
 * when the block was last rendered, and nothing invalidates it. Without
 * this the window recolours around a transcript still drawn in the
 * previous palette --- light-lavender prose on a latte page.
 *
 * It also covers the case no explicit call could: the desktop switching
 * to dark at sunset while `terminal` is the palette.
 */
void
ai_gui_style_add_changed(
	AiGuiStyleChangedFunc func,
	gpointer              user_data
);

void
ai_gui_style_remove_changed(
	AiGuiStyleChangedFunc func,
	gpointer              user_data
);

/**
 * ai_gui_style_attributes:
 * @rendered: a block's rendered text
 *
 * Builds the Pango attributes for @rendered's spans.
 *
 * Span offsets are byte offsets into UTF-8 and #PangoAttribute wants
 * byte indices too, so this is a direct translation with no conversion
 * step to get wrong.
 *
 * Returns: (transfer full): attributes for a #GtkLabel
 */
PangoAttrList *
ai_gui_style_attributes(AiRenderedText *rendered);

/**
 * ai_gui_style_tag_colour:
 * @tag: a style role
 * @out_rgba: (out caller-allocates): where the colour goes
 *
 * Returns: %FALSE when @tag inherits the label's colour, which is what
 *   every tag does under `monochrome`
 */
gboolean
ai_gui_style_tag_colour(
	AiStyleTag  tag,
	GdkRGBA    *out_rgba
);

G_END_DECLS
