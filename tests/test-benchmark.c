/*
 * test-benchmark.c - AiBenchmark: the runner end to end
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Each execution rule in src/bench/ai-benchmark.c has a test here named
 * after it, so a regression fails by name:
 *
 *   round-robin, warmup-excluded, sample-signal-once, timeout-is-error,
 *   cancel-keeps-samples, cancel-in-flight, failed-target-skipped,
 *   capability-refused, error-is-not-no, already-running
 *
 * and the driving case -- TypeSafe Jev against a local Ollama decision
 * model -- runs over two real sockets (jev-vs-ollama), as do embedding
 * and image generation. Nothing leaves loopback: every TYPESAFE_*,
 * OLLAMA_* and LAYA_* variable is unset and HOME is a scratch directory.
 */

#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "test-server.h"

static gchar *scratch_dir;

/* ---- a decider with a read-only property, for --set refusals ---- */

#define TEST_TYPE_RO_DECIDER (test_ro_decider_get_type())
G_DECLARE_FINAL_TYPE(TestRoDecider, test_ro_decider, TEST, RO_DECIDER, GObject)

struct _TestRoDecider
{
	GObject parent_instance;
	guint   level;
};

static void test_ro_decider_iface_init(AiDeciderInterface *iface) { (void)iface; }

G_DEFINE_TYPE_WITH_CODE(TestRoDecider, test_ro_decider, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_DECIDER, test_ro_decider_iface_init))

static void
test_ro_decider_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	(void)pspec;
	if (prop_id == 1)
		g_value_set_uint(value, TEST_RO_DECIDER(object)->level);
}

static void
test_ro_decider_class_init(
	TestRoDeciderClass *klass
){
	G_OBJECT_CLASS(klass)->get_property = test_ro_decider_get_property;
	g_object_class_install_property(G_OBJECT_CLASS(klass), 1,
		g_param_spec_uint("level", NULL, NULL, 0, 10, 3, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
}

static void test_ro_decider_init(TestRoDecider *self) { self->level = 3; }

/* ---- helpers ---------------------------------------------------- */

typedef struct
{
	GMainLoop *loop;
	gboolean   ok;
	gboolean   done;
	GError    *error;
} Wait;

static void
on_run_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	Wait *wait = (Wait *)user_data;

	wait->ok = ai_benchmark_run_finish(AI_BENCHMARK(source), result, &wait->error);
	wait->done = TRUE;
	g_main_loop_quit(wait->loop);
}

/*
 * Runs @bench asynchronously on the default context. AiMockProvider's
 * delay is a global-default timeout, so the synchronous ai_benchmark_run()
 * (private context) would never see it fire.
 */
static gboolean
run_async_and_wait(
	AiBenchmark   *bench,
	GCancellable  *cancellable,
	GError       **error
){
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	Wait wait = { NULL, FALSE, FALSE, NULL };

	wait.loop = loop;
	ai_benchmark_run_async(bench, cancellable, on_run_done, &wait);
	if (!wait.done)
		g_main_loop_run(loop);
	if (wait.error != NULL)
		g_propagate_error(error, wait.error);
	return wait.ok;
}

static AiMockProvider *
mock_chat(
	const gchar *reply,
	guint        delay_ms
){
	AiMockProvider *mock = ai_mock_provider_new();

	ai_mock_provider_set_fallback(mock, reply);
	ai_mock_provider_set_delay_ms(mock, delay_ms);
	return mock;
}

static AiBenchmarkTarget *
object_target(
	const gchar     *label,
	AiBenchmarkKind  kind,
	gpointer         object
){
	return ai_benchmark_target_new_for_object(label, kind, G_OBJECT(object));
}

static void
add_target(
	AiBenchmark       *bench,
	AiBenchmarkTarget *target
){
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_benchmark_add_target(bench, target, &error));
	g_assert_no_error(error);
	g_object_unref(target);
}

static void
add_case(
	AiBenchmark     *bench,
	AiBenchmarkCase *bench_case
){
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_benchmark_add_case(bench, bench_case, &error));
	g_assert_no_error(error);
	g_object_unref(bench_case);
}

static AiBenchmarkSample *
sample_at(
	AiBenchmark *bench,
	guint        index
){
	GPtrArray *samples = ai_benchmark_get_samples(bench);

	g_assert_cmpuint(index, <, samples->len);
	return (AiBenchmarkSample *)g_ptr_array_index(samples, index);
}

static AiBenchmarkSummary *
summary_for(
	GList       *summaries,
	const gchar *target
){
	GList *iter;

	for (iter = summaries; iter != NULL; iter = iter->next)
	{
		AiBenchmarkSummary *s = (AiBenchmarkSummary *)iter->data;

		if (g_str_equal(ai_benchmark_summary_get_target(s), target))
			return s;
	}
	g_assert_not_reached();
	return NULL;
}

static AiDecisionRequest *
refund_request(
	void
){
	AiDecisionRequest *request = ai_decision_request_new("Please refund the duplicate charge.");

	g_assert_true(ai_decision_request_add_boolean(request, "refund", "Is a refund requested?", NULL));
	return request;
}

static AiBenchmarkCase *
refund_case(
	const gchar *expected
){
	g_autoptr(AiDecisionRequest) request = refund_request();
	AiBenchmarkCase *bench_case = ai_benchmark_case_new_decision("refund", request);

	if (expected != NULL)
		g_assert_true(ai_benchmark_case_expect_answer(bench_case, "refund", expected, NULL));
	return bench_case;
}

/* ---- defaults --------------------------------------------------- */

