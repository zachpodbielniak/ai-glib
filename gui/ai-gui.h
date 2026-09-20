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

G_END_DECLS
