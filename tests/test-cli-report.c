/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static gchar *self_path;

/* A real `get_usage` answer from Claude Code 2.1.283, trimmed. It keeps the
 * null windows, an unrecognised window and a disabled credit pool, because
 * those are what a normaliser has to ignore rather than report. */
#define CLAUDE_USAGE \
	"{\"subscription_type\":\"max\",\"rate_limits_available\":true,\"rate_limits\":{" \
	"\"five_hour\":{\"utilization\":15,\"resets_at\":\"2026-09-28T02:30:00.424193+00:00\",\"limit_dollars\":null}," \
	"\"seven_day\":{\"utilization\":21,\"resets_at\":\"2026-10-02T18:00:00.424211+00:00\"}," \
	"\"seven_day_oauth_apps\":null,\"seven_day_opus\":null,\"seven_day_sonnet\":{\"utilization\":null,\"resets_at\":null}," \
	"\"iguana_necktie\":{\"utilization\":0,\"resets_at\":\"2026-11-05T07:59:00+00:00\",\"limit_dollars\":250}," \
	"\"extra_usage\":{\"is_enabled\":false,\"monthly_limit\":10000,\"used_credits\":0,\"utilization\":0,\"currency\":\"USD\",\"decimal_places\":2}," \
	"\"model_scoped\":[{\"display_name\":\"Fable\",\"utilization\":0,\"resets_at\":\"2026-10-02T18:00:00+00:00\"}," \
	"{\"display_name\":7,\"utilization\":50}]},\"behaviors\":null}"

/* Claude Code's stream-json control channel. The stub insists on print mode,
 * no MCP servers, and exactly one control request before it answers: a usage
 * query that sent a prompt, or that started somebody's MCP servers, fails
 * here rather than on a real account. */
static gint
claude_stub(gint argc, gchar **argv)
{
	const gchar *mode = g_getenv("REPORT_TEST_MODE");
	const gchar *result = g_getenv("REPORT_TEST_RESULT");
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GString) args = g_string_new(NULL);
	gchar line[8192];
	JsonObject *root;
	JsonObject *request;
	const gchar *id;
	gint i;
	for (i = 1; i < argc; i++) g_string_append_printf(args, "%s ", argv[i]);
	g_assert_cmpstr(args->str, ==, "-p --input-format stream-json --output-format stream-json --verbose --strict-mcp-config ");
	if (g_strcmp0(mode, "exit") == 0) return 0;
	g_assert_nonnull(fgets(line, sizeof line, stdin));
	g_assert_true(json_parser_load_from_data(parser, line, -1, NULL));
	root = json_node_get_object(json_parser_get_root(parser));
	g_assert_cmpstr(json_object_get_string_member(root, "type"), ==, "control_request");
	id = json_object_get_string_member(root, "request_id");
	request = json_object_get_object_member(root, "request");
	g_assert_cmpstr(json_object_get_string_member(request, "subtype"), ==, "get_usage");
	g_assert_true(json_object_get_boolean_member(request, "skip_behaviors"));
	if (g_strcmp0(mode, "stall") == 0) { g_usleep(10000000); return 0; }
	/* Noise the reader has to skip: a system line, another request's answer. */
	puts("{\"type\":\"system\",\"subtype\":\"init\"}");
	puts("{\"type\":\"control_response\",\"response\":{\"subtype\":\"success\",\"request_id\":\"someone-else\",\"response\":{}}}");
	if (g_strcmp0(mode, "invalid") == 0) puts("not json");
	else if (g_strcmp0(mode, "unsupported") == 0)
		printf("{\"type\":\"control_response\",\"response\":{\"subtype\":\"error\",\"request_id\":\"%s\",\"error\":\"Unsupported control request subtype: get_usage\"}}\n", id);
	else if (g_strcmp0(mode, "error") == 0)
		printf("{\"type\":\"control_response\",\"response\":{\"subtype\":\"error\",\"request_id\":\"%s\",\"error\":\"SECRET_MUST_NOT_LEAK\"}}\n", id);
	else
		printf("{\"type\":\"control_response\",\"response\":{\"subtype\":\"success\",\"request_id\":\"%s\",\"response\":%s}}\n", id, result);
	fflush(stdout);
	/* Stay alive with stdin open: the caller, not EOF, ends this process. */
	while (fgets(line, sizeof line, stdin) != NULL) ;
	return 0;
}

