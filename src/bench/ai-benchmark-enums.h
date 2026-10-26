/*
 * ai-benchmark-enums.h - What a benchmark measures and how a run ended
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

G_BEGIN_DECLS

/**
 * AiBenchmarkKind:
 * @AI_BENCHMARK_KIND_CHAT: a chat turn through #AiProvider, or
 *   #AiStreamable when the target streams
 * @AI_BENCHMARK_KIND_DECISION: a typed decision through #AiDecider
 * @AI_BENCHMARK_KIND_EMBEDDING: an embedding batch through #AiEmbedder
 * @AI_BENCHMARK_KIND_IMAGE: an image generation through #AiImageGenerator
 *
 * The kind of work a target performs and a case describes. A case runs
 * only against targets of its own kind.
 */
typedef enum
{
	AI_BENCHMARK_KIND_CHAT = 0,
	AI_BENCHMARK_KIND_DECISION,
	AI_BENCHMARK_KIND_EMBEDDING,
	AI_BENCHMARK_KIND_IMAGE
} AiBenchmarkKind;

GType
ai_benchmark_kind_get_type(
	void
) G_GNUC_CONST;
#define AI_TYPE_BENCHMARK_KIND (ai_benchmark_kind_get_type())

const gchar *
ai_benchmark_kind_to_string(
	AiBenchmarkKind kind
);

gboolean
ai_benchmark_kind_from_string(
	const gchar     *name,
	AiBenchmarkKind *kind
);

/**
 * AiBenchmarkOutcome:
 * @AI_BENCHMARK_OUTCOME_UNSCORED: the call succeeded and the case has no
 *   expectation to check it against
 * @AI_BENCHMARK_OUTCOME_PASS: the call succeeded and met every expectation
 * @AI_BENCHMARK_OUTCOME_FAIL: the call succeeded and missed an expectation
 * @AI_BENCHMARK_OUTCOME_ERROR: the call failed or timed out. An error is
 *   never a "no": it counts against the error rate, not the accuracy.
 *
 * How one measured run ended.
 */
typedef enum
{
	AI_BENCHMARK_OUTCOME_UNSCORED = 0,
	AI_BENCHMARK_OUTCOME_PASS,
	AI_BENCHMARK_OUTCOME_FAIL,
	AI_BENCHMARK_OUTCOME_ERROR
} AiBenchmarkOutcome;

GType
ai_benchmark_outcome_get_type(
	void
) G_GNUC_CONST;
#define AI_TYPE_BENCHMARK_OUTCOME (ai_benchmark_outcome_get_type())

const gchar *
ai_benchmark_outcome_to_string(
	AiBenchmarkOutcome outcome
);

G_END_DECLS
