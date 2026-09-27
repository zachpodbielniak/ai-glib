/*
 * ai-loop-runner.c - Drives one session's loops and goals
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiLoopSchedule decides what is due; this decides when to ask. ai-tui
 * and ai-gui both run one, so there is one scheduler and one set of
 * rules about when a scheduled turn may start:
 *
 *  - Never mid-turn. ::should-wait lets the frontend say it is busy, and
 *    a fire already in flight holds everything else back, so a slot that
 *    comes due during a turn queues behind it and fires once.
 *  - The file is the truth. Each tick picks up an edit made elsewhere
 *    (`ai loop pause` from a shell), and each change here is written at
 *    once, atomically.
 *  - The timer is a GSource held by pointer on the thread-default
 *    context and torn down with g_source_destroy(), for the reason
 *    AGENTS.md gives: an id means nothing outside the context it came
 *    from.
 */

#include "config.h"

#include <unistd.h>

#include "core/ai-error.h"
#include "harness/ai-loop-runner.h"

struct _AiLoopRunner
{
	GObject         parent_instance;
	AiLoopSchedule *schedule;
	gchar          *directory;
	gchar          *owner;
	gchar          *path;
	gchar          *working_directory;
	gchar          *active_id;
	GSource        *source;
	gint            lock_fd;
};

enum
{
	PROP_0,
	PROP_WORKING_DIRECTORY,
	N_PROPS
};

enum
{
	SIGNAL_SHOULD_WAIT,
	SIGNAL_FIRE,
	SIGNAL_NOTICE,
	SIGNAL_CHANGED,
	N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint       signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(AiLoopRunner, ai_loop_runner, G_TYPE_OBJECT)

static void
runner_release(AiLoopRunner *self)
{
	if (self->lock_fd >= 0)
	{
		close(self->lock_fd);
		self->lock_fd = -1;
	}

	g_clear_pointer(&self->directory, g_free);
	g_clear_pointer(&self->owner, g_free);
	g_clear_pointer(&self->path, g_free);
}

static void
ai_loop_runner_dispose(GObject *object)
{
	AiLoopRunner *self = AI_LOOP_RUNNER(object);

	ai_loop_runner_stop(self);
	runner_release(self);
	g_clear_object(&self->schedule);
	G_OBJECT_CLASS(ai_loop_runner_parent_class)->dispose(object);
}

static void
ai_loop_runner_finalize(GObject *object)
{
	AiLoopRunner *self = AI_LOOP_RUNNER(object);

	g_free(self->working_directory);
	g_free(self->active_id);
	G_OBJECT_CLASS(ai_loop_runner_parent_class)->finalize(object);
}

static void
ai_loop_runner_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiLoopRunner *self = AI_LOOP_RUNNER(object);

	switch (id)
	{
	case PROP_WORKING_DIRECTORY:
		g_value_set_string(value, self->working_directory);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
		break;
	}
}

