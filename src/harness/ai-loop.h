/*
 * ai-loop.h - Scheduled prompts (loops) and goals for one session
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "harness/ai-command.h"

G_BEGIN_DECLS

/**
 * AiLoopKind:
 * @AI_LOOP_KIND_LOOP: a prompt that repeats on an interval, or at a delay
 *   the model chooses
 * @AI_LOOP_KIND_GOAL: turns that continue until a condition holds or a
 *   bound is reached
 *
 * What a schedule entry is.
 */
typedef enum
{
	AI_LOOP_KIND_LOOP,
	AI_LOOP_KIND_GOAL
} AiLoopKind;

/**
 * AiLoopState:
 * @AI_LOOP_STATE_ACTIVE: runs when due
 * @AI_LOOP_STATE_PAUSED: kept, but not run until resumed
 * @AI_LOOP_STATE_MET: a goal whose condition was judged to hold
 * @AI_LOOP_STATE_FAILED: a goal that cannot continue: the model reported
 *   it blocked, or three turns in a row failed
 * @AI_LOOP_STATE_STOPPED: a goal somebody stopped before it was met
 * @AI_LOOP_STATE_EXPIRED: a goal that reached its turn or time bound
 *   without being met
 *
 * One vocabulary for every frontend. The last four are final and only
 * goals reach them; a loop that ends is removed, with a notice saying why.
 */
typedef enum
{
	AI_LOOP_STATE_ACTIVE,
	AI_LOOP_STATE_PAUSED,
	AI_LOOP_STATE_MET,
	AI_LOOP_STATE_FAILED,
	AI_LOOP_STATE_STOPPED,
	AI_LOOP_STATE_EXPIRED
} AiLoopState;

#define AI_TYPE_LOOP_KIND (ai_loop_kind_get_type())
GType
ai_loop_kind_get_type(void) G_GNUC_CONST;

#define AI_TYPE_LOOP_STATE (ai_loop_state_get_type())
GType
ai_loop_state_get_type(void) G_GNUC_CONST;

const gchar *
ai_loop_kind_to_string(AiLoopKind kind);

const gchar *
ai_loop_state_to_string(AiLoopState state);

gboolean
ai_loop_state_is_final(AiLoopState state);

gchar *
ai_loop_format_duration(gint64 duration_us);

const gchar *
ai_loop_help_text(void);

gchar *
ai_loop_format_relative(gint64 delta_us);

#define AI_TYPE_LOOP_SCHEDULE (ai_loop_schedule_get_type())

G_DECLARE_FINAL_TYPE(AiLoopSchedule, ai_loop_schedule, AI, LOOP_SCHEDULE, GObject)

/**
 * ai_loop_schedule_new:
 *
 * Creates an empty schedule. Tasks live only in this object until
 * ai_loop_schedule_save() writes them.
 *
 * Returns: (transfer full): a new schedule
 */
AiLoopSchedule *
ai_loop_schedule_new(void);

/**
 * ai_loop_schedule_is_disabled:
 *
 * Whether %AI_LOOP_DISABLE is set to a value other than 0, false, no, or off.
 * A disabled schedule still stores tasks; nothing is due.
 *
 * Returns: %TRUE when scheduled prompts must not run
 */
gboolean
ai_loop_schedule_is_disabled(void);

/**
 * ai_loop_default_prompt:
 * @cwd: (nullable): working directory
 * @config_dir: (nullable): XDG config directory
 * @home: (nullable): home directory
 * @origin: (out) (optional) (transfer full) (nullable): which file was used
 * @truncated: (out) (optional): whether a file was cut at 25000 bytes
 *
 * The prompt a bare /loop runs. Project `.ai-glib/loop.md` wins, then
 * project `.claude/loop.md`, then `config_dir/ai-glib/loop.md`, then
 * `home/.claude/loop.md`. Otherwise the built-in maintenance prompt.
 *
 * Returns: (transfer full): the prompt text
 */
gchar *
ai_loop_default_prompt(
	const gchar *cwd,
	const gchar *config_dir,
	const gchar *home,
	gchar      **origin,
	gboolean    *truncated
);

