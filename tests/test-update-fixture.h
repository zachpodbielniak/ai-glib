/*
 * test-update-fixture.h - A checkout the running build can update from
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The front-end tests drive the real `ai` and `ai-tui` binaries, whose
 * build commit is baked in. For the updater to accept a checkout, that
 * checkout has to contain that commit -- a fresh `git init` never will.
 * So the fixture borrows it: a bare "upstream" cloned with --shared from
 * the tree the binary was built in (alternates, no copy, nothing written
 * there), its master pointed at the build commit, and two clones of it.
 *
 * Nothing here writes to the source tree. The stub `make` records its
 * arguments; tests point AI_GLIB_UPDATE_MAKE at it so no real build or
 * install ever runs.
 */

#pragma once

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>

#include "ai-glib.h"

typedef struct
{
	gchar *root;
	gchar *upstream;
	gchar *seed;
	gchar *clone;
	gchar *state;
	gchar *make;
	gchar *make_log;
} UpdateFixture;

static inline gchar *
update_fixture_run(const gchar *cwd, const gchar * const *argv)
{
	g_autoptr(GSubprocessLauncher) launcher =
		g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		                          G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *err = NULL;
	gchar *out = NULL;

	g_subprocess_launcher_set_cwd(launcher, cwd);
	g_subprocess_launcher_setenv(launcher, "GIT_CONFIG_NOSYSTEM", "1", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_AUTHOR_NAME", "Test", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_AUTHOR_EMAIL", "test@example.invalid", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_COMMITTER_NAME", "Test", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_COMMITTER_EMAIL", "test@example.invalid", TRUE);
	proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, &out, &err, &error);
	g_assert_no_error(error);
	if (!g_subprocess_get_successful(proc))
		g_error("git %s failed in %s: %s", argv[1], cwd, err);
	return g_strstrip(out);
}

#define UPDATE_GIT(cwd, ...) \
	g_free(update_fixture_run((cwd), (const gchar *[]){ "git", __VA_ARGS__, NULL }))

/* Whether this build can be updated at all: it has to know its commit
 * and its source tree has to still hold it. */
static inline gboolean
update_fixture_available(void)
{
	return ai_build_info_get_commit() != NULL &&
	       ai_build_info_get_source_dir() != NULL &&
	       g_file_test(ai_build_info_get_source_dir(), G_FILE_TEST_IS_DIR);
}

static inline void
update_fixture_write(const gchar *path, const gchar *text, gint mode)
{
	g_autoptr(GError) error = NULL;

	g_file_set_contents(path, text, -1, &error);
	g_assert_no_error(error);
	if (mode != 0)
		g_assert_cmpint(g_chmod(path, mode), ==, 0);
}

