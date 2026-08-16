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

/*
 * Spawn ai-tui inside a sandbox.
 *
 * @dir becomes both the working directory and HOME, so the harness layer
 * sees only files this test wrote. Without that, the suite would read the
 * developer's own ~/.claude -- sixteen command files on this machine --
 * and a listing test would pass or fail depending on whose laptop it ran
 * on.
 */
static Run *
run_tui_in(const gchar *dir, const gchar * const *args)
{
	Run *run = g_new0(Run, 1);
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) envp = NULL;
	g_autofree gchar *config = g_build_filename(dir, ".config", NULL);
	gsize i;

	g_ptr_array_add(argv, g_strdup(tui_binary));

	for (i = 0; args[i] != NULL; i++)
	{
		g_ptr_array_add(argv, g_strdup(args[i]));
	}

	g_ptr_array_add(argv, NULL);

	envp = g_get_environ();
	envp = g_environ_setenv(envp, "HOME", dir, TRUE);
	envp = g_environ_setenv(envp, "XDG_CONFIG_HOME", config, TRUE);
	envp = g_environ_unsetenv(envp, "ANTHROPIC_API_KEY");
	envp = g_environ_unsetenv(envp, "OPENAI_API_KEY");

	g_spawn_sync(dir, (gchar **)argv->pdata, envp,
	             G_SPAWN_DEFAULT, NULL, NULL,
	             &run->stdout_data, &run->stderr_data, &run->status, &error);

	g_assert_no_error(error);

	return run;
}

/* Write @contents to @relative under @dir, creating directories. */
static void
sandbox_write(const gchar *dir, const gchar *relative, const gchar *contents)
{
	g_autofree gchar *path = g_build_filename(dir, relative, NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(parent, 0755), ==, 0);
	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);
}

static gchar *
sandbox_new(void)
{
	g_autoptr(GError) error = NULL;
	gchar            *dir = g_dir_make_tmp("ai-glib-tui-box-XXXXXX", &error);

	g_assert_no_error(error);

	return dir;
}

static void
sandbox_free(gchar *dir)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", dir);

	g_assert_cmpint(system(cmd), ==, 0);
	g_free(dir);
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

/* ----------------------------------------------------------------
 * Driving the real terminal
 * ----------------------------------------------------------------
 *
 * --dump covers everything that happens after a line is submitted, and
 * nothing about how a line is typed. Since Enter stopped meaning "send",
 * that gap is exactly where a regression would live: a build where ^D
 * quit instead of sending, or where Escape got eaten by the Alt-Enter
 * peek, would pass every test above.
 *
 * So these run ai-tui under tmux, which gives it a real pty and real
 * ncurses, send keys at it, and read the screen back. Skipped when tmux
 * is not installed rather than failed --- it is a test dependency, not a
 * library one.
 */

static gboolean
tmux_available(void)
{
	g_autofree gchar *path = g_find_program_in_path("tmux");

	return path != NULL;
}

/* Run a tmux subcommand, returning its stdout. */
static gchar *
tmux_run(const gchar * const *args)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError)    error = NULL;
	gchar               *out = NULL;
	gsize                i;

	g_ptr_array_add(argv, g_strdup("tmux"));

	for (i = 0; args[i] != NULL; i++)
	{
		g_ptr_array_add(argv, g_strdup(args[i]));
	}

	g_ptr_array_add(argv, NULL);

	g_spawn_sync(NULL, (gchar **)argv->pdata, NULL,
	             G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
	             NULL, NULL, &out, NULL, NULL, &error);
	g_assert_no_error(error);

	return out;
}

static void
tmux_kill(const gchar *session)
{
	const gchar *args[] = { "kill-session", "-t", session, NULL };
	g_autofree gchar *out = tmux_run(args);
}

static void
tmux_send(const gchar *session, const gchar *keys)
{
	const gchar *args[] = { "send-keys", "-t", session, keys, NULL };
	g_autofree gchar *out = tmux_run(args);
}

static gchar *
tmux_capture(const gchar *session)
{
	const gchar *args[] = { "capture-pane", "-t", session, "-p", NULL };

	return tmux_run(args);
}

/*
 * Wait for @needle to appear on the pane.
 *
 * Polling rather than sleeping: a fixed sleep is either too short on a
 * loaded machine or wasted time on an idle one, and the failure mode of
 * "too short" is a flaky test that blames the wrong thing.
 */
