/*
 * ai-benchmark-sample.c - One measured run of one case on one target
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A sample is plain data: the runner fills one in per measured call and
 * hands it out on AiBenchmark::sample. The constructor and setters are
 * public so a binding (or a test) can feed measurements taken elsewhere
 * into AiBenchmarkSummary and get the same statistics.
 *
 * Every "not available" is -1, never 0. A provider that reports no usage
 * has not reported zero tokens, and a run with no streamed text has no
 * time to first token rather than an instant one.
 */

#include "bench/ai-benchmark-sample.h"

struct _AiBenchmarkSample
{
	GObject             parent_instance;
	gchar              *target;
	gchar              *case_id;
	gchar              *model;
	guint               iteration;
	gint64              elapsed_us;
	gint64              ttft_us;
	gint64              input_tokens;
	gint64              output_tokens;
	AiBenchmarkOutcome  outcome;
	gdouble             brier;
	gdouble             log_loss;
	gchar              *detail;
	gchar              *error;
};

G_DEFINE_TYPE(AiBenchmarkSample, ai_benchmark_sample, G_TYPE_OBJECT)

static void
ai_benchmark_sample_finalize(
	GObject *object
){
	AiBenchmarkSample *self;

	self = AI_BENCHMARK_SAMPLE(object);
	g_free(self->target);
	g_free(self->case_id);
	g_free(self->model);
	g_free(self->detail);
	g_free(self->error);
	G_OBJECT_CLASS(ai_benchmark_sample_parent_class)->finalize(object);
}

static void
ai_benchmark_sample_class_init(
	AiBenchmarkSampleClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = ai_benchmark_sample_finalize;
}

static void
ai_benchmark_sample_init(
	AiBenchmarkSample *self
){
	self->ttft_us = -1;
	self->input_tokens = -1;
	self->output_tokens = -1;
	self->outcome = AI_BENCHMARK_OUTCOME_UNSCORED;
	self->brier = -1;
	self->log_loss = -1;
}

/**
 * ai_benchmark_sample_new:
 * @target: the target's label
 * @case_id: the case's identifier
 * @iteration: zero-based measured iteration (warmup runs are not samples)
 * @elapsed_us: wall time of the whole call, in microseconds
 *
 * Creates an unscored sample with no tokens and no time to first token.
 *
 * Returns: (transfer full): a new sample
 */
AiBenchmarkSample *
ai_benchmark_sample_new(
	const gchar *target,
	const gchar *case_id,
	guint        iteration,
	gint64       elapsed_us
){
	g_autoptr(AiBenchmarkSample) self = NULL;

	g_return_val_if_fail(target != NULL, NULL);
	g_return_val_if_fail(case_id != NULL, NULL);

	self = (AiBenchmarkSample *)g_object_new(AI_TYPE_BENCHMARK_SAMPLE, NULL);
	self->target = g_strdup(target);
	self->case_id = g_strdup(case_id);
	self->iteration = iteration;
	self->elapsed_us = elapsed_us < 0 ? 0 : elapsed_us;
	return (AiBenchmarkSample *)g_steal_pointer(&self);
}

/**
 * ai_benchmark_sample_get_target:
 * @self: a sample
 *
 * Returns: (transfer none): the target's label
 */
const gchar *
ai_benchmark_sample_get_target(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), NULL);
	return self->target;
}

/**
 * ai_benchmark_sample_get_case_id:
 * @self: a sample
 *
 * Returns: (transfer none): the case's identifier
 */
const gchar *
ai_benchmark_sample_get_case_id(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), NULL);
	return self->case_id;
}

/**
 * ai_benchmark_sample_get_iteration:
 * @self: a sample
 *
 * Returns: the zero-based measured iteration
 */
guint
ai_benchmark_sample_get_iteration(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), 0);
	return self->iteration;
}

/**
 * ai_benchmark_sample_get_elapsed_us:
 * @self: a sample
 *
 * Wall time from just before the call was made to its completion
 * callback, measured by the runner with g_get_monotonic_time(). An
 * errored call still has one; it is excluded from latency statistics.
 *
 * Returns: microseconds
 */
gint64
ai_benchmark_sample_get_elapsed_us(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), 0);
	return self->elapsed_us;
}

/**
 * ai_benchmark_sample_set_ttft_us:
 * @self: a sample
 * @ttft_us: time to the first streamed text, or -1 for none
 */
void
ai_benchmark_sample_set_ttft_us(
	AiBenchmarkSample *self,
	gint64             ttft_us
){
	g_return_if_fail(AI_IS_BENCHMARK_SAMPLE(self));
	self->ttft_us = ttft_us < 0 ? -1 : ttft_us;
}

/**
 * ai_benchmark_sample_get_ttft_us:
 * @self: a sample
 *
 * Returns: microseconds to the first streamed text, or -1 when the call
 *   did not stream or streamed no text
 */
gint64
ai_benchmark_sample_get_ttft_us(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), -1);
	return self->ttft_us;
}

