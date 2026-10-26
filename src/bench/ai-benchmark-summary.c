/*
 * ai-benchmark-summary.c - Aggregate statistics over benchmark samples
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Three rules shape every number here:
 *
 *  1. Latency is computed over successful runs only. A timeout is not a
 *     slow answer; folding it into p95 would make a flaky target look
 *     slow instead of making it look flaky. Errors have their own column.
 *
 *  2. Accuracy is computed over scored runs only. An error is never a
 *     "no", and an unscored run (a case with no expectation) is neither
 *     right nor wrong.
 *
 *  3. "No data" is -1, never 0. A target whose every call failed has no
 *     p50, and printing 0 ms for it would read as the fastest target in
 *     the table.
 *
 * Percentiles are nearest-rank: the smallest observed value with at
 * least p% of the observations at or below it. It never interpolates, so
 * every reported latency is one a real call actually took, and with few
 * samples p95 is simply the maximum -- which is the honest answer for
 * five runs.
 */

#include "bench/ai-benchmark-summary.h"


struct _AiBenchmarkSummary
{
	GObject  parent_instance;
	gchar   *target;
	gchar   *case_id;
	guint    runs;
	guint    errors;
	guint    scored;
	guint    passed;
	gdouble  min_ms;
	gdouble  mean_ms;
	gdouble  p50_ms;
	gdouble  p95_ms;
	gdouble  max_ms;
	gdouble  mean_ttft_ms;
	gint64   input_tokens;
	gint64   output_tokens;
	gdouble  tokens_per_second;
	gdouble  input_tokens_per_second;
	gdouble  mean_brier;
	gdouble  mean_log_loss;
};

G_DEFINE_TYPE(AiBenchmarkSummary, ai_benchmark_summary, G_TYPE_OBJECT)

static void
ai_benchmark_summary_finalize(
	GObject *object
){
	AiBenchmarkSummary *self;

	self = AI_BENCHMARK_SUMMARY(object);
	g_free(self->target);
	g_free(self->case_id);
	G_OBJECT_CLASS(ai_benchmark_summary_parent_class)->finalize(object);
}

static void
ai_benchmark_summary_class_init(
	AiBenchmarkSummaryClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = ai_benchmark_summary_finalize;
}

static void
ai_benchmark_summary_init(
	AiBenchmarkSummary *self
){
	self->min_ms = -1;
	self->mean_ms = -1;
	self->p50_ms = -1;
	self->p95_ms = -1;
	self->max_ms = -1;
	self->mean_ttft_ms = -1;
	self->input_tokens = -1;
	self->output_tokens = -1;
	self->tokens_per_second = -1;
	self->input_tokens_per_second = -1;
	self->mean_brier = -1;
	self->mean_log_loss = -1;
}

