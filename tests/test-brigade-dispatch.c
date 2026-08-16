/*
 * test-brigade-dispatch.c - Starting, queueing, reaping
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The brigade used to track agents without ever starting one. These
 * tests cover the half that was missing, and three claims in particular:
 * that the concurrency limit queues rather than drops, that a finish is
 * remembered until somebody collects it (which is the whole notification
 * mechanism), and that reaping is what forgets an agent.
 */

#include <ai-glib.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	GMainLoop *loop;
	guint      wanted;
	guint      finished;
	guint      idle;
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

static void
on_idle(AiBrigade *brigade, gpointer user_data)
{
	Watch *watch = user_data;

	(void)brigade;
	watch->idle++;
}

static gboolean
on_give_up(gpointer user_data)
{
	Watch *watch = user_data;

	watch->give_up_id = 0;
	g_main_loop_quit(watch->loop);
	return G_SOURCE_REMOVE;
}

/* Run until @wanted agents have finished, or give up. */
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
	AiBrigade                *brigade = ai_brigade_new();
	g_autoptr(AiLocalWorker)  worker = ai_local_worker_new();

	ai_brigade_set_worker(brigade, AI_AGENT_WORKER(worker));

	if (watch != NULL)
	{
		g_signal_connect(brigade, "agent-finished",
		                 G_CALLBACK(on_agent_finished), watch);
		g_signal_connect(brigade, "idle", G_CALLBACK(on_idle), watch);
	}

	return brigade;
}

static AiAgent *
agent_saying(const gchar *id, const gchar *reply, guint delay_ms)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	AiAgent                  *agent;

	ai_mock_provider_push_text(mock, reply);
	ai_mock_provider_set_delay_ms(mock, delay_ms);

	agent = ai_agent_new(id, AI_PROVIDER(mock));
	ai_agent_set_description(agent, "some work");

	return agent;
}

/* ----------------------------------------------------------------
 * Starting
 * ---------------------------------------------------------------- */

static void
test_start_needs_a_worker(void)
{
	g_autoptr(AiBrigade) brigade = ai_brigade_new();
	g_autoptr(AiAgent)   agent = agent_saying("a1", "hi", 0);
	g_autoptr(GError)    error = NULL;

	g_assert_false(ai_brigade_start(brigade, agent, "go", &error));
	g_assert_nonnull(error);
}

static void
test_start_runs_to_completion(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   agent = agent_saying("a1", "the answer", 0);

	g_assert_true(ai_brigade_start(brigade, agent, "go", NULL));

	wait_for(&watch, 1);

	g_assert_cmpint(ai_agent_get_state(agent), ==, AI_AGENT_STATE_DONE);
	g_assert_cmpstr(ai_agent_get_result(agent), ==, "the answer");
	g_assert_cmpuint(watch.idle, ==, 1);
}

/* Starting registers the agent, so a caller need not add it first. */
static void
test_start_registers(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   agent = agent_saying("a1", "hi", 0);

	g_assert_null(ai_brigade_get(brigade, "a1"));
	g_assert_true(ai_brigade_start(brigade, agent, "go", NULL));
	g_assert_true(ai_brigade_get(brigade, "a1") == agent);

	wait_for(&watch, 1);
}

static void
test_duplicate_id_is_refused(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   one = agent_saying("a1", "hi", 0);
	g_autoptr(AiAgent)   two = agent_saying("a1", "hi", 0);
	g_autoptr(GError)    error = NULL;

	g_assert_true(ai_brigade_start(brigade, one, "go", NULL));
	g_assert_false(ai_brigade_start(brigade, two, "go", &error));
	g_assert_nonnull(error);

	wait_for(&watch, 1);
}

/* ----------------------------------------------------------------
 * Concurrency
 * ---------------------------------------------------------------- */

/*
 * Beyond the limit, agents queue.
 *
 * The claim under test is that nothing is dropped: three agents against
 * a limit of one must all finish, one after another. A brigade that
 * silently refused the third would look identical until the results
 * were counted.
 */
