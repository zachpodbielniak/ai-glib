/*
 * test-ai-cli-update.c - `ai --version`, `--check-update` and `--update`
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spawns the built `ai` against a borrowed checkout (see
 * test-update-fixture.h). The library tests cover every state; these
 * cover the wiring: that AI_GLIB_SOURCE_DIR is honoured, that --json is
 * the status the library computed, and that --update runs the pipeline
 * through the CLI with a stub make -- and, with no terminal, stops
 * before a privileged install and prints the command instead.
 *
 * HOME, XDG_STATE_HOME and the working directory are sandboxed.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#include "test-update-fixture.h"

static gchar *ai_bin = NULL;
static gchar *sandbox = NULL;

typedef struct
{
	gchar *out;
	gchar *err;
	gint   status;
} Run;

static void
run_clear(Run *run)
{
	g_clear_pointer(&run->out, g_free);
	g_clear_pointer(&run->err, g_free);
}

static void
run_ai(UpdateFixture *f, const gchar *source_dir, const gchar * const *args, Run *run)
{
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_auto(GStrv) envp = g_get_environ();
	gsize i;

	g_ptr_array_add(argv, ai_bin);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);

	if (f != NULL)
		envp = update_fixture_environ(f, envp);
	if (source_dir != NULL)
		envp = g_environ_setenv(envp, "AI_GLIB_SOURCE_DIR", source_dir, TRUE);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_set_environ(launcher, envp);
	g_subprocess_launcher_set_cwd(launcher, sandbox);
	proc = g_subprocess_launcher_spawnv(launcher, (const gchar * const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, &run->out, &run->err, &error);
	g_assert_no_error(error);
	run->status = g_subprocess_get_exit_status(proc);
}

static JsonObject *
parse_object(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;

	json_parser_load_from_data(parser, text, -1, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static void
test_version(void)
{
	const gchar *args[] = { "--version", NULL };
	g_autofree gchar *summary = ai_build_info_dup_summary();
	g_autofree gchar *expected = g_strdup_printf("ai (ai-glib) %s\n", summary);
	Run run = { NULL, NULL, 0 };

	run_ai(NULL, NULL, args, &run);
	g_assert_cmpint(run.status, ==, 0);
	g_assert_cmpstr(run.out, ==, expected);
	run_clear(&run);
}

/* A checkout that is not there is a clear "unavailable", not a guess. */
static void
test_check_missing_checkout(void)
{
	const gchar *args[] = { "--check-update", "--json", NULL };
	g_autofree gchar *missing = g_build_filename(sandbox, "no-such-checkout", NULL);
	g_autoptr(JsonObject) object = NULL;
	Run run = { NULL, NULL, 0 };

	run_ai(NULL, missing, args, &run);
	g_assert_cmpint(run.status, ==, 0);
	object = parse_object(run.out);
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, "unavailable");
	g_assert_cmpstr(json_object_get_string_member(object, "source_dir"), ==, missing);
	g_assert_nonnull(strstr(json_object_get_string_member(object, "summary"), "does not exist"));
	g_assert_cmpstr(json_object_get_string_member(object, "version"), ==, ai_build_info_get_version());
	run_clear(&run);
}

static void
test_check_behind(void)
{
	const gchar *json_args[] = { "--check-update", "--json", NULL };
	const gchar *text_args[] = { "--check-update", NULL };
	g_autoptr(JsonObject) object = NULL;
	UpdateFixture *f;
	Run run = { NULL, NULL, 0 };

	if (!update_fixture_available())
	{
		g_test_skip("this build does not know its commit or source tree");
		return;
	}
	f = update_fixture_new();

	run_ai(f, NULL, json_args, &run);
	g_assert_cmpint(run.status, ==, 0);
	object = parse_object(run.out);
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, "up-to-date");
	g_assert_cmpstr(json_object_get_string_member(object, "build_commit"), ==,
	                ai_build_info_get_commit());
	run_clear(&run);
	g_clear_pointer(&object, json_object_unref);

	update_fixture_push(f);
	run_ai(f, NULL, json_args, &run);
	g_assert_cmpint(run.status, ==, 0);
	object = parse_object(run.out);
	g_assert_cmpstr(json_object_get_string_member(object, "state"), ==, "behind");
	g_assert_cmpint(json_object_get_int_member(object, "behind"), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(object, "upstream"), ==, "origin/master");
	run_clear(&run);

	run_ai(f, NULL, text_args, &run);
	g_assert_cmpint(run.status, ==, 0);
	g_assert_cmpstr(run.out, ==, "Update available: 1 commit behind origin/master. "
	                             "Run `ai --update` or /update.\n");
	run_clear(&run);

	/* Nothing was built by checking. */
	{
		g_autofree gchar *log = update_fixture_make_log(f);

		g_assert_cmpstr(log, ==, "");
	}
	update_fixture_free(f);
}

