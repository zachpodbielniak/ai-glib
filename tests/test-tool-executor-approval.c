/*
 * test-tool-executor-approval.c - Tool approval and loop observability
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiToolExecutor had no signals at all: a UI could not see the loop, and a
 * host could not refuse a call. Both are now possible, and the constraint
 * that shaped the design is that neither may change what happens for a
 * caller who does neither.
 *
 * That guarantee is the first test in this file, and it is the reason
 * AI_TOOL_APPROVAL_DEFAULT is zero: a signal with no handlers accumulates to
 * zero, which resolves through the policy to ALLOW. There is no
 * compatibility branch to get wrong.
 */

#include <ai-glib.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	guint        approvals_asked;
	GPtrArray   *asked_names;     /* tool names, in order */
	GPtrArray   *events;          /* owned AiEvent* */
	AiToolApproval answer;        /* what the handler returns */
	AiToolApproval second_answer; /* for the two-handler case */
} Watcher;

static Watcher *
watcher_new(AiToolApproval answer)
{
	Watcher *w = g_new0(Watcher, 1);

	w->asked_names = g_ptr_array_new_with_free_func(g_free);
	w->events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	w->answer = answer;
	w->second_answer = AI_TOOL_APPROVAL_DEFAULT;

	return w;
}

static void
watcher_free(Watcher *w)
{
	g_ptr_array_unref(w->asked_names);
	g_ptr_array_unref(w->events);
	g_free(w);
}

static gint
on_approval(AiToolExecutor *exec, AiToolUse *tool_use, gpointer user_data)
{
	Watcher *w = user_data;

	(void)exec;

	w->approvals_asked++;
	g_ptr_array_add(w->asked_names, g_strdup(ai_tool_use_get_name(tool_use)));

	return w->answer;
}

static gint
on_approval_second(AiToolExecutor *exec, AiToolUse *tool_use, gpointer user_data)
{
	Watcher *w = user_data;

	(void)exec;
	(void)tool_use;

	return w->second_answer;
}

static void
on_event(AiEventSource *source, AiEvent *event, gpointer user_data)
{
	Watcher *w = user_data;

	(void)source;

	g_ptr_array_add(w->events, ai_event_ref(event));
}

static guint
count_kind(Watcher *w, AiEventKind kind)
{
	guint i, n = 0;

	for (i = 0; i < w->events->len; i++)
		if (ai_event_get_kind(g_ptr_array_index(w->events, i)) == kind)
			n++;

	return n;
}

static AiEvent *
nth_of_kind(Watcher *w, AiEventKind kind, guint n)
{
	guint i, seen = 0;

	for (i = 0; i < w->events->len; i++)
	{
		AiEvent *e = g_ptr_array_index(w->events, i);

		if (ai_event_get_kind(e) == kind && seen++ == n)
			return e;
	}

	return NULL;
}

/*
 * A provider scripted to ask for one `bash` call and then answer.
 * The second push is what the model says once it has the tool result.
 */
static AiMockProvider *
mock_one_bash_call(void)
{
	AiMockProvider *mock = ai_mock_provider_new();

	ai_mock_provider_push_tool_use(mock, "bash", "{\"command\": \"echo hi\"}");
	ai_mock_provider_push_text(mock, "All done.");

	return mock;
}

/* Run one loop and hand back the final text. */
static gchar *
run_once(AiToolExecutor *exec, AiProvider *provider, GError **error)
{
	g_autoptr(AiMessage) msg = ai_message_new_user("go");
	GList *messages = g_list_append(NULL, msg);
	gchar *result;

	result = ai_tool_executor_run(exec, provider, messages, NULL, 1024,
	                              NULL, error);
	g_list_free(messages);

	return result;
}

/* ----------------------------------------------------------------
 * The compatibility guarantee
 * ---------------------------------------------------------------- */

