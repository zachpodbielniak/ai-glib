/*
 * ai-gui-sidebar.h - The session list
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include "ai-gui.h"
#include "ai-gui-session-store.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_SIDEBAR (ai_gui_sidebar_get_type())

G_DECLARE_FINAL_TYPE(AiGuiSidebar, ai_gui_sidebar, AI_GUI, SIDEBAR, GtkWidget)

GtkWidget *
ai_gui_sidebar_new(AiGuiSessionStore *store);

AiGuiSession *
ai_gui_sidebar_get_selected(AiGuiSidebar *self);

void
ai_gui_sidebar_select(
	AiGuiSidebar *self,
	AiGuiSession *session
);

void
ai_gui_sidebar_focus_search(AiGuiSidebar *self);

G_END_DECLS