/**
 * ai_benchmark_sample_set_tokens:
 * @self: a sample
 * @input_tokens: reported input tokens, or -1 when not reported
 * @output_tokens: reported output tokens, or -1 when not reported
 */
void
ai_benchmark_sample_set_tokens(
	AiBenchmarkSample *self,
	gint64             input_tokens,
	gint64             output_tokens
){
	g_return_if_fail(AI_IS_BENCHMARK_SAMPLE(self));
	self->input_tokens = input_tokens < 0 ? -1 : input_tokens;
	self->output_tokens = output_tokens < 0 ? -1 : output_tokens;
}

/**
 * ai_benchmark_sample_get_input_tokens:
 * @self: a sample
 *
 * Returns: reported input tokens, or -1 when the provider reported none
 */
gint64
ai_benchmark_sample_get_input_tokens(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), -1);
	return self->input_tokens;
}

/**
 * ai_benchmark_sample_get_output_tokens:
 * @self: a sample
 *
 * Returns: reported output tokens, or -1 when the provider reported none
 */
gint64
ai_benchmark_sample_get_output_tokens(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), -1);
	return self->output_tokens;
}

/**
 * ai_benchmark_sample_set_outcome:
 * @self: a sample
 * @outcome: how the run ended
 * @brier: mean Brier score of the scored probabilistic answers, or -1
 * @log_loss: mean log-loss of the scored probabilistic answers, or -1
 * @detail: (nullable): why it failed or was unscored, for a person
 */
void
ai_benchmark_sample_set_outcome(
	AiBenchmarkSample  *self,
	AiBenchmarkOutcome  outcome,
	gdouble             brier,
	gdouble             log_loss,
	const gchar        *detail
){
	g_return_if_fail(AI_IS_BENCHMARK_SAMPLE(self));
	self->outcome = outcome;
	self->brier = brier < 0 ? -1 : brier;
	self->log_loss = log_loss < 0 ? -1 : log_loss;
	g_free(self->detail);
	self->detail = g_strdup(detail);
}

/**
 * ai_benchmark_sample_get_outcome:
 * @self: a sample
 *
 * Returns: how the run ended
 */
AiBenchmarkOutcome
ai_benchmark_sample_get_outcome(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), AI_BENCHMARK_OUTCOME_ERROR);
	return self->outcome;
}

/**
 * ai_benchmark_sample_get_brier:
 * @self: a sample
 *
 * The squared distance between the predicted distribution and the
 * labelled answer, averaged over the scored questions: 0 is perfect, and
 * a Noul that always says 0.5 scores 0.25.
 *
 * Returns: the Brier score, or -1 when nothing probabilistic was scored
 */
gdouble
ai_benchmark_sample_get_brier(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), -1);
	return self->brier;
}

/**
 * ai_benchmark_sample_get_log_loss:
 * @self: a sample
 *
 * Returns: the mean natural-log loss, or -1 when nothing probabilistic
 *   was scored
 */
gdouble
ai_benchmark_sample_get_log_loss(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), -1);
	return self->log_loss;
}

/**
 * ai_benchmark_sample_get_detail:
 * @self: a sample
 *
 * Returns: (transfer none) (nullable): why the run failed an expectation
 *   or was left unscored
 */
const gchar *
ai_benchmark_sample_get_detail(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), NULL);
	return self->detail;
}

/**
 * ai_benchmark_sample_set_error:
 * @self: a sample
 * @message: what went wrong
 *
 * Marks the run as %AI_BENCHMARK_OUTCOME_ERROR and drops any score.
 */
void
ai_benchmark_sample_set_error(
	AiBenchmarkSample *self,
	const gchar       *message
){
	g_return_if_fail(AI_IS_BENCHMARK_SAMPLE(self));
	g_return_if_fail(message != NULL);
	g_free(self->error);
	self->error = g_strdup(message);
	self->outcome = AI_BENCHMARK_OUTCOME_ERROR;
	self->brier = -1;
	self->log_loss = -1;
}

/**
 * ai_benchmark_sample_get_error:
 * @self: a sample
 *
 * Returns: (transfer none) (nullable): the failure, or %NULL when the
 *   call succeeded
 */
const gchar *
ai_benchmark_sample_get_error(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), NULL);
	return self->error;
}

/**
 * ai_benchmark_sample_set_model:
 * @self: a sample
 * @model: (nullable): the model the response says answered
 */
void
ai_benchmark_sample_set_model(
	AiBenchmarkSample *self,
	const gchar       *model
){
	g_return_if_fail(AI_IS_BENCHMARK_SAMPLE(self));
	g_free(self->model);
	self->model = g_strdup(model);
}

/**
 * ai_benchmark_sample_get_model:
 * @self: a sample
 *
 * What the response reported, which can differ from what was asked for
 * (an alias such as `jev-latest` resolves to a release).
 *
 * Returns: (transfer none) (nullable): the reported model
 */
const gchar *
ai_benchmark_sample_get_model(
	AiBenchmarkSample *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_SAMPLE(self), NULL);
	return self->model;
}
