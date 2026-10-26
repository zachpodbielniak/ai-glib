/*
 * test-benchmark-cli.c - `ai benchmark`, spawned for real
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Drives the built `ai` binary against two loopback decision servers --
 * one standing in for a local Ollama (OLLAMA_HOST), one for Laya
 * (LAYA_BASE_URL) -- so quick mode, suite mode, every output format,
 * --dry-run, -t, --set and every exit status are covered with no network.
 *
 * --help is checked structurally: its sections exist, and every option in
 * the real option table (walked through benchmark_option_entries(), not a
 * copy of it) appears with a description. A flag added without help text
 * fails here.
 */

#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <yaml-glib.h>

#include "test-server.h"
#include "../bin/ai-benchmark-cli.h"

static gchar *ai_binary;

static const gchar *spam_yes =
	"{\"model\":\"tev1\",\"answers\":{\"answer\":{\"type\":\"noul\",\"noul\":0.9}},"
	"\"usage\":{\"input_tokens\":12,\"output_tokens\":1}}";
static const gchar *spam_no =
	"{\"model\":\"laya-english\",\"answers\":{\"answer\":{\"type\":\"noul\",\"noul\":0.2}}}";

typedef struct
{
	gint   status;
	gchar *out;
	gchar *err;
	gchar *dir;
} Run;

static void
run_clear(
	Run *run
){
	g_autoptr(GSubprocess) cleanup = NULL;

	g_free(run->out);
	g_free(run->err);
	cleanup = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, NULL, "rm", "-rf", "--", run->dir, NULL);
	if (cleanup != NULL)
		g_subprocess_wait(cleanup, NULL, NULL);
	g_free(run->dir);
}

/*
 * Runs `ai benchmark ARGS...` in a fresh scratch HOME and cwd. @files is
 * NULL or name, content pairs written into that directory first.
 */
static Run
run_ai(
	const gchar *const *args,
	const gchar *const *files,
	const gchar        *ollama_url,
	const gchar        *laya_url
){
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	static const gchar *const unset[] = {
		"TYPESAFE_API_KEY", "TYPESAFE_BASE_URL", "TYPESAFE_DEFAULT_MODEL", "OLLAMA_API_KEY",
		"LAYA_API_KEY", "AI_PROVIDER", "AI_GLIB_DEFAULT_PROVIDER", "ANTHROPIC_API_KEY",
		"CLAUDE_API_KEY", "G_MESSAGES_DEBUG", NULL
	};
	Run run = { 0, NULL, NULL, NULL };
	guint i;

	run.dir = g_dir_make_tmp("ai-benchmark-cli-XXXXXX", &error);
	g_assert_no_error(error);
	for (i = 0; files != NULL && files[i] != NULL; i += 2)
	{
		g_autofree gchar *path = g_build_filename(run.dir, files[i], NULL);

		g_assert_true(g_file_set_contents(path, files[i + 1], -1, NULL));
	}

	g_ptr_array_add(argv, ai_binary);
	g_ptr_array_add(argv, (gpointer)"benchmark");
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);

	g_subprocess_launcher_set_cwd(launcher, run.dir);
	g_subprocess_launcher_setenv(launcher, "HOME", run.dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", run.dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_STATE_HOME", run.dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CACHE_HOME", run.dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_DATA_HOME", run.dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "AI_GLIB_NO_UPDATE_CHECK", "1", TRUE);
	for (i = 0; unset[i] != NULL; i++)
		g_subprocess_launcher_unsetenv(launcher, unset[i]);
	g_subprocess_launcher_setenv(launcher, "OLLAMA_HOST",
	                             ollama_url != NULL ? ollama_url : "http://127.0.0.1:9", TRUE);
	if (laya_url != NULL)
		g_subprocess_launcher_setenv(launcher, "LAYA_BASE_URL", laya_url, TRUE);
	else
		g_subprocess_launcher_unsetenv(launcher, "LAYA_BASE_URL");

	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &run.out, &run.err, &error));
	g_assert_no_error(error);
	g_test_message("stdout: %s\nstderr: %s", run.out, run.err);
	g_assert_true(g_subprocess_get_if_exited(process));
	run.status = g_subprocess_get_exit_status(process);
	return run;
}

