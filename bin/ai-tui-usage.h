/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <math.h>
#include <ai-glib.h>
#include "core/ai-json-util.h"

/* Frontend-private snapshot/cache. Only the main context touches this state;
 * native work receives a separate client and cannot race model/session edits.
 * The owner must stop, drain pending, then clear before releasing user_data. */
typedef struct {
	GObject *provider;
	gchar *model;
	gchar *cwd;
	JsonNode *data;
	GCancellable *cancel;
	gboolean pending;
	gboolean stopped;
	gboolean failed;
	guint generation;
	guint query_generation;
	gint64 next_refresh;
	void (*changed)(gpointer user_data);
	gpointer user_data;
} AiTuiUsage;

static inline JsonObject *
usage_object(JsonNode *node)
{
	return node != NULL && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

/* Never infer a direction from a provider name or an unlabeled percent.
 * Codex's wire usedPercent is normalized to used_percent by AiCliReport. */
static inline gdouble
usage_remaining(JsonObject *row)
{
	gdouble remaining = ai_json_get_double(row, "remaining_percent", NAN);
	gdouble used = ai_json_get_double(row, "used_percent", NAN);
	gdouble limit;

	if (isfinite(remaining) && remaining >= 0 && remaining <= 100)
		return remaining;
	if (isfinite(used) && used >= 0 && used <= 100)
		return 100 - used;
	limit = ai_json_get_double(row, "limit", NAN);
	if (!isfinite(limit) || limit <= 0) return NAN;
	remaining = ai_json_get_double(row, "remaining", NAN);
	used = ai_json_get_double(row, "used", NAN);
	if (isfinite(remaining) && remaining >= 0 && remaining <= limit)
		return (remaining / limit) * 100;
	if (isfinite(used) && used >= 0 && used <= limit)
		return (1 - used / limit) * 100;
	return NAN;
}

static inline gchar *
usage_bar(JsonObject *row)
{
	gdouble remaining = usage_remaining(row);
	gchar cells[11];
	guint i, filled;

	if (!isfinite(remaining)) return g_strdup("Unavailable");
	filled = (guint)(remaining / 10 + 0.5);
	for (i = 0; i < 10; i++) cells[i] = i < filled ? '#' : '-';
	cells[10] = '\0';
	return g_strdup_printf("[%s] %.0f%% remaining", cells, remaining);
}

static inline void
usage_ready(GObject *source, GAsyncResult *result, gpointer user_data)
{
	AiTuiUsage *usage = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = ai_cli_client_query_report_finish(
		AI_CLI_CLIENT(source), result, &error);

	usage->pending = FALSE;
	g_clear_object(&usage->cancel);
	if (usage->stopped) return;
	if (usage->query_generation == usage->generation)
	{
		usage->failed = report == NULL;
		if (report != NULL)
		{
			g_clear_pointer(&usage->data, json_node_unref);
			usage->data = ai_cli_report_dup_data(report);
		}
		/* Back off failures too; redraws must never turn into a query loop. */
		usage->next_refresh = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
	}
	if (usage->changed != NULL) usage->changed(usage->user_data);
}

/* Called on visible draws and the frontend's existing slow housekeeping tick.
 * Returns TRUE when the displayed state changes. Hidden/dump views never query. */
static inline gboolean
usage_refresh(AiTuiUsage *usage, GObject *provider, gboolean visible)
{
	const gchar *model;
	const gchar *cwd;
	g_autoptr(AiCliClient) client = NULL;
	AiCliClient *active;
	gboolean changed = FALSE;

	if (usage->stopped || !visible) return FALSE;
	model = AI_IS_CLI_CLIENT(provider) ? ai_cli_client_get_model(AI_CLI_CLIENT(provider)) : NULL;
	cwd = AI_IS_CLI_CLIENT(provider) ? ai_cli_client_get_working_directory(AI_CLI_CLIENT(provider)) : NULL;
	if (usage->provider != provider || g_strcmp0(usage->model, model) != 0 ||
		g_strcmp0(usage->cwd, cwd) != 0)
	{
		g_set_object(&usage->provider, provider);
		g_free(usage->model); usage->model = g_strdup(model);
		g_free(usage->cwd); usage->cwd = g_strdup(cwd);
		g_clear_pointer(&usage->data, json_node_unref);
		usage->generation++;
		usage->next_refresh = 0;
		usage->failed = FALSE;
		if (usage->cancel != NULL) g_cancellable_cancel(usage->cancel);
		changed = TRUE;
	}
	if (!AI_IS_CLI_CLIENT(provider))
	{
		usage->failed = TRUE;
		return changed;
	}
	if (usage->pending || g_get_monotonic_time() < usage->next_refresh) return changed;
	active = AI_CLI_CLIENT(provider);
	client = g_object_new(G_OBJECT_TYPE(active), NULL);
	ai_cli_client_set_model(client, model);
	ai_cli_client_set_executable_path(client, ai_cli_client_get_executable_path(active));
	ai_cli_client_set_working_directory(client, cwd);
	ai_cli_client_set_environment(client, ai_cli_client_get_environment(active));
	ai_cli_client_set_process_timeout_ms(client, ai_cli_client_get_process_timeout_ms(active));
	usage->cancel = g_cancellable_new();
	usage->pending = TRUE;
	usage->query_generation = usage->generation;
	ai_cli_client_query_report_async(client, AI_CLI_REPORT_USAGE, 20,
		usage->cancel, usage_ready, usage);
	return TRUE;
}

static inline void
usage_stop(AiTuiUsage *usage)
{
	usage->stopped = TRUE;
	if (usage->cancel != NULL) g_cancellable_cancel(usage->cancel);
}

static inline void
usage_clear(AiTuiUsage *usage)
{
	g_assert(!usage->pending);
	g_clear_object(&usage->cancel);
	g_clear_object(&usage->provider);
	g_clear_pointer(&usage->model, g_free);
	g_clear_pointer(&usage->cwd, g_free);
	g_clear_pointer(&usage->data, json_node_unref);
}
