/*
 * test-tool-agent.c - The background-agent tools, and the feature gate
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Two claims here are worth more than the rest.
 *
 * The first is the grant: an executor that was not handed a brigade has
 * no agent_* tools, and a call to one does not merely go unadvertised,
 * it does not dispatch. Adding background agents to the library must
 * change nothing for an application that did not ask for them.
 *
 * The second is the notification. A background agent finishes while the
 * model is doing something else, and the only moment a model can be told
 * anything is a turn boundary --- so the brigade holds the news until
 * the loop collects it, and the loop appends it before the next turn.
 * That is asserted through the real loop rather than by calling the
 * helper, because the bug that matters is "the news never reached the
 * model", not "the string was formatted wrongly".
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	GMainLoop *loop;
	guint      finished;
	guint      wanted;
	guint      give_up_id;
} Watch;

static void
on_agent_finished(AiBrigade *brigade, const gchar *id, gint state,
                  gpointer user_data)
{
	Watch *watch = user_data;

	(void)brigade; (void)id; (void)state;

	watch->finished++;

	if (watch->loop != NULL && watch->finished >= watch->wanted)
		g_main_loop_quit(watch->loop);
}

static gboolean
on_give_up(gpointer user_data)
{
	Watch *watch = user_data;

	watch->give_up_id = 0;
	g_main_loop_quit(watch->loop);
	return G_SOURCE_REMOVE;
}

static void
wait_for(Watch *watch, guint wanted)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	watch->wanted = wanted;

	if (watch->finished >= wanted) return;

	watch->loop = loop;
	watch->give_up_id = g_timeout_add_seconds(5, on_give_up, watch);

	g_main_loop_run(loop);

	if (watch->give_up_id != 0)
	{
		g_source_remove(watch->give_up_id);
		watch->give_up_id = 0;
	}

	watch->loop = NULL;
}

static AiBrigade *
brigade_with_worker(Watch *watch)
{
	AiBrigade               *brigade = ai_brigade_new();
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();

	ai_brigade_set_worker(brigade, AI_AGENT_WORKER(worker));

	if (watch != NULL)
		g_signal_connect(brigade, "agent-finished",
		                 G_CALLBACK(on_agent_finished), watch);

	return brigade;
}

/* Call one tool directly, without the surrounding loop. */
static gchar *
call_tool(
	AiToolExecutor  *executor,
	const gchar     *name,
	const gchar     *json,
	GError         **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(AiToolUse)  use = NULL;

	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	use = ai_tool_use_new("id-1", name, json_parser_get_root(parser));

	return ai_tool_executor_execute(executor, use, NULL, error);
}

static gboolean
offers(AiToolExecutor *executor, const gchar *name)
{
	GList *iter;

	for (iter = ai_tool_executor_get_tools(executor); iter != NULL;
	     iter = iter->next)
	{
		if (g_strcmp0(ai_tool_get_name(iter->data), name) == 0) return TRUE;
	}

	return FALSE;
}

/* Run the executor's loop over one user message. */
static gchar *
run_loop(
	AiToolExecutor  *executor,
	AiMockProvider  *mock,
	GError         **error
){
	g_autoptr(AiMessage) message = ai_message_new_user("go");
	GList               *messages = g_list_append(NULL, message);
	gchar               *reply;

	reply = ai_tool_executor_run(executor, AI_PROVIDER(mock), messages, NULL,
	                             1024, NULL, error);
	g_list_free(messages);

	return reply;
}

/* ----------------------------------------------------------------
 * The grant
 * ---------------------------------------------------------------- */

/*
 * Without a brigade there are no agent tools, and calling one is not
 * refused --- it is unrepresentable.
 */
static void
test_absent_without_a_brigade(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	GError                   *error = NULL;
	gchar                    *result;

	g_assert_false(offers(executor, "agent_spawn"));
	g_assert_false(offers(executor, "agent_status"));
	g_assert_false(offers(executor, "agent_result"));
	g_assert_false(offers(executor, "agent_wait"));
	g_assert_false(offers(executor, "agent_cancel"));

	result = call_tool(executor, "agent_spawn", "{\"prompt\":\"hi\"}", &error);

	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
}