/* --help: the manual's sections, and every real option documented. */
static void
test_help(
	void
){
	const gchar *args[] = { "--help", NULL };
	const gchar *none[] = { NULL };
	static const gchar *const sections[] = {
		"SYNOPSIS", "DESCRIPTION", "METRICS", "EXAMPLES", "EXIT STATUS", "ENVIRONMENT", "SEE ALSO", NULL
	};
	BenchmarkOptions options;
	g_autofree GOptionEntry *entries = NULL;
	Run run = run_ai(args, NULL, NULL, NULL);
	Run bare = run_ai(none, NULL, NULL, NULL);
	guint i;

	g_assert_cmpint(run.status, ==, 0);
	g_assert_true(g_str_has_prefix(run.out, "Usage:\n  ai benchmark [OPTION"));
	for (i = 0; sections[i] != NULL; i++)
		g_assert_nonnull(strstr(run.out, sections[i]));
	/* the examples a reader is most likely to copy */
	g_assert_nonnull(strstr(run.out, "ai benchmark --kind decision -T typesafe:jev-latest -T ollama:tev1"));
	g_assert_nonnull(strstr(run.out, "ai benchmark decisions.yaml --dry-run"));
	g_assert_nonnull(strstr(run.out, "--format csv -o out.csv"));

	memset(&options, 0, sizeof(options));
	entries = benchmark_option_entries(&options);
	for (i = 0; entries[i].long_name != NULL; i++)
	{
		g_autofree gchar *flag = NULL;

		if (g_str_equal(entries[i].long_name, G_OPTION_REMAINING))
			continue;
		flag = g_strdup_printf("--%s", entries[i].long_name);
		g_test_message("%s", flag);
		g_assert_nonnull(entries[i].description);
		g_assert_cmpstr(entries[i].description, !=, "");
		g_assert_nonnull(strstr(run.out, flag));
	}

	/* No arguments at all is a request for help, not an error. */
	g_assert_cmpint(bare.status, ==, 0);
	g_assert_cmpstr(bare.out, ==, run.out);
	run_clear(&run);
	run_clear(&bare);
}

/* The top-level `ai --help` points at the subcommand. */
static void
test_ai_help_mentions_benchmark(
	void
){
	g_autofree gchar *out = NULL;
	g_autoptr(GError) error = NULL;
	gint status;
	const gchar *argv[] = { ai_binary, "--help", NULL };

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
	                           &out, NULL, &status, &error));
	g_assert_nonnull(strstr(out, "ai benchmark --help"));
	g_assert_nonnull(strstr(out, "Benchmark examples"));
	g_assert_nonnull(strstr(out, "ai benchmark -k decision -T typesafe:jev-latest -T ollama:tev1"));
}

static void
test_version_license_kinds(
	void
){
	const gchar *license[] = { "--license", NULL };
	const gchar *kinds[] = { "--list-kinds", NULL };
	Run a = run_ai(license, NULL, NULL, NULL);
	Run b = run_ai(kinds, NULL, NULL, NULL);

	g_assert_cmpint(a.status, ==, 0);
	g_assert_nonnull(strstr(a.out, "AGPL version 3 or later"));
	g_assert_cmpint(b.status, ==, 0);
	g_assert_true(g_str_has_prefix(b.out, "provider "));
	g_assert_nonnull(strstr(b.out, "\nollama "));
	g_assert_nonnull(strstr(b.out, "\ntypesafe "));
	g_assert_nonnull(strstr(b.out, "\njev "));
	run_clear(&a);
	run_clear(&b);
}

/*
 * Quick mode: an Ollama decision model against Laya, same question,
 * labelled answer, JSON out. Each server sees warmup + iterations calls.
 */
