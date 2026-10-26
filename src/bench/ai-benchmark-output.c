/*
 * ai-benchmark-output.c - Table, YAML, JSON and CSV renderings of a run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The four formats live in the library, not in `ai`, so ai-tui, ai-gui and
 * an Emacs frontend print exactly what the CLI prints.
 *
 * Absent values (-1 in the API) are rendered as absent, never as a
 * number: `-` in the table, null in YAML and JSON, an empty field in CSV.
 * Printing 0 for "no latency" or "no tokens reported" reads as a
 * measurement, which it is not.
 *
 * Shapes:
 *
 *   table  one row per target (or per target and case), aligned columns
 *   yaml   name, settings, targets (with build errors), summaries,
 *          per-case summaries, samples
 *   json   the same document as yaml
 *   csv    one row per sample, RFC 4180 quoting, `,` separator
 */

#include "bench/ai-benchmark.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>
#include <yaml-glib.h>

/* ---- shared document -------------------------------------------- */

static void
add_number_or_null(
	JsonBuilder *builder,
	const gchar *member,
	gdouble      value,
	gdouble      scale
){
	json_builder_set_member_name(builder, member);
	if (value < 0 || !isfinite(value))
		json_builder_add_null_value(builder);
	else
		json_builder_add_double_value(builder, round(value * scale) / scale);
}

static void
add_int_or_null(
	JsonBuilder *builder,
	const gchar *member,
	gint64       value
){
	json_builder_set_member_name(builder, member);
	if (value < 0)
		json_builder_add_null_value(builder);
	else
		json_builder_add_int_value(builder, value);
}

static void
add_string_or_null(
	JsonBuilder *builder,
	const gchar *member,
	const gchar *value
){
	json_builder_set_member_name(builder, member);
	if (value == NULL)
		json_builder_add_null_value(builder);
	else
		json_builder_add_string_value(builder, value);
}

static void
add_summary(
	JsonBuilder        *builder,
	AiBenchmarkSummary *summary
){
	json_builder_begin_object(builder);
	add_string_or_null(builder, "target", ai_benchmark_summary_get_target(summary));
	if (ai_benchmark_summary_get_case_id(summary) != NULL)
		add_string_or_null(builder, "case", ai_benchmark_summary_get_case_id(summary));
	json_builder_set_member_name(builder, "runs");
	json_builder_add_int_value(builder, ai_benchmark_summary_get_runs(summary));
	json_builder_set_member_name(builder, "errors");
	json_builder_add_int_value(builder, ai_benchmark_summary_get_errors(summary));
	add_number_or_null(builder, "error_rate", ai_benchmark_summary_get_error_rate(summary), 10000);
	json_builder_set_member_name(builder, "scored");
	json_builder_add_int_value(builder, ai_benchmark_summary_get_scored(summary));
	json_builder_set_member_name(builder, "passed");
	json_builder_add_int_value(builder, ai_benchmark_summary_get_passed(summary));
	add_number_or_null(builder, "accuracy", ai_benchmark_summary_get_accuracy(summary), 10000);
	add_number_or_null(builder, "min_ms", ai_benchmark_summary_get_min_ms(summary), 1000);
	add_number_or_null(builder, "mean_ms", ai_benchmark_summary_get_mean_ms(summary), 1000);
	add_number_or_null(builder, "p50_ms", ai_benchmark_summary_get_p50_ms(summary), 1000);
	add_number_or_null(builder, "p95_ms", ai_benchmark_summary_get_p95_ms(summary), 1000);
	add_number_or_null(builder, "max_ms", ai_benchmark_summary_get_max_ms(summary), 1000);
	add_number_or_null(builder, "mean_ttft_ms", ai_benchmark_summary_get_mean_ttft_ms(summary), 1000);
	add_int_or_null(builder, "input_tokens", ai_benchmark_summary_get_input_tokens(summary));
	add_int_or_null(builder, "output_tokens", ai_benchmark_summary_get_output_tokens(summary));
	add_number_or_null(builder, "input_tokens_per_second",
	                   ai_benchmark_summary_get_input_tokens_per_second(summary), 100);
	add_number_or_null(builder, "output_tokens_per_second",
	                   ai_benchmark_summary_get_output_tokens_per_second(summary), 100);
	add_number_or_null(builder, "mean_brier", ai_benchmark_summary_get_mean_brier(summary), 10000);
	add_number_or_null(builder, "mean_log_loss", ai_benchmark_summary_get_mean_log_loss(summary), 10000);
	json_builder_end_object(builder);
}

