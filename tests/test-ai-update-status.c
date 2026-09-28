/*
 * test-ai-update-status.c - The shared update vocabulary
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ai-update-status.h is what makes `ai --check-update`, the ai-tui
 * status line and the ai-gui banner say the same thing. These assert the
 * wording literally: a change here is a change in three front-ends, and
 * it should be made on purpose.
 */

#include <glib.h>

#include "core/ai-update-status.h"

static AiUpdateStatus *
status_new(AiUpdateState state, guint behind)
{
	AiUpdateStatus *status = g_new0(AiUpdateStatus, 1);

	status->state = state;
	status->behind = behind;
	status->checkout_behind = behind;
	status->upstream = g_strdup("origin/master");
	status->source_dir = g_strdup("/src/ai-glib");
	return status;
}

static void
test_state_names_round_trip(void)
{
	gint state;

	for (state = AI_UPDATE_STATE_UNKNOWN; state <= AI_UPDATE_STATE_UNAVAILABLE; state++)
		g_assert_cmpint(ai_update_state_from_string(ai_update_state_to_string((AiUpdateState)state)), ==, state);

	g_assert_cmpstr(ai_update_state_to_string(AI_UPDATE_STATE_UP_TO_DATE), ==, "up-to-date");
	g_assert_cmpstr(ai_update_state_to_string(AI_UPDATE_STATE_BEHIND), ==, "behind");
	g_assert_cmpstr(ai_update_state_to_string(AI_UPDATE_STATE_DIVERGED), ==, "diverged");
	g_assert_cmpstr(ai_update_state_to_string(AI_UPDATE_STATE_LOCAL_CHANGES), ==, "local-changes");
	g_assert_cmpstr(ai_update_state_to_string(AI_UPDATE_STATE_UNAVAILABLE), ==, "unavailable");
	g_assert_cmpint(ai_update_state_from_string("nonsense"), ==, AI_UPDATE_STATE_UNKNOWN);
	g_assert_cmpint(ai_update_state_from_string(NULL), ==, AI_UPDATE_STATE_UNKNOWN);
}

static void
test_summaries(void)
{
	g_autoptr(AiUpdateStatus) behind = status_new(AI_UPDATE_STATE_BEHIND, 3);
	g_autoptr(AiUpdateStatus) one = status_new(AI_UPDATE_STATE_BEHIND, 1);
	g_autoptr(AiUpdateStatus) current = status_new(AI_UPDATE_STATE_UP_TO_DATE, 0);
	g_autoptr(AiUpdateStatus) diverged = status_new(AI_UPDATE_STATE_DIVERGED, 2);
	g_autoptr(AiUpdateStatus) dirty = status_new(AI_UPDATE_STATE_LOCAL_CHANGES, 4);
	g_autoptr(AiUpdateStatus) gone = status_new(AI_UPDATE_STATE_UNAVAILABLE, 0);
	g_autofree gchar *text = NULL;

	text = ai_update_status_dup_summary(behind);
	g_assert_cmpstr(text, ==, "Update available: 3 commits behind origin/master. "
	                          "Run `ai --update` or /update.");
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_summary(one);
	g_assert_cmpstr(text, ==, "Update available: 1 commit behind origin/master. "
	                          "Run `ai --update` or /update.");
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_summary(current);
	g_assert_cmpstr(text, ==, "Up to date with origin/master.");
	g_clear_pointer(&text, g_free);

	current->ahead = 2;
	text = ai_update_status_dup_summary(current);
	g_assert_cmpstr(text, ==, "Up to date with origin/master (2 local commits not on it).");
	g_clear_pointer(&text, g_free);

	diverged->ahead = 1;
	text = ai_update_status_dup_summary(diverged);
	g_assert_cmpstr(text, ==, "Diverged from origin/master: 1 local and 2 upstream commits. "
	                          "Rebase or reset /src/ai-glib by hand to update.");
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_summary(dirty);
	g_assert_cmpstr(text, ==, "Update available (4 commits behind origin/master), but "
	                          "/src/ai-glib has local changes. Commit or stash them to update.");
	g_clear_pointer(&text, g_free);

	gone->detail = g_strdup("/src/ai-glib is not a git checkout.");
	text = ai_update_status_dup_summary(gone);
	g_assert_cmpstr(text, ==, "Updates unavailable: /src/ai-glib is not a git checkout.");
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_summary(NULL);
	g_assert_cmpstr(text, ==, "Update status not checked yet.");
	g_clear_pointer(&text, g_free);

	behind->pending_restart = TRUE;
	text = ai_update_status_dup_summary(behind);
	g_assert_cmpstr(text, ==, "Update installed; restart to use it.");
}

