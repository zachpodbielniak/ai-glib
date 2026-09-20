/*
 * ai-gui-agents.h - What the background agents are doing
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

/**
 * ai_gui_agents_present:
 * @parent: the widget to present over
 * @session: the session whose brigade to show
 *
 * Shows the brigade, with a way to stop an agent.
 *
 * It does not show what an agent *said*. Collecting a result is
 * ai_brigade_reap(), and the model's own `agent_result` tool reaps too --
 * whichever asks first gets it, so a panel that displayed results would
 * be taking them out of the model's hands to satisfy curiosity.
 */
void
ai_gui_agents_present(
	GtkWidget    *parent,
	AiGuiSession *session
);

G_END_DECLS