/**
 * ai_loop_schedule_command:
 * @self: a schedule
 * @arguments: (nullable): the text after `/loop`
 * @cwd: (nullable): working directory, for the default prompt
 * @config_dir: (nullable): XDG config directory
 * @home: (nullable): home directory
 * @now_us: real time in microseconds, so a test can freeze the clock
 * @error: (nullable): return location for a #GError
 *
 * Applies one `/loop` command: schedule, list, cancel, or stop.
 *
 * Returns: (transfer full) (nullable): the line to show the user, or %NULL
 *   on error
 */
gchar *
ai_loop_schedule_command(
	AiLoopSchedule *self,
	const gchar    *arguments,
	const gchar    *cwd,
	const gchar    *config_dir,
	const gchar    *home,
	gint64          now_us,
	GError        **error
);

/**
 * ai_loop_schedule_get_n_tasks:
 * @self: a schedule
 *
 * Returns: how many tasks are stored
 */
guint
ai_loop_schedule_get_n_tasks(AiLoopSchedule *self);

/**
 * ai_loop_schedule_get_id:
 * @self: a schedule
 * @index: zero-based index
 *
 * Returns: (nullable) (transfer none): the task id, or %NULL
 */
const gchar *
ai_loop_schedule_get_id(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_get_cron:
 * @self: a schedule
 * @index: zero-based index
 *
 * Returns: (nullable) (transfer none): the cron expression, or %NULL for a
 *   self-paced task
 */
const gchar *
ai_loop_schedule_get_cron(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_get_prompt:
 * @self: a schedule
 * @index: zero-based index
 *
 * Returns: (nullable) (transfer none): the stored prompt, or %NULL when the
 *   task uses the default maintenance prompt
 */
const gchar *
ai_loop_schedule_get_prompt(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_get_cadence:
 * @self: a schedule
 * @index: zero-based index
 *
 * Returns: (nullable) (transfer none): a short description such as "every 5m"
 */
const gchar *
ai_loop_schedule_get_cadence(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_get_nominal_us:
 * @self: a schedule
 * @index: zero-based index
 *
 * The cron instant before the per-task offset is added. Zero for a
 * self-paced task.
 *
 * Returns: microseconds since the Unix epoch
 */
gint64
ai_loop_schedule_get_nominal_us(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_get_fire_us:
 * @self: a schedule
 * @index: zero-based index
 *
 * Returns: when the task is next due, in microseconds
 */
gint64
ai_loop_schedule_get_fire_us(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_get_expires_us:
 * @self: a schedule
 * @index: zero-based index
 *
 * Returns: when a fixed task expires, or 0 for a self-paced task
 */
gint64
ai_loop_schedule_get_expires_us(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_get_interval_us:
 * @self: a schedule
 * @index: zero-based index
 *
 * Returns: the fixed interval, or 0 for a self-paced task
 */
gint64
ai_loop_schedule_get_interval_us(AiLoopSchedule *self, guint index);

/**
 * ai_loop_schedule_id_is_dynamic:
 * @self: a schedule
 * @id: task id
 *
 * Returns: %TRUE when @id is a self-paced loop
 */
gboolean
ai_loop_schedule_id_is_dynamic(AiLoopSchedule *self, const gchar *id);

/**
 * ai_loop_schedule_due:
 * @self: a schedule
 * @now_us: real time in microseconds
 *
 * The task that should run, if the session is idle. Missed ticks collapse
 * to one fire. A disabled schedule is never due.
 *
 * Returns: (nullable) (transfer none): a task id
 */
const gchar *
ai_loop_schedule_due(AiLoopSchedule *self, gint64 now_us);

/**
 * ai_loop_schedule_dup_prompt:
 * @self: a schedule
 * @id: task id
 * @cwd: (nullable): working directory
 * @config_dir: (nullable): XDG config directory
 * @home: (nullable): home directory
 *
 * The text to send for @id, including the self-paced follow-up instruction
 * when the task chooses its own delay. The default prompt is read now, so
 * an edit to loop.md applies on the next fire.
 *
 * Returns: (transfer full) (nullable): the prompt, or %NULL if @id is gone
 */
gchar *
ai_loop_schedule_dup_prompt(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *cwd,
	const gchar    *config_dir,
	const gchar    *home
);

/**
 * ai_loop_schedule_note_fired:
 * @self: a schedule
 * @id: task id
 * @now_us: real time in microseconds
 * @notice: (out) (optional) (transfer full) (nullable): a line to show
 *
 * Records that @id is being sent. A fixed task moves to its next slot, or
 * is dropped when this fire is the one past its seven-day expiry. A
 * self-paced task stays until ai_loop_schedule_complete().
 *
 * Returns: %FALSE when @id is not in the schedule
 */
gboolean
ai_loop_schedule_note_fired(
	AiLoopSchedule *self,
	const gchar    *id,
	gint64          now_us,
	gchar         **notice
);

/**
 * ai_loop_schedule_complete:
 * @self: a schedule
 * @id: task id of a self-paced loop
 * @assistant_text: (nullable): the model's reply
 * @now_us: real time in microseconds
 *
 * Reads a trailing `LOOP_NEXT` or `LOOP_STOP` line. Without one, the loop
 * waits 20 minutes once, then stops if the next reply also omits it.
 *
 * Returns: (transfer full) (nullable): a line to show, or %NULL if @id is
 *   not a self-paced task
 */
gchar *
ai_loop_schedule_complete(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *assistant_text,
	gint64          now_us
);

/**
 * ai_loop_schedule_stop_waiting:
 * @self: a schedule
 *
 * Cancels self-paced loops. A loop whose turn is already running is told
 * not to schedule another one. Fixed-interval loops are left alone.
 *
 * Returns: (transfer full) (nullable): a line to show, or %NULL when nothing
 *   was waiting
 */
gchar *
ai_loop_schedule_stop_waiting(AiLoopSchedule *self);

/**
 * ai_loop_schedule_clear:
 * @self: a schedule
 *
 * Drops every task. A fresh session does this.
 */
void
ai_loop_schedule_clear(AiLoopSchedule *self);

/**
 * ai_loop_schedule_clear_inflight:
 * @self: a schedule
 * @id: (nullable): task id
 *
 * Forgets that a fire is in progress, so a send that never started can
 * become due again.
 */
void
ai_loop_schedule_clear_inflight(AiLoopSchedule *self, const gchar *id);

/**
 * ai_loop_schedule_save:
 * @self: a schedule
 * @path: destination file
 * @error: (nullable): return location for a #GError
 *
 * Writes fixed tasks as JSON, mode 0600. Self-paced tasks are omitted.
 * Refuses to follow a symlink at @path or its parent directory.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_save(AiLoopSchedule *self, const gchar *path, GError **error);

/**
 * ai_loop_schedule_load:
 * @self: a schedule
 * @path: source file
 * @now_us: real time; expired tasks are dropped
 * @error: (nullable): return location for a #GError
 *
 * Replaces @self with the fixed tasks in @path. A missing file loads
 * nothing and returns 0. Self-paced tasks are not restored.
 *
 * Returns: the number of tasks loaded, or -1 on error
 */
gint
ai_loop_schedule_load(
	AiLoopSchedule *self,
	const gchar    *path,
	gint64          now_us,
	GError        **error
);

/* ----------------------------------------------------------------
 * Goals, editing and the shared vocabulary. Documented in ai-loop.c.
 * ---------------------------------------------------------------- */

void
ai_loop_schedule_set_commands(AiLoopSchedule *self, AiCommandSet *commands);

gboolean
ai_loop_schedule_is_unattended_safe(AiLoopSchedule *self, const gchar *text);

gint
ai_loop_schedule_find(AiLoopSchedule *self, const gchar *id);

AiLoopKind
ai_loop_schedule_get_kind(AiLoopSchedule *self, guint index);

AiLoopState
ai_loop_schedule_get_state(AiLoopSchedule *self, guint index);

const gchar *
ai_loop_schedule_get_condition(AiLoopSchedule *self, guint index);

const gchar *
ai_loop_schedule_get_reason(AiLoopSchedule *self, guint index);

guint
ai_loop_schedule_get_turns(AiLoopSchedule *self, guint index);

guint
ai_loop_schedule_get_max_turns(AiLoopSchedule *self, guint index);

gint64
ai_loop_schedule_get_deadline_us(AiLoopSchedule *self, guint index);

gboolean
ai_loop_schedule_get_inflight(AiLoopSchedule *self, guint index);

guint
ai_loop_schedule_count_live(AiLoopSchedule *self, AiLoopKind kind);

gint64
ai_loop_schedule_get_next_fire_us(AiLoopSchedule *self);

guint64
ai_loop_schedule_get_revision(AiLoopSchedule *self);

gchar *
ai_loop_schedule_resolve_id(
	AiLoopSchedule *self,
	const gchar    *text,
	GError        **error
);

gchar *
ai_loop_schedule_add_loop(
	AiLoopSchedule *self,
	gint64          interval_us,
	const gchar    *prompt,
	gint64          now_us,
	gchar         **notice,
	GError        **error
);

gchar *
ai_loop_schedule_add_goal(
	AiLoopSchedule *self,
	const gchar    *condition,
	guint           max_turns,
	gint64          max_duration_us,
	gint64          now_us,
	GError        **error
);

gboolean
ai_loop_schedule_pause(
	AiLoopSchedule *self,
	const gchar    *id,
	GError        **error
);

gboolean
ai_loop_schedule_resume(
	AiLoopSchedule *self,
	const gchar    *id,
	gint64          now_us,
	GError        **error
);

gboolean
ai_loop_schedule_remove(
	AiLoopSchedule *self,
	const gchar    *id,
	GError        **error
);

gboolean
ai_loop_schedule_run_now(
	AiLoopSchedule *self,
	const gchar    *id,
	gint64          now_us,
	GError        **error
);

gboolean
ai_loop_schedule_stop_goal(
	AiLoopSchedule *self,
	const gchar    *id,
	gint64          now_us,
	GError        **error
);

gboolean
ai_loop_schedule_set_interval(
	AiLoopSchedule *self,
	const gchar    *id,
	gint64          interval_us,
	gint64          now_us,
	GError        **error
);

gboolean
ai_loop_schedule_set_prompt(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *prompt,
	GError        **error
);

gboolean
ai_loop_schedule_set_condition(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *condition,
	GError        **error
);

gboolean
ai_loop_schedule_set_bounds(
	AiLoopSchedule *self,
	const gchar    *id,
	guint           max_turns,
	gint64          max_duration_us,
	gint64          now_us,
	GError        **error
);

gchar *
ai_loop_schedule_goal_complete(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *assistant_text,
	const gchar    *turn_error,
	gint64          now_us
);

gchar *
ai_loop_schedule_reap(AiLoopSchedule *self, gint64 now_us);

gchar *
ai_loop_schedule_goal_command(
	AiLoopSchedule *self,
	const gchar    *arguments,
	gint64          now_us,
	GError        **error
);

gchar *
ai_loop_schedule_dup_status(
	AiLoopSchedule *self,
	guint           index,
	gint64          now_us
);

gchar *
ai_loop_schedule_dup_excerpt(AiLoopSchedule *self, guint index);

gchar *
ai_loop_schedule_dup_line(
	AiLoopSchedule *self,
	guint           index,
	gint64          now_us
);

gchar *
ai_loop_schedule_dup_details(
	AiLoopSchedule *self,
	guint           index,
	gint64          now_us
);

gchar *
ai_loop_schedule_dup_summary(AiLoopSchedule *self, gint64 now_us);

gchar *
ai_loop_schedule_dup_json(AiLoopSchedule *self, gint64 now_us);

gint
ai_loop_schedule_sync(
	AiLoopSchedule *self,
	const gchar    *path,
	gint64          now_us,
	GError        **error
);

/* ----------------------------------------------------------------
 * The store: one file per owning session, under XDG state.
 * ---------------------------------------------------------------- */

gchar *
ai_loop_store_default_directory(void);

gchar *
ai_loop_store_path(const gchar *directory, const gchar *owner);

gchar **
ai_loop_store_list_owners(const gchar *directory);

gint
ai_loop_store_claim(
	const gchar *directory,
	const gchar *owner,
	GError     **error
);

gboolean
ai_loop_store_remove(
	const gchar *directory,
	const gchar *owner,
	GError     **error
);

gboolean
ai_loop_store_set_resume(
	const gchar *directory,
	const gchar *key,
	const gchar *owner,
	GError     **error
);

gchar *
ai_loop_store_dup_resume(const gchar *directory, const gchar *key);

gchar *
ai_loop_store_dup_summary(
	const gchar *directory,
	const gchar *owner,
	gint64       now_us
);

G_END_DECLS
