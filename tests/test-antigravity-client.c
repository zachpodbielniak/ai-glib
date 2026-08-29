/*
 * test-antigravity-client.c - Unit tests for AiAntigravityClient
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-antigravity-client.h"
#include "providers/ai-antigravity-client-internal.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-message.h"
#include "model/ai-text-content.h"

/* ----------------------------------------------------------------
 * argv helpers
 * ---------------------------------------------------------------- */

static gint
argv_index_of(gchar **argv, const gchar *needle)
{
	gint i;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			return i;
	return -1;
}

static gint
argv_count(gchar **argv, const gchar *needle)
{
	gint i, n = 0;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			n++;
	return n;
}

static const gchar *
argv_value_after(gchar **argv, const gchar *needle)
{
	gint i = argv_index_of(argv, needle);

	if (i < 0 || argv[i + 1] == NULL)
		return NULL;

	return argv[i + 1];
}

static gchar **
build_argv_for(AiAntigravityClient *client, const gchar *system,
	       gboolean streaming)
{
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	gchar **argv;

	argv = ai_antigravity_client_build_argv(
		AI_CLI_CLIENT(client), messages, system, 4096, streaming);

	g_list_free_full(messages, g_object_unref);
	return argv;
}

/* ----------------------------------------------------------------
 * Construction and interfaces
 * ---------------------------------------------------------------- */

static void
test_antigravity_client_new(void)
{
	g_autoptr(AiAntigravityClient) client = NULL;

	client = ai_antigravity_client_new();
	g_assert_nonnull(client);
	g_assert_true(AI_IS_ANTIGRAVITY_CLIENT(client));
	g_assert_true(AI_IS_CLI_CLIENT(client));
}

static void
test_antigravity_client_default_model(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();

	g_assert_cmpstr(ai_cli_client_get_model(AI_CLI_CLIENT(client)),
			==, AI_ANTIGRAVITY_DEFAULT_MODEL);
}

static void
test_antigravity_client_provider_interface(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();

	g_assert_true(AI_IS_PROVIDER(client));
	g_assert_cmpint(ai_provider_get_provider_type(AI_PROVIDER(client)),
			==, AI_PROVIDER_ANTIGRAVITY);
	g_assert_cmpstr(ai_provider_get_name(AI_PROVIDER(client)),
			==, "Antigravity");
	g_assert_cmpstr(ai_provider_get_default_model(AI_PROVIDER(client)),
			==, AI_ANTIGRAVITY_DEFAULT_MODEL);
}

static void
test_antigravity_client_streamable_interface(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();

	g_assert_true(AI_IS_STREAMABLE(client));
}

static void
test_antigravity_client_gtype(void)
{
	GType type = ai_antigravity_client_get_type();

	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiAntigravityClient");
}

static void
test_antigravity_client_model(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();

	ai_cli_client_set_model(AI_CLI_CLIENT(client),
				AI_ANTIGRAVITY_MODEL_CLAUDE_SONNET_4_6);
	g_assert_cmpstr(ai_cli_client_get_model(AI_CLI_CLIENT(client)),
			==, AI_ANTIGRAVITY_MODEL_CLAUDE_SONNET_4_6);
}

static void
test_antigravity_client_executable_path(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();

	g_assert_null(ai_cli_client_get_executable_path(AI_CLI_CLIENT(client)));

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client),
					  "/usr/local/bin/agy");
	g_assert_cmpstr(ai_cli_client_get_executable_path(AI_CLI_CLIENT(client)),
			==, "/usr/local/bin/agy");
}

static void
test_antigravity_client_process_timeout(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();

	g_assert_cmpint(
		ai_cli_client_get_process_timeout_ms(AI_CLI_CLIENT(client)),
		==, 1800000);
}

static void
test_antigravity_client_properties(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	gboolean skip = FALSE;
	gboolean slash = FALSE;
	gboolean sandbox = FALSE;
	gchar *agent = NULL;
	gchar *mode = NULL;

	g_assert_false(ai_antigravity_client_get_skip_permissions(client));

	g_object_get(client,
		     "disable-slash-commands", &slash,
		     "sandbox", &sandbox,
		     NULL);
	g_assert_true(slash);
	g_assert_false(sandbox);

	ai_antigravity_client_set_skip_permissions(client, TRUE);
	g_assert_true(ai_antigravity_client_get_skip_permissions(client));

	g_object_set(client,
		     "agent", "reviewer",
		     "mode", "plan",
		     "sandbox", TRUE,
		     "additional-directories", "/tmp/a,/tmp/b",
		     NULL);

	g_object_get(client,
		     "skip-permissions", &skip,
		     "agent", &agent,
		     "mode", &mode,
		     "sandbox", &sandbox,
		     NULL);
	g_assert_true(skip);
	g_assert_cmpstr(agent, ==, "reviewer");
	g_assert_cmpstr(mode, ==, "plan");
	g_assert_true(sandbox);

	g_free(agent);
	g_free(mode);
}

