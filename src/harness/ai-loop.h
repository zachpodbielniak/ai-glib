/*
 * ai-loop.h - Session-scoped scheduled prompts
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

G_BEGIN_DECLS

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

G_END_DECLS
