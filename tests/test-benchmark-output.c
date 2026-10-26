/*
 * test-benchmark-output.c - Table, YAML, JSON and CSV renderings
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The four formats are the benchmark's interface to people and to other
 * programs, so they are asserted precisely: the table literally, YAML and
 * JSON by parsing them back, CSV by its header and its quoting.
 *
 * The samples are fed in directly rather than measured, so every number
 * is known. ai_benchmark_get_samples() hands out the benchmark's own
 * array; a frontend replaying a saved run could do the same.
 *
 * One rule runs through all four: an absent value is rendered as absent
 * (`-`, null, an empty field), never as 0 and never as `nan`.
 */

#include <ai-glib.h>
#include <string.h>
#include <yaml-glib.h>

static AiBenchmark *
fixture(
	void
){
	AiBenchmark *bench;
	g_autoptr(AiMockDecider) jev_decider = ai_mock_decider_new("{}");
	g_autoptr(AiMockDecider) tev1_decider = ai_mock_decider_new("{}");
	g_autoptr(AiBenchmarkTarget) jev = NULL;
	g_autoptr(AiBenchmarkTarget) tev1 = NULL;
	g_autoptr(AiBenchmarkTarget) broken = NULL;
	g_autoptr(AiDecisionRequest) request = ai_decision_request_new("text");
	g_autoptr(AiBenchmarkCase) bench_case = NULL;
	GPtrArray *samples;
	AiBenchmarkSample *sample;

	bench = ai_benchmark_new();
	ai_benchmark_set_name(bench, "demo");
	ai_benchmark_set_iterations(bench, 2);
	jev = ai_benchmark_target_new_for_object("jev", AI_BENCHMARK_KIND_DECISION, G_OBJECT(jev_decider));
	tev1 = ai_benchmark_target_new_for_object("tev1", AI_BENCHMARK_KIND_DECISION, G_OBJECT(tev1_decider));
	broken = ai_benchmark_target_new("claude-embed", AI_BENCHMARK_KIND_EMBEDDING, "claude", NULL);
	g_assert_true(ai_benchmark_add_target(bench, jev, NULL));
	g_assert_true(ai_benchmark_add_target(bench, tev1, NULL));
	g_assert_true(ai_benchmark_add_target(bench, broken, NULL));
	g_assert_true(ai_decision_request_add_boolean(request, "q", "Q?", NULL));
	bench_case = ai_benchmark_case_new_decision("refund", request);
	g_assert_true(ai_benchmark_add_case(bench, bench_case, NULL));
	g_assert_true(ai_benchmark_prepare(bench, NULL));

	samples = ai_benchmark_get_samples(bench);

	sample = ai_benchmark_sample_new("jev", "refund", 0, 100000);
	ai_benchmark_sample_set_tokens(sample, 40, 2);
	ai_benchmark_sample_set_model(sample, "jev-1.14.0");
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_PASS, 0.01, 0.1053605157, NULL);
	g_ptr_array_add(samples, sample);

	sample = ai_benchmark_sample_new("tev1", "refund", 0, 50000);
	ai_benchmark_sample_set_model(sample, "tev1");
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_FAIL, 0.36, 0.9162907319,
	                                "q: expected true, P(true) was 0.4000");
	g_ptr_array_add(samples, sample);

	sample = ai_benchmark_sample_new("jev", "refund", 1, 120000);
	ai_benchmark_sample_set_tokens(sample, 40, 2);
	ai_benchmark_sample_set_model(sample, "jev-1.14.0");
	ai_benchmark_sample_set_outcome(sample, AI_BENCHMARK_OUTCOME_PASS, 0.01, 0.1053605157, NULL);
	g_ptr_array_add(samples, sample);

	sample = ai_benchmark_sample_new("tev1", "refund", 1, 30000000);
	ai_benchmark_sample_set_error(sample, "bad \"gateway\", retry\nlater");
	g_ptr_array_add(samples, sample);

	return bench;
}

/*
 * The table, literally. Accuracy is over scored runs only (tev1: 0 of 1,
 * its error excluded), latency over successes only, and the target that
 * never built is listed with its reason.
 */
static void
test_table(
	void
){
	g_autoptr(AiBenchmark) bench = fixture();
	g_autofree gchar *table = ai_benchmark_dup_table(bench, FALSE);
	static const gchar *expected =
		"demo\n"
		"\n"
		"target        runs  err  accuracy  p50 ms  p95 ms  mean ms  ttft ms  tokens in/out  in tok/s  out tok/s   brier\n"
		"------------  ----  ---  --------  ------  ------  -------  -------  -------------  --------  ---------  ------\n"
		"jev              2    0    100.0%   100.0   120.0    110.0        -           80/4     363.6       18.2  0.0100\n"
		"tev1             2    1      0.0%    50.0    50.0     50.0        -              -         -          -  0.3600\n"
		"claude-embed     0    0         -       -       -        -        -              -         -          -       -\n"
		"skipped claude-embed: target 'claude-embed': provider 'claude' cannot run embedding "
		"(it does not implement AiEmbedder)\n";

	g_assert_cmpstr(table, ==, expected);
}

