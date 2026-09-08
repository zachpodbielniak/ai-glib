/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include "core/ai-cli-report-private.h"
#include "core/ai-error.h"
#include "core/ai-enums.h"

struct _AiCliReport {
	GObject parent_instance;
	AiCliReportKind kind;
	JsonNode *data;
};
G_DEFINE_TYPE(AiCliReport, ai_cli_report, G_TYPE_OBJECT)

/* The report owns its entire normalized tree; callers receive deep copies. */
static void
report_finalize(GObject *object)
{
	g_clear_pointer(&AI_CLI_REPORT(object)->data, json_node_unref);
	G_OBJECT_CLASS(ai_cli_report_parent_class)->finalize(object);
}
static void
ai_cli_report_class_init(AiCliReportClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = report_finalize;
}
static void
ai_cli_report_init(AiCliReport *self)
{
	self->data = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(self->data, json_object_new());
}

/* Internal construction centralizes the versioned schema and unknown fields. */
AiCliReport *
_ai_cli_report_new(AiCliClient *client, AiCliReportKind kind, const gchar *source)
{
	AiCliReport *report = g_object_new(AI_TYPE_CLI_REPORT, NULL);
	JsonObject *obj = json_node_get_object(report->data);
	g_autoptr(GDateTime) now = g_date_time_new_now_utc();
	g_autofree gchar *stamp = g_date_time_format_iso8601(now);
	report->kind = kind;
	json_object_set_int_member(obj, "schema_version", 1);
	json_object_set_string_member(obj, "provider", ai_provider_type_to_string(
		ai_provider_get_provider_type(AI_PROVIDER(client))));
	json_object_set_string_member(obj, "kind", kind == AI_CLI_REPORT_USAGE ? "usage" : "history");
	json_object_set_string_member(obj, "source", source);
	json_object_set_string_member(obj, "collected_at", stamp);
	json_object_set_string_member(obj, "availability", "available");
	json_object_set_null_member(obj, "detail");
	json_object_set_null_member(obj, "plan");
	json_object_set_null_member(obj, "source_updated_at");
	json_object_set_boolean_member(obj, "truncated", FALSE);
	json_object_set_array_member(obj, "entries", json_array_new());
	return report;
}
JsonObject *
_ai_cli_report_object(AiCliReport *report)
{
	return json_node_get_object(report->data);
}

/* Entries deliberately retain nulls: an unknown bill is not a zero bill. */
JsonObject *
_ai_cli_report_add_entry(AiCliReport *report, const gchar *label, const gchar *unit)
{
	const gchar *fields[] = { "used", "limit", "remaining", "used_percent", "remaining_percent",
		"percent", "start_at", "end_at", "reset_at", "reset_text", "input_tokens",
		"output_tokens", "total_tokens", "cost_usd", "window_minutes",
		"included_cost_usd", "on_demand_cost_usd", "model", NULL };
	JsonObject *row = json_object_new();
	guint i;
	json_object_set_string_member(row, "label", label);
	json_object_set_string_member(row, "unit", unit);
	for (i = 0; fields[i] != NULL; i++) json_object_set_null_member(row, fields[i]);
	json_array_add_object_element(ai_json_get_array(_ai_cli_report_object(report), "entries"), row);
	return row;
}

/**
 * ai_cli_report_get_provider:
 * @self: a report
 * Returns: (transfer none): the canonical CLI provider name
 */
const gchar *
ai_cli_report_get_provider(AiCliReport *self)
{
	g_return_val_if_fail(AI_IS_CLI_REPORT(self), NULL);
	return ai_json_get_string(_ai_cli_report_object(self), "provider", NULL);
}
/**
 * ai_cli_report_get_kind:
 * @self: a report
 * Returns: the requested report kind
 */
AiCliReportKind
ai_cli_report_get_kind(AiCliReport *self)
{
	g_return_val_if_fail(AI_IS_CLI_REPORT(self), AI_CLI_REPORT_USAGE);
	return self->kind;
}
/**
 * ai_cli_report_dup_data:
 * @self: a report
 *
 * Returns a deep copy of schema version 1. Entries have explicit units and
 * null unknown values. See docs/cli-reporting.org for the field contract.
 *
 * Returns: (transfer full): independent normalized JSON
 */
JsonNode *
ai_cli_report_dup_data(AiCliReport *self)
{
	g_autofree gchar *text = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_return_val_if_fail(AI_IS_CLI_REPORT(self), NULL);
	text = json_to_string(self->data, FALSE);
	json_parser_load_from_data(parser, text, -1, NULL);
	return json_node_copy(json_parser_get_root(parser));
}
/**
 * ai_cli_report_to_json:
 * @self: a report
 * Returns: (transfer full): normalized JSON, without terminal escape codes
 */
gchar *
ai_cli_report_to_json(AiCliReport *self)
{
	g_return_val_if_fail(AI_IS_CLI_REPORT(self), NULL);
	return json_to_string(self->data, TRUE);
}

/* Strip terminal controls without altering JSON's lossless field values. */
static void
append_plain(GString *out, const gchar *value)
{
	g_autofree gchar *valid = g_utf8_make_valid(value != NULL ? value : "", -1);
	const gchar *p;
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p))
	{
		gunichar ch = g_utf8_get_char(p);
		if (!g_unichar_iscntrl(ch)) g_string_append_unichar(out, ch);
	}
}
/**
 * ai_cli_report_to_text:
 * @self: a report
 * Returns: (transfer full): plain-text normalized fields with explicit units
 */
