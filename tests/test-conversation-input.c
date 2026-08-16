/*
 * test-conversation-input.c - The input pipeline
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The order of the pipeline is the thing worth pinning: resolve, expand,
 * record, send. Two of those steps are easy to swap and the result of
 * swapping them is subtle -- the transcript showing an expansion instead
 * of what the user typed, or a mention inside a command body being
 * expanded twice. Each is asserted separately below.
 *
 * The passthrough half matters just as much in the other direction: a
 * wrapped CLI has to receive the line byte-for-byte, because it does its
 * own resolution and doing it twice is worse than not at all.
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static gchar *sandbox = NULL;

static void
rm_rf(const gchar *path)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", path);

	g_assert_cmpint(system(cmd), ==, 0);
}

static void
write_file(const gchar *relative, const gchar *contents)
{
	g_autofree gchar *path = g_build_filename(sandbox, relative, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	GError           *error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);
}

static AiCommandSet *
command_set_with(
	const gchar    *name,
	AiResourceKind  kind,
	const gchar    *contents
){
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	AiCommandSet                 *set;

	if (name != NULL)
	{
		g_autoptr(AiResource) resource =
			ai_resource_new_from_data(contents, -1, name, kind, "claude",
			                          AI_RESOURCE_SCOPE_USER, NULL);

		ai_resource_registry_add(registry, resource);
	}

	set = ai_command_set_new(registry);

	return set;
}

typedef struct
{
	GMainLoop       *loop;
	gboolean         ok;
	GError          *error;
	AiCommandResult *command;
} Outcome;

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Outcome *outcome = user_data;

	outcome->ok = ai_conversation_send_input_finish(AI_CONVERSATION(source),
	                                                result,
	                                                &outcome->command,
	                                                &outcome->error);
	g_main_loop_quit(outcome->loop);
}

/* Send one line and wait for the turn to finish. */
static void
send_input(AiConversation *conversation, const gchar *line, Outcome *outcome)
{
	outcome->loop = g_main_loop_new(NULL, FALSE);
	outcome->ok = FALSE;
	outcome->error = NULL;
	outcome->command = NULL;

	ai_conversation_send_input_async(conversation, line, NULL, on_sent,
	                                 outcome);
	g_main_loop_run(outcome->loop);
	g_main_loop_unref(outcome->loop);
	outcome->loop = NULL;
}

static void
outcome_clear(Outcome *outcome)
{
	g_clear_error(&outcome->error);
	g_clear_object(&outcome->command);
}

/* The text of the last user message the provider would have seen. */
static const gchar *
last_user_message(AiConversation *conversation)
{
	GList *messages = ai_conversation_get_messages(conversation);
	GList *iter;
	static gchar *cached = NULL;

	g_clear_pointer(&cached, g_free);

	for (iter = messages; iter != NULL; iter = iter->next)
	{
		if (ai_message_get_role(iter->data) == AI_ROLE_USER)
		{
			g_clear_pointer(&cached, g_free);
			cached = ai_message_get_text(iter->data);
		}
	}

	return cached;
}

/* The transcript, flattened. */
static gchar *
transcript_text(AiConversation *conversation)
{
	return ai_transcript_to_text(ai_conversation_get_transcript(conversation),
	                             0);
}

/* ----------------------------------------------------------------
 * HTTP providers: expand
 * ---------------------------------------------------------------- */

static void
test_http_provider_expands_mentions(void)
{
	g_autoptr(AiMockProvider)  mock = ai_mock_provider_new();
	g_autoptr(AiConversation)  conversation =
		ai_conversation_new(G_OBJECT(mock));
	Outcome                    outcome = { NULL, FALSE, NULL, NULL };

	write_file("hello.c", "int main(void) { return 0; }\n");

	ai_conversation_set_working_directory(conversation, sandbox);
	ai_mock_provider_push_text(mock, "I read it");

	send_input(conversation, "explain @hello.c please", &outcome);

	g_assert_no_error(outcome.error);
	g_assert_true(outcome.ok);
	g_assert_null(outcome.command);

	/* The model got the file. */
	g_assert_nonnull(strstr(last_user_message(conversation),
	                        "int main(void)"));

	/*
	 * The transcript shows what was typed, not the expansion. Showing
	 * the expansion would bury a one-line question under the file it
	 * pulled in.
	 */
	{
		g_autofree gchar *text = transcript_text(conversation);

		g_assert_nonnull(strstr(text, "explain @hello.c please"));
		g_assert_null(strstr(text, "int main(void)"));
	}

	outcome_clear(&outcome);
}