static void
test_defaults_and_properties(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	guint iterations;
	guint warmup;

	g_assert_cmpuint(ai_benchmark_get_iterations(bench), ==, 3);
	g_assert_cmpuint(ai_benchmark_get_warmup(bench), ==, 1);
	g_assert_cmpuint(ai_benchmark_get_timeout_ms(bench), ==, 0);
	g_assert_null(ai_benchmark_get_name(bench));
	g_assert_false(ai_benchmark_is_running(bench));

	g_object_set(bench, "iterations", 7, "warmup", 0, "name", "x", NULL);
	g_object_get(bench, "iterations", &iterations, "warmup", &warmup, NULL);
	g_assert_cmpuint(iterations, ==, 7);
	g_assert_cmpuint(warmup, ==, 0);
	g_assert_cmpstr(ai_benchmark_get_name(bench), ==, "x");
}

/* Labels and ids are unique: they key every summary and every output row. */
static void
test_duplicates_refused(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) decider = ai_mock_decider_new("{}");
	g_autoptr(AiBenchmarkTarget) again = object_target("a", AI_BENCHMARK_KIND_DECISION, decider);
	g_autoptr(AiBenchmarkCase) case_again = refund_case(NULL);
	g_autoptr(GError) error = NULL;

	add_target(bench, object_target("a", AI_BENCHMARK_KIND_DECISION, decider));
	add_case(bench, refund_case(NULL));
	g_assert_false(ai_benchmark_add_target(bench, again, &error));
	g_assert_cmpstr(error->message, ==, "duplicate target label 'a'");
	g_clear_error(&error);
	g_assert_false(ai_benchmark_add_case(bench, case_again, &error));
	g_assert_cmpstr(error->message, ==, "duplicate case id 'refund'");
	g_clear_error(&error);

	/* One wrapped client cannot back two targets: build configures it. */
	{
		g_autoptr(AiBenchmarkTarget) shared = object_target("b", AI_BENCHMARK_KIND_DECISION, decider);

		g_assert_false(ai_benchmark_add_target(bench, shared, &error));
		g_assert_cmpstr(error->message, ==, "target 'b' wraps the same client as target 'a'; give each its own");
		g_assert_cmpuint(ai_benchmark_get_targets(bench)->len, ==, 1);
	}
}

/* -t keeps the named targets in order; an unknown label changes nothing. */
static void
test_select_targets(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) da = ai_mock_decider_new("{}");
	g_autoptr(AiMockDecider) db = ai_mock_decider_new("{}");
	g_autoptr(AiMockDecider) dc = ai_mock_decider_new("{}");
	g_autoptr(GError) error = NULL;
	const gchar *keep[] = { "c", "a", NULL };
	const gchar *bad[] = { "a", "zz", NULL };
	GPtrArray *targets;

	add_target(bench, object_target("a", AI_BENCHMARK_KIND_DECISION, da));
	add_target(bench, object_target("b", AI_BENCHMARK_KIND_DECISION, db));
	add_target(bench, object_target("c", AI_BENCHMARK_KIND_DECISION, dc));

	g_assert_false(ai_benchmark_select_targets(bench, bad, &error));
	g_assert_cmpstr(error->message, ==, "no target labelled 'zz' (targets: a, b, c)");
	g_assert_cmpuint(ai_benchmark_get_targets(bench)->len, ==, 3);

	g_assert_true(ai_benchmark_select_targets(bench, keep, NULL));
	targets = ai_benchmark_get_targets(bench);
	g_assert_cmpuint(targets->len, ==, 2);
	g_assert_cmpstr(ai_benchmark_target_get_label(g_ptr_array_index(targets, 0)), ==, "a");
	g_assert_cmpstr(ai_benchmark_target_get_label(g_ptr_array_index(targets, 1)), ==, "c");
}

/* The plan's size: warmups per target, then cases x iterations x targets of that kind. */
static void
test_count_calls(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) d1 = ai_mock_decider_new("{}");
	g_autoptr(AiMockDecider) d2 = ai_mock_decider_new("{}");
	g_autoptr(AiMockProvider) chat = mock_chat("x", 0);
	g_autoptr(AiDecisionRequest) request = refund_request();
	guint measured;

	add_target(bench, object_target("d1", AI_BENCHMARK_KIND_DECISION, d1));
	add_target(bench, object_target("d2", AI_BENCHMARK_KIND_DECISION, d2));
	add_target(bench, object_target("c1", AI_BENCHMARK_KIND_CHAT, chat));
	add_case(bench, ai_benchmark_case_new_decision("one", request));
	add_case(bench, ai_benchmark_case_new_decision("two", request));
	add_case(bench, ai_benchmark_case_new_chat("hi", "hi"));
	ai_benchmark_set_iterations(bench, 3);
	ai_benchmark_set_warmup(bench, 2);

	/* warmup 2 x 3 targets; measured 3 x (2 cases x 2 targets + 1 x 1) */
	g_assert_cmpuint(ai_benchmark_count_calls(bench, &measured), ==, 6 + 15);
	g_assert_cmpuint(measured, ==, 15);
	g_assert_true(ai_benchmark_prepare(bench, NULL));
	g_assert_cmpuint(ai_benchmark_count_calls(bench, NULL), ==, 21);
}

/* ---- execution rules -------------------------------------------- */

typedef struct
{
	GPtrArray *seen;
	guint      cancel_after;
	GCancellable *cancellable;
} SampleLog;

static void
on_sample(
	AiBenchmark       *bench,
	AiBenchmarkSample *sample,
	gpointer           user_data
){
	SampleLog *log = (SampleLog *)user_data;

	(void)bench;
	g_ptr_array_add(log->seen, g_object_ref(sample));
	if (log->cancel_after > 0 && log->seen->len == log->cancel_after)
		g_cancellable_cancel(log->cancellable);
}

/*
 * round-robin: for each case and iteration every target runs once before
 * the next iteration, so drift lands on all targets alike.
 * sample-signal-once: ::sample fires once per recorded run, in order.
 */
