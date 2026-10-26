/*
 * test-benchmark-stats.c - AiBenchmarkSummary and AiBenchmarkSample maths
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Every number the benchmark prints comes from AiBenchmarkSummary, so
 * every rule it follows is asserted here against hand-computed values:
 * nearest-rank percentiles, errors excluded from latency and accuracy,
 * unscored runs excluded from accuracy, and "no data" as -1 rather
 * than 0.
 */

#include <ai-glib.h>
#include <math.h>

#define EPS (1e-9)

static AiBenchmarkSample *
sample_ms(
	const gchar *target,
	const gchar *case_id,
	guint        iteration,
	gdouble      ms
){
	return ai_benchmark_sample_new(target, case_id, iteration, (gint64)(ms * 1000.0));
}

/*
 * Samples with latencies 1..n ms, added in reverse so the summary has to
 * sort them.
 */
static GPtrArray *
ramp(
	guint n
){
	GPtrArray *samples;
	guint i;

	samples = g_ptr_array_new_with_free_func(g_object_unref);
	for (i = n; i > 0; i--)
		g_ptr_array_add(samples, sample_ms("t", "c", n - i, (gdouble)i));
	return samples;
}

typedef struct
{
	guint   n;
	gdouble p50;
	gdouble p95;
	gdouble mean;
} PercentileRow;

/*
 * Nearest rank: rank = ceil(p/100 * n), so with few samples p95 is the
 * maximum, and every reported value is one a real call took.
 */
static void
test_percentiles(
	void
){
	static const PercentileRow rows[] = {
		{   1,  1,  1,  1.0 },
		{   2,  1,  2,  1.5 },
		{   3,  2,  3,  2.0 },
		{   4,  2,  4,  2.5 },
		{  20, 10, 19, 10.5 },
		{ 100, 50, 95, 50.5 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++)
	{
		g_autoptr(GPtrArray) samples = ramp(rows[i].n);
		g_autoptr(AiBenchmarkSummary) s = ai_benchmark_summary_new("t", NULL, samples);

		g_test_message("n=%u", rows[i].n);
		g_assert_cmpuint(ai_benchmark_summary_get_runs(s), ==, rows[i].n);
		g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_p50_ms(s), rows[i].p50, EPS);
		g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_p95_ms(s), rows[i].p95, EPS);
		g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_ms(s), rows[i].mean, EPS);
		g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_min_ms(s), 1.0, EPS);
		g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_max_ms(s), (gdouble)rows[i].n, EPS);
	}
}

