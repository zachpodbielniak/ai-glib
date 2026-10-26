/*
 * ai-benchmark-cli.h - `ai benchmark`: compare providers, models and features
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A thin frontend over AiBenchmark. Everything that measures, scores or
 * formats lives in the library (src/bench/) so ai-tui, ai-gui and Emacs
 * print exactly what this prints. This file only turns argv into targets
 * and cases, drives the run, and writes the result.
 *
 * Two modes:
 *
 *   ai benchmark SUITE.yaml [options]       a labelled suite file
 *   ai benchmark --kind K -T P[:M] ... ARG  one case built from flags
 *
 * Exit status: 0 the run completed (individual calls may still have
 * failed -- see the err column), 1 nothing could run or every call
 * failed, 2 a bad command line or suite, 130 interrupted (the partial
 * result is still printed).
 */

#pragma once

#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ai-glib.h"

#define BENCHMARK_EXIT_OK          (0)
#define BENCHMARK_EXIT_FAILED      (1)
#define BENCHMARK_EXIT_USAGE       (2)
#define BENCHMARK_EXIT_INTERRUPTED (130)

/* "Not given on the command line": a value no option can legitimately take. */
#define BENCHMARK_UNSET            (G_MININT)

typedef struct
{
	/* settings */
	gint      iterations;
	gint      warmup;
	gint      timeout_ms;
	gchar   **select;
	gchar   **sets;
	gchar    *format;
	gchar    *output;
	gboolean  per_case;
	gboolean  dry_run;
	gboolean  quiet;
	gboolean  list_kinds;
	gboolean  help;
	gboolean  version;
	gboolean  license;

	/* quick mode */
	gchar    *kind;
	gchar   **quick_targets;
	gchar    *question;
	gchar    *expect;
	gchar    *system;
	gint      max_tokens;
	gboolean  stream;
	gchar   **remaining;
} BenchmarkOptions;

typedef struct
{
	GMainLoop *loop;
	gboolean   ok;
	GError    *error;
	guint      done;
	guint      total;
	gboolean   quiet;
} BenchmarkRun;

static inline void
benchmark_options_clear(
	BenchmarkOptions *options
){
	g_strfreev(options->select);
	g_strfreev(options->sets);
	g_free(options->format);
	g_free(options->output);
	g_free(options->kind);
	g_strfreev(options->quick_targets);
	g_free(options->question);
	g_free(options->expect);
	g_free(options->system);
	g_strfreev(options->remaining);
}