static void
test_quick_decision_json(
	void
){
	TServer *ollama = tserver_new();
	TServer *laya = tserver_new();
	const gchar *args[] = {
		"--kind", "decision", "-T", "ollama:tev1", "-T", "laya", "-q", "Is this spam?",
		"--expect", "true", "-n", "2", "--format", "json", "--quiet", "Win a free prize", NULL
	};
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	JsonArray *summaries;
	JsonObject *first;
	JsonObject *second;
	Run run;

	tserver_set_response(ollama, 200, spam_yes);
	tserver_set_response(laya, 200, spam_no);
	run = run_ai(args, NULL, ollama->base_url, laya->base_url);

	g_assert_cmpint(run.status, ==, 0);
	g_assert_cmpstr(run.err, ==, "");
	g_assert_cmpuint(tserver_hits(ollama), ==, 3);
	g_assert_cmpuint(tserver_hits(laya), ==, 3);
	g_assert_nonnull(strstr(ollama->last_body, "Win a free prize"));
	g_assert_nonnull(strstr(laya->last_body, "Is this spam?"));

	g_assert_true(json_parser_load_from_data(parser, run.out, -1, NULL));
	root = json_node_get_object(json_parser_get_root(parser));
	summaries = json_object_get_array_member(root, "summaries");
	first = json_array_get_object_element(summaries, 0);
	second = json_array_get_object_element(summaries, 1);
	g_assert_cmpstr(json_object_get_string_member(first, "target"), ==, "ollama:tev1");
	g_assert_cmpfloat(json_object_get_double_member(first, "accuracy"), ==, 1.0);
	g_assert_cmpint(json_object_get_int_member(first, "input_tokens"), ==, 24);
	g_assert_cmpstr(json_object_get_string_member(second, "target"), ==, "laya");
	g_assert_cmpfloat(json_object_get_double_member(second, "accuracy"), ==, 0.0);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(root, "samples")), ==, 4);

	run_clear(&run);
	tserver_free(ollama);
	tserver_free(laya);
}

/* Without --quiet, one progress line per measured call goes to stderr. */
static void
test_progress(
	void
){
	TServer *ollama = tserver_new();
	const gchar *args[] = {
		"-T", "ollama:tev1", "-q", "Spam?", "-n", "2", "--warmup", "0", "text", NULL
	};
	Run run;

	tserver_set_response(ollama, 200, spam_yes);
	run = run_ai(args, NULL, ollama->base_url, NULL);
	g_assert_cmpint(run.status, ==, 0);
	g_assert_nonnull(strstr(run.err, "[1/2] ollama:tev1 / answer #1: "));
	g_assert_nonnull(strstr(run.err, "[2/2] ollama:tev1 / answer #2: "));
	/* The default format is the table, on stdout. */
	g_assert_true(g_str_has_prefix(run.out, "target "));
	g_assert_nonnull(strstr(run.out, "\nollama:tev1 "));
	run_clear(&run);
	tserver_free(ollama);
}

static const gchar *suite_yaml =
	"name: cli suite\n"
	"kind: decision\n"
	"iterations: 2\n"
	"warmup: 0\n"
	"targets:\n"
	"  - { label: local, provider: ollama, model: tev1 }\n"
	"  - { label: laya, provider: laya }\n"
	"cases:\n"
	"  - id: spam\n"
	"    request:\n"
	"      state: Win a free prize\n"
	"      questions:\n"
	"        answer: { type: noul, instructions: Is this spam? }\n"
	"    expect: { answer: true }\n";

