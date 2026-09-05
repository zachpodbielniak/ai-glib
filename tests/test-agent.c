/*
 * test-agent.c - Agent runtime: budgets, brigade, and the tool loop
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The tool-use loop had no test before AiMockProvider existed, because
 * exercising it meant a network call and a bill.  Several of these are
 * therefore the first coverage that code has had.
 */

#include <ai-glib.h>
#include <glib.h>
#include <string.h>

/* ── Budget ──────────────────────────────────────────────────────── */

static void
test_budget_limits (void)
{
    g_autoptr(AiBudget) b = ai_budget_new();
    const gchar *reason = NULL;

    /* Zero means unlimited, so a fresh budget stops nothing. */
    g_assert_false(ai_budget_exceeded(b, NULL));

    ai_budget_set_max_turns(b, 3);
    ai_budget_add_turn(b);
    ai_budget_add_turn(b);
    g_assert_false(ai_budget_exceeded(b, NULL));
    ai_budget_add_turn(b);
    g_assert_true(ai_budget_exceeded(b, &reason));
    g_assert_cmpstr(reason, ==, "turn limit");
}

static void
test_budget_each_dimension (void)
{
    const gchar *reason = NULL;

    {
        g_autoptr(AiBudget) b = ai_budget_new();
        ai_budget_set_max_input_tokens(b, 100);
        ai_budget_add_usage(b, 100, 0, 0);
        g_assert_true(ai_budget_exceeded(b, &reason));
        g_assert_cmpstr(reason, ==, "input token limit");
    }
    {
        g_autoptr(AiBudget) b = ai_budget_new();
        ai_budget_set_max_output_tokens(b, 50);
        ai_budget_add_usage(b, 0, 50, 0);
        g_assert_true(ai_budget_exceeded(b, &reason));
        g_assert_cmpstr(reason, ==, "output token limit");
    }
    {
        g_autoptr(AiBudget) b = ai_budget_new();
        ai_budget_set_max_cost_micros(b, 1000);
        ai_budget_add_usage(b, 0, 0, 999);
        g_assert_false(ai_budget_exceeded(b, NULL));
        ai_budget_add_usage(b, 0, 0, 1);
        g_assert_true(ai_budget_exceeded(b, &reason));
        g_assert_cmpstr(reason, ==, "cost limit");
    }
}

static void
test_budget_copy_is_independent (void)
{
    g_autoptr(AiBudget) a = ai_budget_new();
    g_autoptr(AiBudget) b = NULL;

    ai_budget_set_max_turns(a, 5);
    ai_budget_add_turn(a);
    b = ai_budget_copy(a);

    ai_budget_add_turn(a);
    g_assert_cmpuint(ai_budget_get_turns(a), ==, 2);
    g_assert_cmpuint(ai_budget_get_turns(b), ==, 1);
}

/* ── Price table ─────────────────────────────────────────────────── */

static void
test_price_table (void)
{
    g_autoptr(AiPriceTable) t = ai_price_table_new();
    gint64 cost;

    /* $3/Mtok in, $15/Mtok out */
    ai_price_table_set(t, "some-model", 3.0, 15.0);

    g_assert_true(ai_price_table_is_priced(t, "some-model"));
    /* case-insensitive: providers are inconsistent, and a miss would
     * look like a missing table entry rather than a typo */
    g_assert_true(ai_price_table_is_priced(t, "SOME-MODEL"));

    /* 1M in + 1M out = $3 + $15 = $18 = 18,000,000 micro-dollars */
    cost = ai_price_table_cost_micros(t, "some-model", 1000000, 1000000);
    g_assert_cmpint(cost, ==, 18000000);

    /* An unpriced model is reported as such, never as free: the two are
     * indistinguishable once summed, and "free" understates spend. */
    g_assert_false(ai_price_table_is_priced(t, "unknown"));
    g_assert_cmpint(ai_price_table_cost_micros(t, "unknown", 1000, 1000),
                    ==, -1);
}

/* ── States ──────────────────────────────────────────────────────── */