static inline const gchar *
benchmark_help_description(
	void
){
	return
		"DESCRIPTION\n"
		"  Runs the same work against several providers or models and compares\n"
		"  latency, accuracy against labelled answers, calibration, error rate\n"
		"  and token use. Calls run one at a time, round-robin across targets, so\n"
		"  drift in a shared GPU or network lands on every target equally. Each\n"
		"  target first makes --warmup unrecorded calls (a local model's first call\n"
		"  loads its weights). Kinds: chat (optionally streamed), decision (TypeSafe\n"
		"  Jev, Ollama decision models, Laya), embedding and image.\n"
		"\n"
		"METRICS\n"
		"  runs/err   measured calls, and how many failed or timed out\n"
		"  accuracy   passed / scored; an error is never a \"no\", and a case with\n"
		"             no expectation is unscored\n"
		"  p50/p95    nearest-rank latency percentiles of successful calls, ms\n"
		"  mean       mean latency of successful calls, ms\n"
		"  ttft       mean time to the first streamed text, ms (--stream only)\n"
		"  tokens     reported input/output tokens\n"
		"  in tok/s   input tokens over wall time; the rate decision models are\n"
		"             compared on, since they score rather than generate\n"
		"  out tok/s  output tokens over wall time; - when nothing was generated\n"
		"  brier      calibration of probabilistic answers: 0 is perfect, always\n"
		"             answering 0.5 scores 0.25\n"
		"  -          not available (never shown as 0)\n"
		"\n"
		"EXAMPLES\n"
		"  # Jev vs two local Ollama decision models, from a labelled suite\n"
		"  ai benchmark docs/examples/benchmark-decisions.yaml\n"
		"\n"
		"  # same, 10 iterations, only two of its targets, CSV for pandas\n"
		"  ai benchmark decisions.yaml -n 10 -t jev -t tev1 --format csv -o out.csv\n"
		"\n"
		"  # validate a suite and print the call plan without calling anything\n"
		"  ai benchmark decisions.yaml --dry-run\n"
		"\n"
		"  # one-off decision comparison, no suite file\n"
		"  ai benchmark --kind decision -T typesafe:jev-latest -T ollama:tev1 \\\n"
		"      -q 'Is this spam?' --expect true 'Win a free prize now'\n"
		"\n"
		"  # chat latency and time to first token, streamed\n"
		"  ai benchmark --kind chat --stream -T ollama:llama3.2 \\\n"
		"      -T claude:claude-haiku-5-5 'Say hi'\n"
		"\n"
		"  # CLI wrappers, with a provider knob applied to every target\n"
		"  ai benchmark --kind chat -T claude-code:sonnet -T grok-build \\\n"
		"      --set effort-level=low --expect paris 'Capital of France?'\n"
		"\n"
		"  # embedding latency between two local models\n"
		"  ai benchmark --kind embedding -T ollama:nomic-embed-text \\\n"
		"      -T ollama:mxbai-embed-large 'first passage' 'second passage'\n"
		"\n"
		"  # image generation latency: one call each, no warmup\n"
		"  ai benchmark --kind image -T openai -T grok -n 1 --warmup 0 'a red fox'\n"
		"\n"
		"  # results as YAML, piped into yq\n"
		"  ai benchmark suite.yaml --format yaml | yq '.summaries[] | [.target, .p95_ms]'\n"
		"\n"
		"  # which providers can run which kinds\n"
		"  ai benchmark --list-kinds\n"
		"\n"
		"EXIT STATUS\n"
		"  0    the run completed (individual calls may still have failed)\n"
		"  1    nothing could run, or every measured call failed\n"
		"  2    bad command line or suite file\n"
		"  130  interrupted with Ctrl+C; the partial result is still printed\n"
		"\n"
		"ENVIRONMENT\n"
		"  TYPESAFE_API_KEY, TYPESAFE_BASE_URL   TypeSafe Jev (typesafe, jev)\n"
		"  OLLAMA_HOST, OLLAMA_API_KEY           Ollama chat, embedding and decisions\n"
		"  LAYA_BASE_URL, LAYA_API_KEY           Laya, unless the target sets base-url\n"
		"  ANTHROPIC_API_KEY, OPENAI_API_KEY ... every chat provider, as for `ai`\n"
		"\n"
		"SEE ALSO\n"
		"  docs/benchmark.org (suite file reference, metric definitions),\n"
		"  ai decide --help, docs/decisions.org\n";
}

/*
 * `-T provider[:model]` -> a target. The label is the argument itself, so
 * two Ollama models stay distinguishable in the table.
 */
static inline AiBenchmarkTarget *
benchmark_quick_target(
	const gchar      *spec,
	AiBenchmarkKind   kind,
	gboolean          stream
){
	g_autofree gchar *provider = NULL;
	const gchar *colon;
	const gchar *model;
	AiBenchmarkTarget *target;

	colon = strchr(spec, ':');
	provider = colon != NULL ? g_strndup(spec, (gsize)(colon - spec)) : g_strdup(spec);
	model = colon != NULL ? colon + 1 : NULL;
	target = ai_benchmark_target_new(spec, kind, provider, model);
	ai_benchmark_target_set_stream(target, stream);
	return target;
}

/*
 * Builds the single case of quick mode from --question/--expect/--system
 * and the positional arguments.
 */