/*
 * The one document YAML and JSON both render, so they cannot disagree.
 */
static JsonNode *
build_document(
	AiBenchmark *self
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autolist(AiBenchmarkSummary) totals = ai_benchmark_dup_summaries(self, FALSE);
	g_autolist(AiBenchmarkSummary) per_case = ai_benchmark_dup_summaries(self, TRUE);
	GList *iter;
	GPtrArray *targets = ai_benchmark_get_targets(self);
	GPtrArray *samples = ai_benchmark_get_samples(self);
	guint i;

	json_builder_begin_object(builder);
	add_string_or_null(builder, "name", ai_benchmark_get_name(self));
	json_builder_set_member_name(builder, "iterations");
	json_builder_add_int_value(builder, ai_benchmark_get_iterations(self));
	json_builder_set_member_name(builder, "warmup");
	json_builder_add_int_value(builder, ai_benchmark_get_warmup(self));
	json_builder_set_member_name(builder, "timeout_ms");
	json_builder_add_int_value(builder, ai_benchmark_get_timeout_ms(self));

	/* targets: what was asked for, and why any of them did not run */
	json_builder_set_member_name(builder, "targets");
	json_builder_begin_array(builder);
	for (i = 0; i < targets->len; i++)
	{
		AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(targets, i);
		const gchar *label = ai_benchmark_target_get_label(target);

		json_builder_begin_object(builder);
		add_string_or_null(builder, "label", label);
		add_string_or_null(builder, "kind", ai_benchmark_kind_to_string(ai_benchmark_target_get_kind(target)));
		add_string_or_null(builder, "provider", ai_benchmark_target_get_provider(target));
		add_string_or_null(builder, "model", ai_benchmark_target_get_model(target));
		json_builder_set_member_name(builder, "stream");
		json_builder_add_boolean_value(builder, ai_benchmark_target_get_stream(target));
		add_string_or_null(builder, "error", ai_benchmark_get_target_error(self, label));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "summaries");
	json_builder_begin_array(builder);
	for (iter = totals; iter != NULL; iter = iter->next)
		add_summary(builder, (AiBenchmarkSummary *)iter->data);
	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "cases");
	json_builder_begin_array(builder);
	for (iter = per_case; iter != NULL; iter = iter->next)
		add_summary(builder, (AiBenchmarkSummary *)iter->data);
	json_builder_end_array(builder);

	/* samples: every measured run, for anyone who wants their own stats */
	json_builder_set_member_name(builder, "samples");
	json_builder_begin_array(builder);
	for (i = 0; i < samples->len; i++)
	{
		AiBenchmarkSample *sample = (AiBenchmarkSample *)g_ptr_array_index(samples, i);

		json_builder_begin_object(builder);
		add_string_or_null(builder, "target", ai_benchmark_sample_get_target(sample));
		add_string_or_null(builder, "case", ai_benchmark_sample_get_case_id(sample));
		json_builder_set_member_name(builder, "iteration");
		json_builder_add_int_value(builder, ai_benchmark_sample_get_iteration(sample));
		add_string_or_null(builder, "outcome",
		                   ai_benchmark_outcome_to_string(ai_benchmark_sample_get_outcome(sample)));
		add_number_or_null(builder, "elapsed_ms",
		                   (gdouble)ai_benchmark_sample_get_elapsed_us(sample) / 1000.0, 1000);
		add_number_or_null(builder, "ttft_ms",
		                   ai_benchmark_sample_get_ttft_us(sample) < 0 ? -1
		                   : (gdouble)ai_benchmark_sample_get_ttft_us(sample) / 1000.0, 1000);
		add_int_or_null(builder, "input_tokens", ai_benchmark_sample_get_input_tokens(sample));
		add_int_or_null(builder, "output_tokens", ai_benchmark_sample_get_output_tokens(sample));
		add_number_or_null(builder, "brier", ai_benchmark_sample_get_brier(sample), 10000);
		add_number_or_null(builder, "log_loss", ai_benchmark_sample_get_log_loss(sample), 10000);
		add_string_or_null(builder, "model", ai_benchmark_sample_get_model(sample));
		add_string_or_null(builder, "detail", ai_benchmark_sample_get_detail(sample));
		add_string_or_null(builder, "error", ai_benchmark_sample_get_error(sample));
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);

	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/**
 * ai_benchmark_dup_json:
 * @self: a benchmark
 *
 * The full result as pretty-printed JSON: settings, targets (with any
 * build error), per-target summaries, per-case summaries and every
 * sample. Absent values are `null`. See docs/benchmark.org for the schema.
 *
 * Returns: (transfer full): JSON text
 */
