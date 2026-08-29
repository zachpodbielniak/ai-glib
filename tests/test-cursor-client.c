/*
 * test-cursor-client.c - Unit tests for AiCursorClient
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-cursor-client.h"
#include "providers/ai-cursor-client-internal.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-message.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

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
build_argv_for(AiCursorClient *client, const gchar *system,
			   gboolean streaming)
{
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	gchar **argv;

	argv = ai_cursor_client_build_argv(
		AI_CLI_CLIENT(client), messages, system, 4096, streaming);

	g_list_free_full(messages, g_object_unref);
	return argv;
}

/* ----------------------------------------------------------------
 * Construction and interfaces
 * ---------------------------------------------------------------- */

static void
test_cursor_client_new(void)
{
	g_autoptr(AiCursorClient) client = NULL;

	client = ai_cursor_client_new();
	g_assert_nonnull(client);
	g_assert_true(AI_IS_CURSOR_CLIENT(client));
	g_assert_true(AI_IS_CLI_CLIENT(client));
}

static void
test_cursor_client_default_model(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();

	g_assert_cmpstr(ai_cli_client_get_model(AI_CLI_CLIENT(client)),
					==, AI_CURSOR_DEFAULT_MODEL);
	g_assert_cmpstr(AI_CURSOR_DEFAULT_MODEL, ==, "auto");
}

static void
test_cursor_client_provider_interface(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();

	g_assert_true(AI_IS_PROVIDER(client));
	g_assert_cmpint(ai_provider_get_provider_type(AI_PROVIDER(client)),
					==, AI_PROVIDER_CURSOR);
	g_assert_cmpstr(ai_provider_get_name(AI_PROVIDER(client)),
					==, "Cursor");
	g_assert_cmpstr(ai_provider_get_default_model(AI_PROVIDER(client)),
					==, AI_CURSOR_DEFAULT_MODEL);
}

static void
test_cursor_client_streamable_interface(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();

	g_assert_true(AI_IS_STREAMABLE(client));
}

static void
test_cursor_client_gtype(void)
{
	GType type = ai_cursor_client_get_type();

	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiCursorClient");
}

static void
test_cursor_client_model(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();

	ai_cli_client_set_model(AI_CLI_CLIENT(client),
							AI_CURSOR_MODEL_COMPOSER_2_5);
	g_assert_cmpstr(ai_cli_client_get_model(AI_CLI_CLIENT(client)),
					==, AI_CURSOR_MODEL_COMPOSER_2_5);
}

static void
test_cursor_client_executable_path(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();

	g_assert_null(ai_cli_client_get_executable_path(AI_CLI_CLIENT(client)));

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client),
									  "/usr/local/bin/cursor-agent");
	g_assert_cmpstr(ai_cli_client_get_executable_path(AI_CLI_CLIENT(client)),
					==, "/usr/local/bin/cursor-agent");
}

static void
test_cursor_client_process_timeout(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();

	g_assert_cmpint(
		ai_cli_client_get_process_timeout_ms(AI_CLI_CLIENT(client)),
		==, 1800000);
}

static void
test_cursor_client_properties(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	gboolean skip = FALSE;
	gboolean trust = FALSE;
	gchar *mode = NULL;
	gchar *sandbox = NULL;
	gchar *params = NULL;

	g_assert_false(ai_cursor_client_get_skip_permissions(client));

	ai_cursor_client_set_skip_permissions(client, TRUE);
	g_assert_true(ai_cursor_client_get_skip_permissions(client));

	g_object_set(client,
				 "mode", "plan",
				 "sandbox", "disabled",
				 "trust", TRUE,
				 "model-params", "context=1m,effort=high",
				 "additional-directories", "/tmp/a,/tmp/b",
				 NULL);

	g_object_get(client,
				 "skip-permissions", &skip,
				 "mode", &mode,
				 "sandbox", &sandbox,
				 "trust", &trust,
				 "model-params", &params,
				 NULL);
	g_assert_true(skip);
	g_assert_cmpstr(mode, ==, "plan");
	g_assert_cmpstr(sandbox, ==, "disabled");
	g_assert_true(trust);
	g_assert_cmpstr(params, ==, "context=1m,effort=high");

	g_free(mode);
	g_free(sandbox);
	g_free(params);
}