static inline AiBenchmarkCase *
benchmark_quick_case(
	BenchmarkOptions  *options,
	AiBenchmarkKind    kind,
	GError           **error
){
	g_autoptr(AiBenchmarkCase) bench_case = NULL;
	guint count;

	count = options->remaining != NULL ? g_strv_length(options->remaining) : 0;
	if (count == 0)
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		                    "quick mode needs the input as an argument (a prompt, the text to decide about, "
		                    "or the texts to embed)");
		return NULL;
	}
	if (kind != AI_BENCHMARK_KIND_EMBEDDING && count > 1)
	{
		g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		            "a %s case takes one argument; quote it", ai_benchmark_kind_to_string(kind));
		return NULL;
	}
	if (options->question != NULL && kind != AI_BENCHMARK_KIND_DECISION)
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		                    "--question applies to --kind decision");
		return NULL;
	}

	switch (kind)
	{
		case AI_BENCHMARK_KIND_CHAT:
			bench_case = ai_benchmark_case_new_chat("prompt", options->remaining[0]);
			ai_benchmark_case_set_system(bench_case, options->system);
			if (options->max_tokens > 0)
				ai_benchmark_case_set_max_tokens(bench_case, options->max_tokens);
			if (options->expect != NULL)
				ai_benchmark_case_expect_contains(bench_case, options->expect);
			break;
		case AI_BENCHMARK_KIND_DECISION:
		{
			g_autoptr(AiDecisionRequest) request = NULL;

			if (options->question == NULL)
			{
				g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
				                    "--kind decision needs --question");
				return NULL;
			}
			request = ai_decision_request_new(options->remaining[0]);
			if (!ai_decision_request_add_boolean(request, "answer", options->question, error))
				return NULL;
			bench_case = ai_benchmark_case_new_decision("answer", request);
			if (options->expect != NULL &&
			    !ai_benchmark_case_expect_answer(bench_case, "answer", options->expect, error))
				return NULL;
			break;
		}
		case AI_BENCHMARK_KIND_EMBEDDING:
			bench_case = ai_benchmark_case_new_embedding("texts", (const gchar *const *)options->remaining);
			if (options->expect != NULL)
			{
				g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
				                    "--expect is not available for embedding in quick mode; use a suite");
				return NULL;
			}
			break;
		case AI_BENCHMARK_KIND_IMAGE:
			bench_case = ai_benchmark_case_new_image("image", options->remaining[0]);
			if (options->expect != NULL)
			{
				g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
				                    "image runs are never scored; drop --expect");
				return NULL;
			}
			break;
		default:
			break;
	}
	return (AiBenchmarkCase *)g_steal_pointer(&bench_case);
}

/*
 * Builds the benchmark from either a suite file or the quick-mode flags,
 * then applies the command-line overrides.
 */
