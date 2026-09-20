/*
 * ai-quota.h - Account allowances, normalised once for every front-end
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API, for the
 * same reason `ai-json-util.h` and `ai-theme.h` are not: it is `static
 * inline` throughout, so nothing new is exported and nothing new is
 * introspected.
 *
 * `AiCliReport` gives each allowance in whatever direction its provider
 * stated --- Codex says `used_percent`, a Claude panel says remaining,
 * some say a count against a limit. Turning that into one number a person
 * can read is a decision, and it has to be *the same* decision in every
 * front-end: a terminal saying 75% remaining while a window says 25% for
 * the same account is worse than either of them saying nothing.
 *
 * So the direction rule, the bar shape and the accessors live here, and
 * so does the refresh policy. Nothing in this file knows about ncurses or
 * GTK; `bin/ai-tui-usage.h` and `gui/ai-gui-quota.c` are the two views of
 * it.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-quota.h is an internal header"
#endif

#include <math.h>

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "core/ai-cli-client.h"
#include "core/ai-cli-report.h"
#include "core/ai-json-util.h"

G_BEGIN_DECLS

/* How long a snapshot is good for, and how long to wait after a failure
 * before asking again. One number for both: a failing provider must not
 * be retried faster than a working one, or a broken CLI becomes a
 * subprocess every redraw. */
#define AI_QUOTA_REFRESH_US (60 * G_USEC_PER_SEC)

/* Cells in the textual bar. A terminal draws these; a window draws a real
 * progress bar from ai_quota_fraction() instead. */
#define AI_QUOTA_BAR_CELLS (10)

/**
 * AiQuota:
 * @provider: (nullable): the provider the snapshot belongs to
 * @model: (nullable): its model when the snapshot was taken
 * @cwd: (nullable): its working directory when the snapshot was taken
 * @data: (nullable): the last successful report body
 * @cancel: (nullable): the in-flight query's cancellable
 * @pending: a query is in flight
 * @stopped: the owner is shutting down; no new queries, no callbacks
 * @failed: the last attempt failed, so @data is stale
 * @generation: bumped whenever the identity changes
 * @query_generation: the generation the in-flight query belongs to
 * @next_refresh: monotonic time before which no query is started
 * @changed: (nullable): called when the displayed state changed
 * @user_data: passed to @changed
 *
 * A front-end's cached view of one provider's account allowances.
 *
 * Only the thread that owns it touches it. The query runs against a
 * *separate* client built from the active one, so a report can never
 * race a model change or a turn on the conversation's own provider.
 *
 * Teardown is ai_quota_stop(), then drain while @pending, then
 * ai_quota_clear(). Skipping the drain leaves a callback pointing at
 * freed memory.
 */
typedef struct
{
	GObject      *provider;
	gchar        *model;
	gchar        *cwd;
	JsonNode     *data;
	GCancellable *cancel;
	gboolean      pending;
	gboolean      stopped;
	gboolean      failed;
	guint         generation;
	guint         query_generation;
	gint64        next_refresh;
	void        (*changed)(gpointer user_data);
	gpointer      user_data;
} AiQuota;

/**
 * ai_quota_object:
 * @node: (nullable): a report body
 *
 * Returns: (nullable): @node as an object, or %NULL if it is anything else
 */
static inline JsonObject *
ai_quota_object(JsonNode *node)
{
	return node != NULL && JSON_NODE_HOLDS_OBJECT(node)
		? json_node_get_object(node) : NULL;
}

/**
 * ai_quota_entries:
 * @self: a cache
 *
 * Returns: (nullable): the allowance rows, or %NULL when there is no
 *   usable snapshot
 */
static inline JsonArray *
ai_quota_entries(AiQuota *self)
{
	return ai_json_get_array(ai_quota_object(self->data), "entries");
}

/**
 * ai_quota_is_partial:
 * @self: a cache
 *
 * Returns: %TRUE when the report came from a panel-text adapter rather
 *   than a stated protocol, so some rows may be missing
 */