static void
ai_loop_runner_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiLoopRunner *self = AI_LOOP_RUNNER(object);

	switch (id)
	{
	case PROP_WORKING_DIRECTORY:
		ai_loop_runner_set_working_directory(self, g_value_get_string(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
		break;
	}
}

static void
ai_loop_runner_class_init(AiLoopRunnerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->dispose = ai_loop_runner_dispose;
	object_class->finalize = ai_loop_runner_finalize;
	object_class->get_property = ai_loop_runner_get_property;
	object_class->set_property = ai_loop_runner_set_property;

	/**
	 * AiLoopRunner:working-directory:
	 *
	 * Where a loop with no prompt looks for `loop.md`.
	 */
	properties[PROP_WORKING_DIRECTORY] =
		g_param_spec_string("working-directory", "Working directory",
		                    "Where the maintenance prompt's loop.md is looked up",
		                    NULL,
		                    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(object_class, N_PROPS, properties);

	/**
	 * AiLoopRunner::should-wait:
	 * @self: the runner
	 *
	 * Asked before every fire. Return %TRUE while anything is in flight
	 * -- a turn, a queued draft, an approval -- and the due entry waits
	 * for the next tick. Any handler returning %TRUE wins.
	 *
	 * Returns: %TRUE to hold scheduled turns back
	 */
	signals[SIGNAL_SHOULD_WAIT] =
		g_signal_new("should-wait", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		             0, g_signal_accumulator_true_handled, NULL, NULL,
		             G_TYPE_BOOLEAN, 0);

	/**
	 * AiLoopRunner::fire:
	 * @self: the runner
	 * @id: the loop or goal
	 * @text: what to send
	 * @expand: %TRUE to send it through the input pipeline, so a
	 *   `/command` runs; %FALSE to send it to the model as text
	 *
	 * Start the turn, and call ai_loop_runner_turn_finished() when it
	 * ends. A handler that could not start one returns %FALSE and the
	 * entry is tried again on a later tick.
	 *
	 * Returns: %TRUE if a turn was started
	 */
	signals[SIGNAL_FIRE] =
		g_signal_new("fire", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		             0, g_signal_accumulator_true_handled, NULL, NULL,
		             G_TYPE_BOOLEAN, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);

	/**
	 * AiLoopRunner::notice:
	 * @self: the runner
	 * @text: a line for the person: a loop ran, a goal was met, a save
	 *   failed
	 */
	signals[SIGNAL_NOTICE] =
		g_signal_new("notice", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * AiLoopRunner::changed:
	 * @self: the runner
	 *
	 * The schedule changed: redraw whatever shows it.
	 */
	signals[SIGNAL_CHANGED] =
		g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
		             0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ai_loop_runner_init(AiLoopRunner *self)
{
	self->schedule = ai_loop_schedule_new();
	self->lock_fd = -1;
}

/**
 * ai_loop_runner_new:
 *
 * A runner with an empty schedule and no file. ai_loop_runner_open()
 * gives it one.
 *
 * Returns: (transfer full): a new runner
 */
AiLoopRunner *
ai_loop_runner_new(void)
{
	return g_object_new(AI_TYPE_LOOP_RUNNER, NULL);
}

/**
 * ai_loop_runner_get_schedule:
 * @self: a runner
 *
 * Returns: (transfer none): the schedule it drives
 */
AiLoopSchedule *
ai_loop_runner_get_schedule(AiLoopRunner *self)
{
	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), NULL);
	return self->schedule;
}

static void
say(AiLoopRunner *self, const gchar *text)
{
	if (text != NULL && text[0] != '\0')
	{
		g_signal_emit(self, signals[SIGNAL_NOTICE], 0, text);
	}
}

/**
 * ai_loop_runner_save:
 * @self: a runner
 * @error: (nullable): return location for a #GError
 *
 * Writes the schedule now. Every change made through the runner already
 * does; this is for one made on ai_loop_runner_get_schedule() directly.
 * A runner with no file succeeds without writing.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_runner_save(AiLoopRunner *self, GError **error)
{
	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), FALSE);

	if (self->path == NULL)
	{
		return TRUE;
	}

	/* Never write a file another process is running. */
	if (self->lock_fd < 0)
	{
		self->lock_fd = ai_loop_store_claim(self->directory, self->owner, error);

		if (self->lock_fd < 0)
		{
			return FALSE;
		}
	}

	return ai_loop_schedule_save(self->schedule, self->path, error);
}

static void
persist(AiLoopRunner *self)
{
	g_autoptr(GError) error = NULL;

	/* The claim is taken on the first write, not on open, so a session
	 * that never schedules anything leaves nothing on disk. */
	if (!ai_loop_runner_save(self, &error))
	{
		g_autofree gchar *text = g_strdup_printf("Could not save loops and goals: %s",
		                                         error->message);

		g_debug("%s", text);
		say(self, text);
	}

	g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

/**
 * ai_loop_runner_open:
 * @self: a runner
 * @directory: (nullable): the store, or %NULL for the default
 * @owner: the session whose schedule this is
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Loads @owner's schedule and claims it. The claim is an advisory lock,
 * so two processes never fire one schedule; the second is refused with
 * %G_IO_ERROR_BUSY and the runner is left as it was. A schedule with no
 * file yet is claimed when it is first saved, so a session that never
 * schedules anything writes nothing. What was missed while nothing held
 * the schedule fires once, not once per missed slot.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_runner_open(
	AiLoopRunner *self,
	const gchar  *directory,
	const gchar  *owner,
	gint64        now_us,
	GError      **error
){
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *path = NULL;
	gint              fd;

	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), FALSE);

	if (directory == NULL)
	{
		fallback = ai_loop_store_default_directory();
		directory = fallback;
	}

	path = ai_loop_store_path(directory, owner);

	if (path == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A schedule owner must be a plain identifier.");
		return FALSE;
	}

	if (g_strcmp0(path, self->path) == 0)
	{
		return TRUE;
	}

	/*
	 * A schedule that exists is claimed now: it may have something due,
	 * and two processes must never fire it. One that does not exist yet
	 * is claimed when it is first written, so opening a session that
	 * never schedules anything writes nothing.
	 */
	fd = -1;

	if (g_file_test(path, G_FILE_TEST_EXISTS))
	{
		fd = ai_loop_store_claim(directory, owner, error);

		if (fd < 0)
		{
			return FALSE;
		}
	}

	/* Load in place so a frontend holding the schedule pointer, and the
	 * command set attached to it, keep working. */
	if (ai_loop_schedule_load(self->schedule, path, now_us, error) < 0)
	{
		if (fd >= 0)
		{
			close(fd);
		}

		return FALSE;
	}

	runner_release(self);
	g_clear_pointer(&self->active_id, g_free);
	self->lock_fd = fd;
	self->directory = g_strdup(directory);
	self->owner = g_strdup(owner);
	self->path = g_steal_pointer(&path);
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
	return TRUE;
}

/**
 * ai_loop_runner_close:
 * @self: a runner
 *
 * Stops using the file and releases the claim. The schedule stays on
 * disk for whoever resumes the session; to make sure nothing it held can
 * fire later, remove it with ai_loop_store_remove() afterwards.
 */
void
ai_loop_runner_close(AiLoopRunner *self)
{
	g_return_if_fail(AI_IS_LOOP_RUNNER(self));

	runner_release(self);
	g_clear_pointer(&self->active_id, g_free);
	ai_loop_schedule_clear(self->schedule);
	g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

/**
 * ai_loop_runner_get_owner:
 * @self: a runner
 *
 * Returns: (transfer none) (nullable): the session whose schedule is open
 */
const gchar *
ai_loop_runner_get_owner(AiLoopRunner *self)
{
	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), NULL);
	return self->owner;
}