/* No samples at all: every statistic is absent, not zero. */
static void
test_empty(
	void
){
	g_autoptr(GPtrArray) samples = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(AiBenchmarkSummary) s = ai_benchmark_summary_new("t", NULL, samples);

	g_assert_cmpuint(ai_benchmark_summary_get_runs(s), ==, 0);
	g_assert_cmpfloat(ai_benchmark_summary_get_error_rate(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_p50_ms(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_p95_ms(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_mean_ms(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_mean_ttft_ms(s), ==, -1);
	g_assert_cmpint(ai_benchmark_summary_get_input_tokens(s), ==, -1);
	g_assert_cmpint(ai_benchmark_summary_get_output_tokens(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_output_tokens_per_second(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_mean_brier(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_mean_log_loss(s), ==, -1);
}

/*
 * A target whose every call failed has runs and errors, and no latency:
 * printing 0 ms would make it the fastest row in the table.
 */
static void
test_all_errors_have_no_latency(
	void
){
	g_autoptr(GPtrArray) samples = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(AiBenchmarkSummary) s = NULL;
	guint i;

	for (i = 0; i < 3; i++)
	{
		AiBenchmarkSample *sample = sample_ms("t", "c", i, 30000);

		ai_benchmark_sample_set_error(sample, "timed out after 30000 ms");
		g_ptr_array_add(samples, sample);
	}
	s = ai_benchmark_summary_new("t", NULL, samples);
	g_assert_cmpuint(ai_benchmark_summary_get_runs(s), ==, 3);
	g_assert_cmpuint(ai_benchmark_summary_get_errors(s), ==, 3);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_error_rate(s), 1.0, EPS);
	g_assert_cmpfloat(ai_benchmark_summary_get_p50_ms(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_mean_ms(s), ==, -1);
	g_assert_cmpfloat(ai_benchmark_summary_get_accuracy(s), ==, -1);
}

/*
 * Errors are excluded from latency and from accuracy; unscored runs are
 * excluded from accuracy but count for latency.
 */
static void
test_errors_and_unscored_excluded(
	void
){
	g_autoptr(GPtrArray) samples = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(AiBenchmarkSummary) s = NULL;
	AiBenchmarkSample *sample;

	sample = sample_ms("t", "c", 0, 10);
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_PASS, -1, -1, NULL);
	g_ptr_array_add(samples, sample);
	sample = sample_ms("t", "c", 1, 20);
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_FAIL, -1, -1, "expected true");
	g_ptr_array_add(samples, sample);
	sample = sample_ms("t", "c", 2, 30);
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_PASS, -1, -1, NULL);
	g_ptr_array_add(samples, sample);
	sample = sample_ms("t", "c", 3, 40);
	g_ptr_array_add(samples, sample); /* unscored */
	sample = sample_ms("t", "c", 4, 99999);
	ai_benchmark_sample_set_error(sample, "connection refused");
	g_ptr_array_add(samples, sample);

	s = ai_benchmark_summary_new("t", NULL, samples);
	g_assert_cmpuint(ai_benchmark_summary_get_runs(s), ==, 5);
	g_assert_cmpuint(ai_benchmark_summary_get_errors(s), ==, 1);
	g_assert_cmpuint(ai_benchmark_summary_get_scored(s), ==, 3);
	g_assert_cmpuint(ai_benchmark_summary_get_passed(s), ==, 2);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_accuracy(s), 2.0 / 3.0, EPS);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_error_rate(s), 0.2, EPS);
	/* latency over the four successes only: 10, 20, 30, 40 */
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_max_ms(s), 40, EPS);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_ms(s), 25, EPS);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_p50_ms(s), 20, EPS);
}

/*
 * Tokens total only over runs that reported them, and throughput only
 * over the wall time of those runs. A provider that reports nothing has
 * no token figure at all.
 */
static void
test_tokens(
	void
){
	g_autoptr(GPtrArray) samples = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(AiBenchmarkSummary) s = NULL;
	AiBenchmarkSample *sample;

	sample = sample_ms("t", "c", 0, 500);
	ai_benchmark_sample_set_tokens(sample, 10, 50);
	g_ptr_array_add(samples, sample);
	sample = sample_ms("t", "c", 1, 1500);
	ai_benchmark_sample_set_tokens(sample, 20, 150);
	g_ptr_array_add(samples, sample);
	sample = sample_ms("t", "c", 2, 9000);
	g_ptr_array_add(samples, sample); /* no usage reported */

	s = ai_benchmark_summary_new("t", NULL, samples);
	g_assert_cmpint(ai_benchmark_summary_get_input_tokens(s), ==, 30);
	g_assert_cmpint(ai_benchmark_summary_get_output_tokens(s), ==, 200);
	/* 200 tokens over 2.0 s of the two reporting runs */
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_output_tokens_per_second(s), 100.0, EPS);

	/* Only input reported: no output total and no throughput. */
	{
		g_autoptr(GPtrArray) only_in = g_ptr_array_new_with_free_func(g_object_unref);
		g_autoptr(AiBenchmarkSummary) t = NULL;

		sample = sample_ms("t", "c", 0, 100);
		ai_benchmark_sample_set_tokens(sample, 7, -1);
		g_ptr_array_add(only_in, sample);
		t = ai_benchmark_summary_new("t", NULL, only_in);
		g_assert_cmpint(ai_benchmark_summary_get_input_tokens(t), ==, 7);
		g_assert_cmpint(ai_benchmark_summary_get_output_tokens(t), ==, -1);
		g_assert_cmpfloat(ai_benchmark_summary_get_output_tokens_per_second(t), ==, -1);
	}

	/* Zero output tokens is a measurement: 0 tok/s, not absent. */
	{
		g_autoptr(GPtrArray) zero = g_ptr_array_new_with_free_func(g_object_unref);
		g_autoptr(AiBenchmarkSummary) t = NULL;

		sample = sample_ms("t", "c", 0, 100);
		ai_benchmark_sample_set_tokens(sample, 3, 0);
		g_ptr_array_add(zero, sample);
		t = ai_benchmark_summary_new("t", NULL, zero);
		g_assert_cmpint(ai_benchmark_summary_get_output_tokens(t), ==, 0);
		g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_output_tokens_per_second(t), 0, EPS);
	}
}