/* The test executable doubles as a native CLI. Reject every method outside
 * initialization and the reporting protocol so tests cannot hide model turns. */
static gint
native_stub(gint argc, gchar **argv)
{
	gchar line[8192];
	const gchar *mode = g_getenv("REPORT_TEST_MODE");
	const gchar *result = g_getenv("REPORT_TEST_RESULT");
	gboolean grok = g_str_equal(argv[1], "agent");
	if (g_str_equal(argv[1], "/usage") || g_str_equal(argv[1], "--prompt-interactive"))
	{
		if (g_str_equal(argv[1], "--prompt-interactive")) g_assert_cmpstr(argv[2], ==, "/usage");
		puts(result); fflush(stdout); g_usleep(10000000); return 0;
	}
	g_assert_cmpint(argc, ==, 4);
	g_assert_cmpstr(argv[2], ==, grok ? "--no-leader" : "--listen");
	g_assert_cmpstr(argv[3], ==, grok ? "stdio" : "stdio://");
	g_assert_nonnull(fgets(line, sizeof line, stdin));
	g_assert_nonnull(strstr(line, "\"method\":\"initialize\""));
	puts("{\"id\":1,\"result\":{}}"); fflush(stdout);
	if (!grok)
	{
		g_assert_nonnull(fgets(line, sizeof line, stdin));
		g_assert_nonnull(strstr(line, "\"method\":\"initialized\""));
	}
	g_assert_nonnull(fgets(line, sizeof line, stdin));
	g_assert_nonnull(strstr(line, g_getenv("REPORT_TEST_METHOD")));
	if (g_strcmp0(mode, "stall") == 0) { g_usleep(10000000); return 0; }
	if (g_strcmp0(mode, "oversized") == 0)
	{
		guint i;
		for (i = 0; i < 1024 * 1024 + 4096; i++) putchar('x');
		putchar('\n');
	}
	else if (g_strcmp0(mode, "invalid") == 0) puts("not json");
	else if (g_strcmp0(mode, "error") == 0)
		puts("{\"id\":2,\"error\":{\"code\":-32601,\"message\":\"SECRET_MUST_NOT_LEAK\"}}");
	else
	{
		puts("{\"method\":\"notification\",\"params\":{}}");
		printf("{\"id\":2,\"result\":%s}\n", result);
	}
	fflush(stdout);
	return 0;
}

/* All provider execution is redirected to the local fixture executable. */
static AiCliClient *
client_new(gboolean grok, const gchar *result, const gchar *method)
{
	AiCliClient *client = grok ? AI_CLI_CLIENT(ai_grok_build_client_new())
	                          : AI_CLI_CLIENT(ai_codex_cli_client_new());
	ai_cli_client_set_executable_path(client, self_path);
	ai_cli_client_set_env(client, "REPORT_TEST_RESULT", result);
	ai_cli_client_set_env(client, "REPORT_TEST_METHOD", method);
	ai_cli_client_set_process_timeout_ms(client, 1500);
	return client;
}

static void
test_codex_usage(void)
{
	g_autoptr(AiCliClient) client = client_new(FALSE,
		"{\"rateLimitsByLimitId\":{\"codex\":{\"planType\":\"pro\",\"primary\":{\"usedPercent\":25,\"windowDurationMins\":300,\"resetsAt\":0},\"secondary\":{\"usedPercent\":\"unknown\"}}}}",
		"account/rateLimits/read");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, NULL, &error);
	g_autoptr(JsonNode) data = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *json = NULL;
	JsonArray *rows;
	JsonObject *row;
	g_assert_no_error(error); g_assert_nonnull(report);
	g_assert_cmpstr(ai_cli_report_get_provider(report), ==, "codex-cli");
	g_assert_cmpint(ai_cli_report_get_kind(report), ==, AI_CLI_REPORT_USAGE);
	data = ai_cli_report_dup_data(report);
	rows = json_object_get_array_member(json_node_get_object(data), "entries");
	g_assert_cmpuint(json_array_get_length(rows), ==, 2);
	row = json_array_get_object_element(rows, 0);
	g_assert_cmpint(json_object_get_int_member(row, "used_percent"), ==, 25);
	g_assert_cmpstr(json_object_get_string_member(row, "reset_at"), ==, "1970-01-01T00:00:00Z");
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(json_array_get_object_element(rows, 1), "used_percent")));
	json_object_set_string_member(row, "label", "MUTATED_COPY");
	json = ai_cli_report_to_json(report);
	g_assert_null(strstr(json, "MUTATED_COPY"));
	text = ai_cli_report_to_text(report);
	g_assert_nonnull(strstr(text, "used_percent: 25"));
	g_assert_null(strchr(text, '\033'));
}

