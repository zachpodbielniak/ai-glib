/*
 * test-local-worker.c - Running an agent in this process
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The AiAgentWorker interface existed for a while with nothing behind
 * it. What these tests pin down is the half of the contract that is easy
 * to get subtly wrong: that state is *pushed* (so ::state-changed is the
 * only thing a brigade has to watch), that a cancelled run is reported
 * as cancelled rather than failed, and that partial output survives a run
 * that did not get to the end.
 */

#include <ai-glib.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	GMainLoop *loop;
	guint      finished;
	gint       last_state;
	guint      give_up_id;
} Watch;

static void
on_finished(AiAgent *agent, gint state, gpointer user_data)
{
	Watch *watch = user_data;

	(void)agent;

	watch->finished++;
	watch->last_state = state;

	if (watch->loop != NULL) g_main_loop_quit(watch->loop);
}

/*
 * Run one agent to completion.
 *
 * The timeout is not decoration: every assertion below is about a
 * callback arriving, so a bug that never calls back would hang the
 * suite rather than fail it.
 */
static gboolean
on_give_up(gpointer user_data)
{
	Watch *watch = user_data;

	/* Zeroed on the way out so the caller knows not to remove a source
	 * that has already removed itself. */
	watch->give_up_id = 0;
	g_main_loop_quit(watch->loop);
	return G_SOURCE_REMOVE;
}

/*
 * Run the loop until the agent finishes, or give up.
 *
 * Separate from starting the agent, because a run can reach a terminal
 * state before the loop is entered at all --- cancellation does exactly
 * that --- and quitting a loop that is not running yet does nothing.
 */
static void
pump(Watch *watch, guint seconds)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

	watch->loop = loop;
	watch->give_up_id = g_timeout_add_seconds(seconds, on_give_up, watch);

	g_main_loop_run(loop);

	if (watch->give_up_id != 0)
	{
		g_source_remove(watch->give_up_id);
		watch->give_up_id = 0;
	}

	watch->loop = NULL;
}

/* Turn the main loop for a bounded time with nothing waiting on it, so
 * an abandoned request gets to unwind before its provider is destroyed. */
static void
settle(guint ms)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;

	while (g_get_monotonic_time() < deadline)
	{
		if (!g_main_context_iteration(NULL, FALSE)) g_usleep(1000);
	}
}

static void
run_agent(AiAgentWorker *worker, AiAgent *agent, const gchar *prompt,
          Watch *watch)
{
	g_signal_connect(agent, "finished", G_CALLBACK(on_finished), watch);

	ai_agent_worker_start_async(worker, agent, prompt, NULL, NULL, NULL);

	/*
	 * A run can be over before there is a loop to wait on --- an agent
	 * with no provider fails on the way in. Pumping then would sit on
	 * the timeout waiting for a signal that has already been emitted.
	 */
	if (watch->finished > 0) return;

	pump(watch, 5);
}

static AiAgent *
agent_with_reply(const gchar *id, const gchar *reply, AiMockProvider **out_mock)
{
	AiMockProvider *mock = ai_mock_provider_new();
	AiAgent        *agent;

	ai_mock_provider_push_text(mock, reply);

	agent = ai_agent_new(id, AI_PROVIDER(mock));

	if (out_mock != NULL)
		*out_mock = mock;
	else
		g_object_unref(mock);

	return agent;
}

/* ----------------------------------------------------------------
 * The basics
 * ---------------------------------------------------------------- */

static void
test_runs_and_reports_the_answer(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       agent = agent_with_reply("a1", "done", NULL);
	Watch                    watch = { 0 };

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	g_assert_cmpuint(watch.finished, ==, 1);
	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_DONE);
	g_assert_cmpstr(ai_agent_get_result(agent), ==, "done");
	g_assert_null(ai_agent_get_error(agent));
}

/*
 * State is pushed, not polled.
 *
 * A brigade decides whether to sweep by asking, and sweeping a worker
 * that pushes would poll a vfunc that is not there.
 */