/* Per case: the label column reads "target / case"; only a target's own kind's cases. */
static void
test_table_per_case(
	void
){
	g_autoptr(AiBenchmark) bench = fixture();
	g_autofree gchar *table = ai_benchmark_dup_table(bench, TRUE);
	g_auto(GStrv) lines = g_strsplit(table, "\n", -1);

	g_assert_true(g_str_has_prefix(lines[2], "target         "));
	g_assert_true(g_str_has_prefix(lines[4], "jev / refund "));
	g_assert_true(g_str_has_prefix(lines[5], "tev1 / refund "));
	/* claude-embed has no embedding case, so no per-case row. */
	g_assert_true(g_str_has_prefix(lines[6], "skipped claude-embed"));
}

/* A UTF-8 label is aligned by terminal columns, not bytes. */
static void
test_table_unicode_alignment(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) da = ai_mock_decider_new("{}");
	g_autoptr(AiMockDecider) db = ai_mock_decider_new("{}");
	g_autoptr(AiBenchmarkTarget) a = ai_benchmark_target_new_for_object("café", AI_BENCHMARK_KIND_DECISION, G_OBJECT(da));
	g_autoptr(AiBenchmarkTarget) b = ai_benchmark_target_new_for_object("tea", AI_BENCHMARK_KIND_DECISION, G_OBJECT(db));
	g_autofree gchar *table = NULL;
	g_auto(GStrv) lines = NULL;

	g_assert_true(ai_benchmark_add_target(bench, a, NULL));
	g_assert_true(ai_benchmark_add_target(bench, b, NULL));
	table = ai_benchmark_dup_table(bench, FALSE);
	lines = g_strsplit(table, "\n", -1);
	/* header, rule, café, tea: every line is the same width in columns */
	g_assert_cmpuint(g_utf8_strlen(lines[0], -1), ==, g_utf8_strlen(lines[2], -1));
	g_assert_cmpuint(g_utf8_strlen(lines[2], -1), ==, g_utf8_strlen(lines[3], -1));
}

static JsonObject *
array_object(
	JsonObject  *root,
	const gchar *member,
	guint        index
){
	JsonArray *array = json_object_get_array_member(root, member);

	g_assert_nonnull(array);
	g_assert_cmpuint(index, <, json_array_get_length(array));
	return json_array_get_object_element(array, index);
}

static void
check_document(
	JsonObject *root
){
	JsonObject *o;

	g_assert_cmpstr(json_object_get_string_member(root, "name"), ==, "demo");
	g_assert_cmpint(json_object_get_int_member(root, "iterations"), ==, 2);

	/* targets: the broken one carries its reason, the others null */
	o = array_object(root, "targets", 2);
	g_assert_cmpstr(json_object_get_string_member(o, "label"), ==, "claude-embed");
	g_assert_cmpstr(json_object_get_string_member(o, "kind"), ==, "embedding");
	g_assert_true(g_str_has_prefix(json_object_get_string_member(o, "error"), "target 'claude-embed'"));
	o = array_object(root, "targets", 0);
	g_assert_true(json_object_get_null_member(o, "error"));
	g_assert_true(json_object_get_null_member(o, "model"));

	/* summaries */
	o = array_object(root, "summaries", 0);
	g_assert_cmpstr(json_object_get_string_member(o, "target"), ==, "jev");
	g_assert_false(json_object_has_member(o, "case"));
	g_assert_cmpint(json_object_get_int_member(o, "runs"), ==, 2);
	g_assert_cmpfloat(json_object_get_double_member(o, "accuracy"), ==, 1.0);
	g_assert_cmpfloat(json_object_get_double_member(o, "p50_ms"), ==, 100.0);
	g_assert_cmpfloat(json_object_get_double_member(o, "p95_ms"), ==, 120.0);
	g_assert_cmpint(json_object_get_int_member(o, "input_tokens"), ==, 80);
	g_assert_cmpfloat(json_object_get_double_member(o, "mean_brier"), ==, 0.01);
	g_assert_true(json_object_get_null_member(o, "mean_ttft_ms"));
	o = array_object(root, "summaries", 1);
	g_assert_cmpint(json_object_get_int_member(o, "errors"), ==, 1);
	g_assert_cmpfloat(json_object_get_double_member(o, "error_rate"), ==, 0.5);
	g_assert_true(json_object_get_null_member(o, "input_tokens"));
	o = array_object(root, "summaries", 2);
	g_assert_cmpint(json_object_get_int_member(o, "runs"), ==, 0);
	g_assert_true(json_object_get_null_member(o, "accuracy"));
	g_assert_true(json_object_get_null_member(o, "p50_ms"));

	/* per-case summaries */
	o = array_object(root, "cases", 0);
	g_assert_cmpstr(json_object_get_string_member(o, "case"), ==, "refund");

	/* samples */
	o = array_object(root, "samples", 3);
	g_assert_cmpstr(json_object_get_string_member(o, "outcome"), ==, "error");
	g_assert_cmpstr(json_object_get_string_member(o, "error"), ==, "bad \"gateway\", retry\nlater");
	g_assert_true(json_object_get_null_member(o, "brier"));
	g_assert_true(json_object_get_null_member(o, "ttft_ms"));
	o = array_object(root, "samples", 1);
	g_assert_cmpstr(json_object_get_string_member(o, "detail"), ==, "q: expected true, P(true) was 0.4000");
	g_assert_cmpfloat(json_object_get_double_member(o, "elapsed_ms"), ==, 50.0);
}