static void
test_history(void)
{
	g_autoptr(AiCliClient) client = client_new(FALSE,
		"{\"dailyUsageBuckets\":[{\"startDate\":\"2026-09-06\",\"tokens\":0},{\"startDate\":\"2026-09-07\",\"tokens\":9007199254740993}]}",
		"account/usage/read");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = ai_cli_client_query_report(client, AI_CLI_REPORT_HISTORY, 1, NULL, &error);
	g_autoptr(JsonNode) data = NULL;
	JsonObject *obj, *row;
	g_assert_no_error(error);
	data = ai_cli_report_dup_data(report); obj = json_node_get_object(data);
	g_assert_true(json_object_get_boolean_member(obj, "truncated"));
	row = json_array_get_object_element(json_object_get_array_member(obj, "entries"), 0);
	g_assert_cmpint(json_object_get_int_member(row, "total_tokens"), ==, G_GINT64_CONSTANT(9007199254740993));
}

static void
test_grok(void)
{
	g_autoptr(AiCliClient) client = client_new(TRUE,
		"{\"subscription_tier\":\"SuperGrok\",\"config\":{\"creditUsagePercent\":38,\"monthlyLimit\":{\"val\":\"10000\"},\"used\":{\"val\":125},\"onDemandUsed\":{},\"history\":[{\"billingCycle\":{\"year\":2026,\"month\":8},\"totalUsed\":{\"val\":250}}]}}",
		"_x.ai/billing");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, NULL, &error);
	g_autoptr(JsonNode) data = NULL;
	JsonArray *rows;
	g_assert_no_error(error);
	data = ai_cli_report_dup_data(report);
	rows = json_object_get_array_member(json_node_get_object(data), "entries");
	g_assert_cmpfloat(json_object_get_double_member(json_array_get_object_element(rows, 0), "used"), ==, 1.25);
	g_assert_cmpfloat(json_object_get_double_member(json_array_get_object_element(rows, 1), "used"), ==, 0);
	g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(json_array_get_object_element(rows, 1), "limit")));
	g_clear_object(&report); g_clear_pointer(&data, json_node_unref);
	report = ai_cli_client_query_report(client, AI_CLI_REPORT_HISTORY, 20, NULL, &error);
	g_assert_no_error(error); data = ai_cli_report_dup_data(report);
	rows = json_object_get_array_member(json_node_get_object(data), "entries");
	g_assert_cmpfloat(json_object_get_double_member(json_array_get_object_element(rows, 0), "cost_usd"), ==, 2.5);
}

/* Exercise transport bounds and sanitized failures with no real backend. */
static void
test_failure(gconstpointer value)
{
	const gchar *mode = value;
	g_autoptr(AiCliClient) client = client_new(FALSE, "{}", "account/rateLimits/read");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = NULL;
	gint64 started = g_get_monotonic_time();
	ai_cli_client_set_env(client, "REPORT_TEST_MODE", mode);
	ai_cli_client_set_process_timeout_ms(client, 300);
	report = ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, NULL, &error);
	g_assert_null(report); g_assert_nonnull(error);
	g_assert_null(strstr(error->message, "SECRET_MUST_NOT_LEAK"));
	g_assert_cmpint(g_get_monotonic_time() - started, <, 3000000);
	if (g_str_equal(mode, "stall")) g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	else if (g_str_equal(mode, "error") || g_str_equal(mode, "missing")) g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
	else g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