static gboolean
tmux_wait_for(const gchar *session, const gchar *needle)
{
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline)
	{
		g_autofree gchar *pane = tmux_capture(session);

		if (pane != NULL && strstr(pane, needle) != NULL)
		{
			return TRUE;
		}

		g_usleep(100 * 1000);
	}

	return FALSE;
}

/*
 * Start ai-tui in a detached tmux session against a stub `grok`.
 *
 * VISUAL is cleared because it beats EDITOR, and a developer with one
 * set would otherwise have their real editor opened by the ^G test ---
 * which is exactly what happened the first time this was tried by hand.
 */
static void
tmux_start_tui(const gchar *session, const gchar *stub_dir,
               const gchar *editor)
{
	g_autofree gchar *grok = g_build_filename(stub_dir, "grok", NULL);
	g_autofree gchar *command = NULL;
	const gchar      *args[] = {
		"new-session", "-d", "-s", session, "-x", "100", "-y", "24",
		NULL, NULL
	};
	g_autofree gchar *out = NULL;
	g_autofree gchar *libdir = g_path_get_dirname(tui_binary);
	g_autofree gchar *libs = g_path_get_dirname(libdir);

	tmux_kill(session);

	command = g_strdup_printf(
		"env -u VISUAL LD_LIBRARY_PATH='%s' GROK_PATH='%s' HOME='%s' "
		"XDG_CONFIG_HOME='%s/.config' EDITOR='%s' '%s' -p grok-build",
		libs, grok, stub_dir, stub_dir,
		editor != NULL ? editor : "true", tui_binary);

	args[8] = command;
	out = tmux_run(args);

	/* The prompt marker is the first thing drawn, so its arrival is the
	 * signal that ncurses is up and reading keys. */
	g_assert_true(tmux_wait_for(session, ">"));
}

