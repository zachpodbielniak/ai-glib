/*
 * test-grok-build-client.c - Unit tests for AiGrokBuildClient
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-grok-build-client.h"
#include "providers/ai-grok-build-client-internal.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"
#include "core/ai-error.h"
#include "model/ai-message.h"
#include "model/ai-text-content.h"

/* ----------------------------------------------------------------
 * argv helpers
 * ---------------------------------------------------------------- */

/* Return the index of @needle in NULL-terminated @argv, or -1. */
static gint
argv_index_of(gchar **argv, const gchar *needle)
{
	gint i;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			return i;
	return -1;
}

/* Count occurrences of @needle in NULL-terminated @argv. */
static gint
argv_count(gchar **argv, const gchar *needle)
{
	gint i, n = 0;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			n++;
	return n;
}

/* Value that follows @needle, or NULL when absent or dangling. */
static const gchar *
argv_value_after(gchar **argv, const gchar *needle)
{
	gint i = argv_index_of(argv, needle);

	if (i < 0 || argv[i + 1] == NULL)
		return NULL;

	return argv[i + 1];
}

/* Build argv for a single-user-message turn against @client. */
static gchar **
build_argv_for(AiGrokBuildClient *client, const gchar *system,
               gboolean streaming)
{
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	gchar **argv;

	argv = ai_grok_build_client_build_argv(
		AI_CLI_CLIENT(client), messages, system, 4096, streaming);

	g_list_free_full(messages, g_object_unref);
	return argv;
}

/* ----------------------------------------------------------------
 * Construction and interfaces
 * ---------------------------------------------------------------- */

static void
test_grok_build_client_new(void)
{
	g_autoptr(AiGrokBuildClient) client = NULL;

	client = ai_grok_build_client_new();
	g_assert_nonnull(client);
	g_assert_true(AI_IS_GROK_BUILD_CLIENT(client));
	g_assert_true(AI_IS_CLI_CLIENT(client));
}

static void
test_grok_build_client_default_model(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	g_assert_cmpstr(ai_cli_client_get_model(AI_CLI_CLIENT(client)),
	                ==, AI_GROK_BUILD_DEFAULT_MODEL);
}

static void
test_grok_build_client_provider_interface(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	g_assert_true(AI_IS_PROVIDER(client));
	g_assert_cmpint(ai_provider_get_provider_type(AI_PROVIDER(client)),
	                ==, AI_PROVIDER_GROK_BUILD);
	g_assert_cmpstr(ai_provider_get_name(AI_PROVIDER(client)),
	                ==, "Grok Build");
	g_assert_cmpstr(ai_provider_get_default_model(AI_PROVIDER(client)),
	                ==, AI_GROK_BUILD_DEFAULT_MODEL);
}

static void
test_grok_build_client_streamable_interface(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	g_assert_true(AI_IS_STREAMABLE(client));
}

static void
test_grok_build_client_gtype(void)
{
	GType type = ai_grok_build_client_get_type();

	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiGrokBuildClient");
}

static void
test_grok_build_client_model(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	ai_cli_client_set_model(AI_CLI_CLIENT(client),
	                        AI_GROK_BUILD_MODEL_GROK_4_5);
	g_assert_cmpstr(ai_cli_client_get_model(AI_CLI_CLIENT(client)),
	                ==, AI_GROK_BUILD_MODEL_GROK_4_5);
}

static void
test_grok_build_client_executable_path(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	/* NULL means "resolve GROK_PATH, else search PATH for grok". */
	g_assert_null(ai_cli_client_get_executable_path(AI_CLI_CLIENT(client)));

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client),
	                                  "/usr/local/bin/grok");
	g_assert_cmpstr(ai_cli_client_get_executable_path(AI_CLI_CLIENT(client)),
	                ==, "/usr/local/bin/grok");
}

/*
 * The inherited process-timeout-ms knob. The default MUST be non-zero:
 * 0 means an unbounded g_subprocess_communicate wait.
 */
static void
test_grok_build_client_process_timeout(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	g_assert_cmpint(
		ai_cli_client_get_process_timeout_ms(AI_CLI_CLIENT(client)),
		==, 1800000);
}

static void
test_grok_build_client_properties(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	/* Defaults */
	g_assert_cmpfloat(ai_grok_build_client_get_total_cost(client), ==, 0.0);
	g_assert_false(ai_grok_build_client_get_skip_permissions(client));
	g_assert_null(ai_grok_build_client_get_permission_mode(client));
	g_assert_null(ai_grok_build_client_get_sandbox(client));
	g_assert_cmpint(ai_grok_build_client_get_max_turns(client), ==, 0);
	g_assert_false(ai_grok_build_client_get_disable_web_search(client));
	/* Verbatim defaults ON so a prompt starting with "/" stays a prompt. */
	g_assert_true(ai_grok_build_client_get_verbatim(client));

	/* Round trips */
	ai_grok_build_client_set_permission_mode(client, "acceptEdits");
	g_assert_cmpstr(ai_grok_build_client_get_permission_mode(client),
	                ==, "acceptEdits");

	ai_grok_build_client_set_allowed_tools(client, "read_file,list_dir");
	g_assert_cmpstr(ai_grok_build_client_get_allowed_tools(client),
	                ==, "read_file,list_dir");

	ai_grok_build_client_set_disallowed_tools(client, "run_terminal_command");
	g_assert_cmpstr(ai_grok_build_client_get_disallowed_tools(client),
	                ==, "run_terminal_command");

	ai_grok_build_client_set_sandbox(client, "workspace");
	g_assert_cmpstr(ai_grok_build_client_get_sandbox(client), ==, "workspace");

	ai_grok_build_client_set_max_turns(client, 12);
	g_assert_cmpint(ai_grok_build_client_get_max_turns(client), ==, 12);

	ai_grok_build_client_set_agent(client, "reviewer");
	g_assert_cmpstr(ai_grok_build_client_get_agent(client), ==, "reviewer");

	ai_grok_build_client_set_rules(client, "Be terse.");
	g_assert_cmpstr(ai_grok_build_client_get_rules(client), ==, "Be terse.");

	ai_grok_build_client_set_disable_web_search(client, TRUE);
	g_assert_true(ai_grok_build_client_get_disable_web_search(client));

	ai_grok_build_client_set_verbatim(client, FALSE);
	g_assert_false(ai_grok_build_client_get_verbatim(client));
}

