/*
 * ai-update-status.h - What "is there an update" means, said once
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API, for the
 * same reason `ai-quota.h` and `ai-theme.h` are not: it is `static
 * inline` throughout, so nothing new is exported and nothing new is
 * introspected.
 *
 * `ai --check-update`, the ai-tui status line and the ai-gui banner all
 * describe the same checkout. Three front-ends that each phrased the
 * result themselves would drift -- one saying "update available" while
 * another says "up to date" about one tree is worse than either saying
 * nothing. So the states, their names on the wire, the sentences, the
 * badge, the refusal wording and the "is a fetch due" policy live here,
 * and nothing in this file knows about ncurses or GTK.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-update-status.h is an internal header"
#endif

#include <glib.h>

G_BEGIN_DECLS

/* Default time between fetches, and the floor a config value is clamped
 * to: an interval of a few seconds would be a fetch per redraw. */
#define AI_UPDATE_DEFAULT_INTERVAL_S (4 * 60 * 60)
#define AI_UPDATE_MIN_INTERVAL_S     (5 * 60)

/* How often a running front-end re-reads the local state. Cheap (a few
 * local git commands) and it is what notices another process having
 * fetched or updated in the meantime; the fetch itself stays on the
 * interval above. */
#define AI_UPDATE_TICK_S (10 * 60)

/*
 * AiUpdateState:
 * @AI_UPDATE_STATE_UNKNOWN: not checked yet
 * @AI_UPDATE_STATE_UP_TO_DATE: this build has everything upstream has
 * @AI_UPDATE_STATE_BEHIND: upstream has commits this build lacks, and
 *   the checkout can take them
 * @AI_UPDATE_STATE_DIVERGED: the checkout and upstream each have commits
 *   the other lacks, so a fast-forward is impossible
 * @AI_UPDATE_STATE_LOCAL_CHANGES: tracked files are modified
 * @AI_UPDATE_STATE_UNAVAILABLE: there is no checkout this build can
 *   update from, or it cannot be read; the detail says why
 */
typedef enum
{
	AI_UPDATE_STATE_UNKNOWN = 0,
	AI_UPDATE_STATE_UP_TO_DATE,
	AI_UPDATE_STATE_BEHIND,
	AI_UPDATE_STATE_DIVERGED,
	AI_UPDATE_STATE_LOCAL_CHANGES,
	AI_UPDATE_STATE_UNAVAILABLE
} AiUpdateState;

/*
 * AiUpdateStatus:
 *
 * One check's answer. @behind counts upstream commits the *running
 * build* lacks, not the checkout: a `git pull` without an install leaves
 * the checkout current and the binary old, and that is still an update.
 * @checkout_behind is the checkout's own distance, which decides whether
 * the pipeline has anything to fast-forward.
 *
 * @pending_restart is set when the state file says this prefix already
 * received upstream's commit and only this process is older -- an update
 * that ran from another window, or from this one a moment ago.
 */
typedef struct
{
	AiUpdateState state;
	guint         behind;
	guint         checkout_behind;
	guint         ahead;
	gboolean      dirty;
	gboolean      pending_restart;
	gboolean      fetch_failed;
	gchar        *upstream;
	gchar        *branch;
	gchar        *source_dir;
	gchar        *build_commit;
	gchar        *head_commit;
	gchar        *upstream_commit;
	gchar        *detail;
	gint64        checked_at;   /* wall-clock seconds */
	gint64        fetched_at;   /* last successful fetch, 0 for never */
	gint64        attempted_at; /* last fetch attempt, successful or not */
} AiUpdateStatus;

static inline void
ai_update_status_free(AiUpdateStatus *status)
{
	if (status == NULL)
		return;
	g_free(status->upstream);
	g_free(status->branch);
	g_free(status->source_dir);
	g_free(status->build_commit);
	g_free(status->head_commit);
	g_free(status->upstream_commit);
	g_free(status->detail);
	g_free(status);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiUpdateStatus, ai_update_status_free)

static inline AiUpdateStatus *
ai_update_status_copy(const AiUpdateStatus *status)
{
	AiUpdateStatus *copy;

	if (status == NULL)
		return NULL;
	copy = g_new0(AiUpdateStatus, 1);
	*copy = *status;
	copy->upstream = g_strdup(status->upstream);
	copy->branch = g_strdup(status->branch);
	copy->source_dir = g_strdup(status->source_dir);
	copy->build_commit = g_strdup(status->build_commit);
	copy->head_commit = g_strdup(status->head_commit);
	copy->upstream_commit = g_strdup(status->upstream_commit);
	copy->detail = g_strdup(status->detail);
	return copy;
}

/* The name on the wire: `ai --check-update --json` and the state file. */
static inline const gchar *
ai_update_state_to_string(AiUpdateState state)
{
	switch (state)
	{
		case AI_UPDATE_STATE_UP_TO_DATE:    return "up-to-date";
		case AI_UPDATE_STATE_BEHIND:        return "behind";
		case AI_UPDATE_STATE_DIVERGED:      return "diverged";
		case AI_UPDATE_STATE_LOCAL_CHANGES: return "local-changes";
		case AI_UPDATE_STATE_UNAVAILABLE:   return "unavailable";
		case AI_UPDATE_STATE_UNKNOWN:
		default:                            return "unknown";
	}
}

static inline AiUpdateState
ai_update_state_from_string(const gchar *name)
{
	gint state;

	for (state = AI_UPDATE_STATE_UP_TO_DATE; state <= AI_UPDATE_STATE_UNAVAILABLE; state++)
	{
		if (g_strcmp0(name, ai_update_state_to_string((AiUpdateState)state)) == 0)
			return (AiUpdateState)state;
	}
	return AI_UPDATE_STATE_UNKNOWN;
}