static void
test_ttft_and_calibration(
	void
){
	g_autoptr(GPtrArray) samples = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(AiBenchmarkSummary) s = NULL;
	AiBenchmarkSample *sample;

	sample = sample_ms("t", "c", 0, 100);
	ai_benchmark_sample_set_ttft_us(sample, 20000);
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_PASS, 0.04, 0.2, NULL);
	g_ptr_array_add(samples, sample);
	sample = sample_ms("t", "c", 1, 100);
	ai_benchmark_sample_set_ttft_us(sample, 40000);
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_FAIL, 0.64, 1.6, NULL);
	g_ptr_array_add(samples, sample);
	sample = sample_ms("t", "c", 2, 100);
	g_ptr_array_add(samples, sample); /* neither streamed nor probabilistic */

	s = ai_benchmark_summary_new("t", NULL, samples);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_ttft_ms(s), 30, EPS);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_brier(s), 0.34, EPS);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_log_loss(s), 0.9, EPS);
}

/* A summary reads only its own target, and only its own case when asked. */
static void
test_filtering(
	void
){
	g_autoptr(GPtrArray) samples = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(AiBenchmarkSummary) a = NULL;
	g_autoptr(AiBenchmarkSummary) a_x = NULL;
	g_autoptr(AiBenchmarkSummary) b = NULL;
	g_autoptr(AiBenchmarkSummary) none = NULL;

	g_ptr_array_add(samples, sample_ms("a", "x", 0, 1));
	g_ptr_array_add(samples, sample_ms("a", "y", 0, 3));
	g_ptr_array_add(samples, sample_ms("b", "x", 0, 100));

	a = ai_benchmark_summary_new("a", NULL, samples);
	a_x = ai_benchmark_summary_new("a", "x", samples);
	b = ai_benchmark_summary_new("b", NULL, samples);
	none = ai_benchmark_summary_new("nobody", NULL, samples);

	g_assert_cmpuint(ai_benchmark_summary_get_runs(a), ==, 2);
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_ms(a), 2, EPS);
	g_assert_cmpuint(ai_benchmark_summary_get_runs(a_x), ==, 1);
	g_assert_cmpstr(ai_benchmark_summary_get_case_id(a_x), ==, "x");
	g_assert_null(ai_benchmark_summary_get_case_id(a));
	g_assert_cmpfloat_with_epsilon(ai_benchmark_summary_get_mean_ms(b), 100, EPS);
	g_assert_cmpuint(ai_benchmark_summary_get_runs(none), ==, 0);
}