/* ----------------------------------------------------------------
 * build_argv
 * ---------------------------------------------------------------- */

/*
 * The baseline command. The prompt is NEVER in argv -- it is piped on
 * stdin and read back through --prompt-file /dev/stdin, which is what
 * keeps a large conversation from hitting ARG_MAX.
 */
static void
test_grok_build_argv_basic(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv[0], ==, "grok");
	g_assert_cmpstr(argv_value_after(argv, "--prompt-file"), ==, "/dev/stdin");
	g_assert_cmpstr(argv_value_after(argv, "--output-format"), ==, "json");
	g_assert_cmpstr(argv_value_after(argv, "--model"),
	                ==, AI_GROK_BUILD_DEFAULT_MODEL);

	/* Verbatim is on by default; streaming flags are not. */
	g_assert_cmpint(argv_index_of(argv, "--verbatim"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--include-partial-messages"), ==, -1);

	/* No positional prompt anywhere. */
	g_assert_cmpint(argv_index_of(argv, "hi"), ==, -1);
}

static void
test_grok_build_argv_streaming(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, TRUE);

	g_assert_cmpstr(argv_value_after(argv, "--output-format"),
	                ==, "streaming-messages-json");
	/* Without this, grok emits whole messages and nothing streams. */
	g_assert_cmpint(argv_index_of(argv, "--include-partial-messages"), >, 0);
}

static void
test_grok_build_argv_skip_permissions(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_skip_permissions(client, TRUE);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--permission-mode"),
	                ==, "bypassPermissions");
	g_assert_cmpint(argv_count(argv, "--permission-mode"), ==, 1);
}

static void
test_grok_build_argv_permission_mode(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_permission_mode(client, "acceptEdits");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--permission-mode"),
	                ==, "acceptEdits");
}

/* An unknown mode is dropped with a warning, not passed through. */
static void
test_grok_build_argv_permission_mode_invalid(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_permission_mode(client, "yolo");

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
	                      "*unknown permission mode 'yolo'*");
	argv = build_argv_for(client, NULL, FALSE);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--permission-mode"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "yolo"), ==, -1);
}

/*
 * skip-permissions wins over a narrower mode, and says so. A caller that
 * asked for acceptEdits and silently got a full bypass is the failure
 * worth surfacing.
 */
static void
test_grok_build_argv_permission_conflict(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_skip_permissions(client, TRUE);
	ai_grok_build_client_set_permission_mode(client, "acceptEdits");

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
	                      "*using bypassPermissions*");
	argv = build_argv_for(client, NULL, FALSE);
	g_test_assert_expected_messages();

	g_assert_cmpstr(argv_value_after(argv, "--permission-mode"),
	                ==, "bypassPermissions");
	g_assert_cmpint(argv_index_of(argv, "acceptEdits"), ==, -1);
}

/* Each CSV item becomes its own --allow / --deny pair. */
static void
test_grok_build_argv_rule_lists(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_allowed_tools(client, "read_file, list_dir");
	ai_grok_build_client_set_disallowed_tools(client, "run_terminal_command");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_count(argv, "--allow"), ==, 2);
	g_assert_cmpint(argv_index_of(argv, "read_file"), >, 0);
	/* Whitespace around a CSV item is stripped, not passed through. */
	g_assert_cmpint(argv_index_of(argv, "list_dir"), >, 0);
	g_assert_cmpint(argv_index_of(argv, " list_dir"), ==, -1);

	g_assert_cmpint(argv_count(argv, "--deny"), ==, 1);
	g_assert_cmpstr(argv_value_after(argv, "--deny"),
	                ==, "run_terminal_command");
}

/* An all-empty list emits nothing rather than a dangling flag. */
static void
test_grok_build_argv_rule_lists_empty_items(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_allowed_tools(client, " , ,");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--allow"), ==, -1);
}

static void
test_grok_build_argv_execution_knobs(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_sandbox(client, "workspace");
	ai_grok_build_client_set_max_turns(client, 7);
	ai_grok_build_client_set_agent(client, "reviewer");
	ai_grok_build_client_set_rules(client, "Be terse.");
	ai_grok_build_client_set_disable_web_search(client, TRUE);
	ai_grok_build_client_set_verbatim(client, FALSE);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--sandbox"), ==, "workspace");
	g_assert_cmpstr(argv_value_after(argv, "--max-turns"), ==, "7");
	g_assert_cmpstr(argv_value_after(argv, "--agent"), ==, "reviewer");
	g_assert_cmpstr(argv_value_after(argv, "--rules"), ==, "Be terse.");
	g_assert_cmpint(argv_index_of(argv, "--disable-web-search"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--verbatim"), ==, -1);
}

/* max-turns 0 means "unset", not "--max-turns 0". */
static void
test_grok_build_argv_max_turns_unset(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--max-turns"), ==, -1);
}

/* A fresh session is primed with the system prompt. */
static void
test_grok_build_argv_system_prompt(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = build_argv_for(client, "You are terse.", FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--system-prompt-override"),
	                ==, "You are terse.");
	g_assert_cmpint(argv_index_of(argv, "--resume"), ==, -1);
}

/*
 * Resuming carries the system prompt already; re-sending it would waste
 * tokens and re-inject the whole prompt.
 */
static void
test_grok_build_argv_resume(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-42");
	argv = build_argv_for(client, "You are terse.", FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--resume"), ==, "sess-42");
	g_assert_cmpint(argv_index_of(argv, "--system-prompt-override"), ==, -1);
}

/* With persistence off, a stored id must not resume anything. */
static void
test_grok_build_argv_resume_no_persistence(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-42");
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	argv = build_argv_for(client, "You are terse.", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--resume"), ==, -1);
	g_assert_cmpstr(argv_value_after(argv, "--system-prompt-override"),
	                ==, "You are terse.");
}

/* The four levels grok accepts pass straight through. */
static void
test_grok_build_argv_effort_levels(void)
{
	static const gchar *levels[] = { "low", "medium", "high", "xhigh" };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
	{
		g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
		g_auto(GStrv) argv = NULL;

		ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), levels[i]);
		argv = build_argv_for(client, NULL, FALSE);

		g_assert_cmpstr(argv_value_after(argv, "--reasoning-effort"),
		                ==, levels[i]);
	}
}

/*
 * AI_EFFORT_MAX has no grok equivalent. Passing it through would make
 * grok print a JSON error and exit 0 -- a run that looks successful and
 * produces nothing -- so it folds onto xhigh instead.
 */
static void
test_grok_build_argv_effort_max_clamped(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "max");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--reasoning-effort"), ==, "xhigh");
}

static void
test_grok_build_argv_effort_invalid(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "turbo");

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
	                      "*unknown effort level 'turbo'*");
	argv = build_argv_for(client, NULL, FALSE);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--reasoning-effort"), ==, -1);
}