static void
test_json(
	void
){
	g_autoptr(AiBenchmark) bench = fixture();
	g_autofree gchar *json = ai_benchmark_dup_json(bench);
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	check_document(json_node_get_object(json_parser_get_root(parser)));
	g_assert_null(strstr(json, "nan"));
}

/* YAML is the same document: parse it back and run the same checks. */
static void
test_yaml(
	void
){
	g_autoptr(AiBenchmark) bench = fixture();
	g_autofree gchar *yaml = ai_benchmark_dup_yaml(bench);
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(yaml_parser_load_from_data(parser, yaml, -1, &error));
	g_assert_no_error(error);
	root = yaml_node_to_json_node(yaml_parser_get_root(parser));
	check_document(json_node_get_object(root));
	g_assert_null(strstr(yaml, "nan"));
}

/*
 * Strings stay strings in YAML: a model called `no` or a label `42` is
 * double-quoted, or a reader would get false and a number back.
 */
static void
test_yaml_quotes_strings(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autoptr(AiMockDecider) decider = ai_mock_decider_new("{}");
	g_autoptr(AiBenchmarkTarget) target = ai_benchmark_target_new_for_object("42", AI_BENCHMARK_KIND_DECISION, G_OBJECT(decider));
	g_autofree gchar *yaml = NULL;
	AiBenchmarkSample *sample;

	g_assert_true(ai_benchmark_add_target(bench, target, NULL));
	sample = ai_benchmark_sample_new("42", "null", 0, 1000);
	ai_benchmark_sample_set_model(sample, "no");
	g_ptr_array_add(ai_benchmark_get_samples(bench), sample);
	yaml = ai_benchmark_dup_yaml(bench);
	g_assert_nonnull(strstr(yaml, "\"42\""));
	g_assert_nonnull(strstr(yaml, "\"no\""));
	g_assert_nonnull(strstr(yaml, "\"null\""));
	g_assert_null(strstr(yaml, "model: no\n"));
}

/*
 * CSV: one row per sample, a fixed header, RFC 4180 quoting for commas,
 * quotes and newlines, absent values as empty fields.
 */
static void
test_csv(
	void
){
	g_autoptr(AiBenchmark) bench = fixture();
	g_autofree gchar *csv = ai_benchmark_dup_csv(bench);
	static const gchar *expected =
		"target,case,iteration,outcome,elapsed_ms,ttft_ms,input_tokens,output_tokens,brier,log_loss,model,detail,error\n"
		"jev,refund,0,pass,100.000,,40,2,0.0100,0.1054,jev-1.14.0,,\n"
		"tev1,refund,0,fail,50.000,,,,0.3600,0.9163,tev1,\"q: expected true, P(true) was 0.4000\",\n"
		"jev,refund,1,pass,120.000,,40,2,0.0100,0.1054,jev-1.14.0,,\n"
		"tev1,refund,1,error,30000.000,,,,,,,,\"bad \"\"gateway\"\", retry\nlater\"\n";

	g_assert_cmpstr(csv, ==, expected);
}

/* Nothing run yet: every format still renders, with no rows of data. */
static void
test_empty_run(
	void
){
	g_autoptr(AiBenchmark) bench = ai_benchmark_new();
	g_autofree gchar *csv = ai_benchmark_dup_csv(bench);
	g_autofree gchar *json = ai_benchmark_dup_json(bench);
	g_autofree gchar *yaml = ai_benchmark_dup_yaml(bench);
	g_autofree gchar *table = ai_benchmark_dup_table(bench, FALSE);

	g_assert_cmpstr(strchr(csv, '\n') + 1, ==, "");
	g_assert_nonnull(strstr(json, "\"samples\""));
	g_assert_nonnull(yaml);
	g_assert_true(g_str_has_prefix(table, "target  runs"));
}

int
main(
	int    argc,
	char **argv
){
	g_unsetenv("ANTHROPIC_API_KEY");
	g_unsetenv("CLAUDE_API_KEY");
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/benchmark/output/table", test_table);
	g_test_add_func("/benchmark/output/table-per-case", test_table_per_case);
	g_test_add_func("/benchmark/output/table-unicode-alignment", test_table_unicode_alignment);
	g_test_add_func("/benchmark/output/json", test_json);
	g_test_add_func("/benchmark/output/yaml", test_yaml);
	g_test_add_func("/benchmark/output/yaml-quotes-strings", test_yaml_quotes_strings);
	g_test_add_func("/benchmark/output/csv", test_csv);
	g_test_add_func("/benchmark/output/empty-run", test_empty_run);
	return g_test_run();
}
