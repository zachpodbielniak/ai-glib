/*
 * ai-gui-chat.h - The transcript, on screen
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include "ai-gui.h"
#include "ai-gui-session.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_CHAT_VIEW (ai_gui_chat_view_get_type())

G_DECLARE_FINAL_TYPE(AiGuiChatView, ai_gui_chat_view, AI_GUI, CHAT_VIEW,
                     GtkWidget)

GtkWidget *
ai_gui_chat_view_new(void);

void
ai_gui_chat_view_set_session(
	AiGuiChatView *self,
	AiGuiSession  *session
);

AiGuiSession *
ai_gui_chat_view_get_session(AiGuiChatView *self);

/**
 * ai_gui_chat_view_set_search:
 * @self: a chat view
 * @needle: (nullable): what to look for, or %NULL to show everything
 *
 * Filters the transcript rather than highlighting inside it.
 *
 * A block is the unit the library hands out and the unit a person
 * reasons about; hiding the ones that do not match keeps the surviving
 * ones whole, where highlighting a substring inside a collapsed tool
 * group would point at text the row is not showing.
 */
void
ai_gui_chat_view_set_search(
	AiGuiChatView *self,
	const gchar   *needle
);

void
ai_gui_chat_view_scroll_to_bottom(AiGuiChatView *self);

/**
 * ai_gui_chat_view_set_expanded_all:
 * @self: a chat view
 * @expanded: whether every block should be expanded
 */
void
ai_gui_chat_view_set_expanded_all(
	AiGuiChatView *self,
	gboolean       expanded
);

G_END_DECLS