/* ----------------------------------------------------------------
 * build_stdin
 * ---------------------------------------------------------------- */

static void
test_grok_build_stdin(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(AiMessage) user = ai_message_new_user("First question");
	g_autoptr(AiMessage) assistant = ai_message_new_assistant("An answer");
	GList *messages = NULL;
	g_autofree gchar *stdin_data = NULL;

	messages = g_list_append(messages, user);
	messages = g_list_append(messages, assistant);

	g_assert_nonnull(klass->build_stdin);
	stdin_data = klass->build_stdin(AI_CLI_CLIENT(client), messages);

	g_assert_nonnull(stdin_data);
	g_assert_nonnull(strstr(stdin_data, "First question"));
	g_assert_nonnull(strstr(stdin_data, "Previous assistant response: "
	                                    "An answer"));

	g_list_free(messages);
}

/* ----------------------------------------------------------------
 * parse_json_output
 * ---------------------------------------------------------------- */

static AiResponse *
parse_json(AiGrokBuildClient *client, const gchar *json, GError **error)
{
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	g_assert_nonnull(klass->parse_json_output);
	return klass->parse_json_output(AI_CLI_CLIENT(client), json, error);
}

/* A real grok --output-format json payload, trimmed. */
static const gchar *GROK_JSON_SUCCESS =
	"{\n"
	"  \"text\": \"OK\",\n"
	"  \"stopReason\": \"end_turn\",\n"
	"  \"sessionId\": \"01a00812-3ebb-7fb3-87c3-a48ecf4b038a\",\n"
	"  \"requestId\": \"0bc42b5b\",\n"
	"  \"thought\": \"simple request\",\n"
	"  \"usage\": {\n"
	"    \"input_tokens\": 20786,\n"
	"    \"cache_read_input_tokens\": 1408,\n"
	"    \"output_tokens\": 24,\n"
	"    \"total_tokens\": 22218\n"
	"  },\n"
	"  \"num_turns\": 1,\n"
	"  \"total_cost_usd\": 0.0072114\n"
	"}\n";

static void
test_grok_build_parse_success(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;
	AiUsage *usage;

	resp = parse_json(client, GROK_JSON_SUCCESS, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resp);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "OK");

	g_assert_cmpint(ai_response_get_stop_reason(resp),
	                ==, AI_STOP_REASON_END_TURN);

	usage = ai_response_get_usage(resp);
	g_assert_nonnull(usage);
	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 20786);
	g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 24);

	g_assert_cmpfloat(ai_grok_build_client_get_total_cost(client),
	                  >, 0.007);

	/* camelCase "sessionId" is what grok emits, and it must be captured. */
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "01a00812-3ebb-7fb3-87c3-a48ecf4b038a");
}

static void
test_grok_build_parse_session_no_persistence(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);

	resp = parse_json(client, GROK_JSON_SUCCESS, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resp);

	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));
}

/*
 * The failure mode that motivated parsing before checking exit status:
 * grok reports a bad --reasoning-effort as a JSON error object on stdout
 * and still exits 0. Treating that as success would return an empty
 * response and call it a win.
 */
static void
test_grok_build_parse_error_object(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client,
		"{\"type\":\"error\",\"message\":\"unknown effort level 'bogus'\"}",
		&error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "unknown effort level"));
}

/* An error line trailing other output is still found. */
static void
test_grok_build_parse_error_trailing_line(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client,
		"warming up\n"
		"{\"type\":\"error\",\"message\":\"boom\"}\n",
		&error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
}

/* The NDJSON result line's snake_case spelling parses too. */
static void
test_grok_build_parse_result_line_shape(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;

	resp = parse_json(client,
		"{\"type\":\"result\",\"subtype\":\"success\",\"result\":\"HELLO\","
		"\"stop_reason\":\"end_turn\",\"session_id\":\"sid-9\","
		"\"usage\":{\"input_tokens\":10,\"output_tokens\":2}}",
		&error);

	g_assert_no_error(error);
	g_assert_nonnull(resp);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "HELLO");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "sid-9");
}

/* Tool calls with no text leave the response empty for the retry path. */
static void
test_grok_build_parse_no_text(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	resp = parse_json(client,
		"{\"text\":\"\",\"stopReason\":\"end_turn\",\"sessionId\":\"s\"}",
		&error);

	g_assert_no_error(error);
	g_assert_nonnull(resp);
	g_assert_null(ai_response_get_content_blocks(resp));
}

static void
test_grok_build_parse_malformed(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "not json at all", &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_grok_build_parse_empty(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "", &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

/* ----------------------------------------------------------------
 * parse_stream_line
 * ---------------------------------------------------------------- */

static gboolean
parse_stream(AiGrokBuildClient *client, const gchar *line, AiResponse *resp,
             gchar **delta, GError **error)
{
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	g_assert_nonnull(klass->parse_stream_line);
	return klass->parse_stream_line(AI_CLI_CLIENT(client), line, resp,
	                                delta, error);
}

static void
test_grok_build_parse_stream_text_delta(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"HELLO\"}}}",
		resp, &delta, &error));

	g_assert_no_error(error);
	g_assert_cmpstr(delta, ==, "HELLO");
}

/* Reasoning is not the answer: thinking deltas produce nothing. */
static void
test_grok_build_parse_stream_thinking_delta(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"index\":0,\"delta\":{\"type\":\"thinking_delta\","
		"\"thinking\":\"hmm\"}}}",
		resp, &delta, &error));

	g_assert_null(delta);
}

/*
 * The whole-message "assistant" line repeats text already delivered as
 * deltas. Emitting it too would double every streamed reply.
 */
static void
test_grok_build_parse_stream_assistant_ignored(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"assistant\",\"message\":{\"type\":\"message\","
		"\"role\":\"assistant\",\"content\":["
		"{\"type\":\"text\",\"text\":\"HELLO\"}]}}",
		resp, &delta, &error));

	g_assert_null(delta);
}

