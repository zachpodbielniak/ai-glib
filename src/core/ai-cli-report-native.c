/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "config.h"
#include <math.h>
#include <string.h>
#include <glib/gstdio.h>
#include "core/ai-cli-report-private.h"
#include "core/ai-subprocess-util.h"
#include "core/ai-error.h"
#include "core/ai-enums.h"

#define REPORT_BYTES (4 * 1024 * 1024)
#define REPORT_LINE_BYTES (1024 * 1024)
#define REPORT_TIMEOUT_MS (30000)

/* A short-lived read-only JSON-RPC connection. No turn/start or prompt method
 * exists on this path. The CLI owns authentication and token refresh. */
typedef struct {
	GSubprocess *process;
	GString *pending;
	GCancellable *cancel;
	gint64 deadline;
	gsize received;
	guint id;
} ReportRpc;

static gint
report_timeout(AiCliClient *client)
{
	gint configured = ai_cli_client_get_process_timeout_ms(client);
	return configured > 0 ? MIN(configured, REPORT_TIMEOUT_MS) : REPORT_TIMEOUT_MS;
}

/* Reads one bounded frame without blocking beyond cancellation or deadline. */
static gchar *
rpc_line(ReportRpc *rpc, GError **error)
{
	GInputStream *stream = g_subprocess_get_stdout_pipe(rpc->process);
	while (TRUE)
	{
		gchar *newline = strchr(rpc->pending->str, '\n');
		gchar buffer[4096];
		gssize size;
		g_autoptr(GError) read_error = NULL;
		if (g_cancellable_set_error_if_cancelled(rpc->cancel, error)) return NULL;
		if (g_get_monotonic_time() >= rpc->deadline)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_TIMEOUT, "Native usage/history query timed out");
			return NULL;
		}
		if ((newline != NULL ? (gsize)(newline - rpc->pending->str) : rpc->pending->len) > REPORT_LINE_BYTES || rpc->received > REPORT_BYTES)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Native report exceeds size limit");
			return NULL;
		}
		if (newline != NULL)
		{
			gsize len = (gsize)(newline - rpc->pending->str);
			gchar *line = g_strndup(rpc->pending->str, len);
			g_string_erase(rpc->pending, 0, len + 1);
			return line;
		}
		if (!G_IS_POLLABLE_INPUT_STREAM(stream))
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED, "Reporting requires pollable subprocess pipes");
			return NULL;
		}
		size = g_pollable_input_stream_read_nonblocking(G_POLLABLE_INPUT_STREAM(stream),
			buffer, sizeof buffer, rpc->cancel, &read_error);
		if (size > 0)
		{
			if (memchr(buffer, '\0', (gsize)size) != NULL)
			{
				g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Native report contains a NUL byte");
				return NULL;
			}
			g_string_append_len(rpc->pending, buffer, size); rpc->received += (gsize)size;
		}
		else if (size == 0)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "Native reporting process closed before replying");
			return NULL;
		}
		else if (g_error_matches(read_error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) g_usleep(5000);
		else { g_propagate_error(error, g_steal_pointer(&read_error)); return NULL; }
	}
}

/* Send JSON only through stdin; account data and credentials never enter argv. */
static gboolean
rpc_send(ReportRpc *rpc, const gchar *wire, GError **error)
{
	return g_output_stream_write_all(g_subprocess_get_stdin_pipe(rpc->process),
		wire, strlen(wire), NULL, rpc->cancel, error);
}

