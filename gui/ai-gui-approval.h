/*
 * ai-gui-approval.h - Asking a person whether a tool may run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include "ai-gui.h"

G_BEGIN_DECLS

/**
 * ai_gui_approval_ask:
 * @parent: (nullable): the window to present over
 * @tool_use: the call awaiting an answer
 *
 * Shows the call and waits for an answer.
 *
 * Blocks by spinning a nested #GMainLoop on
 * g_main_context_get_thread_default(), never the global default: the run
 * this is answering for is driving a private context, and a loop on the
 * wrong one would never dispatch the source that makes progress. That is
 * the rule the library states for any approval handler asking a human,
 * and it is the whole reason this lives in a function of its own.
 *
 * Returns: what the person chose
 */
AiToolApproval
ai_gui_approval_ask(
	GtkWindow *parent,
	AiToolUse *tool_use
);

G_END_DECLS