static void
test_agent_states (void)
{
    g_assert_cmpstr(ai_agent_state_to_string(AI_AGENT_STATE_RUNNING), ==,
                    "running");
    g_assert_cmpint(ai_agent_state_from_string("over-budget"), ==,
                    AI_AGENT_STATE_OVER_BUDGET);

    g_assert_true(ai_agent_state_is_live(AI_AGENT_STATE_RUNNING));
    g_assert_true(ai_agent_state_is_live(AI_AGENT_STATE_WAITING_INPUT));
    g_assert_false(ai_agent_state_is_live(AI_AGENT_STATE_DONE));

    g_assert_true(ai_agent_state_is_terminal(AI_AGENT_STATE_CANCELLED));
    g_assert_true(ai_agent_state_is_terminal(AI_AGENT_STATE_OVER_BUDGET));

    /* Interrupted is neither: it means "was running when we stopped",
     * which is not an outcome and must not read as one. */
    g_assert_false(ai_agent_state_is_terminal(AI_AGENT_STATE_INTERRUPTED));
    g_assert_false(ai_agent_state_is_live(AI_AGENT_STATE_INTERRUPTED));
}

/* ── Agent ───────────────────────────────────────────────────────── */

typedef struct { guint state_changes; guint finished; AiAgentState last; }
    AgentCounters;

static void
on_state_changed (AiAgent *a, gint old, gint new_state, gpointer ud)
{
    AgentCounters *c = ud;
    (void)a; (void)old;
    c->state_changes++;
    c->last = (AiAgentState)new_state;
}

static void
on_finished (AiAgent *a, gint state, gpointer ud)
{
    AgentCounters *c = ud;
    (void)a; (void)state;
    c->finished++;
}

static void
test_agent_lifecycle (void)
{
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiAgent) agent = ai_agent_new("a1", AI_PROVIDER(mock));
    AgentCounters c = { 0, 0, 0 };

    g_signal_connect(agent, "state-changed", G_CALLBACK(on_state_changed), &c);
    g_signal_connect(agent, "finished", G_CALLBACK(on_finished), &c);

    g_assert_cmpstr(ai_agent_get_id(agent), ==, "a1");
    g_assert_nonnull(ai_agent_get_executor(agent));
    g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_IDLE);

    ai_agent_set_state(agent, AI_AGENT_STATE_RUNNING);
    ai_agent_set_state(agent, AI_AGENT_STATE_RUNNING);   /* no-op */
    g_assert_cmpuint(c.state_changes, ==, 1);
    g_assert_cmpuint(c.finished, ==, 0);

    ai_agent_set_state(agent, AI_AGENT_STATE_DONE);
    g_assert_cmpuint(c.finished, ==, 1);

    /* ::finished fires exactly once, however many terminal moves follow */
    ai_agent_set_state(agent, AI_AGENT_STATE_FAILED);
    g_assert_cmpuint(c.finished, ==, 1);
}

static void
test_agent_cancel_is_always_allowed (void)
{
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiAgent) agent = ai_agent_new("a1", AI_PROVIDER(mock));

    ai_agent_set_state(agent, AI_AGENT_STATE_RUNNING);
    ai_agent_cancel(agent);

    g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_CANCELLED);
    g_assert_true(g_cancellable_is_cancelled(ai_agent_get_cancellable(agent)));
}

static void
test_agent_unpriced_turn_does_not_reduce_cost (void)
{
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiAgent) agent = ai_agent_new("a1", AI_PROVIDER(mock));

    ai_agent_record_turn(agent, 10, 5, 1000);
    /* -1 means unpriced; adding it would subtract from the total */
    ai_agent_record_turn(agent, 10, 5, -1);

    g_assert_cmpint(ai_budget_get_cost_micros(ai_agent_get_budget(agent)),
                    ==, 1000);
    g_assert_cmpuint(ai_budget_get_turns(ai_agent_get_budget(agent)), ==, 2);
}

/* ── Brigade ─────────────────────────────────────────────────────── */