gchar *
ai_cli_report_to_text(AiCliReport *self)
{
	g_autoptr(GString) out = g_string_new(NULL);
	JsonObject *obj;
	JsonArray *rows;
	guint i;
	g_return_val_if_fail(AI_IS_CLI_REPORT(self), NULL);
	obj = _ai_cli_report_object(self);
	g_string_append_printf(out, "%s %s\nSource: %s\nCollected: %s\nAvailability: %s\n",
		ai_cli_report_get_provider(self), ai_json_get_string(obj, "kind", ""),
		ai_json_get_string(obj, "source", ""), ai_json_get_string(obj, "collected_at", ""),
		ai_json_get_string(obj, "availability", ""));
	if (ai_json_get_string(obj, "source_updated_at", NULL) != NULL)
	{
		g_string_append(out, "Source updated: ");
		append_plain(out, ai_json_get_string(obj, "source_updated_at", NULL));
		g_string_append_c(out, '\n');
	}
	if (ai_json_get_string(obj, "plan", NULL) != NULL)
	{
		g_string_append(out, "Plan: "); append_plain(out, ai_json_get_string(obj, "plan", NULL));
		g_string_append_c(out, '\n');
	}
	rows = ai_json_get_array(obj, "entries");
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = ai_json_array_get_object(rows, i);
		GList *keys = json_object_get_members(row), *iter;
		append_plain(out, ai_json_get_string(row, "label", ""));
		g_string_append_printf(out, " [%s]\n", ai_json_get_string(row, "unit", "unknown"));
		for (iter = keys; iter != NULL; iter = iter->next)
		{
			const gchar *key = iter->data;
			JsonNode *node = json_object_get_member(row, key);
			g_autofree gchar *value = NULL;
			if (g_str_equal(key, "label") || g_str_equal(key, "unit") || JSON_NODE_HOLDS_NULL(node)) continue;
			value = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
				? g_strdup(json_node_get_string(node)) : json_to_string(node, FALSE);
			g_string_append_printf(out, "  %s: ", key); append_plain(out, value); g_string_append_c(out, '\n');
		}
		g_list_free(keys);
	}
	if (json_array_get_length(rows) == 0) g_string_append(out, "No records returned.\n");
	if (ai_json_get_string(obj, "detail", NULL) != NULL)
	{
		append_plain(out, ai_json_get_string(obj, "detail", NULL)); g_string_append_c(out, '\n');
	}
	if (ai_json_get_boolean(obj, "truncated", FALSE)) g_string_append(out, "More records exist; increase the limit.\n");
	return g_string_free(g_steal_pointer(&out), FALSE);
}

/**
 * ai_cli_client_query_report:
 * @self: a CLI provider
 * @kind: usage or historical usage
 * @limit: maximum historical periods, from 1 to 1000
 * @cancellable: (nullable): cancellation
 * @error: return location for an error
 *
 * Runs only native reporting interfaces; never sends a model prompt or
 * resumes a conversation. Native credentials stay owned by the CLI.
 * Unsupported sources return AI_ERROR_NOT_SUPPORTED, not fabricated zeros.
 * The query honors executable-path, working-directory and environment.
 *
 * Returns: (transfer full) (nullable): a normalized report
 */
AiCliReport *
ai_cli_client_query_report(AiCliClient *self, AiCliReportKind kind,
	guint limit, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);
	if (g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
	if (!AI_IS_PROVIDER(self))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED, "Reporting requires a CLI provider implementation");
		return NULL;
	}
	if ((kind != AI_CLI_REPORT_USAGE && kind != AI_CLI_REPORT_HISTORY) || limit < 1 || limit > 1000)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST, "Report kind or limit is invalid (limit must be 1..1000)");
		return NULL;
	}
	return _ai_cli_report_query_native(self, kind, limit, cancellable, error);
}

typedef struct { AiCliReportKind kind; guint limit; } ReportQuery;

/* All blocking native work stays off the caller's main context. */
static void
query_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
	ReportQuery *query = task_data;
	g_autoptr(GError) error = NULL;
	AiCliReport *report = ai_cli_client_query_report(source, query->kind, query->limit, cancel, &error);
	if (report != NULL) g_task_return_pointer(task, report, g_object_unref);
	else g_task_return_error(task, g_steal_pointer(&error));
}
/**
 * ai_cli_client_query_report_async:
 * @self: a CLI provider
 * @kind: report kind
 * @limit: maximum historical periods, from 1 to 1000
 * @cancellable: (nullable): cancellation
 * @callback: (scope async) (closure user_data): completion callback
 * @user_data: (nullable): callback data
 *
 * Queries native reporting on a worker; do not mutate the client until the
 * callback returns. Does not start a model turn or resume a conversation.
 */
void
ai_cli_client_query_report_async(AiCliClient *self, AiCliReportKind kind,
	guint limit, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;
	ReportQuery *query;
	g_return_if_fail(AI_IS_CLI_CLIENT(self));
	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, ai_cli_client_query_report_async);
	query = g_new0(ReportQuery, 1); query->kind = kind; query->limit = limit;
	g_task_set_task_data(task, query, g_free);
	g_task_run_in_thread(task, query_worker);
}
/**
 * ai_cli_client_query_report_finish:
 * @self: a CLI provider
 * @result: asynchronous result
 * @error: return location for an error
 * Returns: (transfer full) (nullable): the normalized report
 */
AiCliReport *
ai_cli_client_query_report_finish(AiCliClient *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	g_return_val_if_fail(g_async_result_is_tagged(result, ai_cli_client_query_report_async), NULL);
	return g_task_propagate_pointer(G_TASK(result), error);
}
