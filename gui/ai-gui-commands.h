/*
 * ai-gui-commands.h - Built-in slash commands, in the desktop client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The library resolves a line and hands back an #AiCommandResult saying
 * "this one is yours". Nothing is sent for a built-in, so a frontend
 * that ignored the result would swallow every `/help` in silence --
 * which is what ai-gui did before this file existed.
 */

#pragma once

#include "ai-gui.h"
#include "ai-gui-session.h"

G_BEGIN_DECLS

/**
 * ai_gui_commands_handle:
 * @window: the window the command was typed in
 * @session: the session it was typed at
 * @command: the resolved built-in
 *
 * Runs a built-in and writes what it has to say into the transcript.
 *
 * The transcript rather than a toast: `/help` and `/links` are output,
 * and output that disappears after three seconds is output somebody has
 * to ask for twice.
 */
void
ai_gui_commands_handle(
	AiGuiWindow     *window,
	AiGuiSession    *session,
	AiCommandResult *command
);

/**
 * ai_gui_commands_say:
 * @session: the session to write into
 * @format: a printf format
 *
 * Appends a completed status block to @session's transcript.
 */
void
ai_gui_commands_say(
	AiGuiSession *session,
	const gchar  *format,
	...
) G_GNUC_PRINTF(2, 3);

G_END_DECLS