static inline const gchar *
ai_update_plural(guint n, const gchar *one, const gchar *many)
{
	return n == 1 ? one : many;
}

static inline const gchar *
ai_update_checkout_name(const AiUpdateStatus *status)
{
	return status->source_dir != NULL ? status->source_dir : "the checkout";
}

/*
 * The sentence. Every front-end prints exactly this for a status; the
 * command names in it are the same in all three because /update and
 * `ai --update` are the same pipeline.
 */
static inline gchar *
ai_update_status_dup_summary(const AiUpdateStatus *status)
{
	const gchar *upstream;

	if (status == NULL)
		return g_strdup("Update status not checked yet.");

	upstream = status->upstream != NULL ? status->upstream : "upstream";

	switch (status->state)
	{
		case AI_UPDATE_STATE_UP_TO_DATE:
			if (status->ahead > 0)
				return g_strdup_printf("Up to date with %s (%u local %s not on it).",
				                       upstream, status->ahead,
				                       ai_update_plural(status->ahead, "commit", "commits"));
			return g_strdup_printf("Up to date with %s.", upstream);

		case AI_UPDATE_STATE_BEHIND:
			if (status->pending_restart)
				return g_strdup("Update installed; restart to use it.");
			return g_strdup_printf("Update available: %u %s behind %s. "
			                       "Run `ai --update` or /update.",
			                       status->behind,
			                       ai_update_plural(status->behind, "commit", "commits"),
			                       upstream);

		case AI_UPDATE_STATE_DIVERGED:
			return g_strdup_printf("Diverged from %s: %u local and %u upstream %s. "
			                       "Rebase or reset %s by hand to update.",
			                       upstream, status->ahead, status->checkout_behind,
			                       ai_update_plural(status->checkout_behind, "commit", "commits"),
			                       ai_update_checkout_name(status));

		case AI_UPDATE_STATE_LOCAL_CHANGES:
			if (status->behind > 0)
				return g_strdup_printf("Update available (%u %s behind %s), but %s has "
				                       "local changes. Commit or stash them to update.",
				                       status->behind,
				                       ai_update_plural(status->behind, "commit", "commits"),
				                       upstream, ai_update_checkout_name(status));
			return g_strdup_printf("Up to date with %s; %s has local changes.",
			                       upstream, ai_update_checkout_name(status));

		case AI_UPDATE_STATE_UNAVAILABLE:
			return g_strdup_printf("Updates unavailable: %s",
			                       status->detail != NULL ? status->detail : "unknown reason.");

		case AI_UPDATE_STATE_UNKNOWN:
		default:
			return g_strdup("Update status not checked yet.");
	}
}

/*
 * The few words for a status line or a sidebar, or %NULL when there is
 * nothing to say. Only an update that exists earns a badge: "up to
 * date" on screen all day is noise, and "unavailable" is only news to
 * somebody who asked.
 */
static inline gchar *
ai_update_status_dup_badge(const AiUpdateStatus *status)
{
	if (status == NULL || status->behind == 0)
		return NULL;

	switch (status->state)
	{
		case AI_UPDATE_STATE_BEHIND:
			if (status->pending_restart)
				return g_strdup("restart to update");
			return g_strdup_printf("update available (%u)", status->behind);
		case AI_UPDATE_STATE_DIVERGED:
		case AI_UPDATE_STATE_LOCAL_CHANGES:
			return g_strdup("update available (blocked)");
		case AI_UPDATE_STATE_UNKNOWN:
		case AI_UPDATE_STATE_UP_TO_DATE:
		case AI_UPDATE_STATE_UNAVAILABLE:
		default:
			return NULL;
	}
}

/*
 * Why an update will not run, or %NULL when it may.
 *
 * @busy is the front-end's own answer to "is a turn in flight". An
 * update rebuilds and reinstalls the binaries under a running turn's
 * feet, and in ai-tui it takes the terminal away from it.
 */
static inline gchar *
ai_update_status_dup_refusal(const AiUpdateStatus *status, gboolean busy)
{
	if (busy)
		return g_strdup("A turn is running. Stop it or let it finish, then update.");

	if (status == NULL || status->state == AI_UPDATE_STATE_UNKNOWN)
		return g_strdup("The update status could not be determined.");

	switch (status->state)
	{
		case AI_UPDATE_STATE_BEHIND:
			if (status->pending_restart)
				return g_strdup("The update is already installed; restart to use it.");
			return NULL;
		case AI_UPDATE_STATE_UP_TO_DATE:
			return g_strdup_printf("Already up to date with %s.",
			                       status->upstream != NULL ? status->upstream : "upstream");
		case AI_UPDATE_STATE_DIVERGED:
		case AI_UPDATE_STATE_LOCAL_CHANGES:
		case AI_UPDATE_STATE_UNAVAILABLE:
		case AI_UPDATE_STATE_UNKNOWN:
		default:
			return ai_update_status_dup_summary(status);
	}
}

/*
 * Whether a fetch is due. Measured from the last *attempt*, not the last
 * success: an offline machine would otherwise try on every tick, and a
 * failure must not be retried faster than a success is repeated.
 */
static inline gboolean
ai_update_fetch_due(const AiUpdateStatus *status, gint64 now, guint interval_s)
{
	gint64 last;

	if (status == NULL)
		return TRUE;
	last = MAX(status->attempted_at, status->fetched_at);
	if (last <= 0 || last > now)
		return TRUE;
	return now - last >= (gint64)MAX(interval_s, AI_UPDATE_MIN_INTERVAL_S);
}

G_END_DECLS
