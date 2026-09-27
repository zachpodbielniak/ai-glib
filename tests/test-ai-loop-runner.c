/*
 * test-ai-loop-runner.c - Loops and goals driven the way a frontend drives them
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every time here is a fake clock passed to ai_loop_runner_tick(), so a
 * week of schedule runs in microseconds and nothing sleeps. HOME, the
 * XDG directories and the working directory are a private sandbox: a
 * suite that read the developer's real state would pass or fail by whose
 * machine ran it.
 */

#include <ai-glib.h>

#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#define MINUTE ((gint64)60 * G_USEC_PER_SEC)
#define HOUR   (60 * MINUTE)

static gchar *sandbox = NULL;
static gchar *store = NULL;

typedef struct
{
	gboolean   busy;
	gboolean   refuse;
	GPtrArray *fired;
	GPtrArray *texts;
	GPtrArray *notices;
	guint      changed;
} Frontend;

static gboolean
on_should_wait(AiLoopRunner *runner, gpointer user_data)
{
	Frontend *front = user_data;
	return front->busy;
}

static gboolean
on_fire(AiLoopRunner *runner, const gchar *id, const gchar *text, gboolean expand, gpointer user_data)
{
	Frontend *front = user_data;

	if (front->refuse)
	{
		return FALSE;
	}

	g_ptr_array_add(front->fired, g_strdup(id));
	g_ptr_array_add(front->texts, g_strdup(text));
	return TRUE;
}

static void
on_notice(AiLoopRunner *runner, const gchar *text, gpointer user_data)
{
	Frontend *front = user_data;
	g_ptr_array_add(front->notices, g_strdup(text));
}

static void
on_changed(AiLoopRunner *runner, gpointer user_data)
{
	Frontend *front = user_data;
	front->changed++;
}

static void
frontend_init(Frontend *front, AiLoopRunner *runner)
{
	memset(front, 0, sizeof *front);
	front->fired = g_ptr_array_new_with_free_func(g_free);
	front->texts = g_ptr_array_new_with_free_func(g_free);
	front->notices = g_ptr_array_new_with_free_func(g_free);
	g_signal_connect(runner, "should-wait", G_CALLBACK(on_should_wait), front);
	g_signal_connect(runner, "fire", G_CALLBACK(on_fire), front);
	g_signal_connect(runner, "notice", G_CALLBACK(on_notice), front);
	g_signal_connect(runner, "changed", G_CALLBACK(on_changed), front);
}

static void
frontend_clear(Frontend *front)
{
	g_ptr_array_unref(front->fired);
	g_ptr_array_unref(front->texts);
	g_ptr_array_unref(front->notices);
}

static gboolean
notices_contain(Frontend *front, const gchar *needle)
{
	guint i;

	for (i = 0; i < front->notices->len; i++)
	{
		if (strstr(g_ptr_array_index(front->notices, i), needle) != NULL)
		{
			return TRUE;
		}
	}

	return FALSE;
}

static gchar *
run(AiLoopRunner *runner, const gchar *name, const gchar *arguments, gint64 now)
{
	g_autoptr(GError) error = NULL;
	gchar            *notice = ai_loop_runner_command(runner, name, arguments, now, &error);

	g_assert_no_error(error);
	g_assert_nonnull(notice);
	return notice;
}

static AiLoopRunner *
open_runner(const gchar *owner, gint64 now)
{
	g_autoptr(GError) error = NULL;
	AiLoopRunner     *runner = ai_loop_runner_new();

	g_assert_true(ai_loop_runner_open(runner, store, owner, now, &error));
	g_assert_no_error(error);
	ai_loop_runner_set_working_directory(runner, sandbox);
	return runner;
}

/* A fixed loop comes due during a turn. It waits for the turn, then
 * fires exactly once, however many slots went by. */