static void
test_round_robin_and_signal(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockProvider) a = mock_chat("alpha", 0);
	g_autoptr(AiMockProvider) b = mock_chat("beta", 0);
	g_autoptr(GError) error = NULL;
	SampleLog log = { NULL, 0, NULL };
	static const gchar *const expected[][3] = {
		{ "A", "one", "0" }, { "B", "one", "0" }, { "A", "one", "1" }, { "B", "one", "1" },
		{ "A", "two", "0" }, { "B", "two", "0" }, { "A", "two", "1" }, { "B", "two", "1" },
	};
	guint i;

	log.seen = g_ptr_array_new_with_free_func(g_object_unref);
	add_target(bench, object_target("A", AI_BENCHMARK_KIND_CHAT, a));
	add_target(bench, object_target("B", AI_BENCHMARK_KIND_CHAT, b));
	add_case(bench, ai_benchmark_case_new_chat("one", "first"));
	add_case(bench, ai_benchmark_case_new_chat("two", "second"));
	ai_benchmark_set_iterations(bench, 2);
	g_signal_connect(bench, "sample", G_CALLBACK(on_sample), &log);

	g_assert_true(run_async_and_wait(bench, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(ai_benchmark_get_samples(bench)->len, ==, G_N_ELEMENTS(expected));
	g_assert_cmpuint(log.seen->len, ==, G_N_ELEMENTS(expected));
	for (i = 0; i < G_N_ELEMENTS(expected); i++)
	{
		AiBenchmarkSample *sample = sample_at(bench, i);
		g_autofree gchar *iteration = g_strdup_printf("%u", ai_benchmark_sample_get_iteration(sample));

		g_assert_true(g_ptr_array_index(log.seen, i) == (gpointer)sample);
		g_assert_cmpstr(ai_benchmark_sample_get_target(sample), ==, expected[i][0]);
		g_assert_cmpstr(ai_benchmark_sample_get_case_id(sample), ==, expected[i][1]);
		g_assert_cmpstr(iteration, ==, expected[i][2]);
		g_assert_cmpint(ai_benchmark_sample_get_outcome(sample), ==, AI_BENCHMARK_OUTCOME_UNSCORED);
		g_assert_cmpstr(ai_benchmark_sample_get_model(sample), ==, "mock-model");
	}
	g_assert_false(ai_benchmark_is_running(bench));
	g_ptr_array_unref(log.seen);
}

/*
 * warmup-excluded: each target makes `warmup` calls of its first case
 * that produce no sample and no signal; then the measured ones.
 */
static void
test_warmup_excluded(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockProvider) a = mock_chat("ok", 0);
	g_autoptr(GError) error = NULL;
	SampleLog log = { NULL, 0, NULL };

	log.seen = g_ptr_array_new_with_free_func(g_object_unref);
	add_target(bench, object_target("A", AI_BENCHMARK_KIND_CHAT, a));
	add_case(bench, ai_benchmark_case_new_chat("one", "first"));
	add_case(bench, ai_benchmark_case_new_chat("two", "second"));
	ai_benchmark_set_iterations(bench, 1);
	ai_benchmark_set_warmup(bench, 2);
	g_signal_connect(bench, "sample", G_CALLBACK(on_sample), &log);

	g_assert_true(run_async_and_wait(bench, NULL, &error));
	g_assert_cmpuint(ai_mock_provider_get_call_count(a), ==, 2 + 2);
	g_assert_cmpuint(ai_benchmark_get_samples(bench)->len, ==, 2);
	g_assert_cmpuint(log.seen->len, ==, 2);
	g_assert_cmpuint(ai_benchmark_sample_get_iteration(sample_at(bench, 0)), ==, 0);
	g_ptr_array_unref(log.seen);
}

/*
 * Latency is the runner's clock around the call: a mock that waits
 * 40 ms can never be measured faster than that. A streamed target also
 * records a time to first token no later than the whole call.
 */