typedef struct { GMainLoop *loop; AiCliReport *report; GError *error; } AsyncResult;
static void
query_done(GObject *source, GAsyncResult *result, gpointer data)
{
	AsyncResult *out = data;
	out->report = ai_cli_client_query_report_finish(AI_CLI_CLIENT(source), result, &out->error);
	g_main_loop_quit(out->loop);
}
static gboolean
cancel_query(gpointer data)
{
	g_cancellable_cancel(data); return G_SOURCE_REMOVE;
}
static void
test_async_cancel(void)
{
	g_autoptr(AiCliClient) client = client_new(FALSE, "{}", "account/rateLimits/read");
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	AsyncResult out = { g_main_loop_new(NULL, FALSE), NULL, NULL };
	ai_cli_client_set_env(client, "REPORT_TEST_MODE", "stall");
	ai_cli_client_query_report_async(client, AI_CLI_REPORT_USAGE, 20, cancel, query_done, &out);
	g_timeout_add(50, cancel_query, cancel);
	g_main_loop_run(out.loop);
	g_assert_null(out.report); g_assert_error(out.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&out.error); g_main_loop_unref(out.loop);
}

/* Empty history is available, unlike a missing history capability. */
static void
test_async_success(void)
{
	g_autoptr(AiCliClient) client = client_new(FALSE, "{\"dailyUsageBuckets\":[]}", "account/usage/read");
	AsyncResult out = { g_main_loop_new(NULL, FALSE), NULL, NULL };
	g_autoptr(JsonNode) data = NULL;
	ai_cli_client_query_report_async(client, AI_CLI_REPORT_HISTORY, 20, NULL, query_done, &out);
	g_main_loop_run(out.loop);
	g_assert_no_error(out.error); g_assert_nonnull(out.report);
	data = ai_cli_report_dup_data(out.report);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(data), "entries")), ==, 0);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(data), "availability"), ==, "available");
	g_clear_object(&out.report); g_main_loop_unref(out.loop);
}

