/*
 * ai-loop.c - Scheduled prompts (loops) and goals for one session
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * /loop is the Claude Code scheduled-task shape, kept inside one session:
 * a fixed cron interval, a delay the model chooses, or the maintenance
 * prompt. /goal is the other half: turns that continue until a condition
 * holds, judged after each one, with a turn bound and a time bound that
 * end it either way.
 *
 * Every function takes the time as an argument, so a test can run a
 * week of schedule in a microsecond. Nothing here sleeps or owns a
 * main-loop source; AiLoopRunner does, and it calls in.
 *
 * The words every frontend shows -- state names, "in 4m", "turn 3/20" --
 * are produced here and nowhere else, the same arrangement ai-quota.h
 * has for quota. Two frontends that formatted a schedule separately
 * would disagree about it within a month.
 */

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "core/ai-error.h"
#include "core/ai-json-util.h"
#include "harness/ai-command.h"
#include "harness/ai-loop.h"

#define LOOP_MAX_TASKS           50
#define LOOP_PROMPT_MAX          (64 * 1024)
#define LOOP_FILE_MAX            (2 * 1024 * 1024)
#define LOOP_MD_MAX              25000
#define LOOP_EXPIRY_US           ((gint64)7 * 24 * 60 * 60 * G_USEC_PER_SEC)
#define LOOP_MIN_US              (60 * G_USEC_PER_SEC)
#define LOOP_DYNAMIC_MAX_MINUTES 60
#define LOOP_FALLBACK_US         ((gint64)20 * 60 * G_USEC_PER_SEC)
#define LOOP_JITTER_CAP_SEC      (30 * 60)
#define LOOP_MAX_ENTRIES         100
#define GOAL_MAX_LIVE            20
#define GOAL_MAX_FINISHED        30
#define GOAL_CONDITION_MAX       (8 * 1024)
#define GOAL_DEFAULT_TURNS       20
#define GOAL_MAX_TURNS           200
#define GOAL_DEFAULT_US          ((gint64)2 * 60 * 60 * G_USEC_PER_SEC)
#define GOAL_MAX_US              LOOP_EXPIRY_US
#define GOAL_MAX_ERRORS          3
#define GOAL_RETRY_US            (60 * G_USEC_PER_SEC)
#define LOOP_FILE_VERSION        2

static const gint MINUTE_STEPS[] = { 1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60 };
static const gint HOUR_STEPS[] = { 1, 2, 3, 4, 6, 8, 12, 24 };

static const gchar MAINTENANCE_PROMPT[] =
	"Continue this session. On this pass do the first item that applies, and nothing after it:\n"
	"\n"
	"1. Finish unfinished work already started in this conversation.\n"
	"2. If this branch has a pull request, address review comments, failed checks, and merge conflicts.\n"
	"3. If nothing is pending, do one small cleanup on the current change: a bug visible from the diff, or a simplification that preserves behavior.\n"
	"\n"
	"Do not start a new initiative. Do not push, delete, or do anything else that is hard to undo unless this conversation already asked for that action.\n";

static const gchar DYNAMIC_INSTRUCTION[] =
	"\n\nEnd your reply with exactly one final line and nothing after it:\n"
	"LOOP_NEXT: <minutes>m <why this delay>\n"
	"or\n"
	"LOOP_STOP: <why the loop is finished>\n"
	"Use a delay from 1 to 60 minutes. Short while something is still in progress, longer when it is quiet.\n";

typedef struct
{
	gboolean any;
	gboolean on[64];
} CronField;

static const gchar GOAL_INSTRUCTION[] =
	"\n\nWhen you stop for this turn, end your reply with exactly one final line and nothing after it:\n"
	"GOAL_MET: <the evidence that the condition holds now>\n"
	"or\n"
	"GOAL_NOT_MET: <what still remains>\n"
	"or\n"
	"GOAL_BLOCKED: <why it cannot be met without the user>\n"
	"Say GOAL_MET only after checking that the condition holds, not because you expect it to.\n";

typedef struct
{
	CronField minute;
	CronField hour;
	CronField dom;
	CronField month;
	CronField dow;
} Cron;

typedef struct
{
	gchar   *id;
	gboolean dynamic;
	gchar   *cron_text;
	Cron     cron;
	gboolean cron_ok;
	gchar   *prompt;
	gchar   *cadence;
	gint64   created_us;
	gint64   expires_us;
	gint64   nominal_us;
	gint64   fire_us;
	gint64   interval_us;
	gboolean inflight;
	gboolean fallback_armed;
	gboolean stop_requested;
	gboolean run_requested;
	AiLoopKind  kind;
	AiLoopState state;
	gchar      *condition;
	gchar      *reason;
	guint       turns;
	guint       max_turns;
	gint64      max_duration_us;
	gint64      deadline_us;
	gint64      ended_us;
	guint       errors;
} Task;

struct _AiLoopSchedule
{
	GObject    parent_instance;
	GPtrArray *tasks;
	AiCommandSet *commands;
	guint64    revision;
	/* What ai_loop_schedule_sync() last saw on disk, so an unchanged
	 * file is not parsed once a second. */
	gint64     seen_mtime;
	gint64     seen_size;
	guint64    seen_inode;
};

G_DEFINE_TYPE(AiLoopSchedule, ai_loop_schedule, G_TYPE_OBJECT)

static gboolean check_command(AiLoopSchedule *self, const gchar *prompt, GError **error);

static void
task_free(gpointer data)
{
	Task *task = data;

	if (task == NULL)
	{
		return;
	}

	g_free(task->id);
	g_free(task->cron_text);
	g_free(task->prompt);
	g_free(task->cadence);
	g_free(task->condition);
	g_free(task->reason);
	g_free(task);
}

static void
ai_loop_schedule_finalize(GObject *object)
{
	AiLoopSchedule *self = AI_LOOP_SCHEDULE(object);

	g_clear_pointer(&self->tasks, g_ptr_array_unref);
	g_clear_object(&self->commands);
	G_OBJECT_CLASS(ai_loop_schedule_parent_class)->finalize(object);
}

static void
ai_loop_schedule_class_init(AiLoopScheduleClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ai_loop_schedule_finalize;
}

static void
ai_loop_schedule_init(AiLoopSchedule *self)
{
	self->tasks = g_ptr_array_new_with_free_func(task_free);
	self->seen_size = -1;
}

AiLoopSchedule *
ai_loop_schedule_new(void)
{
	return g_object_new(AI_TYPE_LOOP_SCHEDULE, NULL);
}

gboolean
ai_loop_schedule_is_disabled(void)
{
	const gchar *value = g_getenv("AI_LOOP_DISABLE");

	if (value == NULL || value[0] == '\0' || g_strcmp0(value, "0") == 0)
	{
		return FALSE;
	}

	if (g_ascii_strcasecmp(value, "false") == 0 ||
	    g_ascii_strcasecmp(value, "no") == 0 ||
	    g_ascii_strcasecmp(value, "off") == 0)
	{
		return FALSE;
	}

	return TRUE;
}

/* ----------------------------------------------------------------
 * Vocabulary
 * ---------------------------------------------------------------- */

GType
ai_loop_kind_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ AI_LOOP_KIND_LOOP, "AI_LOOP_KIND_LOOP", "loop" },
			{ AI_LOOP_KIND_GOAL, "AI_LOOP_KIND_GOAL", "goal" },
			{ 0, NULL, NULL }
		};
		GType type = g_enum_register_static("AiLoopKind", values);

		g_once_init_leave(&type_id, type);
	}

	return type_id;
}

GType
ai_loop_state_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ AI_LOOP_STATE_ACTIVE, "AI_LOOP_STATE_ACTIVE", "active" },
			{ AI_LOOP_STATE_PAUSED, "AI_LOOP_STATE_PAUSED", "paused" },
			{ AI_LOOP_STATE_MET, "AI_LOOP_STATE_MET", "met" },
			{ AI_LOOP_STATE_FAILED, "AI_LOOP_STATE_FAILED", "failed" },
			{ AI_LOOP_STATE_STOPPED, "AI_LOOP_STATE_STOPPED", "stopped" },
			{ AI_LOOP_STATE_EXPIRED, "AI_LOOP_STATE_EXPIRED", "expired" },
			{ 0, NULL, NULL }
		};
		GType type = g_enum_register_static("AiLoopState", values);

		g_once_init_leave(&type_id, type);
	}

	return type_id;
}

/**
 * ai_loop_kind_to_string:
 * @kind: a kind
 *
 * Returns: (transfer none): "loop" or "goal"
 */
const gchar *
ai_loop_kind_to_string(AiLoopKind kind)
{
	return kind == AI_LOOP_KIND_GOAL ? "goal" : "loop";
}

/**
 * ai_loop_state_to_string:
 * @state: a state
 *
 * The word every frontend shows for @state. Nothing else spells them.
 *
 * Returns: (transfer none): "active", "paused", "met", "failed",
 *   "stopped" or "expired"
 */
const gchar *
ai_loop_state_to_string(AiLoopState state)
{
	switch (state)
	{
	case AI_LOOP_STATE_ACTIVE:
		return "active";
	case AI_LOOP_STATE_PAUSED:
		return "paused";
	case AI_LOOP_STATE_MET:
		return "met";
	case AI_LOOP_STATE_FAILED:
		return "failed";
	case AI_LOOP_STATE_STOPPED:
		return "stopped";
	case AI_LOOP_STATE_EXPIRED:
		return "expired";
	default:
		return "unknown";
	}
}

static gboolean
state_from_string(const gchar *text, AiLoopState *state)
{
	AiLoopState candidate;

	for (candidate = AI_LOOP_STATE_ACTIVE; candidate <= AI_LOOP_STATE_EXPIRED; candidate++)
	{
		if (g_strcmp0(text, ai_loop_state_to_string(candidate)) == 0)
		{
			*state = candidate;
			return TRUE;
		}
	}

	return FALSE;
}

/**
 * ai_loop_state_is_final:
 * @state: a state
 *
 * Returns: %TRUE for met, failed, stopped and expired -- a goal in one of
 *   those will never take another turn
 */
gboolean
ai_loop_state_is_final(AiLoopState state)
{
	return state == AI_LOOP_STATE_MET || state == AI_LOOP_STATE_FAILED ||
	       state == AI_LOOP_STATE_STOPPED || state == AI_LOOP_STATE_EXPIRED;
}

/**
 * ai_loop_format_duration:
 * @duration_us: a span in microseconds
 *
 * A span the way every frontend writes it: "<1m", "4m", "2h 5m", "3d 4h".
 * Rounded down, so a timer never claims more time than is left; anything
 * under a minute is "<1m" rather than a "0m" that reads as "now".
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_loop_format_duration(gint64 duration_us)
{
	gint64 minutes;

	if (duration_us < 0)
	{
		duration_us = -duration_us;
	}

	minutes = duration_us / (60 * G_USEC_PER_SEC);

	if (minutes < 1)
	{
		return g_strdup("<1m");
	}

	if (minutes < 60)
	{
		return g_strdup_printf("%" G_GINT64_FORMAT "m", minutes);
	}

	if (minutes < 24 * 60)
	{
		return minutes % 60 == 0
			? g_strdup_printf("%" G_GINT64_FORMAT "h", minutes / 60)
			: g_strdup_printf("%" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m",
			                  minutes / 60, minutes % 60);
	}

	return (minutes / 60) % 24 == 0
		? g_strdup_printf("%" G_GINT64_FORMAT "d", minutes / (24 * 60))
		: g_strdup_printf("%" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h",
		                  minutes / (24 * 60), (minutes / 60) % 24);
}

/**
 * ai_loop_format_relative:
 * @delta_us: target minus now, in microseconds
 *
 * "in 4m" for the future, "now" for anything already due, "5m ago" is
 * never produced: a due entry is due, however late.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_loop_format_relative(gint64 delta_us)
{
	g_autofree gchar *span = NULL;

	if (delta_us <= 0)
	{
		return g_strdup("now");
	}

	span = ai_loop_format_duration(delta_us);
	return g_strdup_printf("in %s", span);
}

static gboolean
task_is_live(const Task *task)
{
	return !ai_loop_state_is_final(task->state);
}

static guint
count_kind(AiLoopSchedule *self, AiLoopKind kind, gboolean live_only)
{
	guint i;
	guint n = 0;

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (task->kind == kind && (!live_only || task_is_live(task)))
		{
			n++;
		}
	}

	return n;
}

static Task *
task_at(AiLoopSchedule *self, guint index)
{
	if (self == NULL || index >= self->tasks->len)
	{
		return NULL;
	}

	return g_ptr_array_index(self->tasks, index);
}

static Task *
task_by_id(AiLoopSchedule *self, const gchar *id)
{
	guint i;

	if (self == NULL || id == NULL)
	{
		return NULL;
	}

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (g_strcmp0(task->id, id) == 0)
		{
			return task;
		}
	}

	return NULL;
}

static void
task_remove(AiLoopSchedule *self, Task *task)
{
	guint i;

	for (i = 0; i < self->tasks->len; i++)
	{
		if (g_ptr_array_index(self->tasks, i) == task)
		{
			g_ptr_array_remove_index(self->tasks, i);
			return;
		}
	}
}

guint
ai_loop_schedule_get_n_tasks(AiLoopSchedule *self)
{
	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), 0);
	return self->tasks->len;
}

const gchar *
ai_loop_schedule_get_id(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->id : NULL;
}

const gchar *
ai_loop_schedule_get_cron(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->cron_text : NULL;
}

const gchar *
ai_loop_schedule_get_prompt(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->prompt : NULL;
}

const gchar *
ai_loop_schedule_get_cadence(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->cadence : NULL;
}

gint64
ai_loop_schedule_get_nominal_us(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->nominal_us : 0;
}

gint64
ai_loop_schedule_get_fire_us(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->fire_us : 0;
}

gint64
ai_loop_schedule_get_expires_us(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->expires_us : 0;
}

gint64
ai_loop_schedule_get_interval_us(AiLoopSchedule *self, guint index)
{
	Task *task = task_at(self, index);
	return task != NULL ? task->interval_us : 0;
}

gboolean
ai_loop_schedule_id_is_dynamic(AiLoopSchedule *self, const gchar *id)
{
	Task *task = task_by_id(self, id);
	return task != NULL && task->dynamic;
}

void
ai_loop_schedule_clear(AiLoopSchedule *self)
{
	g_return_if_fail(AI_IS_LOOP_SCHEDULE(self));
	g_ptr_array_set_size(self->tasks, 0);
}

void
ai_loop_schedule_clear_inflight(AiLoopSchedule *self, const gchar *id)
{
	Task *task;

	g_return_if_fail(AI_IS_LOOP_SCHEDULE(self));
	task = task_by_id(self, id);

	if (task != NULL)
	{
		/* A goal turn that never started is not one of its turns. */
		if (task->inflight && task->kind == AI_LOOP_KIND_GOAL && task->turns > 0)
		{
			task->turns--;
		}

		task->inflight = FALSE;
	}
}

/* ----------------------------------------------------------------
 * Cron
 * ---------------------------------------------------------------- */

static gboolean
parse_number(const gchar *text, const gchar **end, gint64 *out)
{
	gchar *stopped = NULL;
	gint64 value;

	errno = 0;
	value = g_ascii_strtoll(text, &stopped, 10);

	if (stopped == text || errno == ERANGE || value < 0)
	{
		return FALSE;
	}

	*end = stopped;
	*out = value;
	return TRUE;
}

static void
field_set(CronField *field, gint value)
{
	if (value >= 0 && value < (gint)G_N_ELEMENTS(field->on))
	{
		field->on[value] = TRUE;
	}
}

static gboolean
field_fill(CronField *field, gint lo, gint hi, gint start, gint end, gint step, GError **error)
{
	gint value;

	if (step < 1 || start < lo || end > hi || start > end)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Interval does not fit a cron field.");
		return FALSE;
	}

	for (value = start; value <= end; value += step)
	{
		field_set(field, value);
	}

	return TRUE;
}

static gboolean
parse_part(CronField *field, gint lo, gint hi, const gchar *text, GError **error)
{
	const gchar *end = NULL;
	gint64       start;
	gint64       stop;
	gint64       step = 1;

	if (g_strcmp0(text, "*") == 0)
	{
		field->any = TRUE;
		return TRUE;
	}

	if (g_str_has_prefix(text, "*/"))
	{
		if (!parse_number(text + 2, &end, &step) || *end != '\0')
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                    "Invalid cron step.");
			return FALSE;
		}

		return field_fill(field, lo, hi, lo, hi, (gint)step, error);
	}

	if (!parse_number(text, &end, &start))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Invalid cron field.");
		return FALSE;
	}

	stop = start;

	if (*end == '-')
	{
		if (!parse_number(end + 1, &end, &stop))
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                    "Invalid cron range.");
			return FALSE;
		}
	}

	if (*end == '/')
	{
		if (!parse_number(end + 1, &end, &step))
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                    "Invalid cron step.");
			return FALSE;
		}
	}

	if (*end != '\0')
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Invalid cron field.");
		return FALSE;
	}

	/* 7 is Sunday, the same day as 0. */
	if (lo == 0 && hi == 7)
	{
		if (start == 7)
		{
			start = 0;
		}

		if (stop == 7)
		{
			stop = 0;
		}

		if (start == 0 && stop == 0)
		{
			field_set(field, 0);
			return TRUE;
		}
	}

	return field_fill(field, lo, hi, (gint)start, (gint)stop, (gint)step, error);
}

