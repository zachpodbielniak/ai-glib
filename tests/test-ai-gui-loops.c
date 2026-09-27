/*
 * test-ai-gui-loops.c - Loops and goals in ai-gui's session layer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Links gui/ai-gui-session.c and friends directly, without GTK: the
 * window only draws what these decide. HOME, every XDG directory and the
 * working directory are a private sandbox, so the schedule files written
 * here never touch the developer's own.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include <ai-glib.h>

#include "ai-gui-session-store.h"
#include "ai-gui-util.h"

typedef struct
{
	gchar *home;
	gchar *cwd;
	gchar *original_cwd;
} Fixture;

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	fixture->original_cwd = g_get_current_dir();
	fixture->home = g_dir_make_tmp("ai-gui-loops-XXXXXX", NULL);
	g_assert_nonnull(fixture->home);
	fixture->cwd = g_build_filename(fixture->home, "project", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->cwd, 0700), ==, 0);
	g_setenv("HOME", fixture->home, TRUE);
	g_setenv("XDG_DATA_HOME", fixture->home, TRUE);
	g_setenv("XDG_CONFIG_HOME", fixture->home, TRUE);
	g_setenv("XDG_STATE_HOME", fixture->home, TRUE);
	g_unsetenv("AI_LOOP_DISABLE");
	g_assert_cmpint(g_chdir(fixture->cwd), ==, 0);
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", fixture->home);

	g_chdir(fixture->original_cwd);
	g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
	g_free(fixture->original_cwd);
	g_free(fixture->home);
	g_free(fixture->cwd);
}

static AiGuiOptions *
options_for(Fixture *fixture)
{
	AiGuiOptions *options = g_new0(AiGuiOptions, 1);

	options->provider = g_strdup("ollama");
	options->working_directory = g_strdup(fixture->cwd);
	options->max_tokens = 2048;
	options->expand = TRUE;
	return options;
}

static void
drain(AiGuiSession *session)
{
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (ai_gui_session_get_busy(session) && g_get_monotonic_time() < deadline)
		g_main_context_iteration(NULL, FALSE);

	g_assert_false(ai_gui_session_get_busy(session));
}

static gchar *
first_id(AiLoopSchedule *schedule, AiLoopKind kind)
{
	guint i;

	for (i = 0; i < ai_loop_schedule_get_n_tasks(schedule); i++)
		if (ai_loop_schedule_get_kind(schedule, i) == kind)
			return g_strdup(ai_loop_schedule_get_id(schedule, i));

	return NULL;
}

/* The view's edit fields become one /loop or /goal edit line, carrying
 * only what changed, so the parser the slash command uses decides. */
static void
test_edit_line(void)
{
	g_autofree gchar *a = ai_gui_loops_edit_line("1a2b3c4d", FALSE, "10m", "check it", NULL, NULL, NULL);
	g_autofree gchar *b = ai_gui_loops_edit_line("1a2b3c4d", FALSE, "self-paced", NULL, NULL, NULL, NULL);
	g_autofree gchar *c = ai_gui_loops_edit_line("5e6f7a8b", TRUE, NULL, NULL, "30", "4h", "the build is green");
	g_autofree gchar *d = ai_gui_loops_edit_line("5e6f7a8b", TRUE, NULL, NULL, NULL, NULL, NULL);
	g_autofree gchar *e = ai_gui_loops_edit_line("1a2b3c4d", FALSE, "  ", "", NULL, NULL, NULL);
	g_autofree gchar *f = ai_gui_loops_interval_text((gint64)120 * 60 * G_USEC_PER_SEC);
	g_autofree gchar *g = ai_gui_loops_interval_text(0);
	g_autofree gchar *h = ai_gui_loops_interval_text((gint64)45 * 60 * G_USEC_PER_SEC);

	g_assert_cmpstr(a, ==, "edit 1a2b3c4d --every 10m --prompt check it");
	g_assert_cmpstr(b, ==, "edit 1a2b3c4d --self-paced");
	g_assert_cmpstr(c, ==, "edit 5e6f7a8b --turns 30 --time 4h --condition the build is green");
	g_assert_null(d);
	/* An emptied prompt means the maintenance prompt, and says so. */
	g_assert_cmpstr(e, ==, "edit 1a2b3c4d --prompt");
	g_assert_cmpstr(f, ==, "2h");
	g_assert_cmpstr(g, ==, "self-paced");
	g_assert_cmpstr(h, ==, "45m");
}

/* A goal set in the window runs through the session's own send path,
 * judged after each turn, until the model reports it met. */