static void
test_cursor_client_model_defines(void)
{
	g_assert_cmpstr(AI_CURSOR_MODEL_AUTO, ==, "auto");
	g_assert_cmpstr(AI_CURSOR_MODEL_GPT_5_3_CODEX, ==, "gpt-5.3-codex");
	g_assert_cmpstr(AI_CURSOR_MODEL_COMPOSER_2_5, ==, "composer-2.5");
	g_assert_cmpstr(AI_CURSOR_MODEL_CLAUDE_OPUS_5_THINKING_HIGH,
					==, "claude-opus-5-thinking-high");
	g_assert_cmpstr(AI_CURSOR_MODEL_CURSOR_GROK_4_6_HIGH,
					==, "cursor-grok-4.6-high");
	g_assert_cmpstr(AI_CURSOR_MODEL_GEMINI_3_FLASH, ==, "gemini-3-flash");
	g_assert_cmpstr(AI_CURSOR_MODEL_KIMI_K2_7_CODE, ==, "kimi-k2.7-code");
	g_assert_cmpstr(AI_CURSOR_MODEL_LATEST, ==, AI_CURSOR_MODEL_AUTO);
}

/* ----------------------------------------------------------------
 * build_argv
 * ---------------------------------------------------------------- */

static void
test_cursor_argv_basic(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv[0], ==, "cursor-agent");
	g_assert_cmpint(argv_index_of(argv, "--print"), >, 0);
	g_assert_cmpstr(argv_value_after(argv, "--output-format"), ==, "json");
	g_assert_cmpstr(argv_value_after(argv, "--model"),
					==, AI_CURSOR_DEFAULT_MODEL);
	g_assert_cmpint(argv_index_of(argv, "--stream-partial-output"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "hi"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--force"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--api-key"), ==, -1);
}

static void
test_cursor_argv_streaming(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = build_argv_for(client, NULL, TRUE);

	g_assert_cmpstr(argv_value_after(argv, "--output-format"),
					==, "stream-json");
	g_assert_cmpint(argv_index_of(argv, "--stream-partial-output"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--print"), >, 0);
}

static void
test_cursor_argv_skip_permissions(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cursor_client_set_skip_permissions(client, TRUE);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--force"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--yolo"), ==, -1);
}

static void
test_cursor_argv_add_dir(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "additional-directories", " /tmp/a , /tmp/b ", NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_count(argv, "--add-dir"), ==, 2);
	g_assert_cmpint(argv_index_of(argv, "/tmp/a"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "/tmp/b"), >, 0);
	g_assert_cmpint(argv_index_of(argv, " /tmp/a "), ==, -1);
}

static void
test_cursor_argv_add_dir_empty(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "additional-directories", " , ,", NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--add-dir"), ==, -1);
}