static void
test_http_provider_resolves_commands(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiCommandSet)   set =
		command_set_with("deploy", AI_RESOURCE_COMMAND,
		                 "---\nname: deploy\n---\nDeploy to $1.\n");
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };

	ai_conversation_set_command_set(conversation, set);
	ai_conversation_set_working_directory(conversation, sandbox);
	ai_mock_provider_push_text(mock, "deploying");

	send_input(conversation, "/deploy staging", &outcome);

	g_assert_no_error(outcome.error);
	g_assert_null(outcome.command);
	g_assert_cmpstr(last_user_message(conversation), ==, "Deploy to staging.\n");

	/* And the transcript records the invocation, not the body. */
	{
		g_autofree gchar *text = transcript_text(conversation);

		g_assert_nonnull(strstr(text, "/deploy staging"));
		g_assert_null(strstr(text, "Deploy to staging"));
	}

	outcome_clear(&outcome);
}

static void
test_mentions_inside_a_command_body_expand_once(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiCommandSet)   set =
		command_set_with("review", AI_RESOURCE_COMMAND,
		                 "---\nname: review\n---\nReview @target.c now.\n");
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };
	const gchar              *sent;
	const gchar              *first;

	write_file("target.c", "MARKER_ONE\n");

	ai_conversation_set_command_set(conversation, set);
	ai_conversation_set_working_directory(conversation, sandbox);
	ai_mock_provider_push_text(mock, "reviewed");

	send_input(conversation, "/review", &outcome);

	g_assert_no_error(outcome.error);

	/*
	 * The command's body is expanded, once. Resolution happens first and
	 * expansion second, in one place -- if either step ran twice the
	 * marker would appear twice here.
	 */
	sent = last_user_message(conversation);
	first = strstr(sent, "MARKER_ONE");
	g_assert_nonnull(first);
	g_assert_null(strstr(first + 1, "MARKER_ONE"));

	outcome_clear(&outcome);
}

static void
test_builtin_is_reported_and_nothing_is_sent(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiCommandSet)   set = command_set_with(NULL, 0, NULL);
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };

	ai_conversation_set_command_set(conversation, set);

	send_input(conversation, "/model opus", &outcome);

	g_assert_no_error(outcome.error);
	g_assert_true(outcome.ok);

	/* The frontend is told what to do; the model hears nothing. */
	g_assert_nonnull(outcome.command);
	g_assert_cmpstr(ai_command_result_get_name(outcome.command), ==, "model");
	g_assert_cmpstr(ai_command_result_get_arguments(outcome.command), ==,
	                "opus");
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 0);
	g_assert_null(ai_conversation_get_messages(conversation));

	outcome_clear(&outcome);
}

static void
test_unknown_command_is_an_error_for_http(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiCommandSet)   set = command_set_with(NULL, 0, NULL);
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };

	ai_conversation_set_command_set(conversation, set);

	send_input(conversation, "/nonexistent", &outcome);

	/* There is nothing downstream that could make sense of it. */
	g_assert_nonnull(outcome.error);
	g_assert_false(outcome.ok);
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 0);

	outcome_clear(&outcome);
}

static void
test_no_command_set_sends_the_line_verbatim(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };

	ai_conversation_set_working_directory(conversation, sandbox);
	ai_mock_provider_push_text(mock, "ok");

	/* No command set at all: an embedder that only wants a transcript
	 * gets one, and a slash is just a character. */
	send_input(conversation, "/not-resolved here", &outcome);

	g_assert_no_error(outcome.error);
	g_assert_null(outcome.command);
	g_assert_cmpstr(last_user_message(conversation), ==,
	                "/not-resolved here");

	outcome_clear(&outcome);
}