static void
test_present_with_a_brigade(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);

	ai_tool_executor_set_brigade(executor, brigade);

	g_assert_true(offers(executor, "agent_spawn"));
	g_assert_true(offers(executor, "agent_status"));
	g_assert_true(offers(executor, "agent_result"));
	g_assert_true(offers(executor, "agent_wait"));
	g_assert_true(offers(executor, "agent_cancel"));
}

/* Clearing the brigade takes them away again. */
static void
test_clearing_the_brigade_removes_them(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);

	ai_tool_executor_set_brigade(executor, brigade);
	ai_tool_executor_set_brigade(executor, NULL);

	g_assert_false(offers(executor, "agent_spawn"));
}

/*
 * The feature bit refuses the group even when the brigade is there.
 *
 * That is the switch an embedder reaches for: it has a brigade for its
 * own purposes and does not want the model driving it.
 */
static void
test_feature_bit_withholds_the_group(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);

	ai_tool_executor_set_features(executor, AI_TOOL_FEATURE_SUBAGENTS);
	ai_tool_executor_set_brigade(executor, brigade);

	g_assert_false(offers(executor, "agent_spawn"));

	/* Turning it back on brings them, since the brigade is still set. */
	ai_tool_executor_set_features(executor, AI_TOOL_FEATURE_ALL);
	g_assert_true(offers(executor, "agent_spawn"));
}

static void
test_feature_bit_withholds_subagents(void)
{
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();

	ai_tool_executor_set_features(executor, AI_TOOL_FEATURE_BACKGROUND);
	ai_tool_executor_set_resource_registry(executor, registry);

	g_assert_false(offers(executor, "task"));
	g_assert_false(offers(executor, "skill"));

	ai_tool_executor_set_features(executor, AI_TOOL_FEATURE_ALL);
	g_assert_true(offers(executor, "task"));
	g_assert_true(offers(executor, "skill"));
}

static void
test_features_default_to_all(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();

	g_assert_cmpint(ai_tool_executor_get_features(executor), ==,
	                AI_TOOL_FEATURE_ALL);
}

/* The property is reachable from bindings and from `ai --set`. */
static void
test_features_as_a_property(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	guint                     value = 0;

	g_object_set(executor, "features", (guint)AI_TOOL_FEATURE_NONE, NULL);
	g_object_get(executor, "features", &value, NULL);

	g_assert_cmpuint(value, ==, AI_TOOL_FEATURE_NONE);
}

/* ----------------------------------------------------------------
 * agent_spawn
 * ---------------------------------------------------------------- */

