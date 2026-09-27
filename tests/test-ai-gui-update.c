/*
 * test-ai-gui-update.c - ai-gui's update controller, without a display
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * gui/ai-gui-update.c is GTK-free for the same reason ai-gui-session.c
 * is: this suite links it directly, and a test that needed a display
 * would pass or fail by whose machine ran it. The window only maps its
 * banner text and messages onto widgets.
 *
 * The updater points at a throwaway bare repository and a clone; `make`
 * is a stub and the prefix is not writable, so a run must stop before
 * the install and hand back the command -- ai-gui never runs sudo.
 * HOME, XDG_* and the working directory are sandboxed.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <unistd.h>

#include "ai-gui-update.h"

typedef struct
{
	gchar      *root;
	gchar      *seed;
	gchar      *clone;
	gchar      *make_log;
	gchar      *prefix;
	gchar      *pkexec;
	AiUpdater  *updater;
	AiGuiUpdate *update;
	GPtrArray  *messages;
	guint       changes;
} Fixture;

static gchar *
run_git(const gchar *cwd, const gchar * const *argv)
{
	g_autoptr(GSubprocessLauncher) launcher =
		g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *err = NULL;
	gchar *out = NULL;

	g_subprocess_launcher_set_cwd(launcher, cwd);
	proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, &out, &err, &error);
	g_assert_no_error(error);
	if (!g_subprocess_get_successful(proc))
		g_error("git %s failed: %s", argv[1], err);
	return g_strstrip(out);
}

#define GIT(cwd, ...) g_free(run_git((cwd), (const gchar *[]){ "git", __VA_ARGS__, NULL }))

static void
on_message(AiGuiUpdate *update, const gchar *text, gpointer data)
{
	g_ptr_array_add(((Fixture *)data)->messages, g_strdup(text));
}

static void
on_changed(AiGuiUpdate *update, gpointer data)
{
	((Fixture *)data)->changes++;
}

static void
fixture_set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *upstream = NULL;
	g_autofree gchar *make = NULL;
	g_autofree gchar *bin = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *commit = NULL;

	f->root = g_dir_make_tmp("ai-glib-gui-update-XXXXXX", &error);
	g_assert_no_error(error);
	upstream = g_build_filename(f->root, "upstream.git", NULL);
	f->seed = g_build_filename(f->root, "seed", NULL);
	f->clone = g_build_filename(f->root, "clone", NULL);
	make = g_build_filename(f->root, "make", NULL);
	f->prefix = g_build_filename(f->root, "prefix", NULL);
	f->pkexec = g_build_filename(f->root, "pkexec", NULL);
	bin = g_build_filename(f->prefix, "bin", NULL);
	state = g_build_filename(f->root, "state", NULL);
	f->make_log = g_build_filename(f->root, "make.log", NULL);

	GIT(f->root, "init", "-q", "--bare", "-b", "master", upstream);
	GIT(f->root, "clone", "-q", upstream, f->seed);
	GIT(f->seed, "checkout", "-q", "-b", "master");
	GIT(f->seed, "commit", "-q", "--allow-empty", "-m", "first");
	GIT(f->seed, "push", "-q", "-u", "origin", "master");
	GIT(f->root, "clone", "-q", upstream, f->clone);
	commit = run_git(f->clone, (const gchar *[]){ "git", "rev-parse", "HEAD", NULL });

	script = g_strdup_printf("#!/bin/sh\necho \"$*\" >> '%s'\necho \"stub make $*\"\n", f->make_log);
	g_assert_true(g_file_set_contents(make, script, -1, NULL));
	g_assert_cmpint(g_chmod(make, 0755), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(bin, 0755), ==, 0);
	g_assert_cmpint(g_chmod(bin, 0555), ==, 0);

	/* Never the machine's pkexec. This one says "no agent" unless a case
	 * stages a grant, the way a desktop without polkit would. */
	g_assert_true(g_file_set_contents(f->pkexec,
		"#!/bin/sh\n[ -e \"$0.grant\" ] && exec \"$@\"\nexit 127\n", -1, NULL));
	g_assert_cmpint(g_chmod(f->pkexec, 0755), ==, 0);

	f->updater = g_object_new(AI_TYPE_UPDATER,
	                          "source-dir", f->clone,
	                          "build-commit", commit,
	                          "build-version", "0.3.0",
	                          "state-dir", state,
	                          "make-program", make,
	                          "prefix", f->prefix,
	                          "pkexec-program", f->pkexec,
	                          NULL);
	f->update = ai_gui_update_new(f->updater);
	f->messages = g_ptr_array_new_with_free_func(g_free);
	g_signal_connect(f->update, "message", G_CALLBACK(on_message), f);
	g_signal_connect(f->update, "changed", G_CALLBACK(on_changed), f);
}