static JsonNode *
rpc_call(ReportRpc *rpc, const gchar *method, const gchar *params, GError **error)
{
	g_autofree gchar *wire = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"%s\",\"params\":%s}\n",
		++rpc->id, method, params != NULL ? params : "{}");
	if (!rpc_send(rpc, wire, error)) return NULL;
	while (TRUE)
	{
		g_autofree gchar *line = rpc_line(rpc, error);
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *obj;
		JsonNode *result;
		if (line == NULL) return NULL;
		if (!json_parser_load_from_data(parser, line, -1, NULL) ||
		    (obj = ai_json_root_object(parser)) == NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Invalid native reporting JSON-RPC frame");
			return NULL;
		}
		if (ai_json_get_string(obj, "method", NULL) != NULL) continue;
		if (ai_json_get_int(obj, "id", -1) != rpc->id) continue;
		if (ai_json_get_object(obj, "error") != NULL)
		{
			JsonObject *failure = ai_json_get_object(obj, "error");
			gint code = (gint)ai_json_get_int(failure, "code", 0);
			/* Do not echo arbitrary backend payloads or auth challenge material. */
			g_set_error(error, AI_ERROR, code == -32601 ? AI_ERROR_NOT_SUPPORTED : AI_ERROR_CLI_EXECUTION,
				"Native report method %s failed (RPC code %d); check native CLI authentication and version", method, code);
			return NULL;
		}
		result = ai_json_get_node(obj, "result");
		if (result == NULL || !JSON_NODE_HOLDS_OBJECT(result))
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Native report result must be an object");
			return NULL;
		}
		return json_node_copy(result);
	}
}

/* Preserve known finite numbers exactly; wrong-typed and absent fields stay null. */
static gboolean
copy_number(JsonObject *dst, const gchar *key, JsonObject *src, const gchar *field)
{
	JsonNode *node = ai_json_get_node(src, field);
	GType type = node != NULL && JSON_NODE_HOLDS_VALUE(node) ? json_node_get_value_type(node) : G_TYPE_INVALID;
	if ((type == G_TYPE_INT64 || type == G_TYPE_DOUBLE) && isfinite(json_node_get_double(node)) &&
	    json_node_get_double(node) >= 0)
	{
		json_object_set_member(dst, key, json_node_copy(node)); return TRUE;
	}
	return FALSE;
}
static void
copy_string(JsonObject *dst, const gchar *key, JsonObject *src, const gchar *field)
{
	const gchar *text = ai_json_get_string(src, field, NULL);
	if (text != NULL) json_object_set_string_member(dst, key, text);
}
static void
copy_unix_time(JsonObject *dst, const gchar *key, JsonObject *src, const gchar *field)
{
	gint64 epoch = ai_json_get_int(src, field, -1);
	g_autoptr(GDateTime) date = epoch >= 0 ? g_date_time_new_from_unix_utc(epoch) : NULL;
	if (date != NULL)
	{
		g_autofree gchar *text = g_date_time_format_iso8601(date);
		json_object_set_string_member(dst, key, text);
	}
}

/* Native sources disagree on ordering. Sort ISO dates (or billing months)
 * newest first before limiting, without mutating the provider's JSON. */
static gint
compare_periods(gconstpointer a, gconstpointer b, gpointer field)
{
	JsonObject *left = *(JsonObject *const *)a;
	JsonObject *right = *(JsonObject *const *)b;
	if (field != NULL)
		return g_strcmp0(ai_json_get_string(right, field, ""), ai_json_get_string(left, field, ""));
	left = ai_json_get_object(left, "billingCycle");
	right = ai_json_get_object(right, "billingCycle");
	{
		gint64 ly = ai_json_get_int(left, "year", 0), ry = ai_json_get_int(right, "year", 0);
		gint64 lm = ai_json_get_int(left, "month", 0), rm = ai_json_get_int(right, "month", 0);
		return ly != ry ? (ly < ry ? 1 : -1) : lm != rm ? (lm < rm ? 1 : -1) : 0;
	}
}
static GPtrArray *
sorted_periods(JsonArray *array, const gchar *field)
{
	GPtrArray *sorted = g_ptr_array_new();
	guint i;
	for (i = 0; i < json_array_get_length(array); i++)
	{
		JsonObject *obj = ai_json_array_get_object(array, i);
		if (obj != NULL) g_ptr_array_add(sorted, obj);
	}
	g_ptr_array_sort_with_data(sorted, compare_periods, (gpointer)field);
	return sorted;
}

/* OpenAI returns distinct primary/secondary windows for each metered pool. */
static void
codex_windows(AiCliReport *report, JsonObject *pool, const gchar *id)
{
	const gchar *names[] = { "primary", "secondary", NULL };
	guint i;
	copy_string(_ai_cli_report_object(report), "plan", pool, "planType");
	for (i = 0; names[i] != NULL; i++)
	{
		JsonObject *window = ai_json_get_object(pool, names[i]);
		g_autofree gchar *label = g_strdup_printf("%s/%s", id, names[i]);
		JsonObject *row;
		if (window == NULL) continue;
		row = _ai_cli_report_add_entry(report, label, "percent");
		copy_number(row, "used_percent", window, "usedPercent");
		copy_number(row, "window_minutes", window, "windowDurationMins");
		copy_unix_time(row, "reset_at", window, "resetsAt");
	}
}