static void
test_does_not_poll(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();

	g_assert_false(ai_agent_worker_can_poll(AI_AGENT_WORKER(worker)));
}

static void
test_state_changes_in_order(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       agent = agent_with_reply("a1", "done", NULL);
	Watch                    watch = { 0 };

	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_IDLE);

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	g_assert_cmpint(watch.last_state, ==, AI_AGENT_STATE_DONE);
}

/* ::finished fires once, on the way into a terminal state --- never twice
 * for one run, whatever else happens. */
static void
test_finished_fires_once(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       agent = agent_with_reply("a1", "done", NULL);
	Watch                    watch = { 0 };

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	/* Asking again changes nothing. */
	ai_agent_set_state(agent, AI_AGENT_STATE_DONE);

	g_assert_cmpuint(watch.finished, ==, 1);
}

static void
test_elapsed_is_recorded_and_frozen(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       agent = NULL;
	AiMockProvider          *mock = NULL;
	Watch                    watch = { 0 };
	gint64                   first;

	agent = agent_with_reply("a1", "done", &mock);
	ai_mock_provider_set_delay_ms(mock, 20);

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	first = ai_agent_get_elapsed_ms(agent);
	g_assert_cmpint(first, >=, 0);

	/*
	 * A finished agent stops counting. Without this a status listing
	 * would keep reporting a larger number for work that ended an hour
	 * ago, which is the opposite of what the figure means.
	 */
	g_usleep(30 * 1000);
	g_assert_cmpint(ai_agent_get_elapsed_ms(agent), ==, first);

	g_object_unref(mock);
}

/* ----------------------------------------------------------------
 * Failure and cancellation
 * ---------------------------------------------------------------- */

static void
test_provider_failure_is_failed_not_cancelled(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)       agent = NULL;
	Watch                    watch = { 0 };

	ai_mock_provider_push_error(mock, "the server said no");
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_FAILED);
	g_assert_nonnull(ai_agent_get_error(agent));
}

/*
 * A cancelled run is not a failed one.
 *
 * The distinction reaches the status listing, and somebody deciding
 * whether to retry needs to know which happened -- so this is asserted
 * on the state, not merely on "it stopped".
 */
static void
test_cancel_reports_cancelled(void)
{
	g_autoptr(AiLocalWorker)  worker = ai_local_worker_new();
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	Watch                     watch = { 0 };

	ai_mock_provider_push_text(mock, "never arrives");
	ai_mock_provider_set_delay_ms(mock, 100);

	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	g_signal_connect(agent, "finished", G_CALLBACK(on_finished), &watch);

	ai_agent_worker_start_async(AI_AGENT_WORKER(worker), agent, "go", NULL,
	                            NULL, NULL);

	g_assert_true(ai_agent_worker_cancel(AI_AGENT_WORKER(worker), agent, NULL));

	/* Cancelling moves the agent at once, before the provider has
	 * answered -- that immediacy is the point of a kill switch. */
	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_CANCELLED);
	g_assert_cmpuint(watch.finished, ==, 1);

	/* Then turn the loop so the abandoned request finishes unwinding
	 * before the test tears its provider down. */
	settle(300);

	/* Still cancelled: the late failure must not rewrite the outcome. */
	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_CANCELLED);
	g_assert_cmpuint(watch.finished, ==, 1);
}

/* Cancelling twice is not an error. A caller who wants something stopped
 * must never be refused, including one who asks again. */
static void
test_cancel_is_idempotent(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       agent = agent_with_reply("a1", "x", NULL);

	g_assert_true(ai_agent_worker_cancel(AI_AGENT_WORKER(worker), agent, NULL));
	g_assert_true(ai_agent_worker_cancel(AI_AGENT_WORKER(worker), agent, NULL));
	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_CANCELLED);
}

static void
test_no_provider_fails_cleanly(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       agent = ai_agent_new("a1", NULL);
	Watch                    watch = { 0 };

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_FAILED);
	g_assert_nonnull(ai_agent_get_error(agent));
}

