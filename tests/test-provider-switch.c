/*
 * test-provider-switch.c - Same-process provider context handoff
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <ai-glib.h>

#include <string.h>

#include "test-server.h"

typedef struct
{
	GMainLoop *loop;
	gboolean   ok;
	GError    *error;
} SendRun;

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
	SendRun *run = user_data;

	run->ok = ai_conversation_send_finish(AI_CONVERSATION(source), result,
	                                      &run->error);
	g_main_loop_quit(run->loop);
}

static void
send_and_wait(AiConversation *conversation, const gchar *text, SendRun *run)
{
	run->loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_async(conversation, text, NULL, on_sent, run);
	g_main_loop_run(run->loop);
	g_main_loop_unref(run->loop);
	run->loop = NULL;
}

static void
send_run_clear(SendRun *run)
{
	g_clear_error(&run->error);
}

static gchar *
message_text_at(GList *messages, guint index)
{
	AiMessage *message = g_list_nth_data(messages, index);

	g_assert_nonnull(message);
	return ai_message_get_text(message);
}

static void
test_switch_preserves_context_and_transcript(void)
{
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiMockProvider) second = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	SendRun one = { NULL, FALSE, NULL };
	SendRun two = { NULL, FALSE, NULL };
	GError *error = NULL;
	guint blocks_before;
	GList *received;
	g_autofree gchar *text = NULL;

	ai_mock_provider_push_text(first, "The passphrase is blåbær.");
	send_and_wait(conversation, "Remember this.", &one);
	g_assert_true(one.ok);
	g_assert_no_error(one.error);

	blocks_before = ai_transcript_get_n_blocks(
		ai_conversation_get_transcript(conversation));
	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(second), &error));
	g_assert_no_error(error);
	g_assert_true(ai_conversation_get_provider(conversation) ==
	              G_OBJECT(second));
	g_assert_cmpuint(ai_transcript_get_n_blocks(
		ai_conversation_get_transcript(conversation)), ==, blocks_before);

	ai_mock_provider_push_text(second, "You told me blåbær.");
	send_and_wait(conversation, "What was it?", &two);
	g_assert_true(two.ok);
	g_assert_no_error(two.error);
	g_assert_cmpuint(ai_mock_provider_get_call_count(first), ==, 1);
	g_assert_cmpuint(ai_mock_provider_get_call_count(second), ==, 1);

	received = ai_mock_provider_get_last_messages(second);
	g_assert_cmpuint(g_list_length(received), ==, 3);
	text = message_text_at(received, 1);
	g_assert_cmpstr(text, ==, "The passphrase is blåbær.");

	send_run_clear(&one);
	send_run_clear(&two);
}

static void
test_switch_while_busy_is_atomic(void)
{
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiMockProvider) second = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	SendRun run = { NULL, FALSE, NULL };
	GError *error = NULL;

	ai_mock_provider_set_delay_ms(first, 20);
	ai_mock_provider_push_text(first, "done");
	run.loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_async(conversation, "wait", NULL, on_sent, &run);

	g_assert_true(ai_conversation_get_busy(conversation));
	g_assert_false(ai_conversation_set_provider(
		conversation, G_OBJECT(second), &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_true(ai_conversation_get_provider(conversation) ==
	              G_OBJECT(first));
	g_clear_error(&error);

	g_main_loop_run(run.loop);
	g_main_loop_unref(run.loop);
	run.loop = NULL;
	g_assert_true(run.ok);
	send_run_clear(&run);
}

static void
test_switch_reconnects_events(void)
{
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiMockProvider) second = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	g_autoptr(AiEvent) old_event = ai_event_new_text_delta("old");
	g_autoptr(AiEvent) new_event = ai_event_new_text_delta("new");
	AiTranscript *transcript = ai_conversation_get_transcript(conversation);
	GError *error = NULL;

	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(second), &error));
	g_assert_no_error(error);

	ai_event_source_emit(AI_EVENT_SOURCE(first), old_event);
	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 0);

	ai_event_source_emit(AI_EVENT_SOURCE(second), new_event);
	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 1);
}

static void
test_switch_to_cli_resets_native_state(void)
{
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	GError *error = NULL;
	gboolean continue_session = TRUE;

	ai_conversation_set_working_directory(conversation, g_get_tmp_dir());
	ai_conversation_set_local_tools(conversation, TRUE);
	ai_cli_client_set_session_id(AI_CLI_CLIENT(cli), "foreign-session");
	g_object_set(cli, "continue-session", TRUE, NULL);

	g_assert_false(ai_conversation_get_passthrough_commands(conversation));
	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(cli), &error));
	g_assert_no_error(error);
	g_assert_false(ai_conversation_get_local_tools(conversation));
	g_assert_true(ai_conversation_get_passthrough_commands(conversation));
	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(cli)));
	g_assert_cmpstr(ai_cli_client_get_working_directory(AI_CLI_CLIENT(cli)),
	                ==, g_get_tmp_dir());
	g_object_get(cli, "continue-session", &continue_session, NULL);
	g_assert_false(continue_session);
}

static void
test_explicit_passthrough_survives_switch(void)
{
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	GError *error = NULL;

	ai_conversation_set_passthrough_commands(conversation, FALSE);
	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(cli), &error));
	g_assert_no_error(error);
	g_assert_false(ai_conversation_get_passthrough_commands(conversation));
}

static void
test_tool_endpoint_failure_leaves_provider(void)
{
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	g_autoptr(AiMockProvider) http = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(cli));
	g_autoptr(AiAgentEndpoint) endpoint =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_ENV, NULL);
	GError *error = NULL;

	ai_agent_endpoint_set_env(endpoint, "TEST_TOOL_TOKEN", "not-secret");
	g_assert_true(ai_conversation_set_tool_endpoint(
		conversation, endpoint, &error));
	g_assert_no_error(error);

	g_assert_false(ai_conversation_set_provider(
		conversation, G_OBJECT(http), &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_true(ai_conversation_get_provider(conversation) == G_OBJECT(cli));
	g_clear_error(&error);
}

static void
assert_structured_history(gboolean stream)
{
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiMockProvider) second = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	SendRun run = { NULL, FALSE, NULL };
	SendRun follow_up = { NULL, FALSE, NULL };
	GList *history;
	GList *blocks;
	GError *error = NULL;

	ai_conversation_set_stream(conversation, stream);
	ai_conversation_set_local_tools(conversation, TRUE);
	ai_mock_provider_push_tool_use(
		first, "todo_write",
		"{\"todos\":[{\"content\":\"preserve context\","
		"\"status\":\"completed\"}]}");
	ai_mock_provider_push_text(first, "Tool work complete.");

	send_and_wait(conversation, "Use a tool.", &run);
	g_assert_true(run.ok);
	g_assert_no_error(run.error);

	history = ai_conversation_get_messages(conversation);
	g_assert_cmpuint(g_list_length(history), ==, 4);
	g_assert_cmpint(ai_message_get_role(g_list_nth_data(history, 1)), ==,
	                AI_ROLE_ASSISTANT);
	blocks = ai_message_get_content_blocks(g_list_nth_data(history, 1));
	g_assert_true(AI_IS_TOOL_USE(blocks->data));
	blocks = ai_message_get_content_blocks(g_list_nth_data(history, 2));
	g_assert_true(AI_IS_TOOL_RESULT(blocks->data));
	g_assert_false(ai_tool_result_get_is_error(AI_TOOL_RESULT(blocks->data)));

	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(second), &error));
	g_assert_no_error(error);
	ai_mock_provider_push_text(second, "I can see the tool exchange.");
	send_and_wait(conversation, "Continue.", &follow_up);
	g_assert_true(follow_up.ok);
	g_assert_cmpuint(g_list_length(
		ai_mock_provider_get_last_messages(second)), ==, 5);

	send_run_clear(&run);
	send_run_clear(&follow_up);
}

static void
test_structured_history_streaming(void)
{
	assert_structured_history(TRUE);
}

static void
test_structured_history_non_streaming(void)
{
	assert_structured_history(FALSE);
}

static void
test_failed_tool_turn_does_not_graft_partial_exchange(void)
{
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(provider));
	SendRun run = { NULL, FALSE, NULL };

	ai_conversation_set_local_tools(conversation, TRUE);
	ai_mock_provider_push_tool_use(
		provider, "todo_write",
		"{\"todos\":[{\"content\":\"temporary\","
		"\"status\":\"completed\"}]}");
	ai_mock_provider_push_error(provider, "follow-up failed");

	send_and_wait(conversation, "Try.", &run);
	g_assert_false(run.ok);
	g_assert_nonnull(run.error);
	g_assert_cmpuint(g_list_length(
		ai_conversation_get_messages(conversation)), ==, 1);
	send_run_clear(&run);
}

static void
test_structured_context_reaches_http_wire(void)
{
	static const gchar *response =
		"{\"id\":\"chatcmpl-switch\",\"model\":\"gpt-4o\","
		"\"choices\":[{\"message\":{\"role\":\"assistant\","
		"\"content\":\"continued\"},\"finish_reason\":\"stop\"}]}";
	TServer *server = tserver_new();
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiOpenAIClient) second = NULL;
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	SendRun tool_run = { NULL, FALSE, NULL };
	SendRun next_run = { NULL, FALSE, NULL };
	g_autofree gchar *body = NULL;
	GError *error = NULL;

	tserver_set_response(server, SOUP_STATUS_OK, response);
	ai_config_set_base_url(config, AI_PROVIDER_OPENAI, server->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "test-key");
	ai_config_set_max_retries(config, 0);
	second = ai_openai_client_new_with_config(config);

	ai_conversation_set_stream(conversation, FALSE);
	ai_conversation_set_local_tools(conversation, TRUE);
	ai_mock_provider_push_tool_use(
		first, "todo_write",
		"{\"todos\":[{\"content\":\"wire marker\","
		"\"status\":\"completed\"}]}");
	ai_mock_provider_push_text(first, "Prepared the wire marker.");
	send_and_wait(conversation, "Prepare it.", &tool_run);
	g_assert_true(tool_run.ok);

	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(second), &error));
	g_assert_no_error(error);
	ai_conversation_set_local_tools(conversation, FALSE);
	send_and_wait(conversation, "Continue over HTTP.", &next_run);
	g_assert_true(next_run.ok);
	g_assert_no_error(next_run.error);

	body = tserver_dup_last_body(server);
	g_assert_nonnull(body);
	g_assert_nonnull(strstr(body, "\"tool_calls\""));
	g_assert_nonnull(strstr(body, "\"tool_call_id\":\"mock-tool-1\""));
	g_assert_nonnull(strstr(body, "wire marker"));
	g_assert_nonnull(strstr(body, "Continue over HTTP."));

	send_run_clear(&tool_run);
	send_run_clear(&next_run);
	tserver_free(server);
}

static void
test_clear_then_switch_has_no_old_context(void)
{
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiMockProvider) second = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	SendRun one = { NULL, FALSE, NULL };
	SendRun two = { NULL, FALSE, NULL };
	GError *error = NULL;

	ai_mock_provider_push_text(first, "old");
	send_and_wait(conversation, "old question", &one);
	ai_conversation_clear(conversation);
	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(second), &error));
	g_assert_no_error(error);

	ai_mock_provider_push_text(second, "new");
	send_and_wait(conversation, "new question", &two);
	g_assert_cmpuint(g_list_length(
		ai_mock_provider_get_last_messages(second)), ==, 1);

	send_run_clear(&one);
	send_run_clear(&two);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/provider-switch/context-and-transcript",
	                test_switch_preserves_context_and_transcript);
	g_test_add_func("/ai-glib/provider-switch/busy-is-atomic",
	                test_switch_while_busy_is_atomic);
	g_test_add_func("/ai-glib/provider-switch/reconnects-events",
	                test_switch_reconnects_events);
	g_test_add_func("/ai-glib/provider-switch/cli-native-state",
	                test_switch_to_cli_resets_native_state);
	g_test_add_func("/ai-glib/provider-switch/explicit-passthrough",
	                test_explicit_passthrough_survives_switch);
	g_test_add_func("/ai-glib/provider-switch/tool-endpoint-atomic",
	                test_tool_endpoint_failure_leaves_provider);
	g_test_add_func("/ai-glib/provider-switch/structured/streaming",
	                test_structured_history_streaming);
	g_test_add_func("/ai-glib/provider-switch/structured/non-streaming",
	                test_structured_history_non_streaming);
	g_test_add_func("/ai-glib/provider-switch/structured/error",
	                test_failed_tool_turn_does_not_graft_partial_exchange);
	g_test_add_func("/ai-glib/provider-switch/structured/http-wire",
	                test_structured_context_reaches_http_wire);
	g_test_add_func("/ai-glib/provider-switch/clear",
	                test_clear_then_switch_has_no_old_context);

	return g_test_run();
}