static void
test_latency_and_ttft(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockProvider) slow = mock_chat("Paris", 40);
	g_autoptr(AiMockProvider) streamed = mock_chat("It is paris", 5);
	g_autoptr(AiBenchmarkTarget) stream_target = object_target("streamed", AI_BENCHMARK_KIND_CHAT, streamed);
	g_autoptr(AiBenchmarkCase) bench_case = ai_benchmark_case_new_chat("capital", "Capital of France?");
	g_autolist(AiBenchmarkSummary) summaries = NULL;
	g_autoptr(GError) error = NULL;
	AiBenchmarkSummary *s;
	guint i;

	ai_benchmark_target_set_stream(stream_target, TRUE);
	ai_benchmark_case_expect_contains(bench_case, "paris");
	add_target(bench, object_target("slow", AI_BENCHMARK_KIND_CHAT, slow));
	g_assert_true(ai_benchmark_add_target(bench, stream_target, NULL));
	g_assert_true(ai_benchmark_add_case(bench, bench_case, NULL));
	ai_benchmark_set_iterations(bench, 3);

	g_assert_true(run_async_and_wait(bench, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(ai_mock_provider_get_stream_call_count(streamed), ==, 4);
	g_assert_cmpuint(ai_mock_provider_get_stream_call_count(slow), ==, 0);

	summaries = ai_benchmark_dup_summaries(bench, FALSE);
	s = summary_for(summaries, "slow");
	g_assert_cmpfloat(ai_benchmark_summary_get_min_ms(s), >=, 40.0);
	g_assert_cmpfloat(ai_benchmark_summary_get_mean_ttft_ms(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, 1.0);

	s = summary_for(summaries, "streamed");
	g_assert_cmpfloat(ai_benchmark_summary_get_mean_ttft_ms(s), >=, 0);
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, 1.0);
	for (i = 0; i < ai_benchmark_get_samples(bench)->len; i++)
	{
		AiBenchmarkSample *sample = sample_at(bench, i);

		if (g_str_equal(ai_benchmark_sample_get_target(sample), "streamed"))
		{
			g_assert_cmpint(ai_benchmark_sample_get_ttft_us(sample), >=, 0);
			g_assert_cmpint(ai_benchmark_sample_get_ttft_us(sample), <=,
			                ai_benchmark_sample_get_elapsed_us(sample));
		}
		else
			g_assert_cmpint(ai_benchmark_sample_get_ttft_us(sample), ==, -1);
	}
}

/* Two mock deciders, one right and one wrong: accuracy and Brier side by side. */
static void
test_decision_accuracy(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) good = ai_mock_decider_new(
		"{\"model\":\"good\",\"answers\":{\"refund\":{\"type\":\"noul\",\"noul\":0.9}}}");
	g_autoptr(AiMockDecider) bad = ai_mock_decider_new(
		"{\"model\":\"bad\",\"answers\":{\"refund\":{\"type\":\"noul\",\"noul\":0.2}}}");
	g_autolist(AiBenchmarkSummary) summaries = NULL;
	g_autoptr(GError) error = NULL;
	AiBenchmarkSummary *s;

	add_target(bench, object_target("good", AI_BENCHMARK_KIND_DECISION, good));
	add_target(bench, object_target("bad", AI_BENCHMARK_KIND_DECISION, bad));
	add_case(bench, refund_case("true"));
	ai_benchmark_set_iterations(bench, 2);

	/* The synchronous run works too: the mock schedules nothing global. */
	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	g_assert_no_error(error);

	summaries = ai_benchmark_dup_summaries(bench, FALSE);
	s = summary_for(summaries, "good");
	g_assert_cmpuint(ai_benchmark_summary_get_runs(s), ==, 2);
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, 1.0);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_brier(s), 0.01, 1e-9);
	s = summary_for(summaries, "bad");
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, 0.0);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_brier(s), 0.64, 1e-9);
	g_assert_cmpstr(ai_benchmark_sample_get_model(sample_at(bench, 0)), ==, "good");
	g_assert_cmpstr(ai_benchmark_sample_get_detail(sample_at(bench, 1)), ==,
	                "refund: expected true, P(true) was 0.2000");
}

/* error-is-not-no: a failed call is an error sample, never a FALSE answer. */
static void
test_error_is_not_no(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) broken = ai_mock_decider_new("{}");
	g_autoptr(GError) failure = g_error_new_literal(AI_ERROR, AI_ERROR_SERVER_ERROR, "upstream 503");
	g_autolist(AiBenchmarkSummary) summaries = NULL;
	g_autoptr(GError) error = NULL;
	AiBenchmarkSummary *s;

	ai_mock_decider_set_error(broken, failure);
	add_target(bench, object_target("broken", AI_BENCHMARK_KIND_DECISION, broken));
	add_case(bench, refund_case("false"));
	ai_benchmark_set_iterations(bench, 2);

	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	g_assert_cmpint(ai_benchmark_sample_get_outcome(sample_at(bench, 0)), ==, AI_BENCHMARK_OUTCOME_ERROR);
	g_assert_cmpstr(ai_benchmark_sample_get_error(sample_at(bench, 0)), ==, "upstream 503");
	summaries = ai_benchmark_dup_summaries(bench, FALSE);
	s = summary_for(summaries, "broken");
	g_assert_cmpuint(ai_benchmark_summary_get_errors(s), ==, 2);
	g_assert_cmpuint(ai_benchmark_summary_get_scored(s), ==, 0);
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_p50_ms(s), ==, -1);
}

/*
 * capability-refused and failed-target-skipped: `claude` cannot embed,
 * so it is refused at build with a sentence saying why, keeps a zero-run
 * summary, and the other target still runs.
 */
static void
test_capability_refused_and_skipped(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) decider = ai_mock_decider_new(
		"{\"model\":\"m\",\"answers\":{\"refund\":{\"type\":\"noul\",\"noul\":0.9}}}");
	static const gchar *const texts[] = { "a", "b", NULL };
	g_autolist(AiBenchmarkSummary) summaries = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *table = NULL;

	add_target(bench, ai_benchmark_target_new("claude-embed", AI_BENCHMARK_KIND_EMBEDDING, "claude", NULL));
	add_target(bench, object_target("ok", AI_BENCHMARK_KIND_DECISION, decider));
	add_case(bench, ai_benchmark_case_new_embedding("vectors", texts));
	add_case(bench, refund_case(NULL));
	ai_benchmark_set_iterations(bench, 1);

	g_assert_true(ai_benchmark_prepare(bench, &error));
	g_assert_cmpstr(ai_benchmark_get_target_error(bench, "claude-embed"), ==,
	                "target 'claude-embed': provider 'claude' cannot run embedding (it does not implement AiEmbedder)");
	g_assert_null(ai_benchmark_get_target_error(bench, "ok"));

	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	g_assert_cmpuint(ai_benchmark_get_samples(bench)->len, ==, 1);
	summaries = ai_benchmark_dup_summaries(bench, FALSE);
	g_assert_cmpuint(g_list_length(summaries), ==, 2);
	g_assert_cmpuint(ai_benchmark_summary_get_runs(summary_for(summaries, "claude-embed")), ==, 0);
	table = ai_benchmark_dup_table(bench, FALSE);
	g_assert_nonnull(strstr(table, "skipped claude-embed: target 'claude-embed': provider 'claude' cannot run embedding"));
}