static gboolean
parse_field(CronField *field, gint lo, gint hi, const gchar *text, GError **error)
{
	g_auto(GStrv) parts = NULL;
	guint         i;

	memset(field, 0, sizeof *field);
	parts = g_strsplit(text, ",", -1);

	for (i = 0; parts[i] != NULL; i++)
	{
		if (parts[i][0] == '\0' ||
		    !parse_part(field, lo, hi, parts[i], error))
		{
			if (error != NULL && *error == NULL)
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				                    "Invalid cron field.");
			}

			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
cron_parse(Cron *cron, const gchar *text, GError **error)
{
	g_autofree gchar *copy = NULL;
	g_auto(GStrv)     fields = NULL;
	g_autoptr(GPtrArray) kept = NULL;
	guint             n;

	memset(cron, 0, sizeof *cron);
	copy = g_strstrip(g_strdup(text != NULL ? text : ""));
	fields = g_strsplit_set(copy, " \t", -1);
	/* g_strsplit_set keeps empty tokens around repeated spaces. Drop them. */
	kept = g_ptr_array_new_with_free_func(g_free);

	for (n = 0; fields[n] != NULL; n++)
	{
		if (fields[n][0] != '\0')
		{
			g_ptr_array_add(kept, g_strdup(fields[n]));
		}
	}

	g_ptr_array_add(kept, NULL);
	g_strfreev(fields);
	fields = (GStrv)g_ptr_array_free(g_steal_pointer(&kept), FALSE);

	n = g_strv_length(fields);

	if (n != 5 ||
	    !parse_field(&cron->minute, 0, 59, fields[0], error) ||
	    !parse_field(&cron->hour, 0, 23, fields[1], error) ||
	    !parse_field(&cron->dom, 1, 31, fields[2], error) ||
	    !parse_field(&cron->month, 1, 12, fields[3], error) ||
	    !parse_field(&cron->dow, 0, 7, fields[4], error))
	{
		if (error != NULL && *error == NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                    "A cron expression needs five fields.");
		}

		return FALSE;
	}

	return TRUE;
}

static gboolean
field_hit(const CronField *field, gint value)
{
	if (field->any)
	{
		return TRUE;
	}

	if (value < 0 || value >= (gint)G_N_ELEMENTS(field->on))
	{
		return FALSE;
	}

	return field->on[value];
}

static gboolean
cron_matches(const Cron *cron, const struct tm *when)
{
	gboolean dom_ok;
	gboolean dow_ok;
	gboolean day_ok;

	if (!field_hit(&cron->minute, when->tm_min) ||
	    !field_hit(&cron->hour, when->tm_hour) ||
	    !field_hit(&cron->month, when->tm_mon + 1))
	{
		return FALSE;
	}

	dom_ok = field_hit(&cron->dom, when->tm_mday);
	dow_ok = field_hit(&cron->dow, when->tm_wday);

	/* Both day fields constrained: either one is enough (vixie cron). */
	if (!cron->dom.any && !cron->dow.any)
	{
		day_ok = dom_ok || dow_ok;
	}
	else
	{
		day_ok = dom_ok && dow_ok;
	}

	return day_ok;
}

static gint64
cron_next_us(const Cron *cron, gint64 after_us)
{
	time_t start;
	time_t limit;
	time_t cursor;

	start = (time_t)(after_us / G_USEC_PER_SEC);
	start = (start / 60) * 60 + 60;
	limit = start + (time_t)366 * 24 * 60 * 60;

	for (cursor = start; cursor < limit; cursor += 60)
	{
		struct tm when;

		if (localtime_r(&cursor, &when) != NULL && cron_matches(cron, &when))
		{
			return (gint64)cursor * G_USEC_PER_SEC;
		}
	}

	return 0;
}

static gint64
jitter_us(const gchar *id, gint64 interval_us)
{
	guint32 span;
	guint32 pick;

	if (interval_us >= (gint64)3600 * G_USEC_PER_SEC)
	{
		span = LOOP_JITTER_CAP_SEC;
	}
	else
	{
		span = (guint32)(interval_us / (2 * G_USEC_PER_SEC));
	}

	if (span < 1)
	{
		span = 1;
	}

	pick = g_str_hash(id) % (span + 1);
	return (gint64)pick * G_USEC_PER_SEC;
}

/* ----------------------------------------------------------------
 * Intervals
 * ---------------------------------------------------------------- */

static gboolean
unit_seconds(const gchar *token, gint64 *factor)
{
	if (g_ascii_strcasecmp(token, "s") == 0 ||
	    g_ascii_strcasecmp(token, "sec") == 0 ||
	    g_ascii_strcasecmp(token, "secs") == 0 ||
	    g_ascii_strcasecmp(token, "second") == 0 ||
	    g_ascii_strcasecmp(token, "seconds") == 0)
	{
		*factor = 1;
		return TRUE;
	}

	if (g_ascii_strcasecmp(token, "m") == 0 ||
	    g_ascii_strcasecmp(token, "min") == 0 ||
	    g_ascii_strcasecmp(token, "mins") == 0 ||
	    g_ascii_strcasecmp(token, "minute") == 0 ||
	    g_ascii_strcasecmp(token, "minutes") == 0)
	{
		*factor = 60;
		return TRUE;
	}

	if (g_ascii_strcasecmp(token, "h") == 0 ||
	    g_ascii_strcasecmp(token, "hr") == 0 ||
	    g_ascii_strcasecmp(token, "hrs") == 0 ||
	    g_ascii_strcasecmp(token, "hour") == 0 ||
	    g_ascii_strcasecmp(token, "hours") == 0)
	{
		*factor = 3600;
		return TRUE;
	}

	if (g_ascii_strcasecmp(token, "d") == 0 ||
	    g_ascii_strcasecmp(token, "day") == 0 ||
	    g_ascii_strcasecmp(token, "days") == 0)
	{
		*factor = 86400;
		return TRUE;
	}

	return FALSE;
}

static gint
nearest_step(gint value, const gint *steps, guint n)
{
	guint i;
	gint  best = steps[0];
	gint  best_dist = ABS(value - best);

	for (i = 1; i < n; i++)
	{
		gint dist = ABS(value - steps[i]);

		if (dist < best_dist || (dist == best_dist && steps[i] > best))
		{
			best = steps[i];
			best_dist = dist;
		}
	}

	return best;
}

static gint
snap_minutes(gint64 seconds)
{
	gint64 minutes;

	if (seconds > (gint64)366 * 86400)
	{
		seconds = (gint64)366 * 86400;
	}

	minutes = (seconds + 59) / 60;
	gint   snapped;

	if (minutes < 1)
	{
		minutes = 1;
	}

	if (minutes <= 60)
	{
		return nearest_step((gint)minutes, MINUTE_STEPS, G_N_ELEMENTS(MINUTE_STEPS));
	}

	snapped = 60;
	{
		guint i;
		gint  best_dist = ABS((gint)minutes - snapped);

		for (i = 0; i < G_N_ELEMENTS(HOUR_STEPS); i++)
		{
			gint slot = HOUR_STEPS[i] * 60;
			gint dist = ABS((gint)minutes - slot);

			if (dist < best_dist || (dist == best_dist && slot > snapped))
			{
				snapped = slot;
				best_dist = dist;
			}
		}
	}

	return snapped;
}

typedef struct
{
	gboolean has_interval;
	gint64   seconds;
	gchar   *label;
	gchar   *prompt;
} ParsedLoop;

static void
parsed_clear(ParsedLoop *parsed)
{
	g_free(parsed->label);
	g_free(parsed->prompt);
	memset(parsed, 0, sizeof *parsed);
}

static gboolean
parse_leading_interval(const gchar *text, gint64 *seconds, const gchar **rest)
{
	const gchar *end = NULL;
	gint64       value;
	gint64       factor = 0;
	gchar        unit[2];

	while (g_ascii_isspace(*text))
	{
		text++;
	}

	if (!parse_number(text, &end, &value) || value <= 0 || value > 1000000)
	{
		return FALSE;
	}

	if (*end == '\0' || g_ascii_isspace(*end))
	{
		return FALSE;
	}

	unit[0] = *end;
	unit[1] = '\0';

	if (!unit_seconds(unit, &factor))
	{
		return FALSE;
	}

	end++;

	if (*end != '\0' && !g_ascii_isspace(*end))
	{
		return FALSE;
	}

	*seconds = value * factor;
	*rest = end;
	return TRUE;
}

static const gchar *
last_every(const gchar *text)
{
	const gchar *cursor = text;
	const gchar *found = NULL;

	while (*cursor != '\0')
	{
		if ((cursor == text || g_ascii_isspace(cursor[-1])) &&
		    g_ascii_strncasecmp(cursor, "every", 5) == 0 &&
		    (cursor[5] == '\0' || g_ascii_isspace(cursor[5])))
		{
			found = cursor;
		}

		cursor++;
	}

	return found;
}

static gboolean
parse_suffix_interval(const gchar *text, gint64 *seconds, gchar **label, const gchar **prompt_end)
{
	const gchar *every;
	const gchar *cursor;
	const gchar *number_end = NULL;
	const gchar *token_end;
	gint64       value;
	gint64       factor = 0;
	gchar       *token;

	every = last_every(text);

	if (every == NULL)
	{
		return FALSE;
	}

	cursor = every + 5;

	while (g_ascii_isspace(*cursor))
	{
		cursor++;
	}

	if (!parse_number(cursor, &number_end, &value) || value <= 0 || value > 1000000)
	{
		return FALSE;
	}

	cursor = number_end;

	while (g_ascii_isspace(*cursor))
	{
		cursor++;
	}

	token_end = cursor;

	while (*token_end != '\0' && !g_ascii_isspace(*token_end))
	{
		token_end++;
	}

	token = g_strndup(cursor, (gsize)(token_end - cursor));

	if (!unit_seconds(token, &factor))
	{
		g_free(token);
		return FALSE;
	}

	g_free(token);
	cursor = token_end;

	while (g_ascii_isspace(*cursor))
	{
		cursor++;
	}

	if (*cursor != '\0')
	{
		return FALSE;
	}

	*seconds = value * factor;
	*label = g_strstrip(g_strndup(every, (gsize)(token_end - every)));
	*prompt_end = every;
	return TRUE;
}

static gboolean
parse_loop_text(const gchar *text, ParsedLoop *parsed, GError **error)
{
	const gchar *rest = NULL;
	gint64       seconds = 0;
	gchar       *label = NULL;
	const gchar *prompt_end = NULL;
	gchar       *prompt;

	memset(parsed, 0, sizeof *parsed);

	if (text == NULL)
	{
		text = "";
	}

	if (parse_leading_interval(text, &seconds, &rest))
	{
		const gchar *label_end = rest;

		while (label_end > text && g_ascii_isspace(label_end[-1]))
		{
			label_end--;
		}

		parsed->has_interval = TRUE;
		parsed->seconds = seconds;
		parsed->label = g_strstrip(g_strndup(text, (gsize)(label_end - text)));
		parsed->prompt = g_strdup(g_strstrip((gchar *)rest));

		if (parsed->prompt[0] == '\0')
		{
			g_clear_pointer(&parsed->prompt, g_free);
		}

		return TRUE;
	}

	if (parse_suffix_interval(text, &seconds, &label, &prompt_end))
	{
		prompt = g_strstrip(g_strndup(text, (gsize)(prompt_end - text)));
		parsed->has_interval = TRUE;
		parsed->seconds = seconds;
		parsed->label = label;
		parsed->prompt = prompt[0] != '\0' ? prompt : NULL;

		if (parsed->prompt == NULL)
		{
			g_free(prompt);
		}

		return TRUE;
	}

	parsed->prompt = g_strstrip(g_strdup(text));

	if (parsed->prompt[0] == '\0')
	{
		g_clear_pointer(&parsed->prompt, g_free);
	}

	if (parsed->prompt != NULL && strlen(parsed->prompt) > LOOP_PROMPT_MAX)
	{
		parsed_clear(parsed);
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A loop prompt is limited to %d bytes.", LOOP_PROMPT_MAX);
		return FALSE;
	}

	return TRUE;
}

static gchar *
cadence_for(gint snapped, const struct tm *local)
{
	if (snapped < 60)
	{
		return g_strdup_printf("every %dm", snapped);
	}

	if (snapped == 60)
	{
		return g_strdup("every hour");
	}

	if (snapped < 24 * 60)
	{
		return g_strdup_printf("every %dh", snapped / 60);
	}

	return g_strdup_printf("daily at %02d:%02d", local->tm_hour, local->tm_min);
}

static gchar *
cron_for(gint snapped, const struct tm *local)
{
	if (snapped < 60)
	{
		return g_strdup_printf("*/%d * * * *", snapped);
	}

	if (snapped == 60)
	{
		return g_strdup("0 * * * *");
	}

	if (snapped < 24 * 60)
	{
		return g_strdup_printf("0 */%d * * *", snapped / 60);
	}

	return g_strdup_printf("%d %d * * *", local->tm_min, local->tm_hour);
}

/* ----------------------------------------------------------------
 * Default prompt
 * ---------------------------------------------------------------- */

static gchar *
read_bounded(const gchar *path, gboolean *truncated)
{
	FILE  *file;
	gchar *buffer;
	gsize  got;

	file = fopen(path, "rb");

	if (file == NULL)
	{
		return NULL;
	}

	buffer = g_malloc(LOOP_MD_MAX + 1);
	got = fread(buffer, 1, LOOP_MD_MAX + 1, file);
	fclose(file);

	if (got > LOOP_MD_MAX)
	{
		got = LOOP_MD_MAX;
		*truncated = TRUE;

		while (got > 0 && !g_utf8_validate(buffer, got, NULL))
		{
			got--;
		}
	}
	else if (!g_utf8_validate(buffer, got, NULL))
	{
		g_debug("loop.md is not valid UTF-8: %s", path);
		g_free(buffer);
		return NULL;
	}
	else
	{
		*truncated = FALSE;
	}

	buffer[got] = '\0';
	return buffer;
}

static gchar *
try_loop_file(const gchar *path, gboolean *truncated, gchar **origin, const gchar *label)
{
	gchar *text;

	if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
	{
		return NULL;
	}

	text = read_bounded(path, truncated);

	if (text == NULL)
	{
		return NULL;
	}

	if (origin != NULL)
	{
		*origin = g_strdup(label);
	}

	return text;
}

gchar *
ai_loop_default_prompt(
	const gchar *cwd,
	const gchar *config_dir,
	const gchar *home,
	gchar      **origin,
	gboolean    *truncated
){
	gboolean local_truncated = FALSE;
	gchar   *text = NULL;

	if (origin != NULL)
	{
		*origin = NULL;
	}

	if (cwd != NULL)
	{
		g_autofree gchar *project = g_build_filename(cwd, ".ai-glib", "loop.md", NULL);
		g_autofree gchar *claude = g_build_filename(cwd, ".claude", "loop.md", NULL);

		text = try_loop_file(project, &local_truncated, origin, ".ai-glib/loop.md");

		if (text == NULL)
		{
			text = try_loop_file(claude, &local_truncated, origin, ".claude/loop.md");
		}
	}

	if (text == NULL && config_dir != NULL)
	{
		g_autofree gchar *config = g_build_filename(config_dir, "ai-glib", "loop.md", NULL);
		text = try_loop_file(config, &local_truncated, origin, "config loop.md");
	}

	if (text == NULL && home != NULL)
	{
		g_autofree gchar *user = g_build_filename(home, ".claude", "loop.md", NULL);
		text = try_loop_file(user, &local_truncated, origin, "~/.claude/loop.md");
	}

	if (text == NULL)
	{
		text = g_strdup(MAINTENANCE_PROMPT);
		local_truncated = FALSE;
	}

	if (truncated != NULL)
	{
		*truncated = local_truncated;
	}

	return text;
}

static gchar *
prompt_excerpt(const gchar *prompt)
{
	g_autofree gchar *flat = NULL;
	gchar            *cursor;

	if (prompt == NULL || prompt[0] == '\0')
	{
		return g_strdup("(maintenance prompt)");
	}

	flat = g_strdup(prompt);

	for (cursor = flat; *cursor != '\0'; cursor++)
	{
		if (*cursor == '\n' || *cursor == '\t')
		{
			*cursor = ' ';
		}
	}

	g_strstrip(flat);

	if (g_utf8_strlen(flat, -1) > 72)
	{
		g_autofree gchar *shortened = g_utf8_substring(flat, 0, 72);
		return g_strdup_printf("%s…", shortened);
	}

	return g_strdup(flat);
}