static inline gboolean
ai_quota_is_partial(AiQuota *self)
{
	return g_strcmp0(ai_json_get_string(ai_quota_object(self->data),
	                                    "availability", NULL), "partial") == 0;
}

/**
 * ai_quota_remaining:
 * @row: one allowance
 *
 * The percentage of @row still available.
 *
 * Never infers a direction from a provider name or from an unlabeled
 * `percent`: a number that might mean either is worth less than no
 * number, because a person acts on it. Codex's wire `usedPercent` has
 * already been normalised to `used_percent` by #AiCliReport, so the
 * subtraction here is the only place the conversion happens.
 *
 * Returns: 0..100, or NAN when the row does not say
 */
static inline gdouble
ai_quota_remaining(JsonObject *row)
{
	gdouble remaining = ai_json_get_double(row, "remaining_percent", NAN);
	gdouble used = ai_json_get_double(row, "used_percent", NAN);
	gdouble limit;

	if (isfinite(remaining) && remaining >= 0 && remaining <= 100)
		return remaining;

	if (isfinite(used) && used >= 0 && used <= 100)
		return 100 - used;

	limit = ai_json_get_double(row, "limit", NAN);

	if (!isfinite(limit) || limit <= 0)
		return NAN;

	remaining = ai_json_get_double(row, "remaining", NAN);
	used = ai_json_get_double(row, "used", NAN);

	if (isfinite(remaining) && remaining >= 0 && remaining <= limit)
		return (remaining / limit) * 100;

	if (isfinite(used) && used >= 0 && used <= limit)
		return (1 - used / limit) * 100;

	return NAN;
}

/**
 * ai_quota_fraction:
 * @row: one allowance
 *
 * Returns: the same answer as ai_quota_remaining() as 0..1, or a negative
 *   number when the row does not say. What a #GtkLevelBar wants.
 */
static inline gdouble
ai_quota_fraction(JsonObject *row)
{
	gdouble remaining = ai_quota_remaining(row);

	return isfinite(remaining) ? remaining / 100.0 : -1.0;
}

/**
 * ai_quota_format_percent:
 * @remaining: a percentage, or NAN
 *
 * Rounds for display without rounding an allowance away.
 *
 * `%.0f` turns 0.4% remaining into "0%", which reads as exhausted when it
 * is not --- the same invented zero the report layer refuses to produce
 * from missing data. Anything above zero but below one percent is
 * therefore "<1%".
 *
 * The same at the top, for the opposite reason: glibc prints 99.6 as
 * "100", and a person told they have 100% left of something they have
 * already started spending will not believe the next number either.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL for NAN
 */
static inline gchar *
ai_quota_format_percent(gdouble remaining)
{
	if (!isfinite(remaining))
		return NULL;

	if (remaining > 0 && remaining < 1)
		return g_strdup("<1%");

	if (remaining < 100 && remaining >= 99.5)
		return g_strdup(">99%");

	return g_strdup_printf("%.0f%%", remaining);
}

/**
 * ai_quota_format_bar:
 * @row: one allowance
 *
 * The textual bar a terminal draws.
 *
 * Returns: (transfer full): e.g. `[########--] 75% remaining`, or
 *   `Unavailable`
 */
static inline gchar *
ai_quota_format_bar(JsonObject *row)
{
	gdouble remaining = ai_quota_remaining(row);
	g_autofree gchar *percent = ai_quota_format_percent(remaining);
	gchar cells[AI_QUOTA_BAR_CELLS + 1];
	guint i;
	guint filled;

	if (percent == NULL)
		return g_strdup("Unavailable");

	filled = (guint)(remaining / (100.0 / AI_QUOTA_BAR_CELLS) + 0.5);

	for (i = 0; i < AI_QUOTA_BAR_CELLS; i++)
		cells[i] = i < filled ? '#' : '-';

	cells[AI_QUOTA_BAR_CELLS] = '\0';

	return g_strdup_printf("[%s] %s remaining", cells, percent);
}

/**
 * ai_quota_row_label:
 * @row: one allowance
 *
 * Returns: (transfer none): the provider's own name for it
 */