/* Streaming is a capability too: claude-tmux does not implement AiStreamable. */
static void
test_stream_capability(
	void
){
	g_autoptr(AiBenchmarkTarget) target = ai_benchmark_target_new("tmux", AI_BENCHMARK_KIND_CHAT, "claude-tmux", NULL);
	g_autoptr(AiBenchmarkTarget) unknown = ai_benchmark_target_new("x", AI_BENCHMARK_KIND_CHAT, "gpt5", NULL);
	g_autoptr(AiBenchmarkTarget) jev = ai_benchmark_target_new("j", AI_BENCHMARK_KIND_DECISION, "jev", "jev-latest");
	g_autoptr(GObject) built = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *model = NULL;

	ai_benchmark_target_set_stream(target, TRUE);
	g_assert_null(ai_benchmark_target_build(target, NULL, &error));
	g_assert_cmpstr(error->message, ==,
	                "target 'tmux': provider 'claude-tmux' cannot run streamed chat (it does not implement AiStreamable)");
	g_clear_error(&error);

	g_assert_null(ai_benchmark_target_build(unknown, NULL, &error));
	g_assert_cmpstr(error->message, ==, "target 'x': unknown provider 'gpt5'");
	g_clear_error(&error);

	/* jev is the TypeSafe client, through the same factory as `ai decide`. */
	built = ai_benchmark_target_build(jev, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(AI_IS_TYPESAFE_CLIENT(built));
	g_object_get(built, "model", &model, NULL);
	g_assert_cmpstr(model, ==, "jev-latest");
}

/* nothing to run: every target failed, and the run says why. */
static void
test_nothing_to_run(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	static const gchar *const texts[] = { "a", NULL };
	g_autoptr(GError) error = NULL;

	add_target(bench, ai_benchmark_target_new("claude-embed", AI_BENCHMARK_KIND_EMBEDDING, "claude", NULL));
	add_case(bench, ai_benchmark_case_new_embedding("vectors", texts));
	g_assert_false(ai_benchmark_run(bench, NULL, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_true(g_str_has_prefix(error->message, "nothing to run: target 'claude-embed'"));
	g_assert_false(ai_benchmark_is_running(bench));
}

/*
 * Property assignments reach the built client, override the model, and
 * a bad name, a bad value or a read-only property is a build error.
 */
static void
test_property_overrides(
	void
){
	g_autoptr(AiBenchmarkTarget) target = ai_benchmark_target_new("t", AI_BENCHMARK_KIND_DECISION, "typesafe", "jev-1.13.0");
	g_autoptr(GObject) built = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *model = NULL;
	g_autofree gchar *url = NULL;
	guint timeout;
	gboolean long_context;

	g_assert_true(ai_benchmark_target_add_property(target, "timeout-ms=1234", &error));
	g_assert_true(ai_benchmark_target_add_property(target, "long-context", &error));
	g_assert_true(ai_benchmark_target_add_property(target, "base-url=http://127.0.0.1:9", &error));
	g_assert_true(ai_benchmark_target_has_property(target, "long-context"));
	g_assert_true(ai_benchmark_target_has_property(target, "timeout-ms"));
	g_assert_false(ai_benchmark_target_has_property(target, "timeout"));
	g_assert_cmpuint(g_strv_length((gchar **)ai_benchmark_target_get_properties(target)), ==, 3);
	g_assert_false(ai_benchmark_target_add_property(target, "=x", &error));
	g_assert_cmpstr(error->message, ==, "target 't': property assignment '=x' has no name");
	g_clear_error(&error);

	built = ai_benchmark_target_build(target, NULL, &error);
	g_assert_no_error(error);
	g_object_get(built, "timeout-ms", &timeout, "long-context", &long_context, "model", &model, "base-url", &url, NULL);
	g_assert_cmpuint(timeout, ==, 1234);
	g_assert_true(long_context);
	g_assert_cmpstr(model, ==, "jev-1.13.0");
	g_assert_cmpstr(url, ==, "http://127.0.0.1:9");

	{
		g_autoptr(AiBenchmarkTarget) bad = ai_benchmark_target_new("b", AI_BENCHMARK_KIND_DECISION, "laya", NULL);

		ai_benchmark_target_add_property(bad, "nope=1", NULL);
		g_assert_null(ai_benchmark_target_build(bad, NULL, &error));
		g_assert_true(g_str_has_prefix(error->message, "target 'b': AiLayaClient has no property 'nope' (writable: "));
		g_assert_nonnull(strstr(error->message, "base-url"));
		g_clear_error(&error);
	}
	{
		g_autoptr(AiBenchmarkTarget) bad = ai_benchmark_target_new("b", AI_BENCHMARK_KIND_DECISION, "laya", NULL);

		ai_benchmark_target_add_property(bad, "timeout-ms=soon", NULL);
		g_assert_null(ai_benchmark_target_build(bad, NULL, &error));
		g_assert_cmpstr(error->message, ==, "target 'b': cannot read 'soon' as a value for 'timeout-ms' (guint)");
		g_clear_error(&error);
	}
	/* Out of range is refused, never wrapped or clamped: -1 is not 4294967295. */
	{
		static const gchar *const out_of_range[] = {
			"timeout-ms=-1", "timeout-ms=0", "timeout-ms=99999999999", "timeout-ms=+-3", NULL
		};
		guint k;

		for (k = 0; out_of_range[k] != NULL; k++)
		{
			g_autoptr(AiBenchmarkTarget) bad = ai_benchmark_target_new("b", AI_BENCHMARK_KIND_DECISION, "laya", NULL);

			ai_benchmark_target_add_property(bad, out_of_range[k], NULL);
			g_test_message("%s", out_of_range[k]);
			g_assert_null(ai_benchmark_target_build(bad, NULL, &error));
			g_assert_true(g_str_has_prefix(error->message, "target 'b': cannot read '"));
			g_clear_error(&error);
		}
	}
	{
		g_autoptr(TestRoDecider) ro = g_object_new(TEST_TYPE_RO_DECIDER, NULL);
		g_autoptr(AiBenchmarkTarget) bad = object_target("ro", AI_BENCHMARK_KIND_DECISION, ro);

		ai_benchmark_target_add_property(bad, "level=5", NULL);
		g_assert_null(ai_benchmark_target_build(bad, NULL, &error));
		g_assert_cmpstr(error->message, ==, "target 'ro': property 'level' of TestRoDecider is read-only");
	}
}

/* Ollama's chat model and decision model are different properties. */
static void
test_ollama_decision_model(
	void
){
	g_autoptr(AiBenchmarkTarget) target = ai_benchmark_target_new("o", AI_BENCHMARK_KIND_DECISION, "ollama", "clef");
	g_autoptr(GObject) built = NULL;
	g_autofree gchar *decision_model = NULL;

	built = ai_benchmark_target_build(target, NULL, NULL);
	g_assert_true(AI_IS_OLLAMA_CLIENT(built));
	g_object_get(built, "decision-model", &decision_model, NULL);
	g_assert_cmpstr(decision_model, ==, "clef");
	g_assert_cmpstr(ai_client_get_model(AI_CLIENT(built)), !=, "clef");
}

/*
 * jev-vs-ollama: the driving case, over two real sockets. TypeSafe is
 * configured through properties, Ollama through AiConfig, and each
 * server sees exactly warmup + iterations requests.
 */
static void
test_jev_vs_ollama(
	void
){
	TServer *jev_server = tserver_new();
	TServer *ollama_server = tserver_new();
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(AiBenchmarkTarget) jev = ai_benchmark_target_new("jev", AI_BENCHMARK_KIND_DECISION, "typesafe", "jev-latest");
	g_autoptr(AiBenchmarkTarget) tev1 = ai_benchmark_target_new("tev1", AI_BENCHMARK_KIND_DECISION, "ollama", "tev1");
	g_autolist(AiBenchmarkSummary) summaries = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = g_strdup_printf("base-url=%s", jev_server->base_url);
	AiBenchmarkSummary *s;

	tserver_set_response(jev_server, 200,
		"{\"model\":\"jev-1.14.0\",\"answers\":{\"refund\":{\"type\":\"noul\",\"noul\":0.95}},"
		"\"usage\":{\"input_tokens\":40,\"output_tokens\":2}}");
	tserver_set_response(ollama_server, 200,
		"{\"model\":\"tev1\",\"answers\":{\"refund\":{\"type\":\"noul\",\"noul\":0.4}}}");
	ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, ollama_server->base_url);
	ai_benchmark_set_config(bench, config);
	g_assert_true(ai_benchmark_target_add_property(jev, url, NULL));
	g_assert_true(ai_benchmark_target_add_property(jev, "api-key=test-key", NULL));
	g_assert_true(ai_benchmark_add_target(bench, jev, NULL));
	g_assert_true(ai_benchmark_add_target(bench, tev1, NULL));
	add_case(bench, refund_case("true"));
	ai_benchmark_set_iterations(bench, 2);

	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(tserver_hits(jev_server), ==, 3);
	g_assert_cmpuint(tserver_hits(ollama_server), ==, 3);
	g_assert_cmpstr(jev_server->last_path, ==, "/v1/systemone");
	g_assert_cmpstr(ollama_server->last_path, ==, "/v1/systemone");
	g_assert_nonnull(strstr(ollama_server->last_body, "\"model\":\"tev1\""));
	g_assert_nonnull(strstr(jev_server->last_body, "Is a refund requested?"));

	summaries = ai_benchmark_dup_summaries(bench, FALSE);
	s = summary_for(summaries, "jev");
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, 1.0);
	g_assert_cmpint(ai_benchmark_summary_get_input_tokens(s), ==, 80);
	g_assert_cmpint(ai_benchmark_summary_get_output_tokens(s), ==, 4);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_brier(s), 0.0025, 1e-9);
	s = summary_for(summaries, "tev1");
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, 0.0);
	g_assert_cmpint(ai_benchmark_summary_get_input_tokens(s), ==, -1);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_brier(s), 0.36, 1e-9);
	g_assert_cmpstr(ai_benchmark_sample_get_model(sample_at(bench, 0)), ==, "jev-1.14.0");

	tserver_free(jev_server);
	tserver_free(ollama_server);
}