static void
test_brigade_membership (void)
{
    g_autoptr(AiBrigade) b = ai_brigade_new();
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiAgent) a1 = ai_agent_new("a1", AI_PROVIDER(mock));
    g_autoptr(AiAgent) a2 = ai_agent_new("a2", AI_PROVIDER(mock));
    g_autoptr(GList) all = NULL;

    g_assert_true(ai_brigade_add(b, a1));
    g_assert_true(ai_brigade_add(b, a2));
    /* a duplicate id is refused rather than silently replacing */
    g_assert_false(ai_brigade_add(b, a1));

    g_assert_true(ai_brigade_get(b, "a1") == a1);
    g_assert_null(ai_brigade_get(b, "nope"));

    all = ai_brigade_list(b);
    g_assert_cmpuint(g_list_length(all), ==, 2);

    g_assert_true(ai_brigade_remove(b, "a1"));
    g_assert_false(ai_brigade_remove(b, "a1"));
}

static void
test_brigade_concurrency_cap (void)
{
    g_autoptr(AiBrigade) b = ai_brigade_new();
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiAgent) a1 = ai_agent_new("a1", AI_PROVIDER(mock));
    g_autoptr(AiAgent) a2 = ai_agent_new("a2", AI_PROVIDER(mock));

    ai_brigade_set_max_concurrent(b, 1);
    ai_brigade_add(b, a1);
    ai_brigade_add(b, a2);

    g_assert_true(ai_brigade_can_start(b));
    ai_agent_set_state(a1, AI_AGENT_STATE_RUNNING);
    g_assert_cmpuint(ai_brigade_count_live(b), ==, 1);
    g_assert_false(ai_brigade_can_start(b));

    ai_agent_set_state(a1, AI_AGENT_STATE_DONE);
    g_assert_true(ai_brigade_can_start(b));
}

static void
test_brigade_budget_blocks_start (void)
{
    g_autoptr(AiBrigade) b = ai_brigade_new();

    ai_budget_set_max_cost_micros(ai_brigade_get_budget(b), 100);
    g_assert_true(ai_brigade_can_start(b));

    /* One runaway agent is caught by its own limit; fifty well-behaved
     * ones are caught only by the shared one. */
    ai_budget_add_usage(ai_brigade_get_budget(b), 0, 0, 100);
    g_assert_false(ai_brigade_can_start(b));
}

static void
test_brigade_cancel_all_and_interrupt (void)
{
    g_autoptr(AiBrigade) b = ai_brigade_new();
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiAgent) a1 = ai_agent_new("a1", AI_PROVIDER(mock));
    g_autoptr(AiAgent) a2 = ai_agent_new("a2", AI_PROVIDER(mock));

    ai_brigade_set_max_concurrent(b, 0);   /* unlimited */
    ai_brigade_add(b, a1);
    ai_brigade_add(b, a2);
    ai_agent_set_state(a1, AI_AGENT_STATE_RUNNING);
    ai_agent_set_state(a2, AI_AGENT_STATE_RUNNING);

    g_assert_cmpuint(ai_brigade_cancel_all(b), ==, 2);
    g_assert_cmpuint(ai_brigade_count_live(b), ==, 0);

    /* interrupt only touches live agents, and these are already
     * cancelled */
    ai_agent_set_state(a1, AI_AGENT_STATE_RUNNING);
    g_assert_cmpuint(ai_brigade_interrupt_live(b), ==, 1);
    g_assert_cmpint(ai_agent_get_state(a1), ==, AI_AGENT_STATE_INTERRUPTED);
}

/* ── The tool loop, at last ──────────────────────────────────────── */

typedef struct { GMainLoop *loop; gchar *result; GError *error; } RunResult;

static void
on_run_done (GObject *source, GAsyncResult *res, gpointer ud)
{
    RunResult *r = ud;
    r->result = ai_tool_executor_run_finish(AI_TOOL_EXECUTOR(source), res,
                                            &r->error);
    g_main_loop_quit(r->loop);
}

static gchar *
echo_tool (AiToolUse *tool_use, GCancellable *cancellable, GError **error,
           gpointer user_data)
{
    (void)cancellable; (void)error;
    *(guint *)user_data += 1;
    {
        const gchar *text = ai_tool_use_get_input_string(tool_use, "text");
        return g_strdup(text ? text : "(none)");
    }
}