static inline const gchar *
ai_quota_row_label(JsonObject *row)
{
	return ai_json_get_string(row, "label", "Allowance");
}

/**
 * ai_quota_row_reset:
 * @row: one allowance
 *
 * Prefers the provider's own words over a timestamp we would have to
 * phrase ourselves.
 *
 * Returns: (transfer none) (nullable): when it refills
 */
static inline const gchar *
ai_quota_row_reset(JsonObject *row)
{
	const gchar *reset = ai_json_get_string(row, "reset_text", NULL);

	return reset != NULL ? reset : ai_json_get_string(row, "reset_at", NULL);
}

/**
 * ai_quota_lowest:
 * @self: a cache
 *
 * The allowance closest to running out.
 *
 * A provider that reports a five-hour window and a weekly one will
 * usually have plenty of the second and none of the first, and the one
 * about to stop the conversation is the one worth a single number. Rows
 * whose direction is unknown are skipped rather than counted as zero.
 *
 * Returns: 0..100, or NAN when nothing in the snapshot says
 */
static inline gdouble
ai_quota_lowest(AiQuota *self)
{
	JsonArray *entries = ai_quota_entries(self);
	gdouble lowest = NAN;
	guint n;
	guint i;

	if (entries == NULL)
		return NAN;

	n = json_array_get_length(entries);

	for (i = 0; i < n; i++)
	{
		JsonObject *row = ai_quota_object(json_array_get_element(entries, i));
		gdouble remaining = ai_quota_remaining(row);

		if (!isfinite(remaining))
			continue;

		if (!isfinite(lowest) || remaining < lowest)
			lowest = remaining;
	}

	return lowest;
}

/**
 * ai_quota_heading:
 * @self: a cache
 *
 * Returns: (transfer none): what to call the section, naming a stale,
 *   refreshing or partial snapshot rather than presenting it as current
 */
static inline const gchar *
ai_quota_heading(AiQuota *self)
{
	if (self->data == NULL)
		return "ACCOUNT REMAINING";

	if (self->failed)
		return "REMAINING (stale)";

	if (self->pending)
		return "REMAINING (refreshing)";

	return ai_quota_is_partial(self) ? "REMAINING (partial)"
	                                 : "ACCOUNT REMAINING";
}

/* ================================================================
 * The refresh policy
 * ================================================================ */

static inline void
ai_quota_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiQuota *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = ai_cli_client_query_report_finish(
		AI_CLI_CLIENT(source), result, &error);

	/*
	 * Cleared before the stopped check, so a teardown drain that waits on
	 * @pending always terminates --- including when the cancel it just
	 * fired is what completed this query.
	 */
	self->pending = FALSE;
	g_clear_object(&self->cancel);

	if (self->stopped)
		return;

	if (self->query_generation == self->generation)
	{
		self->failed = report == NULL;

		if (report != NULL)
		{
			g_clear_pointer(&self->data, json_node_unref);
			self->data = ai_cli_report_dup_data(report);
		}

		/* Back off failures too: a redraw must never become a query
		 * loop against a provider that is answering with an error. */
		self->next_refresh = g_get_monotonic_time() + AI_QUOTA_REFRESH_US;
	}

	if (self->changed != NULL)
		self->changed(self->user_data);
}

/**
 * ai_quota_refresh:
 * @self: a cache
 * @provider: (nullable): the conversation's current provider
 * @visible: whether the person can see the result
 *
 * Starts a query when one is due.
 *
 * @visible is the whole throttle: a hidden panel, a dump run or a window
 * nobody is looking at must not spawn a CLI every minute. Identity is
 * provider *and* model *and* working directory, because a report is
 * scoped to all three and showing the previous one under a new heading
 * would be attributing somebody else's allowance to this session.
 *
 * Returns: %TRUE when what is displayed changed
 */