static gchar *
format_local(gint64 when_us)
{
	time_t    sec = (time_t)(when_us / G_USEC_PER_SEC);
	struct tm when;
	char      buffer[64];

	if (localtime_r(&sec, &when) == NULL)
	{
		return g_strdup("unknown time");
	}

	if (strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M:%S", &when) == 0)
	{
		return g_strdup("unknown time");
	}

	return g_strdup(buffer);
}

static gchar *
new_id(AiLoopSchedule *self)
{
	guint attempt;

	for (attempt = 0; attempt < 8; attempt++)
	{
		guint32 bits = g_random_int();
		gchar  *id = g_strdup_printf("%08x", bits);
		guint   i;
		gboolean clash = FALSE;

		for (i = 0; i < self->tasks->len; i++)
		{
			Task *task = g_ptr_array_index(self->tasks, i);

			if (g_strcmp0(task->id, id) == 0)
			{
				clash = TRUE;
				break;
			}
		}

		if (!clash)
		{
			return id;
		}

		g_free(id);
	}

	return g_strdup_printf("%08x", g_random_int());
}

static gboolean
schedule_fixed(
	AiLoopSchedule *self,
	ParsedLoop     *parsed,
	const gchar    *cwd,
	const gchar    *config_dir,
	const gchar    *home,
	gint64          now_us,
	gchar         **notice,
	GError        **error
)
{
	time_t       sec = (time_t)(now_us / G_USEC_PER_SEC);
	struct tm    local;
	gint         snapped;
	g_autoptr(GError) local_error = NULL;
	Task        *task;
	g_autofree gchar *when = NULL;
	g_autofree gchar *excerpt = NULL;
	g_autofree gchar *origin = NULL;
	gboolean          truncated = FALSE;

	if (localtime_r(&sec, &local) == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Cannot read the local time.");
		return FALSE;
	}

	if (count_kind(self, AI_LOOP_KIND_LOOP, FALSE) >= LOOP_MAX_TASKS)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A session holds at most %d scheduled loops.", LOOP_MAX_TASKS);
		return FALSE;
	}

	if (parsed->prompt != NULL && strlen(parsed->prompt) > LOOP_PROMPT_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A loop prompt is limited to %d bytes.", LOOP_PROMPT_MAX);
		return FALSE;
	}

	if (!check_command(self, parsed->prompt, error))
	{
		return FALSE;
	}

	snapped = snap_minutes(parsed->seconds);
	task = g_new0(Task, 1);
	task->id = new_id(self);
	task->cron_text = cron_for(snapped, &local);
	task->cadence = cadence_for(snapped, &local);
	task->prompt = g_steal_pointer(&parsed->prompt);
	task->created_us = now_us;
	task->expires_us = now_us + LOOP_EXPIRY_US;
	task->interval_us = (gint64)snapped * 60 * G_USEC_PER_SEC;

	if (!cron_parse(&task->cron, task->cron_text, &local_error))
	{
		task_free(task);
		g_propagate_error(error, g_steal_pointer(&local_error));
		return FALSE;
	}

	task->cron_ok = TRUE;
	task->nominal_us = cron_next_us(&task->cron, now_us);
	task->fire_us = task->nominal_us + jitter_us(task->id, task->interval_us);

	if (task->nominal_us == 0)
	{
		task_free(task);
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "That interval has no upcoming time.");
		return FALSE;
	}

	g_ptr_array_add(self->tasks, task);
	when = format_local(task->fire_us);

	if (task->prompt == NULL)
	{
		g_free(ai_loop_default_prompt(cwd, config_dir, home, &origin, &truncated));
		excerpt = g_strdup(origin != NULL ? origin : "(maintenance prompt)");

		if (truncated)
		{
			gchar *joined = g_strdup_printf("%s (truncated to 25000 bytes)", excerpt);
			g_free(excerpt);
			excerpt = joined;
		}
	}
	else
	{
		excerpt = prompt_excerpt(task->prompt);
	}

	if ((gint64)snapped * 60 != parsed->seconds)
	{
		*notice = g_strdup_printf(
			"Rounded %s to %s. Loop %s (%s), next %s local. %s",
			parsed->label, task->cadence, task->id, task->cron_text, when, excerpt);
	}
	else
	{
		*notice = g_strdup_printf(
			"Loop %s %s (%s), next %s local. %s",
			task->id, task->cadence, task->cron_text, when, excerpt);
	}

	return TRUE;
}

static gboolean
schedule_dynamic(
	AiLoopSchedule *self,
	ParsedLoop     *parsed,
	const gchar    *cwd,
	const gchar    *config_dir,
	const gchar    *home,
	gint64          now_us,
	gchar         **notice,
	GError        **error
){
	Task             *task;
	g_autofree gchar *origin = NULL;
	g_autofree gchar *excerpt = NULL;
	gboolean          truncated = FALSE;

	if (count_kind(self, AI_LOOP_KIND_LOOP, FALSE) >= LOOP_MAX_TASKS)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A session holds at most %d scheduled loops.", LOOP_MAX_TASKS);
		return FALSE;
	}

	if (!check_command(self, parsed->prompt, error))
	{
		return FALSE;
	}

	if (parsed->prompt == NULL)
	{
		g_free(ai_loop_default_prompt(cwd, config_dir, home, &origin, &truncated));
	}

	task = g_new0(Task, 1);
	task->dynamic = TRUE;
	task->id = new_id(self);
	task->cadence = g_strdup("self-paced");
	task->prompt = g_steal_pointer(&parsed->prompt);
	task->created_us = now_us;
	task->expires_us = now_us + LOOP_EXPIRY_US;
	task->fire_us = now_us;
	g_ptr_array_add(self->tasks, task);
	excerpt = prompt_excerpt(task->prompt);

	if (origin != NULL)
	{
		*notice = g_strdup_printf(
			"Loop %s self-paced, first pass as soon as this session is idle, using %s%s. %s",
			task->id,
			origin,
			truncated ? " (truncated to 25000 bytes)" : "",
			excerpt);
	}
	else
	{
		*notice = g_strdup_printf(
			"Loop %s self-paced, first pass as soon as this session is idle. %s",
			task->id, excerpt);
	}

	return TRUE;
}

static gchar *
format_list(AiLoopSchedule *self, AiLoopKind kind, gint64 now_us)
{
	GString *text;
	guint    i;

	if (count_kind(self, kind, FALSE) == 0)
	{
		return g_strdup(kind == AI_LOOP_KIND_GOAL ? "No goals." : "No scheduled loops.");
	}

	text = g_string_new(kind == AI_LOOP_KIND_GOAL ? "Goals\n" : "Scheduled loops\n");

	for (i = 0; i < self->tasks->len; i++)
	{
		Task             *task = g_ptr_array_index(self->tasks, i);
		g_autofree gchar *line = NULL;

		if (task->kind != kind)
		{
			continue;
		}

		line = ai_loop_schedule_dup_line(self, i, now_us);
		g_string_append_printf(text, "%s\n", line);
	}

	return g_string_free(text, FALSE);
}

static gboolean
id_is_hex8(const gchar *text)
{
	guint i;

	if (text == NULL || strlen(text) != 8)
	{
		return FALSE;
	}

	for (i = 0; i < 8; i++)
	{
		if (!g_ascii_isxdigit(text[i]))
		{
			return FALSE;
		}
	}

	return TRUE;
}

static gchar *
valid_ids(AiLoopSchedule *self, gint kind)
{
	GString *text = g_string_new(NULL);
	guint    i;

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (kind >= 0 && task->kind != (AiLoopKind)kind)
		{
			continue;
		}

		g_string_append_printf(text, "%s%s", text->len > 0 ? ", " : "", task->id);
	}

	if (text->len == 0)
	{
		g_string_append(text, "none");
	}

	return g_string_free(text, FALSE);
}

/*
 * An id, or a prefix of one that names exactly one entry. A typo is an
 * error that says which ids exist, never a silent no-op and never a
 * guess: pausing the wrong loop is worse than pausing none.
 */
static Task *
resolve(AiLoopSchedule *self, const gchar *text, gint kind, GError **error)
{
	g_autofree gchar *wanted = NULL;
	g_autofree gchar *ids = NULL;
	Task             *found = NULL;
	guint             matches = 0;
	guint             i;
	const gchar      *noun = kind == AI_LOOP_KIND_GOAL ? "goal"
		: kind == AI_LOOP_KIND_LOOP ? "loop" : "loop or goal";

	if (text == NULL || text[0] == '\0')
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Which %s? Give its id.", noun);
		return NULL;
	}

	wanted = g_ascii_strdown(text, -1);

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (g_strcmp0(task->id, wanted) == 0)
		{
			found = task;
			matches = 1;
			break;
		}

		if (strlen(wanted) >= 3 && g_str_has_prefix(task->id, wanted))
		{
			found = task;
			matches++;
		}
	}

	if (matches > 1)
	{
		GString *list = g_string_new(NULL);

		for (i = 0; i < self->tasks->len; i++)
		{
			Task *task = g_ptr_array_index(self->tasks, i);

			if (g_str_has_prefix(task->id, wanted))
			{
				g_string_append_printf(list, "%s%s", list->len > 0 ? ", " : "", task->id);
			}
		}

		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "'%s' matches more than one: %s. Give more of the id.", text, list->str);
		g_string_free(list, TRUE);
		return NULL;
	}

	if (found == NULL)
	{
		ids = valid_ids(self, kind);
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "No %s '%s'. Valid ids: %s.", noun, text, ids);
		return NULL;
	}

	if (kind >= 0 && found->kind != (AiLoopKind)kind)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "%s is a %s, not a %s.", found->id, ai_loop_kind_to_string(found->kind),
		            ai_loop_kind_to_string((AiLoopKind)kind));
		return NULL;
	}

	return found;
}

/**
 * ai_loop_schedule_resolve_id:
 * @self: a schedule
 * @text: an id, or a prefix of at least three characters naming one entry
 * @error: (nullable): return location for a #GError
 *
 * An unknown id is an error listing the valid ones; an ambiguous prefix is
 * an error listing what it matched.
 *
 * Returns: (transfer full) (nullable): the full id
 */
gchar *
ai_loop_schedule_resolve_id(AiLoopSchedule *self, const gchar *text, GError **error)
{
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);
	task = resolve(self, text, -1, error);
	return task != NULL ? g_strdup(task->id) : NULL;
}

/**
 * ai_loop_schedule_find:
 * @self: a schedule
 * @id: (nullable): an exact id
 *
 * Returns: the index of @id, or -1
 */
gint
ai_loop_schedule_find(AiLoopSchedule *self, const gchar *id)
{
	guint i;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), -1);

	for (i = 0; id != NULL && i < self->tasks->len; i++)
	{
		if (g_strcmp0(((Task *)g_ptr_array_index(self->tasks, i))->id, id) == 0)
		{
			return (gint)i;
		}
	}

	return -1;
}

/* ----------------------------------------------------------------
 * Tokens. Subcommands take one word; everything else is a prompt, so
 * "/loop list the deploys" stays a prompt.
 * ---------------------------------------------------------------- */

typedef struct
{
	gchar       *word;
	const gchar *start;
} Token;

static GArray *
tokenize(const gchar *text)
{
	GArray      *tokens = g_array_new(FALSE, TRUE, sizeof(Token));
	const gchar *cursor = text;

	while (*cursor != '\0')
	{
		const gchar *begin;
		Token        token;

		while (g_ascii_isspace(*cursor))
		{
			cursor++;
		}

		if (*cursor == '\0')
		{
			break;
		}

		begin = cursor;

		while (*cursor != '\0' && !g_ascii_isspace(*cursor))
		{
			cursor++;
		}

		token.word = g_strndup(begin, (gsize)(cursor - begin));
		token.start = begin;
		g_array_append_val(tokens, token);
	}

	return tokens;
}

static void
tokens_free(GArray *tokens)
{
	guint i;

	for (i = 0; i < tokens->len; i++)
	{
		g_free(g_array_index(tokens, Token, i).word);
	}

	g_array_free(tokens, TRUE);
}

static gboolean
parse_duration(const gchar *text, gint64 *seconds)
{
	const gchar *end = NULL;
	gint64       value;
	gint64       factor = 0;

	if (text == NULL || !parse_number(text, &end, &value) || value <= 0 || value > 1000000)
	{
		return FALSE;
	}

	if (*end == '\0' || !unit_seconds(end, &factor))
	{
		return FALSE;
	}

	*seconds = value * factor;
	return TRUE;
}

static gboolean
parse_count(const gchar *text, guint *out)
{
	const gchar *end = NULL;
	gint64       value;

	if (text == NULL || !parse_number(text, &end, &value) || *end != '\0' ||
	    value < 1 || value > G_MAXUINT)
	{
		return FALSE;
	}

	*out = (guint)value;
	return TRUE;
}

static gboolean
word_is(const gchar *word, const gchar *a, const gchar *b)
{
	return g_strcmp0(word, a) == 0 || (b != NULL && g_strcmp0(word, b) == 0);
}

typedef gboolean (*EachFunc)(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error);

/*
 * Apply one verb to an id or to every entry of @kind. A failure on one
 * entry of "all" stops there and says so; the ones before it keep the
 * change, which the notice reports.
 */
static gchar *
apply_verb(
	AiLoopSchedule *self,
	AiLoopKind      kind,
	const gchar    *which,
	EachFunc        func,
	const gchar    *done,
	gint64          now_us,
	GError        **error
){
	const gchar *noun = ai_loop_kind_to_string(kind);

	if (g_strcmp0(which, "all") == 0)
	{
		g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
		guint                i;

		for (i = 0; i < self->tasks->len; i++)
		{
			Task *task = g_ptr_array_index(self->tasks, i);

			if (task->kind == kind)
			{
				g_ptr_array_add(ids, g_strdup(task->id));
			}
		}

		if (ids->len == 0)
		{
			return g_strdup(kind == AI_LOOP_KIND_GOAL ? "No goals." : "No scheduled loops.");
		}

		for (i = 0; i < ids->len; i++)
		{
			if (!func(self, g_ptr_array_index(ids, i), now_us, error))
			{
				return NULL;
			}
		}

		return g_strdup_printf("%s %u %s%s.", done, ids->len,
		                       kind == AI_LOOP_KIND_GOAL ? "goal" : "scheduled loop",
		                       ids->len == 1 ? "" : "s");
	}

	{
		Task             *task = resolve(self, which, kind, error);
		g_autofree gchar *id = NULL;

		if (task == NULL)
		{
			return NULL;
		}

		id = g_strdup(task->id);

		if (!func(self, id, now_us, error))
		{
			return NULL;
		}

		return g_strdup_printf("%s %s %s.", done, noun, id);
	}
}

static gboolean
each_pause(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	Task *task = task_by_id(self, id);

	/* "all" skips what cannot be paused rather than failing on it. */
	if (task != NULL && ai_loop_state_is_final(task->state))
	{
		return TRUE;
	}

	return ai_loop_schedule_pause(self, id, error);
}

static gboolean
each_resume(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	Task *task = task_by_id(self, id);

	if (task != NULL && task->state != AI_LOOP_STATE_PAUSED)
	{
		return TRUE;
	}

	return ai_loop_schedule_resume(self, id, now_us, error);
}

static gboolean
each_remove(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	return ai_loop_schedule_remove(self, id, error);
}

static gboolean
each_run(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	return ai_loop_schedule_run_now(self, id, now_us, error);
}

static gboolean
each_stop(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	Task *task = task_by_id(self, id);

	if (task != NULL && ai_loop_state_is_final(task->state))
	{
		return TRUE;
	}

	return ai_loop_schedule_stop_goal(self, id, now_us, error);
}

static gchar *
show_one(AiLoopSchedule *self, AiLoopKind kind, const gchar *which, gint64 now_us, GError **error)
{
	Task *task = resolve(self, which, kind, error);

	if (task == NULL)
	{
		return NULL;
	}

	return ai_loop_schedule_dup_details(self, (guint)ai_loop_schedule_find(self, task->id), now_us);
}

/*
 * The part of an edit line after an option that takes the rest of the
 * line: a prompt or a condition can contain anything, including words
 * that look like options.
 */
static const gchar *
rest_after(GArray *tokens, guint index)
{
	const gchar *rest;

	if (index + 1 >= tokens->len)
	{
		return NULL;
	}

	rest = g_array_index(tokens, Token, index + 1).start;
	return rest;
}

static gchar *
edit_loop(AiLoopSchedule *self, GArray *tokens, gint64 now_us, GError **error)
{
	Task             *task;
	g_autofree gchar *id = NULL;
	guint             i;
	gboolean          changed = FALSE;

	task = resolve(self, g_array_index(tokens, Token, 1).word, AI_LOOP_KIND_LOOP, error);

	if (task == NULL)
	{
		return NULL;
	}

	id = g_strdup(task->id);

	for (i = 2; i < tokens->len; i++)
	{
		const gchar *word = g_array_index(tokens, Token, i).word;

		if (word_is(word, "--every", "--interval"))
		{
			gint64 seconds = 0;

			if (i + 1 >= tokens->len ||
			    !parse_duration(g_array_index(tokens, Token, i + 1).word, &seconds))
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				                    "--every needs an interval such as 10m, 2h or 1d.");
				return NULL;
			}

			if (!ai_loop_schedule_set_interval(self, id, seconds * G_USEC_PER_SEC, now_us, error))
			{
				return NULL;
			}

			i++;
			changed = TRUE;
		}
		else if (word_is(word, "--self-paced", NULL))
		{
			if (!ai_loop_schedule_set_interval(self, id, 0, now_us, error))
			{
				return NULL;
			}

			changed = TRUE;
		}
		else if (word_is(word, "--prompt", NULL))
		{
			g_autofree gchar *prompt = g_strdup(rest_after(tokens, i));

			if (prompt != NULL)
			{
				g_strstrip(prompt);
			}

			if (!ai_loop_schedule_set_prompt(self, id, prompt, error))
			{
				return NULL;
			}

			changed = TRUE;
			break;
		}
		else
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "Unknown option %s. Usage: /loop edit ID [--every INTERVAL | --self-paced] [--prompt TEXT]",
			            word);
			return NULL;
		}
	}

	if (!changed)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Usage: /loop edit ID [--every INTERVAL | --self-paced] [--prompt TEXT]");
		return NULL;
	}

	{
		gint index = ai_loop_schedule_find(self, id);
		g_autofree gchar *line = ai_loop_schedule_dup_line(self, (guint)index, now_us);

		return g_strdup_printf("Updated loop %s: %s", id, line);
	}
}