static AiCliReport *
normalize_codex(AiCliClient *client, AiCliReportKind kind, JsonObject *data, guint limit, GError **error)
{
	g_autoptr(AiCliReport) report = _ai_cli_report_new(client, kind,
		kind == AI_CLI_REPORT_USAGE ? "codex account/rateLimits/read" : "codex account/usage/read");
	if (kind == AI_CLI_REPORT_USAGE)
	{
		JsonObject *pools = ai_json_get_object(data, "rateLimitsByLimitId");
		if (pools != NULL)
		{
			GList *keys = json_object_get_members(pools), *iter;
			for (iter = keys; iter != NULL; iter = iter->next)
				codex_windows(report, ai_json_get_object(pools, iter->data), iter->data);
			g_list_free(keys);
		}
		else if (ai_json_get_object(data, "rateLimits") != NULL)
			codex_windows(report, ai_json_get_object(data, "rateLimits"), "codex");
		else goto unavailable;
	}
	else
	{
		JsonArray *days = ai_json_get_array(data, "dailyUsageBuckets");
		g_autoptr(GPtrArray) sorted = NULL;
		guint i, count;
		if (days == NULL) goto unavailable;
		sorted = sorted_periods(days, "startDate");
		count = sorted->len;
		for (i = 0; i < count && i < limit; i++)
		{
			JsonObject *day = g_ptr_array_index(sorted, i);
			const gchar *date = ai_json_get_string(day, "startDate", NULL);
			JsonObject *row;
			if (date == NULL) continue;
			row = _ai_cli_report_add_entry(report, date, "tokens");
			copy_number(row, "total_tokens", day, "tokens");
			copy_string(row, "start_at", day, "startDate");
		}
		json_object_set_boolean_member(_ai_cli_report_object(report), "truncated", count > limit);
	}
	return g_steal_pointer(&report);
unavailable:
	g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED, "Codex account does not expose this report for its authentication mode or CLI version");
	return NULL;
}

/* Proto JSON represents a known zero Cent as {}; absent Cent is unknown. */
static gboolean
copy_cents(JsonObject *row, const gchar *key, JsonObject *data, const gchar *field)
{
	JsonObject *cent = ai_json_get_object(data, field);
	JsonNode *value;
	gdouble amount;
	if (cent == NULL) return FALSE;
	value = ai_json_get_node(cent, "val");
	if (value == NULL) amount = 0;
	else if (JSON_NODE_HOLDS_VALUE(value) && (json_node_get_value_type(value) == G_TYPE_INT64 ||
	         json_node_get_value_type(value) == G_TYPE_DOUBLE)) amount = json_node_get_double(value);
	else if (JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING)
	{
		gchar *end = NULL;
		const gchar *str = json_node_get_string(value);
		amount = g_ascii_strtod(str, &end);
		if (end == str || *end != '\0') return FALSE;
	}
	else return FALSE;
	if (!isfinite(amount) || amount < 0) return FALSE;
	json_object_set_double_member(row, key, amount / 100.0);
	return TRUE;
}