static void
test_over_the_limit_queues(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   one = agent_saying("a1", "one", 20);
	g_autoptr(AiAgent)   two = agent_saying("a2", "two", 20);
	g_autoptr(AiAgent)   three = agent_saying("a3", "three", 20);

	ai_brigade_set_max_concurrent(brigade, 1);

	g_assert_true(ai_brigade_start(brigade, one, "go", NULL));
	g_assert_true(ai_brigade_start(brigade, two, "go", NULL));
	g_assert_true(ai_brigade_start(brigade, three, "go", NULL));

	/* Only one may be live; the others wait their turn. */
	g_assert_cmpuint(ai_brigade_count_live(brigade), ==, 1);
	g_assert_cmpint(ai_agent_get_state(two), ==, AI_AGENT_STATE_QUEUED);
	g_assert_cmpint(ai_agent_get_state(three), ==, AI_AGENT_STATE_QUEUED);

	wait_for(&watch, 3);

	g_assert_cmpstr(ai_agent_get_result(one), ==, "one");
	g_assert_cmpstr(ai_agent_get_result(two), ==, "two");
	g_assert_cmpstr(ai_agent_get_result(three), ==, "three");
}

/* Arrival order, not whatever the hash table happens to yield. */
static void
test_queue_is_first_in_first_out(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   one = agent_saying("a1", "one", 20);
	g_autoptr(AiAgent)   two = agent_saying("a2", "two", 20);
	g_autoptr(AiAgent)   three = agent_saying("a3", "three", 20);

	ai_brigade_set_max_concurrent(brigade, 1);

	ai_brigade_start(brigade, one, "go", NULL);
	ai_brigade_start(brigade, two, "go", NULL);
	ai_brigade_start(brigade, three, "go", NULL);

	wait_for(&watch, 1);

	/* When the first finishes, the second runs --- not the third. */
	g_assert_cmpint(ai_agent_get_state(three), ==, AI_AGENT_STATE_QUEUED);

	wait_for(&watch, 3);
}

static void
test_no_limit_starts_everything(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   one = agent_saying("a1", "one", 20);
	g_autoptr(AiAgent)   two = agent_saying("a2", "two", 20);

	ai_brigade_set_max_concurrent(brigade, 0);

	ai_brigade_start(brigade, one, "go", NULL);
	ai_brigade_start(brigade, two, "go", NULL);

	g_assert_cmpuint(ai_brigade_count_live(brigade), ==, 2);

	wait_for(&watch, 2);
}

/* ----------------------------------------------------------------
 * The notification queue
 * ---------------------------------------------------------------- */

/*
 * A finish is remembered until it is collected.
 *
 * This is the whole notification mechanism: the thing that most wants to
 * know an agent finished --- a conversation, between turns --- is not
 * listening at the moment it happens.
 */
static void
test_finishes_are_remembered(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   agent = agent_saying("a1", "hi", 0);
	g_autofree gchar    *id = NULL;

	ai_brigade_start(brigade, agent, "go", NULL);
	wait_for(&watch, 1);

	id = ai_brigade_take_finished(brigade);
	g_assert_cmpstr(id, ==, "a1");
}

/* Each finish is reported exactly once, so a caller draining in a loop
 * does not repeat itself forever. */
static void
test_a_finish_is_reported_once(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   agent = agent_saying("a1", "hi", 0);
	g_autofree gchar    *first = NULL;
	g_autofree gchar    *second = NULL;

	ai_brigade_start(brigade, agent, "go", NULL);
	wait_for(&watch, 1);

	first = ai_brigade_take_finished(brigade);
	second = ai_brigade_take_finished(brigade);

	g_assert_cmpstr(first, ==, "a1");
	g_assert_null(second);
}

static void
test_finishes_come_back_oldest_first(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   one = agent_saying("a1", "one", 10);
	g_autoptr(AiAgent)   two = agent_saying("a2", "two", 60);
	g_autofree gchar    *first = NULL;
	g_autofree gchar    *second = NULL;

	ai_brigade_start(brigade, one, "go", NULL);
	ai_brigade_start(brigade, two, "go", NULL);

	wait_for(&watch, 2);

	first = ai_brigade_take_finished(brigade);
	second = ai_brigade_take_finished(brigade);

	g_assert_cmpstr(first, ==, "a1");
	g_assert_cmpstr(second, ==, "a2");
}

static void
test_nothing_finished_yields_null(void)
{
	g_autoptr(AiBrigade) brigade = ai_brigade_new();

	g_assert_null(ai_brigade_take_finished(brigade));
}