/* Which subcommand, if any. NULL means the line is a prompt. */
static const gchar *
loop_verb(GArray *tokens, const gchar *const *verbs)
{
	const gchar *first;
	guint        i;

	if (tokens->len == 0)
	{
		return NULL;
	}

	first = g_array_index(tokens, Token, 0).word;

	if (g_strcmp0(first, "edit") == 0 && tokens->len >= 2 &&
	    (tokens->len == 2 || g_str_has_prefix(g_array_index(tokens, Token, 2).word, "--")))
	{
		return "edit";
	}

	if (tokens->len != 2)
	{
		return NULL;
	}

	for (i = 0; verbs[i] != NULL; i++)
	{
		if (g_strcmp0(first, verbs[i]) == 0)
		{
			return verbs[i];
		}
	}

	return NULL;
}

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
 * Applies one `/loop` line: schedule a loop, or `list`, `show ID`,
 * `edit ID ...`, `pause ID|all`, `resume ID|all`, `run ID`,
 * `delete ID|all` (`cancel` is the same), `stop`. A verb with more than
 * one word after it is a prompt, so `/loop list the deploys` schedules.
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
){
	static const gchar *const verbs[] = {
		"show", "pause", "resume", "run", "run-now", "delete", "cancel", NULL
	};
	g_autofree gchar *text = NULL;
	GArray           *tokens;
	const gchar      *verb;
	gchar            *result = NULL;
	ParsedLoop        parsed;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (ai_loop_schedule_is_disabled())
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		                    "Scheduled loops are disabled (AI_LOOP_DISABLE).");
		return NULL;
	}

	text = g_strdup(arguments != NULL ? arguments : "");
	g_strstrip(text);

	if (g_strcmp0(text, "list") == 0)
	{
		return format_list(self, AI_LOOP_KIND_LOOP, now_us);
	}

	if (g_strcmp0(text, "stop") == 0)
	{
		gchar *notice = ai_loop_schedule_stop_waiting(self);
		return notice != NULL ? notice : g_strdup("No self-paced loop is waiting.");
	}

	if (g_strcmp0(text, "cancel") == 0 || g_strcmp0(text, "delete") == 0)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Usage: /loop %s <id|all>", text);
		return NULL;
	}

	tokens = tokenize(text);
	verb = loop_verb(tokens, verbs);

	if (verb != NULL)
	{
		const gchar *which = tokens->len > 1 ? g_array_index(tokens, Token, 1).word : NULL;

		if (g_strcmp0(verb, "show") == 0)
			result = show_one(self, AI_LOOP_KIND_LOOP, which, now_us, error);
		else if (g_strcmp0(verb, "edit") == 0)
			result = edit_loop(self, tokens, now_us, error);
		else if (g_strcmp0(verb, "pause") == 0)
			result = apply_verb(self, AI_LOOP_KIND_LOOP, which, each_pause, "Paused", now_us, error);
		else if (g_strcmp0(verb, "resume") == 0)
			result = apply_verb(self, AI_LOOP_KIND_LOOP, which, each_resume, "Resumed", now_us, error);
		else if (g_strcmp0(verb, "run") == 0 || g_strcmp0(verb, "run-now") == 0)
			result = apply_verb(self, AI_LOOP_KIND_LOOP, which, each_run,
			                    "Queued to run when the session is idle:", now_us, error);
		else if (g_strcmp0(verb, "cancel") == 0)
			result = apply_verb(self, AI_LOOP_KIND_LOOP, which, each_remove, "Cancelled", now_us, error);
		else
			result = apply_verb(self, AI_LOOP_KIND_LOOP, which, each_remove, "Deleted", now_us, error);

		tokens_free(tokens);
		return result;
	}

	tokens_free(tokens);
	memset(&parsed, 0, sizeof parsed);

	if (!parse_loop_text(text, &parsed, error))
	{
		return NULL;
	}

	if (parsed.has_interval)
	{
		gchar *notice = NULL;

		if (!schedule_fixed(self, &parsed, cwd, config_dir, home, now_us, &notice, error))
		{
			parsed_clear(&parsed);
			return NULL;
		}

		parsed_clear(&parsed);
		return notice;
	}

	{
		gchar *notice = NULL;

		if (!schedule_dynamic(self, &parsed, cwd, config_dir, home, now_us, &notice, error))
		{
			parsed_clear(&parsed);
			return NULL;
		}

		parsed_clear(&parsed);
		return notice;
	}
}

static gboolean
task_wants_turn(const Task *task, gint64 now_us)
{
	if (task->inflight || ai_loop_state_is_final(task->state))
	{
		return FALSE;
	}

	if (task->run_requested)
	{
		return TRUE;
	}

	return task->state == AI_LOOP_STATE_ACTIVE && task->fire_us <= now_us;
}

const gchar *
ai_loop_schedule_due(AiLoopSchedule *self, gint64 now_us)
{
	guint i;
	Task *best = NULL;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);

	if (ai_loop_schedule_is_disabled())
	{
		return NULL;
	}

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (!task_wants_turn(task, now_us))
		{
			continue;
		}

		if (best == NULL || task->fire_us < best->fire_us ||
		    (task->fire_us == best->fire_us &&
		     g_strcmp0(task->id, best->id) < 0))
		{
			best = task;
		}
	}

	return best != NULL ? best->id : NULL;
}

static gchar *
goal_prompt(const Task *task, gint64 now_us)
{
	g_autofree gchar *left = NULL;

	if (task->turns == 0)
	{
		return g_strdup_printf("Work toward this goal until it holds:\n\n%s%s",
		                       task->condition, GOAL_INSTRUCTION);
	}

	left = ai_loop_format_duration(MAX(task->deadline_us - now_us, 0));
	return g_strdup_printf(
		"Continue working toward this goal (turn %u of %u, %s left):\n\n%s\n\n"
		"Last check: %s%s",
		task->turns + 1, task->max_turns, left, task->condition,
		task->reason != NULL ? task->reason : "none", GOAL_INSTRUCTION);
}

gchar *
ai_loop_schedule_dup_prompt(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *cwd,
	const gchar    *config_dir,
	const gchar    *home
){
	Task             *task = task_by_id(self, id);
	g_autofree gchar *body = NULL;

	if (task == NULL)
	{
		return NULL;
	}

	if (task->kind == AI_LOOP_KIND_GOAL)
	{
		return goal_prompt(task, g_get_real_time());
	}

	if (task->prompt != NULL)
	{
		body = g_strdup(task->prompt);
	}
	else
	{
		body = ai_loop_default_prompt(cwd, config_dir, home, NULL, NULL);
	}

	/* A /command is sent as typed. The delay instruction appended to it
	 * would become the command's arguments. */
	if (task->dynamic && body[0] != '/')
	{
		return g_strconcat(body, DYNAMIC_INSTRUCTION, NULL);
	}

	return g_steal_pointer(&body);
}

static gboolean
arm_next(Task *task, gint64 now_us)
{
	gint attempt;

	for (attempt = 0; attempt < 8; attempt++)
	{
		gint64 nominal = cron_next_us(&task->cron, now_us);
		gint64 fire;

		if (nominal == 0)
		{
			return FALSE;
		}

		fire = nominal + jitter_us(task->id, task->interval_us);

		if (fire > now_us)
		{
			task->nominal_us = nominal;
			task->fire_us = fire;
			return TRUE;
		}

		now_us = nominal;
	}

	return FALSE;
}

gboolean
ai_loop_schedule_note_fired(
	AiLoopSchedule *self,
	const gchar    *id,
	gint64          now_us,
	gchar         **notice
){
	Task    *task;
	gboolean manual;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);

	if (notice != NULL)
	{
		*notice = NULL;
	}

	task = task_by_id(self, id);

	if (task == NULL)
	{
		return FALSE;
	}

	manual = task->run_requested;
	task->run_requested = FALSE;

	if (task->kind == AI_LOOP_KIND_GOAL)
	{
		task->inflight = TRUE;
		task->turns++;
		return TRUE;
	}

	if (task->dynamic)
	{
		task->inflight = TRUE;
		return TRUE;
	}

	/* A manual run of a paused loop leaves its schedule where it was. */
	if (manual && task->state == AI_LOOP_STATE_PAUSED)
	{
		return TRUE;
	}

	if (now_us >= task->expires_us)
	{
		if (notice != NULL)
		{
			*notice = g_strdup_printf(
				"Loop %s ran for the last time (scheduled loops expire after 7 days).",
				task->id);
		}

		task_remove(self, task);
		return TRUE;
	}

	if (!arm_next(task, now_us))
	{
		if (notice != NULL)
		{
			*notice = g_strdup_printf("Loop %s stopped: its next time could not be computed.", id);
		}

		task_remove(self, task);
		return TRUE;
	}

	return TRUE;
}

static gchar *
control_line(const gchar *text)
{
	g_auto(GStrv) lines = NULL;
	gint          i;
	gchar        *found = NULL;

	if (text == NULL)
	{
		return NULL;
	}

	lines = g_strsplit(text, "\n", -1);

	for (i = 0; lines[i] != NULL; i++)
	{
		g_strstrip(lines[i]);

		if (g_strcmp0(lines[i], "LOOP_STOP") == 0 ||
		    g_str_has_prefix(lines[i], "LOOP_STOP:") ||
		    g_strcmp0(lines[i], "LOOP_NEXT") == 0 ||
		    g_str_has_prefix(lines[i], "LOOP_NEXT:"))
		{
			g_free(found);
			found = g_strdup(lines[i]);
		}
	}

	return found;
}

static gboolean
parse_model_delay(const gchar *text, gint64 *seconds, gchar **reason)
{
	const gchar *cursor;
	const gchar *end = NULL;
	gint64       value;
	gint64       factor = 60;
	const gchar *token_end;

	cursor = text;

	if (!parse_number(cursor, &end, &value) || value <= 0 || value > 1000000)
	{
		return FALSE;
	}

	cursor = end;

	if (*cursor != '\0' && !g_ascii_isspace(*cursor))
	{
		gchar unit[2] = { *cursor, '\0' };

		if (!unit_seconds(unit, &factor) ||
		    (cursor[1] != '\0' && !g_ascii_isspace(cursor[1])))
		{
			return FALSE;
		}

		cursor++;
	}
	else
	{
		gchar *token;

		while (g_ascii_isspace(*cursor))
		{
			cursor++;
		}

		token_end = cursor;

		while (*token_end != '\0' && !g_ascii_isspace(*token_end))
		{
			token_end++;
		}

		token = g_strndup(cursor, (gsize)(token_end - cursor));

		if (unit_seconds(token, &factor))
		{
			cursor = token_end;
		}

		g_free(token);
	}

	while (g_ascii_isspace(*cursor))
	{
		cursor++;
	}

	*seconds = value * factor;
	*reason = g_strdup(cursor);
	return TRUE;
}

gchar *
ai_loop_schedule_complete(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *assistant_text,
	gint64          now_us
){
	Task             *task = task_by_id(self, id);
	g_autofree gchar *line = NULL;
	g_autofree gchar *reason = NULL;
	gint64            seconds = 0;
	gint              minutes;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);

	if (task == NULL || !task->dynamic || task->kind != AI_LOOP_KIND_LOOP)
	{
		return NULL;
	}

	task->inflight = FALSE;

	if (task->stop_requested)
	{
		gchar *notice = g_strdup_printf("Loop %s stopped.", task->id);
		task_remove(self, task);
		return notice;
	}

	if (task->expires_us > 0 && now_us >= task->expires_us)
	{
		gchar *notice = g_strdup_printf(
			"Loop %s ran for the last time (loops expire after 7 days).", task->id);
		task_remove(self, task);
		return notice;
	}

	line = control_line(assistant_text);

	if (line != NULL &&
	    (g_strcmp0(line, "LOOP_STOP") == 0 || g_str_has_prefix(line, "LOOP_STOP:")))
	{
		const gchar *why = strchr(line, ':');
		gchar       *notice;

		why = why != NULL ? g_strstrip((gchar *)why + 1) : "";
		notice = why[0] != '\0'
			? g_strdup_printf("Loop %s stopped: %s", id, why)
			: g_strdup_printf("Loop %s stopped.", id);
		task_remove(self, task);
		return notice;
	}

	if (line != NULL && g_str_has_prefix(line, "LOOP_NEXT"))
	{
		const gchar *rest = line + strlen("LOOP_NEXT");

		if (*rest == ':')
		{
			rest++;
		}

		while (g_ascii_isspace(*rest))
		{
			rest++;
		}

		if (parse_model_delay(rest, &seconds, &reason))
		{
			gint64 accepted;

			minutes = (gint)((seconds + 59) / 60);

			if (minutes < 1)
			{
				minutes = 1;
			}

			if (minutes > LOOP_DYNAMIC_MAX_MINUTES)
			{
				minutes = LOOP_DYNAMIC_MAX_MINUTES;
			}

			accepted = (gint64)minutes * 60 * G_USEC_PER_SEC;
			task->fallback_armed = FALSE;
			task->fire_us = now_us + accepted;

			if (reason != NULL && reason[0] != '\0')
			{
				return g_strdup_printf("Loop %s waits %dm: %s", id, minutes, reason);
			}

			return g_strdup_printf("Loop %s waits %dm.", id, minutes);
		}
	}

	/* A /command was never asked to choose a delay, so its silence is
	 * not a reason to stop it. */
	if (task->prompt != NULL && task->prompt[0] == '/')
	{
		task->fire_us = now_us + LOOP_FALLBACK_US;
		return g_strdup_printf("Loop %s runs its command again in 20m.", id);
	}

	if (task->fallback_armed)
	{
		gchar *notice = g_strdup_printf(
			"Loop %s stopped: it did not choose a delay after the fallback check.",
			id);
		task_remove(self, task);
		return notice;
	}

	task->fallback_armed = TRUE;
	task->fire_us = now_us + LOOP_FALLBACK_US;
	return g_strdup_printf(
		"Loop %s did not choose a delay; checking again in 20m.", id);
}

gchar *
ai_loop_schedule_stop_waiting(AiLoopSchedule *self)
{
	GPtrArray *drop;
	guint      i;
	guint      disarmed = 0;
	GString   *text;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);
	drop = g_ptr_array_new();

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (!task->dynamic)
		{
			continue;
		}

		if (task->inflight)
		{
			task->stop_requested = TRUE;
			disarmed++;
		}
		else
		{
			g_ptr_array_add(drop, task);
		}
	}

	if (drop->len == 0 && disarmed == 0)
	{
		g_ptr_array_unref(drop);
		return NULL;
	}

	text = g_string_new(NULL);

	if (drop->len > 0)
	{
		g_string_append_printf(text, "Stopped %u self-paced loop%s.",
		                       drop->len, drop->len == 1 ? "" : "s");
	}

	for (i = 0; i < drop->len; i++)
	{
		task_remove(self, g_ptr_array_index(drop, i));
	}

	g_ptr_array_unref(drop);

	if (disarmed > 0)
	{
		if (text->len > 0)
		{
			g_string_append(text, " ");
		}

		g_string_append(text, "The running loop will not schedule another turn.");
	}

	return g_string_free(text, FALSE);
}

/* ----------------------------------------------------------------
 * Built-ins that a schedule may run. The command table decides; this
 * only asks it.
 * ---------------------------------------------------------------- */

/**
 * ai_loop_schedule_set_commands:
 * @self: a schedule
 * @commands: (nullable): the command set a scheduled `/name` resolves in
 *
 * With a command set, a loop whose prompt is a built-in that must not run
 * unattended -- `/clear`, `/quit`, `/loop` itself -- is refused when it is
 * scheduled rather than discovered when it fires.
 */
void
ai_loop_schedule_set_commands(AiLoopSchedule *self, AiCommandSet *commands)
{
	g_return_if_fail(AI_IS_LOOP_SCHEDULE(self));
	g_set_object(&self->commands, commands);
}

static gboolean
check_command(AiLoopSchedule *self, const gchar *prompt, GError **error)
{
	const gchar         *cursor;
	g_autofree gchar    *name = NULL;
	g_autoptr(AiCommand) command = NULL;

	if (self->commands == NULL || prompt == NULL || prompt[0] != '/')
	{
		return TRUE;
	}

	for (cursor = prompt + 1; *cursor != '\0' && !g_ascii_isspace(*cursor); cursor++)
	{
	}

	name = g_strndup(prompt + 1, (gsize)(cursor - (prompt + 1)));
	command = ai_command_set_lookup(self->commands, name);

	if (command != NULL && ai_command_get_kind(command) == AI_COMMAND_BUILTIN &&
	    !ai_command_get_schedulable(command))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "/%s cannot run on a schedule: it changes the session itself. "
		            "Scheduled built-ins are the ones /help marks as schedulable.",
		            name);
		return FALSE;
	}

	return TRUE;
}

/**
 * ai_loop_schedule_is_unattended_safe:
 * @self: a schedule
 * @text: a line about to be sent
 *
 * Whether a scheduled `/name` may run as a command. When it may not, the
 * frontend sends the line to the model as text instead, so a schedule
 * written before the rule existed still cannot clear its own session.
 *
 * Returns: %TRUE unless @text names a built-in that is not schedulable
 */
gboolean
ai_loop_schedule_is_unattended_safe(AiLoopSchedule *self, const gchar *text)
{
	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	return check_command(self, text, NULL);
}

/* ----------------------------------------------------------------
 * Adding
 * ---------------------------------------------------------------- */

static gchar *
seconds_label(gint64 seconds)
{
	if (seconds % 86400 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT "d", seconds / 86400);
	if (seconds % 3600 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT "h", seconds / 3600);
	if (seconds % 60 == 0)
		return g_strdup_printf("%" G_GINT64_FORMAT "m", seconds / 60);
	return g_strdup_printf("%" G_GINT64_FORMAT "s", seconds);
}