static gint
compare_int64(
	gconstpointer a,
	gconstpointer b
){
	gint64 x;
	gint64 y;

	x = *(const gint64 *)a;
	y = *(const gint64 *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

/*
 * Nearest-rank percentile of an ascending array. rank = ceil(p * n / 100),
 * clamped into [1, n]; the answer is the rank-th smallest value. Integer
 * arithmetic on purpose: 0.95 * 20 in floating point is a hair over 19,
 * and ceil() of that is 20.
 */
static gint64
nearest_rank(
	const gint64 *sorted,
	guint         n,
	guint         percent
){
	guint64 rank;

	rank = ((guint64)percent * n + 99) / 100;
	if (rank < 1)
		rank = 1;
	if (rank > n)
		rank = n;
	return sorted[rank - 1];
}

/**
 * ai_benchmark_summary_new:
 * @target: the target label to summarise
 * @case_id: (nullable): one case, or %NULL for every case of @target
 * @samples: (element-type AiBenchmarkSample): samples to draw from; those
 *   for other targets or cases are ignored
 *
 * Computes the statistics described in docs/benchmark.org from the
 * samples belonging to @target (and @case_id, when given).
 *
 * Returns: (transfer full): a new summary
 */
AiBenchmarkSummary *
ai_benchmark_summary_new(
	const gchar *target,
	const gchar *case_id,
	GPtrArray   *samples
){
	g_autoptr(AiBenchmarkSummary) self = NULL;
	g_autoptr(GArray) latencies = NULL;
	gint64 latency_total;
	gint64 ttft_total;
	guint ttft_count;
	gint64 in_total;
	gint64 in_elapsed;
	gint64 out_total;
	gint64 out_elapsed;
	gboolean any_in;
	gboolean any_out;
	gdouble brier_total;
	gdouble loss_total;
	guint brier_count;
	guint loss_count;
	guint i;

	g_return_val_if_fail(target != NULL, NULL);
	g_return_val_if_fail(samples != NULL, NULL);

	self = (AiBenchmarkSummary *)g_object_new(AI_TYPE_BENCHMARK_SUMMARY, NULL);
	self->target = g_strdup(target);
	self->case_id = g_strdup(case_id);

	latencies = g_array_new(FALSE, FALSE, sizeof(gint64));
	latency_total = 0;
	ttft_total = 0;
	ttft_count = 0;
	in_total = 0;
	in_elapsed = 0;
	out_total = 0;
	out_elapsed = 0;
	any_in = FALSE;
	any_out = FALSE;
	brier_total = 0;
	loss_total = 0;
	brier_count = 0;
	loss_count = 0;

	/* One pass: count everything, keep successful latencies for sorting. */
	for (i = 0; i < samples->len; i++)
	{
		AiBenchmarkSample *sample = (AiBenchmarkSample *)g_ptr_array_index(samples, i);
		AiBenchmarkOutcome outcome;
		gint64 elapsed;

		if (g_strcmp0(ai_benchmark_sample_get_target(sample), target) != 0)
			continue;
		if (case_id != NULL && g_strcmp0(ai_benchmark_sample_get_case_id(sample), case_id) != 0)
			continue;

		self->runs++;
		outcome = ai_benchmark_sample_get_outcome(sample);
		if (outcome == AI_BENCHMARK_OUTCOME_ERROR)
		{
			self->errors++;
			continue;
		}

		elapsed = ai_benchmark_sample_get_elapsed_us(sample);
		g_array_append_val(latencies, elapsed);
		latency_total += elapsed;

		if (ai_benchmark_sample_get_ttft_us(sample) >= 0)
		{
			ttft_total += ai_benchmark_sample_get_ttft_us(sample);
			ttft_count++;
		}

		/* Tokens: a total only over runs that reported them. */
		if (ai_benchmark_sample_get_input_tokens(sample) >= 0)
		{
			in_total += ai_benchmark_sample_get_input_tokens(sample);
			in_elapsed += elapsed;
			any_in = TRUE;
		}
		if (ai_benchmark_sample_get_output_tokens(sample) >= 0)
		{
			out_total += ai_benchmark_sample_get_output_tokens(sample);
			out_elapsed += elapsed;
			any_out = TRUE;
		}

		if (outcome == AI_BENCHMARK_OUTCOME_PASS || outcome == AI_BENCHMARK_OUTCOME_FAIL)
		{
			self->scored++;
			if (outcome == AI_BENCHMARK_OUTCOME_PASS)
				self->passed++;
		}
		if (ai_benchmark_sample_get_brier(sample) >= 0)
		{
			brier_total += ai_benchmark_sample_get_brier(sample);
			brier_count++;
		}
		if (ai_benchmark_sample_get_log_loss(sample) >= 0)
		{
			loss_total += ai_benchmark_sample_get_log_loss(sample);
			loss_count++;
		}
	}

	/* Latency distribution over successful runs. */
	if (latencies->len > 0)
	{
		const gint64 *sorted;

		g_array_sort(latencies, compare_int64);
		sorted = (const gint64 *)latencies->data;
		self->min_ms = (gdouble)sorted[0] / 1000.0;
		self->max_ms = (gdouble)sorted[latencies->len - 1] / 1000.0;
		self->mean_ms = (gdouble)latency_total / (gdouble)latencies->len / 1000.0;
		self->p50_ms = (gdouble)nearest_rank(sorted, latencies->len, 50) / 1000.0;
		self->p95_ms = (gdouble)nearest_rank(sorted, latencies->len, 95) / 1000.0;
	}

	if (ttft_count > 0)
		self->mean_ttft_ms = (gdouble)ttft_total / (gdouble)ttft_count / 1000.0;
	/*
	 * Throughput over the wall time of the runs that reported the count.
	 * A rate needs something to rate: a decision model scores its answer
	 * from probabilities and generates nothing, so zero output tokens is
	 * "-" rather than a 0 tok/s that reads as a very slow model. Input
	 * throughput is the rate every kind can be compared on.
	 */
	if (any_in)
	{
		self->input_tokens = in_total;
		if (in_total > 0 && in_elapsed > 0)
			self->input_tokens_per_second = (gdouble)in_total / ((gdouble)in_elapsed / 1000000.0);
	}
	if (any_out)
	{
		self->output_tokens = out_total;
		if (out_total > 0 && out_elapsed > 0)
			self->tokens_per_second = (gdouble)out_total / ((gdouble)out_elapsed / 1000000.0);
	}
	if (brier_count > 0)
		self->mean_brier = brier_total / (gdouble)brier_count;
	if (loss_count > 0)
		self->mean_log_loss = loss_total / (gdouble)loss_count;

	return (AiBenchmarkSummary *)g_steal_pointer(&self);
}

/**
 * ai_benchmark_summary_get_target:
 * @self: a summary
 *
 * Returns: (transfer none): the target label
 */
const gchar *
ai_benchmark_summary_get_target(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), NULL);
	return self->target;
}

/**
 * ai_benchmark_summary_get_case_id:
 * @self: a summary
 *
 * Returns: (transfer none) (nullable): the case, or %NULL for a whole-target summary
 */
const gchar *
ai_benchmark_summary_get_case_id(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), NULL);
	return self->case_id;
}

/**
 * ai_benchmark_summary_get_runs:
 * @self: a summary
 *
 * Returns: measured runs, errors included
 */
guint
ai_benchmark_summary_get_runs(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), 0);
	return self->runs;
}