static void
test_queue_behind_turn(void)
{
	g_autoptr(AiLoopRunner) runner = open_runner("queue", 0);
	AiLoopSchedule         *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                front;
	gint64                  now = (gint64)1000 * G_USEC_PER_SEC;
	gint64                  fire;

	frontend_init(&front, runner);
	g_free(run(runner, "loop", "5m check the deploy", now));
	fire = ai_loop_schedule_get_fire_us(schedule, 0);

	g_assert_false(ai_loop_runner_tick(runner, fire - 1));
	front.busy = TRUE;
	g_assert_false(ai_loop_runner_tick(runner, fire));
	g_assert_false(ai_loop_runner_tick(runner, fire + 30 * MINUTE));
	g_assert_cmpuint(front.fired->len, ==, 0);

	front.busy = FALSE;
	g_assert_true(ai_loop_runner_tick(runner, fire + 31 * MINUTE));
	g_assert_cmpuint(front.fired->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(front.texts, 0), ==, "check the deploy");

	/* Its own turn is in flight: nothing else starts, even when due. */
	g_assert_false(ai_loop_runner_tick(runner, fire + 60 * MINUTE));
	ai_loop_runner_turn_finished(runner, "done", NULL, fire + 61 * MINUTE);
	g_assert_null(ai_loop_runner_get_active_id(runner));

	/* One fire for the six missed slots, and the next is in the future. */
	g_assert_cmpint(ai_loop_schedule_get_fire_us(schedule, 0), >, fire + 31 * MINUTE);
	g_assert_cmpuint(front.fired->len, ==, 1);
	frontend_clear(&front);
}

/* Closing the application for three hours: on restart the loop fires
 * once, and the next slot is ahead of the clock. */
static void
test_restart_coalesces(void)
{
	gint64 now = (gint64)5000 * G_USEC_PER_SEC;
	gint64 fire;
	g_autofree gchar *id = NULL;

	{
		g_autoptr(AiLoopRunner) runner = open_runner("restart", now);

		g_free(run(runner, "loop", "10m poll", now));
		g_free(run(runner, "loop", "watch CI", now));
		fire = ai_loop_schedule_get_fire_us(ai_loop_runner_get_schedule(runner), 0);
		id = g_strdup(ai_loop_schedule_get_id(ai_loop_runner_get_schedule(runner), 0));
		/* Self-paced ran once and chose a delay; the process then exits. */
		{
			Frontend front;

			frontend_init(&front, runner);
			g_assert_true(ai_loop_runner_tick(runner, now));
			ai_loop_runner_turn_finished(runner, "LOOP_NEXT: 30m quiet", NULL, now);
			frontend_clear(&front);
		}
	}

	{
		g_autoptr(AiLoopRunner) runner = open_runner("restart", fire + 3 * HOUR);
		AiLoopSchedule         *schedule = ai_loop_runner_get_schedule(runner);
		Frontend                front;
		gint64                  later = fire + 3 * HOUR;

		/* Both kinds survived the restart. */
		g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 2);
		frontend_init(&front, runner);
		g_assert_true(ai_loop_runner_tick(runner, later));
		ai_loop_runner_turn_finished(runner, NULL, NULL, later);
		g_assert_true(ai_loop_runner_tick(runner, later + 1));
		ai_loop_runner_turn_finished(runner, "LOOP_NEXT: 30m", NULL, later + 1);

		/* Two entries, two fires -- not eighteen for the fixed one. */
		g_assert_false(ai_loop_runner_tick(runner, later + 2));
		g_assert_cmpuint(front.fired->len, ==, 2);
		g_assert_true(g_strcmp0(g_ptr_array_index(front.fired, 0), id) == 0 ||
		              g_strcmp0(g_ptr_array_index(front.fired, 1), id) == 0);
		frontend_clear(&front);
	}
}