/**
 * ai_loop_schedule_add_loop:
 * @self: a schedule
 * @interval_us: the interval, or 0 for a self-paced loop
 * @prompt: (nullable): the prompt or `/command`; %NULL for the
 *   maintenance prompt
 * @now_us: real time in microseconds
 * @notice: (out) (optional) (transfer full) (nullable): the confirmation
 * @error: (nullable): return location for a #GError
 *
 * The same schedule `/loop` builds, without the parsing. An interval is
 * rounded to one cron can express, never below one minute, and the
 * notice says so when it was.
 *
 * Returns: (transfer full) (nullable): the new id
 */
gchar *
ai_loop_schedule_add_loop(
	AiLoopSchedule *self,
	gint64          interval_us,
	const gchar    *prompt,
	gint64          now_us,
	gchar         **notice,
	GError        **error
){
	ParsedLoop        parsed;
	g_autofree gchar *local_notice = NULL;
	gboolean          ok;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);

	if (notice != NULL)
	{
		*notice = NULL;
	}

	if (ai_loop_schedule_is_disabled())
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		                    "Scheduled loops are disabled (AI_LOOP_DISABLE).");
		return NULL;
	}

	if (interval_us < 0 || interval_us > (gint64)366 * 86400 * G_USEC_PER_SEC)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A loop interval must be between one minute and a year.");
		return NULL;
	}

	if (prompt != NULL && strlen(prompt) > LOOP_PROMPT_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A loop prompt is limited to %d bytes.", LOOP_PROMPT_MAX);
		return NULL;
	}

	if (!check_command(self, prompt, error))
	{
		return NULL;
	}

	memset(&parsed, 0, sizeof parsed);
	parsed.prompt = prompt != NULL && prompt[0] != '\0' ? g_strstrip(g_strdup(prompt)) : NULL;

	if (interval_us > 0)
	{
		parsed.has_interval = TRUE;
		parsed.seconds = MAX((interval_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1);
		parsed.label = seconds_label(parsed.seconds);
		ok = schedule_fixed(self, &parsed, NULL, NULL, NULL, now_us, &local_notice, error);
	}
	else
	{
		ok = schedule_dynamic(self, &parsed, NULL, NULL, NULL, now_us, &local_notice, error);
	}

	parsed_clear(&parsed);

	if (!ok)
	{
		return NULL;
	}

	if (notice != NULL)
	{
		*notice = g_steal_pointer(&local_notice);
	}

	return g_strdup(((Task *)g_ptr_array_index(self->tasks, self->tasks->len - 1))->id);
}

/* Finished goals are history, kept so a person can see how one ended.
 * The oldest go first once there are more than the cap. */
static void
prune_finished(AiLoopSchedule *self, guint keep)
{
	while (TRUE)
	{
		Task *oldest = NULL;
		guint finished = 0;
		guint i;

		for (i = 0; i < self->tasks->len; i++)
		{
			Task *task = g_ptr_array_index(self->tasks, i);

			if (task->kind != AI_LOOP_KIND_GOAL || task_is_live(task))
			{
				continue;
			}

			finished++;

			if (oldest == NULL || task->ended_us < oldest->ended_us)
			{
				oldest = task;
			}
		}

		if (finished <= keep || oldest == NULL)
		{
			return;
		}

		task_remove(self, oldest);
	}
}

static gboolean
check_bounds(guint max_turns, gint64 max_duration_us, GError **error)
{
	if (max_turns > GOAL_MAX_TURNS)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A goal can take at most %d turns.", GOAL_MAX_TURNS);
		return FALSE;
	}

	if (max_duration_us != 0 &&
	    (max_duration_us < 60 * G_USEC_PER_SEC || max_duration_us > GOAL_MAX_US))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A goal's time bound must be between one minute and 7 days.");
		return FALSE;
	}

	return TRUE;
}

/**
 * ai_loop_schedule_add_goal:
 * @self: a schedule
 * @condition: what must hold for the goal to be met
 * @max_turns: the turn bound, or 0 for the default of 20 (at most 200)
 * @max_duration_us: the time bound, or 0 for the default of two hours
 *   (at most 7 days)
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * A goal always has both bounds. There is no unbounded goal: a condition
 * the model can never satisfy ends at whichever bound comes first and is
 * reported as expired, not met.
 *
 * Returns: (transfer full) (nullable): the new id
 */
gchar *
ai_loop_schedule_add_goal(
	AiLoopSchedule *self,
	const gchar    *condition,
	guint           max_turns,
	gint64          max_duration_us,
	gint64          now_us,
	GError        **error
){
	g_autofree gchar *text = NULL;
	Task             *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);

	if (ai_loop_schedule_is_disabled())
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		                    "Goals are disabled (AI_LOOP_DISABLE).");
		return NULL;
	}

	text = g_strstrip(g_strdup(condition != NULL ? condition : ""));

	if (text[0] == '\0')
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A goal needs a condition, such as: /goal the tests pass");
		return NULL;
	}

	if (strlen(text) > GOAL_CONDITION_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A goal condition is limited to %d bytes.", GOAL_CONDITION_MAX);
		return NULL;
	}

	if (!check_bounds(max_turns, max_duration_us, error))
	{
		return NULL;
	}

	if (count_kind(self, AI_LOOP_KIND_GOAL, TRUE) >= GOAL_MAX_LIVE)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A session holds at most %d unfinished goals.", GOAL_MAX_LIVE);
		return NULL;
	}

	prune_finished(self, GOAL_MAX_FINISHED - 1);

	if (self->tasks->len >= LOOP_MAX_ENTRIES)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A session holds at most %d loops and goals.", LOOP_MAX_ENTRIES);
		return NULL;
	}

	task = g_new0(Task, 1);
	task->kind = AI_LOOP_KIND_GOAL;
	task->state = AI_LOOP_STATE_ACTIVE;
	task->id = new_id(self);
	task->condition = g_steal_pointer(&text);
	task->created_us = now_us;
	task->fire_us = now_us;
	task->max_turns = max_turns != 0 ? max_turns : GOAL_DEFAULT_TURNS;
	task->max_duration_us = max_duration_us != 0 ? max_duration_us : GOAL_DEFAULT_US;
	task->deadline_us = now_us + task->max_duration_us;
	task->expires_us = task->deadline_us;
	g_ptr_array_add(self->tasks, task);
	return g_strdup(task->id);
}

/* ----------------------------------------------------------------
 * Editing
 * ---------------------------------------------------------------- */

static Task *
live_task(AiLoopSchedule *self, const gchar *id, gint kind, const gchar *verb, GError **error)
{
	Task *task = resolve(self, id, kind, error);

	if (task != NULL && ai_loop_state_is_final(task->state))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Goal %s is %s; it cannot be %s.", task->id,
		            ai_loop_state_to_string(task->state), verb);
		return NULL;
	}

	return task;
}

/**
 * ai_loop_schedule_pause:
 * @self: a schedule
 * @id: an id, or an unambiguous prefix
 * @error: (nullable): return location for a #GError
 *
 * A paused entry keeps its place and does not run. A turn already
 * running finishes; it does not schedule another.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_pause(AiLoopSchedule *self, const gchar *id, GError **error)
{
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = live_task(self, id, -1, "paused", error);

	if (task == NULL)
	{
		return FALSE;
	}

	task->state = AI_LOOP_STATE_PAUSED;
	task->run_requested = FALSE;
	return TRUE;
}

/**
 * ai_loop_schedule_resume:
 * @self: a schedule
 * @id: an id, or an unambiguous prefix
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * A fixed loop resumes at its next slot; the ones it missed while paused
 * are skipped, not replayed. A self-paced loop or a goal runs as soon as
 * the session is idle.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_resume(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = live_task(self, id, -1, "resumed", error);

	if (task == NULL)
	{
		return FALSE;
	}

	if (task->state != AI_LOOP_STATE_PAUSED)
	{
		return TRUE;
	}

	task->state = AI_LOOP_STATE_ACTIVE;

	if (task->kind == AI_LOOP_KIND_LOOP && !task->dynamic)
	{
		if (task->fire_us <= now_us && !arm_next(task, now_us))
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "Loop %s has no upcoming time.", task->id);
			task->state = AI_LOOP_STATE_PAUSED;
			return FALSE;
		}
	}
	else if (task->fire_us < now_us || task->kind == AI_LOOP_KIND_GOAL)
	{
		task->fire_us = now_us;
	}

	return TRUE;
}

/**
 * ai_loop_schedule_remove:
 * @self: a schedule
 * @id: an id, or an unambiguous prefix
 * @error: (nullable): return location for a #GError
 *
 * Deletes a loop or a goal, finished or not. A turn it started keeps
 * running and schedules nothing afterwards.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_remove(AiLoopSchedule *self, const gchar *id, GError **error)
{
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = resolve(self, id, -1, error);

	if (task == NULL)
	{
		return FALSE;
	}

	task_remove(self, task);
	return TRUE;
}

/**
 * ai_loop_schedule_run_now:
 * @self: a schedule
 * @id: an id, or an unambiguous prefix
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Runs one turn as soon as the session is idle, even when paused -- a
 * paused entry stays paused afterwards. It still waits for a running turn
 * to finish: nothing here interrupts one.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_run_now(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = live_task(self, id, -1, "run", error);

	if (task == NULL)
	{
		return FALSE;
	}

	if (task->kind == AI_LOOP_KIND_GOAL && task->turns >= task->max_turns)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Goal %s has used all %u of its turns. Raise the bound with "
		            "/goal edit %s --turns N.", task->id, task->max_turns, task->id);
		return FALSE;
	}

	task->run_requested = TRUE;
	return TRUE;
}

/**
 * ai_loop_schedule_stop_goal:
 * @self: a schedule
 * @id: a goal id, or an unambiguous prefix
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Ends a goal as stopped, not met, and keeps it in the list so how it
 * ended stays visible. A turn already running finishes and is not judged.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_stop_goal(AiLoopSchedule *self, const gchar *id, gint64 now_us, GError **error)
{
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = live_task(self, id, AI_LOOP_KIND_GOAL, "stopped", error);

	if (task == NULL)
	{
		return FALSE;
	}

	task->state = AI_LOOP_STATE_STOPPED;
	task->run_requested = FALSE;
	task->ended_us = now_us;
	g_free(task->reason);
	task->reason = g_strdup("stopped by the user before it was met");
	return TRUE;
}

/**
 * ai_loop_schedule_set_interval:
 * @self: a schedule
 * @id: a loop id, or an unambiguous prefix
 * @interval_us: the new interval, or 0 to make the loop self-paced
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_set_interval(
	AiLoopSchedule *self,
	const gchar    *id,
	gint64          interval_us,
	gint64          now_us,
	GError        **error
){
	Task     *task;
	time_t    sec = (time_t)(now_us / G_USEC_PER_SEC);
	struct tm local;
	gint      snapped;
	Cron      cron;
	g_autofree gchar *cron_text = NULL;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = resolve(self, id, AI_LOOP_KIND_LOOP, error);

	if (task == NULL)
	{
		return FALSE;
	}

	if (interval_us < 0 || interval_us > (gint64)366 * 86400 * G_USEC_PER_SEC)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A loop interval must be between one minute and a year.");
		return FALSE;
	}

	if (task->expires_us == 0)
	{
		task->expires_us = now_us + LOOP_EXPIRY_US;
	}

	if (interval_us == 0)
	{
		task->dynamic = TRUE;
		task->fallback_armed = FALSE;
		task->interval_us = 0;
		task->nominal_us = 0;
		g_clear_pointer(&task->cron_text, g_free);
		g_free(task->cadence);
		task->cadence = g_strdup("self-paced");
		task->fire_us = now_us;
		return TRUE;
	}

	if (localtime_r(&sec, &local) == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Cannot read the local time.");
		return FALSE;
	}

	snapped = snap_minutes(MAX((interval_us + G_USEC_PER_SEC - 1) / G_USEC_PER_SEC, 1));
	cron_text = cron_for(snapped, &local);

	if (!cron_parse(&cron, cron_text, error))
	{
		return FALSE;
	}

	task->dynamic = FALSE;
	task->cron = cron;
	task->cron_ok = TRUE;
	g_free(task->cron_text);
	task->cron_text = g_steal_pointer(&cron_text);
	g_free(task->cadence);
	task->cadence = cadence_for(snapped, &local);
	task->interval_us = (gint64)snapped * 60 * G_USEC_PER_SEC;

	if (!arm_next(task, now_us))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "That interval has no upcoming time.");
		return FALSE;
	}

	return TRUE;
}

/**
 * ai_loop_schedule_set_prompt:
 * @self: a schedule
 * @id: a loop id, or an unambiguous prefix
 * @prompt: (nullable): the new prompt or `/command`; %NULL or empty for
 *   the maintenance prompt
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_set_prompt(AiLoopSchedule *self, const gchar *id, const gchar *prompt, GError **error)
{
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = resolve(self, id, AI_LOOP_KIND_LOOP, error);

	if (task == NULL)
	{
		return FALSE;
	}

	if (prompt != NULL && strlen(prompt) > LOOP_PROMPT_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A loop prompt is limited to %d bytes.", LOOP_PROMPT_MAX);
		return FALSE;
	}

	if (!check_command(self, prompt, error))
	{
		return FALSE;
	}

	g_free(task->prompt);
	task->prompt = prompt != NULL && prompt[0] != '\0' ? g_strdup(prompt) : NULL;
	return TRUE;
}

/**
 * ai_loop_schedule_set_condition:
 * @self: a schedule
 * @id: a goal id, or an unambiguous prefix
 * @condition: the new condition
 * @error: (nullable): return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_set_condition(AiLoopSchedule *self, const gchar *id, const gchar *condition, GError **error)
{
	Task             *task;
	g_autofree gchar *text = NULL;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = live_task(self, id, AI_LOOP_KIND_GOAL, "edited", error);

	if (task == NULL)
	{
		return FALSE;
	}

	text = g_strstrip(g_strdup(condition != NULL ? condition : ""));

	if (text[0] == '\0' || strlen(text) > GOAL_CONDITION_MAX)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A goal condition must be 1 to %d bytes.", GOAL_CONDITION_MAX);
		return FALSE;
	}

	g_free(task->condition);
	task->condition = g_steal_pointer(&text);
	return TRUE;
}

/**
 * ai_loop_schedule_set_bounds:
 * @self: a schedule
 * @id: a goal id, or an unambiguous prefix
 * @max_turns: the new turn bound, or 0 to leave it
 * @max_duration_us: the new time bound from when the goal started, or 0
 *   to leave it
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Raising the bounds of an expired goal makes it active again: it ran
 * out of room, and now it has more. A goal that was met, failed or was
 * stopped stays finished -- start a new one.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_schedule_set_bounds(
	AiLoopSchedule *self,
	const gchar    *id,
	guint           max_turns,
	gint64          max_duration_us,
	gint64          now_us,
	GError        **error
){
	Task *task;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	task = resolve(self, id, AI_LOOP_KIND_GOAL, error);

	if (task == NULL || !check_bounds(max_turns, max_duration_us, error))
	{
		return FALSE;
	}

	if (ai_loop_state_is_final(task->state) && task->state != AI_LOOP_STATE_EXPIRED)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Goal %s is %s; start a new goal instead.", task->id,
		            ai_loop_state_to_string(task->state));
		return FALSE;
	}

	if (max_turns != 0)
	{
		task->max_turns = max_turns;
	}

	if (max_duration_us != 0)
	{
		task->max_duration_us = max_duration_us;
		task->deadline_us = task->created_us + max_duration_us;
		task->expires_us = task->deadline_us;
	}

	if (task->state == AI_LOOP_STATE_EXPIRED &&
	    task->turns < task->max_turns && now_us < task->deadline_us)
	{
		task->state = AI_LOOP_STATE_ACTIVE;
		task->ended_us = 0;
		task->fire_us = now_us;
	}

	return TRUE;
}

/* ----------------------------------------------------------------
 * Judging a goal
 * ---------------------------------------------------------------- */

static gchar *
clip(const gchar *text, glong limit)
{
	g_autofree gchar *flat = g_strstrip(g_strdup(text != NULL ? text : ""));

	if (g_utf8_strlen(flat, -1) > limit)
	{
		g_autofree gchar *head = g_utf8_substring(flat, 0, limit);
		return g_strdup_printf("%s…", head);
	}

	return g_steal_pointer(&flat);
}

typedef enum
{
	VERDICT_NONE,
	VERDICT_MET,
	VERDICT_NOT_MET,
	VERDICT_BLOCKED
} Verdict;

/* The last verdict line wins: a reply that quotes the instruction, then
 * answers it, is judged by the answer. */
static Verdict
goal_verdict(const gchar *text, gchar **why)
{
	static const struct { const gchar *word; Verdict verdict; } words[] = {
		{ "GOAL_NOT_MET", VERDICT_NOT_MET },
		{ "GOAL_MET", VERDICT_MET },
		{ "GOAL_BLOCKED", VERDICT_BLOCKED },
	};
	g_auto(GStrv) lines = NULL;
	Verdict       found = VERDICT_NONE;
	gint          i;

	*why = NULL;

	if (text == NULL)
	{
		return VERDICT_NONE;
	}

	lines = g_strsplit(text, "\n", -1);

	for (i = 0; lines[i] != NULL; i++)
	{
		guint w;

		g_strstrip(lines[i]);

		for (w = 0; w < G_N_ELEMENTS(words); w++)
		{
			gsize len = strlen(words[w].word);

			if (g_str_has_prefix(lines[i], words[w].word) &&
			    (lines[i][len] == '\0' || lines[i][len] == ':'))
			{
				g_free(*why);
				*why = clip(lines[i][len] == ':' ? lines[i] + len + 1 : "", 400);
				found = words[w].verdict;
				break;
			}
		}
	}

	return found;
}

