/*
 * ai-gui-quota.h - What is left of the account, in the header bar
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The same allowances ai-tui prints under the provider in its session
 * panel, from the same cache and the same direction rule
 * (`src/core/ai-quota.h`). A terminal has a sidebar to put them in; a
 * window has a header bar, and a number that is always on screen is what
 * makes somebody notice they are nearly out.
 */

#pragma once

#include "ai-gui.h"
#include "ai-gui-session.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_QUOTA (ai_gui_quota_get_type())

G_DECLARE_FINAL_TYPE(AiGuiQuota, ai_gui_quota, AI_GUI, QUOTA, GtkWidget)

GtkWidget *
ai_gui_quota_new(void);

/**
 * ai_gui_quota_set_session:
 * @self: a quota indicator
 * @session: (nullable): the session whose provider to report on
 *
 * Switching sessions invalidates the snapshot rather than relabelling
 * it: an allowance is scoped to a provider, a model and a directory, and
 * showing the previous session's under this one's heading would be
 * attributing somebody else's quota to this conversation.
 */
void
ai_gui_quota_set_session(
	AiGuiQuota   *self,
	AiGuiSession *session
);

/**
 * ai_gui_quota_set_active:
 * @self: a quota indicator
 * @active: whether the window is in front of somebody
 *
 * Polling stops with the window. A report is a CLI subprocess, and a
 * window sitting behind a browser has no business spawning one every
 * minute — the same rule ai-tui applies to a hidden panel.
 */
void
ai_gui_quota_set_active(
	AiGuiQuota *self,
	gboolean    active
);

/**
 * ai_gui_quota_shutdown:
 * @self: a quota indicator
 *
 * Cancels and drains reporting work.
 *
 * Called before the window's own teardown rather than left to dispose,
 * because draining iterates the main context and a half-disposed window
 * is not somewhere to re-enter.
 */
void
ai_gui_quota_shutdown(AiGuiQuota *self);

G_END_DECLS