/* Write an executable stand-in for $EDITOR into @dir. */
static gchar *
editor_stub(const gchar *dir, const gchar *name, const gchar *body)
{
	gchar            *path = g_build_filename(dir, name, NULL);
	g_autoptr(GError) error = NULL;

	g_file_set_contents(path, body, -1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);

	return path;
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

/* ----------------------------------------------------------------
 * The harness layer, through the binary
 * ---------------------------------------------------------------- */

static void
test_help_lists_commands_from_disk(void)
{
	gchar *box = sandbox_new();
	const gchar *args[] = { "--dump", "/help", "-p", "grok-build", NULL };
	Run *run;

	sandbox_write(box, ".claude/commands/deploy.md",
	              "---\ndescription: Ship it\nargument-hint: <env>\n---\n"
	              "Deploy.\n");

	run = run_tui_in(box, args);

	g_assert_cmpint(run->status, ==, 0);

	/* The file, with its hint, its description and where it came from. */
	g_assert_nonnull(strstr(run->stdout_data, "/deploy <env>"));
	g_assert_nonnull(strstr(run->stdout_data, "Ship it"));
	g_assert_nonnull(strstr(run->stdout_data, "[claude]"));

	/* And the built-ins, which exist before any file is read. */
	g_assert_nonnull(strstr(run->stdout_data, "/quit"));
	g_assert_nonnull(strstr(run->stdout_data, "/clear"));

	run_free(run);
	sandbox_free(box);
}

static void
test_commands_listing_names_the_search_paths(void)
{
	gchar *box = sandbox_new();
	const gchar *args[] = { "--dump", "/commands", "-p", "grok-build", NULL };
	Run *run = run_tui_in(box, args);

	/*
	 * An empty listing is exactly when somebody asks "why isn't my file
	 * showing up", so the answer has to be on screen rather than in the
	 * documentation.
	 */
	g_assert_nonnull(strstr(run->stdout_data, "none found"));
	g_assert_nonnull(strstr(run->stdout_data, ".claude/commands"));
	g_assert_nonnull(strstr(run->stdout_data, ".opencode/command"));

	run_free(run);
	sandbox_free(box);
}

static void
test_expand_shows_a_resolved_command(void)
{
	gchar *box = sandbox_new();
	const gchar *args[] = { "--dump", "/expand /greet Zach", "-p",
	                        "grok-build", NULL };
	Run *run;

	sandbox_write(box, ".claude/commands/greet.md",
	              "---\nname: greet\n---\nGreet $1 warmly.\n");

	run = run_tui_in(box, args);

	/* Resolution and substitution, end to end through the binary, with
	 * nothing sent anywhere. */
	g_assert_nonnull(strstr(run->stdout_data, "Greet Zach warmly."));

	run_free(run);
	sandbox_free(box);
}

static void
test_expand_inlines_a_mention(void)
{
	gchar *box = sandbox_new();
	const gchar *args[] = { "--dump", "/expand explain @hello.c", "-p",
	                        "grok-build", NULL };
	Run *run;

	sandbox_write(box, "hello.c", "int main(void) { return 0; }\n");

	run = run_tui_in(box, args);

	g_assert_nonnull(strstr(run->stdout_data, "Referenced files"));
	g_assert_nonnull(strstr(run->stdout_data, "int main(void)"));

	run_free(run);
	sandbox_free(box);
}

static void
test_expand_reports_an_unknown_command(void)
{
	gchar *box = sandbox_new();
	const gchar *http[] = { "--dump", "/expand /nosuchthing", "-p", "claude",
	                        NULL };
	const gchar *cli[] = { "--dump", "/expand /nosuchthing", "-p",
	                       "grok-build", NULL };
	Run *run;

	/* For an HTTP provider there is nothing downstream that could make
	 * sense of it, so it is an error and the message names near misses. */
	run = run_tui_in(box, http);
	g_assert_nonnull(strstr(run->stdout_data, "unknown command"));
	run_free(run);

	/*
	 * For a CLI provider the same line is not ours to refuse: /compact
	 * and its friends mean something to the wrapped tool. It passes
	 * through untouched.
	 */
	run = run_tui_in(box, cli);
	g_assert_null(strstr(run->stdout_data, "unknown command"));
	g_assert_nonnull(strstr(run->stdout_data, "/nosuchthing"));
	run_free(run);

	sandbox_free(box);
}

static void
test_unknown_builtin_free_line_reaches_the_child(void)
{
	gchar *box = sandbox_new();
	Stub  *stub = stub_new("{\"type\":\"result\",\"text\":\"ok\","
	                       "\"stopReason\":\"end_turn\"}\n");
	g_autofree gchar *stdin_path = NULL;
	g_autofree gchar *sent = NULL;
	const gchar *args[] = { "--dump", "explain @hello.c", "-p", "grok-build",
	                        NULL };
	g_autoptr(GPtrArray) argv = NULL;
	Run *run;
	g_auto(GStrv) envp = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *config = g_build_filename(box, ".config", NULL);
	gsize i;

	sandbox_write(box, "hello.c", "SHOULD_NOT_BE_INLINED\n");

	/* Spawned by hand so the sandbox and GROK_PATH apply together. */
	run = g_new0(Run, 1);
	argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_strdup(tui_binary));

	for (i = 0; args[i] != NULL; i++)
	{
		g_ptr_array_add(argv, g_strdup(args[i]));
	}

	g_ptr_array_add(argv, NULL);

	envp = g_get_environ();
	envp = g_environ_setenv(envp, "HOME", box, TRUE);
	envp = g_environ_setenv(envp, "XDG_CONFIG_HOME", config, TRUE);
	envp = g_environ_setenv(envp, "GROK_PATH", stub->stub, TRUE);

	g_spawn_sync(box, (gchar **)argv->pdata, envp, G_SPAWN_DEFAULT, NULL,
	             NULL, &run->stdout_data, &run->stderr_data, &run->status,
	             &error);
	g_assert_no_error(error);

	stdin_path = g_build_filename(stub->dir, "stdin.log", NULL);
	g_file_get_contents(stdin_path, &sent, NULL, NULL);

	/*
	 * A CLI provider receives the line as typed. grok resolves @ itself,
	 * and inlining the file first would hand it the same content twice
	 * under two different names.
	 */
	g_assert_nonnull(sent);
	g_assert_nonnull(strstr(sent, "@hello.c"));
	g_assert_null(strstr(sent, "SHOULD_NOT_BE_INLINED"));

	run_free(run);
	stub_free(stub);
	sandbox_free(box);
}

static void
test_no_expand_leaves_a_command_alone(void)
{
	gchar *box = sandbox_new();
	const gchar *args[] = { "--no-expand", "--dump", "/help", "-p",
	                        "claude", NULL };
	Run *run = run_tui_in(box, args);

	/*
	 * With --no-expand the line is a prompt, not a command, so /help is
	 * never listed -- it is sent, and fails for want of an API key. The
	 * point is that it did not become a listing.
	 */
	g_assert_null(strstr(run->stdout_data, "Empty the transcript"));

	run_free(run);
	sandbox_free(box);
}

