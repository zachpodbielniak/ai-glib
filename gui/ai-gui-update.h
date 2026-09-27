/*
 * ai-gui-update.h - The update banner's state, and /update, for ai-gui
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * No GTK. The window maps ::changed onto its banner and ::message onto
 * the transcript; everything with a decision in it is here, where
 * tests/test-ai-gui-update.c links it without a display.
 *
 * ai-gui has no terminal, so a run never uses sudo. When the prefix
 * needs privilege the install step alone goes through pkexec, whose
 * desktop agent asks in a dialog; with no agent, or a dismissed dialog,
 * it builds, stops, and says the one command to run.
 */

#pragma once

#include <gio/gio.h>

#include <ai-glib.h>
#include "core/ai-updater.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_UPDATE (ai_gui_update_get_type())

G_DECLARE_FINAL_TYPE(AiGuiUpdate, ai_gui_update, AI_GUI, UPDATE, GObject)

typedef enum
{
	AI_GUI_UPDATE_ACTION_NONE,
	AI_GUI_UPDATE_ACTION_UPDATE,
	AI_GUI_UPDATE_ACTION_RESTART
} AiGuiUpdateAction;

AiGuiUpdate *
ai_gui_update_new(AiUpdater *updater);

void
ai_gui_update_start(AiGuiUpdate *self);

void
ai_gui_update_shutdown(AiGuiUpdate *self);

void
ai_gui_update_drain(AiGuiUpdate *self);

gboolean
ai_gui_update_get_busy(AiGuiUpdate *self);

gchar *
ai_gui_update_dup_banner(AiGuiUpdate *self);

void
ai_gui_update_check(AiGuiUpdate *self);

void
ai_gui_update_run(AiGuiUpdate *self, gboolean turn_running);

AiGuiUpdateAction
ai_gui_update_get_action(AiGuiUpdate *self, gboolean turn_running, gboolean *sensitive);

gchar *
ai_gui_update_dup_restart_path(AiGuiUpdate *self);

G_END_DECLS