static void
test_spawn_needs_a_prompt(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);
	GError                   *error = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	g_assert_null(call_tool(executor, "agent_spawn", "{}", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
}

/*
 * Spawning registers an agent and returns; the turn carries on.
 *
 * That the tool does not block is covered where it can be observed
 * directly --- see the agent_result-on-a-live-agent and agent_wait
 * timeout cases, both of which require an agent to still be running
 * after a tool call returned.
 *
 * The parent and the agent draw replies from the same mock queue, in
 * request order, so the pushes below *are* the script: the parent asks
 * to spawn, the agent answers, the parent answers.
 */
static void
test_spawn_registers_an_agent(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autofree gchar         *reply = NULL;
	g_autoptr(GList)          agents = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	ai_mock_provider_push_tool_use(
		mock, "agent_spawn",
		"{\"prompt\":\"look into it\",\"description\":\"the look\"}");
	ai_mock_provider_push_text(mock, "the agent's answer");
	ai_mock_provider_push_text(mock, "started it");
	ai_mock_provider_set_fallback(mock, "nothing more to say");

	reply = run_loop(executor, mock, NULL);
	g_assert_nonnull(reply);

	agents = ai_brigade_list(brigade);
	g_assert_nonnull(agents);
	g_assert_cmpstr(ai_agent_get_description(agents->data), ==, "the look");

	wait_for(&watch, 1);
}

/* The description is what a status listing shows; without one, the
 * prompt stands in rather than leaving the row blank. */
static void
test_spawn_falls_back_to_the_prompt_for_a_description(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autofree gchar         *reply = NULL;
	g_autoptr(GList)          agents = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	ai_mock_provider_push_tool_use(mock, "agent_spawn",
	                               "{\"prompt\":\"count the files\"}");
	ai_mock_provider_push_text(mock, "ok");

	reply = run_loop(executor, mock, NULL);

	agents = ai_brigade_list(brigade);
	g_assert_nonnull(agents);
	g_assert_cmpstr(ai_agent_get_description(agents->data), ==,
	                "count the files");

	wait_for(&watch, 1);
}

/* An unknown agent name names the ones that exist, so the model can
 * correct itself rather than guess again. */
static void
test_spawn_unknown_agent_lists_what_exists(void)
{
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)          brigade = brigade_with_worker(NULL);
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiResource)         resource = NULL;
	GError                       *error = NULL;

	resource = ai_resource_new_from_data("Review the code.\n", -1, "reviewer",
	                                     AI_RESOURCE_AGENT, "claude",
	                                     AI_RESOURCE_SCOPE_USER, NULL);
	ai_resource_registry_add(registry, resource);

	ai_tool_executor_set_brigade(executor, brigade);
	ai_tool_executor_set_resource_registry(executor, registry);

	g_assert_null(call_tool(executor, "agent_spawn",
	                        "{\"prompt\":\"hi\",\"agent\":\"reviewr\"}",
	                        &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "reviewer"));
	g_clear_error(&error);
}

/*
 * An unknown provider is reported, not silently redirected.
 *
 * ai_provider_type_from_string() answers CLAUDE for anything it does not
 * recognise, so a model asking for "gpt5" would otherwise get an agent
 * running on Claude and no indication that its request was ignored.
 */
static void
test_spawn_unknown_provider_errors(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);
	GError                   *error = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	g_assert_null(call_tool(executor, "agent_spawn",
	                        "{\"prompt\":\"hi\",\"provider\":\"nonesuch\"}",
	                        &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
}

/*
 * Naming a model without a provider is refused.
 *
 * A model id means nothing on its own --- "sonnet" is not a thing an
 * arbitrary provider can be set to --- and quietly ignoring it would
 * run the agent on something other than what was asked for.
 */
static void
test_spawn_model_without_provider_errors(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autofree gchar         *reply = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	ai_mock_provider_push_tool_use(
		mock, "agent_spawn",
		"{\"prompt\":\"hi\",\"model\":\"some-model\"}");
	ai_mock_provider_push_text(mock, "understood");

	reply = run_loop(executor, mock, NULL);

	/* The error reaches the model as the tool's result, so the run
	 * continues rather than collapsing. */
	g_assert_cmpstr(reply, ==, "understood");
	g_assert_null(ai_brigade_list(brigade));
}

/*
 * A background agent may not spawn further background agents.
 *
 * One level of fan-out is delegation. Agents spawning agents with nobody
 * watching is a fork bomb that bills, so the child's executor is built
 * without the group at all.
 */
static void
test_spawned_agents_cannot_spawn(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autofree gchar         *reply = NULL;
	g_autoptr(GList)          agents = NULL;
	AiToolExecutor           *child;

	ai_tool_executor_set_brigade(executor, brigade);

	ai_mock_provider_push_tool_use(mock, "agent_spawn",
	                               "{\"prompt\":\"do it\"}");
	ai_mock_provider_push_text(mock, "started");
	ai_mock_provider_push_text(mock, "agent done");

	reply = run_loop(executor, mock, NULL);
	g_assert_nonnull(reply);

	agents = ai_brigade_list(brigade);
	g_assert_nonnull(agents);

	child = ai_agent_get_executor(agents->data);
	g_assert_false(offers(child, "agent_spawn"));
	g_assert_cmpint(ai_tool_executor_get_features(child) &
	                AI_TOOL_FEATURE_BACKGROUND, ==, 0);

	wait_for(&watch, 1);
}

/* ----------------------------------------------------------------
 * agent_status, agent_result, agent_cancel
 * ---------------------------------------------------------------- */

static void
test_status_with_nothing_running(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);
	g_autofree gchar         *result = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	result = call_tool(executor, "agent_status", "{}", NULL);

	g_assert_nonnull(result);
	g_assert_nonnull(strstr(result, "No background agents"));
}