/* Suite mode with -t: only the selected target runs; CSV goes to -o FILE. */
static void
test_suite_select_csv_output(
	void
){
	TServer *ollama = tserver_new();
	TServer *laya = tserver_new();
	const gchar *files[] = { "suite.yaml", suite_yaml, NULL };
	const gchar *args[] = { "suite.yaml", "-t", "laya", "--format", "csv", "-o", "out.csv", "--quiet", NULL };
	g_autofree gchar *csv = NULL;
	g_autofree gchar *path = NULL;
	g_auto(GStrv) lines = NULL;
	Run run;

	tserver_set_response(ollama, 200, spam_yes);
	tserver_set_response(laya, 200, spam_no);
	run = run_ai(args, files, ollama->base_url, laya->base_url);

	g_assert_cmpint(run.status, ==, 0);
	g_assert_cmpstr(run.out, ==, "");
	g_assert_cmpuint(tserver_hits(ollama), ==, 0);
	g_assert_cmpuint(tserver_hits(laya), ==, 2);
	path = g_build_filename(run.dir, "out.csv", NULL);
	g_assert_true(g_file_get_contents(path, &csv, NULL, NULL));
	lines = g_strsplit(csv, "\n", -1);
	g_assert_true(g_str_has_prefix(lines[0], "target,case,iteration,outcome,"));
	g_assert_true(g_str_has_prefix(lines[1], "laya,spam,0,fail,"));
	g_assert_true(g_str_has_prefix(lines[2], "laya,spam,1,fail,"));
	g_assert_cmpstr(lines[3], ==, "");
	run_clear(&run);
	tserver_free(ollama);
	tserver_free(laya);
}

/* --format yaml and --per-case table, from the same suite. */
static void
test_suite_yaml_and_per_case(
	void
){
	TServer *ollama = tserver_new();
	TServer *laya = tserver_new();
	const gchar *files[] = { "suite.yaml", suite_yaml, NULL };
	const gchar *yaml_args[] = { "suite.yaml", "--format", "yaml", "--quiet", NULL };
	const gchar *table_args[] = { "suite.yaml", "--per-case", "--quiet", NULL };
	g_autoptr(YamlParser) parser = yaml_parser_new();
	Run yaml;
	Run table;

	tserver_set_response(ollama, 200, spam_yes);
	tserver_set_response(laya, 200, spam_no);
	yaml = run_ai(yaml_args, files, ollama->base_url, laya->base_url);
	g_assert_cmpint(yaml.status, ==, 0);
	g_assert_true(yaml_parser_load_from_data(parser, yaml.out, -1, NULL));
	g_assert_nonnull(strstr(yaml.out, "name: \"cli suite\""));

	table = run_ai(table_args, files, ollama->base_url, laya->base_url);
	g_assert_cmpint(table.status, ==, 0);
	g_assert_true(g_str_has_prefix(table.out, "cli suite\n\ntarget "));
	g_assert_nonnull(strstr(table.out, "\nlocal / spam "));
	g_assert_nonnull(strstr(table.out, "\nlaya / spam "));
	run_clear(&yaml);
	run_clear(&table);
	tserver_free(ollama);
	tserver_free(laya);
}

/* --dry-run builds every target and prints the plan, and calls nothing. */
static void
test_dry_run_calls_nothing(
	void
){
	TServer *ollama = tserver_new();
	TServer *laya = tserver_new();
	const gchar *files[] = { "suite.yaml", suite_yaml, NULL };
	const gchar *args[] = { "suite.yaml", "--dry-run", "-n", "4", NULL };
	Run run;

	run = run_ai(args, files, ollama->base_url, laya->base_url);
	g_assert_cmpint(run.status, ==, 0);
	g_assert_true(g_str_has_prefix(run.out, "cli suite: 2 targets, 1 cases, 4 iterations, 0 warmup per target\n"));
	g_assert_nonnull(strstr(run.out, "  local "));
	g_assert_nonnull(strstr(run.out, "ollama:tev1"));
	g_assert_nonnull(strstr(run.out, "8 calls (8 measured)\n"));
	g_assert_cmpuint(tserver_hits(ollama), ==, 0);
	g_assert_cmpuint(tserver_hits(laya), ==, 0);
	run_clear(&run);
	tserver_free(ollama);
	tserver_free(laya);
}

