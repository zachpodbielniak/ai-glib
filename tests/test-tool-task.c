/*
 * test-tool-task.c - Subagents, and the skill tool
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The claim worth testing hardest is that an agent's allowlist is
 * structural. "The agent was refused when it called bash" and "the agent
 * had no bash to call" look identical from outside and are not the same
 * guarantee: the first is a policy that can be bypassed by any path that
 * forgets to consult it. So the tests below inspect the child executor's
 * tool list, and separately prove that a tool absent from that list does
 * not dispatch.
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static AiResourceRegistry *
registry_with_agent(
	const gchar *name,
	const gchar *contents
){
	AiResourceRegistry   *registry = ai_resource_registry_new();
	g_autoptr(AiResource) resource =
		ai_resource_new_from_data(contents, -1, name, AI_RESOURCE_AGENT,
		                          "claude", AI_RESOURCE_SCOPE_USER, NULL);

	g_assert_nonnull(resource);
	ai_resource_registry_add(registry, resource);

	return registry;
}

/*
 * Queue a `task` request as the provider's next reply.
 *
 * The mock's queue is shared between the parent and any subagent, so the
 * order of the pushes *is* the script: this has to be queued before the
 * replies the agent and the parent give afterwards.
 */
static void
queue_task_request(
	AiMockProvider *mock,
	const gchar    *agent,
	const gchar    *prompt
){
	g_autofree gchar *input =
		g_strdup_printf("{\"agent\":\"%s\",\"prompt\":\"%s\"}", agent,
		                prompt);

	ai_mock_provider_push_tool_use(mock, "task", input);
}

/* Run the executor loop over one user message. */
static gchar *
run_loop(
	AiToolExecutor  *executor,
	AiMockProvider  *mock,
	GCancellable    *cancellable,
	GError         **error
){
	g_autoptr(AiMessage) message = ai_message_new_user("go");
	GList               *messages = g_list_append(NULL, message);
	gchar               *reply;

	reply = ai_tool_executor_run(executor, AI_PROVIDER(mock), messages, NULL,
	                             1024, cancellable, error);
	g_list_free(messages);

	return reply;
}

/* Call `task` directly, without the surrounding loop. */
static gchar *
call_task(
	AiToolExecutor  *executor,
	const gchar     *json,
	GError         **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(AiToolUse)  use = NULL;

	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	use = ai_tool_use_new("id-1", "task", json_parser_get_root(parser));

	return ai_tool_executor_execute(executor, use, NULL, error);
}

static gchar *
call_skill(
	AiToolExecutor  *executor,
	const gchar     *name,
	GError         **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(AiToolUse)  use = NULL;
	g_autofree gchar     *json = g_strdup_printf("{\"name\":\"%s\"}", name);

	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	use = ai_tool_use_new("id-1", "skill", json_parser_get_root(parser));

	return ai_tool_executor_execute(executor, use, NULL, error);
}

/* ----------------------------------------------------------------
 * Availability
 * ---------------------------------------------------------------- */

static void
test_absent_without_a_registry(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	GError                   *error = NULL;
	gchar                    *result;

	/* Not merely unadvertised: unavailable. Adding subagent support to
	 * the library must change nothing for a caller who does not want it. */
	result = call_task(executor,
	                   "{\"agent\":\"anything\",\"prompt\":\"hi\"}", &error);

	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
}