static AiCliReport *
normalize_grok(AiCliClient *client, AiCliReportKind kind, JsonObject *data, guint limit, GError **error)
{
	g_autoptr(AiCliReport) report = _ai_cli_report_new(client, kind, "grok ACP _x.ai/billing");
	JsonObject *config = ai_json_get_object(data, "config");
	if (config == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED, "Grok account has no billing configuration");
		return NULL;
	}
	copy_string(_ai_cli_report_object(report), "plan", data, "subscription_tier");
	if (kind == AI_CLI_REPORT_USAGE)
	{
		JsonObject *period = ai_json_get_object(config, "currentPeriod");
		JsonObject *row = _ai_cli_report_add_entry(report,
			ai_json_get_string(period, "type", "included allowance"),
			ai_json_get_object(config, "monthlyLimit") != NULL || ai_json_get_object(config, "used") != NULL ? "USD" : "percent");
		copy_number(row, "used_percent", config, "creditUsagePercent");
		copy_cents(row, "limit", config, "monthlyLimit");
		copy_cents(row, "used", config, "used");
		copy_string(row, "start_at", config, "billingPeriodStart");
		copy_string(row, "end_at", config, "billingPeriodEnd");
		copy_string(row, "start_at", period, "start");
		copy_string(row, "end_at", period, "end");
		copy_string(row, "reset_at", row, "end_at");
		if (ai_json_get_object(config, "onDemandUsed") != NULL || ai_json_get_object(config, "onDemandCap") != NULL)
		{
			row = _ai_cli_report_add_entry(report, "on-demand", "USD");
			copy_cents(row, "used", config, "onDemandUsed");
			copy_cents(row, "limit", config, "onDemandCap");
		}
		if (ai_json_get_object(config, "prepaidBalance") != NULL)
		{
			row = _ai_cli_report_add_entry(report, "prepaid balance", "USD");
			copy_cents(row, "remaining", config, "prepaidBalance");
		}
	}
	else
	{
		JsonArray *history = ai_json_get_array(config, "history");
		g_autoptr(GPtrArray) sorted = NULL;
		guint i, count;
		if (history == NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED, "Grok did not return historical billing periods");
			return NULL;
		}
		sorted = sorted_periods(history, NULL);
		count = sorted->len;
		for (i = 0; i < count && i < limit; i++)
		{
			JsonObject *old = g_ptr_array_index(sorted, i);
			JsonObject *cycle = ai_json_get_object(old, "billingCycle");
			gint64 year = ai_json_get_int(cycle, "year", 0), month = ai_json_get_int(cycle, "month", 0);
			g_autofree gchar *label = NULL;
			JsonObject *row;
			if (year < 1 || year > 9999 || month < 1 || month > 12) continue;
			label = g_strdup_printf("%04" G_GINT64_FORMAT "-%02" G_GINT64_FORMAT, year, month);
			row = _ai_cli_report_add_entry(report, label, "USD");
			copy_cents(row, "cost_usd", old, "totalUsed");
			copy_cents(row, "included_cost_usd", old, "includedUsed");
			copy_cents(row, "on_demand_cost_usd", old, "onDemandUsed");
		}
		json_object_set_boolean_member(_ai_cli_report_object(report), "truncated", count > limit);
	}
	return g_steal_pointer(&report);
}

/* Initialize the native transport, issue exactly one read-only report method,
 * then terminate our private process. This never attaches to a live turn. */
static AiCliReport *
query_rpc(AiCliClient *client, AiCliReportKind kind, guint limit, gboolean grok,
	GCancellable *cancel, GError **error)
{
	g_autofree gchar *executable = ai_cli_client_resolve_executable(client, error);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GString) pending = g_string_new(NULL);
	g_autoptr(JsonNode) initialized = NULL;
	g_autoptr(JsonNode) result = NULL;
	ReportRpc rpc = { 0 };
	AiCliReport *report = NULL;
	const gchar *codex_args[] = { executable, "app-server", "--listen", "stdio://", NULL };
	const gchar *grok_args[] = { executable, "agent", "--no-leader", "stdio", NULL };
	if (executable == NULL) return NULL;
	process = ai_cli_client_spawn(client, grok ? grok_args : codex_args,
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
	if (process == NULL) return NULL;
	rpc.process = process; rpc.pending = pending; rpc.cancel = cancel;
	rpc.deadline = g_get_monotonic_time() + (gint64)report_timeout(client) * 1000;
	initialized = rpc_call(&rpc, "initialize", grok
		? "{\"protocolVersion\":1,\"clientCapabilities\":{},\"clientInfo\":{\"name\":\"ai-glib\",\"version\":\"" AI_GLIB_PACKAGE_VERSION "\"}}"
		: "{\"clientInfo\":{\"name\":\"ai-glib\",\"version\":\"" AI_GLIB_PACKAGE_VERSION "\"}}", error);
	if (initialized == NULL) goto done;
	if (!grok && !rpc_send(&rpc, "{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}\n", error)) goto done;
	result = rpc_call(&rpc, grok ? "_x.ai/billing" : kind == AI_CLI_REPORT_USAGE
		? "account/rateLimits/read" : "account/usage/read", "{}", error);
	if (result != NULL)
		report = grok ? normalize_grok(client, kind, json_node_get_object(result), limit, error)
		             : normalize_codex(client, kind, json_node_get_object(result), limit, error);
done:
	g_subprocess_force_exit(process);
	return report;
}