static void
test_status_names_the_agent_and_its_work(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(mock, "hi");
	agent = ai_agent_new("a1", AI_PROVIDER(mock));
	ai_agent_set_description(agent, "audit the search code");

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);

	wait_for(&watch, 1);

	result = call_tool(executor, "agent_status", "{}", NULL);

	g_assert_nonnull(strstr(result, "a1"));
	g_assert_nonnull(strstr(result, "done"));
	g_assert_nonnull(strstr(result, "audit the search code"));
}

static void
test_status_of_an_unknown_agent_errors(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(NULL);
	GError                   *error = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	g_assert_null(call_tool(executor, "agent_status",
	                        "{\"agent_id\":\"nobody\"}", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
}

static void
test_result_collects_and_reaps(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(mock, "here is what I found");
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);

	wait_for(&watch, 1);

	result = call_tool(executor, "agent_result",
	                   "{\"agent_id\":\"a1\"}", NULL);

	g_assert_cmpstr(result, ==, "here is what I found");

	/* Reaped: asking again says so rather than repeating the answer. */
	g_assert_null(ai_brigade_get(brigade, "a1"));
}

static void
test_result_of_a_live_agent_errors(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	GError                   *error = NULL;

	ai_mock_provider_push_text(mock, "eventually");
	ai_mock_provider_set_delay_ms(mock, 200);
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);

	g_assert_null(call_tool(executor, "agent_result",
	                        "{\"agent_id\":\"a1\"}", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	wait_for(&watch, 1);
}

static void
test_cancel_stops_one(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(mock, "never");
	ai_mock_provider_set_delay_ms(mock, 200);
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);

	result = call_tool(executor, "agent_cancel",
	                   "{\"agent_id\":\"a1\"}", NULL);

	g_assert_nonnull(result);
	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_CANCELLED);

	wait_for(&watch, 1);
}

static void
test_cancel_all(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) one = ai_mock_provider_new();
	g_autoptr(AiMockProvider) two = ai_mock_provider_new();
	g_autoptr(AiAgent)        a = NULL;
	g_autoptr(AiAgent)        b = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(one, "never");
	ai_mock_provider_push_text(two, "never");
	ai_mock_provider_set_delay_ms(one, 200);
	ai_mock_provider_set_delay_ms(two, 200);

	a = ai_agent_new("a1", AI_PROVIDER(one));
	b = ai_agent_new("a2", AI_PROVIDER(two));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, a, "go", NULL);
	ai_brigade_start(brigade, b, "go", NULL);

	result = call_tool(executor, "agent_cancel", "{\"agent_id\":\"all\"}",
	                   NULL);

	g_assert_nonnull(strstr(result, "2"));

	wait_for(&watch, 2);
}

/* Cancelling something already finished says so rather than failing:
 * the caller wanted it stopped, and it is. */
static void
test_cancel_a_finished_agent_is_not_an_error(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *result = NULL;
	GError                   *error = NULL;

	ai_mock_provider_push_text(mock, "hi");
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);
	wait_for(&watch, 1);

	result = call_tool(executor, "agent_cancel", "{\"agent_id\":\"a1\"}",
	                   &error);

	g_assert_nonnull(result);
	g_assert_no_error(error);
}

/* ----------------------------------------------------------------
 * agent_wait
 * ---------------------------------------------------------------- */

/* An agent that has already finished is answered without spinning a
 * loop at all. */
static void
test_wait_on_a_finished_agent_returns_at_once(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(mock, "the answer");
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);
	wait_for(&watch, 1);

	result = call_tool(executor, "agent_wait", "{\"agent_id\":\"a1\"}", NULL);

	g_assert_cmpstr(result, ==, "the answer");
}