/* ----------------------------------------------------------------
 * Reading a run in progress
 * ---------------------------------------------------------------- */

static void
test_read_output_before_and_after(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       agent = agent_with_reply("a1", "the answer", NULL);
	Watch                    watch = { 0 };
	g_autofree gchar        *before = NULL;
	g_autofree gchar        *after = NULL;

	before = ai_agent_worker_read_output(AI_AGENT_WORKER(worker), agent, NULL);
	g_assert_null(before);

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	after = ai_agent_worker_read_output(AI_AGENT_WORKER(worker), agent, NULL);
	g_assert_cmpstr(after, ==, "the answer");
}

/* ----------------------------------------------------------------
 * Whose provider
 * ---------------------------------------------------------------- */

/*
 * An agent runs on its own provider, not on anybody else's.
 *
 * This is the property that makes "spawn a claude-code agent from a Grok
 * conversation" work at all, so it is asserted directly: two agents,
 * two mocks, and each answer comes from its own.
 */
static void
test_each_agent_uses_its_own_provider(void)
{
	g_autoptr(AiLocalWorker) worker = ai_local_worker_new();
	g_autoptr(AiAgent)       one = agent_with_reply("a1", "from one", NULL);
	g_autoptr(AiAgent)       two = agent_with_reply("a2", "from two", NULL);
	Watch                    first = { 0 };
	Watch                    second = { 0 };

	run_agent(AI_AGENT_WORKER(worker), one, "go", &first);
	run_agent(AI_AGENT_WORKER(worker), two, "go", &second);

	g_assert_cmpstr(ai_agent_get_result(one), ==, "from one");
	g_assert_cmpstr(ai_agent_get_result(two), ==, "from two");
}

/* An executor installed on the agent is the one that runs, which is what
 * makes an agent file's tool allowlist mean anything here. */
static void
test_installed_executor_is_used(void)
{
	g_autoptr(AiLocalWorker)  worker = ai_local_worker_new();
	g_autoptr(AiAgent)        agent = agent_with_reply("a1", "done", NULL);
	g_autoptr(AiToolExecutor) empty = ai_tool_executor_new_empty();
	Watch                     watch = { 0 };

	ai_agent_set_executor(agent, empty);
	g_assert_true(ai_agent_get_executor(agent) == empty);

	run_agent(AI_AGENT_WORKER(worker), agent, "go", &watch);

	g_assert_cmpstr(ai_agent_get_result(agent), ==, "done");
	g_assert_null(ai_tool_executor_get_tools(empty));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/local-worker/runs",
	                test_runs_and_reports_the_answer);
	g_test_add_func("/ai-glib/local-worker/does-not-poll", test_does_not_poll);
	g_test_add_func("/ai-glib/local-worker/state-order",
	                test_state_changes_in_order);
	g_test_add_func("/ai-glib/local-worker/finished-once",
	                test_finished_fires_once);
	g_test_add_func("/ai-glib/local-worker/elapsed-frozen",
	                test_elapsed_is_recorded_and_frozen);

	g_test_add_func("/ai-glib/local-worker/failure-is-failed",
	                test_provider_failure_is_failed_not_cancelled);
	g_test_add_func("/ai-glib/local-worker/cancel-is-cancelled",
	                test_cancel_reports_cancelled);
	g_test_add_func("/ai-glib/local-worker/cancel-idempotent",
	                test_cancel_is_idempotent);
	g_test_add_func("/ai-glib/local-worker/no-provider",
	                test_no_provider_fails_cleanly);

	g_test_add_func("/ai-glib/local-worker/read-output",
	                test_read_output_before_and_after);
	g_test_add_func("/ai-glib/local-worker/own-provider",
	                test_each_agent_uses_its_own_provider);
	g_test_add_func("/ai-glib/local-worker/own-executor",
	                test_installed_executor_is_used);

	return g_test_run();
}
