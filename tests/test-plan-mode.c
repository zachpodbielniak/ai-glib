/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "providers/ai-claude-tmux-client-internal.h"

static gboolean done;

static void
sent(GObject *source, GAsyncResult *result, gpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	g_assert_true(ai_conversation_send_finish(AI_CONVERSATION(source), result, &error));
	g_assert_no_error(error);
	done = TRUE;
}

static void
send_turn(AiConversation *conversation)
{
	done = FALSE;
	ai_conversation_send_async(conversation, "Investigate the request", NULL, sent, NULL);
	while (!done) g_main_context_iteration(NULL, TRUE);
}

static gint
approve(AiConversation *conversation, AiToolUse *use, gpointer data)
{
	(void)conversation;
	(void)use;
	(*(guint *)data)++;
	return AI_TOOL_APPROVAL_ALLOW;
}

static void
test_local_policy(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(mock));
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	guint approvals = 0;

	ai_conversation_set_stream(conversation, FALSE);
	ai_conversation_set_local_tools(conversation, TRUE);
	ai_conversation_set_system_prompt(conversation, "Keep this instruction.");
	g_signal_connect(conversation, "approval-requested", G_CALLBACK(approve), &approvals);
	g_assert_true(ai_conversation_set_plan_mode(conversation, TRUE, &error));
	g_assert_no_error(error);
	ai_mock_provider_push_tool_use(mock, "write", "{\"path\":\"forbidden.txt\",\"content\":\"bad\"}");
	ai_mock_provider_push_tool_use(mock, "bash", "{\"command\":\"touch forbidden-shell.txt\"}");
	ai_mock_provider_push_tool_use(mock, "task", "{}");
	ai_mock_provider_push_tool_use(mock, "agent_spawn", "{}");
	ai_mock_provider_push_tool_use(mock, "ls", "{\"path\":\".\"}");
	ai_mock_provider_push_text(mock, "Here is the plan.");
	send_turn(conversation);
	g_assert_false(g_file_test("forbidden.txt", G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test("forbidden-shell.txt", G_FILE_TEST_EXISTS));
	g_assert_cmpuint(approvals, ==, 0);
	g_assert_nonnull(strstr(ai_mock_provider_get_last_system_prompt(mock), "[Plan mode]"));
	g_assert_nonnull(strstr(ai_mock_provider_get_last_system_prompt(mock), "Keep this instruction."));
	g_assert_cmpstr(ai_conversation_get_system_prompt(conversation), ==, "Keep this instruction.");
	text = ai_transcript_to_text(ai_conversation_get_transcript(conversation), 0);
	g_assert_nonnull(strstr(text, "Here is the plan."));

	g_assert_true(ai_conversation_set_plan_mode(conversation, FALSE, &error));
	ai_mock_provider_push_tool_use(mock, "write", "{\"path\":\"allowed.txt\",\"content\":\"ok\"}");
	ai_mock_provider_push_text(mock, "Implemented.");
	send_turn(conversation);
	g_assert_true(g_file_test("allowed.txt", G_FILE_TEST_EXISTS));
	g_assert_cmpuint(approvals, ==, 1);
	g_assert_cmpstr(ai_mock_provider_get_last_system_prompt(mock), ==, "Keep this instruction.");
	g_assert_cmpint(g_unlink("allowed.txt"), ==, 0);
}

