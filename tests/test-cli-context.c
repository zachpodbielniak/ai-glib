/*
 * test-cli-context.c - Canonical message projection for CLI providers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <ai-glib.h>

#include "core/ai-cli-client-private.h"
#include "providers/ai-claude-tmux-client-internal.h"
#include "providers/ai-opencode-client-internal.h"

#include <string.h>

static GList *
structured_messages(void)
{
	GList *messages = NULL;
	g_autoptr(AiMessage) user = ai_message_new_user("Inspect the file.");
	g_autoptr(AiMessage) assistant = ai_message_new(AI_ROLE_ASSISTANT);
	g_autoptr(AiTextContent) text = ai_text_content_new("I will inspect it.");
	g_autoptr(AiToolUse) use =
		ai_tool_use_new_from_json_string("call-7", "read",
		                                 "{\"path\":\"résumé.txt\"}");
	g_autoptr(AiMessage) result =
		ai_message_new_tool_result_with_name(
			"call-7", "read", "line one\nline two", FALSE);
	g_autoptr(AiMessage) follow_up = ai_message_new_user("Summarize it.");

	ai_message_add_content_block(
		assistant, (AiContentBlock *)g_steal_pointer(&text));
	ai_message_add_content_block(
		assistant, (AiContentBlock *)g_steal_pointer(&use));

	messages = g_list_append(messages, g_steal_pointer(&user));
	messages = g_list_append(messages, g_steal_pointer(&assistant));
	messages = g_list_append(messages, g_steal_pointer(&result));
	messages = g_list_append(messages, g_steal_pointer(&follow_up));

	return messages;
}

static void
assert_projection(const gchar *prompt)
{
	g_assert_nonnull(prompt);
	g_assert_nonnull(strstr(prompt, "Inspect the file."));
	g_assert_nonnull(strstr(prompt, "Previous assistant response:"));
	g_assert_nonnull(strstr(prompt, "Tool call: read; id=call-7"));
	g_assert_nonnull(strstr(prompt, "Tool result: read; id=call-7; error=false"));
	g_assert_nonnull(strstr(prompt, "line one"));
	g_assert_nonnull(strstr(prompt, "Summarize it."));
	g_assert_nonnull(strstr(prompt, "résumé.txt"));
}

static void
test_text_only_projection_is_stable(void)
{
	g_autoptr(AiMessage) user = ai_message_new_user("hello");
	g_autoptr(AiMessage) assistant = ai_message_new_assistant("world");
	g_autofree gchar *user_text = ai_cli_client_project_message(user);
	g_autofree gchar *assistant_text =
		ai_cli_client_project_message(assistant);

	g_assert_cmpstr(user_text, ==, "hello");
	g_assert_cmpstr(assistant_text, ==,
	                "Previous assistant response: world");
}

static void
test_image_projection_is_explicit(void)
{
	static const guint8 data[] = { 0x89, 0x50, 0x4e, 0x47 };
	g_autoptr(GBytes) bytes = g_bytes_new_static(data, sizeof data);
	g_autoptr(AiImageContent) image =
		ai_image_content_new_from_bytes(bytes, "image/png");
	g_autoptr(AiMessage) message = ai_message_new(AI_ROLE_USER);
	g_autofree gchar *projected = NULL;

	ai_message_add_content_block(
		message, (AiContentBlock *)g_steal_pointer(&image));
	projected = ai_cli_client_project_message(message);

	g_assert_nonnull(strstr(projected, "mime=image/png"));
	g_assert_nonnull(strstr(projected, "bytes=4"));
	g_assert_nonnull(strstr(projected, "not representable"));
}

static void
test_all_subprocess_cli_builders_use_projection(void)
{
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();
	g_autoptr(AiAntigravityClient) agy = ai_antigravity_client_new();
	g_autoptr(AiCursorClient) cursor = ai_cursor_client_new();
	AiCliClient *clients[5];
	GList *messages = structured_messages();
	guint i;

	clients[0] = AI_CLI_CLIENT(claude);
	clients[1] = AI_CLI_CLIENT(opencode);
	clients[2] = AI_CLI_CLIENT(grok);
	clients[3] = AI_CLI_CLIENT(agy);
	clients[4] = AI_CLI_CLIENT(cursor);

	for (i = 0; i < G_N_ELEMENTS(clients); i++)
	{
		AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(clients[i]);
		g_autofree gchar *prompt = NULL;

		g_assert_nonnull(klass->build_stdin);
		prompt = klass->build_stdin(clients[i], messages);
		assert_projection(prompt);
	}

	g_list_free_full(messages, g_object_unref);
}

static void
test_claude_tmux_uses_projection(void)
{
	GList *messages = structured_messages();
	g_autofree gchar *prompt =
		ai_claude_tmux_client_build_prompt(messages);

	assert_projection(prompt);
	g_list_free_full(messages, g_object_unref);
}

static void
test_switched_cli_seeds_once_then_sends_delta(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	GList *messages = structured_messages();
	g_autoptr(AiMessage) newest = ai_message_new_user("Only this is new.");
	g_autofree gchar *seed = NULL;
	g_autofree gchar *delta = NULL;

	ai_cli_client_mark_portable_context(AI_CLI_CLIENT(client));
	seed = klass->build_stdin(AI_CLI_CLIENT(client), messages);
	g_assert_nonnull(strstr(seed, "Inspect the file."));
	g_assert_nonnull(strstr(seed, "Tool result: read"));

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "new-native-session");
	messages = g_list_append(messages, g_object_ref(newest));
	delta = klass->build_stdin(AI_CLI_CLIENT(client), messages);
	g_assert_nonnull(strstr(delta, "Only this is new."));
	g_assert_null(strstr(delta, "Inspect the file."));
	g_assert_null(strstr(delta, "Tool result: read"));

	g_list_free_full(messages, g_object_unref);
}

static void
test_opencode_uses_per_turn_system_prompt_once(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(AiMessage) message = ai_message_new_user("hello");
	GList *messages = g_list_append(NULL, message);
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *fresh = NULL;
	g_autofree gchar *resumed = NULL;

	argv = ai_opencode_client_build_argv(
		AI_CLI_CLIENT(client), messages, "portable system", 4096, FALSE);
	fresh = klass->build_stdin(AI_CLI_CLIENT(client), messages);
	g_assert_nonnull(strstr(fresh, "<system>\nportable system\n</system>"));

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "session-1");
	g_clear_pointer(&argv, g_strfreev);
	argv = ai_opencode_client_build_argv(
		AI_CLI_CLIENT(client), messages, "portable system", 4096, FALSE);
	resumed = klass->build_stdin(AI_CLI_CLIENT(client), messages);
	g_assert_null(strstr(resumed, "portable system"));

	g_list_free(messages);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/cli-context/text-stable",
	                test_text_only_projection_is_stable);
	g_test_add_func("/ai-glib/cli-context/image-explicit",
	                test_image_projection_is_explicit);
	g_test_add_func("/ai-glib/cli-context/builders",
	                test_all_subprocess_cli_builders_use_projection);
	g_test_add_func("/ai-glib/cli-context/claude-tmux",
	                test_claude_tmux_uses_projection);
	g_test_add_func("/ai-glib/cli-context/seed-once",
	                test_switched_cli_seeds_once_then_sends_delta);
	g_test_add_func("/ai-glib/cli-context/opencode-system",
	                test_opencode_uses_per_turn_system_prompt_once);

	return g_test_run();
}