gchar *
ai_benchmark_dup_json(
	AiBenchmark *self
){
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(JsonGenerator) generator = NULL;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);

	root = build_document(self);
	generator = json_generator_new();
	json_generator_set_pretty(generator, TRUE);
	json_generator_set_indent(generator, 2);
	json_generator_set_root(generator, root);
	return json_generator_to_data(generator, NULL);
}

/*
 * yaml_node_from_json_node() leaves every scalar's style to the emitter,
 * which writes a label or model such as `no`, `42` or `null` unquoted, and
 * a reader then gets a boolean, a number or nothing. Walk the two trees
 * together and double-quote every node that was a JSON string.
 */
static void
quote_strings(
	JsonNode *json,
	YamlNode *yaml
){
	if (json == NULL || yaml == NULL)
		return;

	if (JSON_NODE_HOLDS_OBJECT(json) && yaml_node_get_node_type(yaml) == YAML_NODE_MAPPING)
	{
		JsonObject *object = json_node_get_object(json);
		YamlMapping *mapping = yaml_node_get_mapping(yaml);
		g_autoptr(GList) names = json_object_get_members(object);
		GList *iter;

		for (iter = names; iter != NULL; iter = iter->next)
			quote_strings(json_object_get_member(object, (const gchar *)iter->data),
			              yaml_mapping_get_member(mapping, (const gchar *)iter->data));
	}
	else if (JSON_NODE_HOLDS_ARRAY(json) && yaml_node_get_node_type(yaml) == YAML_NODE_SEQUENCE)
	{
		JsonArray *array = json_node_get_array(json);
		YamlSequence *sequence = yaml_node_get_sequence(yaml);
		guint length = MIN(json_array_get_length(array), yaml_sequence_get_length(sequence));
		guint i;

		for (i = 0; i < length; i++)
			quote_strings(json_array_get_element(array, i), yaml_sequence_get_element(sequence, i));
	}
	else if (JSON_NODE_HOLDS_VALUE(json) && json_node_get_value_type(json) == G_TYPE_STRING &&
	         yaml_node_get_node_type(yaml) == YAML_NODE_SCALAR)
		yaml_node_set_scalar_style(yaml, YAML_SCALAR_STYLE_DOUBLE_QUOTED);
}

/**
 * ai_benchmark_dup_yaml:
 * @self: a benchmark
 *
 * The same document as ai_benchmark_dup_json(), as YAML. Every string is
 * double-quoted, so a label or model called `no` or `42` reads back as a
 * string.
 *
 * Returns: (transfer full): YAML text
 */
gchar *
ai_benchmark_dup_yaml(
	AiBenchmark *self
){
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(YamlNode) yaml = NULL;
	g_autoptr(YamlGenerator) generator = NULL;
	g_autoptr(GError) error = NULL;
	gchar *text;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);

	root = build_document(self);
	yaml = yaml_node_from_json_node(root);
	quote_strings(root, yaml);
	generator = yaml_generator_new();
	yaml_generator_set_root(generator, yaml);
	text = yaml_generator_to_data(generator, NULL, &error);
	if (text == NULL)
	{
		/* Not reachable with a document we built; keep the contract anyway. */
		g_debug("benchmark: YAML emit failed: %s", error != NULL ? error->message : "unknown");
		return g_strdup("");
	}
	return text;
}

/* ---- CSV -------------------------------------------------------- */

/*
 * RFC 4180: a field containing a comma, a quote, CR or LF is quoted, and
 * a quote inside it is doubled. Anything else is written as is.
 */
static void
csv_field(
	GString     *out,
	const gchar *value
){
	const gchar *p;

	if (value == NULL)
		return;
	if (strpbrk(value, ",\"\r\n") == NULL)
	{
		g_string_append(out, value);
		return;
	}
	g_string_append_c(out, '"');
	for (p = value; *p != '\0'; p++)
	{
		if (*p == '"')
			g_string_append_c(out, '"');
		g_string_append_c(out, *p);
	}
	g_string_append_c(out, '"');
}

static void
csv_number(
	GString     *out,
	gdouble      value,
	const gchar *format
){
	gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

	if (value < 0 || !isfinite(value))
		return;
	g_string_append(out, g_ascii_formatd(buffer, sizeof(buffer), format, value));
}