/* ----------------------------------------------------------------
 * Keys, under a real terminal
 * ---------------------------------------------------------------- */

#define TUI_SESSION "ai-glib-tui-test"

/*
 * The reply the stub grok gives, so a send is visible on the pane.
 *
 * ai-tui streams by default, and grok's streaming format is
 * Anthropic-shaped rather than its own camelCase result envelope --- so
 * this is the delta shape, not `{"text": ...}`.
 */
#define STUB_REPLY \
	"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\"," \
	"\"delta\":{\"type\":\"text_delta\",\"text\":\"the reply\"}}}\n" \
	"{\"type\":\"result\",\"result\":\"the reply\",\"session_id\":\"s1\"}\n"

static void
test_enter_inserts_a_newline(void)
{
	g_autofree gchar  *pane = NULL;
	Stub              *stub;
	gchar             *box;

	if (!tmux_available())
	{
		g_test_skip("tmux is not installed");
		return;
	}

	stub = stub_new(STUB_REPLY);
	box = sandbox_new();

	tmux_start_tui(TUI_SESSION, stub->dir, NULL);

	tmux_send(TUI_SESSION, "alpha");
	tmux_send(TUI_SESSION, "Enter");
	tmux_send(TUI_SESSION, "beta");

	/*
	 * Both halves on screen at once is the whole claim: Enter did not
	 * send the first line away, it opened a second.
	 */
	g_assert_true(tmux_wait_for(TUI_SESSION, "beta"));

	pane = tmux_capture(TUI_SESSION);

	g_assert_nonnull(strstr(pane, "alpha"));
	g_assert_nonnull(strstr(pane, "beta"));

	/* And the second row carries the continuation marker, which only
	 * exists when the input grew past one row. */
	g_assert_nonnull(strstr(pane, "\342\224\202"));   /* │ */

	/* Nothing was sent: the stub's reply is nowhere. */
	g_assert_null(strstr(pane, "the reply"));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

static void
test_ctrl_d_sends_the_prompt(void)
{
	Stub  *stub;
	gchar *box;

	if (!tmux_available())
	{
		g_test_skip("tmux is not installed");
		return;
	}

	stub = stub_new(STUB_REPLY);
	box = sandbox_new();

	tmux_start_tui(TUI_SESSION, stub->dir, NULL);

	tmux_send(TUI_SESSION, "ask something");
	g_assert_true(tmux_wait_for(TUI_SESSION, "ask something"));

	tmux_send(TUI_SESSION, "C-d");

	/* The stub's answer arriving proves the turn actually went. */
	g_assert_true(tmux_wait_for(TUI_SESSION, "the reply"));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

/*
 * Escape survived the Alt-Enter peek.
 *
 * Alt-Enter arrives as ESC followed by a carriage return, so the handler
 * reads one key ahead to tell it from a bare Escape. Get that wrong and
 * Escape either stops working or swallows whatever was typed after it.
 */
static void
test_escape_still_dismisses_the_menu(void)
{
	g_autofree gchar  *pane = NULL;
	Stub              *stub;
	gchar             *box;

	if (!tmux_available())
	{
		g_test_skip("tmux is not installed");
		return;
	}

	stub = stub_new(STUB_REPLY);
	box = sandbox_new();

	sandbox_write(box, ".claude/commands/deploy.md",
	              "---\ndescription: Ship it\n---\nDeploy.\n");

	tmux_start_tui(TUI_SESSION, stub->dir, NULL);

	tmux_send(TUI_SESSION, "/dep");
	g_assert_true(tmux_wait_for(TUI_SESSION, "/dep"));

	tmux_send(TUI_SESSION, "Escape");
	g_usleep(500 * 1000);

	pane = tmux_capture(TUI_SESSION);

	/* The line is untouched --- Escape dismissed a menu, not the text. */
	g_assert_nonnull(strstr(pane, "/dep"));

	/* And a key typed after it still lands, which is what a swallowed
	 * peek would break. */
	tmux_send(TUI_SESSION, "loy");
	g_assert_true(tmux_wait_for(TUI_SESSION, "/deploy"));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

static void
test_ctrl_g_round_trips_through_the_editor(void)
{
	g_autofree gchar  *pane = NULL;
	Stub              *stub;
	gchar             *box;
	g_autofree gchar  *editor = NULL;

	if (!tmux_available())
	{
		g_test_skip("tmux is not installed");
		return;
	}

	stub = stub_new(STUB_REPLY);
	box = sandbox_new();

	editor = editor_stub(box, "editor.sh",
	                     "#!/bin/sh\n"
	                     "printf 'from the editor\\nand a second line\\n' "
	                     "> \"$1\"\n");

	tmux_start_tui(TUI_SESSION, stub->dir, editor);

	tmux_send(TUI_SESSION, "typed by hand");
	g_assert_true(tmux_wait_for(TUI_SESSION, "typed by hand"));

	tmux_send(TUI_SESSION, "C-g");

	g_assert_true(tmux_wait_for(TUI_SESSION, "from the editor"));

	pane = tmux_capture(TUI_SESSION);

	/* What the editor wrote replaced what was typed, newlines and all. */
	g_assert_nonnull(strstr(pane, "and a second line"));
	g_assert_null(strstr(pane, "typed by hand"));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

/*
 * Quitting the editor without saving leaves the prompt alone.
 *
 * A non-zero exit is how somebody says they changed their mind, and the
 * file on disk may be a half-finished draft. Reading it back anyway
 * would make the cancel do the opposite of cancelling --- so the stub
 * here writes something *and* fails, and the something must not appear.
 */
static void
test_an_aborted_edit_keeps_the_prompt(void)
{
	g_autofree gchar  *pane = NULL;
	Stub              *stub;
	gchar             *box;
	g_autofree gchar  *editor = NULL;

	if (!tmux_available())
	{
		g_test_skip("tmux is not installed");
		return;
	}

	stub = stub_new(STUB_REPLY);
	box = sandbox_new();

	editor = editor_stub(box, "abort.sh",
	                     "#!/bin/sh\n"
	                     "printf 'must be ignored\\n' > \"$1\"\n"
	                     "exit 1\n");

	tmux_start_tui(TUI_SESSION, stub->dir, editor);

	tmux_send(TUI_SESSION, "keep me");
	g_assert_true(tmux_wait_for(TUI_SESSION, "keep me"));

	tmux_send(TUI_SESSION, "C-g");

	g_assert_true(tmux_wait_for(TUI_SESSION, "unchanged"));

	pane = tmux_capture(TUI_SESSION);

	g_assert_nonnull(strstr(pane, "keep me"));
	g_assert_null(strstr(pane, "must be ignored"));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	tui_binary = find_tui_binary(argv[0]);

	/*
	 * Absolute, because the sandboxed runs below spawn the binary from a
	 * different working directory and a relative path would resolve
	 * against theirs.
	 */
	if (tui_binary != NULL && !g_path_is_absolute(tui_binary))
	{
		g_autofree gchar *cwd = g_get_current_dir();
		gchar            *absolute = g_canonicalize_filename(tui_binary, cwd);

		g_free(tui_binary);
		tui_binary = absolute;
	}

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

	g_test_add_func("/ai-glib/ai-tui/help-lists-files",
	                test_help_lists_commands_from_disk);
	g_test_add_func("/ai-glib/ai-tui/commands-search-paths",
	                test_commands_listing_names_the_search_paths);
	g_test_add_func("/ai-glib/ai-tui/expand-command",
	                test_expand_shows_a_resolved_command);
	g_test_add_func("/ai-glib/ai-tui/expand-mention",
	                test_expand_inlines_a_mention);
	g_test_add_func("/ai-glib/ai-tui/expand-unknown",
	                test_expand_reports_an_unknown_command);
	g_test_add_func("/ai-glib/ai-tui/passthrough",
	                test_unknown_builtin_free_line_reaches_the_child);
	g_test_add_func("/ai-glib/ai-tui/no-expand",
	                test_no_expand_leaves_a_command_alone);

	g_test_add_func("/ai-glib/ai-tui/keys/enter-is-a-newline",
	                test_enter_inserts_a_newline);
	g_test_add_func("/ai-glib/ai-tui/keys/ctrl-d-sends",
	                test_ctrl_d_sends_the_prompt);
	g_test_add_func("/ai-glib/ai-tui/keys/escape-still-dismisses",
	                test_escape_still_dismisses_the_menu);
	g_test_add_func("/ai-glib/ai-tui/keys/editor-round-trip",
	                test_ctrl_g_round_trips_through_the_editor);
	g_test_add_func("/ai-glib/ai-tui/keys/editor-abort",
	                test_an_aborted_edit_keeps_the_prompt);

	return g_test_run();
}