/* ----------------------------------------------------------------
 * build_argv
 * ---------------------------------------------------------------- */

static void
test_antigravity_argv_basic(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv[0], ==, "agy");
	g_assert_cmpstr(argv_value_after(argv, "--input-format"),
			==, "stream-json");
	g_assert_cmpstr(argv_value_after(argv, "--output-format"),
			==, "stream-json");
	g_assert_cmpstr(argv_value_after(argv, "--model"),
			==, AI_ANTIGRAVITY_DEFAULT_MODEL);
	g_assert_cmpstr(argv_value_after(argv, "--print-timeout"), ==, "30m");
	g_assert_cmpint(argv_index_of(argv, "--disable-slash-commands"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--print"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "hi"), ==, -1);
}

static void
test_antigravity_argv_streaming_same_format(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, TRUE);

	/* stream-json input requires stream-json output either way. */
	g_assert_cmpstr(argv_value_after(argv, "--output-format"),
			==, "stream-json");
}

static void
test_antigravity_argv_skip_permissions(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	ai_antigravity_client_set_skip_permissions(client, TRUE);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--dangerously-skip-permissions"),
			>, 0);
}

static void
test_antigravity_argv_add_dir(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "additional-directories", " /tmp/a , /tmp/b ", NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_count(argv, "--add-dir"), ==, 2);
	g_assert_cmpint(argv_index_of(argv, "/tmp/a"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "/tmp/b"), >, 0);
	g_assert_cmpint(argv_index_of(argv, " /tmp/a "), ==, -1);
}

static void
test_antigravity_argv_add_dir_empty(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "additional-directories", " , ,", NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--add-dir"), ==, -1);
}

static void
test_antigravity_argv_knobs(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
		     "agent", "reviewer",
		     "mode", "plan",
		     "json-schema", "{\"type\":\"string\"}",
		     "log-file", "/tmp/agy.log",
		     "project", "demo",
		     "new-project", TRUE,
		     "sandbox", TRUE,
		     "print-timeout", "15m",
		     NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--agent"), ==, "reviewer");
	g_assert_cmpstr(argv_value_after(argv, "--mode"), ==, "plan");
	g_assert_cmpstr(argv_value_after(argv, "--json-schema"),
			==, "{\"type\":\"string\"}");
	g_assert_cmpstr(argv_value_after(argv, "--log-file"), ==, "/tmp/agy.log");
	g_assert_cmpstr(argv_value_after(argv, "--project"), ==, "demo");
	g_assert_cmpstr(argv_value_after(argv, "--print-timeout"), ==, "15m");
	g_assert_cmpint(argv_index_of(argv, "--new-project"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--sandbox"), >, 0);
}

static void
test_antigravity_argv_mode_invalid(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "mode", "yolo", NULL);

	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
			      "*unknown mode 'yolo'*");
	argv = build_argv_for(client, NULL, FALSE);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--mode"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "yolo"), ==, -1);
}

static void
test_antigravity_argv_slash_commands_off(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "disable-slash-commands", FALSE, NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--disable-slash-commands"), ==, -1);
}

static void
test_antigravity_argv_resume(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-42");
	argv = build_argv_for(client, "You are terse.", FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--conversation"), ==, "sess-42");
	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

static void
test_antigravity_argv_resume_no_persistence(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-42");
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	argv = build_argv_for(client, "You are terse.", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--conversation"), ==, -1);
}

static void
test_antigravity_argv_continue(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	argv = build_argv_for(client, "SYS", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--continue"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--conversation"), ==, -1);
}

static void
test_antigravity_argv_continue_yields_to_session_id(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-9");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--conversation"), ==, "sess-9");
	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

static void
test_antigravity_argv_continue_needs_persistence(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	argv = build_argv_for(client, "SYS", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

static void
test_antigravity_argv_effort_levels(void)
{
	static const gchar *levels[] = { "low", "medium", "high" };
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(levels); i++)
	{
		g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
		g_auto(GStrv) argv = NULL;

		ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), levels[i]);
		argv = build_argv_for(client, NULL, FALSE);

		g_assert_cmpstr(argv_value_after(argv, "--effort"), ==, levels[i]);
	}
}