static void
test_no_handler_behaves_as_before(void)
{
	/*
	 * The whole design rests on this. With nothing connected the emission
	 * accumulates to DEFAULT, the policy resolves it to ALLOW, and the tool
	 * runs -- exactly as it did before approval existed.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;

	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "All done.");

	/* Two turns: the tool call, then the answer. */
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 2);
}

static void
test_defaults(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();

	g_assert_cmpint(ai_tool_executor_get_approval_policy(exec), ==,
	                AI_TOOL_APPROVAL_ALLOW);
	g_assert_false(ai_tool_executor_get_stream(exec));
}

static void
test_properties_round_trip(void)
{
	/* Reachable from bindings and from `ai --set`, so they must be real. */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	gint policy = 0;
	gboolean stream = FALSE;

	g_object_set(exec,
	             "approval-policy", (gint)AI_TOOL_APPROVAL_DENY,
	             "stream", TRUE,
	             NULL);
	g_object_get(exec, "approval-policy", &policy, "stream", &stream, NULL);

	g_assert_cmpint(policy, ==, AI_TOOL_APPROVAL_DENY);
	g_assert_true(stream);

	g_assert_cmpint(ai_tool_executor_get_approval_policy(exec), ==,
	                AI_TOOL_APPROVAL_DENY);
	g_assert_true(ai_tool_executor_get_stream(exec));
}

/* ----------------------------------------------------------------
 * Approval
 * ---------------------------------------------------------------- */

static void
test_allow(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpuint(w->approvals_asked, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(w->asked_names, 0), ==, "bash");
	g_assert_cmpstr(result, ==, "All done.");

	watcher_free(w);
}

static void
test_deny_continues_the_run(void)
{
	/*
	 * A refusal is reported to the model as the tool's result, so it can
	 * apologise and try something else. That is more useful than an aborted
	 * turn, and it is why only DENY_ALL stops the run.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_DENY);
	AiEvent *finished;

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "All done.");

	/* The denial round-trips as a tool result flagged as an error. */
	finished = nth_of_kind(w, AI_EVENT_TOOL_FINISHED, 0);
	g_assert_nonnull(finished);
	g_assert_true(ai_tool_result_get_is_error(
		ai_event_get_tool_result(finished)));
	g_assert_true(strstr(
		ai_tool_result_get_content(ai_event_get_tool_result(finished)),
		"denied") != NULL);

	watcher_free(w);
}

static void
test_deny_all_aborts(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_DENY_ALL);

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CANCELLED);

	/* The model was never asked a second time. */
	g_assert_cmpuint(ai_mock_provider_get_call_count(mock), ==, 1);

	watcher_free(w);
}

static void
test_allow_always_stops_asking(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW_ALWAYS);

	/* Three bash calls across three turns, then an answer. */
	ai_mock_provider_push_tool_use(mock, "bash", "{\"command\": \"echo 1\"}");
	ai_mock_provider_push_tool_use(mock, "bash", "{\"command\": \"echo 2\"}");
	ai_mock_provider_push_tool_use(mock, "bash", "{\"command\": \"echo 3\"}");
	ai_mock_provider_push_text(mock, "done");

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "done");

	/* Asked once; the other two were covered by the remembered answer. */
	g_assert_cmpuint(w->approvals_asked, ==, 1);

	watcher_free(w);
}