static void
remove_tree(const gchar *path)
{
	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autoptr(GFileEnumerator) children = NULL;
	GFileInfo *info;

	g_chmod(path, 0755);
	children = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME ","
	                                     G_FILE_ATTRIBUTE_STANDARD_TYPE,
	                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	while (children != NULL &&
	       (info = g_file_enumerator_next_file(children, NULL, NULL)) != NULL)
	{
		g_autofree gchar *child = g_build_filename(path, g_file_info_get_name(info), NULL);

		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
			remove_tree(child);
		else
			g_unlink(child);
		g_object_unref(info);
	}
	g_rmdir(path);
}

static void
fixture_tear_down(Fixture *f, gconstpointer data)
{
	ai_gui_update_shutdown(f->update);
	g_clear_object(&f->update);
	g_clear_object(&f->updater);
	g_ptr_array_unref(f->messages);
	remove_tree(f->root);
	g_free(f->root);
	g_free(f->seed);
	g_free(f->clone);
	g_free(f->make_log);
	g_free(f->prefix);
	g_free(f->pkexec);
}

static void
push_upstream(Fixture *f)
{
	GIT(f->seed, "commit", "-q", "--allow-empty", "-m", "upstream change");
	GIT(f->seed, "push", "-q", "origin", "master");
}

static gboolean
saw(Fixture *f, const gchar *fragment)
{
	guint i;

	for (i = 0; i < f->messages->len; i++)
		if (strstr(g_ptr_array_index(f->messages, i), fragment) != NULL)
			return TRUE;
	return FALSE;
}

static gchar *
make_log(Fixture *f)
{
	gchar *text = NULL;

	if (!g_file_get_contents(f->make_log, &text, NULL, NULL))
		return g_strdup("");
	return text;
}

static void
wait_idle(Fixture *f)
{
	while (ai_gui_update_get_busy(f->update))
		g_main_context_iteration(NULL, TRUE);
	while (g_main_context_iteration(NULL, FALSE))
		;
}

/* No banner for "up to date"; the summary sentence when one exists. */
static void
test_banner(Fixture *f, gconstpointer data)
{
	g_autofree gchar *banner = NULL;

	g_assert_null(ai_gui_update_dup_banner(f->update));
	g_assert_cmpint(ai_gui_update_get_action(f->update, FALSE, NULL), ==, AI_GUI_UPDATE_ACTION_NONE);
	ai_gui_update_check(f->update);
	g_assert_true(saw(f, "Checking for updates"));
	wait_idle(f);
	g_assert_true(saw(f, "Up to date with origin/master."));
	g_assert_null(ai_gui_update_dup_banner(f->update));

	push_upstream(f);
	ai_gui_update_check(f->update);
	wait_idle(f);
	banner = ai_gui_update_dup_banner(f->update);
	g_assert_cmpstr(banner, ==, "Update available: 1 commit behind origin/master. "
	                            "Run `ai --update` or /update.");
	g_assert_cmpuint(f->changes, >, 0);

	/* The button: Update, and not while a turn runs. */
	{
		gboolean sensitive = FALSE;

		g_assert_cmpint(ai_gui_update_get_action(f->update, FALSE, &sensitive), ==,
		                AI_GUI_UPDATE_ACTION_UPDATE);
		g_assert_true(sensitive);
		g_assert_cmpint(ai_gui_update_get_action(f->update, TRUE, &sensitive), ==,
		                AI_GUI_UPDATE_ACTION_UPDATE);
		g_assert_false(sensitive);
	}
}

static void
test_refuses_mid_turn(Fixture *f, gconstpointer data)
{
	g_autofree gchar *log = NULL;

	push_upstream(f);
	ai_gui_update_run(f->update, TRUE);
	g_assert_false(ai_gui_update_get_busy(f->update));
	g_assert_true(saw(f, "A turn is running."));
	log = make_log(f);
	g_assert_cmpstr(log, ==, "");
}

/* Everything but the privileged install, then the exact command. */
static void
test_run_needs_privilege(Fixture *f, gconstpointer data)
{
	g_autofree gchar *log = NULL;

	if (geteuid() == 0)
	{
		g_test_skip("root can write anywhere");
		return;
	}
	push_upstream(f);
	ai_gui_update_run(f->update, FALSE);
	g_assert_true(ai_gui_update_get_busy(f->update));
	{
		g_autofree gchar *banner = ai_gui_update_dup_banner(f->update);

		g_assert_cmpstr(banner, ==, "Updating ai-glib…");
	}
	/* Disabled while it runs, and a second click does not start another. */
	{
		gboolean sensitive = TRUE;

		ai_gui_update_get_action(f->update, FALSE, &sensitive);
		g_assert_false(sensitive);
	}
	ai_gui_update_run(f->update, FALSE);
	g_assert_true(saw(f, "already running"));

	wait_idle(f);
	g_assert_true(saw(f, "==> Building"));
	g_assert_true(saw(f, "Built, not installed"));
	g_assert_true(saw(f, "Finish with: sudo "));
	log = make_log(f);
	g_assert_nonnull(strstr(log, " all PREFIX="));
	g_assert_null(strstr(log, "install"));
}

