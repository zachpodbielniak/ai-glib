/*
 * ai-gui-session-store.h - The sessions on disk and in the window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include <gio/gio.h>

#include "ai-gui-session.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_SESSION_STORE (ai_gui_session_store_get_type())

G_DECLARE_FINAL_TYPE(AiGuiSessionStore, ai_gui_session_store,
                     AI_GUI, SESSION_STORE, GObject)

/**
 * ai_gui_session_store_new:
 * @directory: (nullable): where sessions live, or %NULL for the default
 *
 * A directory rather than a fixed path so a test can point the store
 * somewhere of its own. A suite that wrote into the developer's real
 * `~/.local/share` would pass or fail by whose machine ran it.
 *
 * Returns: (transfer full): the store
 */
AiGuiSessionStore *
ai_gui_session_store_new(const gchar *directory);

const gchar *
ai_gui_session_store_get_directory(AiGuiSessionStore *self);

/**
 * ai_gui_session_store_load:
 * @self: a store
 * @options: the options a restored session inherits
 *
 * Reads every saved session, newest first.
 *
 * A file that will not parse costs itself and nothing else: one bad
 * session must not hide sixteen good ones. Same rule the harness layer
 * applies to resource files, and for the same reason -- these are files
 * on disk that somebody else's crash may have truncated.
 *
 * Returns: how many sessions were read
 */
guint
ai_gui_session_store_load(
	AiGuiSessionStore  *self,
	const AiGuiOptions *options
);

void
ai_gui_session_store_add(
	AiGuiSessionStore *self,
	AiGuiSession      *session
);

gboolean
ai_gui_session_store_remove(
	AiGuiSessionStore *self,
	AiGuiSession      *session
);

gboolean
ai_gui_session_store_find(
	AiGuiSessionStore *self,
	AiGuiSession      *session,
	guint             *out_position
);

AiGuiSession *
ai_gui_session_store_get(
	AiGuiSessionStore *self,
	guint              position
);

guint
ai_gui_session_store_get_n_sessions(AiGuiSessionStore *self);

/**
 * ai_gui_session_store_save:
 * @self: a store
 * @session: the session to write
 * @error: (out) (optional): where a write failure goes
 *
 * Returns: %TRUE on success
 */
gboolean
ai_gui_session_store_save(
	AiGuiSessionStore  *self,
	AiGuiSession       *session,
	GError            **error
);

/**
 * ai_gui_session_store_save_all:
 * @self: a store
 *
 * Writes every session, reporting failures to the log rather than to the
 * caller: this runs at shutdown, where there is nobody left to tell.
 *
 * Returns: how many were written
 */
guint
ai_gui_session_store_save_all(AiGuiSessionStore *self);

G_END_DECLS