static void
test_allow_always_is_forgotten_between_runs(void)
{
	/*
	 * An answer given about one task is not consent for the next. The
	 * remembered set is per-run, and this is what says so.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) first = mock_one_bash_call();
	g_autoptr(AiMockProvider) second = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *r1 = NULL;
	g_autofree gchar *r2 = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW_ALWAYS);

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);

	r1 = run_once(exec, AI_PROVIDER(first), &error);
	g_assert_no_error(error);

	r2 = run_once(exec, AI_PROVIDER(second), &error);
	g_assert_no_error(error);

	g_assert_cmpuint(w->approvals_asked, ==, 2);

	watcher_free(w);
}

static void
test_policy_deny_without_handler(void)
{
	/*
	 * The unattended-agent setting: nobody is there to say yes, so an
	 * unanswered call is a refusal rather than a silent grant.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_DEFAULT);

	ai_tool_executor_set_approval_policy(exec, AI_TOOL_APPROVAL_DENY);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_true(ai_tool_result_get_is_error(ai_event_get_tool_result(
		nth_of_kind(w, AI_EVENT_TOOL_FINISHED, 0))));

	watcher_free(w);
}

static void
test_handler_default_falls_through_to_policy(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_DEFAULT);

	ai_tool_executor_set_approval_policy(exec, AI_TOOL_APPROVAL_DENY);
	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);

	/* The handler ran and abstained; the policy decided. */
	g_assert_cmpuint(w->approvals_asked, ==, 1);
	g_assert_true(ai_tool_result_get_is_error(ai_event_get_tool_result(
		nth_of_kind(w, AI_EVENT_TOOL_FINISHED, 0))));

	watcher_free(w);
}

static void
test_second_handler_decides_when_first_abstains(void)
{
	/*
	 * The case g_signal_accumulator_first_wins() would have broken: it
	 * halts after the first handler whatever it returns, so a handler
	 * answering DEFAULT would silently veto every handler behind it. The
	 * custom accumulator keeps asking while the answer is DEFAULT.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_DEFAULT);

	w->second_answer = AI_TOOL_APPROVAL_DENY;

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	g_signal_connect(exec, "approval-requested",
	                 G_CALLBACK(on_approval_second), w);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_cmpuint(w->approvals_asked, ==, 1);
	g_assert_true(ai_tool_result_get_is_error(ai_event_get_tool_result(
		nth_of_kind(w, AI_EVENT_TOOL_FINISHED, 0))));

	watcher_free(w);
}

static void
test_first_decisive_handler_stops_emission(void)
{
	/* The converse: a real answer is final, and later handlers do not run. */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);

	w->second_answer = AI_TOOL_APPROVAL_DENY;

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	g_signal_connect(exec, "approval-requested",
	                 G_CALLBACK(on_approval_second), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "All done.");

	watcher_free(w);
}

/* ----------------------------------------------------------------
 * Observability
 * ---------------------------------------------------------------- */

static void
test_executor_is_an_event_source(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();

	g_assert_true(AI_IS_EVENT_SOURCE(exec));
}

static void
test_tool_events_bracket_the_call(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);
	AiEvent *started;
	AiEvent *finished;
	guint i;
	gint started_at = -1;
	gint finished_at = -1;

	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);
	result = run_once(exec, AI_PROVIDER(mock), &error);
	g_assert_no_error(error);

	g_assert_cmpuint(count_kind(w, AI_EVENT_TOOL_STARTED), ==, 1);
	g_assert_cmpuint(count_kind(w, AI_EVENT_TOOL_FINISHED), ==, 1);

	started = nth_of_kind(w, AI_EVENT_TOOL_STARTED, 0);
	finished = nth_of_kind(w, AI_EVENT_TOOL_FINISHED, 0);

	/* Started before finished -- a running tool must show as running. */
	for (i = 0; i < w->events->len; i++)
	{
		if (g_ptr_array_index(w->events, i) == started) started_at = (gint)i;
		if (g_ptr_array_index(w->events, i) == finished) finished_at = (gint)i;
	}
	g_assert_cmpint(started_at, >=, 0);
	g_assert_cmpint(started_at, <, finished_at);

	/* Both name the same call, so a consumer can match them. */
	g_assert_cmpstr(ai_tool_use_get_name(ai_event_get_tool_use(started)),
	                ==, "bash");
	g_assert_cmpstr(ai_event_get_tool_use_id(started), ==,
	                ai_event_get_tool_use_id(finished));

	watcher_free(w);
}