static gchar *
first_goal_id(AiLoopSchedule *schedule)
{
	guint i;

	for (i = 0; i < ai_loop_schedule_get_n_tasks(schedule); i++)
	{
		if (ai_loop_schedule_get_kind(schedule, i) == AI_LOOP_KIND_GOAL)
		{
			return g_strdup(ai_loop_schedule_get_id(schedule, i));
		}
	}

	return NULL;
}

static void
test_goal_met(void)
{
	g_autoptr(AiLoopRunner) runner = open_runner("goal-met", 0);
	AiLoopSchedule         *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                front;
	g_autofree gchar       *id = NULL;
	gint64                  now = 100 * G_USEC_PER_SEC;
	gint                    index;

	frontend_init(&front, runner);
	g_free(run(runner, "goal", "the tests pass --turns 5", now));
	id = first_goal_id(schedule);
	g_assert_nonnull(id);
	g_assert_true(ai_loop_runner_has_pending_goal(runner));

	g_assert_true(ai_loop_runner_tick(runner, now));
	g_assert_nonnull(strstr(g_ptr_array_index(front.texts, 0), "the tests pass"));
	g_assert_nonnull(strstr(g_ptr_array_index(front.texts, 0), "GOAL_MET"));
	ai_loop_runner_turn_finished(runner, "Fixed one.\nGOAL_NOT_MET: two tests still fail", NULL, now + MINUTE);
	g_assert_true(notices_contain(&front, "not met yet (turn 1/5)"));

	/* The next turn is sent at once and carries the last check. */
	g_assert_true(ai_loop_runner_tick(runner, now + MINUTE));
	g_assert_nonnull(strstr(g_ptr_array_index(front.texts, 1), "turn 2 of 5"));
	g_assert_nonnull(strstr(g_ptr_array_index(front.texts, 1), "two tests still fail"));
	ai_loop_runner_turn_finished(runner, "All green.\nGOAL_MET: make test passes, 0 failures", NULL, now + 2 * MINUTE);

	index = ai_loop_schedule_find(schedule, id);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_MET);
	g_assert_cmpstr(ai_loop_schedule_get_reason(schedule, (guint)index), ==, "make test passes, 0 failures");
	g_assert_true(notices_contain(&front, "met after 2 turns"));
	g_assert_false(ai_loop_runner_has_pending_goal(runner));
	g_assert_false(ai_loop_runner_tick(runner, now + 3 * MINUTE));
	frontend_clear(&front);
}

/* A goal the model never meets ends at its bound, reported as not met,
 * whatever the model says in between. */
static void
test_goal_hits_turn_bound(void)
{
	g_autoptr(AiLoopRunner) runner = open_runner("goal-turns", 0);
	AiLoopSchedule         *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                front;
	g_autofree gchar       *id = NULL;
	gint64                  now = 100 * G_USEC_PER_SEC;
	guint                   turn;
	gint                    index;

	frontend_init(&front, runner);
	g_free(run(runner, "goal", "prove P = NP --turns 3", now));
	id = first_goal_id(schedule);

	for (turn = 0; turn < 3; turn++)
	{
		g_assert_true(ai_loop_runner_tick(runner, now));
		/* No verdict line at all: counts as not met. */
		ai_loop_runner_turn_finished(runner, "Still thinking.", NULL, now);
	}

	g_assert_false(ai_loop_runner_tick(runner, now));
	g_assert_cmpuint(front.fired->len, ==, 3);
	index = ai_loop_schedule_find(schedule, id);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_EXPIRED);
	g_assert_nonnull(strstr(ai_loop_schedule_get_reason(schedule, (guint)index), "not met after 3 turns"));
	g_assert_true(notices_contain(&front, "expired: not met after 3 turns"));
	frontend_clear(&front);
}

