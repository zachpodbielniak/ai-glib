/*
 * test-benchmark-suite.c - Loading AiBenchmark from YAML and JSON suites
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The suite file is not ours, so most of this is one table of malformed
 * documents, each with the exact error it must produce and nothing else:
 * no critical, no abort, no half-loaded benchmark. Run with criticals
 * fatal (GTest's default), most of what the table asserts is that the
 * process is still alive and said the right thing.
 *
 * The rest proves the things that are easy to get wrong in YAML: quoting
 * is honoured (`"no"` is a string, an unquoted `no` is a boolean), a BOM
 * and CRLF line endings load, JSON loads, `request-file` resolves against
 * the suite's own directory, and every shipped example suite in
 * docs/examples loads -- so the examples cannot rot.
 */

#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>

static gchar *repo_root;
static gchar *scratch_dir;

/* A valid decision request, as YAML, for cases that need one. */
#define REQUEST \
	"    request:\n" \
	"      state: Please refund the duplicate charge.\n" \
	"      questions:\n" \
	"        refund: { type: noul, instructions: Is a refund requested? }\n"

#define DECISION_TARGET \
	"targets:\n" \
	"  - { provider: ollama, model: tev1, kind: decision }\n"

static AiBenchmark *
load(
	const gchar  *yaml,
	GError      **error
){
	return ai_benchmark_new_from_data(yaml, -1, scratch_dir, error);
}

static void
test_minimal_and_defaults(
	void
){
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;
	AiBenchmarkTarget *target;
	AiBenchmarkCase *bench_case;

	bench = load(DECISION_TARGET
	             "cases:\n"
	             "  - id: a\n"
	             "    kind: decision\n"
	             REQUEST, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bench);
	g_assert_null(ai_benchmark_get_name(bench));
	g_assert_cmpuint(ai_benchmark_get_iterations(bench), ==, 3);
	g_assert_cmpuint(ai_benchmark_get_warmup(bench), ==, 1);
	g_assert_cmpuint(ai_benchmark_get_timeout_ms(bench), ==, 0);

	target = g_ptr_array_index(ai_benchmark_get_targets(bench), 0);
	g_assert_cmpstr(ai_benchmark_target_get_label(target), ==, "ollama:tev1");
	g_assert_cmpstr(ai_benchmark_target_get_provider(target), ==, "ollama");
	g_assert_cmpstr(ai_benchmark_target_get_model(target), ==, "tev1");
	g_assert_false(ai_benchmark_target_get_stream(target));

	bench_case = g_ptr_array_index(ai_benchmark_get_cases(bench), 0);
	g_assert_cmpstr(ai_benchmark_case_get_id(bench_case), ==, "a");
	g_assert_false(ai_benchmark_case_has_expectation(bench_case));
}