/* Shut down mid-run: no message after, and the run is cancelled. */
static void
test_shutdown_is_silent(Fixture *f, gconstpointer data)
{
	guint before;
	gint i;

	push_upstream(f);
	ai_gui_update_run(f->update, FALSE);
	ai_gui_update_shutdown(f->update);
	before = f->messages->len;
	for (i = 0; i < 300; i++)
	{
		g_main_context_iteration(NULL, FALSE);
		g_usleep(10000);
	}
	g_assert_cmpuint(f->messages->len, ==, before);
	g_assert_false(ai_gui_update_get_busy(f->update));
}

/* The drain returns once the cancelled run has really finished. */
static void
test_drain(Fixture *f, gconstpointer data)
{
	g_autofree gchar *log = NULL;
	gint64 started;

	push_upstream(f);
	ai_gui_update_run(f->update, FALSE);
	ai_gui_update_check(f->update);
	ai_gui_update_shutdown(f->update);
	started = g_get_monotonic_time();
	ai_gui_update_drain(f->update);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 20 * G_USEC_PER_SEC);
	g_assert_false(ai_updater_is_checking(f->updater));
	log = make_log(f);
	g_assert_null(strstr(log, "install"));
}

/* Granted through pkexec: installed, and the button becomes Restart now. */
static void
test_run_installs_then_restart(Fixture *f, gconstpointer data)
{
	g_autofree gchar *grant = g_strconcat(f->pkexec, ".grant", NULL);
	g_autofree gchar *banner = NULL;
	g_autofree gchar *restart = NULL;
	g_autofree gchar *expected = NULL;
	gboolean sensitive = FALSE;

	if (geteuid() == 0)
	{
		g_test_skip("root can write anywhere");
		return;
	}
	g_assert_true(g_file_set_contents(grant, "", -1, NULL));
	push_upstream(f);
	ai_gui_update_run(f->update, FALSE);
	wait_idle(f);

	g_assert_true(saw(f, "Installed 0.3.0"));
	banner = ai_gui_update_dup_banner(f->update);
	g_assert_true(g_str_has_prefix(banner, "Installed 0.3.0"));
	g_assert_cmpint(ai_gui_update_get_action(f->update, TRUE, &sensitive), ==,
	                AI_GUI_UPDATE_ACTION_RESTART);
	/* Restarting does not interrupt a turn in this process's sense: the
	 * window saves every session first. It is offered regardless. */
	g_assert_true(sensitive);
	restart = ai_gui_update_dup_restart_path(f->update);
	expected = g_build_filename(f->prefix, "bin", "ai-gui", NULL);
	g_assert_cmpstr(restart, ==, expected);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *home = NULL;
	g_autofree gchar *state = NULL;

	g_test_init(&argc, &argv, NULL);

	home = g_dir_make_tmp("ai-glib-gui-update-home-XXXXXX", NULL);
	state = g_build_filename(home, "state", NULL);
	g_setenv("HOME", home, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("XDG_CONFIG_HOME", home, TRUE);
	g_setenv("GIO_USE_VFS", "local", TRUE);
	g_setenv("GIT_CONFIG_NOSYSTEM", "1", TRUE);
	g_setenv("GIT_ALLOW_PROTOCOL", "file", TRUE);
	g_setenv("GIT_AUTHOR_NAME", "Test", TRUE);
	g_setenv("GIT_AUTHOR_EMAIL", "test@example.invalid", TRUE);
	g_setenv("GIT_COMMITTER_NAME", "Test", TRUE);
	g_setenv("GIT_COMMITTER_EMAIL", "test@example.invalid", TRUE);
	g_assert_cmpint(g_chdir(home), ==, 0);

	g_test_add("/ai-glib/ai-gui/update/banner", Fixture, NULL,
	           fixture_set_up, test_banner, fixture_tear_down);
	g_test_add("/ai-glib/ai-gui/update/refuses-mid-turn", Fixture, NULL,
	           fixture_set_up, test_refuses_mid_turn, fixture_tear_down);
	g_test_add("/ai-glib/ai-gui/update/run-needs-privilege", Fixture, NULL,
	           fixture_set_up, test_run_needs_privilege, fixture_tear_down);
	g_test_add("/ai-glib/ai-gui/update/shutdown-is-silent", Fixture, NULL,
	           fixture_set_up, test_shutdown_is_silent, fixture_tear_down);
	g_test_add("/ai-glib/ai-gui/update/installs-then-restart", Fixture, NULL,
	           fixture_set_up, test_run_installs_then_restart, fixture_tear_down);
	g_test_add("/ai-glib/ai-gui/update/drain", Fixture, NULL,
	           fixture_set_up, test_drain, fixture_tear_down);

	return g_test_run();
}