static void
test_goal_hits_time_bound(void)
{
	g_autoptr(AiLoopRunner) runner = open_runner("goal-time", 0);
	AiLoopSchedule         *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                front;
	g_autofree gchar       *id = NULL;
	gint64                  now = 100 * G_USEC_PER_SEC;
	gint                    index;

	frontend_init(&front, runner);
	g_free(run(runner, "goal", "the build is green --time 30m", now));
	id = first_goal_id(schedule);
	g_assert_true(ai_loop_runner_tick(runner, now));
	ai_loop_runner_turn_finished(runner, "GOAL_NOT_MET: waiting on CI", NULL, now + MINUTE);

	/* Held back by a busy session until past the deadline: the reap ends
	 * it before anything fires. */
	front.busy = TRUE;
	g_assert_false(ai_loop_runner_tick(runner, now + 31 * MINUTE));
	index = ai_loop_schedule_find(schedule, id);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_EXPIRED);
	g_assert_nonnull(strstr(ai_loop_schedule_get_reason(schedule, (guint)index), "its time bound"));
	g_assert_nonnull(strstr(ai_loop_schedule_get_reason(schedule, (guint)index), "waiting on CI"));

	/* Raising the bound gives it room again. */
	{
		g_autofree gchar *line = g_strdup_printf("edit %s --time 2h", id);
		g_free(run(runner, "goal", line, now + 32 * MINUTE));
	}
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_ACTIVE);
	front.busy = FALSE;
	g_assert_true(ai_loop_runner_tick(runner, now + 33 * MINUTE));
	frontend_clear(&front);
}

static void
test_goal_blocked_and_errors(void)
{
	g_autoptr(AiLoopRunner) runner = open_runner("goal-fail", 0);
	AiLoopSchedule         *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                front;
	g_autofree gchar       *id = NULL;
	gint64                  now = 100 * G_USEC_PER_SEC;
	gint                    index;
	guint                   i;

	frontend_init(&front, runner);
	g_free(run(runner, "goal", "deploy to prod", now));
	id = first_goal_id(schedule);
	g_assert_true(ai_loop_runner_tick(runner, now));
	ai_loop_runner_turn_finished(runner, "GOAL_BLOCKED: needs the deploy key", NULL, now);
	index = ai_loop_schedule_find(schedule, id);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_FAILED);
	g_assert_cmpstr(ai_loop_schedule_get_reason(schedule, (guint)index), ==, "blocked: needs the deploy key");

	g_clear_pointer(&id, g_free);
	g_free(run(runner, "goal", "delete all", now));
	g_free(run(runner, "goal", "flaky provider", now));
	id = first_goal_id(schedule);

	for (i = 0; i < 3; i++)
	{
		gint64 when = now + (gint64)i * 2 * MINUTE;

		g_assert_true(ai_loop_runner_tick(runner, when));
		ai_loop_runner_turn_finished(runner, NULL, "HTTP 500", when);

		/* A failed turn is retried after a pause, not in a hot loop. */
		if (i < 2)
		{
			g_assert_false(ai_loop_runner_tick(runner, when + 1));
		}
	}

	index = ai_loop_schedule_find(schedule, id);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_FAILED);
	g_assert_nonnull(strstr(ai_loop_schedule_get_reason(schedule, (guint)index), "3 turns in a row failed"));
	frontend_clear(&front);
}

/* `ai loop pause` from another process edits the file; the running
 * session picks it up on its next tick. */