static void
test_wait_blocks_until_it_finishes(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(mock, "worth the wait");
	ai_mock_provider_set_delay_ms(mock, 50);
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);

	result = call_tool(executor, "agent_wait", "{\"agent_id\":\"a1\"}", NULL);

	g_assert_nonnull(result);
	g_assert_nonnull(strstr(result, "worth the wait"));
}

/*
 * A timeout is not a failure.
 *
 * The agent is still working, and saying so is more useful than an error
 * the model has to interpret --- and the agent must certainly not be
 * stopped by somebody giving up on waiting for it.
 */
static void
test_wait_timeout_reports_still_running(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(mock, "eventually");
	ai_mock_provider_set_delay_ms(mock, 3000);
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, agent, "go", NULL);

	result = call_tool(executor, "agent_wait",
	                   "{\"agent_id\":\"a1\",\"timeout_seconds\":1}", NULL);

	g_assert_nonnull(result);
	g_assert_nonnull(strstr(result, "Still running"));
	g_assert_true(ai_agent_state_is_live(ai_agent_get_state(agent)));

	ai_agent_cancel(agent);
	wait_for(&watch, 1);
}

static void
test_wait_for_whichever_finishes_first(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) quick = ai_mock_provider_new();
	g_autoptr(AiMockProvider) slow = ai_mock_provider_new();
	g_autoptr(AiAgent)        a = NULL;
	g_autoptr(AiAgent)        b = NULL;
	g_autofree gchar         *result = NULL;

	ai_mock_provider_push_text(quick, "quick answer");
	ai_mock_provider_set_delay_ms(quick, 30);
	ai_mock_provider_push_text(slow, "slow answer");
	ai_mock_provider_set_delay_ms(slow, 400);

	a = ai_agent_new("slow-one", AI_PROVIDER(slow));
	b = ai_agent_new("quick-one", AI_PROVIDER(quick));

	ai_tool_executor_set_brigade(executor, brigade);
	ai_brigade_start(brigade, a, "go", NULL);
	ai_brigade_start(brigade, b, "go", NULL);

	result = call_tool(executor, "agent_wait", "{}", NULL);

	g_assert_nonnull(strstr(result, "quick answer"));

	wait_for(&watch, 2);
}

/* ----------------------------------------------------------------
 * Notification
 * ---------------------------------------------------------------- */

/*
 * The model is told, at the next turn boundary, that an agent finished.
 *
 * Driven through the real loop: the failure that matters is the news
 * never arriving, and a test of the formatting helper would not notice
 * that. The model spawns, is told it started, answers, and the finish
 * arrives as a further turn rather than being lost.
 */
static void
test_the_model_is_told_when_an_agent_finishes(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autofree gchar         *reply = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	/*
	 * Turn 1: spawn. Turn 2: the agent's own reply, taken from the same
	 * queue. Turn 3: the parent says it started. Turn 4: having been told
	 * the agent finished, it answers again.
	 */
	ai_mock_provider_push_tool_use(mock, "agent_spawn",
	                               "{\"prompt\":\"look\",\"description\":\"a look\"}");
	ai_mock_provider_push_text(mock, "the agent's finding");
	ai_mock_provider_push_text(mock, "I started an agent");
	ai_mock_provider_push_text(mock, "and it has now finished");

	reply = run_loop(executor, mock, NULL);

	/*
	 * The last thing said is the answer given *after* the notice. If the
	 * finish had not been reported, the run would have ended at "I
	 * started an agent".
	 */
	g_assert_cmpstr(reply, ==, "and it has now finished");
	g_assert_cmpuint(watch.finished, ==, 1);
}

/* News already collected is not repeated: a run that reaped an agent
 * itself must not then be told about it. */