static void
test_tool_loop_async (void)
{
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
    g_autoptr(AiTool) tool = ai_tool_new("echo", "Echo the input");
    g_autoptr(AiMessage) msg = ai_message_new_user("go");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = NULL;
    RunResult r = { NULL, NULL, NULL };
    guint calls = 0;

    ai_tool_add_parameter(tool, "text", "string", "What to echo", TRUE);
    ai_tool_executor_register_callback(exec, tool, echo_tool, &calls, NULL);

    /* one tool call, then a final answer */
    ai_mock_provider_push_tool_use(mock, "echo", "{\"text\":\"hello\"}");
    ai_mock_provider_push_text(mock, "all done");

    messages = g_list_append(NULL, msg);
    r.loop = loop;

    ai_tool_executor_run_async(exec, AI_PROVIDER(mock), messages, NULL,
                               0, 0, NULL, on_run_done, &r);
    g_main_loop_run(loop);

    g_assert_no_error(r.error);
    g_assert_cmpstr(r.result, ==, "all done");
    g_assert_cmpuint(calls, ==, 1);
    /* two provider round trips: the tool call and the answer */
    g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 2);

    g_free(r.result);
    g_list_free(messages);
}

static void
test_tool_loop_respects_max_turns (void)
{
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
    g_autoptr(AiTool) tool = ai_tool_new("echo", "Echo");
    g_autoptr(AiMessage) msg = ai_message_new_user("go");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = NULL;
    RunResult r = { NULL, NULL, NULL };
    guint calls = 0;
    gint i;

    ai_tool_add_parameter(tool, "text", "string", "What", TRUE);
    ai_tool_executor_register_callback(exec, tool, echo_tool, &calls, NULL);

    /* a model that never stops calling tools */
    for (i = 0; i < 20; i++)
        ai_mock_provider_push_tool_use(mock, "echo", "{\"text\":\"x\"}");

    messages = g_list_append(NULL, msg);
    r.loop = loop;

    ai_tool_executor_run_async(exec, AI_PROVIDER(mock), messages, NULL,
                               0, 3, NULL, on_run_done, &r);
    g_main_loop_run(loop);

    /* The cap is the caller's, not a constant: an orchestrator budgets
     * turns per agent, and a limit it cannot set is one it has to work
     * around.
     *
     * It counts provider round trips -- the thing that costs money --
     * and fires on receiving the Nth, so a cap of 3 means 3 requests
     * were made and the third reply was refused rather than acted on.
     * Two tool calls therefore ran, not three. */
    g_assert_nonnull(r.error);
    g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 3);
    g_assert_cmpuint(calls, ==, 2);
    g_assert_true(strstr(r.error->message, "turn limit") != NULL);
    g_clear_error(&r.error);
    g_free(r.result);
    g_list_free(messages);
}

static void
test_tool_loop_propagates_provider_error (void)
{
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("go");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = NULL;
    RunResult r = { NULL, NULL, NULL };

    ai_mock_provider_push_error(mock, "upstream exploded");

    messages = g_list_append(NULL, msg);
    r.loop = loop;

    ai_tool_executor_run_async(exec, AI_PROVIDER(mock), messages, NULL,
                               0, 0, NULL, on_run_done, &r);
    g_main_loop_run(loop);

    g_assert_nonnull(r.error);
    g_assert_null(r.result);
    g_assert_true(strstr(r.error->message, "exploded") != NULL);
    g_clear_error(&r.error);
    g_list_free(messages);
}