/**
 * ai_loop_runner_get_path:
 * @self: a runner
 *
 * Returns: (transfer none) (nullable): the schedule file
 */
const gchar *
ai_loop_runner_get_path(AiLoopRunner *self)
{
	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), NULL);
	return self->path;
}

/**
 * ai_loop_runner_get_active_id:
 * @self: a runner
 *
 * Returns: (transfer none) (nullable): the entry whose turn is running
 */
const gchar *
ai_loop_runner_get_active_id(AiLoopRunner *self)
{
	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), NULL);
	return self->active_id;
}

/**
 * ai_loop_runner_get_working_directory:
 * @self: a runner
 *
 * Returns: (transfer none) (nullable): where loop.md is looked up
 */
const gchar *
ai_loop_runner_get_working_directory(AiLoopRunner *self)
{
	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), NULL);
	return self->working_directory;
}

/**
 * ai_loop_runner_set_working_directory:
 * @self: a runner
 * @directory: (nullable): where loop.md is looked up
 */
void
ai_loop_runner_set_working_directory(AiLoopRunner *self, const gchar *directory)
{
	g_return_if_fail(AI_IS_LOOP_RUNNER(self));

	if (g_strcmp0(self->working_directory, directory) == 0)
	{
		return;
	}

	g_free(self->working_directory);
	self->working_directory = g_strdup(directory);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WORKING_DIRECTORY]);
}

static gboolean
on_tick(gpointer user_data)
{
	ai_loop_runner_tick(AI_LOOP_RUNNER(user_data), g_get_real_time());
	return G_SOURCE_CONTINUE;
}

/**
 * ai_loop_runner_start:
 * @self: a runner
 *
 * Checks once a second, on the thread-default main context -- the one a
 * caller driving a private nested loop is actually running.
 */
void
ai_loop_runner_start(AiLoopRunner *self)
{
	g_return_if_fail(AI_IS_LOOP_RUNNER(self));

	if (self->source != NULL)
	{
		return;
	}

	self->source = g_timeout_source_new_seconds(1);
	g_source_set_callback(self->source, on_tick, self, NULL);
	g_source_attach(self->source, g_main_context_get_thread_default());
}

/**
 * ai_loop_runner_stop:
 * @self: a runner
 *
 * Nothing fires from the timer after this returns.
 */
void
ai_loop_runner_stop(AiLoopRunner *self)
{
	g_return_if_fail(AI_IS_LOOP_RUNNER(self));

	if (self->source != NULL)
	{
		g_source_destroy(self->source);
		g_clear_pointer(&self->source, g_source_unref);
	}
}