static void
test_outside_edit_is_picked_up(void)
{
	g_autoptr(AiLoopRunner)   runner = open_runner("outside", 0);
	g_autoptr(AiLoopSchedule) other = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	AiLoopSchedule           *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                  front;
	g_autofree gchar         *id = NULL;
	gint64                    now = 100 * G_USEC_PER_SEC;

	frontend_init(&front, runner);
	g_free(run(runner, "loop", "5m ping", now));
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));

	g_assert_cmpint(ai_loop_schedule_load(other, ai_loop_runner_get_path(runner), now, &error), ==, 1);
	g_assert_true(ai_loop_schedule_pause(other, id, &error));
	g_assert_true(ai_loop_schedule_save(other, ai_loop_runner_get_path(runner), &error));
	g_assert_no_error(error);

	g_assert_false(ai_loop_runner_tick(runner, now + HOUR));
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_PAUSED);
	g_assert_cmpuint(front.fired->len, ==, 0);

	/* Run-now from outside fires a paused loop once and leaves it paused. */
	g_assert_true(ai_loop_schedule_run_now(other, id, now + HOUR, &error));
	g_assert_true(ai_loop_schedule_save(other, ai_loop_runner_get_path(runner), &error));
	g_assert_true(ai_loop_runner_tick(runner, now + HOUR + 1));
	ai_loop_runner_turn_finished(runner, "ok", NULL, now + HOUR + 2);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_PAUSED);
	g_assert_false(ai_loop_runner_tick(runner, now + 2 * HOUR));

	/* A file deleted from outside means nothing is scheduled. */
	g_assert_true(ai_loop_store_remove(store, "outside", &error));
	g_assert_false(ai_loop_runner_tick(runner, now + 3 * HOUR));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);
	frontend_clear(&front);
}

static void
test_refused_fire_is_retried_and_close_stops(void)
{
	g_autoptr(AiLoopRunner) runner = open_runner("refused", 0);
	AiLoopSchedule         *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                front;
	gint64                  now = 100 * G_USEC_PER_SEC;

	frontend_init(&front, runner);
	g_free(run(runner, "goal", "anything --turns 2", now));
	front.refuse = TRUE;
	g_assert_false(ai_loop_runner_tick(runner, now));
	g_assert_null(ai_loop_runner_get_active_id(runner));
	/* A turn that never started is not counted against the bound. */
	g_assert_cmpuint(ai_loop_schedule_get_turns(schedule, 0), ==, 0);
	front.refuse = FALSE;
	g_assert_true(ai_loop_runner_tick(runner, now + 1));
	ai_loop_runner_turn_finished(runner, "GOAL_NOT_MET: x", NULL, now + 2);

	/* Closed: nothing fires, and the second process can claim it. */
	ai_loop_runner_close(runner);
	g_assert_false(ai_loop_runner_tick(runner, now + HOUR));
	g_assert_cmpuint(front.fired->len, ==, 1);

	{
		g_autoptr(AiLoopRunner) again = open_runner("refused", now + HOUR);

		g_assert_cmpuint(ai_loop_schedule_get_n_tasks(ai_loop_runner_get_schedule(again)), ==, 1);
	}

	frontend_clear(&front);
}