static void
csv_int(
	GString *out,
	gint64   value
){
	if (value >= 0)
		g_string_append_printf(out, "%" G_GINT64_FORMAT, value);
}

/**
 * ai_benchmark_dup_csv:
 * @self: a benchmark
 *
 * One row per measured run, with a header row:
 * `target,case,iteration,outcome,elapsed_ms,ttft_ms,input_tokens,output_tokens,brier,log_loss,model,detail,error`.
 * Absent values are empty fields; numbers use `.` whatever the locale.
 *
 * Returns: (transfer full): CSV text
 */
gchar *
ai_benchmark_dup_csv(
	AiBenchmark *self
){
	g_autoptr(GString) out = NULL;
	GPtrArray *samples;
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);

	out = g_string_new("target,case,iteration,outcome,elapsed_ms,ttft_ms,"
	                   "input_tokens,output_tokens,brier,log_loss,model,detail,error\n");
	samples = ai_benchmark_get_samples(self);
	for (i = 0; i < samples->len; i++)
	{
		AiBenchmarkSample *sample = (AiBenchmarkSample *)g_ptr_array_index(samples, i);
		gint64 ttft = ai_benchmark_sample_get_ttft_us(sample);

		csv_field(out, ai_benchmark_sample_get_target(sample));
		g_string_append_c(out, ',');
		csv_field(out, ai_benchmark_sample_get_case_id(sample));
		g_string_append_printf(out, ",%u,", ai_benchmark_sample_get_iteration(sample));
		g_string_append(out, ai_benchmark_outcome_to_string(ai_benchmark_sample_get_outcome(sample)));
		g_string_append_c(out, ',');
		csv_number(out, (gdouble)ai_benchmark_sample_get_elapsed_us(sample) / 1000.0, "%.3f");
		g_string_append_c(out, ',');
		csv_number(out, ttft < 0 ? -1 : (gdouble)ttft / 1000.0, "%.3f");
		g_string_append_c(out, ',');
		csv_int(out, ai_benchmark_sample_get_input_tokens(sample));
		g_string_append_c(out, ',');
		csv_int(out, ai_benchmark_sample_get_output_tokens(sample));
		g_string_append_c(out, ',');
		csv_number(out, ai_benchmark_sample_get_brier(sample), "%.4f");
		g_string_append_c(out, ',');
		csv_number(out, ai_benchmark_sample_get_log_loss(sample), "%.4f");
		g_string_append_c(out, ',');
		csv_field(out, ai_benchmark_sample_get_model(sample));
		g_string_append_c(out, ',');
		csv_field(out, ai_benchmark_sample_get_detail(sample));
		g_string_append_c(out, ',');
		csv_field(out, ai_benchmark_sample_get_error(sample));
		g_string_append_c(out, '\n');
	}
	return g_string_free((GString *)g_steal_pointer(&out), FALSE);
}

/* ---- table ------------------------------------------------------ */

#define N_COLUMNS (12)

static gchar *
cell_ms(
	gdouble value
){
	return value < 0 ? g_strdup("-") : g_strdup_printf("%.1f", value);
}

static gchar *
cell_ratio(
	gdouble value,
	gboolean percent
){
	if (value < 0)
		return g_strdup("-");
	return percent ? g_strdup_printf("%.1f%%", value * 100.0) : g_strdup_printf("%.4f", value);
}

static gchar *
cell_tokens(
	gint64 in,
	gint64 out
){
	if (in < 0 && out < 0)
		return g_strdup("-");
	if (in < 0)
		return g_strdup_printf("-/%" G_GINT64_FORMAT, out);
	if (out < 0)
		return g_strdup_printf("%" G_GINT64_FORMAT "/-", in);
	return g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, in, out);
}

/* Width in terminal columns, so a UTF-8 label does not skew alignment. */
static guint
display_width(
	const gchar *text
){
	guint width;
	const gchar *p;

	width = 0;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p))
		width += g_unichar_iswide(g_utf8_get_char(p)) ? 2 : 1;
	return width;
}

/**
 * ai_benchmark_dup_table:
 * @self: a benchmark
 * @per_case: %TRUE for a row per (target, case), %FALSE for a row per target
 *
 * An aligned plain-text table for a terminal. Columns: target (and case),
 * runs, errors, accuracy, p50, p95, mean, TTFT (all milliseconds),
 * tokens in/out, input and output tokens per second, Brier. `-` means not
 * available. Targets that could not be built are listed below the table
 * with the reason.
 *
 * Returns: (transfer full): the table text, ending in a newline
 */