static void
sync_from_disk(AiLoopRunner *self, gint64 now_us)
{
	g_autoptr(GError) error = NULL;
	gint              changed;

	if (self->path == NULL)
	{
		return;
	}

	changed = ai_loop_schedule_sync(self->schedule, self->path, now_us, &error);

	if (changed < 0)
	{
		/* Somebody wrote something unreadable. Keep running what is in
		 * memory; the next save replaces the file. */
		g_debug("loop file %s unreadable, keeping the schedule in memory: %s",
		        self->path, error->message);
	}
	else if (changed > 0)
	{
		g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
	}
}

/**
 * ai_loop_runner_tick:
 * @self: a runner
 * @now_us: real time in microseconds; tests pass a fake clock
 *
 * One check: pick up outside edits, end what ran out of time, and fire
 * the earliest due entry if nothing is in flight. At most one fire per
 * tick, and none while a scheduled turn is still running.
 *
 * Returns: %TRUE if a turn was started
 */
gboolean
ai_loop_runner_tick(AiLoopRunner *self, gint64 now_us)
{
	g_autofree gchar *reaped = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *prompt = NULL;
	g_autofree gchar *fired = NULL;
	g_autofree gchar *line = NULL;
	gboolean          wait = FALSE;
	gboolean          expand;
	gboolean          started = FALSE;
	gint              index;

	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), FALSE);

	sync_from_disk(self, now_us);
	reaped = ai_loop_schedule_reap(self->schedule, now_us);

	if (reaped != NULL)
	{
		say(self, reaped);
		persist(self);
	}

	if (self->active_id != NULL)
	{
		return FALSE;
	}

	id = g_strdup(ai_loop_schedule_due(self->schedule, now_us));

	if (id == NULL)
	{
		return FALSE;
	}

	g_signal_emit(self, signals[SIGNAL_SHOULD_WAIT], 0, &wait);

	if (wait)
	{
		return FALSE;
	}

	prompt = ai_loop_schedule_dup_prompt(self->schedule, id, self->working_directory,
	                                     g_get_user_config_dir(), g_get_home_dir());
	index = ai_loop_schedule_find(self->schedule, id);

	if (prompt == NULL || index < 0)
	{
		return FALSE;
	}

	line = g_strdup_printf("%s %s running.",
	                       ai_loop_schedule_get_kind(self->schedule, (guint)index) == AI_LOOP_KIND_GOAL
	                           ? "Goal" : "Loop",
	                       id);

	if (!ai_loop_schedule_note_fired(self->schedule, id, now_us, &fired))
	{
		return FALSE;
	}

	say(self, fired);
	expand = ai_loop_schedule_is_unattended_safe(self->schedule, prompt);
	self->active_id = g_strdup(id);
	persist(self);
	say(self, line);
	g_signal_emit(self, signals[SIGNAL_FIRE], 0, id, prompt, expand, &started);

	if (!started && g_strcmp0(self->active_id, id) == 0)
	{
		g_autofree gchar *retry = g_strdup_printf("%s did not start; it will be tried again.", id);

		ai_loop_schedule_clear_inflight(self->schedule, id);
		g_clear_pointer(&self->active_id, g_free);
		say(self, retry);
		persist(self);
	}

	return started;
}

/**
 * ai_loop_runner_turn_finished:
 * @self: a runner
 * @assistant_text: (nullable): the reply, for a self-paced loop's delay
 *   or a goal's verdict
 * @turn_error: (nullable): why the turn failed; %NULL when it did not
 * @now_us: real time in microseconds
 *
 * Call when a turn ends. It does nothing unless the runner started that
 * turn, so a frontend can call it after every turn.
 */
void
ai_loop_runner_turn_finished(
	AiLoopRunner *self,
	const gchar  *assistant_text,
	const gchar  *turn_error,
	gint64        now_us
){
	g_autofree gchar *id = NULL;
	g_autofree gchar *notice = NULL;
	gint              index;

	g_return_if_fail(AI_IS_LOOP_RUNNER(self));

	if (self->active_id == NULL)
	{
		return;
	}

	id = g_steal_pointer(&self->active_id);

	/* An edit made while the turn ran is what gets judged against. */
	sync_from_disk(self, now_us);
	index = ai_loop_schedule_find(self->schedule, id);

	if (index < 0)
	{
		g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
		return;
	}

	if (ai_loop_schedule_get_kind(self->schedule, (guint)index) == AI_LOOP_KIND_GOAL)
	{
		notice = ai_loop_schedule_goal_complete(self->schedule, id, assistant_text,
		                                        turn_error, now_us);
	}
	else if (ai_loop_schedule_id_is_dynamic(self->schedule, id))
	{
		notice = ai_loop_schedule_complete(self->schedule, id,
		                                   turn_error == NULL ? assistant_text : NULL,
		                                   now_us);
	}

	say(self, notice);
	persist(self);
}

