/*
 * test-ai-tui-update.c - /update and the version in ai-tui
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drives the real ai-tui through --dump, which runs one line with no
 * terminal and prints the transcript. That is the path with no curses to
 * hand over, so /update runs without sudo and must stop before a
 * privileged install; the interactive path shares everything else with
 * it and with `ai --update`.
 *
 * The checkout is borrowed (see test-update-fixture.h); `make` is a
 * stub. HOME, XDG_* and the working directory are sandboxed.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#include "test-update-fixture.h"

static gchar *tui_bin = NULL;
static gchar *sandbox = NULL;

static gchar *
run_tui(UpdateFixture *f, const gchar *line, gint *status)
{
	const gchar *argv[] = { tui_bin, "--dump", line, "-p", "grok-build", "--width", "0", NULL };
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) envp = g_get_environ();
	g_autofree gchar *err = NULL;
	gchar *out = NULL;

	envp = update_fixture_environ(f, envp);
	envp = g_environ_unsetenv(envp, "ANTHROPIC_API_KEY");
	envp = g_environ_unsetenv(envp, "OPENAI_API_KEY");
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_set_environ(launcher, envp);
	g_subprocess_launcher_set_cwd(launcher, sandbox);
	proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, &out, &err, &error);
	g_assert_no_error(error);
	*status = g_subprocess_get_exit_status(proc);
	return out;
}

static gboolean
skip_unless_available(void)
{
	if (update_fixture_available())
		return FALSE;
	g_test_skip("this build does not know its commit or source tree");
	return TRUE;
}

static void
test_version(void)
{
	const gchar *argv[] = { tui_bin, "--version", NULL };
	g_autofree gchar *summary = ai_build_info_dup_summary();
	g_autofree gchar *expected = g_strdup_printf("ai-tui %s\n", summary);
	g_autofree gchar *out = NULL;
	gint status = -1;

	g_assert_true(g_spawn_sync(sandbox, (gchar **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
	                           &out, NULL, &status, NULL));
	g_assert_true(g_spawn_check_wait_status(status, NULL));
	g_assert_cmpstr(out, ==, expected);
}

static void
test_status(void)
{
	UpdateFixture *f;
	g_autofree gchar *out = NULL;
	gint status;

	if (skip_unless_available())
		return;
	f = update_fixture_new();
	update_fixture_push(f);

	out = run_tui(f, "/update status", &status);
	g_assert_cmpint(status, ==, 0);
	g_assert_nonnull(strstr(out, "Update available: 1 commit behind origin/master."));
	g_assert_nonnull(strstr(out, ai_build_info_get_version()));
	update_fixture_free(f);
}

static void
test_refused(void)
{
	UpdateFixture *f;
	g_autofree gchar *out = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *log = NULL;
	gint status;

	if (skip_unless_available())
		return;
	f = update_fixture_new();
	update_fixture_push(f);
	file = g_build_filename(f->clone, "config.mk", NULL);
	update_fixture_write(file, "local edit\n", 0);

	out = run_tui(f, "/update", &status);
	g_assert_cmpint(status, ==, 0);
	g_assert_nonnull(strstr(out, "Commit or stash them to update."));
	log = update_fixture_make_log(f);
	g_assert_cmpstr(log, ==, "");
	update_fixture_free(f);
}

/* No terminal: build, stop before the privileged install, say the command. */
static void
test_run_without_terminal(void)
{
	UpdateFixture *f;
	g_autofree gchar *out = NULL;
	g_autofree gchar *log = NULL;
	gint status;

	if (skip_unless_available())
		return;
	if (access(ai_build_info_get_prefix(), W_OK) == 0)
	{
		g_test_skip("the build prefix is writable here; the privilege path cannot be shown");
		return;
	}
	f = update_fixture_new();
	update_fixture_push(f);

	out = run_tui(f, "/update", &status);
	g_assert_cmpint(status, ==, 0);
	g_assert_nonnull(strstr(out, "==> Building"));
	g_assert_nonnull(strstr(out, "Built, not installed"));
	g_assert_nonnull(strstr(out, " install PREFIX="));
	log = update_fixture_make_log(f);
	g_assert_nonnull(strstr(log, " all PREFIX="));
	g_assert_null(strstr(log, "install"));
	update_fixture_free(f);
}

static void
test_not_built(void)
{
	g_test_skip("ai-tui was not built (no ncursesw)");
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *state = NULL;

	g_test_init(&argc, &argv, NULL);

	sandbox = g_dir_make_tmp("ai-glib-tui-update-XXXXXX", NULL);
	state = g_build_filename(sandbox, "state", NULL);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("GIO_USE_VFS", "local", TRUE);

	dir = g_path_get_dirname(argv[0]);
	tui_bin = g_build_filename(dir, "..", "bin", "ai-tui", NULL);
	if (!g_path_is_absolute(tui_bin))
	{
		g_autofree gchar *cwd = g_get_current_dir();
		gchar *absolute = g_build_filename(cwd, tui_bin, NULL);

		g_free(tui_bin);
		tui_bin = absolute;
	}
	g_assert_cmpint(g_chdir(sandbox), ==, 0);

	if (!g_file_test(tui_bin, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_add_func("/ai-glib/ai-tui/update/not-built", test_not_built);
		return g_test_run();
	}

	g_test_add_func("/ai-glib/ai-tui/update/version", test_version);
	g_test_add_func("/ai-glib/ai-tui/update/status", test_status);
	g_test_add_func("/ai-glib/ai-tui/update/refused", test_refused);
	g_test_add_func("/ai-glib/ai-tui/update/run-without-terminal", test_run_without_terminal);

	return g_test_run();
}