static inline AiBenchmark *
benchmark_from_options(
	BenchmarkOptions  *options,
	GError           **error
){
	g_autoptr(AiBenchmark) bench = NULL;
	GPtrArray *targets;
	guint i;

	if (options->quick_targets != NULL)
	{
		g_autoptr(AiBenchmarkCase) bench_case = NULL;
		AiBenchmarkKind kind;

		/* Quick mode: --kind, -T..., one case from the arguments. */
		if (options->kind == NULL)
			kind = options->question != NULL ? AI_BENCHMARK_KIND_DECISION : AI_BENCHMARK_KIND_CHAT;
		else if (!ai_benchmark_kind_from_string(options->kind, &kind))
		{
			g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
			            "unknown --kind '%s' (chat, decision, embedding or image)", options->kind);
			return NULL;
		}
		if (options->stream && kind != AI_BENCHMARK_KIND_CHAT)
		{
			g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
			                    "--stream applies to --kind chat");
			return NULL;
		}

		bench = ai_benchmark_new();
		for (i = 0; options->quick_targets[i] != NULL; i++)
		{
			g_autoptr(AiBenchmarkTarget) target = benchmark_quick_target(options->quick_targets[i], kind,
			                                                             options->stream);

			if (!ai_benchmark_add_target(bench, target, error))
				return NULL;
		}
		bench_case = benchmark_quick_case(options, kind, error);
		if (bench_case == NULL || !ai_benchmark_add_case(bench, bench_case, error))
			return NULL;
	}
	else
	{
		/* Suite mode: exactly one positional argument, the file. */
		if (options->remaining == NULL || options->remaining[1] != NULL)
		{
			g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
			                    "give one suite file, or use -T for quick mode (see --help)");
			return NULL;
		}
		if (options->kind != NULL || options->question != NULL || options->expect != NULL ||
		    options->system != NULL || options->stream || options->max_tokens > 0)
		{
			g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
			                    "--kind, --question, --expect, --system, --max-tokens and --stream are quick-mode "
			                    "options; put them in the suite file instead");
			return NULL;
		}
		bench = ai_benchmark_new_from_file(options->remaining[0], error);
		if (bench == NULL)
			return NULL;
		if (options->select != NULL &&
		    !ai_benchmark_select_targets(bench, (const gchar *const *)options->select, error))
			return NULL;
	}

	/* Command-line settings win over the suite's. */
	if (options->iterations > 0)
		ai_benchmark_set_iterations(bench, (guint)options->iterations);
	if (options->warmup != BENCHMARK_UNSET)
		ai_benchmark_set_warmup(bench, (guint)options->warmup);
	if (options->timeout_ms != BENCHMARK_UNSET)
		ai_benchmark_set_timeout_ms(bench, (guint)options->timeout_ms);

	/*
	 * --set applies to every target. LAYA_BASE_URL and LAYA_API_KEY are a
	 * frontend's to read (AiLayaClient does not), exactly as `ai decide`
	 * reads them; a target's own base-url wins.
	 */
	targets = ai_benchmark_get_targets(bench);
	for (i = 0; i < targets->len; i++)
	{
		AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(targets, i);
		guint k;

		if (ai_benchmark_target_get_kind(target) == AI_BENCHMARK_KIND_DECISION &&
		    g_strcmp0(ai_benchmark_target_get_provider(target), "laya") == 0)
		{
			const gchar *url = g_getenv("LAYA_BASE_URL");
			const gchar *key = g_getenv("LAYA_API_KEY");

			if (url != NULL && *url != '\0' && !ai_benchmark_target_has_property(target, "base-url"))
			{
				g_autofree gchar *assignment = g_strdup_printf("base-url=%s", url);

				ai_benchmark_target_add_property(target, assignment, NULL);
			}
			if (key != NULL && *key != '\0' && !ai_benchmark_target_has_property(target, "api-key"))
			{
				g_autofree gchar *assignment = g_strdup_printf("api-key=%s", key);

				ai_benchmark_target_add_property(target, assignment, NULL);
			}
		}
		for (k = 0; options->sets != NULL && options->sets[k] != NULL; k++)
			if (!ai_benchmark_target_add_property(target, options->sets[k], error))
				return NULL;
	}
	return (AiBenchmark *)g_steal_pointer(&bench);
}

/*
 * One line of output per row: every provider name the factories know and
 * whether it can run each kind. Built from the objects themselves, so a
 * provider that gains an interface shows up here with no edit.
 */
static inline void
benchmark_print_kinds(
	void
){
	g_autoptr(GEnumClass) klass = (GEnumClass *)g_type_class_ref(AI_TYPE_PROVIDER_TYPE);
	static const gchar *const deciders[] = { "typesafe", "laya", NULL };
	guint i;

	g_print("%-18s %-5s %-7s %-9s %-10s %s\n", "provider", "chat", "stream", "decision", "embedding", "image");
	for (i = 0; i < klass->n_values; i++)
	{
		const gchar *name = klass->values[i].value_nick;
		g_autoptr(GObject) object = ai_provider_factory_new_from_string(name, NULL, NULL);
		g_autoptr(GObject) decider = ai_provider_factory_new_decider(name, NULL, NULL);

		if (object == NULL)
			continue;
		g_print("%-18s %-5s %-7s %-9s %-10s %s\n", name,
		        AI_IS_PROVIDER(object) ? "yes" : "-",
		        AI_IS_STREAMABLE(object) ? "yes" : "-",
		        decider != NULL && AI_IS_DECIDER(decider) ? "yes" : "-",
		        AI_IS_EMBEDDER(object) ? "yes" : "-",
		        AI_IS_IMAGE_GENERATOR(object) ? "yes" : "-");
	}
	for (i = 0; deciders[i] != NULL; i++)
		g_print("%-18s %-5s %-7s %-9s %-10s %s\n", deciders[i], "-", "-", "yes", "-", "-");
	g_print("%-18s %-5s %-7s %-9s %-10s %s\n", "jev", "-", "-", "yes", "-", "-");
	g_print("\njev is the typesafe client. Decision targets need --kind decision (or kind: decision).\n");
}

