/*
 * ai-loop.c - Session-scoped scheduled prompts
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * /loop is the Claude Code scheduled-task shape, kept inside one session:
 * a fixed cron interval, a delay the model chooses, or the maintenance
 * prompt. The frontend decides when the session is idle and calls in.
 * Nothing here sleeps or owns a main-loop source.
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "core/ai-error.h"
#include "core/ai-json-util.h"
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
} Task;

struct _AiLoopSchedule
{
	GObject    parent_instance;
	GPtrArray *tasks;
};

G_DEFINE_TYPE(AiLoopSchedule, ai_loop_schedule, G_TYPE_OBJECT)

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
	g_free(task);
}

static void
ai_loop_schedule_finalize(GObject *object)
{
	AiLoopSchedule *self = AI_LOOP_SCHEDULE(object);

	g_clear_pointer(&self->tasks, g_ptr_array_unref);
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

	if (self->tasks->len >= LOOP_MAX_TASKS)
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

	if (self->tasks->len >= LOOP_MAX_TASKS)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "A session holds at most %d scheduled loops.", LOOP_MAX_TASKS);
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
format_list(AiLoopSchedule *self, gint64 now_us)
{
	GString *text;
	guint    i;

	if (self->tasks->len == 0)
	{
		return g_strdup("No scheduled loops.");
	}

	text = g_string_new("Scheduled loops\n");

	for (i = 0; i < self->tasks->len; i++)
	{
		Task             *task = g_ptr_array_index(self->tasks, i);
		g_autofree gchar *when = NULL;
		g_autofree gchar *excerpt = prompt_excerpt(task->prompt);
		const gchar      *state;

		if (task->inflight)
		{
			state = "running";
		}
		else if (task->fire_us <= now_us)
		{
			state = "due now";
		}
		else
		{
			when = format_local(task->fire_us);
			state = when;
		}

		g_string_append_printf(text, "%s  %-12s  %s  %s\n",
		                       task->id, task->cadence, state, excerpt);
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
cancel_tasks(AiLoopSchedule *self, const gchar *which, GError **error)
{
	guint removed = 0;

	if (g_strcmp0(which, "all") == 0)
	{
		removed = self->tasks->len;
		g_ptr_array_set_size(self->tasks, 0);

		if (removed == 0)
		{
			return g_strdup("No scheduled loops to cancel.");
		}

		return g_strdup_printf("Cancelled %u scheduled loop%s.",
		                       removed, removed == 1 ? "" : "s");
	}

	if (!id_is_hex8(which))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Usage: /loop cancel <id|all>");
		return NULL;
	}

	{
		Task *task = task_by_id(self, which);

		if (task == NULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "No scheduled loop %s.", which);
			return NULL;
		}

		task_remove(self, task);
	}

	return g_strdup_printf("Cancelled loop %s.", which);
}

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
	g_autofree gchar *text = NULL;
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
		return format_list(self, now_us);
	}

	if (g_strcmp0(text, "stop") == 0)
	{
		gchar *notice = ai_loop_schedule_stop_waiting(self);
		return notice != NULL ? notice : g_strdup("No self-paced loop is waiting.");
	}

	if (g_strcmp0(text, "cancel") == 0)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Usage: /loop cancel <id|all>");
		return NULL;
	}

	if (g_str_has_prefix(text, "cancel ") || g_str_has_prefix(text, "cancel\t"))
	{
		gchar *which = g_strdup(text + 6);
		gchar *notice;

		g_strstrip(which);

		if (g_strcmp0(which, "all") == 0 || id_is_hex8(which))
		{
			notice = cancel_tasks(self, which, error);
			g_free(which);
			return notice;
		}

		g_free(which);
	}

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

		if (task->inflight || task->fire_us > now_us)
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

	if (task->prompt != NULL)
	{
		body = g_strdup(task->prompt);
	}
	else
	{
		body = ai_loop_default_prompt(cwd, config_dir, home, NULL, NULL);
	}

	if (task->dynamic)
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
	Task *task;

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

	if (task->dynamic)
	{
		task->inflight = TRUE;
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

	if (task == NULL || !task->dynamic)
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
 * Persistence. Self-paced loops are not restored; Claude Code does not
 * restore them either. A symlink is refused so a schedule file cannot
 * be pointed at somewhere else between runs.
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

gboolean
ai_loop_schedule_save(AiLoopSchedule *self, const gchar *path, GError **error)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) generator = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *parent = NULL;
	g_autofree gchar *data = NULL;
	gsize             length = 0;
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

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, 1);
	json_builder_set_member_name(builder, "tasks");
	json_builder_begin_array(builder);

	for (i = 0; i < self->tasks->len; i++)
	{
		Task *task = g_ptr_array_index(self->tasks, i);

		if (task->dynamic)
		{
			continue;
		}

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_string_value(builder, task->id);
		json_builder_set_member_name(builder, "cron");
		json_builder_add_string_value(builder, task->cron_text);
		json_builder_set_member_name(builder, "cadence");
		json_builder_add_string_value(builder, task->cadence);
		json_builder_set_member_name(builder, "created_us");
		json_builder_add_int_value(builder, task->created_us);
		json_builder_set_member_name(builder, "expires_us");
		json_builder_add_int_value(builder, task->expires_us);
		json_builder_set_member_name(builder, "nominal_us");
		json_builder_add_int_value(builder, task->nominal_us);
		json_builder_set_member_name(builder, "fire_us");
		json_builder_add_int_value(builder, task->fire_us);
		json_builder_set_member_name(builder, "interval_us");
		json_builder_add_int_value(builder, task->interval_us);

		if (task->prompt != NULL)
		{
			json_builder_set_member_name(builder, "prompt");
			json_builder_add_string_value(builder, task->prompt);
		}

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

	return g_file_set_contents_full(path, data, length,
	                                G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

static gboolean
load_task(JsonObject *object, gint64 now_us, Task **out)
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
	task->id = g_strdup(id);
	task->cron_text = g_strdup(cron_text);
	task->cadence = g_strdup(cadence != NULL && cadence[0] != '\0' ? cadence : "scheduled");
	task->prompt = prompt != NULL && prompt[0] != '\0' ? g_strdup(prompt) : NULL;
	task->created_us = ai_json_get_int(object, "created_us", 0);
	task->expires_us = ai_json_get_int(object, "expires_us", 0);
	task->nominal_us = ai_json_get_int(object, "nominal_us", 0);
	task->fire_us = ai_json_get_int(object, "fire_us", 0);
	task->interval_us = ai_json_get_int(object, "interval_us", 0);

	if (task->expires_us <= now_us || task->interval_us < LOOP_MIN_US ||
	    task->fire_us <= 0 || !cron_parse(&task->cron, task->cron_text, &error))
	{
		g_debug("skipping loop %s: expired or unreadable", id);
		task_free(task);
		return FALSE;
	}

	task->cron_ok = TRUE;
	*out = task;
	return TRUE;
}

gint
ai_loop_schedule_load(
	AiLoopSchedule *self,
	const gchar    *path,
	gint64          now_us,
	GError        **error
){
	g_autofree gchar     *data = NULL;
	gsize                 length = 0;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject           *root;
	JsonArray            *tasks;
	guint                 i;
	guint                 n;

	g_return_val_if_fail(AI_IS_LOOP_SCHEDULE(self), -1);
	g_return_val_if_fail(path != NULL, -1);

	g_ptr_array_set_size(self->tasks, 0);

	if (!g_file_test(path, G_FILE_TEST_EXISTS))
	{
		return 0;
	}

	if (refuse_link(path, error))
	{
		return -1;
	}

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

	if (!g_utf8_validate(data, (gssize)length, NULL))
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

	if (root == NULL || ai_json_get_int(root, "version", 0) != 1)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Loop file is not version 1.");
		return -1;
	}

	tasks = ai_json_get_array(root, "tasks");

	if (tasks == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "Loop file has no task list.");
		return -1;
	}

	n = json_array_get_length(tasks);

	for (i = 0; i < n && self->tasks->len < LOOP_MAX_TASKS; i++)
	{
		JsonObject *object = ai_json_array_get_object(tasks, i);
		Task       *task = NULL;

		if (object != NULL && load_task(object, now_us, &task))
		{
			if (task_by_id(self, task->id) != NULL)
			{
				task_free(task);
				continue;
			}

			g_ptr_array_add(self->tasks, task);
		}
	}

	return (gint)self->tasks->len;
}
