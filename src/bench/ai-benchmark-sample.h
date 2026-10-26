/*
 * ai-benchmark-sample.h - One measured run of one case on one target
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include "bench/ai-benchmark-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_BENCHMARK_SAMPLE (ai_benchmark_sample_get_type())
G_DECLARE_FINAL_TYPE(AiBenchmarkSample, ai_benchmark_sample, AI, BENCHMARK_SAMPLE, GObject)

AiBenchmarkSample *
ai_benchmark_sample_new(
	const gchar *target,
	const gchar *case_id,
	guint        iteration,
	gint64       elapsed_us
);

const gchar *
ai_benchmark_sample_get_target(
	AiBenchmarkSample *self
);

const gchar *
ai_benchmark_sample_get_case_id(
	AiBenchmarkSample *self
);

guint
ai_benchmark_sample_get_iteration(
	AiBenchmarkSample *self
);

gint64
ai_benchmark_sample_get_elapsed_us(
	AiBenchmarkSample *self
);

void
ai_benchmark_sample_set_ttft_us(
	AiBenchmarkSample *self,
	gint64             ttft_us
);

gint64
ai_benchmark_sample_get_ttft_us(
	AiBenchmarkSample *self
);

void
ai_benchmark_sample_set_tokens(
	AiBenchmarkSample *self,
	gint64             input_tokens,
	gint64             output_tokens
);

gint64
ai_benchmark_sample_get_input_tokens(
	AiBenchmarkSample *self
);

gint64
ai_benchmark_sample_get_output_tokens(
	AiBenchmarkSample *self
);

void
ai_benchmark_sample_set_outcome(
	AiBenchmarkSample  *self,
	AiBenchmarkOutcome  outcome,
	gdouble             brier,
	gdouble             log_loss,
	const gchar        *detail
);

AiBenchmarkOutcome
ai_benchmark_sample_get_outcome(
	AiBenchmarkSample *self
);

gdouble
ai_benchmark_sample_get_brier(
	AiBenchmarkSample *self
);

gdouble
ai_benchmark_sample_get_log_loss(
	AiBenchmarkSample *self
);

const gchar *
ai_benchmark_sample_get_detail(
	AiBenchmarkSample *self
);

void
ai_benchmark_sample_set_error(
	AiBenchmarkSample *self,
	const gchar       *message
);

const gchar *
ai_benchmark_sample_get_error(
	AiBenchmarkSample *self
);

void
ai_benchmark_sample_set_model(
	AiBenchmarkSample *self,
	const gchar       *model
);

const gchar *
ai_benchmark_sample_get_model(
	AiBenchmarkSample *self
);

G_END_DECLS
