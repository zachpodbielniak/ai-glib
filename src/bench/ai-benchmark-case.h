/*
 * ai-benchmark-case.h - One workload and what a correct answer looks like
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
#include "core/ai-enums.h"
#include "model/ai-decision.h"
#include "model/ai-embedding.h"
#include "bench/ai-benchmark-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_BENCHMARK_CASE (ai_benchmark_case_get_type())
G_DECLARE_FINAL_TYPE(AiBenchmarkCase, ai_benchmark_case, AI, BENCHMARK_CASE, GObject)

AiBenchmarkCase *
ai_benchmark_case_new_chat(
	const gchar *id,
	const gchar *prompt
);

AiBenchmarkCase *
ai_benchmark_case_new_decision(
	const gchar       *id,
	AiDecisionRequest *request
);

AiBenchmarkCase *
ai_benchmark_case_new_embedding(
	const gchar        *id,
	const gchar *const *texts
);

AiBenchmarkCase *
ai_benchmark_case_new_image(
	const gchar *id,
	const gchar *prompt
);

const gchar *
ai_benchmark_case_get_id(
	AiBenchmarkCase *self
);

AiBenchmarkKind
ai_benchmark_case_get_kind(
	AiBenchmarkCase *self
);

const gchar *
ai_benchmark_case_get_prompt(
	AiBenchmarkCase *self
);

AiDecisionRequest *
ai_benchmark_case_get_request(
	AiBenchmarkCase *self
);

const gchar * const *
ai_benchmark_case_get_texts(
	AiBenchmarkCase *self
);

/* chat */

void
ai_benchmark_case_set_system(
	AiBenchmarkCase *self,
	const gchar     *system_prompt
);

const gchar *
ai_benchmark_case_get_system(
	AiBenchmarkCase *self
);

void
ai_benchmark_case_set_max_tokens(
	AiBenchmarkCase *self,
	gint             max_tokens
);

gint
ai_benchmark_case_get_max_tokens(
	AiBenchmarkCase *self
);

void
ai_benchmark_case_expect_contains(
	AiBenchmarkCase *self,
	const gchar     *text
);

void
ai_benchmark_case_expect_equals(
	AiBenchmarkCase *self,
	const gchar     *text
);

gboolean
ai_benchmark_case_expect_regex(
	AiBenchmarkCase  *self,
	const gchar      *pattern,
	GError          **error
);

/* decision */

gboolean
ai_benchmark_case_expect_answer(
	AiBenchmarkCase  *self,
	const gchar      *question_id,
	const gchar      *expected,
	GError          **error
);

void
ai_benchmark_case_set_noul_threshold(
	AiBenchmarkCase *self,
	gdouble          threshold
);

gdouble
ai_benchmark_case_get_noul_threshold(
	AiBenchmarkCase *self
);

void
ai_benchmark_case_set_box_iou(
	AiBenchmarkCase *self,
	gdouble          iou
);

/* embedding */

gboolean
ai_benchmark_case_expect_similarity(
	AiBenchmarkCase  *self,
	guint             similar_a,
	guint             similar_b,
	guint             dissimilar_a,
	guint             dissimilar_b,
	GError          **error
);

/* image */

void
ai_benchmark_case_set_image_size(
	AiBenchmarkCase *self,
	AiImageSize      size
);

AiImageSize
ai_benchmark_case_get_image_size(
	AiBenchmarkCase *self
);

void
ai_benchmark_case_set_image_count(
	AiBenchmarkCase *self,
	gint             count
);

gint
ai_benchmark_case_get_image_count(
	AiBenchmarkCase *self
);

/* scoring */

gboolean
ai_benchmark_case_has_expectation(
	AiBenchmarkCase *self
);

AiBenchmarkOutcome
ai_benchmark_case_score_text(
	AiBenchmarkCase  *self,
	const gchar      *text,
	gchar           **detail
);

AiBenchmarkOutcome
ai_benchmark_case_score_decision(
	AiBenchmarkCase     *self,
	AiDecisionResponse  *response,
	gdouble             *brier,
	gdouble             *log_loss,
	gchar              **detail
);

AiBenchmarkOutcome
ai_benchmark_case_score_embedding(
	AiBenchmarkCase  *self,
	AiEmbedding      *embedding,
	gchar           **detail
);

G_END_DECLS