/* Execute tmux control commands with the same environment/cwd as the provider.
 * Captures are fixed-size panes; stderr is never mixed into report JSON. */
static gchar *
panel_command(AiCliClient *client, const gchar *tmux, const gchar *socket,
	const gchar *const *args, GCancellable *cancel, gint timeout, GError **error)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GSubprocess) process = NULL;
	gchar *output = NULL;
	guint i;
	g_ptr_array_add(argv, (gpointer)tmux); g_ptr_array_add(argv, (gpointer)"-S");
	g_ptr_array_add(argv, (gpointer)socket); g_ptr_array_add(argv, (gpointer)"-f");
	g_ptr_array_add(argv, (gpointer)"/dev/null");
	for (i = 0; args[i] != NULL; i++) g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	process = ai_cli_client_spawn(client, (const gchar *const *)argv->pdata,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
	if (process == NULL) return NULL;
	if (!ai_subprocess_communicate_utf8_bounded(process, NULL, timeout, cancel, &output, NULL, error))
	{
		g_free(output); return NULL;
	}
	if (!g_subprocess_get_successful(process))
	{
		g_free(output);
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "Native usage panel exited or tmux failed");
		return NULL;
	}
	return output;
}

/* Panels lack a stable wire schema. Keep percentage direction and reset text
 * explicit, and mark these reports partial rather than inventing timestamps. */
static AiCliReport *
normalize_panel(AiCliClient *client, const gchar *text)
{
	g_autoptr(AiCliReport) report = _ai_cli_report_new(client, AI_CLI_REPORT_USAGE, "native /usage panel");
	g_autoptr(GRegex) percent = g_regex_new("([0-9]+(?:\\.[0-9]+)?)%[[:space:]]*(used|remaining|left)?", G_REGEX_CASELESS, 0, NULL);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autofree gchar *label = g_strdup("quota");
	JsonObject *last = NULL;
	guint i, count = 0;
	for (i = 0; lines[i] != NULL; i++)
	{
		g_autofree gchar *lower = g_utf8_strdown(lines[i], -1);
		g_autoptr(GMatchInfo) match = NULL;
		gchar *line = g_strstrip(lines[i]);
		/* Native tables can indent labels inside Unicode box-drawing borders. */
		while (*line != '\0')
		{
			gunichar ch = g_utf8_get_char(line);
			if (g_unichar_isspace(ch) || ch == '|' || (ch >= 0x2500 && ch <= 0x257f))
				line = g_utf8_next_char(line);
			else break;
		}
		if (g_regex_match(percent, line, 0, &match))
		{
			g_autofree gchar *value = g_match_info_fetch(match, 1);
			g_autofree gchar *direction = g_match_info_fetch(match, 2);
			gint start;
			gdouble number = g_ascii_strtod(value, NULL);
			g_match_info_fetch_pos(match, 0, &start, NULL);
			/* A table row can carry its own model label before the number. */
			if (start > 0 && g_ascii_isalnum(line[0]))
			{
				g_free(label); label = g_strndup(line, (gsize)start); g_strstrip(label);
			}
			last = _ai_cli_report_add_entry(report, label, "percent"); count++;
			json_object_set_double_member(last, "percent", number);
			if (g_ascii_strcasecmp(direction, "used") == 0)
				json_object_set_double_member(last, "used_percent", number);
			else if (g_ascii_strcasecmp(direction, "remaining") == 0 || g_ascii_strcasecmp(direction, "left") == 0)
				json_object_set_double_member(last, "remaining_percent", number);
		}
		else if (last != NULL && (strstr(lower, "reset") != NULL || strstr(lower, "refresh") != NULL))
			json_object_set_string_member(last, "reset_text", line);
		else if (*line != '\0' && g_unichar_isalnum(g_utf8_get_char(line)))
		{
			g_free(label); label = g_strdup(line);
		}
	}
	if (count == 0) return NULL;
	json_object_set_string_member(_ai_cli_report_object(report), "availability", "partial");
	json_object_set_string_member(_ai_cli_report_object(report), "detail",
		"Visible native panel only; unlabeled percentages have unknown direction and reset text is not converted to timestamps.");
	return g_steal_pointer(&report);
}