static void
test_cursor_argv_knobs(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
				 "mode", "ask",
				 "sandbox", "enabled",
				 "workspace", "/tmp/ws",
				 "endpoint", "https://api.example",
				 "plugin-dirs", "/p1,/p2",
				 "headers", "X-Foo: 1,X-Bar: 2",
				 "auto-review", TRUE,
				 "approve-mcps", TRUE,
				 "trust", TRUE,
				 NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--mode"), ==, "ask");
	g_assert_cmpstr(argv_value_after(argv, "--sandbox"), ==, "enabled");
	g_assert_cmpstr(argv_value_after(argv, "--workspace"), ==, "/tmp/ws");
	g_assert_cmpstr(argv_value_after(argv, "--endpoint"),
					==, "https://api.example");
	g_assert_cmpint(argv_count(argv, "--plugin-dir"), ==, 2);
	g_assert_cmpint(argv_index_of(argv, "/p1"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "/p2"), >, 0);
	g_assert_cmpint(argv_count(argv, "--header"), ==, 2);
	g_assert_cmpint(argv_index_of(argv, "X-Foo: 1"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "X-Bar: 2"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--auto-review"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--approve-mcps"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--trust"), >, 0);
}

static void
test_cursor_argv_mode_invalid(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
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
test_cursor_argv_sandbox_invalid(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "sandbox", "maybe", NULL);

	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE,
						  "*unknown sandbox 'maybe'*");
	argv = build_argv_for(client, NULL, FALSE);
	g_test_assert_expected_messages();

	g_assert_cmpint(argv_index_of(argv, "--sandbox"), ==, -1);
}

static void
test_cursor_argv_model_params(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_model(AI_CLI_CLIENT(client),
							AI_CURSOR_MODEL_CLAUDE_OPUS_4_8_HIGH);
	g_object_set(client, "model-params", "context=1m,effort=high,fast=false",
				 NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--model"),
					==, "claude-opus-4-8-high[context=1m,effort=high,fast=false]");
}

static void
test_cursor_argv_model_params_already_bracketed(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_model(AI_CLI_CLIENT(client),
							"claude-opus-4-8[effort=low]");
	g_object_set(client, "model-params", "context=1m", NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--model"),
					==, "claude-opus-4-8[effort=low]");
}

static void
test_cursor_argv_api_key_not_in_argv(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "api-key", "secret-key", NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--api-key"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "secret-key"), ==, -1);
}

static void
test_cursor_argv_worktree_named(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
				 "worktree", "feature",
				 "worktree-base", "main",
				 "skip-worktree-setup", TRUE,
				 "worktree-auto", TRUE,
				 NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--worktree"), ==, "feature");
	g_assert_cmpstr(argv_value_after(argv, "--worktree-base"), ==, "main");
	g_assert_cmpint(argv_index_of(argv, "--skip-worktree-setup"), >, 0);
}

static void
test_cursor_argv_worktree_auto(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;
	gint i;

	g_object_set(client, "worktree-auto", TRUE, NULL);
	argv = build_argv_for(client, NULL, FALSE);

	i = argv_index_of(argv, "--worktree");
	g_assert_cmpint(i, >, 0);
	/* Optional argument: the next word is another flag, not a name. */
	g_assert_true(argv[i + 1] == NULL || argv[i + 1][0] == '-');
}

static void
test_cursor_argv_worktree_setup_gated(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
				 "skip-worktree-setup", TRUE,
				 "worktree-base", "main",
				 NULL);
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--skip-worktree-setup"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--worktree-base"), ==, -1);
}