static void
test_concurrent_agents_do_not_interfere (void)
{
    /* The whole reason the async API exists.  Two agents, each with its
     * own executor, running at the same time on one thread. */
    g_autoptr(AiMockProvider) m1 = ai_mock_provider_new();
    g_autoptr(AiMockProvider) m2 = ai_mock_provider_new();
    g_autoptr(AiAgent) a1 = ai_agent_new("a1", AI_PROVIDER(m1));
    g_autoptr(AiAgent) a2 = ai_agent_new("a2", AI_PROVIDER(m2));
    g_autoptr(AiMessage) msg1 = ai_message_new_user("one");
    g_autoptr(AiMessage) msg2 = ai_message_new_user("two");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *l1 = NULL, *l2 = NULL;
    RunResult r1 = { NULL, NULL, NULL }, r2 = { NULL, NULL, NULL };

    /* Delays overlap the runs; without them the first finishes before
     * the second starts and nothing is actually concurrent. */
    ai_mock_provider_set_delay_ms(m1, 30);
    ai_mock_provider_set_delay_ms(m2, 10);
    ai_mock_provider_push_text(m1, "from one");
    ai_mock_provider_push_text(m2, "from two");

    l1 = g_list_append(NULL, msg1);
    l2 = g_list_append(NULL, msg2);
    r1.loop = loop;
    r2.loop = loop;

    ai_tool_executor_run_async(ai_agent_get_executor(a1), AI_PROVIDER(m1),
                               l1, NULL, 0, 0, NULL, on_run_done, &r1);
    ai_tool_executor_run_async(ai_agent_get_executor(a2), AI_PROVIDER(m2),
                               l2, NULL, 0, 0, NULL, on_run_done, &r2);

    while (r1.result == NULL || r2.result == NULL)
        g_main_context_iteration(NULL, TRUE);

    g_assert_cmpstr(r1.result, ==, "from one");
    g_assert_cmpstr(r2.result, ==, "from two");

    g_free(r1.result);
    g_free(r2.result);
    g_list_free(l1);
    g_list_free(l2);
}

static void
test_sync_run_still_works (void)
{
    /* The async refactor must not have changed the blocking API. */
    g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
    g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("go");
    g_autoptr(GError) error = NULL;
    g_autofree gchar *result = NULL;
    GList *messages = g_list_append(NULL, msg);

    ai_mock_provider_push_text(mock, "sync answer");
    result = ai_tool_executor_run(exec, AI_PROVIDER(mock), messages,
                                  NULL, 0, NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpstr(result, ==, "sync answer");
    g_list_free(messages);
}

static void
test_agent_outlives_brigade (void)
{
    g_autoptr(AiAgent) agent = ai_agent_new ("retained", NULL);
    g_autoptr(AiBrigade) brigade = ai_brigade_new ();
    gpointer weak_brigade = brigade;

    g_object_add_weak_pointer (G_OBJECT (brigade), &weak_brigade);
    g_assert_true (ai_brigade_add (brigade, agent));
    g_clear_object (&brigade);
    g_assert_null (weak_brigade);

    /* Both signals used to call into the brigade after its final unref. */
    ai_agent_record_turn (agent, 10, 20, 0);
    ai_agent_set_state (agent, AI_AGENT_STATE_DONE);
    g_assert_cmpint (ai_agent_get_state (agent), ==, AI_AGENT_STATE_DONE);
}

int
main (int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ai-glib/brigade/agent-outlives-brigade", test_agent_outlives_brigade);

    g_test_add_func("/ai-glib/budget/limits", test_budget_limits);
    g_test_add_func("/ai-glib/budget/dimensions", test_budget_each_dimension);
    g_test_add_func("/ai-glib/budget/copy", test_budget_copy_is_independent);
    g_test_add_func("/ai-glib/price-table/basics", test_price_table);
    g_test_add_func("/ai-glib/agent/states", test_agent_states);
    g_test_add_func("/ai-glib/agent/lifecycle", test_agent_lifecycle);
    g_test_add_func("/ai-glib/agent/cancel", test_agent_cancel_is_always_allowed);
    g_test_add_func("/ai-glib/agent/unpriced",
                    test_agent_unpriced_turn_does_not_reduce_cost);
    g_test_add_func("/ai-glib/brigade/membership", test_brigade_membership);
    g_test_add_func("/ai-glib/brigade/concurrency", test_brigade_concurrency_cap);
    g_test_add_func("/ai-glib/brigade/budget", test_brigade_budget_blocks_start);
    g_test_add_func("/ai-glib/brigade/cancel-interrupt",
                    test_brigade_cancel_all_and_interrupt);
    g_test_add_func("/ai-glib/tool-loop/async", test_tool_loop_async);
    g_test_add_func("/ai-glib/tool-loop/max-turns",
                    test_tool_loop_respects_max_turns);
    g_test_add_func("/ai-glib/tool-loop/provider-error",
                    test_tool_loop_propagates_provider_error);
    g_test_add_func("/ai-glib/tool-loop/concurrent",
                    test_concurrent_agents_do_not_interfere);
    g_test_add_func("/ai-glib/tool-loop/sync-still-works",
                    test_sync_run_still_works);

    return g_test_run();
}