typedef struct
{
	const gchar *name;
	const gchar *args[12];
	gint         status;
	const gchar *stderr_has;
} ExitRow;

/* Every exit status, each with the sentence that explains it. */
static void
test_exit_statuses(
	void
){
	static const ExitRow rows[] = {
		{ "unknown-option", { "--frobnicate", NULL }, 2, "Try 'ai benchmark --help'." },
		{ "bad-format", { "-T", "ollama", "--format", "xml", "hi", NULL }, 2, "unknown --format 'xml'" },
		{ "missing-suite", { "nope.yaml", NULL }, 2, "benchmark suite: cannot read nope.yaml" },
		{ "two-suites", { "a.yaml", "b.yaml", NULL }, 2, "give one suite file, or use -T for quick mode" },
		{ "quick-no-input", { "-T", "ollama", NULL }, 2, "quick mode needs the input as an argument" },
		{ "decision-no-question", { "--kind", "decision", "-T", "ollama", "hi", NULL }, 2,
		  "--kind decision needs --question" },
		{ "stream-decision", { "--kind", "decision", "--stream", "-T", "ollama", "-q", "Q?", "hi", NULL }, 2,
		  "--stream applies to --kind chat" },
		{ "select-in-quick", { "-T", "ollama", "-t", "x", "hi", NULL }, 2, "-t selects suite targets" },
		{ "bad-kind", { "--kind", "speech", "-T", "ollama", "hi", NULL }, 2, "unknown --kind 'speech'" },
		{ "image-expect", { "--kind", "image", "-T", "grok", "--expect", "x", "fox", NULL }, 2,
		  "image runs are never scored" },
		{ "iterations-range", { "-T", "ollama", "-n", "999999", "hi", NULL }, 2, "--iterations must be from 1 to 100000" },
		{ "warmup-negative", { "-T", "ollama", "--warmup", "-1", "hi", NULL }, 2, "--warmup must be from 0 to 1000" },
		{ "timeout-negative", { "-T", "ollama", "--timeout-ms", "-5", "hi", NULL }, 2, "--timeout-ms must be 0 (none) or more" },
		{ "nothing-to-run", { "--kind", "embedding", "-T", "claude", "hi", NULL }, 1,
		  "nothing to run: target 'claude': provider 'claude' cannot run embedding" },
		{ "set-unknown-property", { "--kind", "decision", "-T", "laya", "-q", "Q?", "--set", "nope=1", "hi", NULL }, 1,
		  "AiLayaClient has no property 'nope'" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++)
	{
		Run run;

		g_test_message("%s", rows[i].name);
		run = run_ai(rows[i].args, NULL, NULL, NULL);
		g_assert_cmpint(run.status, ==, rows[i].status);
		g_assert_nonnull(strstr(run.err, rows[i].stderr_has));
		g_assert_cmpstr(run.out, ==, "");
		run_clear(&run);
	}
}

/* Every measured call failed: the result still prints, and the status says so. */
static void
test_every_call_failed(
	void
){
	TServer *ollama = tserver_new();
	const gchar *args[] = { "-T", "ollama:tev1", "-q", "Spam?", "-n", "2", "--warmup", "0", "--quiet", "x", NULL };
	Run run;

	tserver_set_response(ollama, 500, "{\"error\":\"model not loaded\"}");
	run = run_ai(args, NULL, ollama->base_url, NULL);
	g_assert_cmpint(run.status, ==, 1);
	g_assert_nonnull(strstr(run.err, "every measured call failed"));
	g_assert_nonnull(strstr(run.out, "\nollama:tev1 "));
	run_clear(&run);
	tserver_free(ollama);
}

/* -t that leaves a case with no target of its kind says so instead of silently skipping it. */
static void
test_select_leaves_case_unrun(
	void
){
	TServer *laya = tserver_new();
	const gchar *suite =
		"kind: decision\n"
		"targets:\n"
		"  - { label: laya, provider: laya }\n"
		"  - { label: chat, kind: chat, provider: ollama }\n"
		"cases:\n"
		"  - id: spam\n"
		"    request: { state: x, questions: { answer: { type: noul, instructions: Spam? } } }\n"
		"  - { id: greet, kind: chat, prompt: hi }\n";
	const gchar *files[] = { "suite.yaml", suite, NULL };
	const gchar *args[] = { "suite.yaml", "-t", "laya", "--dry-run", NULL };
	Run run;

	run = run_ai(args, files, NULL, laya->base_url);
	g_assert_cmpint(run.status, ==, 0);
	g_assert_nonnull(strstr(run.err, "note: case 'greet' has no selected chat target and will not run"));
	g_assert_cmpuint(tserver_hits(laya), ==, 0);
	run_clear(&run);
	tserver_free(laya);
}

/* A suite target's own base-url wins over LAYA_BASE_URL. */
static void
test_laya_env_does_not_override(
	void
){
	TServer *configured = tserver_new();
	TServer *environment = tserver_new();
	g_autofree gchar *suite = g_strdup_printf(
		"kind: decision\niterations: 1\nwarmup: 0\n"
		"targets:\n  - { label: laya, provider: laya, properties: [base-url=%s] }\n"
		"cases:\n  - id: spam\n    request: { state: x, questions: { answer: { type: noul, instructions: Spam? } } }\n",
		configured->base_url);
	const gchar *files[] = { "suite.yaml", suite, NULL };
	const gchar *args[] = { "suite.yaml", "--quiet", NULL };
	Run run;

	tserver_set_response(configured, 200, spam_no);
	tserver_set_response(environment, 200, spam_no);
	run = run_ai(args, files, NULL, environment->base_url);
	g_assert_cmpint(run.status, ==, 0);
	g_assert_cmpuint(tserver_hits(configured), ==, 1);
	g_assert_cmpuint(tserver_hits(environment), ==, 0);
	run_clear(&run);
	tserver_free(configured);
	tserver_free(environment);
}

int
main(
	int    argc,
	char **argv
){
	g_autofree gchar *dir = NULL;

	g_test_init(&argc, &argv, NULL);
	/* Absolute: every run sets the child's cwd to its own scratch directory. */
	dir = g_path_get_dirname(argv[0]);
	{
		g_autofree gchar *relative = g_build_filename(dir, "..", "bin", "ai", NULL);
		g_autoptr(GFile) file = g_file_new_for_path(relative);

		ai_binary = g_file_get_path(file);
	}
	if (!g_file_test(ai_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai binary not built");
		g_free(ai_binary);
		return 0;
	}

	g_test_add_func("/benchmark/cli/help", test_help);
	g_test_add_func("/benchmark/cli/ai-help-mentions-benchmark", test_ai_help_mentions_benchmark);
	g_test_add_func("/benchmark/cli/version-license-kinds", test_version_license_kinds);
	g_test_add_func("/benchmark/cli/quick-decision-json", test_quick_decision_json);
	g_test_add_func("/benchmark/cli/progress", test_progress);
	g_test_add_func("/benchmark/cli/suite-select-csv-output", test_suite_select_csv_output);
	g_test_add_func("/benchmark/cli/suite-yaml-and-per-case", test_suite_yaml_and_per_case);
	g_test_add_func("/benchmark/cli/dry-run-calls-nothing", test_dry_run_calls_nothing);
	g_test_add_func("/benchmark/cli/exit-statuses", test_exit_statuses);
	g_test_add_func("/benchmark/cli/every-call-failed", test_every_call_failed);
	g_test_add_func("/benchmark/cli/laya-env-does-not-override", test_laya_env_does_not_override);
	g_test_add_func("/benchmark/cli/select-leaves-case-unrun", test_select_leaves_case_unrun);
	{
		gint status = g_test_run();

		g_free(ai_binary);
		return status;
	}
}