/* Invalid requests and pre-cancellation must not launch a native executable. */
static void
test_validation(void)
{
	g_autoptr(AiCliClient) client = AI_CLI_CLIENT(ai_codex_cli_client_new());
	g_autoptr(GError) error = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	ai_cli_client_set_executable_path(client, "/does/not/exist");
	g_assert_null(ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 0, NULL, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST); g_clear_error(&error);
	g_assert_null(ai_cli_client_query_report(client, (AiCliReportKind)99, 20, NULL, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST); g_clear_error(&error);
	g_cancellable_cancel(cancel);
	g_assert_null(ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, cancel, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_cache(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-report-cache-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "stats-cache.json", NULL);
	g_autoptr(AiCliClient) client = AI_CLI_CLIENT(ai_claude_code_client_new());
	g_autoptr(AiCliReport) report = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	g_assert_true(g_file_set_contents(path, "{\"lastComputedDate\":\"2026-09-07\",\"dailyModelTokens\":[{\"date\":\"2026-09-07\",\"tokensByModel\":{\"sonnet\":123}}]}", -1, NULL));
	ai_cli_client_set_env(client, "CLAUDE_CONFIG_DIR", dir);
	ai_cli_client_set_executable_path(client, "/does/not/exist");
	report = ai_cli_client_query_report(client, AI_CLI_REPORT_HISTORY, 20, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(report);
	text = ai_cli_report_to_text(report); g_assert_nonnull(strstr(text, "total_tokens: 123"));
	g_unlink(path); g_rmdir(dir);
}

static void
test_panel(gconstpointer value)
{
	gboolean trust = GPOINTER_TO_INT(value) == 2;
	g_autofree gchar *tmux = g_find_program_in_path("tmux");
	g_autoptr(AiCliClient) client = GPOINTER_TO_INT(value) == 1
		? AI_CLI_CLIENT(ai_antigravity_client_new()) : AI_CLI_CLIENT(ai_claude_code_client_new());
	g_autoptr(AiCliReport) report = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	if (tmux == NULL) { g_test_skip("tmux unavailable"); return; }
	ai_cli_client_set_executable_path(client, self_path);
	ai_cli_client_set_process_timeout_ms(client, 3000);
	/* Claude reaches the panel only when its CLI predates get_usage. */
	ai_cli_client_set_env(client, "REPORT_TEST_MODE", "unsupported");
	ai_cli_client_set_env(client, "REPORT_TEST_RESULT", trust ? "Do you trust the contents of this project?"
		: "Usage\nCurrent session\n25% used\nResets tomorrow\nWeekly\n75% remaining\nUnknown\n40%\n│ Gemini Pro 80% left │");
	report = ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, NULL, &error);
	if (trust) { g_assert_null(report); g_assert_error(error, AI_ERROR, AI_ERROR_PERMISSION_DENIED); return; }
	g_assert_no_error(error); g_assert_nonnull(report);
	json = ai_cli_report_to_json(report);
	g_assert_nonnull(strstr(json, "\"used_percent\" : 25"));
	g_assert_nonnull(strstr(json, "\"remaining_percent\" : 75"));
	g_assert_nonnull(strstr(json, "Resets tomorrow"));
	g_assert_nonnull(strstr(json, "partial"));
	g_assert_nonnull(strstr(json, "\"label\" : \"Gemini Pro\""));
}

static AiCliClient *
claude_new(gboolean tmux, const gchar *mode, const gchar *result)
{
	AiCliClient *client = tmux ? AI_CLI_CLIENT(ai_claude_tmux_client_new())
	                           : AI_CLI_CLIENT(ai_claude_code_client_new());
	ai_cli_client_set_executable_path(client, self_path);
	ai_cli_client_set_env(client, "REPORT_TEST_MODE", mode);
	ai_cli_client_set_env(client, "REPORT_TEST_RESULT", result);
	ai_cli_client_set_process_timeout_ms(client, 1500);
	return client;
}

static JsonObject *
entry_labelled(JsonArray *rows, const gchar *label)
{
	guint i;
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		if (g_strcmp0(json_object_get_string_member(row, "label"), label) == 0) return row;
	}
	return NULL;
}

/* The fix for a panel that always read "Unavailable": the structured
 * answer, normalised to used_percent so ai-quota.h can say how much is left. */
static void
test_claude_usage(gconstpointer value)
{
	g_autoptr(AiCliClient) client = claude_new(GPOINTER_TO_INT(value), "ok", CLAUDE_USAGE);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, NULL, &error);
	g_autoptr(JsonNode) data = NULL;
	JsonObject *obj, *row;
	JsonArray *rows;
	g_assert_no_error(error); g_assert_nonnull(report);
	data = ai_cli_report_dup_data(report); obj = json_node_get_object(data);
	g_assert_cmpstr(json_object_get_string_member(obj, "source"), ==, "claude get_usage");
	g_assert_cmpstr(json_object_get_string_member(obj, "availability"), ==, "available");
	g_assert_cmpstr(json_object_get_string_member(obj, "plan"), ==, "max");
	rows = json_object_get_array_member(obj, "entries");
	/* Session, weekly, Fable. Null windows, a null utilization, an unknown
	 * key, a disabled credit pool and a non-string model label are not rows. */
	g_assert_cmpuint(json_array_get_length(rows), ==, 3);
	row = json_array_get_object_element(rows, 0);
	g_assert_cmpstr(json_object_get_string_member(row, "label"), ==, "Session (5h)");
	g_assert_cmpint(json_object_get_int_member(row, "used_percent"), ==, 15);
	g_assert_cmpint(json_object_get_int_member(row, "window_minutes"), ==, 300);
	g_assert_cmpstr(json_object_get_string_member(row, "reset_at"), ==, "2026-09-28T02:30:00Z");
	row = entry_labelled(rows, "Weekly");
	g_assert_nonnull(row);
	g_assert_cmpint(json_object_get_int_member(row, "used_percent"), ==, 21);
	g_assert_cmpint(json_object_get_int_member(row, "window_minutes"), ==, 10080);
	row = entry_labelled(rows, "Weekly (Fable)");
	g_assert_nonnull(row);
	g_assert_cmpint(json_object_get_int_member(row, "used_percent"), ==, 0);
	g_assert_null(entry_labelled(rows, "Weekly (Sonnet)"));
}