/* The system/init line carries no text and must not break anything. */
static void
test_grok_build_parse_stream_system_init(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"s\","
		"\"model\":\"grok-4.6\"}",
		resp, &delta, &error));

	g_assert_null(delta);
}

static void
test_grok_build_parse_stream_result(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	AiUsage *usage;

	g_assert_true(parse_stream(client,
		"{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
		"\"result\":\"HELLO\",\"stop_reason\":\"end_turn\","
		"\"total_cost_usd\":0.0068,"
		"\"usage\":{\"input_tokens\":19251,\"output_tokens\":22},"
		"\"session_id\":\"01a00812-75ed\"}",
		resp, &delta, &error));

	g_assert_no_error(error);
	g_assert_null(delta);

	/* No delta ever arrived, so the result text back-fills the response. */
	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "HELLO");

	usage = ai_response_get_usage(resp);
	g_assert_nonnull(usage);
	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 19251);

	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "01a00812-75ed");
	g_assert_cmpfloat(ai_grok_build_client_get_total_cost(client), >, 0.006);
}

/* Deltas already delivered the text; the result line must not repeat it. */
static void
test_grok_build_parse_stream_result_no_double_text(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autoptr(AiTextContent) existing = ai_text_content_new("HELLO");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;

	ai_response_add_content_block(resp,
		(AiContentBlock *)g_steal_pointer(&existing));

	g_assert_true(parse_stream(client,
		"{\"type\":\"result\",\"result\":\"HELLO\"}",
		resp, &delta, &error));

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "HELLO");
}

static void
test_grok_build_parse_stream_error_line(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_false(parse_stream(client,
		"{\"type\":\"error\",\"message\":\"boom\"}",
		resp, &delta, &error));

	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_cmpint(ai_response_get_stop_reason(resp),
	                ==, AI_STOP_REASON_ERROR);
}

/* Blank and non-JSON lines are ignored, not errors. */
static void
test_grok_build_parse_stream_noise(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client, "", resp, &delta, &error));
	g_assert_null(delta);

	g_assert_true(parse_stream(client, "garbage", resp, &delta, &error));
	g_assert_null(delta);
	g_assert_no_error(error);
}

/* ----------------------------------------------------------------
 * Property plumbing (the GObject path bindings actually use)
 * ---------------------------------------------------------------- */

static void
on_notify_count(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
	gint *count = user_data;
	(void)obj;
	(void)pspec;
	(*count)++;
}

/* Every property must round-trip through g_object_set/get, not just the
 * C accessors -- that is the path PyGObject and friends take. */
static void
test_grok_build_property_round_trip(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autofree gchar *permission_mode = NULL;
	g_autofree gchar *allowed = NULL;
	g_autofree gchar *disallowed = NULL;
	g_autofree gchar *sandbox = NULL;
	g_autofree gchar *agent = NULL;
	g_autofree gchar *rules = NULL;
	gboolean skip = FALSE, no_web = FALSE, verbatim = FALSE;
	gint max_turns = 0;

	g_object_set(client,
	             "skip-permissions", TRUE,
	             "permission-mode", "plan",
	             "allowed-tools", "read_file",
	             "disallowed-tools", "run_terminal_command",
	             "sandbox", "workspace",
	             "max-turns", 9,
	             "agent", "reviewer",
	             "rules", "Be terse.",
	             "disable-web-search", TRUE,
	             "verbatim", FALSE,
	             NULL);

	g_object_get(client,
	             "skip-permissions", &skip,
	             "permission-mode", &permission_mode,
	             "allowed-tools", &allowed,
	             "disallowed-tools", &disallowed,
	             "sandbox", &sandbox,
	             "max-turns", &max_turns,
	             "agent", &agent,
	             "rules", &rules,
	             "disable-web-search", &no_web,
	             "verbatim", &verbatim,
	             NULL);

	g_assert_true(skip);
	g_assert_cmpstr(permission_mode, ==, "plan");
	g_assert_cmpstr(allowed, ==, "read_file");
	g_assert_cmpstr(disallowed, ==, "run_terminal_command");
	g_assert_cmpstr(sandbox, ==, "workspace");
	g_assert_cmpint(max_turns, ==, 9);
	g_assert_cmpstr(agent, ==, "reviewer");
	g_assert_cmpstr(rules, ==, "Be terse.");
	g_assert_true(no_web);
	g_assert_false(verbatim);

	/* The C accessors must see what the property path wrote. */
	g_assert_true(ai_grok_build_client_get_skip_permissions(client));
	g_assert_cmpstr(ai_grok_build_client_get_sandbox(client), ==, "workspace");
}

/* total-cost is reported by the CLI, never set by the caller. */
static void
test_grok_build_total_cost_read_only(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	GParamSpec *pspec;

	pspec = g_object_class_find_property(G_OBJECT_GET_CLASS(client),
	                                     "total-cost");
	g_assert_nonnull(pspec);
	g_assert_true((pspec->flags & G_PARAM_READABLE) != 0);
	g_assert_true((pspec->flags & G_PARAM_WRITABLE) == 0);
}

/* A setter that changes nothing must not emit notify. */
static void
test_grok_build_notify_on_change_only(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	gint count = 0;

	g_signal_connect(client, "notify::skip-permissions",
	                 G_CALLBACK(on_notify_count), &count);
	g_signal_connect(client, "notify::sandbox",
	                 G_CALLBACK(on_notify_count), &count);

	/* Setting the existing value is a no-op. */
	ai_grok_build_client_set_skip_permissions(client, FALSE);
	ai_grok_build_client_set_sandbox(client, NULL);
	g_assert_cmpint(count, ==, 0);

	ai_grok_build_client_set_skip_permissions(client, TRUE);
	ai_grok_build_client_set_sandbox(client, "workspace");
	g_assert_cmpint(count, ==, 2);

	/* And again with the same values. */
	ai_grok_build_client_set_skip_permissions(client, TRUE);
	ai_grok_build_client_set_sandbox(client, "workspace");
	g_assert_cmpint(count, ==, 2);
}

