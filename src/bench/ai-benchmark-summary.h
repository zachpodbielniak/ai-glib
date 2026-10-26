/*
 * ai-benchmark-summary.h - Aggregate statistics over benchmark samples
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
#include "bench/ai-benchmark-sample.h"

G_BEGIN_DECLS

#define AI_TYPE_BENCHMARK_SUMMARY (ai_benchmark_summary_get_type())
G_DECLARE_FINAL_TYPE(AiBenchmarkSummary, ai_benchmark_summary, AI, BENCHMARK_SUMMARY, GObject)

AiBenchmarkSummary *
ai_benchmark_summary_new(
	const gchar *target,
	const gchar *case_id,
	GPtrArray   *samples
);

const gchar *
ai_benchmark_summary_get_target(
	AiBenchmarkSummary *self
);

const gchar *
ai_benchmark_summary_get_case_id(
	AiBenchmarkSummary *self
);

guint
ai_benchmark_summary_get_runs(
	AiBenchmarkSummary *self
);

guint
ai_benchmark_summary_get_errors(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_error_rate(
	AiBenchmarkSummary *self
);

guint
ai_benchmark_summary_get_scored(
	AiBenchmarkSummary *self
);

guint
ai_benchmark_summary_get_passed(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_accuracy(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_min_ms(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_mean_ms(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_p50_ms(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_p95_ms(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_max_ms(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_mean_ttft_ms(
	AiBenchmarkSummary *self
);

gint64
ai_benchmark_summary_get_input_tokens(
	AiBenchmarkSummary *self
);

gint64
ai_benchmark_summary_get_output_tokens(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_output_tokens_per_second(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_input_tokens_per_second(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_mean_brier(
	AiBenchmarkSummary *self
);

gdouble
ai_benchmark_summary_get_mean_log_loss(
	AiBenchmarkSummary *self
);

G_END_DECLS