static void
test_goal_runs_in_session(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiGuiOptions)   options = options_for(fixture);
	g_autoptr(AiGuiSession)   session = ai_gui_session_new(options, "ollama", NULL, NULL);
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(GError)         error = NULL;
	g_autofree gchar         *notice = NULL;
	g_autofree gchar         *id = NULL;
	g_autofree gchar         *summary = NULL;
	AiLoopRunner             *runner = ai_gui_session_get_loops(session);
	AiLoopSchedule           *schedule = ai_loop_runner_get_schedule(runner);
	gint64                    now = g_get_real_time();

	g_assert_true(ai_conversation_set_provider(ai_gui_session_get_conversation(session),
	                                           G_OBJECT(provider), &error));
	/* Replies are consumed in order: the unrelated turn goes first. */
	ai_mock_provider_push_text(provider, "unrelated answer");
	ai_mock_provider_push_text(provider, "Working.\nGOAL_NOT_MET: one left");
	ai_mock_provider_push_text(provider, "Done.\nGOAL_MET: all of them");

	notice = ai_loop_runner_command(runner, "goal", "the list is empty --turns 4", now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(notice);
	id = first_id(schedule, AI_LOOP_KIND_GOAL);
	summary = ai_gui_session_dup_loop_summary(session);
	g_assert_cmpstr(summary, ==, "1 goal, next now");

	/* A turn somebody else started holds the goal back. */
	g_assert_true(ai_gui_session_send(session, "unrelated question", NULL, &error));
	g_assert_false(ai_loop_runner_tick(runner, now));
	drain(session);

	/* The session reports a finish at the real time, so tick at it too. */
	g_assert_true(ai_loop_runner_tick(runner, g_get_real_time()));
	drain(session);
	g_assert_true(ai_loop_runner_tick(runner, g_get_real_time()));
	drain(session);

	{
		gint index = ai_loop_schedule_find(schedule, id);

		g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_MET);
		g_assert_cmpstr(ai_loop_schedule_get_reason(schedule, (guint)index), ==, "all of them");
	}

	g_assert_cmpuint(ai_mock_provider_get_call_count(provider), ==, 3);
	g_assert_false(ai_loop_runner_tick(runner, g_get_real_time()));
}

/* Closing the application and opening it again brings a session's
 * loops back, because the schedule is keyed by the session's own id. */
static void
test_loops_survive_restart(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiGuiOptions) options = options_for(fixture);
	g_autoptr(GError)       error = NULL;
	g_autoptr(JsonNode)     json = NULL;
	g_autoptr(AiGuiSession) restored = NULL;
	g_autofree gchar       *loop_id = NULL;

	{
		AiGuiSession *session = ai_gui_session_new(options, "ollama", NULL, NULL);
		AiLoopRunner *runner = ai_gui_session_get_loops(session);
		gpointer      alive = session;
		gint64        deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

		g_free(ai_loop_runner_command(runner, "loop", "15m check the deploy", g_get_real_time(), &error));
		g_assert_no_error(error);
		loop_id = first_id(ai_loop_runner_get_schedule(runner), AI_LOOP_KIND_LOOP);
		json = ai_gui_session_to_json(session);

		/* The window closing: the project lookup still holds a reference
		 * until it lands, and the claim goes with the last one. */
		g_object_add_weak_pointer(G_OBJECT(session), &alive);
		g_object_unref(session);

		while (alive != NULL && g_get_monotonic_time() < deadline)
			g_main_context_iteration(NULL, FALSE);

		g_assert_null(alive);
	}

	restored = ai_gui_session_new_from_json(json_node_get_object(json), options, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_loop_schedule_find(ai_loop_runner_get_schedule(ai_gui_session_get_loops(restored)),
	                                      loop_id), ==, 0);
}

/* Deleting a session deletes its schedule: nothing it set up fires
 * later, from this window or from a resumed ai-tui. */
static void
test_delete_forgets_loops(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiGuiOptions)      options = options_for(fixture);
	g_autofree gchar            *store_dir = g_build_filename(fixture->home, "gui-sessions", NULL);
	g_autoptr(AiGuiSessionStore) store = ai_gui_session_store_new(store_dir);
	g_autoptr(AiGuiSession)      session = ai_gui_session_new(options, "ollama", NULL, NULL);
	g_autoptr(GError)            error = NULL;
	g_autofree gchar            *path = NULL;
	AiLoopRunner                *runner = ai_gui_session_get_loops(session);

	ai_gui_session_store_add(store, session);
	g_free(ai_loop_runner_command(runner, "goal", "anything at all", g_get_real_time(), &error));
	g_assert_no_error(error);
	path = g_strdup(ai_loop_runner_get_path(runner));
	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));

	g_assert_true(ai_gui_session_store_remove(store, session));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_false(ai_loop_runner_tick(runner, g_get_real_time()));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(ai_loop_runner_get_schedule(runner)), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-gui/loops/edit-line", test_edit_line);
	g_test_add("/ai-gui/loops/goal-runs-in-session", Fixture, NULL,
	           fixture_set_up, test_goal_runs_in_session, fixture_tear_down);
	g_test_add("/ai-gui/loops/survive-restart", Fixture, NULL,
	           fixture_set_up, test_loops_survive_restart, fixture_tear_down);
	g_test_add("/ai-gui/loops/delete-forgets", Fixture, NULL,
	           fixture_set_up, test_delete_forgets_loops, fixture_tear_down);
	return g_test_run();
}