/* Native slash commands are dispatched by the CLI, never submitted through its
 * model print API. Trust/auth prompts remain a user decision and fail closed. */
static AiCliReport *
query_panel(AiCliClient *client, GCancellable *cancel, GError **error)
{
	g_autofree gchar *tmux = g_find_program_in_path("tmux");
	g_autofree gchar *executable = NULL;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *socket = NULL;
	g_autofree gchar *quoted = NULL;
	g_autofree gchar *command = NULL;
	g_autofree gchar *previous = NULL;
	g_autofree gchar *output = NULL;
	AiCliReport *report = NULL;
	AiProviderType type = ai_provider_get_provider_type(AI_PROVIDER(client));
	gint64 deadline = g_get_monotonic_time() + (gint64)report_timeout(client) * 1000;
	const gchar *capture[] = { "capture-pane", "-p", "-t", "report", NULL };
	const gchar *stop[] = { "kill-server", NULL };
	const gchar *start[] = { "new-session", "-d", "-s", "report", "-x", "180", "-y", "100", NULL, NULL };
	if (tmux == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND, "tmux is required to read this provider's /usage panel"); return NULL;
	}
	executable = ai_cli_client_resolve_executable(client, error);
	if (executable == NULL) return NULL;
	dir = g_dir_make_tmp("ai-glib-report-XXXXXX", error);
	if (dir == NULL) return NULL;
	socket = g_build_filename(dir, "tmux.sock", NULL);
	quoted = g_shell_quote(executable);
	command = g_strdup_printf("exec %s %s/usage", quoted,
		type == AI_PROVIDER_ANTIGRAVITY ? "--prompt-interactive " : "");
	start[8] = command;
	output = panel_command(client, tmux, socket, start, cancel, MIN(report_timeout(client), 2000), error);
	if (output == NULL) goto done;
	while (g_get_monotonic_time() < deadline)
	{
		g_autofree gchar *lower = NULL;
		if (g_cancellable_set_error_if_cancelled(cancel, error)) goto done;
		g_clear_pointer(&output, g_free);
		output = panel_command(client, tmux, socket, capture, cancel,
			(gint)MAX(1, MIN(2000, (deadline - g_get_monotonic_time()) / 1000)), error);
		if (output == NULL) goto done;
		lower = g_utf8_strdown(output, -1);
		if (strstr(lower, "trust this folder") != NULL || strstr(lower, "trust the contents") != NULL)
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_PERMISSION_DENIED,
				"Native CLI requires workspace trust; open it interactively in this directory first"); goto done;
		}
		if (previous != NULL && g_str_equal(previous, output) &&
		    strstr(lower, "loading") == NULL && strstr(lower, "refreshing") == NULL &&
		    (strstr(lower, "usage") != NULL || strstr(lower, "quota") != NULL))
		{
			report = normalize_panel(client, output);
			if (report != NULL) goto done;
		}
		g_free(previous); previous = g_strdup(output);
		g_usleep(100000);
	}
	g_set_error_literal(error, AI_ERROR, AI_ERROR_TIMEOUT,
		"Native /usage panel was not available; check CLI authentication, trust, and version");
done:
	g_clear_pointer(&output, g_free);
	output = panel_command(client, tmux, socket, stop, NULL, 2000, NULL);
	g_unlink(socket); g_rmdir(dir);
	return report;
}

/* Claude's own statistics cache is a read-only historical source. Its date
 * stamps remain source dates, and per-model totals are not priced locally. */