/* Only an update that exists earns space on a status line. */
static void
test_badges(void)
{
	g_autoptr(AiUpdateStatus) behind = status_new(AI_UPDATE_STATE_BEHIND, 3);
	g_autoptr(AiUpdateStatus) current = status_new(AI_UPDATE_STATE_UP_TO_DATE, 0);
	g_autoptr(AiUpdateStatus) dirty = status_new(AI_UPDATE_STATE_LOCAL_CHANGES, 4);
	g_autoptr(AiUpdateStatus) clean_dirty = status_new(AI_UPDATE_STATE_LOCAL_CHANGES, 0);
	g_autoptr(AiUpdateStatus) gone = status_new(AI_UPDATE_STATE_UNAVAILABLE, 0);
	g_autofree gchar *badge = NULL;

	badge = ai_update_status_dup_badge(behind);
	g_assert_cmpstr(badge, ==, "update available (3)");
	g_clear_pointer(&badge, g_free);

	badge = ai_update_status_dup_badge(dirty);
	g_assert_cmpstr(badge, ==, "update available (blocked)");
	g_clear_pointer(&badge, g_free);

	behind->pending_restart = TRUE;
	badge = ai_update_status_dup_badge(behind);
	g_assert_cmpstr(badge, ==, "restart to update");
	g_clear_pointer(&badge, g_free);

	g_assert_null(ai_update_status_dup_badge(current));
	g_assert_null(ai_update_status_dup_badge(clean_dirty));
	g_assert_null(ai_update_status_dup_badge(gone));
	g_assert_null(ai_update_status_dup_badge(NULL));
}

static void
test_refusals(void)
{
	g_autoptr(AiUpdateStatus) behind = status_new(AI_UPDATE_STATE_BEHIND, 3);
	g_autoptr(AiUpdateStatus) current = status_new(AI_UPDATE_STATE_UP_TO_DATE, 0);
	g_autoptr(AiUpdateStatus) dirty = status_new(AI_UPDATE_STATE_LOCAL_CHANGES, 4);
	g_autoptr(AiUpdateStatus) diverged = status_new(AI_UPDATE_STATE_DIVERGED, 2);
	g_autofree gchar *text = NULL;

	g_assert_null(ai_update_status_dup_refusal(behind, FALSE));

	text = ai_update_status_dup_refusal(behind, TRUE);
	g_assert_cmpstr(text, ==, "A turn is running. Stop it or let it finish, then update.");
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_refusal(current, FALSE);
	g_assert_cmpstr(text, ==, "Already up to date with origin/master.");
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_refusal(dirty, FALSE);
	g_assert_true(g_str_has_suffix(text, "Commit or stash them to update."));
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_refusal(diverged, FALSE);
	g_assert_true(g_str_has_prefix(text, "Diverged from origin/master"));
	g_clear_pointer(&text, g_free);

	text = ai_update_status_dup_refusal(NULL, FALSE);
	g_assert_nonnull(text);
	g_clear_pointer(&text, g_free);

	behind->pending_restart = TRUE;
	text = ai_update_status_dup_refusal(behind, FALSE);
	g_assert_cmpstr(text, ==, "The update is already installed; restart to use it.");
}

/* The throttle is measured from the last attempt, success or not. */
static void
test_fetch_due(void)
{
	g_autoptr(AiUpdateStatus) status = status_new(AI_UPDATE_STATE_UP_TO_DATE, 0);
	gint64 now = 1000000;

	g_assert_true(ai_update_fetch_due(NULL, now, 3600));
	g_assert_true(ai_update_fetch_due(status, now, 3600));

	status->attempted_at = now - 60;
	g_assert_false(ai_update_fetch_due(status, now, 3600));

	status->attempted_at = now - 3600;
	g_assert_true(ai_update_fetch_due(status, now, 3600));

	/* A failed attempt counts: no retry storm while offline. */
	status->fetched_at = now - 100000;
	status->attempted_at = now - 10;
	g_assert_false(ai_update_fetch_due(status, now, 3600));

	/* The interval has a floor. */
	status->fetched_at = 0;
	status->attempted_at = now - 30;
	g_assert_false(ai_update_fetch_due(status, now, 1));
	status->attempted_at = now - AI_UPDATE_MIN_INTERVAL_S;
	g_assert_true(ai_update_fetch_due(status, now, 1));

	/* A clock that went backwards is not a reason to wait forever. */
	status->attempted_at = now + 5000;
	g_assert_true(ai_update_fetch_due(status, now, 3600));
}

static void
test_copy(void)
{
	g_autoptr(AiUpdateStatus) status = status_new(AI_UPDATE_STATE_BEHIND, 3);
	g_autoptr(AiUpdateStatus) copy = NULL;

	status->detail = g_strdup("detail");
	copy = ai_update_status_copy(status);
	g_assert_true(copy != status);
	g_assert_true(copy->upstream != status->upstream);
	g_assert_cmpstr(copy->upstream, ==, "origin/master");
	g_assert_cmpstr(copy->detail, ==, "detail");
	g_assert_cmpuint(copy->behind, ==, 3);
	g_assert_null(ai_update_status_copy(NULL));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/update-status/state-names", test_state_names_round_trip);
	g_test_add_func("/ai-glib/update-status/summaries", test_summaries);
	g_test_add_func("/ai-glib/update-status/badges", test_badges);
	g_test_add_func("/ai-glib/update-status/refusals", test_refusals);
	g_test_add_func("/ai-glib/update-status/fetch-due", test_fetch_due);
	g_test_add_func("/ai-glib/update-status/copy", test_copy);

	return g_test_run();
}