/* ----------------------------------------------------------------
 * CLI providers: pass through
 * ---------------------------------------------------------------- */

static void
test_cli_provider_defaults_to_passthrough(void)
{
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	g_autoptr(AiConversation)    conversation =
		ai_conversation_new(G_OBJECT(cli));
	g_autoptr(AiMockProvider)    mock = ai_mock_provider_new();
	g_autoptr(AiConversation)    http =
		ai_conversation_new(G_OBJECT(mock));

	/*
	 * The default is by provider kind, and it is the whole point: grok,
	 * claude-code and opencode resolve @ and / themselves, so expanding
	 * first would fight them and /compact would never arrive.
	 */
	g_assert_true(ai_conversation_get_passthrough_commands(conversation));
	g_assert_false(ai_conversation_get_passthrough_commands(http));
}

static void
test_passthrough_can_be_overridden_both_ways(void)
{
	g_autoptr(AiGrokBuildClient) cli = ai_grok_build_client_new();
	g_autoptr(AiConversation)    conversation =
		ai_conversation_new(G_OBJECT(cli));

	ai_conversation_set_passthrough_commands(conversation, FALSE);
	g_assert_false(ai_conversation_get_passthrough_commands(conversation));

	ai_conversation_set_passthrough_commands(conversation, TRUE);
	g_assert_true(ai_conversation_get_passthrough_commands(conversation));
}

static void
test_passthrough_leaves_the_line_untouched(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };
	const gchar              *line = "explain @hello.c and /compact after";

	write_file("hello.c", "SHOULD_NOT_BE_INLINED\n");

	ai_conversation_set_working_directory(conversation, sandbox);
	ai_conversation_set_passthrough_commands(conversation, TRUE);
	ai_mock_provider_push_text(mock, "ok");

	send_input(conversation, line, &outcome);

	g_assert_no_error(outcome.error);

	/* Byte-for-byte. Doing the expansion twice would be worse than not
	 * doing it at all. */
	g_assert_cmpstr(last_user_message(conversation), ==, line);

	outcome_clear(&outcome);
}

static void
test_unknown_command_passes_through(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiCommandSet)   set = command_set_with(NULL, 0, NULL);
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };

	ai_conversation_set_command_set(conversation, set);
	ai_conversation_set_passthrough_commands(conversation, TRUE);
	ai_mock_provider_push_text(mock, "compacted");

	send_input(conversation, "/compact", &outcome);

	/* /compact means something to claude and nothing here. Refusing it
	 * would take away a feature the wrapped CLI has. */
	g_assert_no_error(outcome.error);
	g_assert_null(outcome.command);
	g_assert_cmpstr(last_user_message(conversation), ==, "/compact");

	outcome_clear(&outcome);
}

static void
test_local_builtins_still_run_under_passthrough(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiCommandSet)   set = command_set_with(NULL, 0, NULL);
	Outcome                   outcome = { NULL, FALSE, NULL, NULL };

	ai_conversation_set_command_set(conversation, set);
	ai_conversation_set_passthrough_commands(conversation, TRUE);

	send_input(conversation, "/clear", &outcome);

	/* The wrapped CLI has no opinion about this program's transcript. */
	g_assert_no_error(outcome.error);
	g_assert_nonnull(outcome.command);
	g_assert_cmpstr(ai_command_result_get_name(outcome.command), ==, "clear");
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 0);

	outcome_clear(&outcome);
}

/* ----------------------------------------------------------------
 * Resolution without sending
 * ---------------------------------------------------------------- */

static void
test_resolve_input_without_sending(void)
{
	g_autoptr(AiMockProvider)  mock = ai_mock_provider_new();
	g_autoptr(AiConversation)  conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiCommandSet)    set =
		command_set_with("greet", AI_RESOURCE_COMMAND,
		                 "---\nname: greet\n---\nSay hello to $1.\n");
	g_autoptr(AiCommandResult) result = NULL;
	GError                    *error = NULL;

	ai_conversation_set_command_set(conversation, set);

	/* What /expand is built on: the decision, inspectable, with nothing
	 * sent. */
	result = ai_conversation_resolve_input(conversation, "/greet world",
	                                       NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpint(ai_command_result_get_outcome(result), ==,
	                AI_COMMAND_OUTCOME_PROMPT);
	g_assert_cmpstr(ai_command_result_get_prompt(result), ==,
	                "Say hello to world.\n");
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 0);
}