/* ----------------------------------------------------------------
 * Reaping
 * ---------------------------------------------------------------- */

static void
test_reap_returns_the_answer_and_forgets(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   agent = agent_saying("a1", "the answer", 0);
	g_autofree gchar    *text = NULL;

	ai_brigade_start(brigade, agent, "go", NULL);
	wait_for(&watch, 1);

	text = ai_brigade_reap(brigade, "a1", NULL);

	g_assert_cmpstr(text, ==, "the answer");
	g_assert_null(ai_brigade_get(brigade, "a1"));
}

/*
 * Reaping a live agent is refused, not waited on.
 *
 * A call that blocked here would turn its caller into the run. The state
 * is available to check first, and ::agent-finished says when.
 */
static void
test_reap_refuses_a_live_agent(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   agent = agent_saying("a1", "hi", 100);
	g_autoptr(GError)    error = NULL;
	g_autofree gchar    *text = NULL;

	ai_brigade_start(brigade, agent, "go", NULL);

	text = ai_brigade_reap(brigade, "a1", &error);

	g_assert_null(text);
	g_assert_nonnull(error);

	wait_for(&watch, 1);
}

static void
test_reap_unknown_errors(void)
{
	g_autoptr(AiBrigade) brigade = ai_brigade_new();
	g_autoptr(GError)    error = NULL;

	g_assert_null(ai_brigade_reap(brigade, "nobody", &error));
	g_assert_nonnull(error);
}

/* A failed run still reaps: "what happened to it" is what was asked. */
static void
test_reap_a_failure_reports_why(void)
{
	Watch                     watch = { 0 };
	g_autoptr(AiBrigade)      brigade = brigade_with_worker(&watch);
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiAgent)        agent = NULL;
	g_autofree gchar         *text = NULL;

	ai_mock_provider_push_error(mock, "the server said no");
	agent = ai_agent_new("a1", AI_PROVIDER(mock));

	ai_brigade_start(brigade, agent, "go", NULL);
	wait_for(&watch, 1);

	text = ai_brigade_reap(brigade, "a1", NULL);

	g_assert_nonnull(text);
	g_assert_nonnull(strstr(text, "the server said no"));
}

/* Removing an agent takes its pending notification with it: announcing
 * the finish of something nobody can look up would be worse than
 * silence. */
static void
test_removing_drops_its_notification(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   agent = agent_saying("a1", "hi", 0);

	ai_brigade_start(brigade, agent, "go", NULL);
	wait_for(&watch, 1);

	g_assert_true(ai_brigade_remove(brigade, "a1"));
	g_assert_null(ai_brigade_take_finished(brigade));
}

/* ----------------------------------------------------------------
 * Ids
 * ---------------------------------------------------------------- */

static void
test_generated_ids_are_unique(void)
{
	g_autoptr(AiBrigade) brigade = ai_brigade_new();
	g_autofree gchar    *one = ai_brigade_generate_id(brigade, NULL);
	g_autofree gchar    *two = ai_brigade_generate_id(brigade, NULL);
	g_autofree gchar    *named = ai_brigade_generate_id(brigade, "reviewer");

	g_assert_cmpstr(one, !=, two);
	g_assert_true(g_str_has_prefix(one, "agent-"));
	g_assert_true(g_str_has_prefix(named, "reviewer-"));
}

/* A generated id never collides with one already registered, even when
 * the counter would otherwise have produced it. */
static void
test_generated_ids_skip_taken_ones(void)
{
	g_autoptr(AiBrigade) brigade = ai_brigade_new();
	g_autoptr(AiAgent)   squatter = agent_saying("agent-1", "hi", 0);
	g_autofree gchar    *fresh = NULL;

	g_assert_true(ai_brigade_add(brigade, squatter));

	fresh = ai_brigade_generate_id(brigade, NULL);
	g_assert_cmpstr(fresh, !=, "agent-1");
}

/* ----------------------------------------------------------------
 * Cancellation
 * ---------------------------------------------------------------- */