static void
test_antigravity_argv_effort_max_clamped(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "max");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--effort"), ==, "high");
}

static void
test_antigravity_argv_effort_xhigh_clamped(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "xhigh");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--effort"), ==, "high");
}

static void
test_antigravity_argv_effort_invalid(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_effort_level(AI_CLI_CLIENT(client), "turbo");

	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
			      "*unknown effort level 'turbo'*");
	argv = build_argv_for(client, NULL, FALSE);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--effort"), ==, -1);
}

/* ----------------------------------------------------------------
 * build_stdin
 * ---------------------------------------------------------------- */

static void
test_antigravity_stdin(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
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
	g_assert_true(g_str_has_prefix(stdin_data, "{\"event\":\"user\""));
	g_assert_nonnull(strstr(stdin_data, "First question"));
	g_assert_nonnull(strstr(stdin_data, "Previous assistant response: "
					    "An answer"));
	g_assert_true(g_str_has_suffix(stdin_data, "\n"));

	g_list_free(messages);
}

static void
test_antigravity_stdin_system_prompt(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_auto(GStrv) argv = NULL;
	g_autoptr(AiMessage) user = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, user);
	g_autofree gchar *stdin_data = NULL;

	argv = build_argv_for(client, "Be terse.", FALSE);
	(void)argv;

	stdin_data = klass->build_stdin(AI_CLI_CLIENT(client), messages);
	g_assert_nonnull(strstr(stdin_data, "<system>"));
	g_assert_nonnull(strstr(stdin_data, "Be terse."));

	g_list_free(messages);
}

static void
test_antigravity_stdin_no_system_on_resume(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_auto(GStrv) argv = NULL;
	g_autoptr(AiMessage) user = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, user);
	g_autofree gchar *stdin_data = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-1");
	argv = build_argv_for(client, "Be terse.", FALSE);
	(void)argv;

	stdin_data = klass->build_stdin(AI_CLI_CLIENT(client), messages);
	g_assert_null(strstr(stdin_data, "<system>"));
	g_assert_null(strstr(stdin_data, "Be terse."));

	g_list_free(messages);
}

/* ----------------------------------------------------------------
 * parse_json_output
 * ---------------------------------------------------------------- */

static AiResponse *
parse_json(AiAntigravityClient *client, const gchar *json, GError **error)
{
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	g_assert_nonnull(klass->parse_json_output);
	return klass->parse_json_output(AI_CLI_CLIENT(client), json, error);
}

static const gchar *AGY_JSON_SUCCESS =
	"{\n"
	"  \"conversation_id\": \"055a398f-db14-4c5f-abbb-1bf03f8120a7\",\n"
	"  \"status\": \"SUCCESS\",\n"
	"  \"response\": \"A git rebase rewrites history.\\n\",\n"
	"  \"duration_seconds\": 7.16,\n"
	"  \"num_turns\": 1,\n"
	"  \"usage\": {\n"
	"    \"input_tokens\": 10415,\n"
	"    \"output_tokens\": 657,\n"
	"    \"thinking_tokens\": 616,\n"
	"    \"cache_read_tokens\": 8113,\n"
	"    \"total_tokens\": 11072\n"
	"  }\n"
	"}\n";

static void
test_antigravity_parse_json_success(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;
	AiUsage *usage;

	resp = parse_json(client, AGY_JSON_SUCCESS, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resp);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "A git rebase rewrites history.\n");

	usage = ai_response_get_usage(resp);
	g_assert_nonnull(usage);
	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 10415);
	g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 657);

	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
			==, "055a398f-db14-4c5f-abbb-1bf03f8120a7");
}

static void
test_antigravity_parse_json_error_status(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client,
		"{\"conversation_id\":\"\",\"status\":\"ERROR\","
		"\"response\":\"\",\"error\":\"invalid model selection\"}",
		&error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "invalid model selection"));
}

static void
test_antigravity_parse_json_stream_result(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;
	const gchar *ndjson =
		"{\"event\":\"init\",\"conversation_id\":\"c1\",\"init\":{}}\n"
		"{\"event\":\"step_update\",\"step_update\":{"
		"\"conversation_id\":\"c1\",\"step_index\":0,"
		"\"state\":\"DONE\",\"step_type\":\"user_input\"}}\n"
		"{\"event\":\"result\",\"result\":{"
		"\"conversation_id\":\"c1\",\"status\":\"SUCCESS\","
		"\"response\":\"apple\\n\","
		"\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n";

	resp = parse_json(client, ndjson, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resp);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "apple\n");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
			==, "c1");
}