/* ----------------------------------------------------------------
 * State
 * ---------------------------------------------------------------- */

static void
test_send_while_busy_is_refused(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	Outcome                   first = { NULL, FALSE, NULL, NULL };
	Outcome                   second = { NULL, FALSE, NULL, NULL };

	ai_mock_provider_set_delay_ms(mock, 50);
	ai_mock_provider_push_text(mock, "one");

	first.loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_input_async(conversation, "first", NULL, on_sent,
	                                 &first);

	/* The second call must be refused cleanly rather than corrupting the
	 * transcript with a second turn block. */
	second.loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_input_async(conversation, "second", NULL, on_sent,
	                                 &second);
	g_main_loop_run(second.loop);

	g_assert_nonnull(second.error);
	g_assert_false(second.ok);

	g_main_loop_run(first.loop);
	g_assert_no_error(first.error);

	g_main_loop_unref(first.loop);
	g_main_loop_unref(second.loop);
	outcome_clear(&first);
	outcome_clear(&second);
}

static void
test_command_set_hands_the_registry_to_the_executor(void)
{
	g_autoptr(AiMockProvider)     mock = ai_mock_provider_new();
	g_autoptr(AiConversation)     conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiCommandSet)       set = ai_command_set_new(registry);

	g_assert_null(ai_tool_executor_get_resource_registry(
		ai_conversation_get_executor(conversation)));

	ai_conversation_set_command_set(conversation, set);

	/*
	 * One assignment. A user who can type /reviewer expects the model to
	 * be able to reach the same agent through `task`, and two settings
	 * that could disagree would be a bug waiting to be filed.
	 */
	g_assert_true(ai_tool_executor_get_resource_registry(
		ai_conversation_get_executor(conversation)) == registry);
}

static void
test_working_directory_reaches_the_executor(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));

	ai_conversation_set_working_directory(conversation, sandbox);

	g_assert_cmpstr(ai_conversation_get_working_directory(conversation), ==,
	                sandbox);
	g_assert_cmpstr(ai_tool_executor_get_working_directory(
		ai_conversation_get_executor(conversation)), ==, sandbox);
}

static void
test_clear_empties_the_todo_list(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	AiToolExecutor           *executor =
		ai_conversation_get_executor(conversation);
	g_autoptr(JsonParser)     parser = json_parser_new();
	g_autoptr(AiToolUse)      use = NULL;
	g_autofree gchar         *result = NULL;

	g_assert_true(json_parser_load_from_data(parser,
		"{\"todos\":[{\"content\":\"a\",\"status\":\"pending\"}]}", -1, NULL));
	use = ai_tool_use_new("id", "todo_write", json_parser_get_root(parser));
	result = ai_tool_executor_execute(executor, use, NULL, NULL);
	g_assert_nonnull(result);

	/* A block appeared for it. */
	g_assert_cmpuint(
		ai_transcript_get_n_blocks(
			ai_conversation_get_transcript(conversation)), ==, 1);

	ai_conversation_clear(conversation);

	/* And /clear takes both: the list is part of this conversation, and
	 * leaving it behind would be the one thing on screen that did not go. */
	g_assert_cmpuint(ai_tool_executor_get_n_todos(executor), ==, 0);
	g_assert_cmpuint(
		ai_transcript_get_n_blocks(
			ai_conversation_get_transcript(conversation)), ==, 0);
}

static void
test_todo_updates_reuse_one_block(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	AiToolExecutor           *executor =
		ai_conversation_get_executor(conversation);
	guint                     i;

	for (i = 0; i < 5; i++)
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(AiToolUse)  use = NULL;
		g_autofree gchar     *result = NULL;
		g_autofree gchar     *json =
			g_strdup_printf("{\"todos\":[{\"content\":\"item %u\","
			                "\"status\":\"pending\"}]}", i);

		g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
		use = ai_tool_use_new("id", "todo_write",
		                      json_parser_get_root(parser));
		result = ai_tool_executor_execute(executor, use, NULL, NULL);
		g_assert_nonnull(result);
	}

	/* Five revisions, one block. This is what stops a long task from
	 * burying its own conversation under copies of its plan. */
	g_assert_cmpuint(
		ai_transcript_get_n_blocks(
			ai_conversation_get_transcript(conversation)), ==, 1);
}