static inline gboolean
ai_quota_refresh(
	AiQuota *self,
	GObject *provider,
	gboolean visible
){
	const gchar *model;
	const gchar *cwd;
	g_autoptr(AiCliClient) client = NULL;
	AiCliClient *active;
	gboolean changed = FALSE;

	if (self->stopped || !visible)
		return FALSE;

	model = AI_IS_CLI_CLIENT(provider)
		? ai_cli_client_get_model(AI_CLI_CLIENT(provider)) : NULL;
	cwd = AI_IS_CLI_CLIENT(provider)
		? ai_cli_client_get_working_directory(AI_CLI_CLIENT(provider)) : NULL;

	if (self->provider != provider || g_strcmp0(self->model, model) != 0 ||
	    g_strcmp0(self->cwd, cwd) != 0)
	{
		g_set_object(&self->provider, provider);
		g_free(self->model);
		self->model = g_strdup(model);
		g_free(self->cwd);
		self->cwd = g_strdup(cwd);
		g_clear_pointer(&self->data, json_node_unref);
		self->generation++;
		self->next_refresh = 0;
		self->failed = FALSE;

		/* The in-flight answer belongs to the old identity. Cancelling
		 * is belt-and-braces; the generation check discards it anyway. */
		if (self->cancel != NULL)
			g_cancellable_cancel(self->cancel);

		changed = TRUE;
	}

	if (!AI_IS_CLI_CLIENT(provider))
	{
		/* An HTTP provider has no account report to read. Saying so is
		 * the answer, not an empty bar. */
		self->failed = TRUE;
		return changed;
	}

	if (self->pending || g_get_monotonic_time() < self->next_refresh)
		return changed;

	/*
	 * A separate client, built from the active one.
	 *
	 * Querying through the conversation's own provider would put a
	 * subprocess on the object a turn may be using, and a report is
	 * read-only work that has no business sharing that lifetime.
	 */
	active = AI_CLI_CLIENT(provider);
	client = g_object_new(G_OBJECT_TYPE(active), NULL);
	ai_cli_client_set_model(client, model);
	ai_cli_client_set_executable_path(client,
		ai_cli_client_get_executable_path(active));
	ai_cli_client_set_working_directory(client, cwd);
	ai_cli_client_set_environment(client,
		ai_cli_client_get_environment(active));
	ai_cli_client_set_process_timeout_ms(client,
		ai_cli_client_get_process_timeout_ms(active));

	self->cancel = g_cancellable_new();
	self->pending = TRUE;
	self->query_generation = self->generation;

	/* The GTask holds its own reference to @client, so the autoptr
	 * releasing it here does not end the query. */
	ai_cli_client_query_report_async(client, AI_CLI_REPORT_USAGE, 20,
	                                 self->cancel, ai_quota_ready, self);

	return TRUE;
}

/**
 * ai_quota_stop:
 * @self: a cache
 *
 * Refuses further queries and cancels the one in flight. Callbacks stop
 * touching the owner from here on; drain @pending before clearing.
 */
static inline void
ai_quota_stop(AiQuota *self)
{
	self->stopped = TRUE;

	if (self->cancel != NULL)
		g_cancellable_cancel(self->cancel);
}

/**
 * ai_quota_drain:
 * @self: a cache
 *
 * Iterates the thread-default context until no query is in flight.
 *
 * Bounded by the query's own process timeout and by the cancel
 * ai_quota_stop() fired, so this is a short wait rather than an open
 * one. Calling it before ai_quota_clear() is what stops a late callback
 * from writing into freed memory.
 */
static inline void
ai_quota_drain(AiQuota *self)
{
	while (self->pending)
		g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
}

/**
 * ai_quota_clear:
 * @self: a cache
 *
 * Releases everything. ai_quota_stop() and ai_quota_drain() first.
 */
static inline void
ai_quota_clear(AiQuota *self)
{
	g_assert(!self->pending);

	g_clear_object(&self->cancel);
	g_clear_object(&self->provider);
	g_clear_pointer(&self->model, g_free);
	g_clear_pointer(&self->cwd, g_free);
	g_clear_pointer(&self->data, json_node_unref);
}

G_END_DECLS