static void
test_a_reaped_agent_is_not_announced(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autofree gchar         *reply = NULL;

	ai_tool_executor_set_brigade(executor, brigade);

	ai_mock_provider_push_tool_use(mock, "agent_spawn",
	                               "{\"prompt\":\"look\"}");
	ai_mock_provider_push_text(mock, "the finding");
	ai_mock_provider_push_tool_use(mock, "agent_wait", "{}");
	ai_mock_provider_push_text(mock, "collected it");

	reply = run_loop(executor, mock, NULL);

	/* agent_wait reaped it, so nothing is left to announce and the run
	 * ends on the answer rather than going round again. */
	g_assert_cmpstr(reply, ==, "collected it");
	g_assert_null(ai_brigade_list(brigade));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/agent-tools/absent-without-brigade",
	                test_absent_without_a_brigade);
	g_test_add_func("/ai-glib/agent-tools/present-with-brigade",
	                test_present_with_a_brigade);
	g_test_add_func("/ai-glib/agent-tools/clearing-removes",
	                test_clearing_the_brigade_removes_them);
	g_test_add_func("/ai-glib/agent-tools/feature-withholds-background",
	                test_feature_bit_withholds_the_group);
	g_test_add_func("/ai-glib/agent-tools/feature-withholds-subagents",
	                test_feature_bit_withholds_subagents);
	g_test_add_func("/ai-glib/agent-tools/features-default",
	                test_features_default_to_all);
	g_test_add_func("/ai-glib/agent-tools/features-property",
	                test_features_as_a_property);

	g_test_add_func("/ai-glib/agent-tools/spawn-needs-prompt",
	                test_spawn_needs_a_prompt);
	g_test_add_func("/ai-glib/agent-tools/spawn-registers",
	                test_spawn_registers_an_agent);
	g_test_add_func("/ai-glib/agent-tools/spawn-description-fallback",
	                test_spawn_falls_back_to_the_prompt_for_a_description);
	g_test_add_func("/ai-glib/agent-tools/spawn-unknown-agent",
	                test_spawn_unknown_agent_lists_what_exists);
	g_test_add_func("/ai-glib/agent-tools/spawn-unknown-provider",
	                test_spawn_unknown_provider_errors);
	g_test_add_func("/ai-glib/agent-tools/spawn-model-needs-provider",
	                test_spawn_model_without_provider_errors);
	g_test_add_func("/ai-glib/agent-tools/spawned-cannot-spawn",
	                test_spawned_agents_cannot_spawn);

	g_test_add_func("/ai-glib/agent-tools/status-empty",
	                test_status_with_nothing_running);
	g_test_add_func("/ai-glib/agent-tools/status-names-work",
	                test_status_names_the_agent_and_its_work);
	g_test_add_func("/ai-glib/agent-tools/status-unknown",
	                test_status_of_an_unknown_agent_errors);

	g_test_add_func("/ai-glib/agent-tools/result-reaps",
	                test_result_collects_and_reaps);
	g_test_add_func("/ai-glib/agent-tools/result-live",
	                test_result_of_a_live_agent_errors);

	g_test_add_func("/ai-glib/agent-tools/cancel-one", test_cancel_stops_one);
	g_test_add_func("/ai-glib/agent-tools/cancel-all", test_cancel_all);
	g_test_add_func("/ai-glib/agent-tools/cancel-finished",
	                test_cancel_a_finished_agent_is_not_an_error);

	g_test_add_func("/ai-glib/agent-tools/wait-finished",
	                test_wait_on_a_finished_agent_returns_at_once);
	g_test_add_func("/ai-glib/agent-tools/wait-blocks",
	                test_wait_blocks_until_it_finishes);
	g_test_add_func("/ai-glib/agent-tools/wait-timeout",
	                test_wait_timeout_reports_still_running);
	g_test_add_func("/ai-glib/agent-tools/wait-any",
	                test_wait_for_whichever_finishes_first);

	g_test_add_func("/ai-glib/agent-tools/notification",
	                test_the_model_is_told_when_an_agent_finishes);
	g_test_add_func("/ai-glib/agent-tools/reaped-not-announced",
	                test_a_reaped_agent_is_not_announced);

	return g_test_run();
}