static void
test_antigravity_parse_json_empty_response(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	resp = parse_json(client,
		"{\"conversation_id\":\"s1\",\"status\":\"SUCCESS\","
		"\"response\":\"\"}",
		&error);

	g_assert_no_error(error);
	g_assert_nonnull(resp);
	g_assert_null(ai_response_get_content_blocks(resp));
}

static void
test_antigravity_parse_json_garbage(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "not json at all", &error);
	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_antigravity_parse_json_no_persistence(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	resp = parse_json(client, AGY_JSON_SUCCESS, &error);
	g_assert_no_error(error);
	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));
}

/* ----------------------------------------------------------------
 * parse_stream_line
 * ---------------------------------------------------------------- */

static gboolean
parse_stream(AiAntigravityClient *client, const gchar *line, AiResponse *resp,
	     gchar **delta, GError **error)
{
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	g_assert_nonnull(klass->parse_stream_line);
	return klass->parse_stream_line(AI_CLI_CLIENT(client), line, resp,
					delta, error);
}

static void
test_antigravity_parse_stream_text_delta(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_true(parse_stream(client,
		"{\"event\":\"step_update\",\"step_update\":{"
		"\"step_index\":2,\"state\":\"ACTIVE\","
		"\"step_type\":\"agent_response\",\"text_delta\":\"apple\"}}",
		resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(delta, ==, "apple");
}

static void
test_antigravity_parse_stream_thinking_delta(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_true(parse_stream(client,
		"{\"event\":\"step_update\",\"step_update\":{"
		"\"step_index\":2,\"state\":\"ACTIVE\","
		"\"step_type\":\"agent_response\","
		"\"thinking_delta\":\"hmm\"}}",
		resp, &delta, &error));
	g_assert_no_error(error);
	/* thinking is not folded into the text delta. */
	g_assert_null(delta);
}

static void
test_antigravity_parse_stream_init(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_true(parse_stream(client,
		"{\"event\":\"init\",\"conversation_id\":\"c3b66b04\","
		"\"init\":{\"cwd\":\"/tmp\"}}",
		resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_null(delta);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
			==, "c3b66b04");
}

static void
test_antigravity_parse_stream_result(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;
	g_autofree gchar *text = NULL;
	AiUsage *usage;

	g_assert_true(parse_stream(client,
		"{\"event\":\"result\",\"result\":{"
		"\"conversation_id\":\"c1\",\"status\":\"SUCCESS\","
		"\"response\":\"HELLO\","
		"\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}}",
		resp, &delta, &error));
	g_assert_no_error(error);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "HELLO");
	usage = ai_response_get_usage(resp);
	g_assert_nonnull(usage);
	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 5);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
			==, "c1");
}

static void
test_antigravity_parse_stream_result_no_double_text(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(AiTextContent) already = ai_text_content_new("HELLO");

	ai_response_add_content_block(resp,
		(AiContentBlock *)g_steal_pointer(&already));

	g_assert_true(parse_stream(client,
		"{\"event\":\"result\",\"result\":{"
		"\"status\":\"SUCCESS\",\"response\":\"HELLO\"}}",
		resp, &delta, &error));
	g_assert_no_error(error);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "HELLO");
}

