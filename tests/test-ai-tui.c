/*
 * test-ai-tui.c - The ai-tui binary, driven without a terminal
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A TUI is awkward to test, which is why --dump exists: one turn, no
 * terminal, transcript printed. That makes the whole path -- argv, provider
 * construction, the NDJSON parser, the event stream, the folding, and the
 * summariser -- testable end to end against a stub `grok`, with no network
 * and no tty.
 *
 * The test that matters most here is the one asserting a grouped summary
 * line appears in that output. Everything below it is unit-tested in
 * isolation elsewhere; this is the proof the pieces are wired together.
 */

#include <glib.h>
#include <glib/gstdio.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	gchar *stdout_data;
	gchar *stderr_data;
	gint   status;
} Run;

static gchar *tui_binary = NULL;

static void
run_free(Run *run)
{
	g_free(run->stdout_data);
	g_free(run->stderr_data);
	g_free(run);
}

/* Spawn ai-tui with @argv (after the binary), optionally with extra env. */
static Run *
run_tui(const gchar * const *args, const gchar *env_key, const gchar *env_value)
{
	Run *run = g_new0(Run, 1);
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) envp = NULL;
	gsize i;

	g_ptr_array_add(argv, g_strdup(tui_binary));

	for (i = 0; args[i] != NULL; i++)
	{
		g_ptr_array_add(argv, g_strdup(args[i]));
	}

	g_ptr_array_add(argv, NULL);

	envp = g_get_environ();

	if (env_key != NULL)
	{
		envp = g_environ_setenv(envp, env_key, env_value, TRUE);
	}

	/*
	 * No API keys, ever: a test that reached the network would be slow,
	 * flaky, and would spend somebody's money.
	 */
	envp = g_environ_unsetenv(envp, "ANTHROPIC_API_KEY");
	envp = g_environ_unsetenv(envp, "OPENAI_API_KEY");

	g_spawn_sync(NULL, (gchar **)argv->pdata, envp,
	             G_SPAWN_DEFAULT, NULL, NULL,
	             &run->stdout_data, &run->stderr_data, &run->status, &error);

	g_assert_no_error(error);

	return run;
}

/* A stub `grok` that replays a canned NDJSON session. */
typedef struct
{
	gchar *dir;
	gchar *stub;
} Stub;

#define STUB_TEMPLATE                                      \
	"#!/bin/sh\n"                                      \
	"cat > \"%s/stdin.log\"\n"                         \
	"cat \"%s/stdout\"\n"

static Stub *
stub_new(const gchar *ndjson)
{
	Stub *stub = g_new0(Stub, 1);
	g_autofree gchar *script = NULL;
	g_autofree gchar *out_path = NULL;
	g_autoptr(GError) error = NULL;

	stub->dir = g_dir_make_tmp("ai-glib-tui-XXXXXX", &error);
	g_assert_no_error(error);

	stub->stub = g_build_filename(stub->dir, "grok", NULL);
	script = g_strdup_printf(STUB_TEMPLATE, stub->dir, stub->dir);

	g_file_set_contents(stub->stub, script, -1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(stub->stub, 0700), ==, 0);

	out_path = g_build_filename(stub->dir, "stdout", NULL);
	g_file_set_contents(out_path, ndjson, -1, &error);
	g_assert_no_error(error);

	return stub;
}

static void
stub_free(Stub *stub)
{
	const gchar *names[] = { "grok", "stdout", "stdin.log", NULL };
	gsize i;

	for (i = 0; names[i] != NULL; i++)
	{
		g_autofree gchar *path = g_build_filename(stub->dir, names[i], NULL);
		g_remove(path);
	}

	g_rmdir(stub->dir);
	g_free(stub->dir);
	g_free(stub->stub);
	g_free(stub);
}

/* ----------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------- */

static void
test_version(void)
{
	const gchar *args[] = { "--version", NULL };
	Run *run = run_tui(args, NULL, NULL);

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(g_str_has_prefix(run->stdout_data, "ai-tui "));

	run_free(run);
}