/* A Laya target refuses a box locally: an error sample, and no request sent. */
static void
test_laya_refusal_sends_nothing(
	void
){
	TServer *server = tserver_new();
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiBenchmarkTarget) laya = ai_benchmark_target_new("laya", AI_BENCHMARK_KIND_DECISION, "laya", NULL);
	g_autoptr(AiDecisionRequest) request = ai_decision_request_new("text");
	g_autofree gchar *url = g_strdup_printf("base-url=%s", server->base_url);
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_decision_request_add_box(request, "logo", "Where is the logo?", NULL));
	g_assert_true(ai_benchmark_target_add_property(laya, url, NULL));
	g_assert_true(ai_benchmark_add_target(bench, laya, NULL));
	add_case(bench, ai_benchmark_case_new_decision("box", request));
	ai_benchmark_set_iterations(bench, 1);
	ai_benchmark_set_warmup(bench, 0);

	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	g_assert_cmpint(ai_benchmark_sample_get_outcome(sample_at(bench, 0)), ==, AI_BENCHMARK_OUTCOME_ERROR);
	g_assert_nonnull(ai_benchmark_sample_get_error(sample_at(bench, 0)));
	g_assert_cmpuint(tserver_hits(server), ==, 0);
	tserver_free(server);
}

/*
 * timeout-is-error: a server slower than the deadline produces an error
 * sample that says so, and the run does not wait for the server.
 */