static void
test_cancel_all_stops_the_live_ones(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   one = agent_saying("a1", "one", 500);
	g_autoptr(AiAgent)   two = agent_saying("a2", "two", 500);

	ai_brigade_start(brigade, one, "go", NULL);
	ai_brigade_start(brigade, two, "go", NULL);

	g_assert_cmpuint(ai_brigade_cancel_all(brigade), ==, 2);

	g_assert_cmpint(ai_agent_get_state(one), ==, AI_AGENT_STATE_CANCELLED);
	g_assert_cmpint(ai_agent_get_state(two), ==, AI_AGENT_STATE_CANCELLED);

	/* Let the abandoned requests unwind before their providers go. */
	{
		gint64 deadline = g_get_monotonic_time() + 700 * 1000;

		while (g_get_monotonic_time() < deadline)
		{
			if (!g_main_context_iteration(NULL, FALSE)) g_usleep(1000);
		}
	}
}

/* A queued agent that is cancelled must not later be dispatched. */
static void
test_cancelling_a_queued_agent_does_not_start_it(void)
{
	Watch                watch = { 0 };
	g_autoptr(AiBrigade) brigade = brigade_with_worker(&watch);
	g_autoptr(AiAgent)   one = agent_saying("a1", "one", 20);
	g_autoptr(AiAgent)   two = agent_saying("a2", "two", 20);

	ai_brigade_set_max_concurrent(brigade, 1);

	ai_brigade_start(brigade, one, "go", NULL);
	ai_brigade_start(brigade, two, "go", NULL);

	g_assert_cmpint(ai_agent_get_state(two), ==, AI_AGENT_STATE_QUEUED);

	ai_agent_cancel(two);
	g_assert_cmpint(ai_agent_get_state(two), ==, AI_AGENT_STATE_CANCELLED);

	wait_for(&watch, 2);

	/* It was cancelled while waiting, so it never ran and has no answer. */
	g_assert_cmpint(ai_agent_get_state(two), ==, AI_AGENT_STATE_CANCELLED);
	g_assert_null(ai_agent_get_result(two));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/brigade-dispatch/needs-a-worker",
	                test_start_needs_a_worker);
	g_test_add_func("/ai-glib/brigade-dispatch/runs",
	                test_start_runs_to_completion);
	g_test_add_func("/ai-glib/brigade-dispatch/registers",
	                test_start_registers);
	g_test_add_func("/ai-glib/brigade-dispatch/duplicate-id",
	                test_duplicate_id_is_refused);

	g_test_add_func("/ai-glib/brigade-dispatch/queues",
	                test_over_the_limit_queues);
	g_test_add_func("/ai-glib/brigade-dispatch/queue-order",
	                test_queue_is_first_in_first_out);
	g_test_add_func("/ai-glib/brigade-dispatch/no-limit",
	                test_no_limit_starts_everything);

	g_test_add_func("/ai-glib/brigade-dispatch/finishes-remembered",
	                test_finishes_are_remembered);
	g_test_add_func("/ai-glib/brigade-dispatch/reported-once",
	                test_a_finish_is_reported_once);
	g_test_add_func("/ai-glib/brigade-dispatch/oldest-first",
	                test_finishes_come_back_oldest_first);
	g_test_add_func("/ai-glib/brigade-dispatch/nothing-finished",
	                test_nothing_finished_yields_null);

	g_test_add_func("/ai-glib/brigade-dispatch/reap",
	                test_reap_returns_the_answer_and_forgets);
	g_test_add_func("/ai-glib/brigade-dispatch/reap-live",
	                test_reap_refuses_a_live_agent);
	g_test_add_func("/ai-glib/brigade-dispatch/reap-unknown",
	                test_reap_unknown_errors);
	g_test_add_func("/ai-glib/brigade-dispatch/reap-failure",
	                test_reap_a_failure_reports_why);
	g_test_add_func("/ai-glib/brigade-dispatch/remove-drops-notice",
	                test_removing_drops_its_notification);

	g_test_add_func("/ai-glib/brigade-dispatch/unique-ids",
	                test_generated_ids_are_unique);
	g_test_add_func("/ai-glib/brigade-dispatch/ids-skip-taken",
	                test_generated_ids_skip_taken_ones);

	g_test_add_func("/ai-glib/brigade-dispatch/cancel-all",
	                test_cancel_all_stops_the_live_ones);
	g_test_add_func("/ai-glib/brigade-dispatch/cancel-queued",
	                test_cancelling_a_queued_agent_does_not_start_it);

	return g_test_run();
}