/* Every kind, every key, the top-level kind default and both property forms. */
static void
test_every_kind(
	void
){
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;
	GPtrArray *targets;
	GPtrArray *cases;
	AiBenchmarkTarget *target;
	AiBenchmarkCase *bench_case;
	const gchar *const *props;

	bench = load(
		"name: everything\n"
		"kind: chat\n"
		"iterations: 7\n"
		"warmup: 0\n"
		"timeout-ms: 2500\n"
		"targets:\n"
		"  - label: fast\n"
		"    provider: ollama\n"
		"    model: llama3.2\n"
		"    stream: true\n"
		"    properties: [temperature=0.2, max-tokens=64]\n"
		"  - label: jev\n"
		"    kind: decision\n"
		"    provider: jev\n"
		"    properties: { timeout-ms: 1234, long-context: true }\n"
		"  - { label: emb, kind: embedding, provider: ollama, model: nomic-embed-text }\n"
		"  - { label: img, kind: image, provider: grok }\n"
		"cases:\n"
		"  - id: chat\n"
		"    prompt: Capital of France?\n"
		"    system: Be brief.\n"
		"    max-tokens: 32\n"
		"    expect: { contains: paris }\n"
		"  - id: decide\n"
		"    kind: decision\n"
		"    noul-threshold: 0.7\n"
		"    box-iou: 0.4\n"
		REQUEST
		"    expect: { refund: true }\n"
		"  - id: vectors\n"
		"    kind: embedding\n"
		"    texts: [cat, kitten, invoice]\n"
		"    expect: { similar: [0, 1], dissimilar: [0, 2] }\n"
		"  - id: fox\n"
		"    kind: image\n"
		"    prompt: a red fox\n"
		"    size: 512x512\n"
		"    count: 2\n", &error);
	g_assert_no_error(error);

	g_assert_cmpstr(ai_benchmark_get_name(bench), ==, "everything");
	g_assert_cmpuint(ai_benchmark_get_iterations(bench), ==, 7);
	g_assert_cmpuint(ai_benchmark_get_warmup(bench), ==, 0);
	g_assert_cmpuint(ai_benchmark_get_timeout_ms(bench), ==, 2500);

	targets = ai_benchmark_get_targets(bench);
	g_assert_cmpuint(targets->len, ==, 4);
	target = g_ptr_array_index(targets, 0);
	g_assert_cmpint(ai_benchmark_target_get_kind(target), ==, AI_BENCHMARK_KIND_CHAT);
	g_assert_true(ai_benchmark_target_get_stream(target));
	props = ai_benchmark_target_get_properties(target);
	g_assert_cmpstr(props[0], ==, "temperature=0.2");
	g_assert_cmpstr(props[1], ==, "max-tokens=64");
	g_assert_null(props[2]);
	target = g_ptr_array_index(targets, 1);
	g_assert_cmpint(ai_benchmark_target_get_kind(target), ==, AI_BENCHMARK_KIND_DECISION);
	g_assert_null(ai_benchmark_target_get_model(target));
	props = ai_benchmark_target_get_properties(target);
	g_assert_cmpstr(props[0], ==, "timeout-ms=1234");
	g_assert_cmpstr(props[1], ==, "long-context=true");

	cases = ai_benchmark_get_cases(bench);
	g_assert_cmpuint(cases->len, ==, 4);
	bench_case = g_ptr_array_index(cases, 0);
	g_assert_cmpstr(ai_benchmark_case_get_system(bench_case), ==, "Be brief.");
	g_assert_cmpint(ai_benchmark_case_get_max_tokens(bench_case), ==, 32);
	g_assert_cmpint(ai_benchmark_case_score_text(bench_case, "PARIS", NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
	bench_case = g_ptr_array_index(cases, 1);
	g_assert_cmpfloat(ai_benchmark_case_get_noul_threshold(bench_case), ==, 0.7);
	g_assert_true(ai_benchmark_case_has_expectation(bench_case));
	bench_case = g_ptr_array_index(cases, 2);
	g_assert_cmpuint(g_strv_length((gchar **)ai_benchmark_case_get_texts(bench_case)), ==, 3);
	g_assert_true(ai_benchmark_case_has_expectation(bench_case));
	bench_case = g_ptr_array_index(cases, 3);
	g_assert_cmpint(ai_benchmark_case_get_image_size(bench_case), ==, AI_IMAGE_SIZE_512);
	g_assert_cmpint(ai_benchmark_case_get_image_count(bench_case), ==, 2);
}

/*
 * Quoting is honoured. yaml-glib would type `"no"` as a boolean and `"42"`
 * as a number; the loader keeps any quoted scalar a string. An option
 * literally called `no`, a prompt of `yes` and a state of `"42"` all
 * survive.
 */
static void
test_quoting_honoured(
	void
){
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;
	AiBenchmarkCase *bench_case;

	bench = load(
		"targets:\n"
		"  - { label: d, provider: ollama, kind: decision }\n"
		"  - { label: c, provider: ollama, kind: chat }\n"
		"cases:\n"
		"  - id: yesno\n"
		"    kind: decision\n"
		"    request:\n"
		"      state: \"42\"\n"
		"      questions:\n"
		"        answer:\n"
		"          type: choice\n"
		"          instructions: Is the number even?\n"
		"          criteria: { \"yes\": it is even, \"no\": it is odd }\n"
		"    expect: { answer: no }\n"
		"  - id: plain\n"
		"    kind: chat\n"
		"    prompt: yes\n"
		"    expect: { equals: \"true\" }\n", &error);
	g_assert_no_error(error);

	bench_case = g_ptr_array_index(ai_benchmark_get_cases(bench), 0);
	json = ai_decision_request_dup_json(ai_benchmark_case_get_request(bench_case));
	g_assert_nonnull(strstr(json, "\"state\":\"42\""));
	g_assert_nonnull(strstr(json, "\"no\":\"it is odd\""));
	/* YAML 1.2 booleans: an unquoted `no` is the option's name, not false. */
	g_assert_true(ai_benchmark_case_has_expectation(bench_case));
	bench_case = g_ptr_array_index(ai_benchmark_get_cases(bench), 1);
	g_assert_cmpstr(ai_benchmark_case_get_prompt(bench_case), ==, "yes");
	g_assert_cmpint(ai_benchmark_case_score_text(bench_case, "true", NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
}

/* JSON is YAML: a suite written as JSON loads the same. */
static void
test_json_suite(
	void
){
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;

	bench = load(
		"{\"kind\": \"decision\", \"iterations\": 2,"
		" \"targets\": [{\"label\": \"j\", \"provider\": \"typesafe\"}],"
		" \"cases\": [{\"id\": \"a\", \"request\": {\"state\": \"hi\","
		"   \"questions\": {\"q\": {\"type\": \"noul\", \"instructions\": \"Greeting?\"}}},"
		"   \"expect\": {\"q\": true}}]}", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(ai_benchmark_get_iterations(bench), ==, 2);
	g_assert_true(ai_benchmark_case_has_expectation(g_ptr_array_index(ai_benchmark_get_cases(bench), 0)));
}

/* A UTF-8 BOM and CRLF line endings, as an editor on another OS writes them. */
static void
test_bom_and_crlf(
	void
){
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;

	bench = load("\xEF\xBB\xBF" "name: crlf\r\nkind: chat\r\ntargets:\r\n  - provider: ollama\r\n"
	             "cases:\r\n  - id: a\r\n    prompt: hi\r\n", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(ai_benchmark_get_name(bench), ==, "crlf");
	g_assert_cmpstr(ai_benchmark_case_get_prompt(g_ptr_array_index(ai_benchmark_get_cases(bench), 0)), ==, "hi");
}

/* request-file resolves against the suite's directory, not the cwd. */
static void
test_request_file(
	void
){
	g_autofree gchar *dir = g_build_filename(scratch_dir, "suites", NULL);
	g_autofree gchar *suite = g_build_filename(dir, "s.yaml", NULL);
	g_autofree gchar *request = g_build_filename(dir, "triage.json", NULL);
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(request,
		"{\"state\":\"Refund please\",\"questions\":{\"refund\":{\"type\":\"noul\",\"instructions\":\"Refund?\"}}}",
		-1, NULL));
	g_assert_true(g_file_set_contents(suite,
		DECISION_TARGET
		"cases:\n"
		"  - { id: a, kind: decision, request-file: triage.json, expect: { refund: yes } }\n", -1, NULL));

	bench = ai_benchmark_new_from_file(suite, &error);
	g_assert_no_error(error);
	g_assert_true(ai_benchmark_case_has_expectation(g_ptr_array_index(ai_benchmark_get_cases(bench), 0)));

	g_clear_object(&bench);
	bench = ai_benchmark_new_from_file("/nonexistent/suite.yaml", &error);
	g_assert_null(bench);
	g_assert_true(g_str_has_prefix(error->message, "benchmark suite: cannot read /nonexistent/suite.yaml: "));
}

typedef struct
{
	const gchar *name;
	const gchar *yaml;
	const gchar *message;  /* exact, after "benchmark suite: " */
	gboolean     prefix;   /* match only the start (parser wording) */
} Malformed;

static const Malformed malformed[] = {
	{ "empty", "", "", TRUE },
	{ "null-root", "null\n", "the document must be a mapping with 'targets' and 'cases'", FALSE },
	{ "sequence-root", "- a\n- b\n", "the document must be a mapping with 'targets' and 'cases'", FALSE },
	{ "two-documents", "a: 1\n---\nb: 2\n", "expected one YAML document, found 2", FALSE },
	{ "not-yaml", "targets: [\n", "not valid YAML: ", TRUE },
	{ "unknown-top-key", "iteration: 10\n",
	  "top level: unknown key 'iteration' (expected one of: name, kind, iterations, warmup, timeout-ms, targets, cases)", FALSE },
	{ "iterations-zero", "iterations: 0\n", "top level: 'iterations' must be a whole number from 1 to 100000", FALSE },
	{ "iterations-negative", "iterations: -3\n", "top level: 'iterations' must be a whole number from 1 to 100000", FALSE },
	{ "iterations-quoted", "iterations: \"5\"\n", "top level: 'iterations' must be a whole number from 1 to 100000", FALSE },
	{ "iterations-fraction", "iterations: 2.5\n", "top level: 'iterations' must be a whole number from 1 to 100000", FALSE },
	{ "iterations-huge", "iterations: 4000000000\n",
	  "top level: 'iterations' must be a whole number from 1 to 100000", FALSE },
	{ "warmup-huge", "warmup: 5000\n", "top level: 'warmup' must be a whole number from 0 to 1000", FALSE },
	{ "top-kind", "kind: decisions\n",
	  "top level: unknown kind 'decisions' (chat, decision, embedding or image)", FALSE },
	{ "name-mapping", "name: { a: b }\n", "top level: 'name' must be a string", FALSE },
	{ "no-targets", "cases: []\n", "'targets' must be a non-empty list", FALSE },
	{ "targets-mapping", "targets: { a: b }\n", "'targets' must be a non-empty list", FALSE },
	{ "target-scalar", "targets: [ollama]\n", "targets[0] must be a mapping", FALSE },
	{ "target-typo", "targets: [{ provider: ollama, modle: tev1, kind: chat }]\n",
	  "targets[0]: unknown key 'modle' (expected one of: label, kind, provider, model, stream, properties)", FALSE },
	{ "target-no-provider", "targets: [{ model: tev1, kind: chat }]\n", "targets[0]: 'provider' is required", FALSE },
	{ "target-no-kind", "targets: [{ provider: ollama }]\n",
	  "targets[0]: 'kind' is required (or set a top-level 'kind')", FALSE },
	{ "target-bad-kind", "targets: [{ provider: ollama, kind: chatt }]\n",
	  "targets[0]: unknown kind 'chatt' (chat, decision, embedding or image)", FALSE },
	{ "stream-decision", "targets: [{ provider: ollama, kind: decision, stream: true }]\n",
	  "targets[0]: 'stream' applies to chat targets only", FALSE },
	{ "stream-yes-is-text", "targets: [{ provider: ollama, kind: chat, stream: yes }]\n",
	  "targets[0]: 'stream' must be true or false", FALSE },
	{ "stream-maybe", "targets: [{ provider: ollama, kind: chat, stream: maybe }]\n",
	  "targets[0]: 'stream' must be true or false", FALSE },
	{ "properties-scalar", "targets: [{ provider: ollama, kind: chat, properties: 5 }]\n",
	  "targets[0]: 'properties' must be a list of NAME=VALUE or a mapping", FALSE },
	{ "properties-nested", "targets: [{ provider: ollama, kind: chat, properties: [[a]] }]\n",
	  "targets[0]: properties[0] must be NAME=VALUE", FALSE },
	{ "duplicate-label", "kind: chat\ntargets: [{ provider: ollama }, { provider: ollama }]\n",
	  "targets[1]: duplicate target label 'ollama'", FALSE },
	{ "no-cases", "kind: chat\ntargets: [{ provider: ollama }]\n", "'cases' must be a non-empty list", FALSE },
	{ "case-no-id", "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ prompt: hi }]\n",
	  "cases[0]: 'id' is required", FALSE },
	{ "case-typo", "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ id: a, promt: hi }]\n",
	  "case 'a': unknown key 'promt' (expected one of: id, kind, prompt, system, max-tokens, expect)", FALSE },
	{ "case-no-prompt", "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ id: a }]\n",
	  "case 'a': 'prompt' is required", FALSE },
	{ "chat-two-expectations",
	  "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ id: a, prompt: hi, expect: { contains: x, equals: y } }]\n",
	  "case 'a'.expect needs exactly one of contains, equals or regex", FALSE },
	{ "chat-expect-typo",
	  "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ id: a, prompt: hi, expect: { contain: x } }]\n",
	  "case 'a'.expect: unknown key 'contain' (expected one of: contains, equals, regex)", FALSE },
	{ "chat-bad-regex",
	  "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ id: a, prompt: hi, expect: { regex: '(x' } }]\n",
	  "case 'a': invalid regex '(x': ", TRUE },
	{ "max-tokens-zero",
	  "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ id: a, prompt: hi, max-tokens: 0 }]\n",
	  "case 'a': 'max-tokens' must be a whole number from 1 to 2147483647", FALSE },
	{ "decision-no-request", DECISION_TARGET "cases: [{ id: a, kind: decision }]\n",
	  "case 'a' needs exactly one of 'request' or 'request-file'", FALSE },
	{ "decision-both-requests",
	  DECISION_TARGET "cases:\n  - id: a\n    kind: decision\n    request-file: x.json\n" REQUEST,
	  "case 'a' needs exactly one of 'request' or 'request-file'", FALSE },
	{ "decision-invalid-request",
	  DECISION_TARGET "cases: [{ id: a, kind: decision, request: { state: hi } }]\n",
	  "case 'a'.request: A decision needs a state (text, object or array, up to 50000 characters) and 1 to 64 questions", FALSE },
	{ "decision-missing-file",
	  DECISION_TARGET "cases: [{ id: a, kind: decision, request-file: missing.json }]\n",
	  "case 'a': request-file: cannot read ", TRUE },
	{ "request-file-device",
	  DECISION_TARGET "cases: [{ id: a, kind: decision, request-file: /dev/zero }]\n",
	  "case 'a': request-file: cannot read /dev/zero: not a regular file", FALSE },
	{ "decision-unknown-question",
	  DECISION_TARGET "cases:\n  - id: a\n    kind: decision\n" REQUEST "    expect: { refnd: true }\n",
	  "case 'a': expectation names question 'refnd', which the request does not ask", FALSE },
	{ "decision-bad-answer",
	  DECISION_TARGET "cases:\n  - id: a\n    kind: decision\n" REQUEST "    expect: { refund: maybe }\n",
	  "case 'a': 'maybe' is not a valid answer for noul question 'refund'", FALSE },
	{ "decision-answer-mapping",
	  DECISION_TARGET "cases:\n  - id: a\n    kind: decision\n" REQUEST "    expect: { refund: { a: b } }\n",
	  "case 'a'.expect.refund must be true/false, an option, a level, or [top, bottom, left, right]", FALSE },
	{ "threshold-range",
	  DECISION_TARGET "cases:\n  - id: a\n    kind: decision\n    noul-threshold: 1.5\n" REQUEST,
	  "case 'a': 'noul-threshold' must be a number from 0 to 1", FALSE },
	{ "embedding-no-texts",
	  "kind: embedding\ntargets: [{ provider: ollama }]\ncases: [{ id: e, texts: [] }]\n",
	  "case 'e': 'texts' must be a non-empty list of strings", FALSE },
	{ "embedding-bad-pair",
	  "kind: embedding\ntargets: [{ provider: ollama }]\ncases: [{ id: e, texts: [a, b], expect: { similar: [0], dissimilar: [0, 1] } }]\n",
	  "case 'e'.expect: 'similar' must be a pair of text indices, e.g. [0, 1]", FALSE },
	{ "embedding-index-range",
	  "kind: embedding\ntargets: [{ provider: ollama }]\ncases: [{ id: e, texts: [a, b], expect: { similar: [0, 1], dissimilar: [0, 5] } }]\n",
	  "case 'e': similarity indices must name texts 0 to 1 of an embedding case", FALSE },
	{ "image-bad-size",
	  "kind: image\ntargets: [{ provider: grok }]\ncases: [{ id: i, prompt: fox, size: huge }]\n",
	  "case 'i': unknown size 'huge' (auto, 256x256, 512x512, 1024x1024, 1024x1792 or 1792x1024)", FALSE },
	{ "image-count-range",
	  "kind: image\ntargets: [{ provider: grok }]\ncases: [{ id: i, prompt: fox, count: 17 }]\n",
	  "case 'i': 'count' must be a whole number from 1 to 16", FALSE },
	{ "no-target-for-kind",
	  "targets: [{ provider: ollama, kind: chat }]\ncases: [{ id: e, kind: embedding, texts: [a] }]\n",
	  "case 'e' has kind embedding and no target has that kind", FALSE },
	{ "duplicate-case",
	  "kind: chat\ntargets: [{ provider: ollama }]\ncases: [{ id: a, prompt: x }, { id: a, prompt: y }]\n",
	  "duplicate case id 'a'", FALSE },
};

static void
test_malformed(
	gconstpointer data
){
	const Malformed *row = (const Malformed *)data;
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *expected = g_strconcat("benchmark suite: ", row->message, NULL);

	bench = load(row->yaml, &error);
	g_assert_null(bench);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	if (row->prefix)
		g_assert_true(g_str_has_prefix(error->message, expected));
	else
		g_assert_cmpstr(error->message, ==, expected);
}

/* Bytes the loader refuses before YAML ever sees them. */
static void
test_bounds(
	void
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) deep = g_string_new("a: ");
	g_autofree gchar *huge = NULL;
	guint i;

	g_assert_null(ai_benchmark_new_from_file("/dev/zero", &error));
	g_assert_cmpstr(error->message, ==, "benchmark suite: cannot read /dev/zero: not a regular file");
	g_clear_error(&error);

	g_assert_null(ai_benchmark_new_from_data("name: \xC3\x28\n", -1, NULL, &error));
	g_assert_cmpstr(error->message, ==, "benchmark suite: not valid UTF-8 text");
	g_clear_error(&error);

	g_assert_null(ai_benchmark_new_from_data("name: a\0b\n", 10, NULL, &error));
	g_assert_cmpstr(error->message, ==, "benchmark suite: not valid UTF-8 text");
	g_clear_error(&error);

	huge = g_malloc(4 * 1024 * 1024 + 2);
	memset(huge, ' ', 4 * 1024 * 1024 + 1);
	huge[4 * 1024 * 1024 + 1] = '\0';
	g_assert_null(ai_benchmark_new_from_data(huge, -1, NULL, &error));
	g_assert_cmpstr(error->message, ==, "benchmark suite: larger than 4194304 bytes");
	g_clear_error(&error);

	for (i = 0; i < 40; i++)
		g_string_append_c(deep, '[');
	for (i = 0; i < 40; i++)
		g_string_append_c(deep, ']');
	g_string_append_c(deep, '\n');
	g_assert_null(ai_benchmark_new_from_data(deep->str, -1, NULL, &error));
	g_assert_cmpstr(error->message, ==, "benchmark suite: nested deeper than 32 levels");
}

/* The suites shipped in docs/examples load, and say what the docs say. */
static void
test_shipped_examples(
	void
){
	static const struct
	{
		const gchar *file;
		guint        targets;
		guint        cases;
		guint        iterations;
	} examples[] = {
		{ "benchmark-decisions.yaml", 3, 5, 5 },
		{ "benchmark-chat.yaml", 3, 3, 3 },
		{ "benchmark-embedding.yaml", 2, 2, 5 },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(examples); i++)
	{
		g_autofree gchar *path = g_build_filename(repo_root, "docs", "examples", examples[i].file, NULL);
		g_autoptr(AiBenchmark) bench = NULL;
		g_autoptr(GError) error = NULL;
		GPtrArray *cases;
		guint k;

		g_test_message("%s", path);
		bench = ai_benchmark_new_from_file(path, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(ai_benchmark_get_targets(bench)->len, ==, examples[i].targets);
		g_assert_cmpuint(ai_benchmark_get_cases(bench)->len, ==, examples[i].cases);
		g_assert_cmpuint(ai_benchmark_get_iterations(bench), ==, examples[i].iterations);
		/* Every example case is labelled: an example that measures nothing is a bad example. */
		cases = ai_benchmark_get_cases(bench);
		for (k = 0; k < cases->len; k++)
			g_assert_true(ai_benchmark_case_has_expectation(g_ptr_array_index(cases, k)));
	}
}

int
main(
	int    argc,
	char **argv
){
	g_autofree gchar *here = NULL;
	gint status;
	guint i;

	/* build/<type>/tests/test-benchmark-suite -> the repository root. */
	here = g_path_get_dirname(argv[0]);
	{
		g_autofree gchar *joined = g_build_filename(here, "..", "..", "..", NULL);
		g_autoptr(GFile) file = g_file_new_for_path(joined);

		repo_root = g_file_get_path(file);
	}
	scratch_dir = g_dir_make_tmp("ai-benchmark-suite-XXXXXX", NULL);
	g_setenv("HOME", scratch_dir, TRUE);
	g_setenv("XDG_CONFIG_HOME", scratch_dir, TRUE);

	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/benchmark/suite/minimal-and-defaults", test_minimal_and_defaults);
	g_test_add_func("/benchmark/suite/every-kind", test_every_kind);
	g_test_add_func("/benchmark/suite/quoting-honoured", test_quoting_honoured);
	g_test_add_func("/benchmark/suite/json", test_json_suite);
	g_test_add_func("/benchmark/suite/bom-and-crlf", test_bom_and_crlf);
	g_test_add_func("/benchmark/suite/request-file", test_request_file);
	g_test_add_func("/benchmark/suite/bounds", test_bounds);
	g_test_add_func("/benchmark/suite/shipped-examples", test_shipped_examples);
	for (i = 0; i < G_N_ELEMENTS(malformed); i++)
	{
		g_autofree gchar *path = g_strdup_printf("/benchmark/suite/malformed/%s", malformed[i].name);

		g_test_add_data_func(path, &malformed[i], test_malformed);
	}
	status = g_test_run();

	{
		g_autoptr(GSubprocess) cleanup = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL,
		                                                  "rm", "-rf", "--", scratch_dir, NULL);

		if (cleanup != NULL)
			g_subprocess_wait(cleanup, NULL, NULL);
	}
	g_free(scratch_dir);
	g_free(repo_root);
	return status;
}