static void
test_timeout_is_error(
	void
){
	TServer *server = tserver_new();
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(GError) error = NULL;
	AiBenchmarkSample *sample;

	tserver_set_response(server, 200,
		"{\"model\":\"tev1\",\"answers\":{\"refund\":{\"type\":\"noul\",\"noul\":0.9}}}");
	tserver_set_delay(server, 600);
	ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, server->base_url);
	ai_benchmark_set_config(bench, config);
	add_target(bench, ai_benchmark_target_new("slow", AI_BENCHMARK_KIND_DECISION, "ollama", "tev1"));
	add_case(bench, refund_case("true"));
	ai_benchmark_set_iterations(bench, 1);
	ai_benchmark_set_warmup(bench, 0);
	ai_benchmark_set_timeout_ms(bench, 50);

	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	sample = sample_at(bench, 0);
	g_assert_cmpint(ai_benchmark_sample_get_outcome(sample), ==, AI_BENCHMARK_OUTCOME_ERROR);
	g_assert_cmpstr(ai_benchmark_sample_get_error(sample), ==, "timed out after 50 ms");
	g_assert_cmpint(ai_benchmark_sample_get_elapsed_us(sample), <, 600 * 1000);
	tserver_free(server);
}

/*
 * cancel-keeps-samples: cancelling between calls stops the run with
 * G_IO_ERROR_CANCELLED and keeps every sample already taken.
 */
static void
test_cancel_keeps_samples(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockProvider) a = mock_chat("ok", 1);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	SampleLog log = { NULL, 2, NULL };

	log.seen = g_ptr_array_new_with_free_func(g_object_unref);
	log.cancellable = cancellable;
	add_target(bench, object_target("A", AI_BENCHMARK_KIND_CHAT, a));
	add_case(bench, ai_benchmark_case_new_chat("one", "first"));
	ai_benchmark_set_iterations(bench, 10);
	g_signal_connect(bench, "sample", G_CALLBACK(on_sample), &log);

	g_assert_false(run_async_and_wait(bench, cancellable, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_cmpuint(ai_benchmark_get_samples(bench)->len, ==, 2);
	g_assert_false(ai_benchmark_is_running(bench));
	g_ptr_array_unref(log.seen);
}

static gboolean
cancel_later(
	gpointer cancellable
){
	g_cancellable_cancel(G_CANCELLABLE(cancellable));
	return G_SOURCE_REMOVE;
}

/*
 * cancel-in-flight: a call cut short by the user is not recorded -- its
 * latency would be an artefact of when the user pressed Ctrl+C.
 */
static void
test_cancel_in_flight(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockProvider) a = mock_chat("ok", 200);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GError) error = NULL;

	add_target(bench, object_target("A", AI_BENCHMARK_KIND_CHAT, a));
	add_case(bench, ai_benchmark_case_new_chat("one", "first"));
	ai_benchmark_set_warmup(bench, 0);
	g_timeout_add(30, cancel_later, cancellable);

	g_assert_false(run_async_and_wait(bench, cancellable, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_cmpuint(ai_benchmark_get_samples(bench)->len, ==, 0);
}

/* already-running: a second run while one is in flight is refused. */
static void
test_already_running(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockProvider) a = mock_chat("ok", 20);
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(GMainLoop) second_loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(AiBenchmarkCase) late = ai_benchmark_case_new_chat("late", "x");
	g_autoptr(GError) error = NULL;
	Wait first = { NULL, FALSE, FALSE, NULL };
	Wait second = { NULL, FALSE, FALSE, NULL };

	add_target(bench, object_target("A", AI_BENCHMARK_KIND_CHAT, a));
	add_case(bench, ai_benchmark_case_new_chat("one", "first"));
	ai_benchmark_set_iterations(bench, 1);
	ai_benchmark_set_warmup(bench, 0);

	first.loop = loop;
	second.loop = second_loop;
	ai_benchmark_run_async(bench, NULL, on_run_done, &first);
	g_assert_true(ai_benchmark_is_running(bench));
	g_assert_false(ai_benchmark_add_case(bench, late, &error));
	g_assert_cmpstr(error->message, ==, "cannot add a case while the benchmark is running");
	g_clear_error(&error);
	/* The run's plan indexes the targets and their clients: neither may change. */
	{
		const gchar *keep[] = { "A", NULL };
		guint measured;

		g_assert_false(ai_benchmark_select_targets(bench, keep, &error));
		g_assert_cmpstr(error->message, ==, "cannot select targets while the benchmark is running");
		g_clear_error(&error);
		g_assert_false(ai_benchmark_prepare(bench, &error));
		g_assert_cmpstr(error->message, ==, "cannot prepare while the benchmark is running");
		g_clear_error(&error);
		g_assert_cmpuint(ai_benchmark_count_calls(bench, &measured), ==, 1);
	}
	ai_benchmark_run_async(bench, NULL, on_run_done, &second);
	if (!second.done)
		g_main_loop_run(second_loop);
	g_assert_false(second.ok);
	g_assert_cmpstr(second.error->message, ==, "the benchmark is already running");
	if (!first.done)
		g_main_loop_run(loop);
	g_assert_true(first.ok);
	g_clear_error(&second.error);
}

