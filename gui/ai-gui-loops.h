/*
 * ai-gui-loops.h - See and edit a session's loops and goals
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

void
ai_gui_loops_present(
	GtkWidget    *parent,
	AiGuiSession *session
);

G_END_DECLS