static void
test_tool_result_content_reaches_the_event(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);
	AiEvent *finished;

	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);
	result = run_once(exec, AI_PROVIDER(mock), &error);
	g_assert_no_error(error);

	finished = nth_of_kind(w, AI_EVENT_TOOL_FINISHED, 0);
	g_assert_nonnull(ai_event_get_tool_result(finished));

	/* `echo hi` actually ran. */
	g_assert_true(strstr(
		ai_tool_result_get_content(ai_event_get_tool_result(finished)),
		"hi") != NULL);

	watcher_free(w);
}

static void
test_events_are_labelled_executor(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);

	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);
	result = run_once(exec, AI_PROVIDER(mock), &error);
	g_assert_no_error(error);

	/* So a transcript merging provider and tool activity can label them. */
	g_assert_cmpstr(ai_event_get_source(nth_of_kind(w, AI_EVENT_TOOL_STARTED, 0)),
	                ==, "AiToolExecutor");

	watcher_free(w);
}

static void
test_two_tool_calls_across_turns(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);

	ai_mock_provider_push_tool_use(mock, "bash", "{\"command\": \"echo a\"}");
	ai_mock_provider_push_tool_use(mock, "bash", "{\"command\": \"echo b\"}");
	ai_mock_provider_push_text(mock, "both done");

	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);
	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpuint(count_kind(w, AI_EVENT_TOOL_STARTED), ==, 2);
	g_assert_cmpuint(count_kind(w, AI_EVENT_TOOL_FINISHED), ==, 2);

	watcher_free(w);
}

static void
test_no_tool_events_when_no_tools_run(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);

	ai_mock_provider_push_text(mock, "no tools needed");

	g_signal_connect(exec, "approval-requested", G_CALLBACK(on_approval), w);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "no tools needed");
	g_assert_cmpuint(w->approvals_asked, ==, 0);
	g_assert_cmpuint(count_kind(w, AI_EVENT_TOOL_STARTED), ==, 0);

	watcher_free(w);
}

static void
test_stream_off_by_default(void)
{
	/*
	 * The default must be the old path, or every existing caller silently
	 * changes behaviour. The answer alone cannot prove which path ran --
	 * the two agree by design -- so the mock counts them separately.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;

	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "All done.");
	g_assert_cmpuint(ai_mock_provider_get_stream_call_count(mock), ==, 0);
}

static void
test_stream_takes_the_streaming_path(void)
{
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;

	ai_tool_executor_set_stream(exec, TRUE);
	result = run_once(exec, AI_PROVIDER(mock), &error);

	g_assert_no_error(error);
	g_assert_cmpstr(result, ==, "All done.");

	/* Both turns streamed: the tool call and the answer after it. */
	g_assert_cmpuint(ai_mock_provider_get_stream_call_count(mock), ==, 2);
}

static void
test_stream_still_runs_tools(void)
{
	/*
	 * The point of the whole change: live tokens *and* tool execution,
	 * which were mutually exclusive before because the loop only ever
	 * called chat_async.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = mock_one_bash_call();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);

	ai_tool_executor_set_stream(exec, TRUE);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);
	g_assert_no_error(error);

	/* Text arrived as a delta... */
	g_assert_cmpuint(count_kind(w, AI_EVENT_TEXT_DELTA), >, 0);

	/* ...and the tool still ran, on the same stream. */
	g_assert_cmpuint(count_kind(w, AI_EVENT_TOOL_STARTED), >, 0);
	g_assert_cmpuint(count_kind(w, AI_EVENT_TOOL_FINISHED), ==, 1);

	watcher_free(w);
}