static void
test_cursor_argv_resume(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-42");
	argv = build_argv_for(client, "You are terse.", FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--resume"), ==, "sess-42");
	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

static void
test_cursor_argv_resume_no_persistence(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-42");
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	argv = build_argv_for(client, "You are terse.", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--resume"), ==, -1);
}

static void
test_cursor_argv_continue(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	argv = build_argv_for(client, "SYS", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--continue"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--resume"), ==, -1);
}

static void
test_cursor_argv_continue_yields_to_session_id(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-9");
	argv = build_argv_for(client, NULL, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--resume"), ==, "sess-9");
	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

static void
test_cursor_argv_continue_needs_persistence(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	argv = build_argv_for(client, "SYS", FALSE);

	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

/* ----------------------------------------------------------------
 * build_stdin
 * ---------------------------------------------------------------- */

static void
test_cursor_stdin(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
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
	g_assert_nonnull(strstr(stdin_data, "plain text response"));
	g_assert_null(strstr(stdin_data, "{\"event\":\"user\""));

	g_list_free(messages);
}

static void
test_cursor_stdin_system_prompt(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
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
test_cursor_stdin_no_system_on_resume(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
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
parse_json(AiCursorClient *client, const gchar *json, GError **error)
{
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	g_assert_nonnull(klass->parse_json_output);
	return klass->parse_json_output(AI_CLI_CLIENT(client), json, error);
}

static const gchar *CURSOR_JSON_SUCCESS =
	"{\n"
	"  \"type\": \"result\",\n"
	"  \"subtype\": \"success\",\n"
	"  \"is_error\": false,\n"
	"  \"result\": \"A git rebase rewrites history.\\n\",\n"
	"  \"session_id\": \"055a398f-db14-4c5f-abbb-1bf03f8120a7\"\n"
	"}\n";

static void
test_cursor_parse_json_success(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;

	resp = parse_json(client, CURSOR_JSON_SUCCESS, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resp);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "A git rebase rewrites history.\n");

	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
					==, "055a398f-db14-4c5f-abbb-1bf03f8120a7");
}

static void
test_cursor_parse_json_error_flag(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client,
		"{\"type\":\"result\",\"is_error\":true,"
		"\"result\":\"invalid model selection\"}",
		&error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "invalid model selection"));
}

static void
test_cursor_parse_json_ndjson_result(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;
	g_autofree gchar *text = NULL;
	const gchar *ndjson =
		"{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"c1\"}\n"
		"{\"type\":\"assistant\",\"message\":{\"content\":["
		"{\"type\":\"text\",\"text\":\"ignored\"}]}}\n"
		"{\"type\":\"result\",\"is_error\":false,\"result\":\"apple\\n\","
		"\"session_id\":\"c1\"}\n";

	resp = parse_json(client, ndjson, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resp);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "apple\n");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
					==, "c1");
}

static void
test_cursor_parse_json_empty_response(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	resp = parse_json(client,
		"{\"type\":\"result\",\"is_error\":false,\"result\":\"\","
		"\"session_id\":\"s1\"}",
		&error);

	g_assert_no_error(error);
	g_assert_nonnull(resp);
	g_assert_null(ai_response_get_content_blocks(resp));
}

static void
test_cursor_parse_json_garbage(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "not json at all", &error);
	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_cursor_parse_json_no_persistence(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) resp = NULL;

	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	resp = parse_json(client, CURSOR_JSON_SUCCESS, &error);
	g_assert_no_error(error);
	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));
}

static void
test_cursor_parse_json_wrong_type(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	AiResponse *resp;

	resp = parse_json(client, "{\"type\":\"assistant\",\"result\":\"x\"}",
					  &error);
	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

/* ----------------------------------------------------------------
 * parse_stream_line / parse_stream_events
 * ---------------------------------------------------------------- */

static gboolean
parse_stream(AiCursorClient *client, const gchar *line, AiResponse *resp,
			 gchar **delta, GError **error)
{
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	g_assert_nonnull(klass->parse_stream_line);
	return klass->parse_stream_line(AI_CLI_CLIENT(client), line, resp,
									delta, error);
}

static void
test_cursor_parse_stream_text_delta(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"assistant\",\"timestamp_ms\":1,"
		"\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"apple\"}]}}",
		resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(delta, ==, "apple");
}

static void
test_cursor_parse_stream_duplicate_flush(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	/* With --stream-partial-output, events that carry model_call_id are
	 * duplicate flushes, not new text. */
	g_assert_true(parse_stream(client,
		"{\"type\":\"assistant\",\"timestamp_ms\":1,\"model_call_id\":\"m1\","
		"\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"apple\"}]}}",
		resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_null(delta);
}

static void
test_cursor_parse_stream_init(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"system\",\"subtype\":\"init\","
		"\"session_id\":\"c3b66b04\"}",
		resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_null(delta);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
					==, "c3b66b04");
}

static void
test_cursor_parse_stream_result(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;
	g_autofree gchar *text = NULL;

	g_assert_true(parse_stream(client,
		"{\"type\":\"result\",\"is_error\":false,\"result\":\"HELLO\","
		"\"session_id\":\"c1\"}",
		resp, &delta, &error));
	g_assert_no_error(error);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "HELLO");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
					==, "c1");
}

static void
test_cursor_parse_stream_result_no_double_text(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(AiTextContent) already = ai_text_content_new("HELLO");

	ai_response_add_content_block(resp,
		(AiContentBlock *)g_steal_pointer(&already));

	g_assert_true(parse_stream(client,
		"{\"type\":\"result\",\"is_error\":false,\"result\":\"HELLO\"}",
		resp, &delta, &error));
	g_assert_no_error(error);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "HELLO");
}

