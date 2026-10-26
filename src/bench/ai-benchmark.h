/*
 * ai-benchmark.h - Measure and compare providers, models and features
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

#include <gio/gio.h>
#include "core/ai-config.h"
#include "bench/ai-benchmark-enums.h"
#include "bench/ai-benchmark-target.h"
#include "bench/ai-benchmark-case.h"
#include "bench/ai-benchmark-sample.h"
#include "bench/ai-benchmark-summary.h"

G_BEGIN_DECLS

/**
 * AI_BENCHMARK_MAX_ITERATIONS:
 *
 * The most measured iterations a benchmark accepts.
 */
#define AI_BENCHMARK_MAX_ITERATIONS (100000)

/**
 * AI_BENCHMARK_MAX_WARMUP:
 *
 * The most warmup calls per target a benchmark accepts.
 */
#define AI_BENCHMARK_MAX_WARMUP (1000)

#define AI_TYPE_BENCHMARK (ai_benchmark_get_type())
G_DECLARE_FINAL_TYPE(AiBenchmark, ai_benchmark, AI, BENCHMARK, GObject)

AiBenchmark *
ai_benchmark_new(
	void
);

AiBenchmark *
ai_benchmark_new_from_file(
	const gchar  *path,
	GError      **error
);

AiBenchmark *
ai_benchmark_new_from_data(
	const gchar  *data,
	gssize        length,
	const gchar  *base_dir,
	GError      **error
);

const gchar *
ai_benchmark_get_name(
	AiBenchmark *self
);

void
ai_benchmark_set_name(
	AiBenchmark *self,
	const gchar *name
);

guint
ai_benchmark_get_iterations(
	AiBenchmark *self
);

void
ai_benchmark_set_iterations(
	AiBenchmark *self,
	guint        iterations
);

guint
ai_benchmark_get_warmup(
	AiBenchmark *self
);

void
ai_benchmark_set_warmup(
	AiBenchmark *self,
	guint        warmup
);

guint
ai_benchmark_get_timeout_ms(
	AiBenchmark *self
);

void
ai_benchmark_set_timeout_ms(
	AiBenchmark *self,
	guint        timeout_ms
);

AiConfig *
ai_benchmark_get_config(
	AiBenchmark *self
);

void
ai_benchmark_set_config(
	AiBenchmark *self,
	AiConfig    *config
);

gboolean
ai_benchmark_add_target(
	AiBenchmark        *self,
	AiBenchmarkTarget  *target,
	GError            **error
);

gboolean
ai_benchmark_add_case(
	AiBenchmark      *self,
	AiBenchmarkCase  *bench_case,
	GError          **error
);

GPtrArray *
ai_benchmark_get_targets(
	AiBenchmark *self
);

GPtrArray *
ai_benchmark_get_cases(
	AiBenchmark *self
);

AiBenchmarkTarget *
ai_benchmark_find_target(
	AiBenchmark *self,
	const gchar *label
);

gboolean
ai_benchmark_select_targets(
	AiBenchmark         *self,
	const gchar *const  *labels,
	GError             **error
);

gboolean
ai_benchmark_prepare(
	AiBenchmark  *self,
	GError      **error
);

const gchar *
ai_benchmark_get_target_error(
	AiBenchmark *self,
	const gchar *label
);

guint
ai_benchmark_count_calls(
	AiBenchmark *self,
	guint       *measured
);

void
ai_benchmark_run_async(
	AiBenchmark         *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gboolean
ai_benchmark_run_finish(
	AiBenchmark   *self,
	GAsyncResult  *result,
	GError       **error
);

gboolean
ai_benchmark_run(
	AiBenchmark   *self,
	GCancellable  *cancellable,
	GError       **error
);

gboolean
ai_benchmark_is_running(
	AiBenchmark *self
);

GPtrArray *
ai_benchmark_get_samples(
	AiBenchmark *self
);

GList *
ai_benchmark_dup_summaries(
	AiBenchmark *self,
	gboolean     per_case
);

gchar *
ai_benchmark_dup_table(
	AiBenchmark *self,
	gboolean     per_case
);

gchar *
ai_benchmark_dup_yaml(
	AiBenchmark *self
);

gchar *
ai_benchmark_dup_json(
	AiBenchmark *self
);

gchar *
ai_benchmark_dup_csv(
	AiBenchmark *self
);

G_END_DECLS