/* A negative turn budget is rejected, not silently emitted. */
static void
test_grok_build_max_turns_negative(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	ai_grok_build_client_set_max_turns(client, 5);

	g_test_expect_message(NULL, G_LOG_LEVEL_CRITICAL, "*max_turns >= 0*");
	ai_grok_build_client_set_max_turns(client, -1);
	g_test_assert_expected_messages();

	g_assert_cmpint(ai_grok_build_client_get_max_turns(client), ==, 5);
}

/* Clearing a string property back to NULL must work. */
static void
test_grok_build_clear_string_properties(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();

	ai_grok_build_client_set_permission_mode(client, "plan");
	ai_grok_build_client_set_permission_mode(client, NULL);
	g_assert_null(ai_grok_build_client_get_permission_mode(client));

	ai_grok_build_client_set_agent(client, "reviewer");
	ai_grok_build_client_set_agent(client, NULL);
	g_assert_null(ai_grok_build_client_get_agent(client));
}

/* ----------------------------------------------------------------
 * build_argv edge cases
 * ---------------------------------------------------------------- */

/*
 * Positional lock: --prompt-file /dev/stdin leads the arg tail. The base
 * pipeline overwrites argv[0] with the resolved path, so anything that
 * shifted these two would silently change what grok reads the prompt from.
 */
static void
test_grok_build_argv_prompt_file_position(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv[1], ==, "--prompt-file");
	g_assert_cmpstr(argv[2], ==, "/dev/stdin");
	g_assert_cmpint(argv_count(argv, "--prompt-file"), ==, 1);
}

/* No messages at all still produces a runnable command. */
static void
test_grok_build_argv_null_messages(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	argv = ai_grok_build_client_build_argv(AI_CLI_CLIENT(client), NULL,
	                                       NULL, 4096, FALSE);

	g_assert_nonnull(argv);
	g_assert_cmpstr(argv[0], ==, "grok");
	g_assert_cmpstr(argv_value_after(argv, "--prompt-file"), ==, "/dev/stdin");
}

/* An empty model string falls back to the default rather than emitting "". */
static void
test_grok_build_argv_empty_model(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_model(AI_CLI_CLIENT(client), "");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--model"),
	                ==, AI_GROK_BUILD_DEFAULT_MODEL);
	g_assert_cmpint(argv_count(argv, "--model"), ==, 1);
}

/* Model ids are passed through byte for byte; no shell is involved. */
static void
test_grok_build_argv_model_verbatim(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_model(AI_CLI_CLIENT(client), "grok-4.6 $(rm -rf /)");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--model"),
	                ==, "grok-4.6 $(rm -rf /)");
}

/* An empty system prompt is not a system prompt. */
static void
test_grok_build_argv_empty_system_prompt(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = build_argv_for(client, "", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--system-prompt-override"), ==, -1);
}

/* Nor is an empty session id a session. */
static void
test_grok_build_argv_empty_session_id(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "");
	argv = build_argv_for(client, "SYS", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--resume"), ==, -1);
	g_assert_cmpstr(argv_value_after(argv, "--system-prompt-override"),
	                ==, "SYS");
}

/* An empty effort string omits the flag rather than emitting "". */
static void
test_grok_build_argv_empty_effort(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--reasoning-effort"), ==, -1);
}

/* Effort is matched case-sensitively; "LOW" is not a grok level. */
static void
test_grok_build_argv_effort_case(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "LOW");

	g_test_expect_message(NULL, G_LOG_LEVEL_WARNING,
	                      "*unknown effort level 'LOW'*");
	argv = build_argv_for(client, NULL, FALSE);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--reasoning-effort"), ==, -1);
}

/* "default" is a real grok mode and must survive validation. */
static void
test_grok_build_argv_permission_mode_default(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_permission_mode(client, "default");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--permission-mode"),
	                ==, "default");
}

/*
 * skip-permissions alongside an explicit "bypassPermissions" says the same
 * thing twice, so there is nothing to warn about. A warning here would be
 * fatal under GTest, which is the assertion.
 */
static void
test_grok_build_argv_permission_agreement_is_quiet(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_skip_permissions(client, TRUE);
	ai_grok_build_client_set_permission_mode(client, "bypassPermissions");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_count(argv, "--permission-mode"), ==, 1);
	g_assert_cmpstr(argv_value_after(argv, "--permission-mode"),
	                ==, "bypassPermissions");
}

/* A rule is one argv word: internal spaces and parens survive intact. */
static void
test_grok_build_argv_rule_with_spaces(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_grok_build_client_set_allowed_tools(client,
	                                       "run_terminal_command(git log)");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--allow"),
	                ==, "run_terminal_command(git log)");
}

/* Streaming and resume compose; neither one drops the other's flags. */
static void
test_grok_build_argv_streaming_with_resume(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-7");
	ai_grok_build_client_set_skip_permissions(client, TRUE);
	argv = build_argv_for(client, "SYS", TRUE);

	g_assert_cmpstr(argv_value_after(argv, "--output-format"),
	                ==, "streaming-messages-json");
	g_assert_cmpint(argv_index_of(argv, "--include-partial-messages"), >, 0);
	g_assert_cmpstr(argv_value_after(argv, "--resume"), ==, "sess-7");
	g_assert_cmpstr(argv_value_after(argv, "--permission-mode"),
	                ==, "bypassPermissions");
	g_assert_cmpint(argv_index_of(argv, "--system-prompt-override"), ==, -1);
}

/* Building the argv must not mutate the client. */
static void
test_grok_build_argv_is_repeatable(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_auto(GStrv) first = NULL;
	g_auto(GStrv) second = NULL;
	g_autofree gchar *joined_first = NULL;
	g_autofree gchar *joined_second = NULL;

	ai_grok_build_client_set_allowed_tools(client, "read_file,list_dir");
	ai_grok_build_client_set_max_turns(client, 3);

	first = build_argv_for(client, "SYS", FALSE);
	second = build_argv_for(client, "SYS", FALSE);

	joined_first = g_strjoinv(" ", first);
	joined_second = g_strjoinv(" ", second);
	g_assert_cmpstr(joined_first, ==, joined_second);
}

/* ----------------------------------------------------------------
 * build_stdin edge cases
 * ---------------------------------------------------------------- */

static gchar *
build_stdin_for(AiGrokBuildClient *client, GList *messages)
{
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	g_assert_nonnull(klass->build_stdin);
	return klass->build_stdin(AI_CLI_CLIENT(client), messages);
}