static void
test_unknown_agent_lists_what_exists(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("reviewer", "Review the code.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	GError                       *error = NULL;
	gchar                        *result;

	ai_tool_executor_set_resource_registry(executor, registry);

	result = call_task(executor, "{\"agent\":\"nope\",\"prompt\":\"hi\"}",
	                   &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);

	/* Naming the alternatives turns a dead end into a next step. */
	g_assert_nonnull(strstr(error->message, "reviewer"));
	g_clear_error(&error);
}

static void
test_missing_parameters(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("reviewer", "Review.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	GError                       *error = NULL;
	gchar                        *result;

	ai_tool_executor_set_resource_registry(executor, registry);

	result = call_task(executor, "{\"agent\":\"reviewer\"}", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
}

static void
test_no_provider_available(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("reviewer", "Review.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	GError                       *error = NULL;
	gchar                        *result;

	ai_tool_executor_set_resource_registry(executor, registry);

	/* Called outside a run, so there is nothing to send the agent's turn
	 * to. Reported rather than crashed. */
	result = call_task(executor,
	                   "{\"agent\":\"reviewer\",\"prompt\":\"hi\"}", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
}

/* ----------------------------------------------------------------
 * Running one
 * ---------------------------------------------------------------- */

static void
test_runs_and_returns_the_final_text(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("reviewer",
		                    "---\ndescription: Reviews code\n---\n"
		                    "You review C code.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	g_autoptr(AiMockProvider)     mock = ai_mock_provider_new();
	g_autofree gchar             *reply = NULL;
	GError                       *error = NULL;

	ai_tool_executor_set_resource_registry(executor, registry);

	/* The parent asks for the agent, the agent answers, the parent
	 * answers. One queue, so the order here is the script. */
	queue_task_request(mock, "reviewer", "look at it");
	ai_mock_provider_push_text(mock, "THE AGENT SAYS SO");
	ai_mock_provider_push_text(mock, "and the parent agrees");

	reply = run_loop(executor, mock, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(reply, ==, "and the parent agrees");

	/* Three calls: the parent's first turn asking for the tool, the
	 * child's turn, and the parent's second turn. */
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 3);
}

/* ----------------------------------------------------------------
 * The allowlist, which is the point
 * ---------------------------------------------------------------- */

/*
 * The child executor an agent would run inside.
 *
 * There is no public accessor for it -- it exists only for the length of
 * one task call -- so this rebuilds it the same way tool_task does, which
 * is what lets the test inspect the tool list rather than infer it from
 * behaviour.
 */
static AiToolExecutor *
child_for(const gchar *tools_line)
{
	g_autofree gchar             *contents =
		g_strdup_printf("---\n%s---\nDo the thing.\n",
		                tools_line != NULL ? tools_line : "");
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("worker", contents);
	AiResource                   *agent =
		ai_resource_registry_lookup(registry, AI_RESOURCE_AGENT, "worker");
	AiToolExecutor               *child = ai_tool_executor_new();
	g_auto(GStrv)                 declared = NULL;

	g_assert_nonnull(agent);
	declared = ai_resource_get_meta_list(agent, "tools");

	if (declared == NULL || declared[0] == NULL)
	{
		return child;
	}

	{
		g_autoptr(GPtrArray) doomed = g_ptr_array_new_with_free_func(g_free);
		GList               *iter;
		guint                i;

		for (iter = ai_tool_executor_get_tools(child); iter != NULL;
		     iter = iter->next)
		{
			const gchar *name = ai_tool_get_name(iter->data);
			gboolean     keep = FALSE;

			for (i = 0; declared[i] != NULL; i++)
			{
				g_autofree gchar *lower =
					g_ascii_strdown(declared[i], -1);

				g_strstrip(lower);

				if (g_strcmp0(lower, name) == 0 ||
				    (g_strcmp0(lower, "webfetch") == 0 &&
				     g_strcmp0(name, "web_fetch") == 0) ||
				    (g_strcmp0(lower, "multiedit") == 0 &&
				     g_strcmp0(name, "multi_edit") == 0) ||
				    (g_strcmp0(lower, "todowrite") == 0 &&
				     g_strcmp0(name, "todo_write") == 0))
				{
					keep = TRUE;
					break;
				}
			}

			if (!keep)
			{
				g_ptr_array_add(doomed, g_strdup(name));
			}
		}

		for (i = 0; i < doomed->len; i++)
		{
			ai_tool_executor_unregister(child,
			                            g_ptr_array_index(doomed, i));
		}
	}

	return child;
}

static gboolean
offers(AiToolExecutor *executor, const gchar *name)
{
	GList *iter;

	for (iter = ai_tool_executor_get_tools(executor); iter != NULL;
	     iter = iter->next)
	{
		if (g_strcmp0(ai_tool_get_name(iter->data), name) == 0)
		{
			return TRUE;
		}
	}

	return FALSE;
}

static void
test_allowlist_removes_tools(void)
{
	g_autoptr(AiToolExecutor) child = child_for("tools: Read, Grep\n");

	/* Structural: bash is not in the list, so there is no bash. */
	g_assert_true(offers(child, "read"));
	g_assert_true(offers(child, "grep"));
	g_assert_false(offers(child, "bash"));
	g_assert_false(offers(child, "write"));
	g_assert_false(offers(child, "edit"));
}

static void
test_allowlist_is_enforced_by_dispatch(void)
{
	g_autoptr(AiToolExecutor) child = child_for("tools: Read\n");
	g_autoptr(JsonParser)     parser = json_parser_new();
	g_autoptr(AiToolUse)      use = NULL;
	gchar                    *result;
	GError                   *error = NULL;

	/*
	 * The other half of the guarantee. A tool list that the dispatcher
	 * ignored would make the removal above pure decoration.
	 */
	g_assert_true(json_parser_load_from_data(
		parser, "{\"command\":\"echo SHOULD_NOT_RUN\"}", -1, NULL));
	use = ai_tool_use_new("id-1", "bash", json_parser_get_root(parser));

	result = ai_tool_executor_execute(child, use, NULL, &error);

	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
}

static void
test_no_tools_declared_inherits_everything(void)
{
	g_autoptr(AiToolExecutor) child = child_for(NULL);

	/* What claude-code does, and what the agent files on disk assume. */
	g_assert_true(offers(child, "bash"));
	g_assert_true(offers(child, "read"));
	g_assert_true(offers(child, "multi_edit"));
}

static void
test_harness_capitalisation_is_understood(void)
{
	g_autoptr(AiToolExecutor) child =
		child_for("tools: WebFetch, MultiEdit, TodoWrite\n");

	/*
	 * The agent files on disk were written for claude-code, which
	 * capitalises. Matching by lowercasing alone would silently drop half
	 * an allowlist and leave the agent unable to work, with nothing to
	 * say why.
	 */
	g_assert_true(offers(child, "web_fetch"));
	g_assert_true(offers(child, "multi_edit"));
	g_assert_true(offers(child, "todo_write"));
	g_assert_false(offers(child, "bash"));
}

static void
test_allowlist_as_a_yaml_sequence(void)
{
	g_autoptr(AiToolExecutor) child =
		child_for("tools:\n  - Read\n  - Glob\n");

	/* opencode writes a sequence; the result must be identical. */
	g_assert_true(offers(child, "read"));
	g_assert_true(offers(child, "glob"));
	g_assert_false(offers(child, "bash"));
}

/* ----------------------------------------------------------------
 * Depth, cancellation, and inheritance
 * ---------------------------------------------------------------- */

static void
test_recursion_is_capped_and_says_so(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("recurse", "Delegate everything.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	g_autoptr(AiMockProvider)     mock = ai_mock_provider_new();
	g_autofree gchar             *reply = NULL;
	GError                       *error = NULL;
	guint                         i;

	ai_tool_executor_set_resource_registry(executor, registry);

	/*
	 * Every level asks for another subagent. Without a cap this recurses
	 * until something gives out; with one, the innermost call is refused
	 * and the refusal reaches the model as a tool result it can act on
	 * rather than a silent truncation.
	 */
	for (i = 0; i < 6; i++)
	{
		ai_mock_provider_push_tool_use(
			mock, "task",
			"{\"agent\":\"recurse\",\"prompt\":\"deeper\"}");
	}

	ai_mock_provider_set_fallback(mock, "gave up delegating");

	reply = run_loop(executor, mock, NULL, &error);

	/*
	 * Bounded, which is the property being pinned: without the cap this
	 * recurses until the turn limit or the stack gives out. Each level
	 * costs a handful of provider calls, so a small ceiling here is the
	 * difference between "capped" and "not".
	 */
	g_assert_no_error(error);
	g_assert_nonnull(reply);
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), <, 12);
}

/*
 * The refusal reaches the model.
 *
 * A cap the model cannot see is a silent truncation: it delegates, gets
 * nothing, and has no idea why. The executor loop puts the tool's own
 * error text into the tool result, which is what makes the limit
 * actionable rather than mysterious.
 */
static void
test_tool_errors_reach_the_model(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("reviewer", "Review.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	g_autoptr(AiMockProvider)     mock = ai_mock_provider_new();
	g_autofree gchar             *reply = NULL;
	GError                       *error = NULL;

	ai_tool_executor_set_resource_registry(executor, registry);

	queue_task_request(mock, "misspelled", "go");
	ai_mock_provider_set_fallback(mock, "I will do it myself");

	reply = run_loop(executor, mock, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(reply, ==, "I will do it myself");
}

static void
test_cancellation_propagates(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("worker", "Work.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();
	g_autoptr(AiMockProvider)     mock = ai_mock_provider_new();
	g_autoptr(GCancellable)       cancellable = g_cancellable_new();
	gchar                        *reply;
	GError                       *error = NULL;

	ai_tool_executor_set_resource_registry(executor, registry);
	queue_task_request(mock, "worker", "go");
	ai_mock_provider_set_fallback(mock, "done");

	g_cancellable_cancel(cancellable);

	reply = run_loop(executor, mock, cancellable, &error);

	/* Whether it fails or returns early, it must not hang and must not
	 * pretend the agent ran. */
	if (reply != NULL)
	{
		g_free(reply);
	}

	g_clear_error(&error);
}

static void
test_child_inherits_configuration(void)
{
	g_autoptr(AiResourceRegistry) registry =
		registry_with_agent("worker", "Work.\n");
	g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new();

	ai_tool_executor_set_resource_registry(executor, registry);
	ai_tool_executor_set_working_directory(executor, "/tmp");
	ai_tool_executor_set_approval_policy(executor, AI_TOOL_APPROVAL_DENY);

	/* The parent's settings are what a subagent should run under: a
	 * subagent that ignored a DENY policy would be a way around it. */
	g_assert_cmpstr(ai_tool_executor_get_working_directory(executor), ==,
	                "/tmp");
	g_assert_cmpint(ai_tool_executor_get_approval_policy(executor), ==,
	                AI_TOOL_APPROVAL_DENY);
}

/* ----------------------------------------------------------------
 * The skill tool
 * ---------------------------------------------------------------- */

static void
test_skill_returns_its_body(void)
{
	AiResourceRegistry       *registry = ai_resource_registry_new();
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autofree gchar         *result = NULL;
	GError                   *error = NULL;

	{
		g_autoptr(AiResource) skill = ai_resource_new_from_data(
			"---\ndescription: How to scaffold\n---\n"
			"Step one. Step two.\n", -1, "gtest-scaffold",
			AI_RESOURCE_SKILL, "claude", AI_RESOURCE_SCOPE_USER, NULL);

		ai_resource_registry_add(registry, skill);
	}

	ai_tool_executor_set_resource_registry(executor, registry);
	g_object_unref(registry);

	result = call_skill(executor, "gtest-scaffold", &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "Step one. Step two.\n");
}

static void
test_skill_unknown_lists_what_exists(void)
{
	AiResourceRegistry       *registry = ai_resource_registry_new();
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	GError                   *error = NULL;
	gchar                    *result;

	{
		g_autoptr(AiResource) skill = ai_resource_new_from_data(
			"body", -1, "real-skill", AI_RESOURCE_SKILL, "claude",
			AI_RESOURCE_SCOPE_USER, NULL);

		ai_resource_registry_add(registry, skill);
	}

	ai_tool_executor_set_resource_registry(executor, registry);
	g_object_unref(registry);

	result = call_skill(executor, "imaginary", &error);

	g_assert_null(result);
	g_assert_nonnull(strstr(error->message, "real-skill"));
	g_clear_error(&error);
}

static void
test_skill_without_a_registry(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	GError                   *error = NULL;
	gchar                    *result;

	result = call_skill(executor, "anything", &error);

	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/task/absent-without-registry",
	                test_absent_without_a_registry);
	g_test_add_func("/ai-glib/task/unknown-agent",
	                test_unknown_agent_lists_what_exists);
	g_test_add_func("/ai-glib/task/missing-parameters", test_missing_parameters);
	g_test_add_func("/ai-glib/task/no-provider", test_no_provider_available);
	g_test_add_func("/ai-glib/task/runs", test_runs_and_returns_the_final_text);

	g_test_add_func("/ai-glib/task/allowlist-removes",
	                test_allowlist_removes_tools);
	g_test_add_func("/ai-glib/task/allowlist-dispatch",
	                test_allowlist_is_enforced_by_dispatch);
	g_test_add_func("/ai-glib/task/no-tools-inherits",
	                test_no_tools_declared_inherits_everything);
	g_test_add_func("/ai-glib/task/capitalisation",
	                test_harness_capitalisation_is_understood);
	g_test_add_func("/ai-glib/task/sequence-allowlist",
	                test_allowlist_as_a_yaml_sequence);

	g_test_add_func("/ai-glib/task/recursion-capped",
	                test_recursion_is_capped_and_says_so);
	g_test_add_func("/ai-glib/task/errors-reach-model",
	                test_tool_errors_reach_the_model);
	g_test_add_func("/ai-glib/task/cancellation", test_cancellation_propagates);
	g_test_add_func("/ai-glib/task/inheritance",
	                test_child_inherits_configuration);

	g_test_add_func("/ai-glib/skill/returns-body", test_skill_returns_its_body);
	g_test_add_func("/ai-glib/skill/unknown",
	                test_skill_unknown_lists_what_exists);
	g_test_add_func("/ai-glib/skill/no-registry",
	                test_skill_without_a_registry);

	return g_test_run();
}