static void
test_cursor_parse_stream_error_result(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_false(parse_stream(client,
		"{\"type\":\"result\",\"is_error\":true,\"result\":\"nope\"}",
		resp, &delta, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_cmpint(ai_response_get_stop_reason(resp),
					==, AI_STOP_REASON_ERROR);
}

static void
test_cursor_parse_stream_noise(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *delta = NULL;

	g_assert_true(parse_stream(client, "", resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_true(parse_stream(client, "garbage", resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_true(parse_stream(client, "{\"type\":\"future_thing\"}",
							   resp, &delta, &error));
	g_assert_no_error(error);
}

static void
test_cursor_parse_stream_tool(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events =
		g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	gboolean ok;
	AiToolUse *tu;

	ok = klass->parse_stream_events(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_call\",\"subtype\":\"started\","
		"\"call_id\":\"c1\",\"tool_call\":{"
		"\"readToolCall\":{\"args\":{\"path\":\"/tmp/a\"}}}}",
		resp, events, &error);

	g_assert_true(ok);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, ==, 1);
	g_assert_cmpint(ai_event_get_kind(events->pdata[0]),
					==, AI_EVENT_TOOL_STARTED);
	tu = ai_event_get_tool_use(events->pdata[0]);
	g_assert_cmpstr(ai_tool_use_get_name(tu), ==, "read");
	g_assert_cmpstr(ai_tool_use_get_id(tu), ==, "c1");

	ok = klass->parse_stream_events(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_call\",\"subtype\":\"completed\","
		"\"call_id\":\"c1\",\"tool_call\":{"
		"\"readToolCall\":{\"result\":{\"success\":{"
		"\"content\":\"hello\"}}}}}",
		resp, events, &error);

	g_assert_true(ok);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, ==, 2);
	g_assert_cmpint(ai_event_get_kind(events->pdata[1]),
					==, AI_EVENT_TOOL_FINISHED);
}

static void
test_cursor_parse_stream_tool_function(void)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(AiResponse) resp = ai_response_new("", "m");
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) events =
		g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	gboolean ok;
	AiToolUse *tu;

	ok = klass->parse_stream_events(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_call\",\"subtype\":\"started\","
		"\"call_id\":\"c2\",\"tool_call\":{"
		"\"function\":{\"name\":\"Read\",\"arguments\":{\"path\":\"x\"}}}}",
		resp, events, &error);

	g_assert_true(ok);
	g_assert_no_error(error);
	g_assert_cmpuint(events->len, ==, 1);
	tu = ai_event_get_tool_use(events->pdata[0]);
	g_assert_cmpstr(ai_tool_use_get_name(tu), ==, "Read");
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/cursor-client/new",
					test_cursor_client_new);
	g_test_add_func("/ai-glib/cursor-client/default-model",
					test_cursor_client_default_model);
	g_test_add_func("/ai-glib/cursor-client/provider-interface",
					test_cursor_client_provider_interface);
	g_test_add_func("/ai-glib/cursor-client/streamable-interface",
					test_cursor_client_streamable_interface);
	g_test_add_func("/ai-glib/cursor-client/gtype",
					test_cursor_client_gtype);
	g_test_add_func("/ai-glib/cursor-client/model",
					test_cursor_client_model);
	g_test_add_func("/ai-glib/cursor-client/executable-path",
					test_cursor_client_executable_path);
	g_test_add_func("/ai-glib/cursor-client/process-timeout",
					test_cursor_client_process_timeout);
	g_test_add_func("/ai-glib/cursor-client/properties",
					test_cursor_client_properties);
	g_test_add_func("/ai-glib/cursor-client/model-defines",
					test_cursor_client_model_defines);

	g_test_add_func("/ai-glib/cursor-client/build-argv/basic",
					test_cursor_argv_basic);
	g_test_add_func("/ai-glib/cursor-client/build-argv/streaming",
					test_cursor_argv_streaming);
	g_test_add_func("/ai-glib/cursor-client/build-argv/skip-permissions",
					test_cursor_argv_skip_permissions);
	g_test_add_func("/ai-glib/cursor-client/build-argv/add-dir",
					test_cursor_argv_add_dir);
	g_test_add_func("/ai-glib/cursor-client/build-argv/add-dir-empty",
					test_cursor_argv_add_dir_empty);
	g_test_add_func("/ai-glib/cursor-client/build-argv/knobs",
					test_cursor_argv_knobs);
	g_test_add_func("/ai-glib/cursor-client/build-argv/mode-invalid",
					test_cursor_argv_mode_invalid);
	g_test_add_func("/ai-glib/cursor-client/build-argv/sandbox-invalid",
					test_cursor_argv_sandbox_invalid);
	g_test_add_func("/ai-glib/cursor-client/build-argv/model-params",
					test_cursor_argv_model_params);
	g_test_add_func("/ai-glib/cursor-client/build-argv/model-params-bracketed",
					test_cursor_argv_model_params_already_bracketed);
	g_test_add_func("/ai-glib/cursor-client/build-argv/api-key-not-argv",
					test_cursor_argv_api_key_not_in_argv);
	g_test_add_func("/ai-glib/cursor-client/build-argv/worktree-named",
					test_cursor_argv_worktree_named);
	g_test_add_func("/ai-glib/cursor-client/build-argv/worktree-auto",
					test_cursor_argv_worktree_auto);
	g_test_add_func("/ai-glib/cursor-client/build-argv/worktree-gated",
					test_cursor_argv_worktree_setup_gated);
	g_test_add_func("/ai-glib/cursor-client/build-argv/resume",
					test_cursor_argv_resume);
	g_test_add_func("/ai-glib/cursor-client/build-argv/resume-no-persist",
					test_cursor_argv_resume_no_persistence);
	g_test_add_func("/ai-glib/cursor-client/build-argv/continue",
					test_cursor_argv_continue);
	g_test_add_func("/ai-glib/cursor-client/build-argv/continue-yields",
					test_cursor_argv_continue_yields_to_session_id);
	g_test_add_func("/ai-glib/cursor-client/build-argv/continue-persist",
					test_cursor_argv_continue_needs_persistence);

	g_test_add_func("/ai-glib/cursor-client/stdin",
					test_cursor_stdin);
	g_test_add_func("/ai-glib/cursor-client/stdin/system",
					test_cursor_stdin_system_prompt);
	g_test_add_func("/ai-glib/cursor-client/stdin/no-system-resume",
					test_cursor_stdin_no_system_on_resume);

	g_test_add_func("/ai-glib/cursor-client/parse-json/success",
					test_cursor_parse_json_success);
	g_test_add_func("/ai-glib/cursor-client/parse-json/error",
					test_cursor_parse_json_error_flag);
	g_test_add_func("/ai-glib/cursor-client/parse-json/ndjson",
					test_cursor_parse_json_ndjson_result);
	g_test_add_func("/ai-glib/cursor-client/parse-json/empty",
					test_cursor_parse_json_empty_response);
	g_test_add_func("/ai-glib/cursor-client/parse-json/garbage",
					test_cursor_parse_json_garbage);
	g_test_add_func("/ai-glib/cursor-client/parse-json/no-persist",
					test_cursor_parse_json_no_persistence);
	g_test_add_func("/ai-glib/cursor-client/parse-json/wrong-type",
					test_cursor_parse_json_wrong_type);

	g_test_add_func("/ai-glib/cursor-client/parse-stream/text-delta",
					test_cursor_parse_stream_text_delta);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/duplicate-flush",
					test_cursor_parse_stream_duplicate_flush);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/init",
					test_cursor_parse_stream_init);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/result",
					test_cursor_parse_stream_result);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/no-double-text",
					test_cursor_parse_stream_result_no_double_text);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/error",
					test_cursor_parse_stream_error_result);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/noise",
					test_cursor_parse_stream_noise);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/tool",
					test_cursor_parse_stream_tool);
	g_test_add_func("/ai-glib/cursor-client/parse-stream/tool-function",
					test_cursor_parse_stream_tool_function);

	return g_test_run();
}