/* No messages still yields a prompt (the trailer), never NULL. */
static void
test_grok_build_stdin_no_messages(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autofree gchar *stdin_data = build_stdin_for(client, NULL);

	g_assert_nonnull(stdin_data);
	g_assert_nonnull(strstr(stdin_data, "plain text response"));
}

/*
 * The system prompt travels in argv (--system-prompt-override), NOT in the
 * piped prompt. Inlining it the way the OpenCode client must would send it
 * twice on a fresh session.
 */
static void
test_grok_build_stdin_excludes_system_prompt(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiMessage) user = ai_message_new_user("hello");
	GList *messages = g_list_append(NULL, user);
	g_autofree gchar *stdin_data = NULL;

	ai_cli_client_set_system_prompt(AI_CLI_CLIENT(client), "SECRET-SYSTEM");
	stdin_data = build_stdin_for(client, messages);

	g_assert_null(strstr(stdin_data, "SECRET-SYSTEM"));
	g_assert_nonnull(strstr(stdin_data, "hello"));

	g_list_free(messages);
}

/* Empty messages contribute nothing, and no stray blank separators. */
static void
test_grok_build_stdin_skips_empty_messages(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiMessage) empty = ai_message_new_user("");
	g_autoptr(AiMessage) real = ai_message_new_user("actual content");
	GList *messages = NULL;
	g_autofree gchar *stdin_data = NULL;

	messages = g_list_append(messages, empty);
	messages = g_list_append(messages, real);

	stdin_data = build_stdin_for(client, messages);

	g_assert_true(g_str_has_prefix(stdin_data, "actual content"));

	g_list_free(messages);
}

/* Consecutive user turns are separated, not concatenated. */
static void
test_grok_build_stdin_multiple_user_messages(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiMessage) first = ai_message_new_user("AAA");
	g_autoptr(AiMessage) second = ai_message_new_user("BBB");
	GList *messages = NULL;
	g_autofree gchar *stdin_data = NULL;

	messages = g_list_append(messages, first);
	messages = g_list_append(messages, second);

	stdin_data = build_stdin_for(client, messages);

	g_assert_nonnull(strstr(stdin_data, "AAA\n\nBBB"));

	g_list_free(messages);
}

/* ----------------------------------------------------------------
 * parse_json_output edge cases
 * ---------------------------------------------------------------- */

/* A JSON array is valid JSON but not a response. */
static void
test_grok_build_parse_array_root(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "[1, 2, 3]", &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

/* So is a bare scalar. */
static void
test_grok_build_parse_scalar_root(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "42", &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_grok_build_parse_whitespace_only(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "   \n\n  \n", &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

/*
 * A banner or upgrade notice ahead of the pretty-printed object must not
 * cost us the response. The last-line fallback cannot help here -- the
 * object spans lines -- which is why there is a first-brace fallback too.
 */
static void
test_grok_build_parse_leading_noise(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *payload = NULL;

	payload = g_strconcat("A new version of grok is available!\n",
	                      GROK_JSON_SUCCESS, NULL);

	resp = parse_json(client, payload, &error);

	g_assert_no_error(error);
	g_assert_nonnull(resp);
	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "OK");
}

/* An error with no message still produces a usable GError. */
static void
test_grok_build_parse_error_without_message(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "{\"type\":\"error\"}", &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "Unknown error"));
}

/* The alternate "error" spelling of the message field is accepted. */
static void
test_grok_build_parse_error_alternate_field(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client,
		"{\"type\":\"error\",\"error\":\"rate limited\"}", &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "rate limited"));
}

/* A result flagged is_error is a failure, not an answer. */
static void
test_grok_build_parse_result_is_error(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client,
		"{\"type\":\"result\",\"subtype\":\"error_during_execution\","
		"\"is_error\":true,\"result\":\"turn limit exceeded\"}",
		&error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "turn limit exceeded"));
}

/*
 * Wrong-typed members are ignored rather than crashing or tripping a
 * json-glib critical. Subprocess output is untrusted input.
 */
static void
test_grok_build_parse_wrong_types(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	resp = parse_json(client,
		"{\"text\":123,\"stopReason\":[],\"sessionId\":{},"
		"\"usage\":\"nope\",\"total_cost_usd\":\"free\"}",
		&error);

	g_assert_no_error(error);
	g_assert_nonnull(resp);
	/* Nothing usable, but a well-formed response object. */
	g_assert_null(ai_response_get_content_blocks(resp));
	g_assert_null(ai_response_get_usage(resp));
	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));
	g_assert_cmpfloat(ai_grok_build_client_get_total_cost(client), ==, 0.0);
	g_assert_cmpint(ai_response_get_stop_reason(resp),
	                ==, AI_STOP_REASON_END_TURN);
}

/* An integer cost is still a cost. */
static void
test_grok_build_parse_integer_cost(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	resp = parse_json(client,
		"{\"text\":\"hi\",\"total_cost_usd\":2}", &error);

	g_assert_no_error(error);
	g_assert_cmpfloat(ai_grok_build_client_get_total_cost(client), ==, 2.0);
}

/* A truncated turn must not be reported as a clean end_turn. */
static void
test_grok_build_parse_stop_reason_max_tokens(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	resp = parse_json(client,
		"{\"text\":\"partial\",\"stopReason\":\"max_tokens\"}", &error);

	g_assert_no_error(error);
	g_assert_cmpint(ai_response_get_stop_reason(resp),
	                ==, AI_STOP_REASON_MAX_TOKENS);
}

/* An unrecognised or absent stop reason degrades to end_turn. */
static void
test_grok_build_parse_stop_reason_unknown(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) unknown = NULL;
	g_autoptr(AiResponse) absent = NULL;

	unknown = parse_json(client,
		"{\"text\":\"hi\",\"stopReason\":\"who_knows\"}", &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_response_get_stop_reason(unknown),
	                ==, AI_STOP_REASON_END_TURN);

	absent = parse_json(client, "{\"text\":\"hi\"}", &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_response_get_stop_reason(absent),
	                ==, AI_STOP_REASON_END_TURN);
}

/* A second turn replaces the stored session id rather than appending. */
static void
test_grok_build_parse_session_update(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) first = NULL;
	g_autoptr(AiResponse) second = NULL;

	first = parse_json(client, "{\"text\":\"a\",\"sessionId\":\"s1\"}", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "s1");

	second = parse_json(client, "{\"text\":\"b\",\"sessionId\":\"s2\"}", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "s2");
}