static void
test_claim_is_exclusive(void)
{
	g_autoptr(AiLoopRunner) first = open_runner("exclusive", 0);
	g_autoptr(AiLoopRunner) second = ai_loop_runner_new();
	g_autoptr(AiLoopRunner) quiet = open_runner("never-used", 0);
	g_autoptr(GError)       error = NULL;
	g_autofree gchar       *unused = g_build_filename(store, "never-used", NULL);
	g_autofree gchar       *unused_lock = g_build_filename(store, "never-used.lock", NULL);

	/* A session that schedules nothing leaves nothing on disk. */
	g_assert_false(ai_loop_runner_tick(quiet, 0));
	g_assert_false(g_file_test(unused, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(unused_lock, G_FILE_TEST_EXISTS));

	/* The first write claims it; a second process is then refused. */
	g_free(run(first, "loop", "5m claim it", 0));
	g_assert_false(ai_loop_runner_open(second, store, "exclusive", 0, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);
	g_clear_error(&error);
	g_assert_false(ai_loop_runner_open(second, store, "../escape", 0, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

/* A schedule must not clear its own session: /clear is refused when it
 * is scheduled, and a /command that is allowed runs as a command. */
static void
test_scheduled_commands(void)
{
	g_autoptr(AiLoopRunner)   runner = open_runner("commands", 0);
	g_autoptr(AiCommandSet)   commands = ai_command_set_new(NULL);
	g_autoptr(GError)         error = NULL;
	AiLoopSchedule           *schedule = ai_loop_runner_get_schedule(runner);
	Frontend                  front;
	gchar                    *notice;
	gint64                    now = 100 * G_USEC_PER_SEC;

	ai_loop_schedule_set_commands(schedule, commands);
	frontend_init(&front, runner);
	notice = ai_loop_runner_command(runner, "loop", "10m /clear", now, &error);
	g_assert_null(notice);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "/clear cannot run on a schedule"));
	g_clear_error(&error);
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);

	g_free(run(runner, "loop", "/todos", now));
	g_assert_true(ai_loop_runner_tick(runner, now));
	g_assert_cmpstr(g_ptr_array_index(front.texts, 0), ==, "/todos");
	ai_loop_runner_turn_finished(runner, NULL, NULL, now);
	/* A command was never asked for a delay; silence is not a stop. */
	g_assert_true(notices_contain(&front, "runs its command again in 20m"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 1);
	frontend_clear(&front);
}

/* The real timer is a thread-default GSource, and stopping it by pointer
 * means nothing fires afterwards. */
static void
test_timer_on_private_context(void)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(AiLoopRunner) runner = NULL;
	Frontend                front;
	gint64                  deadline;

	g_main_context_push_thread_default(context);
	runner = open_runner("timer", g_get_real_time());
	frontend_init(&front, runner);
	g_free(run(runner, "goal", "fire on the private context --turns 1", g_get_real_time()));
	ai_loop_runner_start(runner);
	deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

	while (front.fired->len == 0 && g_get_monotonic_time() < deadline)
	{
		g_main_context_iteration(context, TRUE);
	}

	g_assert_cmpuint(front.fired->len, ==, 1);
	ai_loop_runner_stop(runner);
	ai_loop_runner_stop(runner);
	g_main_context_pop_thread_default(context);
	frontend_clear(&front);
}

static void
rm_rf(const gchar *path)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", path);
	g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
}

int
main(int argc, char **argv)
{
	gint status;

	sandbox = g_dir_make_tmp("ai-loop-runner-XXXXXX", NULL);
	g_assert_nonnull(sandbox);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_setenv("XDG_STATE_HOME", sandbox, TRUE);
	g_setenv("XDG_DATA_HOME", sandbox, TRUE);
	g_unsetenv("AI_LOOP_DISABLE");
	g_assert_cmpint(g_chdir(sandbox), ==, 0);
	store = g_build_filename(sandbox, "loops", NULL);

	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/loop-runner/queue-behind-turn", test_queue_behind_turn);
	g_test_add_func("/ai-glib/loop-runner/restart-coalesces", test_restart_coalesces);
	g_test_add_func("/ai-glib/loop-runner/goal-met", test_goal_met);
	g_test_add_func("/ai-glib/loop-runner/goal-turn-bound", test_goal_hits_turn_bound);
	g_test_add_func("/ai-glib/loop-runner/goal-time-bound", test_goal_hits_time_bound);
	g_test_add_func("/ai-glib/loop-runner/goal-blocked-and-errors", test_goal_blocked_and_errors);
	g_test_add_func("/ai-glib/loop-runner/outside-edit", test_outside_edit_is_picked_up);
	g_test_add_func("/ai-glib/loop-runner/refused-and-close", test_refused_fire_is_retried_and_close_stops);
	g_test_add_func("/ai-glib/loop-runner/claim", test_claim_is_exclusive);
	g_test_add_func("/ai-glib/loop-runner/commands", test_scheduled_commands);
	g_test_add_func("/ai-glib/loop-runner/timer", test_timer_on_private_context);
	status = g_test_run();
	g_chdir("/");
	rm_rf(sandbox);
	g_free(store);
	g_free(sandbox);
	return status;
}