static void
test_license(void)
{
	const gchar *args[] = { "--license", NULL };
	Run *run = run_tui(args, NULL, NULL);

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(strstr(run->stdout_data, "AGPL-3.0") != NULL);

	run_free(run);
}

static void
test_help(void)
{
	const gchar *args[] = { "--help", NULL };
	Run *run = run_tui(args, NULL, NULL);

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(strstr(run->stdout_data, "--dump") != NULL);
	g_assert_true(strstr(run->stdout_data, "--provider") != NULL);

	run_free(run);
}

static void
test_unknown_provider_is_an_error(void)
{
	/*
	 * ai_provider_type_from_string() answers CLAUDE for anything it does
	 * not know, so an unknown name would silently become Claude. A caller
	 * who asked for something else deserves to be told.
	 */
	const gchar *args[] = { "-p", "definitely-not-a-provider", "--dry-run", NULL };
	Run *run = run_tui(args, NULL, NULL);

	g_assert_cmpint(run->status, !=, 0);
	g_assert_true(strstr(run->stderr_data, "unknown provider") != NULL);

	run_free(run);
}

static void
test_dry_run_cli_provider(void)
{
	const gchar *args[] = { "-p", "grok-build", "--dry-run", NULL };
	Stub *stub = stub_new("");
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_cmpint(run->status, ==, 0);

	/* The command grok would actually be given. */
	g_assert_true(strstr(run->stdout_data, "--prompt-file /dev/stdin") != NULL);
	g_assert_true(strstr(run->stdout_data, stub->stub) != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_dry_run_http_provider_says_so(void)
{
	const gchar *args[] = { "-p", "claude", "--dry-run", NULL };
	Run *run = run_tui(args, NULL, NULL);

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(strstr(run->stdout_data, "HTTP provider") != NULL);

	run_free(run);
}

static void
test_dry_run_honours_model(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "-m", "grok-4.5", "--dry-run", NULL
	};
	Stub *stub = stub_new("");
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_true(strstr(run->stdout_data, "grok-4.5") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_dry_run_honours_set(void)
{
	/* --set reaches any provider property, with no flag of its own. */
	const gchar *args[] = {
		"-p", "grok-build", "--set", "sandbox=true", "--dry-run", NULL
	};
	Stub *stub = stub_new("");
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(strstr(run->stdout_data, "--sandbox") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_unknown_set_property_is_an_error(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--set", "no-such-knob=1", "--dry-run", NULL
	};
	Stub *stub = stub_new("");
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_cmpint(run->status, !=, 0);
	g_assert_true(strstr(run->stderr_data, "no property") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_unparseable_set_value_is_an_error(void)
{
	/* A value that does not parse must not become a silent zero. */
	const gchar *args[] = {
		"-p", "grok-build", "--set", "max-turns=lots", "--dry-run", NULL
	};
	Stub *stub = stub_new("");
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_cmpint(run->status, !=, 0);
	g_assert_true(strstr(run->stderr_data, "cannot parse") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_dump_prints_the_transcript(void)
{
	const gchar *ndjson =
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello from the model.\"}}}\n"
		"{\"type\":\"result\",\"result\":\"Hello from the model.\","
		"\"session_id\":\"s1\"}\n";
	const gchar *args[] = { "-p", "grok-build", "--dump", "say hello", NULL };
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_cmpint(run->status, ==, 0);

	/* The user turn and the reply, in order. */
	g_assert_true(strstr(run->stdout_data, "> say hello") != NULL);
	g_assert_true(strstr(run->stdout_data, "Hello from the model.") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_dump_shows_the_grouped_tool_summary(void)
{
	/*
	 * The end-to-end proof, and the reason this file exists.
	 *
	 * A real grok NDJSON session goes in; the summariser's grouped line
	 * comes out. Everything between -- the parser, the event stream, the
	 * folding rules, the tool-style table, the diff derivation -- has to be
	 * right for this to pass, and none of it is stubbed.
	 */
	const gchar *ndjson =
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"Now the codec registry.\"}}}\n"
		"{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\","
		"\"id\":\"t1\",\"name\":\"Write\",\"input\":{\"file_path\":\"a.c\","
		"\"content\":\"x\\ny\\nz\"}}]}}\n"
		"{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\","
		"\"id\":\"t2\",\"name\":\"Write\",\"input\":{\"file_path\":\"b.c\","
		"\"content\":\"p\\nq\"}}]}}\n"
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"Cleaning up.\"}}}\n"
		"{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\","
		"\"id\":\"t3\",\"name\":\"Bash\",\"input\":{\"command\":\"make\"}}]}}\n"
		"{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\","
		"\"id\":\"t4\",\"name\":\"Bash\",\"input\":{\"command\":\"ls\"}}]}}\n"
		"{\"type\":\"result\",\"result\":\"Done.\",\"session_id\":\"s1\","
		"\"usage\":{\"input_tokens\":100,\"output_tokens\":20},"
		"\"total_cost_usd\":0.01}\n";
	const gchar *args[] = { "-p", "grok-build", "--dump", "build it", NULL };
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui(args, "GROK_PATH", stub->stub);
	const gchar *first_group;
	const gchar *second_group;

	g_assert_cmpint(run->status, ==, 0);

	/* Two writes collapsed into one line, with their combined diff. */
	first_group = strstr(run->stdout_data, "Created 2 files  +5-0");
	g_assert_nonnull(first_group);

	/* Two commands, grouped separately because prose came between. */
	second_group = strstr(run->stdout_data, "Ran 2 commands");
	g_assert_nonnull(second_group);

	/* The narration between them, in the right order. */
	g_assert_true(strstr(run->stdout_data, "Now the codec registry.") <
	              first_group);
	g_assert_true(first_group < strstr(run->stdout_data, "Cleaning up."));
	g_assert_true(strstr(run->stdout_data, "Cleaning up.") < second_group);

	/* And the turn's cost, which grok is one of the few to report. */
	g_assert_true(strstr(run->stdout_data, "100 in / 20 out") != NULL);
	g_assert_true(strstr(run->stdout_data, "$0.0100") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_dump_reaches_the_child(void)
{
	/* The prompt is piped, so a run against an empty stdin would "succeed". */
	const gchar *ndjson = "{\"type\":\"result\",\"result\":\"ok\"}\n";
	const gchar *args[] = { "-p", "grok-build", "--dump", "the prompt", NULL };
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui(args, "GROK_PATH", stub->stub);
	g_autofree gchar *path = g_build_filename(stub->dir, "stdin.log", NULL);
	g_autofree gchar *seen = NULL;

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(g_file_get_contents(path, &seen, NULL, NULL));
	g_assert_true(strstr(seen, "the prompt") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_dump_width_wraps(void)
{
	const gchar *ndjson =
		"{\"type\":\"result\",\"result\":\"one two three four five six seven\"}\n";
	const gchar *args[] = {
		"-p", "grok-build", "--width", "12", "--dump", "wrap it", NULL
	};
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui(args, "GROK_PATH", stub->stub);
	g_auto(GStrv) lines = NULL;
	gsize i;

	g_assert_cmpint(run->status, ==, 0);

	lines = g_strsplit(run->stdout_data, "\n", -1);

	for (i = 0; lines[i] != NULL; i++)
	{
		g_assert_cmpuint(g_utf8_strlen(lines[i], -1), <=, 12);
	}

	run_free(run);
	stub_free(stub);
}

static void
test_dump_reports_a_failing_provider(void)
{
	/*
	 * grok prints this on stdout and exits 0, so the transcript is the only
	 * place a user would ever see it.
	 */
	const gchar *ndjson =
		"{\"type\":\"error\",\"message\":\"unknown reasoning effort\"}\n";
	const gchar *args[] = { "-p", "grok-build", "--dump", "go", NULL };
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_true(strstr(run->stdout_data, "unknown reasoning effort") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_interactive_without_a_tty_errors_cleanly(void)
{
	/*
	 * g_spawn_sync gives the child a pipe, not a terminal. Starting
	 * ncurses on that would leave the caller's terminal in an odd state
	 * for no benefit, so it refuses and says what to use instead.
	 */
	const gchar *args[] = { "-p", "grok-build", NULL };
	Stub *stub = stub_new("");
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	g_assert_cmpint(run->status, !=, 0);
	g_assert_true(strstr(run->stderr_data, "not a terminal") != NULL);
	g_assert_true(strstr(run->stderr_data, "--dump") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_local_tools_declined_for_cli_provider(void)
{
	const gchar *ndjson = "{\"type\":\"result\",\"result\":\"ok\"}\n";
	const gchar *args[] = {
		"-p", "grok-build", "--local-tools", "--dump", "go", NULL
	};
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui(args, "GROK_PATH", stub->stub);

	/* Said out loud rather than silently ignored. */
	g_assert_true(strstr(run->stderr_data, "runs its own tools") != NULL);

	run_free(run);
	stub_free(stub);
}

/* Locate the built binary relative to argv[0], as test-ai-cli.c does. */
static gchar *
find_tui_binary(const gchar *argv0)
{
	g_autofree gchar *dir = g_path_get_dirname(argv0);
	g_autofree gchar *candidate =
		g_build_filename(dir, "..", "bin", "ai-tui", NULL);

	if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
	{
		return g_steal_pointer(&candidate);
	}

	return NULL;
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	tui_binary = find_tui_binary(argv[0]);

	if (tui_binary == NULL)
	{
		/*
		 * ai-tui is skipped when ncursesw is absent, so its tests are too
		 * -- a machine without a terminal library should still get a green
		 * suite for everything that does build.
		 */
		g_print("# ai-tui not built (ncursesw missing?); skipping\n");
		return 0;
	}

	g_test_add_func("/ai-glib/ai-tui/version", test_version);
	g_test_add_func("/ai-glib/ai-tui/license", test_license);
	g_test_add_func("/ai-glib/ai-tui/help", test_help);
	g_test_add_func("/ai-glib/ai-tui/unknown-provider",
	                test_unknown_provider_is_an_error);
	g_test_add_func("/ai-glib/ai-tui/dry-run", test_dry_run_cli_provider);
	g_test_add_func("/ai-glib/ai-tui/dry-run-http",
	                test_dry_run_http_provider_says_so);
	g_test_add_func("/ai-glib/ai-tui/dry-run-model", test_dry_run_honours_model);
	g_test_add_func("/ai-glib/ai-tui/dry-run-set", test_dry_run_honours_set);
	g_test_add_func("/ai-glib/ai-tui/set-unknown",
	                test_unknown_set_property_is_an_error);
	g_test_add_func("/ai-glib/ai-tui/set-unparseable",
	                test_unparseable_set_value_is_an_error);
	g_test_add_func("/ai-glib/ai-tui/dump", test_dump_prints_the_transcript);
	g_test_add_func("/ai-glib/ai-tui/dump-grouped-summary",
	                test_dump_shows_the_grouped_tool_summary);
	g_test_add_func("/ai-glib/ai-tui/dump-prompt-reaches-child",
	                test_dump_reaches_the_child);
	g_test_add_func("/ai-glib/ai-tui/dump-width", test_dump_width_wraps);
	g_test_add_func("/ai-glib/ai-tui/dump-error",
	                test_dump_reports_a_failing_provider);
	g_test_add_func("/ai-glib/ai-tui/no-tty",
	                test_interactive_without_a_tty_errors_cleanly);
	g_test_add_func("/ai-glib/ai-tui/local-tools-declined",
	                test_local_tools_declined_for_cli_provider);

	return g_test_run();
}