/* ----------------------------------------------------------------
 * Background agents
 * ---------------------------------------------------------------- */

/*
 * A conversation offers no background-agent tools until it is given a
 * brigade. Restated here as well as on the executor, because this is the
 * object an application actually holds.
 */
static void
test_background_agents_are_off_until_enabled(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	AiToolExecutor           *executor =
		ai_conversation_get_executor(conversation);
	GList                    *iter;
	gboolean                  found = FALSE;

	g_assert_null(ai_conversation_get_brigade(conversation));

	for (iter = ai_tool_executor_get_tools(executor); iter != NULL;
	     iter = iter->next)
	{
		if (g_strcmp0(ai_tool_get_name(iter->data), "agent_spawn") == 0)
			found = TRUE;
	}

	g_assert_false(found);
}

static void
test_enabling_installs_a_working_brigade(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	AiBrigade                *brigade;

	brigade = ai_conversation_enable_background_agents(conversation, 2);

	g_assert_nonnull(brigade);
	g_assert_true(ai_conversation_get_brigade(conversation) == brigade);
	g_assert_nonnull(ai_brigade_get_worker(brigade));
	g_assert_cmpuint(ai_brigade_get_max_concurrent(brigade), ==, 2);

	/* The executor is where the tools live, and it has them now. */
	g_assert_true(ai_tool_executor_get_brigade(
		ai_conversation_get_executor(conversation)) == brigade);

	/* Enabling twice keeps the first one rather than replacing it and
	 * orphaning whatever was already running in it. */
	g_assert_true(
		ai_conversation_enable_background_agents(conversation, 8) == brigade);
	g_assert_cmpuint(ai_brigade_get_max_concurrent(brigade), ==, 2);
}

typedef struct
{
	guint  count;
	gchar *last_id;
} FinishSpy;

static void
on_conversation_agent_finished(AiConversation *conversation,
                               const gchar *agent_id, gint state,
                               gpointer user_data)
{
	FinishSpy *spy = user_data;

	(void)conversation; (void)state;

	spy->count++;
	g_free(spy->last_id);
	spy->last_id = g_strdup(agent_id);
}

/*
 * The panel appears when something is spawned, updates in place, and the
 * finish is republished for a frontend to show.
 *
 * The signal matters because the model is told separately and later ---
 * at the next turn boundary. The person watching should not have to send
 * a message to find out that the thing they started has finished.
 */
static void
test_agents_get_a_block_and_a_signal(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiMockProvider) agent_mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiAgent)        agent = NULL;
	AiBrigade                *brigade;
	FinishSpy                 spy = { 0 };
	AiTranscript             *transcript =
		ai_conversation_get_transcript(conversation);
	guint                     blocks_after_start;

	ai_mock_provider_push_text(agent_mock, "the finding");

	brigade = ai_conversation_enable_background_agents(conversation, 4);

	g_signal_connect(conversation, "agent-finished",
	                 G_CALLBACK(on_conversation_agent_finished), &spy);

	agent = ai_agent_new("a1", AI_PROVIDER(agent_mock));
	ai_agent_set_description(agent, "the work");

	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 0);

	ai_brigade_start(brigade, agent, "go", NULL);

	blocks_after_start = ai_transcript_get_n_blocks(transcript);
	g_assert_cmpuint(blocks_after_start, ==, 1);

	{
		gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

		while (spy.count == 0 && g_get_monotonic_time() < deadline)
		{
			if (!g_main_context_iteration(NULL, FALSE)) g_usleep(1000);
		}
	}

	g_assert_cmpuint(spy.count, ==, 1);
	g_assert_cmpstr(spy.last_id, ==, "a1");

	/* Still one block: the state change rewrote it rather than adding a
	 * second copy, exactly as the todo list does. */
	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 1);

	g_free(spy.last_id);
}