static inline UpdateFixture *
update_fixture_new(void)
{
	UpdateFixture *f = g_new0(UpdateFixture, 1);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *script = NULL;

	f->root = g_dir_make_tmp("ai-glib-update-fixture-XXXXXX", &error);
	g_assert_no_error(error);
	f->upstream = g_build_filename(f->root, "upstream.git", NULL);
	f->seed = g_build_filename(f->root, "seed", NULL);
	f->clone = g_build_filename(f->root, "clone", NULL);
	f->state = g_build_filename(f->root, "state", NULL);
	f->make = g_build_filename(f->root, "make", NULL);
	f->make_log = g_build_filename(f->root, "make.log", NULL);

	UPDATE_GIT(f->root, "clone", "-q", "--bare", "--shared", "--no-tags",
	           ai_build_info_get_source_dir(), f->upstream);
	UPDATE_GIT(f->upstream, "update-ref", "refs/heads/master", ai_build_info_get_commit());
	UPDATE_GIT(f->upstream, "symbolic-ref", "HEAD", "refs/heads/master");
	UPDATE_GIT(f->root, "clone", "-q", "--shared", "--no-tags", "-b", "master", f->upstream, f->clone);
	UPDATE_GIT(f->root, "clone", "-q", "--shared", "--no-tags", "-b", "master", f->upstream, f->seed);

	/*
	 * The pipeline runs `git submodule update --init`, and .gitmodules
	 * names network URLs. Point each at the submodule already checked out
	 * in the source tree -- `submodule init` keeps a URL that is already
	 * configured -- and update_fixture_environ() forbids every protocol
	 * but file, so a test can never reach the network by accident.
	 */
	{
		g_autofree gchar *paths = update_fixture_run(f->clone, (const gchar *[]){
			"git", "config", "-f", ".gitmodules", "--get-regexp", "^submodule\\..*\\.path$", NULL });
		g_auto(GStrv) lines = g_strsplit(paths, "\n", -1);
		guint i;

		for (i = 0; lines[i] != NULL; i++)
		{
			g_auto(GStrv) pair = g_strsplit(lines[i], " ", 2);
			g_autofree gchar *key = NULL;
			g_autofree gchar *local = NULL;

			if (g_strv_length(pair) != 2)
				continue;
			/* submodule.<name>.path -> submodule.<name>.url */
			key = g_strdup(pair[0]);
			strcpy(key + strlen(key) - strlen("path"), "url");
			local = g_build_filename(ai_build_info_get_source_dir(), pair[1], NULL);
			UPDATE_GIT(f->clone, "config", key, local);
		}
		UPDATE_GIT(f->clone, "config", "protocol.file.allow", "always");
	}

	script = g_strdup_printf("#!/bin/sh\necho \"$*\" >> '%s'\necho \"stub make $*\"\n",
	                         f->make_log);
	update_fixture_write(f->make, script, 0755);
	return f;
}

/* One commit on upstream master that this build does not have. */
static inline void
update_fixture_push(UpdateFixture *f)
{
	UPDATE_GIT(f->seed, "commit", "-q", "--allow-empty", "-m", "upstream change");
	UPDATE_GIT(f->seed, "push", "-q", "origin", "master");
}

static inline gchar *
update_fixture_make_log(UpdateFixture *f)
{
	gchar *text = NULL;

	if (!g_file_get_contents(f->make_log, &text, NULL, NULL))
		return g_strdup("");
	return text;
}

/* The environment a front-end runs under: this checkout, this state
 * directory, the stub make. */
static inline gchar **
update_fixture_environ(UpdateFixture *f, gchar **envp)
{
	envp = g_environ_setenv(envp, "AI_GLIB_SOURCE_DIR", f->clone, TRUE);
	envp = g_environ_setenv(envp, "XDG_STATE_HOME", f->state, TRUE);
	envp = g_environ_setenv(envp, "AI_GLIB_UPDATE_MAKE", f->make, TRUE);
	envp = g_environ_setenv(envp, "GIT_CONFIG_NOSYSTEM", "1", TRUE);
	envp = g_environ_setenv(envp, "GIT_ALLOW_PROTOCOL", "file", TRUE);
	envp = g_environ_unsetenv(envp, "AI_GLIB_NO_UPDATE_CHECK");
	return envp;
}

static inline void
update_fixture_remove_tree(const gchar *path)
{
	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autoptr(GFileEnumerator) children = NULL;
	GFileInfo *info;

	children = g_file_enumerate_children(file, G_FILE_ATTRIBUTE_STANDARD_NAME ","
	                                     G_FILE_ATTRIBUTE_STANDARD_TYPE,
	                                     G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
	while (children != NULL &&
	       (info = g_file_enumerator_next_file(children, NULL, NULL)) != NULL)
	{
		g_autofree gchar *child = g_build_filename(path, g_file_info_get_name(info), NULL);

		if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
			update_fixture_remove_tree(child);
		else
			g_unlink(child);
		g_object_unref(info);
	}
	g_rmdir(path);
}

static inline void
update_fixture_free(UpdateFixture *f)
{
	update_fixture_remove_tree(f->root);
	g_free(f->root);
	g_free(f->upstream);
	g_free(f->seed);
	g_free(f->clone);
	g_free(f->state);
	g_free(f->make);
	g_free(f->make_log);
	g_free(f);
}