/* The sample's own invariants: -1 for absent, and an error drops scores. */
static void
test_sample(
	void
){
	g_autoptr(AiBenchmarkSample) sample = ai_benchmark_sample_new("t", "c", 4, -5);

	g_assert_cmpint(ai_benchmark_sample_get_elapsed_us(sample), ==, 0);
	g_assert_cmpuint(ai_benchmark_sample_get_iteration(sample), ==, 4);
	g_assert_cmpint(ai_benchmark_sample_get_ttft_us(sample), ==, -1);
	g_assert_cmpint(ai_benchmark_sample_get_input_tokens(sample), ==, -1);
	g_assert_cmpint(ai_benchmark_sample_get_output_tokens(sample), ==, -1);
	g_assert_cmpint(ai_benchmark_sample_get_outcome(sample), ==, AI_BENCHMARK_OUTCOME_UNSCORED);
	g_assert_null(ai_benchmark_sample_get_error(sample));

	ai_benchmark_sample_set_tokens(sample, -9, 12);
	g_assert_cmpint(ai_benchmark_sample_get_input_tokens(sample), ==, -1);
	g_assert_cmpint(ai_benchmark_sample_get_output_tokens(sample), ==, 12);

	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_PASS, 0.1, 0.3, NULL);
	ai_benchmark_sample_set_error(sample, "boom");
	g_assert_cmpint(ai_benchmark_sample_get_outcome(sample), ==, AI_BENCHMARK_OUTCOME_ERROR);
	g_assert_cmpfloat(ai_benchmark_sample_get_brier(sample), ==, -1);
	g_assert_cmpfloat(ai_benchmark_sample_get_log_loss(sample), ==, -1);
	g_assert_cmpstr(ai_benchmark_sample_get_error(sample), ==, "boom");
}

/* The vocabulary every output format uses. */
static void
test_enum_strings(
	void
){
	AiBenchmarkKind kind;

	g_assert_cmpstr(ai_benchmark_kind_to_string(AI_BENCHMARK_KIND_CHAT), ==, "chat");
	g_assert_cmpstr(ai_benchmark_kind_to_string(AI_BENCHMARK_KIND_DECISION), ==, "decision");
	g_assert_cmpstr(ai_benchmark_kind_to_string(AI_BENCHMARK_KIND_EMBEDDING), ==, "embedding");
	g_assert_cmpstr(ai_benchmark_kind_to_string(AI_BENCHMARK_KIND_IMAGE), ==, "image");
	g_assert_true(ai_benchmark_kind_from_string("decision", &kind));
	g_assert_cmpint(kind, ==, AI_BENCHMARK_KIND_DECISION);
	g_assert_false(ai_benchmark_kind_from_string("decisions", &kind));
	g_assert_false(ai_benchmark_kind_from_string(NULL, &kind));
	g_assert_cmpstr(ai_benchmark_outcome_to_string(AI_BENCHMARK_OUTCOME_UNSCORED), ==, "unscored");
	g_assert_cmpstr(ai_benchmark_outcome_to_string(AI_BENCHMARK_OUTCOME_PASS), ==, "pass");
	g_assert_cmpstr(ai_benchmark_outcome_to_string(AI_BENCHMARK_OUTCOME_FAIL), ==, "fail");
	g_assert_cmpstr(ai_benchmark_outcome_to_string(AI_BENCHMARK_OUTCOME_ERROR), ==, "error");
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/benchmark/stats/percentiles-nearest-rank", test_percentiles);
	g_test_add_func("/benchmark/stats/empty-is-absent", test_empty);
	g_test_add_func("/benchmark/stats/all-errors-no-latency", test_all_errors_have_no_latency);
	g_test_add_func("/benchmark/stats/errors-and-unscored-excluded", test_errors_and_unscored_excluded);
	g_test_add_func("/benchmark/stats/tokens", test_tokens);
	g_test_add_func("/benchmark/stats/ttft-and-calibration", test_ttft_and_calibration);
	g_test_add_func("/benchmark/stats/filtering", test_filtering);
	g_test_add_func("/benchmark/stats/sample", test_sample);
	g_test_add_func("/benchmark/stats/enum-strings", test_enum_strings);
	return g_test_run();
}