static void
test_claude_failure(gconstpointer value)
{
	const gchar *mode = value;
	g_autoptr(AiCliClient) client = claude_new(FALSE, mode,
		g_str_equal(mode, "no-limits") ? "{\"subscription_type\":null,\"rate_limits_available\":false,\"rate_limits\":null}"
		: g_str_equal(mode, "empty") ? "{\"rate_limits_available\":true,\"rate_limits\":{\"five_hour\":null}}"
		: "{}");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiCliReport) report = NULL;
	gint64 started = g_get_monotonic_time();
	ai_cli_client_set_process_timeout_ms(client, 400);
	report = ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, NULL, &error);
	g_assert_null(report); g_assert_nonnull(error);
	g_assert_null(strstr(error->message, "SECRET_MUST_NOT_LEAK"));
	g_assert_cmpint(g_get_monotonic_time() - started, <, 3000000);
	if (g_str_equal(mode, "stall")) g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	else if (g_str_equal(mode, "invalid")) g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
	else if (g_str_equal(mode, "error") || g_str_equal(mode, "exit")) g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	else
	{
		/* "Your plan has no limits here" is an answer, not a breakage. */
		g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
		if (g_str_equal(mode, "no-limits")) g_assert_nonnull(strstr(error->message, "API key"));
	}
}

/* Ollama transport runs another model through the claude launcher; a plan
 * limit is meaningless there and nothing may be spawned to find that out. */
static void
test_claude_ollama(void)
{
	g_autoptr(AiCliClient) client = AI_CLI_CLIENT(ai_claude_code_client_new());
	g_autoptr(GError) error = NULL;
	ai_cli_client_set_executable_path(client, "/does/not/exist");
	ai_cli_client_set_model(client, "ollama/qwen3");
	g_assert_null(ai_cli_client_query_report(client, AI_CLI_REPORT_USAGE, 20, NULL, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
	g_assert_nonnull(strstr(error->message, "ollama"));
}

int
main(int argc, char **argv)
{
	gint result;
	/* GTest's own -p selects a test path, so the stub needs the second
	 * word too before it takes over as Claude. */
	if (argc > 2 && g_str_equal(argv[1], "-p") && g_str_equal(argv[2], "--input-format"))
		return claude_stub(argc, argv);
	if (argc > 1 && (g_str_equal(argv[1], "app-server") || g_str_equal(argv[1], "agent") ||
	    g_str_equal(argv[1], "/usage") || g_str_equal(argv[1], "--prompt-interactive"))) return native_stub(argc, argv);
	g_test_init(&argc, &argv, NULL);
	alarm(30);
	self_path = g_file_read_link("/proc/self/exe", NULL);
	g_test_add_func("/cli-report/codex/usage", test_codex_usage);
	g_test_add_func("/cli-report/codex/history", test_history);
	g_test_add_func("/cli-report/grok", test_grok);
	g_test_add_data_func("/cli-report/failure/stall", "stall", test_failure);
	g_test_add_data_func("/cli-report/failure/invalid", "invalid", test_failure);
	g_test_add_data_func("/cli-report/failure/oversized", "oversized", test_failure);
	g_test_add_data_func("/cli-report/failure/error", "error", test_failure);
	g_test_add_data_func("/cli-report/failure/missing", "missing", test_failure);
	g_test_add_func("/cli-report/async-cancel", test_async_cancel);
	g_test_add_func("/cli-report/async-success", test_async_success);
	g_test_add_func("/cli-report/validation", test_validation);
	g_test_add_func("/cli-report/claude-cache", test_cache);
	g_test_add_data_func("/cli-report/claude/usage/code", GINT_TO_POINTER(0), test_claude_usage);
	g_test_add_data_func("/cli-report/claude/usage/tmux", GINT_TO_POINTER(1), test_claude_usage);
	g_test_add_data_func("/cli-report/claude/failure/stall", "stall", test_claude_failure);
	g_test_add_data_func("/cli-report/claude/failure/invalid", "invalid", test_claude_failure);
	g_test_add_data_func("/cli-report/claude/failure/error", "error", test_claude_failure);
	g_test_add_data_func("/cli-report/claude/failure/exit", "exit", test_claude_failure);
	g_test_add_data_func("/cli-report/claude/failure/no-limits", "no-limits", test_claude_failure);
	g_test_add_data_func("/cli-report/claude/failure/empty", "empty", test_claude_failure);
	g_test_add_func("/cli-report/claude/ollama", test_claude_ollama);
	g_test_add_data_func("/cli-report/panel/claude-fallback", GINT_TO_POINTER(0), test_panel);
	g_test_add_data_func("/cli-report/panel/agy", GINT_TO_POINTER(1), test_panel);
	g_test_add_data_func("/cli-report/panel/trust", GINT_TO_POINTER(2), test_panel);
	result = g_test_run(); g_free(self_path); return result;
}