/**
 * ai_loop_runner_turn_cancelled:
 * @self: a runner
 * @now_us: real time in microseconds
 *
 * The person stopped a scheduled turn. That is an instruction, not a
 * failure: a goal is paused, its turn uncounted, until it is resumed;
 * a self-paced loop waits as if it had not named a delay. Nothing
 * refires straight away.
 */
void
ai_loop_runner_turn_cancelled(AiLoopRunner *self, gint64 now_us)
{
	g_autofree gchar *id = NULL;
	g_autofree gchar *notice = NULL;
	gint              index;

	g_return_if_fail(AI_IS_LOOP_RUNNER(self));

	if (self->active_id == NULL)
	{
		return;
	}

	id = g_steal_pointer(&self->active_id);
	sync_from_disk(self, now_us);
	index = ai_loop_schedule_find(self->schedule, id);

	if (index < 0)
	{
		g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
		return;
	}

	if (ai_loop_schedule_get_kind(self->schedule, (guint)index) == AI_LOOP_KIND_GOAL)
	{
		ai_loop_schedule_clear_inflight(self->schedule, id);

		if (ai_loop_schedule_pause(self->schedule, id, NULL))
		{
			notice = g_strdup_printf("Goal %s paused: its turn was stopped. /goal resume %s continues it.",
			                         id, id);
		}
	}
	else if (ai_loop_schedule_id_is_dynamic(self->schedule, id))
	{
		notice = ai_loop_schedule_complete(self->schedule, id, NULL, now_us);
	}

	say(self, notice);
	persist(self);
}

/**
 * ai_loop_runner_command:
 * @self: a runner
 * @name: "loop" or "goal"
 * @arguments: (nullable): what followed the command
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Applies a `/loop` or `/goal` line to the current file and saves it.
 *
 * Returns: (transfer full) (nullable): the line to show, or %NULL on error
 */
gchar *
ai_loop_runner_command(
	AiLoopRunner *self,
	const gchar  *name,
	const gchar  *arguments,
	gint64        now_us,
	GError      **error
){
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	gchar            *notice;

	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), NULL);

	sync_from_disk(self, now_us);
	before = ai_loop_schedule_dup_json(self->schedule, now_us);

	if (g_strcmp0(name, "goal") == 0)
	{
		notice = ai_loop_schedule_goal_command(self->schedule, arguments, now_us, error);
	}
	else
	{
		notice = ai_loop_schedule_command(self->schedule, arguments, self->working_directory,
		                                  g_get_user_config_dir(), g_get_home_dir(),
		                                  now_us, error);
	}

	/* `list` and `show` change nothing, and must not write -- or claim --
	 * a file for a session that has never scheduled anything. */
	after = ai_loop_schedule_dup_json(self->schedule, now_us);

	if (notice != NULL && g_strcmp0(before, after) != 0)
	{
		persist(self);
	}

	return notice;
}

/**
 * ai_loop_runner_has_pending_goal:
 * @self: a runner
 *
 * What a one-shot run such as `ai-tui --dump` waits for before it exits.
 *
 * Returns: %TRUE while an active goal is unfinished
 */
gboolean
ai_loop_runner_has_pending_goal(AiLoopRunner *self)
{
	guint i;
	guint n;

	g_return_val_if_fail(AI_IS_LOOP_RUNNER(self), FALSE);

	n = ai_loop_schedule_get_n_tasks(self->schedule);

	for (i = 0; i < n; i++)
	{
		if (ai_loop_schedule_get_kind(self->schedule, i) == AI_LOOP_KIND_GOAL &&
		    ai_loop_schedule_get_state(self->schedule, i) == AI_LOOP_STATE_ACTIVE)
		{
			return TRUE;
		}
	}

	return FALSE;
}

/**
 * ai_loop_runner_clear:
 * @self: a runner
 *
 * Drops every loop and goal and saves the empty schedule. A turn one of
 * them started finishes and schedules nothing.
 */
void
ai_loop_runner_clear(AiLoopRunner *self)
{
	g_return_if_fail(AI_IS_LOOP_RUNNER(self));

	ai_loop_schedule_clear(self->schedule);
	g_clear_pointer(&self->active_id, g_free);
	persist(self);
}