static gchar *
expire_goal(Task *task, gint64 now_us)
{
	g_autofree gchar *last = g_strdup(task->reason != NULL ? task->reason : "none");
	g_autofree gchar *span = ai_loop_format_duration(task->max_duration_us);

	task->state = AI_LOOP_STATE_EXPIRED;
	task->ended_us = now_us;
	task->run_requested = FALSE;
	g_free(task->reason);

	if (task->turns >= task->max_turns)
	{
		task->reason = g_strdup_printf("not met after %u turn%s, its bound. Last check: %s",
		                               task->turns, task->turns == 1 ? "" : "s", last);
	}
	else
	{
		task->reason = g_strdup_printf("not met within %s, its time bound, after %u turn%s. Last check: %s",
		                               span, task->turns, task->turns == 1 ? "" : "s", last);
	}

	return g_strdup_printf("Goal %s expired: %s", task->id, task->reason);
}

/**
 * ai_loop_schedule_goal_complete:
 * @self: a schedule
 * @id: the goal whose turn just ended
 * @assistant_text: (nullable): the model's reply
 * @turn_error: (nullable): why the turn failed, or %NULL if it did not
 * @now_us: real time in microseconds
 *
 * Judges the turn. The judge is the working model's own final verdict
 * line, and the model can be wrong; what guarantees an end is the bound,
 * which is checked here whatever the verdict said. No verdict line counts
 * as not met. Three failed turns in a row end the goal as failed.
 *
 * Returns: (transfer full) (nullable): a line to show, or %NULL if @id is
 *   not a goal
 */
gchar *
ai_loop_schedule_goal_complete(
	AiLoopSchedule *self,
	const gchar    *id,
	const gchar    *assistant_text,
	const gchar    *turn_error,
	gint64          now_us
){
	Task             *task;
	g_autofree gchar *why = NULL;
	Verdict           verdict = VERDICT_NONE;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);
	task = task_by_id(self, id);

	if (task == NULL || task->kind != AI_LOOP_KIND_GOAL)
	{
		return NULL;
	}

	task->inflight = FALSE;

	if (ai_loop_state_is_final(task->state))
	{
		return NULL;
	}

	g_free(task->reason);

	if (turn_error != NULL)
	{
		g_autofree gchar *what = clip(turn_error, 200);

		task->errors++;
		task->reason = g_strdup_printf("the turn failed: %s", what);

		if (task->errors >= GOAL_MAX_ERRORS)
		{
			task->state = AI_LOOP_STATE_FAILED;
			task->ended_us = now_us;
			g_free(task->reason);
			task->reason = g_strdup_printf("%u turns in a row failed; the last: %s",
			                               task->errors, what);
			return g_strdup_printf("Goal %s failed: %s", task->id, task->reason);
		}
	}
	else
	{
		task->errors = 0;
		verdict = goal_verdict(assistant_text, &why);

		switch (verdict)
		{
		case VERDICT_MET:
			task->state = AI_LOOP_STATE_MET;
			task->ended_us = now_us;
			task->reason = g_strdup(why != NULL && why[0] != '\0' ? why
				: "the model reported it met without saying how");
			return g_strdup_printf("Goal %s met after %u turn%s: %s", task->id,
			                       task->turns, task->turns == 1 ? "" : "s", task->reason);
		case VERDICT_BLOCKED:
			task->state = AI_LOOP_STATE_FAILED;
			task->ended_us = now_us;
			task->reason = g_strdup_printf("blocked: %s",
				why != NULL && why[0] != '\0' ? why : "no reason given");
			return g_strdup_printf("Goal %s failed: %s", task->id, task->reason);
		case VERDICT_NOT_MET:
			task->reason = g_strdup(why != NULL && why[0] != '\0' ? why : "no reason given");
			break;
		case VERDICT_NONE:
		default:
			task->reason = g_strdup("the reply had no GOAL_ verdict line");
			break;
		}
	}

	if (task->turns >= task->max_turns || now_us >= task->deadline_us)
	{
		return expire_goal(task, now_us);
	}

	task->fire_us = turn_error != NULL ? now_us + GOAL_RETRY_US : now_us;

	if (task->state == AI_LOOP_STATE_PAUSED)
	{
		return g_strdup_printf("Goal %s paused after turn %u/%u: %s", task->id,
		                       task->turns, task->max_turns, task->reason);
	}

	return g_strdup_printf("Goal %s not met yet (turn %u/%u): %s", task->id,
	                       task->turns, task->max_turns, task->reason);
}

/**
 * ai_loop_schedule_reap:
 * @self: a schedule
 * @now_us: real time in microseconds
 *
 * Ends what has run out of time while nothing was running: a goal past
 * its time bound -- including one that passed it while the application
 * was closed -- and a paused loop past its seven days. Call it before
 * asking what is due.
 *
 * Returns: (transfer full) (nullable): lines to show, or %NULL
 */
gchar *
ai_loop_schedule_reap(AiLoopSchedule *self, gint64 now_us)
{
	g_autoptr(GString) text = g_string_new(NULL);
	guint              i = 0;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);

	while (i < self->tasks->len)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (task->inflight || ai_loop_state_is_final(task->state))
		{
			i++;
			continue;
		}

		if (task->kind == AI_LOOP_KIND_GOAL &&
		    (now_us >= task->deadline_us || task->turns >= task->max_turns))
		{
			g_autofree gchar *notice = expire_goal(task, now_us);

			g_string_append_printf(text, "%s%s", text->len > 0 ? "\n" : "", notice);
		}
		else if (task->kind == AI_LOOP_KIND_LOOP && task->expires_us > 0 &&
		         now_us >= task->expires_us &&
		         (task->dynamic || task->state == AI_LOOP_STATE_PAUSED))
		{
			g_string_append_printf(text, "%sLoop %s expired: loops end 7 days after they are created.",
			                       text->len > 0 ? "\n" : "", task->id);
			task_remove(self, task);
			continue;
		}

		i++;
	}

	prune_finished(self, GOAL_MAX_FINISHED);
	return text->len > 0 ? g_strdup(text->str) : NULL;
}

/* ----------------------------------------------------------------
 * /goal
 * ---------------------------------------------------------------- */

static gchar *
edit_goal(AiLoopSchedule *self, GArray *tokens, gint64 now_us, GError **error)
{
	Task             *task;
	g_autofree gchar *id = NULL;
	guint             i;
	guint             turns = 0;
	gint64            seconds = 0;
	g_autofree gchar *condition = NULL;

	task = resolve(self, g_array_index(tokens, Token, 1).word, AI_LOOP_KIND_GOAL, error);

	if (task == NULL)
	{
		return NULL;
	}

	id = g_strdup(task->id);

	for (i = 2; i < tokens->len; i++)
	{
		const gchar *word = g_array_index(tokens, Token, i).word;
		const gchar *value = i + 1 < tokens->len ? g_array_index(tokens, Token, i + 1).word : NULL;

		if (word_is(word, "--turns", NULL))
		{
			if (!parse_count(value, &turns))
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				                    "--turns needs a number from 1 to 200.");
				return NULL;
			}

			i++;
		}
		else if (word_is(word, "--time", NULL))
		{
			if (!parse_duration(value, &seconds))
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				                    "--time needs a span such as 30m, 2h or 1d.");
				return NULL;
			}

			i++;
		}
		else if (word_is(word, "--condition", NULL))
		{
			condition = g_strdup(rest_after(tokens, i));
			break;
		}
		else
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "Unknown option %s. Usage: /goal edit ID [--turns N] [--time SPAN] [--condition TEXT]",
			            word);
			return NULL;
		}
	}

	if (turns == 0 && seconds == 0 && condition == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Usage: /goal edit ID [--turns N] [--time SPAN] [--condition TEXT]");
		return NULL;
	}

	/* Check everything before changing anything, so a bad option leaves
	 * the goal exactly as it was. */
	if (!check_bounds(turns, seconds * G_USEC_PER_SEC, error))
	{
		return NULL;
	}

	if (condition != NULL && !ai_loop_schedule_set_condition(self, id, condition, error))
	{
		return NULL;
	}

	if ((turns != 0 || seconds != 0) &&
	    !ai_loop_schedule_set_bounds(self, id, turns, seconds * G_USEC_PER_SEC, now_us, error))
	{
		return NULL;
	}

	{
		g_autofree gchar *line = ai_loop_schedule_dup_line(self, (guint)ai_loop_schedule_find(self, id), now_us);
		return g_strdup_printf("Updated goal %s: %s", id, line);
	}
}

/* Trailing --turns N and --time SPAN, peeled off the end of a new goal. */
static gboolean
goal_options(GArray *tokens, guint *count, guint *turns, gint64 *seconds, GError **error)
{
	*count = tokens->len;

	while (*count >= 2)
	{
		const gchar *word = g_array_index(tokens, Token, *count - 2).word;
		const gchar *value = g_array_index(tokens, Token, *count - 1).word;

		if (g_strcmp0(word, "--turns") == 0)
		{
			if (!parse_count(value, turns))
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				                    "--turns needs a number from 1 to 200.");
				return FALSE;
			}
		}
		else if (g_strcmp0(word, "--time") == 0)
		{
			if (!parse_duration(value, seconds))
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				                    "--time needs a span such as 30m, 2h or 1d.");
				return FALSE;
			}
		}
		else
		{
			break;
		}

		*count -= 2;
	}

	return TRUE;
}

/**
 * ai_loop_schedule_goal_command:
 * @self: a schedule
 * @arguments: (nullable): the text after `/goal`
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Applies one `/goal` line: `CONDITION [--turns N] [--time SPAN]` starts a
 * goal; `list`, `show ID`, `edit ID ...`, `pause ID|all`, `resume ID|all`,
 * `run ID`, `stop ID|all` and `delete ID|all` manage them. As with /loop,
 * a verb followed by more than one word is a condition.
 *
 * Returns: (transfer full) (nullable): the line to show the user, or %NULL
 *   on error
 */
gchar *
ai_loop_schedule_goal_command(
	AiLoopSchedule *self,
	const gchar    *arguments,
	gint64          now_us,
	GError        **error
){
	static const gchar *const verbs[] = {
		"show", "pause", "resume", "run", "run-now", "stop", "delete", "cancel", NULL
	};
	g_autofree gchar *text = NULL;
	GArray           *tokens;
	const gchar      *verb;
	gchar            *result = NULL;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);
	g_return_val_if_fail(error == NULL || *error == NULL, NULL);

	if (ai_loop_schedule_is_disabled())
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		                    "Goals are disabled (AI_LOOP_DISABLE).");
		return NULL;
	}

	text = g_strstrip(g_strdup(arguments != NULL ? arguments : ""));

	if (text[0] == '\0' || g_strcmp0(text, "list") == 0)
	{
		return format_list(self, AI_LOOP_KIND_GOAL, now_us);
	}

	tokens = tokenize(text);
	verb = loop_verb(tokens, verbs);

	if (verb == NULL && tokens->len == 1 &&
	    (g_strcmp0(text, "stop") == 0 || g_strcmp0(text, "delete") == 0 ||
	     g_strcmp0(text, "pause") == 0 || g_strcmp0(text, "resume") == 0 ||
	     g_strcmp0(text, "show") == 0 || g_strcmp0(text, "run") == 0))
	{
		tokens_free(tokens);
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Usage: /goal %s <id|all>", text);
		return NULL;
	}

	if (verb != NULL)
	{
		const gchar *which = g_array_index(tokens, Token, 1).word;

		if (g_strcmp0(verb, "show") == 0)
			result = show_one(self, AI_LOOP_KIND_GOAL, which, now_us, error);
		else if (g_strcmp0(verb, "edit") == 0)
			result = edit_goal(self, tokens, now_us, error);
		else if (g_strcmp0(verb, "pause") == 0)
			result = apply_verb(self, AI_LOOP_KIND_GOAL, which, each_pause, "Paused", now_us, error);
		else if (g_strcmp0(verb, "resume") == 0)
			result = apply_verb(self, AI_LOOP_KIND_GOAL, which, each_resume, "Resumed", now_us, error);
		else if (g_strcmp0(verb, "run") == 0 || g_strcmp0(verb, "run-now") == 0)
			result = apply_verb(self, AI_LOOP_KIND_GOAL, which, each_run,
			                    "Queued to run when the session is idle:", now_us, error);
		else if (g_strcmp0(verb, "stop") == 0)
			result = apply_verb(self, AI_LOOP_KIND_GOAL, which, each_stop, "Stopped", now_us, error);
		else
			result = apply_verb(self, AI_LOOP_KIND_GOAL, which, each_remove, "Deleted", now_us, error);

		tokens_free(tokens);
		return result;
	}

	{
		guint             count = 0;
		guint             turns = 0;
		gint64            seconds = 0;
		g_autofree gchar *condition = NULL;
		g_autofree gchar *id = NULL;

		if (!goal_options(tokens, &count, &turns, &seconds, error))
		{
			tokens_free(tokens);
			return NULL;
		}

		condition = count == tokens->len ? g_strdup(text)
			: g_strndup(text, (gsize)(g_array_index(tokens, Token, count).start - text));
		tokens_free(tokens);
		id = ai_loop_schedule_add_goal(self, condition, turns, seconds * G_USEC_PER_SEC, now_us, error);

		if (id == NULL)
		{
			return NULL;
		}

		{
			Task             *task = task_by_id(self, id);
			g_autofree gchar *span = ai_loop_format_duration(task->max_duration_us);

			return g_strdup_printf(
				"Goal %s set: %s. It runs as soon as the session is idle and continues "
				"until the condition is met, for at most %u turns or %s.",
				id, task->condition, task->max_turns, span);
		}
	}
}

/* ----------------------------------------------------------------
 * What every frontend shows
 * ---------------------------------------------------------------- */

#define TASK_AT_OR_RETURN(val) \
	Task *task = task_at(self, index); \
	if (task == NULL) return (val)

AiLoopKind
ai_loop_schedule_get_kind(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(AI_LOOP_KIND_LOOP);
	return task->kind;
}

AiLoopState
ai_loop_schedule_get_state(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(AI_LOOP_STATE_ACTIVE);
	return task->state;
}

const gchar *
ai_loop_schedule_get_condition(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(NULL);
	return task->condition;
}

const gchar *
ai_loop_schedule_get_reason(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(NULL);
	return task->reason;
}

guint
ai_loop_schedule_get_turns(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(0);
	return task->turns;
}

guint
ai_loop_schedule_get_max_turns(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(0);
	return task->max_turns;
}

gint64
ai_loop_schedule_get_deadline_us(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(0);
	return task->deadline_us;
}

gboolean
ai_loop_schedule_get_inflight(AiLoopSchedule *self, guint index)
{
	TASK_AT_OR_RETURN(FALSE);
	return task->inflight;
}

#undef TASK_AT_OR_RETURN

/**
 * ai_loop_schedule_count_live:
 * @self: a schedule
 * @kind: loops or goals
 *
 * Returns: how many entries of @kind are active or paused
 */
guint
ai_loop_schedule_count_live(AiLoopSchedule *self, AiLoopKind kind)
{
	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), 0);
	return count_kind(self, kind, TRUE);
}

/**
 * ai_loop_schedule_get_next_fire_us:
 * @self: a schedule
 *
 * Returns: the earliest time an active entry runs, or 0 when none will
 */
gint64
ai_loop_schedule_get_next_fire_us(AiLoopSchedule *self)
{
	gint64 best = 0;
	guint  i;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), 0);

	for (i = 0; i < self->tasks->len; i++)
	{
		Task  *task = g_ptr_array_index(self->tasks, i);
		gint64 when;

		if (task->inflight || ai_loop_state_is_final(task->state) ||
		    (task->state == AI_LOOP_STATE_PAUSED && !task->run_requested))
		{
			continue;
		}

		when = task->run_requested ? 1 : task->fire_us;

		if (best == 0 || when < best)
		{
			best = when;
		}
	}

	return best;
}

/**
 * ai_loop_schedule_get_revision:
 * @self: a schedule
 *
 * A token that changes on every save and is read back on load, so a
 * reader can tell whether the file is still the one it wrote.
 *
 * Returns: the revision token
 */
guint64
ai_loop_schedule_get_revision(AiLoopSchedule *self)
{
	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), 0);
	return self->revision;
}

static const gchar *
trigger_name(const Task *task)
{
	if (task->kind == AI_LOOP_KIND_GOAL)
		return "goal";
	if (task->prompt == NULL)
		return "maintenance";
	if (task->prompt[0] == '/')
		return "command";
	return "prompt";
}

static gchar *
when_text(const Task *task, gint64 now_us)
{
	g_autofree gchar *rel = NULL;

	if (task->inflight)
		return g_strdup("running");
	if (task->run_requested)
		return g_strdup("due now");
	if (task->state != AI_LOOP_STATE_ACTIVE)
		return g_strdup("not scheduled");
	if (task->fire_us <= now_us)
		return g_strdup("due now");

	rel = ai_loop_format_relative(task->fire_us - now_us);
	return g_strdup_printf("next %s", rel);
}

static gchar *
goal_progress(const Task *task, gint64 now_us)
{
	g_autofree gchar *left = NULL;

	if (ai_loop_state_is_final(task->state))
	{
		return g_strdup_printf("%u/%u turns", task->turns, task->max_turns);
	}

	left = ai_loop_format_duration(MAX(task->deadline_us - now_us, 0));
	return g_strdup_printf("turn %u/%u, %s left", task->turns, task->max_turns, left);
}