/* NDJSON: the last line wins, which is where the result lands. */
static void
test_grok_build_parse_ndjson_last_line(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;

	resp = parse_json(client,
		"{\"type\":\"system\",\"subtype\":\"init\"}\n"
		"{\"type\":\"assistant\",\"message\":{}}\n"
		"{\"type\":\"result\",\"result\":\"FINAL\",\"session_id\":\"s\"}\n",
		&error);

	g_assert_no_error(error);
	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "FINAL");
}

/* ----------------------------------------------------------------
 * parse_stream_line edge cases
 * ---------------------------------------------------------------- */

/* A stream_event with no event object is ignored, not a crash. */
static void
test_grok_build_parse_stream_missing_event(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client, "{\"type\":\"stream_event\"}",
	                           resp, &delta, &error));
	g_assert_null(delta);

	g_assert_true(parse_stream(client,
		"{\"type\":\"stream_event\",\"event\":\"not-an-object\"}",
		resp, &delta, &error));
	g_assert_null(delta);

	g_assert_true(parse_stream(client,
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\"}}",
		resp, &delta, &error));
	g_assert_null(delta);
}

/* An empty text_delta yields no delta rather than an empty one. */
static void
test_grok_build_parse_stream_empty_text_delta(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"\"}}}",
		resp, &delta, &error));

	g_assert_null(delta);
}

/* Successive deltas each come back; the caller accumulates them. */
static void
test_grok_build_parse_stream_successive_deltas(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) acc = g_string_new("");
	const gchar *lines[] = {
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"HEL\"}}}",
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"LO\"}}}",
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(lines); i++)
	{
		g_autofree gchar *delta = NULL;

		g_assert_true(parse_stream(client, lines[i], resp, &delta, &error));
		g_assert_nonnull(delta);
		g_string_append(acc, delta);
	}

	g_assert_cmpstr(acc->str, ==, "HELLO");
}

/* A result line flagged is_error fails the stream. */
static void
test_grok_build_parse_stream_result_is_error(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_false(parse_stream(client,
		"{\"type\":\"result\",\"is_error\":true,"
		"\"result\":\"tool call did not complete\"}",
		resp, &delta, &error));

	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "tool call did not complete"));
	g_assert_cmpint(ai_response_get_stop_reason(resp),
	                ==, AI_STOP_REASON_ERROR);
}

/* With persistence off, the streamed result must not store a session. */
static void
test_grok_build_parse_stream_result_no_persistence(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);

	g_assert_true(parse_stream(client,
		"{\"type\":\"result\",\"result\":\"hi\",\"session_id\":\"s\"}",
		resp, &delta, &error));

	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));
}

/* A truncated streamed turn keeps its real stop reason. */
static void
test_grok_build_parse_stream_result_stop_reason(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"result\",\"result\":\"partial\","
		"\"stop_reason\":\"max_tokens\"}",
		resp, &delta, &error));

	g_assert_cmpint(ai_response_get_stop_reason(resp),
	                ==, AI_STOP_REASON_MAX_TOKENS);
}

/* Wrong-typed members in a result line are ignored, not fatal. */
static void
test_grok_build_parse_stream_result_wrong_types(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"result\",\"result\":7,\"usage\":[],"
		"\"session_id\":false,\"total_cost_usd\":{}}",
		resp, &delta, &error));

	g_assert_no_error(error);
	g_assert_null(ai_response_get_usage(resp));
	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));
}