static AiCliReport *
query_claude_history(AiCliClient *client, guint limit, GCancellable *cancel, GError **error)
{
	const gchar *config = ai_cli_client_get_env(client, "CLAUDE_CONFIG_DIR");
	g_autofree gchar *default_dir = NULL;
	g_autofree gchar *path = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileInputStream) stream = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *buffer = g_malloc(REPORT_BYTES + 1);
	g_autoptr(AiCliReport) report = NULL;
	g_autoptr(GPtrArray) sorted = NULL;
	JsonObject *obj;
	JsonArray *days;
	gsize size;
	guint i, count;
	if (config == NULL) config = g_getenv("CLAUDE_CONFIG_DIR");
	if (config == NULL)
	{
		const gchar *home = ai_cli_client_get_env(client, "HOME");
		default_dir = g_build_filename(home != NULL ? home : g_get_home_dir(), ".claude", NULL);
		config = default_dir;
	}
	path = g_build_filename(config, "stats-cache.json", NULL);
	file = g_file_new_for_path(path);
	stream = g_file_read(file, cancel, error);
	if (stream == NULL) return NULL;
	if (!g_input_stream_read_all(G_INPUT_STREAM(stream), buffer, REPORT_BYTES + 1, &size, cancel, error)) return NULL;
	if (size > REPORT_BYTES || !json_parser_load_from_data(parser, buffer, (gssize)size, NULL) ||
	    (obj = ai_json_root_object(parser)) == NULL || (days = ai_json_get_array(obj, "dailyModelTokens")) == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Claude statistics cache is oversized or has an unsupported schema"); return NULL;
	}
	report = _ai_cli_report_new(client, AI_CLI_REPORT_HISTORY, "claude stats-cache.json");
	copy_string(_ai_cli_report_object(report), "source_updated_at", obj, "lastComputedDate");
	json_object_set_string_member(_ai_cli_report_object(report), "availability", "partial");
	json_object_set_string_member(_ai_cli_report_object(report), "detail", "Local cached daily model token totals; may lag native usage and do not imply subscription cost.");
	sorted = sorted_periods(days, "date");
	count = sorted->len;
	for (i = 0; i < count && i < limit; i++)
	{
		JsonObject *day = g_ptr_array_index(sorted, i);
		JsonObject *models = ai_json_get_object(day, "tokensByModel");
		const gchar *date = ai_json_get_string(day, "date", NULL);
		GList *keys, *iter;
		if (date == NULL || models == NULL) continue;
		keys = json_object_get_members(models);
		for (iter = keys; iter != NULL; iter = iter->next)
		{
			g_autofree gchar *label = g_strdup_printf("%s/%s", date, (const gchar *)iter->data);
			JsonObject *row = _ai_cli_report_add_entry(report, label, "tokens");
			copy_number(row, "total_tokens", models, iter->data);
			json_object_set_string_member(row, "model", iter->data);
			json_object_set_string_member(row, "start_at", date);
		}
		g_list_free(keys);
	}
	json_object_set_boolean_member(_ai_cli_report_object(report), "truncated", count > limit);
	return g_steal_pointer(&report);
}

/* Explicit dispatch prevents unknown slash commands from becoming model turns. */
AiCliReport *
_ai_cli_report_query_native(AiCliClient *client, AiCliReportKind kind,
	guint limit, GCancellable *cancellable, GError **error)
{
	AiProviderType type = ai_provider_get_provider_type(AI_PROVIDER(client));
	switch (type)
	{
		case AI_PROVIDER_CODEX_CLI: return query_rpc(client, kind, limit, FALSE, cancellable, error);
		case AI_PROVIDER_GROK_BUILD: return query_rpc(client, kind, limit, TRUE, cancellable, error);
		case AI_PROVIDER_CLAUDE_CODE:
		case AI_PROVIDER_CLAUDE_TMUX:
			return kind == AI_CLI_REPORT_USAGE ? query_panel(client, cancellable, error)
				: query_claude_history(client, limit, cancellable, error);
		case AI_PROVIDER_ANTIGRAVITY:
			if (kind == AI_CLI_REPORT_USAGE) return query_panel(client, cancellable, error);
			break;
		default: break;
	}
	g_set_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		"%s has no verified native %s reporting interface in ai-glib",
		ai_provider_type_to_string(type), kind == AI_CLI_REPORT_USAGE ? "usage" : "history");
	return NULL;
}