/**
 * ai_benchmark_summary_get_errors:
 * @self: a summary
 *
 * Returns: runs that failed or timed out
 */
guint
ai_benchmark_summary_get_errors(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), 0);
	return self->errors;
}

/**
 * ai_benchmark_summary_get_error_rate:
 * @self: a summary
 *
 * Returns: errors / runs from 0 to 1, or -1 with no runs
 */
gdouble
ai_benchmark_summary_get_error_rate(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	if (self->runs == 0)
		return -1;
	return (gdouble)self->errors / (gdouble)self->runs;
}

/**
 * ai_benchmark_summary_get_scored:
 * @self: a summary
 *
 * Returns: successful runs that were checked against an expectation
 */
guint
ai_benchmark_summary_get_scored(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), 0);
	return self->scored;
}

/**
 * ai_benchmark_summary_get_passed:
 * @self: a summary
 *
 * Returns: scored runs that met every expectation
 */
guint
ai_benchmark_summary_get_passed(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), 0);
	return self->passed;
}

/**
 * ai_benchmark_summary_get_accuracy:
 * @self: a summary
 *
 * Returns: passed / scored from 0 to 1, or -1 when nothing was scored
 */
gdouble
ai_benchmark_summary_get_accuracy(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	if (self->scored == 0)
		return -1;
	return (gdouble)self->passed / (gdouble)self->scored;
}

/**
 * ai_benchmark_summary_get_min_ms:
 * @self: a summary
 *
 * Returns: fastest successful run in milliseconds, or -1 with none
 */
gdouble
ai_benchmark_summary_get_min_ms(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->min_ms;
}

/**
 * ai_benchmark_summary_get_mean_ms:
 * @self: a summary
 *
 * Returns: mean successful latency in milliseconds, or -1 with none
 */
gdouble
ai_benchmark_summary_get_mean_ms(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->mean_ms;
}

/**
 * ai_benchmark_summary_get_p50_ms:
 * @self: a summary
 *
 * Returns: nearest-rank median latency in milliseconds, or -1 with none
 */
gdouble
ai_benchmark_summary_get_p50_ms(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->p50_ms;
}

/**
 * ai_benchmark_summary_get_p95_ms:
 * @self: a summary
 *
 * Returns: nearest-rank 95th percentile latency in milliseconds, or -1
 */
gdouble
ai_benchmark_summary_get_p95_ms(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->p95_ms;
}

/**
 * ai_benchmark_summary_get_max_ms:
 * @self: a summary
 *
 * Returns: slowest successful run in milliseconds, or -1 with none
 */
gdouble
ai_benchmark_summary_get_max_ms(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->max_ms;
}

/**
 * ai_benchmark_summary_get_mean_ttft_ms:
 * @self: a summary
 *
 * Returns: mean time to first streamed text in milliseconds, or -1 when
 *   no run streamed text
 */
gdouble
ai_benchmark_summary_get_mean_ttft_ms(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->mean_ttft_ms;
}

/**
 * ai_benchmark_summary_get_input_tokens:
 * @self: a summary
 *
 * Returns: total reported input tokens, or -1 when none were reported
 */
gint64
ai_benchmark_summary_get_input_tokens(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->input_tokens;
}

/**
 * ai_benchmark_summary_get_output_tokens:
 * @self: a summary
 *
 * Returns: total reported output tokens, or -1 when none were reported
 */
gint64
ai_benchmark_summary_get_output_tokens(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->output_tokens;
}

/**
 * ai_benchmark_summary_get_output_tokens_per_second:
 * @self: a summary
 *
 * Output tokens divided by the wall time of the runs that reported them.
 * Wall time includes the time to first token, so this is end-to-end
 * throughput, not decode speed.
 *
 * Returns: tokens per second, or -1 when no run reported output tokens
 *   or every run reported zero (a decision model generates nothing)
 */
gdouble
ai_benchmark_summary_get_output_tokens_per_second(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->tokens_per_second;
}

/**
 * ai_benchmark_summary_get_input_tokens_per_second:
 * @self: a summary
 *
 * Input tokens divided by the wall time of the runs that reported them:
 * how fast a target gets through what it was sent, end to end. Unlike
 * output throughput it exists for every kind that reports usage, so it is
 * the rate a hosted and a local decision model can be compared on.
 *
 * Returns: tokens per second, or -1 when no input tokens were reported
 */
gdouble
ai_benchmark_summary_get_input_tokens_per_second(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->input_tokens_per_second;
}

/**
 * ai_benchmark_summary_get_mean_brier:
 * @self: a summary
 *
 * Returns: mean Brier score over scored probabilistic runs, or -1
 */
gdouble
ai_benchmark_summary_get_mean_brier(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->mean_brier;
}

/**
 * ai_benchmark_summary_get_mean_log_loss:
 * @self: a summary
 *
 * Returns: mean log-loss over scored probabilistic runs, or -1
 */
gdouble
ai_benchmark_summary_get_mean_log_loss(
	AiBenchmarkSummary *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SUMMARY(self), -1);
	return self->mean_log_loss;
}