static void
test_antigravity_parse_stream_error_result(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_false(parse_stream(client,
		"{\"event\":\"result\",\"result\":{"
		"\"status\":\"ERROR\",\"error\":\"nope\"}}",
		resp, &delta, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_cmpint(ai_response_get_stop_reason(resp),
			==, AI_STOP_REASON_ERROR);
}

static void
test_antigravity_parse_stream_noise(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_true(parse_stream(client, "", resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_true(parse_stream(client, "garbage", resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_true(parse_stream(client, "{\"event\":\"future_thing\"}",
				   resp, &delta, &error));
	g_assert_no_error(error);
}

static void
test_antigravity_parse_stream_tool(void)
{
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events =
		g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	gboolean ok;

	ok = klass->parse_stream_events(AI_CLI_CLIENT(client),
		"{\"event\":\"step_update\",\"step_update\":{"
		"\"step_index\":4,\"state\":\"DONE\",\"step_type\":\"tool\","
		"\"tool_name\":\"run_command\","
		"\"tool_info\":{\"name\":\"run_command\","
		"\"parameters\":{\"CommandLine\":\"echo hi\"},"
		"\"output\":\"hi\\n\"}}}",
		resp, events, &error);

	g_assert_true(ok);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, >=, 2);
	g_assert_cmpint(ai_event_get_kind(events->pdata[0]),
			==, AI_EVENT_TOOL_STARTED);
	g_assert_cmpint(ai_event_get_kind(events->pdata[events->len - 1]),
			==, AI_EVENT_TOOL_FINISHED);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/antigravity-client/new",
			test_antigravity_client_new);
	g_test_add_func("/ai-glib/antigravity-client/default-model",
			test_antigravity_client_default_model);
	g_test_add_func("/ai-glib/antigravity-client/provider-interface",
			test_antigravity_client_provider_interface);
	g_test_add_func("/ai-glib/antigravity-client/streamable-interface",
			test_antigravity_client_streamable_interface);
	g_test_add_func("/ai-glib/antigravity-client/gtype",
			test_antigravity_client_gtype);
	g_test_add_func("/ai-glib/antigravity-client/model",
			test_antigravity_client_model);
	g_test_add_func("/ai-glib/antigravity-client/executable-path",
			test_antigravity_client_executable_path);
	g_test_add_func("/ai-glib/antigravity-client/process-timeout",
			test_antigravity_client_process_timeout);
	g_test_add_func("/ai-glib/antigravity-client/properties",
			test_antigravity_client_properties);

	g_test_add_func("/ai-glib/antigravity-client/build-argv/basic",
			test_antigravity_argv_basic);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/streaming",
			test_antigravity_argv_streaming_same_format);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/skip-permissions",
			test_antigravity_argv_skip_permissions);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/add-dir",
			test_antigravity_argv_add_dir);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/add-dir-empty",
			test_antigravity_argv_add_dir_empty);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/knobs",
			test_antigravity_argv_knobs);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/mode-invalid",
			test_antigravity_argv_mode_invalid);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/slash-off",
			test_antigravity_argv_slash_commands_off);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/resume",
			test_antigravity_argv_resume);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/resume-no-persist",
			test_antigravity_argv_resume_no_persistence);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/continue",
			test_antigravity_argv_continue);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/continue-yields",
			test_antigravity_argv_continue_yields_to_session_id);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/continue-persist",
			test_antigravity_argv_continue_needs_persistence);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/effort",
			test_antigravity_argv_effort_levels);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/effort-max",
			test_antigravity_argv_effort_max_clamped);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/effort-xhigh",
			test_antigravity_argv_effort_xhigh_clamped);
	g_test_add_func("/ai-glib/antigravity-client/build-argv/effort-invalid",
			test_antigravity_argv_effort_invalid);

	g_test_add_func("/ai-glib/antigravity-client/stdin",
			test_antigravity_stdin);
	g_test_add_func("/ai-glib/antigravity-client/stdin/system",
			test_antigravity_stdin_system_prompt);
	g_test_add_func("/ai-glib/antigravity-client/stdin/no-system-resume",
			test_antigravity_stdin_no_system_on_resume);

	g_test_add_func("/ai-glib/antigravity-client/parse-json/success",
			test_antigravity_parse_json_success);
	g_test_add_func("/ai-glib/antigravity-client/parse-json/error",
			test_antigravity_parse_json_error_status);
	g_test_add_func("/ai-glib/antigravity-client/parse-json/stream-result",
			test_antigravity_parse_json_stream_result);
	g_test_add_func("/ai-glib/antigravity-client/parse-json/empty",
			test_antigravity_parse_json_empty_response);
	g_test_add_func("/ai-glib/antigravity-client/parse-json/garbage",
			test_antigravity_parse_json_garbage);
	g_test_add_func("/ai-glib/antigravity-client/parse-json/no-persist",
			test_antigravity_parse_json_no_persistence);

	g_test_add_func("/ai-glib/antigravity-client/parse-stream/text-delta",
			test_antigravity_parse_stream_text_delta);
	g_test_add_func("/ai-glib/antigravity-client/parse-stream/thinking",
			test_antigravity_parse_stream_thinking_delta);
	g_test_add_func("/ai-glib/antigravity-client/parse-stream/init",
			test_antigravity_parse_stream_init);
	g_test_add_func("/ai-glib/antigravity-client/parse-stream/result",
			test_antigravity_parse_stream_result);
	g_test_add_func("/ai-glib/antigravity-client/parse-stream/no-double",
			test_antigravity_parse_stream_result_no_double_text);
	g_test_add_func("/ai-glib/antigravity-client/parse-stream/error",
			test_antigravity_parse_stream_error_result);
	g_test_add_func("/ai-glib/antigravity-client/parse-stream/noise",
			test_antigravity_parse_stream_noise);
	g_test_add_func("/ai-glib/antigravity-client/parse-stream/tool",
			test_antigravity_parse_stream_tool);

	return g_test_run();
}