/*
 * /clear empties the panel and leaves the agents alone.
 *
 * Clearing a transcript is an instruction about the display. Killing
 * work somebody started because they wanted a clean screen would be a
 * surprising way to lose an hour of it.
 */
static void
test_clear_empties_the_panel_but_not_the_brigade(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiMockProvider) agent_mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiAgent)        agent = NULL;
	AiBrigade                *brigade;

	ai_mock_provider_push_text(agent_mock, "hi");
	ai_mock_provider_set_delay_ms(agent_mock, 50);

	brigade = ai_conversation_enable_background_agents(conversation, 4);

	agent = ai_agent_new("a1", AI_PROVIDER(agent_mock));
	ai_brigade_start(brigade, agent, "go", NULL);

	ai_conversation_clear(conversation);

	g_assert_cmpuint(
		ai_transcript_get_n_blocks(
			ai_conversation_get_transcript(conversation)), ==, 0);
	g_assert_nonnull(ai_brigade_get(brigade, "a1"));

	{
		gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

		while (ai_agent_state_is_live(ai_agent_get_state(agent)) &&
		       g_get_monotonic_time() < deadline)
		{
			if (!g_main_context_iteration(NULL, FALSE)) g_usleep(1000);
		}
	}
}

int
main(int argc, char *argv[])
{
	GError *error = NULL;
	int     status;

	g_test_init(&argc, &argv, NULL);

	sandbox = g_dir_make_tmp("ai-glib-convinput-XXXXXX", &error);
	g_assert_no_error(error);

	g_test_add_func("/ai-glib/input/http-expands-mentions",
	                test_http_provider_expands_mentions);
	g_test_add_func("/ai-glib/input/http-resolves-commands",
	                test_http_provider_resolves_commands);
	g_test_add_func("/ai-glib/input/body-mentions-once",
	                test_mentions_inside_a_command_body_expand_once);
	g_test_add_func("/ai-glib/input/builtin-not-sent",
	                test_builtin_is_reported_and_nothing_is_sent);
	g_test_add_func("/ai-glib/input/unknown-command-http",
	                test_unknown_command_is_an_error_for_http);
	g_test_add_func("/ai-glib/input/no-command-set",
	                test_no_command_set_sends_the_line_verbatim);

	g_test_add_func("/ai-glib/input/cli-default",
	                test_cli_provider_defaults_to_passthrough);
	g_test_add_func("/ai-glib/input/override",
	                test_passthrough_can_be_overridden_both_ways);
	g_test_add_func("/ai-glib/input/passthrough-verbatim",
	                test_passthrough_leaves_the_line_untouched);
	g_test_add_func("/ai-glib/input/unknown-passes-through",
	                test_unknown_command_passes_through);
	g_test_add_func("/ai-glib/input/local-builtins",
	                test_local_builtins_still_run_under_passthrough);

	g_test_add_func("/ai-glib/input/resolve-only",
	                test_resolve_input_without_sending);

	g_test_add_func("/ai-glib/input/busy", test_send_while_busy_is_refused);
	g_test_add_func("/ai-glib/input/registry-shared",
	                test_command_set_hands_the_registry_to_the_executor);
	g_test_add_func("/ai-glib/input/cwd-shared",
	                test_working_directory_reaches_the_executor);
	g_test_add_func("/ai-glib/input/clear-todos",
	                test_clear_empties_the_todo_list);
	g_test_add_func("/ai-glib/input/todo-one-block",
	                test_todo_updates_reuse_one_block);

	g_test_add_func("/ai-glib/input/agents-off-by-default",
	                test_background_agents_are_off_until_enabled);
	g_test_add_func("/ai-glib/input/agents-enable",
	                test_enabling_installs_a_working_brigade);
	g_test_add_func("/ai-glib/input/agents-block-and-signal",
	                test_agents_get_a_block_and_a_signal);
	g_test_add_func("/ai-glib/input/agents-survive-clear",
	                test_clear_empties_the_panel_but_not_the_brigade);

	status = g_test_run();

	rm_rf(sandbox);
	g_free(sandbox);

	return status;
}