static void
test_provider_events_are_forwarded(void)
{
	/*
	 * A frontend subscribes to one object and sees the whole turn. The
	 * source label is preserved through the forward so a transcript can
	 * still tell provider output from tool activity.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);
	AiEvent *delta;

	ai_mock_provider_push_text(mock, "hello from the provider");
	ai_tool_executor_set_stream(exec, TRUE);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);
	g_assert_no_error(error);

	delta = nth_of_kind(w, AI_EVENT_TEXT_DELTA, 0);
	g_assert_nonnull(delta);
	g_assert_cmpstr(ai_event_get_text(delta), ==, "hello from the provider");
	g_assert_cmpstr(ai_event_get_source(delta), ==, "AiMockProvider");

	watcher_free(w);
}

static void
test_provider_events_stop_after_the_run(void)
{
	/*
	 * The handler is disconnected when the run ends, so an executor reused
	 * against a second provider never keeps forwarding the first one's
	 * events -- and a provider outliving the executor is not a dangling
	 * connection.
	 */
	g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	Watcher *w = watcher_new(AI_TOOL_APPROVAL_ALLOW);
	guint after_run;

	ai_mock_provider_push_text(mock, "one");
	ai_tool_executor_set_stream(exec, TRUE);
	g_signal_connect(exec, "event", G_CALLBACK(on_event), w);

	result = run_once(exec, AI_PROVIDER(mock), &error);
	g_assert_no_error(error);

	after_run = w->events->len;
	g_assert_cmpuint(after_run, >, 0);

	/* Anything the provider says now must not reach the executor. */
	{
		g_autoptr(AiEvent) stray = ai_event_new_status("after the run");
		ai_event_source_emit(AI_EVENT_SOURCE(mock), stray);
	}

	g_assert_cmpuint(w->events->len, ==, after_run);

	watcher_free(w);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/executor/no-handler-unchanged",
	                test_no_handler_behaves_as_before);
	g_test_add_func("/ai-glib/executor/defaults", test_defaults);
	g_test_add_func("/ai-glib/executor/properties", test_properties_round_trip);

	g_test_add_func("/ai-glib/executor/approval/allow", test_allow);
	g_test_add_func("/ai-glib/executor/approval/deny-continues",
	                test_deny_continues_the_run);
	g_test_add_func("/ai-glib/executor/approval/deny-all", test_deny_all_aborts);
	g_test_add_func("/ai-glib/executor/approval/allow-always",
	                test_allow_always_stops_asking);
	g_test_add_func("/ai-glib/executor/approval/allow-always-per-run",
	                test_allow_always_is_forgotten_between_runs);
	g_test_add_func("/ai-glib/executor/approval/policy-deny",
	                test_policy_deny_without_handler);
	g_test_add_func("/ai-glib/executor/approval/default-falls-through",
	                test_handler_default_falls_through_to_policy);
	g_test_add_func("/ai-glib/executor/approval/second-handler-decides",
	                test_second_handler_decides_when_first_abstains);
	g_test_add_func("/ai-glib/executor/approval/first-decisive-wins",
	                test_first_decisive_handler_stops_emission);

	g_test_add_func("/ai-glib/executor/events/is-event-source",
	                test_executor_is_an_event_source);
	g_test_add_func("/ai-glib/executor/events/bracket-the-call",
	                test_tool_events_bracket_the_call);
	g_test_add_func("/ai-glib/executor/events/result-content",
	                test_tool_result_content_reaches_the_event);
	g_test_add_func("/ai-glib/executor/events/labelled",
	                test_events_are_labelled_executor);
	g_test_add_func("/ai-glib/executor/events/two-calls",
	                test_two_tool_calls_across_turns);
	g_test_add_func("/ai-glib/executor/events/none-when-no-tools",
	                test_no_tool_events_when_no_tools_run);
	g_test_add_func("/ai-glib/executor/stream/off-by-default",
	                test_stream_off_by_default);
	g_test_add_func("/ai-glib/executor/stream/takes-streaming-path",
	                test_stream_takes_the_streaming_path);
	g_test_add_func("/ai-glib/executor/stream/still-runs-tools",
	                test_stream_still_runs_tools);
	g_test_add_func("/ai-glib/executor/stream/forwards-provider-events",
	                test_provider_events_are_forwarded);
	g_test_add_func("/ai-glib/executor/stream/stops-forwarding-after-run",
	                test_provider_events_stop_after_the_run);

	return g_test_run();
}
