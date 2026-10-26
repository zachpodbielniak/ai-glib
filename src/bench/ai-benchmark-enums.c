/*
 * ai-benchmark-enums.c - What a benchmark measures and how a run ended
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The nicks are the vocabulary of the suite file and of every output
 * format, so they are spelled once, here, and read back through
 * GEnumClass rather than through a second table.
 */

#include "bench/ai-benchmark-enums.h"

GType
ai_benchmark_kind_get_type(
	void
){
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ AI_BENCHMARK_KIND_CHAT, "AI_BENCHMARK_KIND_CHAT", "chat" },
			{ AI_BENCHMARK_KIND_DECISION, "AI_BENCHMARK_KIND_DECISION", "decision" },
			{ AI_BENCHMARK_KIND_EMBEDDING, "AI_BENCHMARK_KIND_EMBEDDING", "embedding" },
			{ AI_BENCHMARK_KIND_IMAGE, "AI_BENCHMARK_KIND_IMAGE", "image" },
			{ 0, NULL, NULL }
		};
		GType type;

		type = g_enum_register_static("AiBenchmarkKind", values);
		g_once_init_leave(&type_id, type);
	}
	return (GType)type_id;
}

/**
 * ai_benchmark_kind_to_string:
 * @kind: a kind
 *
 * Returns: (transfer none): the suite-file spelling, e.g. `decision`
 */
const gchar *
ai_benchmark_kind_to_string(
	AiBenchmarkKind kind
){
	g_autoptr(GEnumClass) klass = (GEnumClass *)g_type_class_ref(AI_TYPE_BENCHMARK_KIND);
	GEnumValue *value;

	value = g_enum_get_value(klass, (gint)kind);
	return value != NULL ? value->value_nick : "chat";
}

/**
 * ai_benchmark_kind_from_string:
 * @name: (nullable): `chat`, `decision`, `embedding` or `image`
 * @kind: (out): the parsed kind
 *
 * Returns: %TRUE when @name is one of the four spellings
 */
gboolean
ai_benchmark_kind_from_string(
	const gchar     *name,
	AiBenchmarkKind *kind
){
	g_autoptr(GEnumClass) klass = NULL;
	GEnumValue *value;

	g_return_val_if_fail(kind != NULL, FALSE);
	if (name == NULL)
		return FALSE;

	klass = (GEnumClass *)g_type_class_ref(AI_TYPE_BENCHMARK_KIND);
	value = g_enum_get_value_by_nick(klass, name);
	if (value == NULL)
		return FALSE;

	*kind = (AiBenchmarkKind)value->value;
	return TRUE;
}

GType
ai_benchmark_outcome_get_type(
	void
){
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		static const GEnumValue values[] = {
			{ AI_BENCHMARK_OUTCOME_UNSCORED, "AI_BENCHMARK_OUTCOME_UNSCORED", "unscored" },
			{ AI_BENCHMARK_OUTCOME_PASS, "AI_BENCHMARK_OUTCOME_PASS", "pass" },
			{ AI_BENCHMARK_OUTCOME_FAIL, "AI_BENCHMARK_OUTCOME_FAIL", "fail" },
			{ AI_BENCHMARK_OUTCOME_ERROR, "AI_BENCHMARK_OUTCOME_ERROR", "error" },
			{ 0, NULL, NULL }
		};
		GType type;

		type = g_enum_register_static("AiBenchmarkOutcome", values);
		g_once_init_leave(&type_id, type);
	}
	return (GType)type_id;
}

/**
 * ai_benchmark_outcome_to_string:
 * @outcome: an outcome
 *
 * Returns: (transfer none): `unscored`, `pass`, `fail` or `error`
 */
const gchar *
ai_benchmark_outcome_to_string(
	AiBenchmarkOutcome outcome
){
	g_autoptr(GEnumClass) klass = (GEnumClass *)g_type_class_ref(AI_TYPE_BENCHMARK_OUTCOME);
	GEnumValue *value;

	value = g_enum_get_value(klass, (gint)outcome);
	return value != NULL ? value->value_nick : "error";
}