gchar *
ai_benchmark_dup_table(
	AiBenchmark *self,
	gboolean     per_case
){
	static const gchar *const headers[N_COLUMNS] = {
		"target", "runs", "err", "accuracy", "p50 ms", "p95 ms",
		"mean ms", "ttft ms", "tokens in/out", "in tok/s", "out tok/s", "brier"
	};
	g_autolist(AiBenchmarkSummary) summaries = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GList *iter;
	g_autoptr(GString) out = NULL;
	GPtrArray *targets;
	guint widths[N_COLUMNS];
	guint i;
	guint col;

	g_return_val_if_fail(AI_IS_BENCHMARK(self), NULL);

	summaries = ai_benchmark_dup_summaries(self, per_case);
	rows = g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
	for (col = 0; col < N_COLUMNS; col++)
		widths[col] = display_width(headers[col]);

	/* 1. Render every cell, tracking each column's width. */
	for (iter = summaries; iter != NULL; iter = iter->next)
	{
		AiBenchmarkSummary *s = (AiBenchmarkSummary *)iter->data;
		gchar **row = g_new0(gchar *, N_COLUMNS + 1);

		row[0] = ai_benchmark_summary_get_case_id(s) != NULL
			? g_strdup_printf("%s / %s", ai_benchmark_summary_get_target(s), ai_benchmark_summary_get_case_id(s))
			: g_strdup(ai_benchmark_summary_get_target(s));
		row[1] = g_strdup_printf("%u", ai_benchmark_summary_get_runs(s));
		row[2] = g_strdup_printf("%u", ai_benchmark_summary_get_errors(s));
		row[3] = cell_ratio(ai_benchmark_summary_get_accuracy(s), TRUE);
		row[4] = cell_ms(ai_benchmark_summary_get_p50_ms(s));
		row[5] = cell_ms(ai_benchmark_summary_get_p95_ms(s));
		row[6] = cell_ms(ai_benchmark_summary_get_mean_ms(s));
		row[7] = cell_ms(ai_benchmark_summary_get_mean_ttft_ms(s));
		row[8] = cell_tokens(ai_benchmark_summary_get_input_tokens(s),
		                     ai_benchmark_summary_get_output_tokens(s));
		row[9] = cell_ms(ai_benchmark_summary_get_input_tokens_per_second(s));
		row[10] = cell_ms(ai_benchmark_summary_get_output_tokens_per_second(s));
		row[11] = cell_ratio(ai_benchmark_summary_get_mean_brier(s), FALSE);
		for (col = 0; col < N_COLUMNS; col++)
			widths[col] = MAX(widths[col], display_width(row[col]));
		g_ptr_array_add(rows, row);
	}

	/* 2. Header, rule, rows. The label column is left-aligned, numbers right. */
	out = g_string_new(NULL);
	if (ai_benchmark_get_name(self) != NULL)
		g_string_append_printf(out, "%s\n\n", ai_benchmark_get_name(self));

	for (i = 0; i <= rows->len + 1; i++)
	{
		gchar **row = i == 0 ? (gchar **)headers : i == 1 ? NULL : (gchar **)g_ptr_array_index(rows, i - 2);

		for (col = 0; col < N_COLUMNS; col++)
		{
			guint pad;
			guint k;

			if (col > 0)
				g_string_append(out, "  ");
			if (row == NULL)
			{
				for (k = 0; k < widths[col]; k++)
					g_string_append_c(out, '-');
				continue;
			}
			pad = widths[col] - display_width(row[col]);
			if (col == 0)
			{
				g_string_append(out, row[col]);
				if (col + 1 < N_COLUMNS)
					for (k = 0; k < pad; k++)
						g_string_append_c(out, ' ');
			}
			else
			{
				for (k = 0; k < pad; k++)
					g_string_append_c(out, ' ');
				g_string_append(out, row[col]);
			}
		}
		g_string_append_c(out, '\n');
	}

	/* 3. Targets that never ran, with the reason. */
	targets = ai_benchmark_get_targets(self);
	for (i = 0; i < targets->len; i++)
	{
		const gchar *label = ai_benchmark_target_get_label((AiBenchmarkTarget *)g_ptr_array_index(targets, i));
		const gchar *reason = ai_benchmark_get_target_error(self, label);

		if (reason != NULL)
			g_string_append_printf(out, "skipped %s: %s\n", label, reason);
	}
	return g_string_free((GString *)g_steal_pointer(&out), FALSE);
}
