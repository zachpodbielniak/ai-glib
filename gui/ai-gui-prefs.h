/*
 * ai-gui-prefs.h - Everything the command line can say, as a dialog
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
 * ai_gui_prefs_present:
 * @parent: the widget to present over
 * @session: the session being configured
 *
 * Shows the preferences for @session.
 *
 * The provider page is built by walking the provider's own writable
 * #GParamSpec list, exactly as `ai --set` resolves a name. That is what
 * makes a new provider knob reachable here the moment it is added: a
 * property is the interface, and nothing in this file names one.
 */
void
ai_gui_prefs_present(
	GtkWidget    *parent,
	AiGuiSession *session
);

G_END_DECLS
