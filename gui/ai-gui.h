/*
 * ai-gui.h - Shared declarations for ai-gui, the GTK4 desktop client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * ai-gui is to the desktop what `ai-tui` is to the terminal: a frontend
 * over AiConversation and the view layer, holding no opinion about what a
 * tool is or how a turn is structured.  Everything about what a
 * conversation *means* stays in src/view/ and src/harness/; what lives
 * here is turning an #AiStyleTag into a Pango attribute, putting widgets
 * on a screen, and reading input.
 *
 * The widgets are built in C rather than from .ui files, for the reason
 * clawtilla's client gives: a compiler catches a mistake in this file,
 * where a typo in a .ui file becomes a runtime warning somebody has to be
 * watching for.
 */

#pragma once

#include <adwaita.h>
#include <gtk/gtk.h>

#include <ai-glib.h>

#include "ai-gui-util.h"

G_BEGIN_DECLS

#define AI_GUI_APP_ID "org.copyleft.AiGlib.Gui"


/* ---- Window ---- */

#define AI_GUI_TYPE_WINDOW (ai_gui_window_get_type())

G_DECLARE_FINAL_TYPE(AiGuiWindow, ai_gui_window, AI_GUI, WINDOW,
                     AdwApplicationWindow)

AiGuiWindow *
ai_gui_window_new(
	AdwApplication *app,
	AiGuiOptions   *options
);

void
ai_gui_window_toast(
	AiGuiWindow *self,
	const gchar *format,
	...
) G_GNUC_PRINTF(2, 3);

/**
 * ai_gui_window_show_dashboard:
 * @self: a window
 * @show: whether to show the dashboard instead of the conversation
 *
 * The conversation keeps running behind it, draft and all: this swaps
 * which page is visible and nothing else.
 */
void
ai_gui_window_show_dashboard(
	AiGuiWindow *self,
	gboolean     show
);

gboolean
ai_gui_window_get_dashboard(AiGuiWindow *self);

/**
 * ai_gui_window_open_project:
 * @self: a window
 * @directory: an existing directory
 *
 * Starts a conversation there and switches to it.
 *
 * ai-tui opens a tmux window because it is a terminal program; a session
 * in this window is the desktop equivalent, and it keeps every open
 * conversation one click apart instead of scattered across panes.
 */
void
ai_gui_window_open_project(
	AiGuiWindow *self,
	const gchar *directory
);

/**
 * ai_gui_window_refresh_links:
 * @self: a window
 *
 * Re-reads the current session's linked work into the header button.
 */
void
ai_gui_window_refresh_links(AiGuiWindow *self);

/**
 * ai_gui_window_set_theme:
 * @self: a window
 * @name: a palette name, as `--theme` spells it
 *
 * Applies a palette and remembers it for the next run.
 *
 * Only a choice made *in* the application is remembered. A `--theme` on
 * the command line is for that run: writing it back would make a one-off
 * look at nord the new permanent setting.
 */
void
ai_gui_window_set_theme(
	AiGuiWindow *self,
	const gchar *name
);

/**
 * ai_gui_window_set_color_scheme:
 * @self: a window
 * @name: `system`, `light` or `dark`
 */
void
ai_gui_window_set_color_scheme(
	AiGuiWindow *self,
	const gchar *name
);

/**
 * ai_gui_window_export:
 * @self: a window
 * @arguments: (nullable): `[text|markdown|org] [path]`, as `/export` takes
 *
 * With a path, writes it; without one, opens the file chooser.
 */
void
ai_gui_window_export(
	AiGuiWindow *self,
	const gchar *arguments
);

G_END_DECLS