/* Line shapes we do not model are skipped without complaint. */
static void
test_grok_build_parse_stream_unknown_shapes(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "grok-4.6");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(parse_stream(client, "[1,2,3]", resp, &delta, &error));
	g_assert_null(delta);

	g_assert_true(parse_stream(client, "{\"type\":\"user\"}",
	                           resp, &delta, &error));
	g_assert_null(delta);

	g_assert_true(parse_stream(client, "{\"no_type\":true}",
	                           resp, &delta, &error));
	g_assert_null(delta);
	g_assert_no_error(error);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/grok-build-client/new",
	                test_grok_build_client_new);
	g_test_add_func("/ai-glib/grok-build-client/default-model",
	                test_grok_build_client_default_model);
	g_test_add_func("/ai-glib/grok-build-client/provider-interface",
	                test_grok_build_client_provider_interface);
	g_test_add_func("/ai-glib/grok-build-client/streamable-interface",
	                test_grok_build_client_streamable_interface);
	g_test_add_func("/ai-glib/grok-build-client/gtype",
	                test_grok_build_client_gtype);
	g_test_add_func("/ai-glib/grok-build-client/model",
	                test_grok_build_client_model);
	g_test_add_func("/ai-glib/grok-build-client/executable-path",
	                test_grok_build_client_executable_path);
	g_test_add_func("/ai-glib/grok-build-client/process-timeout",
	                test_grok_build_client_process_timeout);
	g_test_add_func("/ai-glib/grok-build-client/properties",
	                test_grok_build_client_properties);
	g_test_add_func("/ai-glib/grok-build-client/property-round-trip",
	                test_grok_build_property_round_trip);
	g_test_add_func("/ai-glib/grok-build-client/total-cost-read-only",
	                test_grok_build_total_cost_read_only);
	g_test_add_func("/ai-glib/grok-build-client/notify-on-change-only",
	                test_grok_build_notify_on_change_only);
	g_test_add_func("/ai-glib/grok-build-client/max-turns-negative",
	                test_grok_build_max_turns_negative);
	g_test_add_func("/ai-glib/grok-build-client/clear-string-properties",
	                test_grok_build_clear_string_properties);

	g_test_add_func("/ai-glib/grok-build-client/build-argv/basic",
	                test_grok_build_argv_basic);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/streaming",
	                test_grok_build_argv_streaming);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/skip-permissions",
	                test_grok_build_argv_skip_permissions);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/permission-mode",
	                test_grok_build_argv_permission_mode);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/permission-mode-invalid",
	                test_grok_build_argv_permission_mode_invalid);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/permission-conflict",
	                test_grok_build_argv_permission_conflict);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/rule-lists",
	                test_grok_build_argv_rule_lists);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/rule-lists-empty",
	                test_grok_build_argv_rule_lists_empty_items);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/execution-knobs",
	                test_grok_build_argv_execution_knobs);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/max-turns-unset",
	                test_grok_build_argv_max_turns_unset);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/system-prompt",
	                test_grok_build_argv_system_prompt);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/resume",
	                test_grok_build_argv_resume);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/resume-no-persistence",
	                test_grok_build_argv_resume_no_persistence);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/effort-levels",
	                test_grok_build_argv_effort_levels);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/effort-max-clamped",
	                test_grok_build_argv_effort_max_clamped);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/effort-invalid",
	                test_grok_build_argv_effort_invalid);

	g_test_add_func("/ai-glib/grok-build-client/build-argv/prompt-file-position",
	                test_grok_build_argv_prompt_file_position);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/null-messages",
	                test_grok_build_argv_null_messages);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/empty-model",
	                test_grok_build_argv_empty_model);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/model-verbatim",
	                test_grok_build_argv_model_verbatim);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/empty-system-prompt",
	                test_grok_build_argv_empty_system_prompt);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/empty-session-id",
	                test_grok_build_argv_empty_session_id);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/empty-effort",
	                test_grok_build_argv_empty_effort);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/effort-case",
	                test_grok_build_argv_effort_case);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/permission-mode-default",
	                test_grok_build_argv_permission_mode_default);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/permission-agreement-quiet",
	                test_grok_build_argv_permission_agreement_is_quiet);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/rule-with-spaces",
	                test_grok_build_argv_rule_with_spaces);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/streaming-with-resume",
	                test_grok_build_argv_streaming_with_resume);
	g_test_add_func("/ai-glib/grok-build-client/build-argv/repeatable",
	                test_grok_build_argv_is_repeatable);

	g_test_add_func("/ai-glib/grok-build-client/build-stdin",
	                test_grok_build_stdin);
	g_test_add_func("/ai-glib/grok-build-client/build-stdin/no-messages",
	                test_grok_build_stdin_no_messages);
	g_test_add_func("/ai-glib/grok-build-client/build-stdin/excludes-system-prompt",
	                test_grok_build_stdin_excludes_system_prompt);
	g_test_add_func("/ai-glib/grok-build-client/build-stdin/skips-empty",
	                test_grok_build_stdin_skips_empty_messages);
	g_test_add_func("/ai-glib/grok-build-client/build-stdin/multiple-user",
	                test_grok_build_stdin_multiple_user_messages);

	g_test_add_func("/ai-glib/grok-build-client/parse/success",
	                test_grok_build_parse_success);
	g_test_add_func("/ai-glib/grok-build-client/parse/session-no-persistence",
	                test_grok_build_parse_session_no_persistence);
	g_test_add_func("/ai-glib/grok-build-client/parse/error-object",
	                test_grok_build_parse_error_object);
	g_test_add_func("/ai-glib/grok-build-client/parse/error-trailing-line",
	                test_grok_build_parse_error_trailing_line);
	g_test_add_func("/ai-glib/grok-build-client/parse/result-line-shape",
	                test_grok_build_parse_result_line_shape);
	g_test_add_func("/ai-glib/grok-build-client/parse/no-text",
	                test_grok_build_parse_no_text);
	g_test_add_func("/ai-glib/grok-build-client/parse/malformed",
	                test_grok_build_parse_malformed);
	g_test_add_func("/ai-glib/grok-build-client/parse/empty",
	                test_grok_build_parse_empty);
	g_test_add_func("/ai-glib/grok-build-client/parse/array-root",
	                test_grok_build_parse_array_root);
	g_test_add_func("/ai-glib/grok-build-client/parse/scalar-root",
	                test_grok_build_parse_scalar_root);
	g_test_add_func("/ai-glib/grok-build-client/parse/whitespace-only",
	                test_grok_build_parse_whitespace_only);
	g_test_add_func("/ai-glib/grok-build-client/parse/leading-noise",
	                test_grok_build_parse_leading_noise);
	g_test_add_func("/ai-glib/grok-build-client/parse/error-without-message",
	                test_grok_build_parse_error_without_message);
	g_test_add_func("/ai-glib/grok-build-client/parse/error-alternate-field",
	                test_grok_build_parse_error_alternate_field);
	g_test_add_func("/ai-glib/grok-build-client/parse/result-is-error",
	                test_grok_build_parse_result_is_error);
	g_test_add_func("/ai-glib/grok-build-client/parse/wrong-types",
	                test_grok_build_parse_wrong_types);
	g_test_add_func("/ai-glib/grok-build-client/parse/integer-cost",
	                test_grok_build_parse_integer_cost);
	g_test_add_func("/ai-glib/grok-build-client/parse/stop-reason-max-tokens",
	                test_grok_build_parse_stop_reason_max_tokens);
	g_test_add_func("/ai-glib/grok-build-client/parse/stop-reason-unknown",
	                test_grok_build_parse_stop_reason_unknown);
	g_test_add_func("/ai-glib/grok-build-client/parse/session-update",
	                test_grok_build_parse_session_update);
	g_test_add_func("/ai-glib/grok-build-client/parse/ndjson-last-line",
	                test_grok_build_parse_ndjson_last_line);

	g_test_add_func("/ai-glib/grok-build-client/parse-stream/text-delta",
	                test_grok_build_parse_stream_text_delta);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/thinking-delta",
	                test_grok_build_parse_stream_thinking_delta);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/assistant-ignored",
	                test_grok_build_parse_stream_assistant_ignored);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/system-init",
	                test_grok_build_parse_stream_system_init);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/result",
	                test_grok_build_parse_stream_result);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/result-no-double-text",
	                test_grok_build_parse_stream_result_no_double_text);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/error-line",
	                test_grok_build_parse_stream_error_line);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/noise",
	                test_grok_build_parse_stream_noise);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/missing-event",
	                test_grok_build_parse_stream_missing_event);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/empty-text-delta",
	                test_grok_build_parse_stream_empty_text_delta);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/successive-deltas",
	                test_grok_build_parse_stream_successive_deltas);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/result-is-error",
	                test_grok_build_parse_stream_result_is_error);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/result-no-persistence",
	                test_grok_build_parse_stream_result_no_persistence);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/result-stop-reason",
	                test_grok_build_parse_stream_result_stop_reason);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/result-wrong-types",
	                test_grok_build_parse_stream_result_wrong_types);
	g_test_add_func("/ai-glib/grok-build-client/parse-stream/unknown-shapes",
	                test_grok_build_parse_stream_unknown_shapes);

	return g_test_run();
}