static void
test_busy(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(mock));
	g_autoptr(GError) error = NULL;
	ai_conversation_set_stream(conversation, FALSE);
	ai_mock_provider_set_delay_ms(mock, 20);
	done = FALSE;
	ai_conversation_send_async(conversation, "hello", NULL, sent, NULL);
	g_assert_false(ai_conversation_set_plan_mode(conversation, TRUE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_false(ai_conversation_get_plan_mode(conversation));
	while (!done) g_main_context_iteration(NULL, TRUE);
}

static const gchar *
flag_value(gchar **argv, const gchar *flag)
{
	guint i;
	for (i = 0; argv[i] != NULL; i++)
		if (g_str_equal(argv[i], flag)) return argv[i + 1];
	return NULL;
}

static void
test_native(void)
{
	static const struct {
		const gchar *provider, *property, *previous, *flag, *value;
	} cases[] = {
		{ "claude-code", "permission-mode", "acceptEdits", "--permission-mode", "plan" },
		{ "grok-build", "permission-mode", "acceptEdits", "--permission-mode", "plan" },
		{ "opencode", "agent", "reviewer", "--agent", "plan" },
		{ "antigravity", "mode", "accept-edits", "--mode", "plan" },
		{ "cursor", "mode", "ask", "--mode", "plan" },
		{ "codex-cli", "sandbox", "workspace-write", "--sandbox", "read-only" },
		{ "claude-tmux", "permission-mode", "acceptEdits", "--permission-mode", "plan" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GObject) provider = ai_provider_factory_new_from_string(cases[i].provider, NULL, &error);
		g_autoptr(AiConversation) conversation = NULL;
		g_autoptr(AiMockProvider) replacement = ai_mock_provider_new();
		g_autoptr(AiMessage) message = ai_message_new_user("Plan a change");
		g_autofree gchar *value = NULL;
		GList messages = { message, NULL, NULL };
		gboolean skip = TRUE;
		guint stream;
		g_assert_no_error(error);
		g_object_set(provider, cases[i].property, cases[i].previous, "skip-permissions", TRUE, NULL);
		conversation = ai_conversation_new(provider);
		g_assert_true(ai_conversation_set_plan_mode(conversation, TRUE, &error));
		g_object_get(provider, cases[i].property, &value, "skip-permissions", &skip, NULL);
		g_assert_cmpstr(value, ==, cases[i].value);
		g_assert_false(skip);
		for (stream = 0; stream < 2; stream++)
		{
			g_auto(GStrv) argv = NULL;
			if (AI_IS_CLAUDE_TMUX_CLIENT(provider))
			{
				g_autoptr(GPtrArray) args = ai_claude_tmux_client_build_session_argv(
					"tmux", "socket", "session", ".", "claude", stream, "id",
					"settings", NULL, NULL, skip, NULL, value);
				argv = (gchar **)g_ptr_array_free(g_steal_pointer(&args), FALSE);
			}
			else
				argv = AI_CLI_CLIENT_GET_CLASS(provider)->build_argv(AI_CLI_CLIENT(provider), &messages, NULL, 100, stream);
			g_assert_nonnull(argv);
			g_assert_cmpstr(flag_value(argv, cases[i].flag), ==, cases[i].value);
			g_assert_false(g_strv_contains((const gchar * const *)argv, "--auto"));
			g_assert_false(g_strv_contains((const gchar * const *)argv, "--force"));
			g_assert_false(g_strv_contains((const gchar * const *)argv, "--dangerously-skip-permissions"));
		}
		g_clear_pointer(&value, g_free);
		/* Switching restores the outgoing provider and retains plan intent. */
		g_assert_true(ai_conversation_set_provider(conversation, G_OBJECT(replacement), &error));
		g_assert_true(ai_conversation_get_plan_mode(conversation));
		g_object_get(provider, cases[i].property, &value, "skip-permissions", &skip, NULL);
		g_assert_cmpstr(value, ==, cases[i].previous);
		g_assert_true(skip);
		g_assert_true(ai_conversation_set_provider(conversation, provider, &error));
		g_assert_true(ai_conversation_set_plan_mode(conversation, FALSE, &error));
		g_clear_pointer(&value, g_free);
		g_object_get(provider, cases[i].property, &value, "skip-permissions", &skip, NULL);
		g_assert_cmpstr(value, ==, cases[i].previous);
		g_assert_true(skip);
	}
}

static void
test_commands_and_fork(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiMockProvider) other = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiConversation) branch = NULL;
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiResource) resource = ai_resource_new_from_data(
		"---\nshell: true\n---\n!`touch command-ran.txt`\n", -1,
		"inspect", AI_RESOURCE_COMMAND, "claude", AI_RESOURCE_SCOPE_USER, NULL);
	g_autoptr(AiCommandSet) commands = NULL;
	g_autoptr(AiCommandResult) result = NULL;
	g_autoptr(GError) error = NULL;

	ai_resource_registry_add(registry, resource);
	commands = ai_command_set_new(registry);
	ai_conversation_set_command_set(conversation, commands);
	g_assert_true(ai_conversation_set_plan_mode(conversation, TRUE, &error));
	result = ai_conversation_resolve_input(conversation, "/inspect", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_false(g_file_test("command-ran.txt", G_FILE_TEST_EXISTS));
	g_assert_nonnull(strstr(ai_command_result_get_prompt(result), "touch command-ran.txt"));
	g_assert_cmpint(ai_command_set_get_shell_policy(commands), ==, AI_COMMAND_SHELL_OPT_IN);
	branch = ai_conversation_fork(conversation, G_OBJECT(other), &error);
	g_assert_no_error(error);
	g_assert_true(ai_conversation_get_plan_mode(branch));
	g_assert_true(ai_conversation_set_plan_mode(conversation, FALSE, &error));
	g_clear_object(&result);
	result = ai_conversation_resolve_input(conversation, "/inspect", NULL, &error);
	g_assert_no_error(error);
	g_assert_true(g_file_test("command-ran.txt", G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_unlink("command-ran.txt"), ==, 0);
}

static void
test_tmux_property_lifetime(void)
{
	g_autoptr(AiClaudeTmuxClient) provider = ai_claude_tmux_client_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autofree gchar *mode = NULL;
	g_object_set(provider, "permission-mode", "acceptEdits", NULL);
	g_object_set(provider, "tmux-path", "/usr/bin/tmux", NULL);
	conversation = ai_conversation_new(G_OBJECT(provider));
	g_assert_true(ai_conversation_set_plan_mode(conversation, TRUE, NULL));
	g_clear_object(&conversation);
	g_object_get(provider, "permission-mode", &mode, NULL);
	g_assert_cmpstr(mode, ==, "acceptEdits");
}

static void
test_native_prompt(void)
{
	g_autoptr(AiGrokBuildClient) provider = ai_grok_build_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autofree gchar *directory = g_get_current_dir();
	g_autofree gchar *script = g_build_filename(directory, "grok-stub", NULL);
	g_autofree gchar *args = NULL;
	const gchar *body =
		"#!/bin/sh\nprintf '%s\\n' \"$@\" > args.txt\ncat >/dev/null\n"
		"printf '%s\\n' '{\"text\":\"planned\",\"stopReason\":\"end_turn\"}'\n";

	g_assert_true(g_file_set_contents(script, body, -1, NULL));
	g_assert_cmpint(g_chmod(script, 0700), ==, 0);
	ai_cli_client_set_executable_path(AI_CLI_CLIENT(provider), script);
	ai_cli_client_set_system_prompt(AI_CLI_CLIENT(provider), "Preserve provider instructions.");
	ai_conversation_set_stream(conversation, FALSE);
	g_assert_true(ai_conversation_set_plan_mode(conversation, TRUE, NULL));
	send_turn(conversation);
	g_assert_true(g_file_get_contents("args.txt", &args, NULL, NULL));
	g_assert_nonnull(strstr(args, "Preserve provider instructions."));
	g_assert_nonnull(strstr(args, "[Plan mode]"));
	g_assert_cmpint(g_unlink("args.txt"), ==, 0);
	g_assert_cmpint(g_unlink(script), ==, 0);
}

int
main(int argc, char **argv)
{
	g_autofree gchar *sandbox = g_dir_make_tmp("ai-plan-XXXXXX", NULL);
	g_autofree gchar *cwd = g_get_current_dir();
	gint result;
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_assert_cmpint(g_chdir(sandbox), ==, 0);
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/plan/local-policy", test_local_policy);
	g_test_add_func("/plan/busy", test_busy);
	g_test_add_func("/plan/native", test_native);
	g_test_add_func("/plan/commands-fork", test_commands_and_fork);
	g_test_add_func("/plan/tmux-lifetime", test_tmux_property_lifetime);
	g_test_add_func("/plan/native-prompt", test_native_prompt);
	result = g_test_run();
	g_assert_cmpint(g_chdir(cwd), ==, 0);
	g_rmdir(sandbox);
	return result;
}