static inline void
benchmark_on_sample(
	AiBenchmark       *bench,
	AiBenchmarkSample *sample,
	gpointer           user_data
){
	BenchmarkRun *run;
	const gchar *outcome;

	(void)bench;
	run = (BenchmarkRun *)user_data;
	run->done++;
	if (run->quiet)
		return;

	outcome = ai_benchmark_outcome_to_string(ai_benchmark_sample_get_outcome(sample));
	g_printerr("[%u/%u] %s / %s #%u: %.1f ms %s%s%s\n", run->done, run->total,
	           ai_benchmark_sample_get_target(sample), ai_benchmark_sample_get_case_id(sample),
	           ai_benchmark_sample_get_iteration(sample) + 1,
	           (gdouble)ai_benchmark_sample_get_elapsed_us(sample) / 1000.0, outcome,
	           ai_benchmark_sample_get_error(sample) != NULL ? ": " : "",
	           ai_benchmark_sample_get_error(sample) != NULL ? ai_benchmark_sample_get_error(sample) : "");
}

static inline void
benchmark_on_done(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	BenchmarkRun *run;

	run = (BenchmarkRun *)user_data;
	run->ok = ai_benchmark_run_finish(AI_BENCHMARK(source), result, &run->error);
	g_main_loop_quit(run->loop);
}

static inline gboolean
benchmark_on_interrupt(
	gpointer cancellable
){
	g_cancellable_cancel(G_CANCELLABLE(cancellable));
	return G_SOURCE_CONTINUE;
}

/*
 * Renders the result in the requested format and writes it to stdout or
 * --output.
 */
static inline gboolean
benchmark_write_result(
	AiBenchmark      *bench,
	BenchmarkOptions *options,
	GError          **error
){
	g_autofree gchar *text = NULL;
	const gchar *format;

	format = options->format != NULL ? options->format : "table";
	if (g_str_equal(format, "table"))
		text = ai_benchmark_dup_table(bench, options->per_case);
	else if (g_str_equal(format, "yaml"))
		text = ai_benchmark_dup_yaml(bench);
	else if (g_str_equal(format, "json"))
	{
		g_autofree gchar *json = ai_benchmark_dup_json(bench);

		text = g_strconcat(json, "\n", NULL);
	}
	else
		text = ai_benchmark_dup_csv(bench);

	if (options->output == NULL)
	{
		fputs(text, stdout);
		fflush(stdout);
		return TRUE;
	}
	return g_file_set_contents(options->output, text, -1, error);
}

/*
 * The option table, as a function so tests can walk it: every long name
 * here must appear in --help with a description. Free with g_free().
 */
