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
#include <gio/gio.h>
#include <unistd.h>

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
static gchar *tmux_socket = NULL;

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

/*
 * Spawn ai-tui with @stdin_text on its stdin.
 *
 * g_spawn_sync attaches stdin to /dev/null, which is how the no-tty
 * tests work; this is the pipe-a-prompt path, matching `ai`.
 */
static Run *
run_tui_feed(const gchar * const *args, const gchar *stdin_text,
             const gchar *env_key, const gchar *env_value)
{
	Run *run = g_new0(Run, 1);
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	gsize i;

	g_ptr_array_add(argv, tui_binary);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	if (env_key != NULL)
		g_subprocess_launcher_setenv(launcher, env_key, env_value, TRUE);
	g_subprocess_launcher_unsetenv(launcher, "ANTHROPIC_API_KEY");
	g_subprocess_launcher_unsetenv(launcher, "OPENAI_API_KEY");

	proc = g_subprocess_launcher_spawnv(
		launcher, (const gchar * const *)argv->pdata, &error);
	g_assert_no_error(error);

	g_subprocess_communicate_utf8(proc, stdin_text, NULL,
	                              &run->stdout_data, &run->stderr_data,
	                              &error);
	g_assert_no_error(error);
	run->status = g_subprocess_get_exit_status(proc);
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
	g_ptr_array_add(argv, g_strdup("-L"));
	g_ptr_array_add(argv, g_strdup(tmux_socket));
	g_ptr_array_add(argv, g_strdup("-f"));
	g_ptr_array_add(argv, g_strdup("/dev/null"));

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
 * Is the session still there?
 *
 * tmux ends a session when the command in it exits, so this is how
 * "ai-tui quit" is observed --- and, more to the point, how "ai-tui did
 * not quit" is.
 */
static gboolean
tmux_alive(const gchar *session)
{
	const gchar *args[] = { "list-sessions", "-F", "#{session_name}", NULL };
	g_autofree gchar *out = tmux_run(args);
	g_auto(GStrv)     names = NULL;
	gsize             i;

	if (out == NULL)
	{
		return FALSE;
	}

	names = g_strsplit(out, "\n", -1);

	for (i = 0; names[i] != NULL; i++)
	{
		if (g_strcmp0(names[i], session) == 0)
		{
			return TRUE;
		}
	}

	return FALSE;
}

static gboolean
tmux_wait_for_exit(const gchar *session)
{
	gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;

	while (g_get_monotonic_time() < deadline)
	{
		if (!tmux_alive(session))
		{
			return TRUE;
		}

		g_usleep(100 * 1000);
	}

	return FALSE;
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

/* Resizes and theme changes are observed on the real rendered screen. */
static void
tmux_resize(const gchar *width, const gchar *height)
{
	const gchar *args[] = { "resize-window", "-x", width, "-y", height, NULL };
	g_autofree gchar *out = tmux_run(args);
}

/*
 * Start ai-tui in a detached tmux session against a stub `grok`.
 *
 * VISUAL is cleared because it beats EDITOR, and a developer with one
 * set would otherwise have their real editor opened by the ^G test ---
 * which is exactly what happened the first time this was tried by hand.
 */
static void
tmux_start_tui_with_options(const gchar *session, const gchar *stub_dir,
                           const gchar *editor, const gchar *environment,
                           const gchar *options)
{
	g_autofree gchar *grok = g_build_filename(stub_dir, "grok", NULL);
	g_autofree gchar *command = NULL;
	const gchar      *args[] = {
		"new-session", "-d", "-s", session, "-x", "100", "-y", "24",
		"-c", stub_dir, "/bin/bash", "--noprofile", "--norc", "-c", NULL, NULL
	};
	g_autofree gchar *out = NULL;
	g_autofree gchar *libdir = g_path_get_dirname(tui_binary);
	g_autofree gchar *libs = g_path_get_dirname(libdir);

	tmux_kill(session);

	command = g_strdup_printf(
		"exec env -u VISUAL -u NO_COLOR -u AI_TUI_THEME TERM=xterm-256color LC_ALL=C.UTF-8 LD_LIBRARY_PATH='%s' GROK_PATH='%s' HOME='%s' "
		"XDG_CONFIG_HOME='%s/.config' EDITOR='%s' %s '%s' -p grok-build %s",
		libs, grok, stub_dir, stub_dir,
		editor != NULL ? editor : "true", environment != NULL ? environment : "",
		tui_binary, options != NULL ? options : "");

	args[14] = command;
	out = tmux_run(args);

	/* The prompt marker is the first thing drawn, so its arrival is the
	 * signal that ncurses is up and reading keys. */
	g_assert_true(tmux_wait_for(session, "COMPOSE"));
}

static void
tmux_start_tui(const gchar *session, const gchar *stub_dir, const gchar *editor)
{
	tmux_start_tui_with_options(session, stub_dir, editor, NULL, NULL);
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
	g_assert_true(strstr(run->stdout_data, "[PROMPT]") != NULL);

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
test_positional_prompt_without_a_tty_runs_one_shot(void)
{
	const gchar *ndjson = "{\"type\":\"result\",\"result\":\"ok\"}\n";
	const gchar *args[] = { "-p", "grok-build", "the prompt", NULL };
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
test_prompt_from_stdin_runs_one_shot(void)
{
	const gchar *ndjson =
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"piped\"}}}\n"
		"{\"type\":\"result\",\"result\":\"piped\",\"session_id\":\"s1\"}\n";
	const gchar *args[] = { "-p", "grok-build", NULL };
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui_feed(args, "hello from stdin\n",
	                       "GROK_PATH", stub->stub);
	g_autofree gchar *path = g_build_filename(stub->dir, "stdin.log", NULL);
	g_autofree gchar *seen = NULL;

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(strstr(run->stdout_data, "> hello from stdin") != NULL);
	g_assert_true(strstr(run->stdout_data, "piped") != NULL);
	g_assert_true(g_file_get_contents(path, &seen, NULL, NULL));
	g_assert_true(strstr(seen, "hello from stdin") != NULL);

	run_free(run);
	stub_free(stub);
}

static void
test_positional_prompt_wins_over_stdin(void)
{
	const gchar *ndjson = "{\"type\":\"result\",\"result\":\"ok\"}\n";
	const gchar *args[] = { "-p", "grok-build", "from argv", NULL };
	Stub *stub = stub_new(ndjson);
	Run *run = run_tui_feed(args, "from stdin\n", "GROK_PATH", stub->stub);
	g_autofree gchar *path = g_build_filename(stub->dir, "stdin.log", NULL);
	g_autofree gchar *seen = NULL;

	g_assert_cmpint(run->status, ==, 0);
	g_assert_true(g_file_get_contents(path, &seen, NULL, NULL));
	g_assert_true(strstr(seen, "from argv") != NULL);
	g_assert_null(strstr(seen, "from stdin"));

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
test_provider_command_shows_and_switches(void)
{
	gchar *box = sandbox_new();
	const gchar *show[] = {
		"--dump", "/provider", "-p", "grok-build", NULL
	};
	const gchar *change[] = {
		"--dump", "/provider cursor", "-p", "grok-build", NULL
	};
	Run *run;

	run = run_tui_in(box, show);
	g_assert_cmpint(run->status, ==, 0);
	g_assert_nonnull(strstr(run->stdout_data, "Provider: Grok Build"));
	run_free(run);

	run = run_tui_in(box, change);
	g_assert_cmpint(run->status, ==, 0);
	g_assert_nonnull(strstr(run->stdout_data,
	                        "Provider switched to Cursor"));
	g_assert_nonnull(strstr(run->stdout_data, "Context preserved"));
	run_free(run);

	sandbox_free(box);
}

static void
test_provider_command_failure_keeps_current(void)
{
	gchar *box = sandbox_new();
	const gchar *args[] = {
		"--dump", "/provider not-real", "-p", "grok-build", NULL
	};
	Run *run = run_tui_in(box, args);

	g_assert_cmpint(run->status, ==, 0);
	g_assert_nonnull(strstr(run->stdout_data, "Provider unchanged"));
	g_assert_nonnull(strstr(run->stdout_data, "unknown provider"));

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
test_alt_enter_inserts_a_newline(void)
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
	tmux_send(TUI_SESSION, "M-Enter");
	tmux_send(TUI_SESSION, "beta");

	/*
	 * Both halves on screen at once is the whole claim: Alt-Enter did not
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
test_enter_sends_the_prompt(void)
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

	tmux_send(TUI_SESSION, "Enter");

	/* The stub's answer arriving proves the turn actually went. */
	g_assert_true(tmux_wait_for(TUI_SESSION, "the reply"));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

static void
test_positional_prompt_sends_in_the_tui(void)
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

	tmux_start_tui_with_options(TUI_SESSION, stub->dir, NULL, NULL,
	                            "ask something");

	/* Leftover argv is the first turn: no typing, no Enter. */
	g_assert_true(tmux_wait_for(TUI_SESSION, "the reply"));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

/*
 * One ^C throws the line away and stays.
 *
 * ^C had never reached the key handler at all: cbreak() leaves ISIG on,
 * so the terminal raised SIGINT and the program died --- which is what
 * this asserts against. The session still being there afterwards is the
 * whole point.
 */
static void
test_one_interrupt_clears_and_stays(void)
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

	tmux_send(TUI_SESSION, "a half written thought");
	g_assert_true(tmux_wait_for(TUI_SESSION, "half written"));

	tmux_send(TUI_SESSION, "C-c");

	/* The armed hint appearing proves the line went and the program did
	 * not. */
	g_assert_true(tmux_wait_for(TUI_SESSION, "again to quit"));

	pane = tmux_capture(TUI_SESSION);
	g_assert_null(strstr(pane, "half written"));

	g_assert_true(tmux_alive(TUI_SESSION));

	tmux_kill(TUI_SESSION);
	stub_free(stub);
	sandbox_free(box);
}

/*
 * Two, close together, leave.
 *
 * The gap has to be well inside INTERRUPT_WINDOW_MS or the first press
 * disarms itself and the second merely re-arms --- which is the
 * behaviour being bought, and also the way this test goes wrong if the
 * machine is loaded.
 */
static void
test_two_interrupts_quit(void)
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

	tmux_send(TUI_SESSION, "C-c");
	g_assert_true(tmux_wait_for(TUI_SESSION, "again to quit"));

	tmux_send(TUI_SESSION, "C-c");

	g_assert_true(tmux_wait_for_exit(TUI_SESSION));

	stub_free(stub);
	sandbox_free(box);
}

/*
 * And a lone one, left to expire, does not.
 *
 * Without the timer the flag would simply stay set, so a ^C now and
 * another one ten minutes later would quit --- which is the accident the
 * whole arrangement exists to prevent.
 */
static void
test_an_expired_interrupt_does_not_quit(void)
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

	tmux_send(TUI_SESSION, "C-c");
	g_assert_true(tmux_wait_for(TUI_SESSION, "again to quit"));

	/* Comfortably past the window. */
	g_usleep(2500 * 1000);

	tmux_send(TUI_SESSION, "C-c");
	g_usleep(500 * 1000);

	g_assert_true(tmux_alive(TUI_SESSION));

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

/* Listing and validation must not need provider credentials or a terminal. */
static void
test_theme_options(void)
{
	const gchar *list[] = { "--list-themes", "-p", "invalid-provider", NULL };
	const gchar *bad[] = { "--theme", "not-a-theme", "--dry-run", NULL };
	const gchar *override[] = { "--theme", "nord", "--dry-run", "-p", "grok-build", NULL };
	Run *run = run_tui(list, NULL, NULL);

	g_assert_cmpint(run->status, ==, 0);
	g_assert_nonnull(strstr(run->stdout_data, "catppuccin-mocha (default)"));
	g_assert_nonnull(strstr(run->stdout_data, "catppuccin-latte"));
	g_assert_nonnull(strstr(run->stdout_data, "monochrome"));
	run_free(run);
	run = run_tui(bad, NULL, NULL);
	g_assert_cmpint(run->status, !=, 0);
	g_assert_nonnull(strstr(run->stderr_data, "unknown theme"));
	run_free(run);
	run = run_tui(override, "AI_TUI_THEME", "invalid-environment-theme");
	g_assert_cmpint(run->status, ==, 0);
	run_free(run);
}

static void
test_themes_and_resizing(void)
{
	Stub *stub;
	guint i;
	const gchar *names[] = { "catppuccin-latte", "nord", "terminal", "monochrome", "catppuccin-mocha" };
	const gchar *capture[] = { "capture-pane", "-t", TUI_SESSION, "-p", "-e", NULL };
	g_autofree gchar *pane = NULL;

	if (!tmux_available()) { g_test_skip("tmux is not installed"); return; }
	stub = stub_new(STUB_REPLY);
	tmux_start_tui(TUI_SESSION, stub->dir, NULL);
	tmux_resize("120", "36");
	g_assert_true(tmux_wait_for(TUI_SESSION, "SESSION"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "catppuccin-mocha"));
	pane = tmux_run(capture);
	g_assert_nonnull(strstr(pane, "38;5;"));
	g_assert_nonnull(strstr(pane, "48;5;"));
	g_test_message("Mocha 120x36:\n%s", pane);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		tmux_send(TUI_SESSION, "C-t");
		g_assert_true(tmux_wait_for(TUI_SESSION, names[i]));
		g_clear_pointer(&pane, g_free);
		pane = tmux_run(capture);
		if (g_str_equal(names[i], "monochrome"))
		{
			g_assert_null(strstr(pane, "38;5;"));
			g_assert_null(strstr(pane, "48;5;"));
		}
	}
	tmux_send(TUI_SESSION, "C-p");
	g_usleep(150000);
	g_clear_pointer(&pane, g_free);
	pane = tmux_capture(TUI_SESSION);
	g_assert_null(strstr(pane, "SESSION"));
	tmux_send(TUI_SESSION, "C-p");
	g_assert_true(tmux_wait_for(TUI_SESSION, "SESSION"));
	tmux_send(TUI_SESSION, "retained-draft");
	tmux_resize("40", "10");
	g_assert_true(tmux_wait_for(TUI_SESSION, "retained-draft"));
	g_clear_pointer(&pane, g_free);
	pane = tmux_capture(TUI_SESSION);
	g_assert_null(strstr(pane, "SESSION"));
	g_test_message("Narrow 40x10:\n%s", pane);
	tmux_resize("20", "5");
	g_assert_true(tmux_wait_for(TUI_SESSION, "resize"));
	tmux_resize("120", "36");
	g_assert_true(tmux_wait_for(TUI_SESSION, "SESSION"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "retained-draft"));
	tmux_kill(TUI_SESSION);
	stub_free(stub);
}

/* Unicode must survive the entire input path, including code points whose
 * numeric values collide with ncurses KEY_* constants. */
static void
test_unicode_and_search(void)
{
	Stub *stub;
	g_autofree gchar *pane = NULL;
	g_autofree gchar *stdin_path = NULL;
	g_autofree gchar *input = NULL;
	const gchar *text = "café 中文 λ é";

	if (!tmux_available()) { g_test_skip("tmux is not installed"); return; }
	stub = stub_new(STUB_REPLY);
	tmux_start_tui(TUI_SESSION, stub->dir, NULL);
	tmux_send(TUI_SESSION, text);
	g_assert_true(tmux_wait_for(TUI_SESSION, "café 中文 λ"));
	tmux_send(TUI_SESSION, "Enter");
	g_assert_true(tmux_wait_for(TUI_SESSION, "the reply"));
	stdin_path = g_build_filename(stub->dir, "stdin.log", NULL);
	g_assert_true(g_file_get_contents(stdin_path, &input, NULL, NULL));
	g_assert_nonnull(strstr(input, text));
	tmux_send(TUI_SESSION, "preserved-draft");
	tmux_send(TUI_SESSION, "C-f");
	tmux_send(TUI_SESSION, "the reply");
	g_assert_true(tmux_wait_for(TUI_SESSION, "1 matching rows"));
	tmux_send(TUI_SESSION, "Enter");
	tmux_send(TUI_SESSION, "Up");
	tmux_send(TUI_SESSION, "C-u");
	tmux_send(TUI_SESSION, "no-such-needle");
	g_assert_true(tmux_wait_for(TUI_SESSION, "No matches"));
	tmux_send(TUI_SESSION, "Escape");
	g_assert_true(tmux_wait_for(TUI_SESSION, "preserved-draft"));
	tmux_send(TUI_SESSION, "Up");
	tmux_send(TUI_SESSION, "Down");
	g_assert_true(tmux_wait_for(TUI_SESSION, "preserved-draft"));
	tmux_send(TUI_SESSION, "C-l");
	g_usleep(150000);
	pane = tmux_capture(TUI_SESSION);
	g_assert_null(strstr(pane, "[scrolled]"));
	tmux_kill(TUI_SESSION);
	stub_free(stub);
}

static void
test_long_bracketed_paste(void)
{
	Stub *stub;
	g_autoptr(GString) paste = g_string_new(NULL);
	g_autofree gchar *pane = NULL;
	guint i;
	const gchar *set[] = { "set-buffer", "--", NULL, NULL };
	const gchar *send[] = { "paste-buffer", "-p", "-t", TUI_SESSION, NULL };
	g_autofree gchar *out = NULL;

	if (!tmux_available()) { g_test_skip("tmux is not installed"); return; }
	stub = stub_new(STUB_REPLY);
	tmux_start_tui(TUI_SESSION, stub->dir, NULL);
	for (i = 0; i < 18; i++) g_string_append_printf(paste, "draft-line-%02u\n", i);
	g_string_append(paste, "final-draft-line");
	set[2] = paste->str;
	out = tmux_run(set);
	g_clear_pointer(&out, g_free);
	out = tmux_run(send);
	g_assert_true(tmux_wait_for(TUI_SESSION, "final-draft-line"));
	pane = tmux_capture(TUI_SESSION);
	g_assert_null(strstr(pane, "the reply"));
	g_assert_null(strstr(pane, "draft-line-00"));
	tmux_send(TUI_SESSION, "C-a");
	g_assert_true(tmux_wait_for(TUI_SESSION, "draft-line-00"));
	tmux_send(TUI_SESSION, "C-e");
	g_assert_true(tmux_wait_for(TUI_SESSION, "final-draft-line"));
	tmux_send(TUI_SESSION, "Enter");
	g_assert_true(tmux_wait_for(TUI_SESSION, "the reply"));
	tmux_kill(TUI_SESSION);
	stub_free(stub);
}

/* NO_COLOR and low-color terminfo are exercised against the styled
 * positive control above, not by simply trusting absence of escapes. */
static void
test_theme_fallbacks(void)
{
	static const struct {
		const gchar *environment;
		const gchar *options;
		const gchar *name;
		gboolean extended;
	} cases[] = {
		{ "AI_TUI_THEME=nord", "", "nord", TRUE },
		{ "NO_COLOR=1 AI_TUI_THEME=nord", "", "nord", FALSE },
		{ "NO_COLOR=1 AI_TUI_THEME=nord", "--theme catppuccin-mocha", "catppuccin-mocha", TRUE },
		{ "TERM=xterm", "--theme catppuccin-mocha", "catppuccin-mocha", FALSE },
		{ "TERM=vt100", "--theme catppuccin-mocha", "catppuccin-mocha", FALSE }
	};
	guint i;
	if (!tmux_available()) { g_test_skip("tmux is not installed"); return; }
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		Stub *stub = stub_new(STUB_REPLY);
		const gchar *capture[] = { "capture-pane", "-t", TUI_SESSION, "-p", "-e", NULL };
		g_autofree gchar *pane = NULL;
		tmux_start_tui_with_options(TUI_SESSION, stub->dir, NULL, cases[i].environment, cases[i].options);
		g_assert_true(tmux_wait_for(TUI_SESSION, cases[i].name));
		pane = tmux_run(capture);
		g_assert_cmpint(strstr(pane, "38;5;") != NULL, ==, cases[i].extended);
		g_assert_cmpint(strstr(pane, "48;5;") != NULL, ==, cases[i].extended);
		tmux_kill(TUI_SESSION);
		stub_free(stub);
	}
}

static void
test_busy_keeps_draft(void)
{
	Stub *stub;
	g_autofree gchar *script = NULL;
	g_autofree gchar *pane = NULL;
	if (!tmux_available()) { g_test_skip("tmux is not installed"); return; }
	stub = stub_new(STUB_REPLY);
	script = g_strdup_printf("#!/bin/sh\ncat > '%s/stdin.log'\n"
		"while [ ! -f '%s/release' ]; do sleep 0.05; done\ncat '%s/stdout'\n",
		stub->dir, stub->dir, stub->dir);
	sandbox_write(stub->dir, "grok", script);
	tmux_start_tui_with_options(TUI_SESSION, stub->dir, NULL, NULL, "--no-animation");
	tmux_send(TUI_SESSION, "first request");
	tmux_send(TUI_SESSION, "Enter");
	g_assert_true(tmux_wait_for(TUI_SESSION, "DRAFT / waiting"));
	tmux_send(TUI_SESSION, "second-draft");
	tmux_send(TUI_SESSION, "Enter");
	g_usleep(150000);
	g_assert_true(tmux_wait_for(TUI_SESSION, "second-draft"));
	pane = tmux_capture(TUI_SESSION);
	g_assert_null(strstr(pane, "the reply"));
	sandbox_write(stub->dir, "release", "ready\n");
	g_assert_true(tmux_wait_for(TUI_SESSION, "the reply"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "second-draft"));
	tmux_kill(TUI_SESSION);
	stub_free(stub);
}

/* Ctrl shortcuts work with a live draft, a menu, and a search field. The
 * old function keys are deliberately unbound, not hidden aliases. */
static void
test_control_shortcuts(void)
{
	Stub *stub;
	g_autofree gchar *pane = NULL;
	guint i;
	const gchar *old_keys[] = { "F1", "F2", "F3", "F4" };

	if (!tmux_available()) { g_test_skip("tmux is not installed"); return; }
	stub = stub_new(STUB_REPLY);
	tmux_start_tui_with_options(TUI_SESSION, stub->dir, NULL, NULL, "--no-animation");
	tmux_resize("120", "36");
	g_assert_true(tmux_wait_for(TUI_SESSION, "SESSION"));
	for (i = 0; i < G_N_ELEMENTS(old_keys); i++) tmux_send(TUI_SESSION, old_keys[i]);
	g_usleep(150000);
	pane = tmux_capture(TUI_SESSION);
	g_assert_nonnull(strstr(pane, "catppuccin-mocha"));
	g_assert_nonnull(strstr(pane, "MAKE SOMETHING WORTH SHIPPING."));
	g_assert_nonnull(strstr(pane, "SESSION"));
	tmux_send(TUI_SESSION, "/pro");
	g_assert_true(tmux_wait_for(TUI_SESSION, "Show or change the provider"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "COMMANDS"));
	tmux_send(TUI_SESSION, "C-o");
	g_assert_true(tmux_wait_for(TUI_SESSION, "cycle theme"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "COMPOSE"));
	tmux_send(TUI_SESSION, "C-f");
	tmux_send(TUI_SESSION, "cycle theme");
	g_assert_true(tmux_wait_for(TUI_SESSION, "1 matching rows"));
	tmux_send(TUI_SESSION, "C-t");
	g_assert_true(tmux_wait_for(TUI_SESSION, "catppuccin-latte"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "SEARCH / case sensitive"));
	tmux_send(TUI_SESSION, "C-p");
	g_assert_true(tmux_wait_for(TUI_SESSION, "Panel hidden"));
	tmux_send(TUI_SESSION, "C-l");
	g_assert_true(tmux_wait_for(TUI_SESSION, "Following latest output"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "COMPOSE"));
	g_clear_pointer(&pane, g_free);
	pane = tmux_capture(TUI_SESSION);
	g_assert_null(strstr(pane, "SEARCH / case sensitive"));
	g_assert_null(strstr(pane, "[scrolled]"));
	g_assert_nonnull(strstr(pane, "/pro"));
	tmux_kill(TUI_SESSION);
	stub_free(stub);
}

/* Compare only the border: elapsed time should still advance in reduced
 * motion mode, so comparing whole busy screens would give a false failure. */
static gchar *
composer_border(void)
{
	g_autofree gchar *pane = tmux_capture(TUI_SESSION);
	const gchar *start = strstr(pane, "╭");
	const gchar *end;

	g_assert_nonnull(start);
	end = strchr(start, '\n');
	g_assert_nonnull(end);
	return g_strndup(start, (gsize)(end - start));
}

static void
test_activity_motion(gconstpointer data)
{
	gboolean reduced = GPOINTER_TO_INT(data);
	Stub *stub;
	g_autofree gchar *script = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *next = NULL;
	g_autofree gchar *idle = NULL;
	gint64 deadline;
	gboolean changed = FALSE;

	if (!tmux_available()) { g_test_skip("tmux is not installed"); return; }
	stub = stub_new(STUB_REPLY);
	script = g_strdup_printf("#!/bin/sh\ncat > '%s/stdin.log'\n"
		"while [ ! -f '%s/release' ]; do sleep 0.05; done\ncat '%s/stdout'\n",
		stub->dir, stub->dir, stub->dir);
	sandbox_write(stub->dir, "grok", script);
	tmux_start_tui_with_options(TUI_SESSION, stub->dir, NULL, NULL,
		reduced ? "--no-animation" : "");
	tmux_send(TUI_SESSION, "watch the border");
	tmux_send(TUI_SESSION, "Enter");
	g_assert_true(tmux_wait_for(TUI_SESSION, "DRAFT / waiting"));
	first = composer_border();
	if (reduced) g_assert_null(strstr(first, "━"));
	else g_assert_nonnull(strstr(first, "━"));
	deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
	do
	{
		g_usleep(150000);
		g_clear_pointer(&next, g_free);
		next = composer_border();
		changed = g_strcmp0(first, next) != 0;
	} while (!changed && g_get_monotonic_time() < deadline);
	g_assert_cmpint(changed, ==, !reduced);
	sandbox_write(stub->dir, "release", "ready\n");
	g_assert_true(tmux_wait_for(TUI_SESSION, "Turn complete"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "ready"));
	g_clear_pointer(&first, g_free);
	first = composer_border();
	g_assert_null(strstr(first, "━"));
	idle = tmux_capture(TUI_SESSION);
	g_usleep(250000);
	g_clear_pointer(&next, g_free);
	next = tmux_capture(TUI_SESSION);
	g_assert_cmpstr(idle, ==, next);
	tmux_kill(TUI_SESSION);
	stub_free(stub);
}

#define PREVIEW_REPLY \
	"{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"edit\",\"name\":\"Edit\",\"input\":{\"file_path\":\"demo.c\",\"old_string\":\"static int answer(void) { return 1; }\",\"new_string\":\"static int answer(void) { return 2027; }\"}}]}}\n" \
	"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"edit\",\"content\":[{\"type\":\"text\",\"text\":\"Applied\"}]}]}}\n" \
	"{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"build\",\"name\":\"Bash\",\"input\":{\"command\":\"make -j4\"}}]}}\n" \
	"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"build\",\"content\":\"build-line-0\\nbuild-line-1\\nbuild-line-2\\nbuild-line-3\\nbuild-line-4\\nbuild-line-5\\nbuild-line-6\"}]}}\n" \
	"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"Preview complete\"}}}\n" \
	"{\"type\":\"result\",\"result\":\"Preview complete\",\"session_id\":\"s1\"}\n"

/* Read SGR foreground state at a token rather than accepting any colored
 * header as evidence that the source itself is syntax-highlighted. */
static gint
foreground_at(const gchar *capture, const gchar *needle)
{
	const gchar *at = strstr(capture, needle), *p = capture;
	gint foreground = -1;
	g_assert_nonnull(at);
	while ((p = strstr(p, "\033[")) != NULL && p < at)
	{
		const gchar *end = strchr(p, 'm');
		g_autofree gchar *sequence = NULL;
		g_auto(GStrv) fields = NULL;
		guint i;
		g_assert_nonnull(end);
		sequence = g_strndup(p + 2, (gsize)(end - p - 2));
		fields = g_strsplit(sequence, ";", -1);
		for (i = 0; fields[i] != NULL; i++)
		{
			gint value = (gint)g_ascii_strtoll(fields[i], NULL, 10);
			if ((value == 38 || value == 48) && fields[i + 1] != NULL &&
				g_str_equal(fields[i + 1], "5") && fields[i + 2] != NULL)
			{
				if (value == 38) foreground = (gint)g_ascii_strtoll(fields[i + 2], NULL, 10);
				i += 2;
			}
			else if (value == 0 || value == 39) foreground = -1;
			else if (value >= 30 && value <= 37) foreground = value - 30;
		}
		p = end + 1;
	}
	return foreground;
}

static void
test_inline_tool_previews(void)
{
	Stub *stub = stub_new(PREVIEW_REPLY);
	g_autofree gchar *property = g_strconcat("executable-path=", stub->stub, NULL);
	const gchar *args[] = { "-p", "grok-build", "--set", property, "--dump", "render previews", NULL };
	Run *run = run_tui_in(stub->dir, args);
	const gchar *capture[] = { "capture-pane", "-t", TUI_SESSION, "-p", "-e", NULL };
	g_autofree gchar *pane = NULL;

	g_assert_cmpint(run->status, ==, 0);
	g_assert_nonnull(strstr(run->stdout_data, "return 1;"));
	g_assert_nonnull(strstr(run->stdout_data, "return 2027;"));
	g_assert_nonnull(strstr(run->stdout_data, "$ make -j4"));
	g_assert_nonnull(strstr(run->stdout_data, "build-line-5"));
	g_assert_null(strstr(run->stdout_data, "build-line-6"));
	g_assert_null(strstr(run->stdout_data, "not confirmed successful"));
	g_assert_null(strchr(run->stdout_data, '\033'));
	run_free(run);
	if (!tmux_available()) { stub_free(stub); g_test_skip("tmux is not installed; dump assertions passed"); return; }
	tmux_start_tui_with_options(TUI_SESSION, stub->dir, NULL, NULL, "--no-animation");
	tmux_resize("150", "44");
	tmux_send(TUI_SESSION, "render previews");
	tmux_send(TUI_SESSION, "Enter");
	g_assert_true(tmux_wait_for(TUI_SESSION, "build-line-5"));
	g_assert_true(tmux_wait_for(TUI_SESSION, "Preview complete"));
	pane = tmux_run(capture);
	g_assert_cmpint(foreground_at(pane, "return"), ==, 183); /* Mocha mauve */
	g_assert_cmpint(foreground_at(pane, "answer"), ==, 111); /* Mocha blue */
	g_assert_cmpint(foreground_at(pane, "2027"), ==, 216); /* Mocha peach */
	g_assert_cmpint(foreground_at(pane, "+     "), ==, 151); /* addition green */
	g_assert_cmpint(foreground_at(pane, "-    1"), ==, 211); /* removal red */
	g_test_message("Inline tool previews:\n%s", pane);
	tmux_send(TUI_SESSION, "C-t");
	g_assert_true(tmux_wait_for(TUI_SESSION, "catppuccin-latte"));
	g_clear_pointer(&pane, g_free);
	pane = tmux_run(capture);
	g_assert_cmpint(foreground_at(pane, "return"), !=, 183);
	tmux_send(TUI_SESSION, "C-n");
	tmux_send(TUI_SESSION, "C-b");
	g_assert_true(tmux_wait_for(TUI_SESSION, "build-line-6"));
	tmux_kill(TUI_SESSION);
	stub_free(stub);
}

int
main(int argc, char *argv[])
{
	gint status;

	g_test_init(&argc, &argv, NULL);
	/* Never inherit the developer's tmux options or touch their sessions. */
	tmux_socket = g_strdup_printf("ai-tui-test-%u", (guint)getpid());

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
	g_test_add_func("/ai-glib/ai-tui/themes", test_theme_options);
	g_test_add_func("/ai-glib/ai-tui/inline-tool-previews", test_inline_tool_previews);
	g_test_add_func("/ai-glib/ai-tui/keys/themes-resize", test_themes_and_resizing);
	g_test_add_func("/ai-glib/ai-tui/keys/unicode-search", test_unicode_and_search);
	g_test_add_func("/ai-glib/ai-tui/keys/long-paste", test_long_bracketed_paste);
	g_test_add_func("/ai-glib/ai-tui/keys/theme-fallbacks", test_theme_fallbacks);
	g_test_add_func("/ai-glib/ai-tui/keys/busy-draft", test_busy_keeps_draft);
	g_test_add_func("/ai-glib/ai-tui/keys/control-shortcuts", test_control_shortcuts);
	g_test_add_data_func("/ai-glib/ai-tui/keys/activity-motion", GINT_TO_POINTER(FALSE), test_activity_motion);
	g_test_add_data_func("/ai-glib/ai-tui/keys/reduced-motion", GINT_TO_POINTER(TRUE), test_activity_motion);
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
	g_test_add_func("/ai-glib/ai-tui/positional-prompt-without-tty",
	                test_positional_prompt_without_a_tty_runs_one_shot);
	g_test_add_func("/ai-glib/ai-tui/prompt-from-stdin",
	                test_prompt_from_stdin_runs_one_shot);
	g_test_add_func("/ai-glib/ai-tui/positional-prompt-wins-over-stdin",
	                test_positional_prompt_wins_over_stdin);
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
	g_test_add_func("/ai-glib/ai-tui/provider-command",
	                test_provider_command_shows_and_switches);
	g_test_add_func("/ai-glib/ai-tui/provider-command-error",
	                test_provider_command_failure_keeps_current);
	g_test_add_func("/ai-glib/ai-tui/expand-mention",
	                test_expand_inlines_a_mention);
	g_test_add_func("/ai-glib/ai-tui/expand-unknown",
	                test_expand_reports_an_unknown_command);
	g_test_add_func("/ai-glib/ai-tui/passthrough",
	                test_unknown_builtin_free_line_reaches_the_child);
	g_test_add_func("/ai-glib/ai-tui/no-expand",
	                test_no_expand_leaves_a_command_alone);

	g_test_add_func("/ai-glib/ai-tui/keys/enter-sends",
	                test_enter_sends_the_prompt);
	g_test_add_func("/ai-glib/ai-tui/keys/positional-prompt-sends",
	                test_positional_prompt_sends_in_the_tui);
	g_test_add_func("/ai-glib/ai-tui/keys/alt-enter-is-a-newline",
	                test_alt_enter_inserts_a_newline);
	g_test_add_func("/ai-glib/ai-tui/keys/one-interrupt-stays",
	                test_one_interrupt_clears_and_stays);
	g_test_add_func("/ai-glib/ai-tui/keys/two-interrupts-quit",
	                test_two_interrupts_quit);
	g_test_add_func("/ai-glib/ai-tui/keys/expired-interrupt",
	                test_an_expired_interrupt_does_not_quit);
	g_test_add_func("/ai-glib/ai-tui/keys/escape-still-dismisses",
	                test_escape_still_dismisses_the_menu);
	g_test_add_func("/ai-glib/ai-tui/keys/editor-round-trip",
	                test_ctrl_g_round_trips_through_the_editor);
	g_test_add_func("/ai-glib/ai-tui/keys/editor-abort",
	                test_an_aborted_edit_keeps_the_prompt);

	status = g_test_run();
	if (tmux_available())
	{
		const gchar *args[] = { "kill-server", NULL };
		g_autofree gchar *out = tmux_run(args);
	}
	g_free(tmux_socket);
	g_free(tui_binary);
	return status;
}