static void
test_update_refused(void)
{
	const gchar *args[] = { "--update", NULL };
	g_autofree gchar *file = NULL;
	g_autofree gchar *log = NULL;
	UpdateFixture *f;
	Run run = { NULL, NULL, 0 };

	if (!update_fixture_available())
	{
		g_test_skip("this build does not know its commit or source tree");
		return;
	}
	f = update_fixture_new();

	/* Up to date: nothing to do is success, and nothing runs. */
	run_ai(f, NULL, args, &run);
	g_assert_cmpint(run.status, ==, 0);
	g_assert_nonnull(strstr(run.out, "Already up to date with origin/master."));
	run_clear(&run);

	update_fixture_push(f);
	file = g_build_filename(f->clone, "config.mk", NULL);
	update_fixture_write(file, "local edit\n", 0);
	run_ai(f, NULL, args, &run);
	g_assert_cmpint(run.status, ==, 1);
	g_assert_nonnull(strstr(run.err, "local changes"));
	run_clear(&run);

	log = update_fixture_make_log(f);
	g_assert_cmpstr(log, ==, "");
	update_fixture_free(f);
}

/*
 * The whole pipeline through the CLI. The build's prefix is not
 * writable by a normal user, and a test has no terminal, so it must
 * build, stop before the install, and say exactly what to run.
 */
static void
test_update_needs_privilege(void)
{
	const gchar *args[] = { "--update", NULL };
	g_autofree gchar *log = NULL;
	g_autofree gchar *head = NULL;
	g_autofree gchar *upstream_head = NULL;
	UpdateFixture *f;
	Run run = { NULL, NULL, 0 };

	if (!update_fixture_available())
	{
		g_test_skip("this build does not know its commit or source tree");
		return;
	}
	if (access(ai_build_info_get_prefix(), W_OK) == 0)
	{
		g_test_skip("the build prefix is writable here; the privilege path cannot be shown");
		return;
	}
	f = update_fixture_new();
	update_fixture_push(f);

	run_ai(f, NULL, args, &run);
	g_assert_cmpint(run.status, ==, 3);
	g_assert_nonnull(strstr(run.out, "==> Building"));
	g_assert_nonnull(strstr(run.out, "sudo "));
	g_assert_nonnull(strstr(run.out, " install PREFIX="));
	g_assert_nonnull(strstr(run.out, f->clone));

	log = update_fixture_make_log(f);
	g_assert_nonnull(strstr(log, "clean"));
	g_assert_nonnull(strstr(log, " all PREFIX="));
	g_assert_null(strstr(log, "install"));

	head = update_fixture_run(f->clone, (const gchar *[]){ "git", "rev-parse", "HEAD", NULL });
	upstream_head = update_fixture_run(f->seed, (const gchar *[]){ "git", "rev-parse", "HEAD", NULL });
	g_assert_cmpstr(head, ==, upstream_head);
	run_clear(&run);
	update_fixture_free(f);
}

/* --json on its own still belongs to a report or a check. */
static void
test_json_needs_a_mode(void)
{
	const gchar *args[] = { "--json", NULL };
	const gchar *both[] = { "--check-update", "--update", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(NULL, NULL, args, &run);
	g_assert_cmpint(run.status, ==, 2);
	run_clear(&run);

	run_ai(NULL, NULL, both, &run);
	g_assert_cmpint(run.status, ==, 2);
	run_clear(&run);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *dir = NULL;
	g_autofree gchar *state = NULL;

	g_test_init(&argc, &argv, NULL);

	sandbox = g_dir_make_tmp("ai-glib-cli-update-XXXXXX", NULL);
	state = g_build_filename(sandbox, "state", NULL);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("GIO_USE_VFS", "local", TRUE);
	g_unsetenv("AI_GLIB_UPDATE_MAKE");
	g_unsetenv("AI_GLIB_SOURCE_DIR");

	dir = g_path_get_dirname(argv[0]);
	ai_bin = g_build_filename(dir, "..", "bin", "ai", NULL);
	if (!g_path_is_absolute(ai_bin))
	{
		g_autofree gchar *cwd = g_get_current_dir();
		gchar *absolute = g_build_filename(cwd, ai_bin, NULL);

		g_free(ai_bin);
		ai_bin = absolute;
	}
	g_assert_cmpint(g_chdir(sandbox), ==, 0);

	g_test_add_func("/ai-glib/ai-cli/update/version", test_version);
	g_test_add_func("/ai-glib/ai-cli/update/check-missing-checkout", test_check_missing_checkout);
	g_test_add_func("/ai-glib/ai-cli/update/check-behind", test_check_behind);
	g_test_add_func("/ai-glib/ai-cli/update/refused", test_update_refused);
	g_test_add_func("/ai-glib/ai-cli/update/needs-privilege", test_update_needs_privilege);
	g_test_add_func("/ai-glib/ai-cli/update/json-needs-a-mode", test_json_needs_a_mode);

	return g_test_run();
}