/**
 * ai_loop_schedule_dup_line:
 * @self: a schedule
 * @index: zero-based index
 * @now_us: real time in microseconds
 *
 * One entry on one line, the way `/loop list`, `/goal list`, `ai loop
 * list`, the ai-tui panel and the ai-gui view all show it:
 *
 * |[
 * 1a2b3c4d  loop  active  every 5m  next in 4m  check the deploy
 * 5e6f7a8b  goal  active  turn 3/20, 1h 50m left  the tests pass
 * ]|
 *
 * Returns: (transfer full) (nullable): the line, or %NULL for a bad index
 */
gchar *
ai_loop_schedule_dup_line(AiLoopSchedule *self, guint index, gint64 now_us)
{
	Task             *task = task_at(self, index);
	g_autofree gchar *excerpt = NULL;

	if (task == NULL)
	{
		return NULL;
	}

	if (task->kind == AI_LOOP_KIND_GOAL)
	{
		g_autofree gchar *progress = goal_progress(task, now_us);

		excerpt = prompt_excerpt(task->condition);

		if (task->inflight)
		{
			return g_strdup_printf("%s  goal  %s  %s, running  %s", task->id,
			                       ai_loop_state_to_string(task->state), progress, excerpt);
		}

		return g_strdup_printf("%s  goal  %s  %s  %s", task->id,
		                       ai_loop_state_to_string(task->state), progress, excerpt);
	}

	{
		g_autofree gchar *when = when_text(task, now_us);

		excerpt = prompt_excerpt(task->prompt);
		return g_strdup_printf("%s  loop  %s  %s  %s  %s", task->id,
		                       ai_loop_state_to_string(task->state),
		                       task->cadence != NULL ? task->cadence : "scheduled",
		                       when, excerpt);
	}
}

/**
 * ai_loop_schedule_dup_details:
 * @self: a schedule
 * @index: zero-based index
 * @now_us: real time in microseconds
 *
 * Everything about one entry, for `show`.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL for a bad index
 */
gchar *
ai_loop_schedule_dup_details(AiLoopSchedule *self, guint index, gint64 now_us)
{
	Task    *task = task_at(self, index);
	GString *text;

	if (task == NULL)
	{
		return NULL;
	}

	text = g_string_new(NULL);
	g_string_append_printf(text, "%s %s\n", task->kind == AI_LOOP_KIND_GOAL ? "Goal" : "Loop", task->id);
	g_string_append_printf(text, "  State:      %s%s\n", ai_loop_state_to_string(task->state),
	                       task->inflight ? ", running now" : "");

	if (task->kind == AI_LOOP_KIND_GOAL)
	{
		g_autofree gchar *progress = goal_progress(task, now_us);
		g_autofree gchar *span = ai_loop_format_duration(task->max_duration_us);

		g_string_append_printf(text, "  Condition:  %s\n", task->condition);
		g_string_append_printf(text, "  Progress:   %s\n", progress);
		g_string_append_printf(text, "  Bounds:     %u turns, %s\n", task->max_turns, span);

		if (!ai_loop_state_is_final(task->state))
		{
			g_autofree gchar *when = when_text(task, now_us);
			g_string_append_printf(text, "  Next turn:  %s\n", when);
		}

		if (task->reason != NULL)
		{
			g_string_append_printf(text, "  %s %s\n",
			                       ai_loop_state_is_final(task->state) ? "Ended:     " : "Last check:",
			                       task->reason);
		}
	}
	else
	{
		g_autofree gchar *when = when_text(task, now_us);

		g_string_append_printf(text, "  Trigger:    %s\n", trigger_name(task));
		g_string_append_printf(text, "  Cadence:    %s", task->cadence != NULL ? task->cadence : "scheduled");

		if (task->cron_text != NULL)
		{
			g_string_append_printf(text, " (%s)", task->cron_text);
		}

		g_string_append_printf(text, "\n  Next:       %s", when);

		if (!task->inflight && task->state == AI_LOOP_STATE_ACTIVE && task->fire_us > now_us)
		{
			g_autofree gchar *local = format_local(task->fire_us);
			g_string_append_printf(text, " (%s local)", local);
		}

		g_string_append_c(text, '\n');

		if (task->expires_us > 0)
		{
			g_autofree gchar *rel = ai_loop_format_relative(task->expires_us - now_us);
			g_string_append_printf(text, "  Expires:    %s\n", rel);
		}

		g_string_append_printf(text, "  Prompt:     %s\n",
		                       task->prompt != NULL ? task->prompt : "(maintenance prompt, from loop.md when present)");
	}

	if (text->len > 0 && text->str[text->len - 1] == '\n')
	{
		g_string_truncate(text, text->len - 1);
	}

	return g_string_free(text, FALSE);
}

/**
 * ai_loop_schedule_dup_summary:
 * @self: a schedule
 * @now_us: real time in microseconds
 *
 * The one-line count a dashboard row or a sidebar shows:
 * "2 loops, 1 goal, next in 4m". Only active and paused entries count.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when nothing is
 *   scheduled
 */
gchar *
ai_loop_schedule_dup_summary(AiLoopSchedule *self, gint64 now_us)
{
	guint    loops;
	guint    goals;
	gint64   next;
	gboolean running = FALSE;
	GString *text;
	guint    i;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);

	loops = count_kind(self, AI_LOOP_KIND_LOOP, TRUE);
	goals = count_kind(self, AI_LOOP_KIND_GOAL, TRUE);

	if (loops == 0 && goals == 0)
	{
		return NULL;
	}

	for (i = 0; i < self->tasks->len; i++)
	{
		running = running || ((Task *)g_ptr_array_index(self->tasks, i))->inflight;
	}

	text = g_string_new(NULL);

	if (loops > 0)
	{
		g_string_append_printf(text, "%u loop%s", loops, loops == 1 ? "" : "s");
	}

	if (goals > 0)
	{
		g_string_append_printf(text, "%s%u goal%s", text->len > 0 ? ", " : "", goals, goals == 1 ? "" : "s");
	}

	next = ai_loop_schedule_get_next_fire_us(self);

	if (running)
	{
		g_string_append(text, ", running");
	}
	else if (next != 0)
	{
		g_autofree gchar *rel = ai_loop_format_relative(next - now_us);
		g_string_append_printf(text, ", next %s", rel);
	}
	else
	{
		g_string_append(text, ", all paused");
	}

	return g_string_free(text, FALSE);
}

/**
 * ai_loop_schedule_dup_json:
 * @self: a schedule
 * @now_us: real time in microseconds
 *
 * Every entry as a JSON array, for `ai loop list --json` and anything
 * else that wants the fields rather than the sentence. `state` is the
 * same word ai_loop_state_to_string() gives; `line` is
 * ai_loop_schedule_dup_line().
 *
 * Returns: (transfer full): a JSON array
 */
gchar *
ai_loop_schedule_dup_json(AiLoopSchedule *self, gint64 now_us)
{
	g_autoptr(JsonBuilder)   builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode)      root = NULL;
	guint                    i;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), NULL);

	json_builder_begin_array(builder);

	for (i = 0; i < self->tasks->len; i++)
	{
		Task             *task = g_ptr_array_index(self->tasks, i);
		g_autofree gchar *line = ai_loop_schedule_dup_line(self, i, now_us);
		gint64            next = 0;

		if (!task->inflight && !ai_loop_state_is_final(task->state) &&
		    (task->state == AI_LOOP_STATE_ACTIVE || task->run_requested))
		{
			next = task->run_requested ? now_us : task->fire_us;
		}

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, task->id);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, ai_loop_kind_to_string(task->kind));
		json_builder_set_member_name(builder, "state");
		json_builder_add_string_value(builder, ai_loop_state_to_string(task->state));
		json_builder_set_member_name(builder, "running");
		json_builder_add_boolean_value(builder, task->inflight);
		json_builder_set_member_name(builder, "trigger");
		json_builder_add_string_value(builder, trigger_name(task));
		json_builder_set_member_name(builder, "next_fire_us");
		json_builder_add_int_value(builder, next);

		if (task->kind == AI_LOOP_KIND_GOAL)
		{
			json_builder_set_member_name(builder, "condition");
			json_builder_add_string_value(builder, task->condition);
			json_builder_set_member_name(builder, "turns");
			json_builder_add_int_value(builder, task->turns);
			json_builder_set_member_name(builder, "max_turns");
			json_builder_add_int_value(builder, task->max_turns);
			json_builder_set_member_name(builder, "deadline_us");
			json_builder_add_int_value(builder, task->deadline_us);
		}
		else
		{
			json_builder_set_member_name(builder, "cadence");
			json_builder_add_string_value(builder, task->cadence);
			json_builder_set_member_name(builder, "self_paced");
			json_builder_add_boolean_value(builder, task->dynamic);
			json_builder_set_member_name(builder, "interval_us");
			json_builder_add_int_value(builder, task->interval_us);
			json_builder_set_member_name(builder, "prompt");

			if (task->prompt != NULL)
				json_builder_add_string_value(builder, task->prompt);
			else
				json_builder_add_null_value(builder);

			json_builder_set_member_name(builder, "expires_us");
			json_builder_add_int_value(builder, task->expires_us);
		}

		json_builder_set_member_name(builder, "reason");

		if (task->reason != NULL)
			json_builder_add_string_value(builder, task->reason);
		else
			json_builder_add_null_value(builder);

		json_builder_set_member_name(builder, "line");
		json_builder_add_string_value(builder, line);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

/* ----------------------------------------------------------------
 * Persistence. Version 2 keeps everything -- fixed and self-paced
 * loops, goals finished or not -- because the point of the file is that
 * closing the application does not forget what it was asked to do.
 * Version 1 files, which held fixed loops only, still load.
 *
 * A symlink is refused so a schedule file cannot be pointed at somewhere
 * else between runs. Writes are atomic (write, fsync, rename).
 * ---------------------------------------------------------------- */

static gboolean
path_is_link(const gchar *path)
{
	GStatBuf status;

	return g_lstat(path, &status) == 0 && S_ISLNK(status.st_mode);
}

static gboolean
refuse_link(const gchar *path, GError **error)
{
	g_autofree gchar *parent = g_path_get_dirname(path);

	if (path_is_link(path) || path_is_link(parent))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_PERMISSION_DENIED,
		            "Refusing to use a symlinked loop file: %s", path);
		return TRUE;
	}

	return FALSE;
}

static void
remember_file(AiLoopSchedule *self, const gchar *path)
{
	GStatBuf status;

	if (g_stat(path, &status) == 0)
	{
		self->seen_mtime = (gint64)status.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) +
		                   status.st_mtim.tv_nsec;
		self->seen_size = (gint64)status.st_size;
		self->seen_inode = (guint64)status.st_ino;
	}
	else
	{
		self->seen_mtime = -1;
		self->seen_size = -1;
		self->seen_inode = 0;
	}
}

static void
add_int(JsonBuilder *builder, const gchar *name, gint64 value)
{
	json_builder_set_member_name(builder, name);
	json_builder_add_int_value(builder, value);
}

static void
add_string(JsonBuilder *builder, const gchar *name, const gchar *value)
{
	if (value != NULL)
	{
		json_builder_set_member_name(builder, name);
		json_builder_add_string_value(builder, value);
	}
}

gboolean
ai_loop_schedule_save(AiLoopSchedule *self, const gchar *path, GError **error)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *data = NULL;
	g_autofree gchar *revision = NULL;
	gsize             length = 0;
	guint64           next_revision;
	guint             i;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), FALSE);
	g_return_val_if_fail(path != NULL, FALSE);

	if (refuse_link(path, error))
	{
		return FALSE;
	}

	parent = g_path_get_dirname(path);

	if (g_mkdir_with_parents(parent, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot create %s: %s", parent, g_strerror(errno));
		return FALSE;
	}

	/* A random token, not a counter: two writers that both started from
	 * revision N would otherwise both write N+1 and neither could tell. */
	do
	{
		next_revision = ((guint64)g_random_int() << 32) | g_random_int();
	}
	while (next_revision == 0 || next_revision == self->revision);

	revision = g_strdup_printf("%016" G_GINT64_MODIFIER "x", next_revision);
	builder = json_builder_new();
	json_builder_begin_object(builder);
	add_int(builder, "version", LOOP_FILE_VERSION);
	add_string(builder, "revision", revision);
	json_builder_set_member_name(builder, "tasks");
	json_builder_begin_array(builder);

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		json_builder_begin_object(builder);
		add_string(builder, "id", task->id);
		add_string(builder, "kind", ai_loop_kind_to_string(task->kind));
		add_string(builder, "state", ai_loop_state_to_string(task->state));
		add_string(builder, "cron", task->cron_text);
		add_string(builder, "cadence", task->cadence);
		add_string(builder, "prompt", task->prompt);
		add_string(builder, "condition", task->condition);
		add_string(builder, "reason", task->reason);
		json_builder_set_member_name(builder, "self_paced");
		json_builder_add_boolean_value(builder, task->dynamic);
		json_builder_set_member_name(builder, "fallback_armed");
		json_builder_add_boolean_value(builder, task->fallback_armed);
		json_builder_set_member_name(builder, "run_requested");
		json_builder_add_boolean_value(builder, task->run_requested);
		add_int(builder, "created_us", task->created_us);
		add_int(builder, "expires_us", task->expires_us);
		add_int(builder, "nominal_us", task->nominal_us);
		add_int(builder, "fire_us", task->fire_us);
		add_int(builder, "interval_us", task->interval_us);
		add_int(builder, "turns", task->turns);
		add_int(builder, "max_turns", task->max_turns);
		add_int(builder, "max_duration_us", task->max_duration_us);
		add_int(builder, "deadline_us", task->deadline_us);
		add_int(builder, "ended_us", task->ended_us);
		add_int(builder, "errors", task->errors);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	generator = json_generator_new();
	json_generator_set_pretty(generator, TRUE);
	json_generator_set_root(generator, root);
	data = json_generator_to_data(generator, &length);

	if (length > LOOP_FILE_MAX)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Loop file exceeds 2 MiB.");
		return FALSE;
	}

	if (!g_file_set_contents_full(path, data, length,
	                              G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
	{
		return FALSE;
	}

	self->revision = next_revision;
	remember_file(self, path);
	return TRUE;
}

static gboolean
load_task_v1(JsonObject *object, gint64 now_us, Task **out)
{
	const gchar *id;
	const gchar *cron_text;
	const gchar *cadence;
	const gchar *prompt;
	Task        *task;
	g_autoptr(GError) error = NULL;

	id = ai_json_get_string(object, "id", NULL);
	cron_text = ai_json_get_string(object, "cron", NULL);
	cadence = ai_json_get_string(object, "cadence", NULL);
	prompt = ai_json_get_string(object, "prompt", NULL);

	if (!id_is_hex8(id) || cron_text == NULL || cron_text[0] == '\0')
	{
		g_debug("skipping a loop record with no id or cron");
		return FALSE;
	}

	task = g_new0(Task, 1);
	task->id = g_ascii_strdown(id, -1);
	task->cron_text = g_strdup(cron_text);
	task->cadence = g_strdup(cadence != NULL && cadence[0] != '\0' ? cadence : "scheduled");
	task->prompt = prompt != NULL && prompt[0] != '\0' ? g_strdup(prompt) : NULL;
	task->created_us = ai_json_get_int(object, "created_us", 0);
	task->expires_us = ai_json_get_int(object, "expires_us", 0);
	task->nominal_us = ai_json_get_int(object, "nominal_us", 0);
	task->fire_us = ai_json_get_int(object, "fire_us", 0);
	task->interval_us = ai_json_get_int(object, "interval_us", 0);

	if (task->expires_us <= now_us || task->interval_us < LOOP_MIN_US ||
	    task->fire_us <= 0 || !cron_parse(&task->cron, task->cron_text, &error) ||
	    (task->prompt != NULL && strlen(task->prompt) > LOOP_PROMPT_MAX))
	{
		g_debug("skipping loop %s: expired or unreadable", id);
		task_free(task);
		return FALSE;
	}

	task->cron_ok = TRUE;
	*out = task;
	return TRUE;
}

static gboolean
load_task_v2(JsonObject *object, gint64 now_us, Task **out)
{
	const gchar *kind = ai_json_get_string(object, "kind", NULL);
	const gchar *state = ai_json_get_string(object, "state", NULL);
	const gchar *id = ai_json_get_string(object, "id", NULL);
	Task        *task = NULL;
	AiLoopState  parsed_state = AI_LOOP_STATE_ACTIVE;

	if (!id_is_hex8(id) || !state_from_string(state, &parsed_state))
	{
		g_debug("skipping a loop record with no id or an unknown state");
		return FALSE;
	}

	if (g_strcmp0(kind, "goal") == 0)
	{
		const gchar *condition = ai_json_get_string(object, "condition", NULL);
		const gchar *reason = ai_json_get_string(object, "reason", NULL);
		gint64       max_turns = ai_json_get_int(object, "max_turns", 0);
		gint64       turns = ai_json_get_int(object, "turns", -1);
		gint64       duration = ai_json_get_int(object, "max_duration_us", 0);

		if (condition == NULL || condition[0] == '\0' || strlen(condition) > GOAL_CONDITION_MAX ||
		    max_turns < 1 || max_turns > GOAL_MAX_TURNS || turns < 0 || turns > max_turns ||
		    duration < 60 * G_USEC_PER_SEC || duration > GOAL_MAX_US)
		{
			g_debug("skipping goal %s: unreadable bounds or condition", id);
			return FALSE;
		}

		task = g_new0(Task, 1);
		task->kind = AI_LOOP_KIND_GOAL;
		task->condition = g_strdup(condition);
		task->reason = reason != NULL ? g_strdup(reason) : NULL;
		task->turns = (guint)turns;
		task->max_turns = (guint)max_turns;
		task->max_duration_us = duration;
		task->created_us = ai_json_get_int(object, "created_us", 0);
		task->deadline_us = task->created_us + duration;
		task->expires_us = task->deadline_us;
		task->ended_us = ai_json_get_int(object, "ended_us", 0);
		task->errors = (guint)CLAMP(ai_json_get_int(object, "errors", 0), 0, GOAL_MAX_ERRORS);
	}
	else if (g_strcmp0(kind, "loop") == 0)
	{
		gboolean self_paced = ai_json_get_boolean(object, "self_paced", FALSE);
		const gchar *prompt = ai_json_get_string(object, "prompt", NULL);

		if (ai_loop_state_is_final(parsed_state) ||
		    (prompt != NULL && strlen(prompt) > LOOP_PROMPT_MAX))
		{
			g_debug("skipping loop %s: a loop is never finished, or its prompt is too long", id);
			return FALSE;
		}

		if (!self_paced)
		{
			if (!load_task_v1(object, now_us, &task))
			{
				return FALSE;
			}
		}
		else
		{
			task = g_new0(Task, 1);
			task->dynamic = TRUE;
			task->cadence = g_strdup("self-paced");
			task->prompt = prompt != NULL && prompt[0] != '\0' ? g_strdup(prompt) : NULL;
			task->created_us = ai_json_get_int(object, "created_us", 0);
			task->expires_us = ai_json_get_int(object, "expires_us", 0);
			task->fallback_armed = ai_json_get_boolean(object, "fallback_armed", FALSE);

			if (task->expires_us <= now_us)
			{
				g_debug("skipping loop %s: expired", id);
				task_free(task);
				return FALSE;
			}
		}
	}
	else
	{
		g_debug("skipping a loop record of unknown kind");
		return FALSE;
	}

	g_free(task->id);
	task->id = g_ascii_strdown(id, -1);
	task->state = parsed_state;
	task->fire_us = ai_json_get_int(object, "fire_us", 0);
	task->run_requested = ai_json_get_boolean(object, "run_requested", FALSE);
	*out = task;
	return TRUE;
}

static gint
load_into(AiLoopSchedule *self, const gchar *path, gint64 now_us, GError **error)
{
	g_autofree gchar     *data = NULL;
	gsize                 length = 0;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject           *root;
	JsonArray            *tasks;
	gint64                version;
	const gchar          *revision;
	guint                 loops = 0;
	guint                 i;
	guint                 n;

	g_ptr_array_set_size(self->tasks, 0);
	self->revision = 0;

	if (!g_file_test(path, G_FILE_TEST_EXISTS) && !path_is_link(path))
	{
		remember_file(self, path);
		return 0;
	}

	if (refuse_link(path, error))
	{
		return -1;
	}

	remember_file(self, path);

	if (!g_file_get_contents(path, &data, &length, error))
	{
		return -1;
	}

	if (length > LOOP_FILE_MAX)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Loop file exceeds 2 MiB.");
		return -1;
	}

	if (!g_utf8_validate(data, (gssize)length, NULL) || memchr(data, '\0', length) != NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Loop file is not valid UTF-8.");
		return -1;
	}

	parser = json_parser_new();

	if (!json_parser_load_from_data(parser, data, (gssize)length, error))
	{
		return -1;
	}

	root = ai_json_root_object(parser);
	version = root != NULL ? ai_json_get_int(root, "version", 0) : 0;

	if (version != 1 && version != LOOP_FILE_VERSION)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Loop file is version %" G_GINT64_FORMAT "; this build reads 1 and 2.", version);
		return -1;
	}

	tasks = ai_json_get_array(root, "tasks");

	if (tasks == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Loop file has no task list.");
		return -1;
	}

	revision = ai_json_get_string(root, "revision", NULL);
	self->revision = revision != NULL ? g_ascii_strtoull(revision, NULL, 16) : 0;
	n = json_array_get_length(tasks);

	/* One bad record costs itself. The file is ours, but it is on disk,
	 * and a hand edit or a crash mid-way must not hide the good ones. */
	for (i = 0; i < n && self->tasks->len < LOOP_MAX_ENTRIES; i++)
	{
		JsonObject *object = ai_json_array_get_object(tasks, i);
		Task       *task = NULL;
		gboolean    ok;

		if (object == NULL)
		{
			g_debug("skipping a loop record that is not an object");
			continue;
		}

		ok = version == 1 ? load_task_v1(object, now_us, &task)
			: load_task_v2(object, now_us, &task);

		if (!ok)
		{
			continue;
		}

		if (task_by_id(self, task->id) != NULL ||
		    (task->kind == AI_LOOP_KIND_LOOP && loops >= LOOP_MAX_TASKS))
		{
			g_debug("skipping loop %s: duplicate id or over the cap", task->id);
			task_free(task);
			continue;
		}

		if (task->kind == AI_LOOP_KIND_LOOP)
		{
			loops++;
		}

		g_ptr_array_add(self->tasks, task);
	}

	return (gint)self->tasks->len;
}