/* An Ollama embedding batch over a socket, scored by similarity ranking. */
static void
test_embedding_over_socket(
	void
){
	TServer *server = tserver_new();
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiConfig) config = ai_config_new();
	static const gchar *const texts[] = { "cat", "kitten", "invoice", NULL };
	g_autoptr(AiBenchmarkCase) bench_case = ai_benchmark_case_new_embedding("pets", texts);
	g_autoptr(GError) error = NULL;

	tserver_set_response(server, 200,
		"{\"model\":\"nomic-embed-text\",\"embeddings\":[[1,0,0],[0.9,0.1,0],[0,0,1]]}");
	ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, server->base_url);
	ai_benchmark_set_config(bench, config);
	g_assert_true(ai_benchmark_case_expect_similarity(bench_case, 0, 1, 0, 2, NULL));
	add_target(bench, ai_benchmark_target_new("nomic", AI_BENCHMARK_KIND_EMBEDDING, "ollama", "nomic-embed-text"));
	g_assert_true(ai_benchmark_add_case(bench, bench_case, NULL));
	ai_benchmark_set_iterations(bench, 1);
	ai_benchmark_set_warmup(bench, 0);

	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(server->last_path, ==, "/api/embed");
	g_assert_nonnull(strstr(server->last_body, "nomic-embed-text"));
	g_assert_cmpint(ai_benchmark_sample_get_outcome(sample_at(bench, 0)), ==, AI_BENCHMARK_OUTCOME_PASS);
	g_assert_cmpstr(ai_benchmark_sample_get_model(sample_at(bench, 0)), ==, "nomic-embed-text");
	tserver_free(server);
}

/* Image generation over a socket: measured and never scored. */
static void
test_image_over_socket(
	void
){
	TServer *server = tserver_new();
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiConfig) config = ai_config_new();
	g_autoptr(GError) error = NULL;

	tserver_set_response(server, 200, "{\"created\":1,\"data\":[{\"b64_json\":\"aGk=\"}]}");
	ai_config_set_base_url(config, AI_PROVIDER_GROK, server->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_GROK, "test-key");
	ai_config_set_max_retries(config, 0);
	ai_benchmark_set_config(bench, config);
	add_target(bench, ai_benchmark_target_new("grok", AI_BENCHMARK_KIND_IMAGE, "grok", NULL));
	add_case(bench, ai_benchmark_case_new_image("fox", "a red fox"));
	ai_benchmark_set_iterations(bench, 1);
	ai_benchmark_set_warmup(bench, 0);

	g_assert_true(ai_benchmark_run(bench, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(server->last_path, ==, "/v1/images/generations");
	g_assert_cmpint(ai_benchmark_sample_get_outcome(sample_at(bench, 0)), ==, AI_BENCHMARK_OUTCOME_UNSCORED);
	tserver_free(server);
}

int
main(
	int    argc,
	char **argv
){
	static const gchar *const unset[] = {
		"TYPESAFE_API_KEY", "TYPESAFE_BASE_URL", "TYPESAFE_DEFAULT_MODEL",
		"OLLAMA_HOST", "OLLAMA_API_KEY", "LAYA_BASE_URL", "LAYA_API_KEY",
		"AI_PROVIDER", "AI_GLIB_DEFAULT_PROVIDER", "XAI_API_KEY", "GROK_API_KEY",
		"ANTHROPIC_API_KEY", "CLAUDE_API_KEY", NULL
	};
	gint status;
	guint i;

	/* Before g_test_init and any TServer thread: nothing real is reachable. */
	for (i = 0; unset[i] != NULL; i++)
		g_unsetenv(unset[i]);
	scratch_dir = g_dir_make_tmp("ai-benchmark-XXXXXX", NULL);
	g_assert_nonnull(scratch_dir);
	g_setenv("HOME", scratch_dir, TRUE);
	g_setenv("XDG_CONFIG_HOME", scratch_dir, TRUE);
	g_setenv("XDG_STATE_HOME", scratch_dir, TRUE);
	g_setenv("XDG_CACHE_HOME", scratch_dir, TRUE);
	g_setenv("XDG_DATA_HOME", scratch_dir, TRUE);
	g_assert_cmpint(g_chdir(scratch_dir), ==, 0);

	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/benchmark/run/defaults-and-properties", test_defaults_and_properties);
	g_test_add_func("/benchmark/run/duplicates-refused", test_duplicates_refused);
	g_test_add_func("/benchmark/run/select-targets", test_select_targets);
	g_test_add_func("/benchmark/run/count-calls", test_count_calls);
	g_test_add_func("/benchmark/run/round-robin", test_round_robin_and_signal);
	g_test_add_func("/benchmark/run/warmup-excluded", test_warmup_excluded);
	g_test_add_func("/benchmark/run/latency-and-ttft", test_latency_and_ttft);
	g_test_add_func("/benchmark/run/decision-accuracy", test_decision_accuracy);
	g_test_add_func("/benchmark/run/error-is-not-no", test_error_is_not_no);
	g_test_add_func("/benchmark/run/capability-refused", test_capability_refused_and_skipped);
	g_test_add_func("/benchmark/run/stream-capability", test_stream_capability);
	g_test_add_func("/benchmark/run/nothing-to-run", test_nothing_to_run);
	g_test_add_func("/benchmark/run/property-overrides", test_property_overrides);
	g_test_add_func("/benchmark/run/ollama-decision-model", test_ollama_decision_model);
	g_test_add_func("/benchmark/run/jev-vs-ollama", test_jev_vs_ollama);
	g_test_add_func("/benchmark/run/laya-refusal-sends-nothing", test_laya_refusal_sends_nothing);
	g_test_add_func("/benchmark/run/timeout-is-error", test_timeout_is_error);
	g_test_add_func("/benchmark/run/cancel-keeps-samples", test_cancel_keeps_samples);
	g_test_add_func("/benchmark/run/cancel-in-flight", test_cancel_in_flight);
	g_test_add_func("/benchmark/run/already-running", test_already_running);
	g_test_add_func("/benchmark/run/embedding-over-socket", test_embedding_over_socket);
	g_test_add_func("/benchmark/run/image-over-socket", test_image_over_socket);
	status = g_test_run();

	{
		g_autoptr(GSubprocess) cleanup = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL,
		                                                  "rm", "-rf", "--", scratch_dir, NULL);

		if (cleanup != NULL)
			g_subprocess_wait(cleanup, NULL, NULL);
	}
	g_free(scratch_dir);
	return status;
}
