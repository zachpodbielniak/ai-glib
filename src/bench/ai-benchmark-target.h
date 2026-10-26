/*
 * ai-benchmark-target.h - One provider + model + settings to measure
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
#include "core/ai-config.h"
#include "bench/ai-benchmark-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_BENCHMARK_TARGET (ai_benchmark_target_get_type())
G_DECLARE_FINAL_TYPE(AiBenchmarkTarget, ai_benchmark_target, AI, BENCHMARK_TARGET, GObject)

AiBenchmarkTarget *
ai_benchmark_target_new(
	const gchar     *label,
	AiBenchmarkKind  kind,
	const gchar     *provider,
	const gchar     *model
);

AiBenchmarkTarget *
ai_benchmark_target_new_for_object(
	const gchar     *label,
	AiBenchmarkKind  kind,
	GObject         *object
);

const gchar *
ai_benchmark_target_get_label(
	AiBenchmarkTarget *self
);

AiBenchmarkKind
ai_benchmark_target_get_kind(
	AiBenchmarkTarget *self
);

const gchar *
ai_benchmark_target_get_provider(
	AiBenchmarkTarget *self
);

const gchar *
ai_benchmark_target_get_model(
	AiBenchmarkTarget *self
);

GObject *
ai_benchmark_target_get_object(
	AiBenchmarkTarget *self
);

gboolean
ai_benchmark_target_get_stream(
	AiBenchmarkTarget *self
);

void
ai_benchmark_target_set_stream(
	AiBenchmarkTarget *self,
	gboolean           stream
);

gboolean
ai_benchmark_target_add_property(
	AiBenchmarkTarget  *self,
	const gchar        *assignment,
	GError            **error
);

const gchar * const *
ai_benchmark_target_get_properties(
	AiBenchmarkTarget *self
);

gboolean
ai_benchmark_target_has_property(
	AiBenchmarkTarget *self,
	const gchar       *name
);

GObject *
ai_benchmark_target_build(
	AiBenchmarkTarget  *self,
	AiConfig           *config,
	GError            **error
);

G_END_DECLS