static inline GOptionEntry *
benchmark_option_entries(
	BenchmarkOptions *options
){
	GOptionEntry entries[] = {
		{ "iterations", 'n', 0, G_OPTION_ARG_INT, &options->iterations,
		  "Measured runs of each case on each target (default: 3, or the suite's)", "N" },
		{ "warmup", 0, 0, G_OPTION_ARG_INT, &options->warmup,
		  "Unrecorded calls per target before measuring (default: 1)", "N" },
		{ "timeout-ms", 0, 0, G_OPTION_ARG_INT, &options->timeout_ms,
		  "Per-call deadline; a slower call is an error (default: none of its own)", "MS" },
		{ "target", 't', 0, G_OPTION_ARG_STRING_ARRAY, &options->select,
		  "Run only this suite target; repeatable", "LABEL" },
		{ "set", 0, 0, G_OPTION_ARG_STRING_ARRAY, &options->sets,
		  "Set a property on every target, as ai --set; repeatable", "NAME=VALUE" },
		{ "format", 'f', 0, G_OPTION_ARG_STRING, &options->format,
		  "Output: table (default), yaml, json or csv", "FORMAT" },
		{ "output", 'o', 0, G_OPTION_ARG_FILENAME, &options->output,
		  "Write the result to FILE instead of stdout", "FILE" },
		{ "per-case", 0, 0, G_OPTION_ARG_NONE, &options->per_case,
		  "Table: one row per target and case, not per target", NULL },
		{ "dry-run", 0, 0, G_OPTION_ARG_NONE, &options->dry_run,
		  "Validate, build every target and print the call plan; call nothing", NULL },
		{ "quiet", 0, 0, G_OPTION_ARG_NONE, &options->quiet,
		  "No per-call progress on stderr", NULL },
		{ "list-kinds", 0, 0, G_OPTION_ARG_NONE, &options->list_kinds,
		  "Show which providers can run which kinds, and exit", NULL },
		{ "kind", 'k', 0, G_OPTION_ARG_STRING, &options->kind,
		  "Quick mode: chat, decision, embedding or image (default: chat)", "KIND" },
		{ "add-target", 'T', 0, G_OPTION_ARG_STRING_ARRAY, &options->quick_targets,
		  "Quick mode: a target as PROVIDER[:MODEL]; repeatable", "PROVIDER[:MODEL]" },
		{ "question", 'q', 0, G_OPTION_ARG_STRING, &options->question,
		  "Quick mode: a yes/no decision question about TEXT", "QUESTION" },
		{ "expect", 'e', 0, G_OPTION_ARG_STRING, &options->expect,
		  "Quick mode: decision answer (true/false), or text a chat reply must contain", "ANSWER" },
		{ "system", 's', 0, G_OPTION_ARG_STRING, &options->system,
		  "Quick mode: chat system prompt", "TEXT" },
		{ "max-tokens", 0, 0, G_OPTION_ARG_INT, &options->max_tokens,
		  "Quick mode: chat output cap (default: 1024)", "N" },
		{ "stream", 0, 0, G_OPTION_ARG_NONE, &options->stream,
		  "Quick mode: stream chat and report time to first token", NULL },
		{ "help", 'h', 0, G_OPTION_ARG_NONE, &options->help, "Show this help", NULL },
		{ "version", 0, 0, G_OPTION_ARG_NONE, &options->version, "Show version", NULL },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &options->license, "Show license", NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &options->remaining, NULL,
		  "SUITE.yaml|INPUT..." },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

	return (GOptionEntry *)g_memdup2(entries, sizeof(entries));
}

/*
 * Parses argv. Returns FALSE on a bad command line.
 */
static inline gboolean
benchmark_options_parse(
	BenchmarkOptions  *options,
	gint               argc,
	gchar            **argv,
	gchar            **help,
	GError           **error
){
	g_autoptr(GOptionContext) context = NULL;
	g_autofree GOptionEntry *entries = benchmark_option_entries(options);

	context = g_option_context_new("- compare providers, models and decision models");
	g_option_context_set_help_enabled(context, FALSE);
	g_option_context_add_main_entries(context, entries, NULL);
	g_option_context_set_summary(context,
		"SYNOPSIS\n"
		"  ai benchmark SUITE.yaml [-n N] [--warmup N] [--timeout-ms MS] [-t LABEL]...\n"
		"               [--format table|yaml|json|csv] [-o FILE] [--per-case] [--dry-run]\n"
		"  ai benchmark --kind KIND -T PROVIDER[:MODEL]... [-q QUESTION] [--expect ANSWER]\n"
		"               [--stream] [--set NAME=VALUE]... INPUT...\n"
		"  ai benchmark --list-kinds");
	g_option_context_set_description(context, benchmark_help_description());

	*help = g_option_context_get_help(context, TRUE, NULL);
	if (!g_option_context_parse(context, &argc, &argv, error))
		return FALSE;

	if (options->format != NULL && !g_str_equal(options->format, "table") &&
	    !g_str_equal(options->format, "yaml") && !g_str_equal(options->format, "json") &&
	    !g_str_equal(options->format, "csv"))
	{
		g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		            "unknown --format '%s' (table, yaml, json or csv)", options->format);
		return FALSE;
	}
	/* Range-check here: the library's setters treat out-of-range as a bug. */
	if (options->iterations < 0 || options->iterations > AI_BENCHMARK_MAX_ITERATIONS)
	{
		g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		            "--iterations must be from 1 to %d", AI_BENCHMARK_MAX_ITERATIONS);
		return FALSE;
	}
	if (options->warmup != BENCHMARK_UNSET &&
	    (options->warmup < 0 || options->warmup > AI_BENCHMARK_MAX_WARMUP))
	{
		g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		            "--warmup must be from 0 to %d", AI_BENCHMARK_MAX_WARMUP);
		return FALSE;
	}
	if (options->timeout_ms != BENCHMARK_UNSET && options->timeout_ms < 0)
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		                    "--timeout-ms must be 0 (none) or more");
		return FALSE;
	}
	if (options->max_tokens < 0)
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		                    "--max-tokens must be positive");
		return FALSE;
	}
	if (options->select != NULL && options->quick_targets != NULL)
	{
		g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
		                    "-t selects suite targets; quick mode takes -T only");
		return FALSE;
	}
	return TRUE;
}

