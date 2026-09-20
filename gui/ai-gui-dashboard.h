/*
 * ai-gui-dashboard.h - Every ai-glib session on this machine, at a glance
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

#define AI_GUI_TYPE_DASHBOARD (ai_gui_dashboard_get_type())

G_DECLARE_FINAL_TYPE(AiGuiDashboard, ai_gui_dashboard, AI_GUI, DASHBOARD,
                     GtkWidget)

GtkWidget *
ai_gui_dashboard_new(void);

/**
 * ai_gui_dashboard_set_store:
 * @self: a dashboard
 * @store: (transfer none): this window's own sessions
 *
 * The store is what lets a row be recognised as one of this window's
 * own. Those rows are switched to here rather than focused through
 * tmux, which is the one thing ai-tui's dashboard cannot do for them.
 */
void
ai_gui_dashboard_set_store(
	AiGuiDashboard    *self,
	AiGuiSessionStore *store
);

/**
 * ai_gui_dashboard_set_registry:
 * @self: a dashboard
 * @directory: (nullable): the registry, or %NULL for the default
 */
void
ai_gui_dashboard_set_registry(
	AiGuiDashboard *self,
	const gchar    *directory
);

/**
 * ai_gui_dashboard_refresh:
 * @self: a dashboard
 *
 * Re-reads the registry now.
 */
void
ai_gui_dashboard_refresh(AiGuiDashboard *self);

/**
 * ai_gui_dashboard_set_polling:
 * @self: a dashboard
 * @polling: whether to keep re-reading the registry
 *
 * Polling stops while the dashboard is hidden. A window showing a
 * conversation has no use for a re-read every two seconds, and the
 * records are files: the cost is real and entirely wasted.
 */
void
ai_gui_dashboard_set_polling(
	AiGuiDashboard *self,
	gboolean        polling
);

G_END_DECLS
