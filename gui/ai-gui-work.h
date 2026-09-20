/*
 * ai-gui-work.h - Project registration, the dashboard's data, and its actions
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The registry an #AiWorkSession writes is shared by every ai-glib
 * front-end on this machine, so ai-gui's dashboard lists ai-tui's
 * sessions and ai-tui's lists ai-gui's. That is why the ordering and the
 * project grouping are reimplemented here to the letter rather than
 * approximated: two dashboards that sorted the same records differently
 * would be two answers to "what needs me next".
 *
 * No GTK. Everything here is a file read, a subprocess or a comparison,
 * and tests/test-ai-gui-work.c links it without a display.
 */

#pragma once

#include <gio/gio.h>

#include <ai-glib.h>

G_BEGIN_DECLS

/**
 * ai_gui_work_priority:
 * @status: a session status string
 *
 * Returns: the dashboard's ordering rank, lowest first
 */
gint
ai_gui_work_priority(const gchar *status);

/**
 * ai_gui_work_compare:
 * @a: (type AiWorkSession): a pointer to a session pointer
 * @b: (type AiWorkSession): a pointer to a session pointer
 *
 * Orders by attention, then project, then id — the same three keys
 * ai-tui's dashboard uses, so both agree about which row is first.
 *
 * Returns: a qsort-style ordering
 */
gint
ai_gui_work_compare(
	gconstpointer a,
	gconstpointer b
);

/**
 * ai_gui_work_list:
 * @directory: (nullable): the registry, or %NULL for the default
 * @error: (out) (optional): where a read failure goes
 *
 * Returns: (transfer full) (element-type AiWorkSession) (nullable): every
 *   recorded session, already sorted
 */
GPtrArray *
ai_gui_work_list(
	const gchar  *directory,
	GError      **error
);

/**
 * ai_gui_work_new_async:
 * @directory: the working directory to identify
 *
 * Builds an #AiWorkSession off the main thread.
 *
 * ai_work_session_new() runs `git rev-parse` twice, and its own
 * documentation says to call it from a worker for UI use. A dashboard
 * that stuttered every time somebody opened a session would be paying
 * that advice no attention.
 */
void
ai_gui_work_new_async(
	const gchar         *directory,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

AiWorkSession *
ai_gui_work_new_finish(
	GAsyncResult  *result,
	GError       **error
);

/**
 * ai_gui_work_is_live:
 * @session: a record read back from the registry
 *
 * Returns: %TRUE unless the record says DISCONNECTED
 */
gboolean
ai_gui_work_is_live(AiWorkSession *session);

/**
 * ai_gui_work_can_resume:
 * @session: a record read back from the registry
 *
 * Returns: %TRUE when the record is disconnected *and* names a native
 *   provider session, which is the only case a resume can honour
 */
gboolean
ai_gui_work_can_resume(AiWorkSession *session);

/**
 * ai_gui_work_open_url:
 * @url: an absolute http(s) URL
 * @error: (out) (optional): where a launch failure goes
 *
 * Opens @url with `xdg-open` on the machine running ai-gui.
 *
 * An explicit user action, never an automatic fetch: a dashboard that
 * opened a browser tab because a row refreshed would be unusable.
 *
 * Returns: %TRUE if the launcher started
 */
gboolean
ai_gui_work_open_url(
	const gchar  *url,
	GError      **error
);

/**
 * ai_gui_work_focus_pane_async:
 * @session: the row to focus
 *
 * Switches tmux to the pane a record names.
 *
 * Used for rows this process does not own — an ai-tui in a terminal
 * somewhere. The row's *own* socket is used rather than this process's,
 * because ai-gui is not inside tmux at all and the pane lives wherever
 * its reporter put it.
 *
 * The pane's published `@ai_session` is checked against the record's id
 * first. A pane id is reused when a pane closes, so switching without
 * that check opens whatever unrelated work took the number.
 */
void
ai_gui_work_focus_pane_async(
	AiWorkSession       *session,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gboolean
ai_gui_work_focus_pane_finish(
	GAsyncResult  *result,
	GError       **error
);

/**
 * ai_gui_work_create_worktree_async:
 * @directory: an existing checkout to branch from
 *
 * Branches `ai/<uuid>` from @directory's HEAD into an adjacent
 * `.ai-worktrees/<name>-<uuid>` checkout.
 *
 * Uncommitted changes in @directory are not copied — `git worktree add`
 * does not carry them, and pretending otherwise would lose work at the
 * moment somebody most expects it to be there.
 */
void
ai_gui_work_create_worktree_async(
	const gchar         *directory,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * ai_gui_work_create_worktree_finish:
 *
 * Returns: (transfer full) (nullable): the new checkout's path
 */
gchar *
ai_gui_work_create_worktree_finish(
	GAsyncResult  *result,
	GError       **error
);

/**
 * ai_gui_work_claim:
 * @directory: the registry directory
 * @id: the session id to claim
 * @error: (out) (optional): where the refusal goes
 *
 * Takes the advisory lock for @id.
 *
 * Two processes resuming one dashboard record would each believe they
 * owned it and overwrite the other's heartbeat, so the record would
 * flicker between two states. ai-tui takes the same lock; the file is
 * shared, so the exclusion is across both front-ends.
 *
 * Returns: the held descriptor, or -1. Close it to release.
 */
gint
ai_gui_work_claim(
	const gchar  *directory,
	const gchar  *id,
	GError      **error
);

G_END_DECLS