/*
 * `ai benchmark ...`; @argv[0] is "benchmark".
 */
static inline gint
benchmark_cli_main(
	gint    argc,
	gchar **argv
){
	BenchmarkOptions options;
	BenchmarkRun run;
	g_autoptr(AiBenchmark) bench = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GCancellable) cancellable = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autofree gchar *help = NULL;
	GPtrArray *samples;
	guint sigint_id;
	guint measured;
	guint calls;
	guint i;
	gboolean any_success;
	gint status;

	/* Usage lines read "ai benchmark", not the bare program name. */
	g_set_prgname("ai benchmark");
	memset(&options, 0, sizeof(options));
	options.warmup = BENCHMARK_UNSET;
	options.timeout_ms = BENCHMARK_UNSET;

	/* 1. Command line. */
	if (!benchmark_options_parse(&options, argc, argv, &help, &error))
	{
		g_printerr("ai benchmark: %s\nTry 'ai benchmark --help'.\n", error->message);
		benchmark_options_clear(&options);
		return BENCHMARK_EXIT_USAGE;
	}
	if (options.help || (argc <= 1))
	{
		g_print("%s", help);
		benchmark_options_clear(&options);
		return BENCHMARK_EXIT_OK;
	}
	if (options.version || options.license)
	{
		if (options.version)
			g_print("ai-glib %d.%d.%d\n", AI_VERSION_MAJOR, AI_VERSION_MINOR, AI_VERSION_MICRO);
		else
			g_print("ai-glib is licensed under GNU AGPL version 3 or later.\n"
			        "https://www.gnu.org/licenses/agpl-3.0.html\n");
		benchmark_options_clear(&options);
		return BENCHMARK_EXIT_OK;
	}
	if (options.list_kinds)
	{
		benchmark_print_kinds();
		benchmark_options_clear(&options);
		return BENCHMARK_EXIT_OK;
	}

	/* 2. Suite or quick mode. */
	bench = benchmark_from_options(&options, &error);
	if (bench == NULL)
	{
		g_printerr("ai benchmark: %s\n", error->message);
		benchmark_options_clear(&options);
		return BENCHMARK_EXIT_USAGE;
	}

	/* A case -t left with no target of its kind would silently not run. */
	{
		GPtrArray *cases = ai_benchmark_get_cases(bench);
		GPtrArray *targets = ai_benchmark_get_targets(bench);
		guint c;

		for (c = 0; c < cases->len; c++)
		{
			AiBenchmarkCase *bench_case = (AiBenchmarkCase *)g_ptr_array_index(cases, c);
			gboolean matched = FALSE;
			guint t;

			for (t = 0; t < targets->len && !matched; t++)
				matched = ai_benchmark_target_get_kind((AiBenchmarkTarget *)g_ptr_array_index(targets, t))
				          == ai_benchmark_case_get_kind(bench_case);
			if (!matched)
				g_printerr("ai benchmark: note: case '%s' has no selected %s target and will not run\n",
				           ai_benchmark_case_get_id(bench_case),
				           ai_benchmark_kind_to_string(ai_benchmark_case_get_kind(bench_case)));
		}
	}

	/* 3. --dry-run: build everything, print the plan, call nothing. */
	if (options.dry_run)
	{
		GPtrArray *targets = ai_benchmark_get_targets(bench);
		gboolean runnable = ai_benchmark_prepare(bench, &error);

		calls = ai_benchmark_count_calls(bench, &measured);
		g_print("%s%s%u targets, %u cases, %u iterations, %u warmup per target\n",
		        ai_benchmark_get_name(bench) != NULL ? ai_benchmark_get_name(bench) : "",
		        ai_benchmark_get_name(bench) != NULL ? ": " : "",
		        targets->len, ai_benchmark_get_cases(bench)->len,
		        ai_benchmark_get_iterations(bench), ai_benchmark_get_warmup(bench));
		for (i = 0; i < targets->len; i++)
		{
			AiBenchmarkTarget *target = (AiBenchmarkTarget *)g_ptr_array_index(targets, i);
			const gchar *label = ai_benchmark_target_get_label(target);
			const gchar *reason = ai_benchmark_get_target_error(bench, label);
			g_autofree gchar *what = ai_benchmark_target_get_model(target) != NULL
				? g_strdup_printf("%s:%s", ai_benchmark_target_get_provider(target),
				                  ai_benchmark_target_get_model(target))
				: g_strdup(ai_benchmark_target_get_provider(target));

			g_print("  %-20s %-9s %-32s %s\n", label,
			        ai_benchmark_kind_to_string(ai_benchmark_target_get_kind(target)),
			        what, reason != NULL ? reason : "ok");
		}
		g_print("%u calls (%u measured)\n", calls, measured);
		if (!runnable)
			g_printerr("ai benchmark: %s\n", error->message);
		benchmark_options_clear(&options);
		return runnable ? BENCHMARK_EXIT_OK : BENCHMARK_EXIT_FAILED;
	}

	/* 4. Run on the default context; Ctrl+C cancels and keeps what ran. */
	cancellable = g_cancellable_new();
	loop = g_main_loop_new(NULL, FALSE);
	memset(&run, 0, sizeof(run));
	run.loop = loop;
	run.quiet = options.quiet;
	/* Count after building, so a target that cannot run is not counted. */
	ai_benchmark_prepare(bench, NULL);
	calls = ai_benchmark_count_calls(bench, &run.total);
	(void)calls;

	g_signal_connect(bench, "sample", G_CALLBACK(benchmark_on_sample), &run);
	sigint_id = g_unix_signal_add(SIGINT, benchmark_on_interrupt, cancellable);
	ai_benchmark_run_async(bench, cancellable, benchmark_on_done, &run);
	g_main_loop_run(loop);
	g_source_remove(sigint_id);

	/* 5. Report. A cancelled run still prints what it measured. */
	status = BENCHMARK_EXIT_OK;
	if (!run.ok)
	{
		if (g_error_matches(run.error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		{
			g_printerr("ai benchmark: interrupted; printing the %u samples taken\n",
			           ai_benchmark_get_samples(bench)->len);
			status = BENCHMARK_EXIT_INTERRUPTED;
		}
		else
		{
			g_printerr("ai benchmark: %s\n", run.error->message);
			g_clear_error(&run.error);
			benchmark_options_clear(&options);
			return BENCHMARK_EXIT_FAILED;
		}
	}
	g_clear_error(&run.error);

	if (!benchmark_write_result(bench, &options, &error))
	{
		g_printerr("ai benchmark: %s\n", error->message);
		benchmark_options_clear(&options);
		return BENCHMARK_EXIT_FAILED;
	}

	samples = ai_benchmark_get_samples(bench);
	any_success = FALSE;
	for (i = 0; i < samples->len; i++)
		if (ai_benchmark_sample_get_outcome((AiBenchmarkSample *)g_ptr_array_index(samples, i))
		    != AI_BENCHMARK_OUTCOME_ERROR)
			any_success = TRUE;
	if (status == BENCHMARK_EXIT_OK && !any_success)
	{
		g_printerr("ai benchmark: every measured call failed\n");
		status = BENCHMARK_EXIT_FAILED;
	}
	benchmark_options_clear(&options);
	return status;
}