/**
 * ai_loop_schedule_load:
 * @self: a schedule
 * @path: source file
 * @now_us: real time; expired loops are dropped
 * @error: (nullable): return location for a #GError
 *
 * Replaces @self with the entries in @path. A missing file loads nothing
 * and returns 0. A record that cannot be read is skipped with a g_debug
 * and costs nothing else. A goal that passed its time bound while the
 * file sat on disk is loaded as it was, and ends at the next
 * ai_loop_schedule_reap() with the reason saying so.
 *
 * Returns: the number of entries loaded, or -1 on error
 */
gint
ai_loop_schedule_load(
	AiLoopSchedule *self,
	const gchar    *path,
	gint64          now_us,
	GError        **error
){
	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), -1);
	g_return_val_if_fail(path != NULL, -1);

	return load_into(self, path, now_us, error);
}

/**
 * ai_loop_schedule_sync:
 * @self: a schedule
 * @path: the file @self was loaded from or saved to
 * @now_us: real time in microseconds
 * @error: (nullable): return location for a #GError
 *
 * Picks up an edit somebody else made -- `ai loop pause` from another
 * terminal -- by reloading @path when it is no longer the file @self last
 * wrote. A turn in progress is carried across the reload, so an edit
 * cannot make a running entry look idle and fire it twice. An unchanged
 * file is noticed from its size, inode and mtime without being parsed.
 *
 * Returns: 1 if @self was reloaded, 0 if nothing changed, -1 on error
 *   (@self is left as it was)
 */
gint
ai_loop_schedule_sync(AiLoopSchedule *self, const gchar *path, gint64 now_us, GError **error)
{
	GStatBuf                  status;
	gint64                    mtime;
	g_autoptr(AiLoopSchedule) fresh = NULL;
	g_autoptr(GHashTable)     running = NULL;
	guint                     i;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), -1);
	g_return_val_if_fail(path != NULL, -1);

	if (g_stat(path, &status) != 0)
	{
		if (self->seen_size == -1)
		{
			return 0;
		}

		/* Deleted from outside: `ai loop delete all`, or the session
		 * was removed. What was scheduled is gone. */
		fresh = ai_loop_schedule_new();
		remember_file(fresh, path);
	}
	else
	{
		mtime = (gint64)status.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) + status.st_mtim.tv_nsec;

		if (mtime == self->seen_mtime && (gint64)status.st_size == self->seen_size &&
		    (guint64)status.st_ino == self->seen_inode)
		{
			return 0;
		}

		fresh = ai_loop_schedule_new();

		if (load_into(fresh, path, now_us, error) < 0)
		{
			return -1;
		}

		if (fresh->revision != 0 && fresh->revision == self->revision)
		{
			self->seen_mtime = fresh->seen_mtime;
			self->seen_size = fresh->seen_size;
			self->seen_inode = fresh->seen_inode;
			return 0;
		}
	}

	running = g_hash_table_new(g_str_hash, g_str_equal);

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (task->inflight)
		{
			g_hash_table_add(running, task->id);
		}
	}

	for (i = 0; i < fresh->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(fresh->tasks, i);
		Task *mine = task_by_id(self, task->id);

		if (g_hash_table_contains(running, task->id) && mine != NULL)
		{
			task->inflight = TRUE;
			task->stop_requested = mine->stop_requested;

			/* The turn was counted when it started; the file may have
			 * been written before that. */
			if (task->kind == AI_LOOP_KIND_GOAL && task->turns < mine->turns)
			{
				task->turns = mine->turns;
			}
		}
	}

	g_ptr_array_unref(self->tasks);
	self->tasks = g_steal_pointer(&fresh->tasks);
	fresh->tasks = g_ptr_array_new_with_free_func(task_free);
	self->revision = fresh->revision;
	self->seen_mtime = fresh->seen_mtime;
	self->seen_size = fresh->seen_size;
	self->seen_inode = fresh->seen_inode;
	return 1;
}

/* ----------------------------------------------------------------
 * The store
 * ---------------------------------------------------------------- */

/**
 * ai_loop_store_default_directory:
 *
 * `$XDG_STATE_HOME/ai-glib/sessions/loops`, beside the dashboard's
 * session registry.
 *
 * Returns: (transfer full): the directory
 */
gchar *
ai_loop_store_default_directory(void)
{
	return g_build_filename(g_get_user_state_dir(), "ai-glib", "sessions", "loops", NULL);
}

static gboolean
owner_is_valid(const gchar *owner)
{
	const gchar *cursor;

	if (owner == NULL || owner[0] == '\0' || owner[0] == '.' || strlen(owner) > 128)
	{
		return FALSE;
	}

	for (cursor = owner; *cursor != '\0'; cursor++)
	{
		if (!g_ascii_isalnum(*cursor) && *cursor != '-' && *cursor != '_')
		{
			return FALSE;
		}
	}

	return TRUE;
}

/**
 * ai_loop_store_path:
 * @directory: (nullable): the store, or %NULL for the default
 * @owner: the owning session's id
 *
 * Returns: (transfer full) (nullable): the file, or %NULL when @owner is
 *   not a plain identifier -- it becomes a file name, so a `/` or `..`
 *   in it is refused rather than followed
 */
gchar *
ai_loop_store_path(const gchar *directory, const gchar *owner)
{
	g_autofree gchar *fallback = NULL;

	if (!owner_is_valid(owner))
	{
		return NULL;
	}

	if (directory == NULL)
	{
		fallback = ai_loop_store_default_directory();
		directory = fallback;
	}

	return g_build_filename(directory, owner, NULL);
}

static gint
compare_names(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/**
 * ai_loop_store_list_owners:
 * @directory: (nullable): the store, or %NULL for the default
 *
 * Returns: (transfer full) (array zero-terminated=1): every session with a
 *   schedule file, sorted
 */
gchar **
ai_loop_store_list_owners(const gchar *directory)
{
	g_autofree gchar  *fallback = NULL;
	g_autoptr(GDir)    dir = NULL;
	g_autoptr(GPtrArray) owners = g_ptr_array_new_with_free_func(g_free);
	const gchar       *name;

	if (directory == NULL)
	{
		fallback = ai_loop_store_default_directory();
		directory = fallback;
	}

	dir = g_dir_open(directory, 0, NULL);

	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = g_build_filename(directory, name, NULL);

		if (owner_is_valid(name) && g_file_test(path, G_FILE_TEST_IS_REGULAR) &&
		    !path_is_link(path))
		{
			g_ptr_array_add(owners, g_strdup(name));
		}
	}

	g_ptr_array_sort(owners, compare_names);
	g_ptr_array_add(owners, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&owners), FALSE);
}

/**
 * ai_loop_store_claim:
 * @directory: (nullable): the store, or %NULL for the default
 * @owner: the owning session's id
 * @error: (nullable): return location for a #GError
 *
 * Takes the advisory lock that says this process runs @owner's schedule.
 * Two processes firing one schedule would send every prompt twice, so the
 * second one is refused. Close the descriptor to release it.
 *
 * Returns: a file descriptor, or -1 with @error set (%G_IO_ERROR_BUSY when
 *   another process holds it)
 */
gint
ai_loop_store_claim(const gchar *directory, const gchar *owner, GError **error)
{
	g_autofree gchar *path = ai_loop_store_path(directory, owner);
	g_autofree gchar *lock = NULL;
	g_autofree gchar *parent = NULL;
	gint              fd;

	if (path == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A schedule owner must be a plain identifier.");
		return -1;
	}

	parent = g_path_get_dirname(path);

	if (g_mkdir_with_parents(parent, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot create %s: %s", parent, g_strerror(errno));
		return -1;
	}

	lock = g_strconcat(path, ".lock", NULL);

	if (path_is_link(lock))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_PERMISSION_DENIED,
		            "Refusing to use a symlinked lock: %s", lock);
		return -1;
	}

	fd = g_open(lock, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);

	if (fd < 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot open %s: %s", lock, g_strerror(errno));
		return -1;
	}

	if (flock(fd, LOCK_EX | LOCK_NB) != 0)
	{
		close(fd);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
		                    "Another running session already owns these loops and goals.");
		return -1;
	}

	return fd;
}

/**
 * ai_loop_store_remove:
 * @directory: (nullable): the store, or %NULL for the default
 * @owner: the owning session's id
 * @error: (nullable): return location for a #GError
 *
 * Deletes @owner's schedule. A session that is closed for good calls
 * this, so nothing it scheduled can fire later. A missing file is not an
 * error.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_store_remove(const gchar *directory, const gchar *owner, GError **error)
{
	g_autofree gchar *path = ai_loop_store_path(directory, owner);

	if (path == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A schedule owner must be a plain identifier.");
		return FALSE;
	}

	if (g_unlink(path) != 0 && errno != ENOENT)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot delete %s: %s", path, g_strerror(errno));
		return FALSE;
	}

	return TRUE;
}

static gchar *
resume_path(const gchar *directory, const gchar *key)
{
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *sum = NULL;

	if (key == NULL)
	{
		return NULL;
	}

	if (directory == NULL)
	{
		fallback = ai_loop_store_default_directory();
		directory = fallback;
	}

	sum = g_compute_checksum_for_string(G_CHECKSUM_SHA256, key, -1);
	return g_build_filename(directory, "resume", sum, NULL);
}

/**
 * ai_loop_store_set_resume:
 * @directory: (nullable): the store, or %NULL for the default
 * @key: what a later `--continue` will know, such as provider and directory
 * @owner: the session whose schedule that is
 * @error: (nullable): return location for a #GError
 *
 * Records which schedule a `--continue` from the same place should pick
 * up. It stores a pointer, not a copy: a copy would be a second file for
 * `ai loop pause` to miss.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_loop_store_set_resume(const gchar *directory, const gchar *key, const gchar *owner, GError **error)
{
	g_autofree gchar *path = resume_path(directory, key);
	g_autofree gchar *parent = NULL;
	g_autofree gchar *text = NULL;

	if (path == NULL || !owner_is_valid(owner))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "A schedule owner must be a plain identifier.");
		return FALSE;
	}

	if (refuse_link(path, error))
	{
		return FALSE;
	}

	parent = g_path_get_dirname(path);

	if (g_mkdir_with_parents(parent, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "Cannot create %s: %s", parent, g_strerror(errno));
		return FALSE;
	}

	text = g_strdup_printf("{\"version\": %d, \"owner\": \"%s\"}\n", LOOP_FILE_VERSION, owner);
	return g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

/**
 * ai_loop_store_dup_resume:
 * @directory: (nullable): the store, or %NULL for the default
 * @key: the same key given to ai_loop_store_set_resume()
 *
 * Returns: (transfer full) (nullable): the owner recorded for @key, or
 *   %NULL when there is none or the record is not a pointer -- a copy
 *   written by an older build, which the caller may load as a schedule
 */
gchar *
ai_loop_store_dup_resume(const gchar *directory, const gchar *key)
{
	g_autofree gchar     *path = resume_path(directory, key);
	g_autofree gchar     *data = NULL;
	gsize                 length = 0;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject           *root;
	const gchar          *owner;

	if (path == NULL || path_is_link(path) ||
	    !g_file_get_contents(path, &data, &length, NULL) || length > 4096 ||
	    !json_parser_load_from_data(parser, data, (gssize)length, NULL))
	{
		return NULL;
	}

	root = ai_json_root_object(parser);
	owner = root != NULL ? ai_json_get_string(root, "owner", NULL) : NULL;
	return owner_is_valid(owner) ? g_strdup(owner) : NULL;
}

/**
 * ai_loop_store_dup_summary:
 * @directory: (nullable): the store, or %NULL for the default
 * @owner: (nullable): a session id
 * @now_us: real time in microseconds
 *
 * ai_loop_schedule_dup_summary() for a session this process does not
 * run, read from its file. A dashboard row calls it; an unreadable file
 * gives %NULL, the same as none.
 *
 * Returns: (transfer full) (nullable): "2 loops, 1 goal, next in 4m", or
 *   %NULL
 */
gchar *
ai_loop_store_dup_summary(const gchar *directory, const gchar *owner, gint64 now_us)
{
	g_autofree gchar         *path = ai_loop_store_path(directory, owner);
	g_autoptr(AiLoopSchedule) schedule = NULL;

	if (path == NULL || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
	{
		return NULL;
	}

	schedule = ai_loop_schedule_new();

	if (load_into(schedule, path, now_us, NULL) <= 0)
	{
		return NULL;
	}

	return ai_loop_schedule_dup_summary(schedule, now_us);
}
