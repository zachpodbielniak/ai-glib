/*
 * test-ai-build-info.c - Build provenance, and the stamp that carries it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two halves. The API answers for the library this test linked against.
 * The generator, build-aux/gen-build-stamp.sh, is driven against a
 * throwaway repository to prove the property the Makefile relies on: the
 * header changes when a commit, the dirty flag or an install path
 * changes, and is left byte-for-byte alone otherwise -- or every `make`
 * would relink everything.
 *
 * HOME and the working directory are sandboxed.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <sys/stat.h>

#include "ai-glib.h"

static gchar *script = NULL;

static void
test_api(void)
{
	const gchar *commit = ai_build_info_get_commit();
	const gchar *describe = ai_build_info_get_describe();
	g_autofree gchar *summary = ai_build_info_dup_summary();
	g_autofree gchar *prefix = NULL;
	gsize i;

	g_assert_cmpstr(ai_build_info_get_version(), ==, AI_GLIB_VERSION_STRING);
	g_assert_nonnull(ai_build_info_get_date());
	g_assert_cmpuint(strlen(ai_build_info_get_date()), ==, strlen("2026-09-27T12:00:00Z"));
	g_assert_nonnull(ai_build_info_get_prefix());

	prefix = g_strdup_printf("%s (", AI_GLIB_VERSION_STRING);
	g_assert_true(g_str_has_prefix(summary, prefix));
	g_assert_nonnull(strstr(summary, ai_build_info_get_date()));

	/* Either the build came from git and says which commit, or neither. */
	g_assert_true((commit == NULL) == (describe == NULL));
	if (commit != NULL)
	{
		g_assert_cmpuint(strlen(commit), ==, 40);
		for (i = 0; commit[i] != '\0'; i++)
			g_assert_true(g_ascii_isxdigit(commit[i]));
		g_assert_nonnull(strstr(summary, describe));
		g_assert_true(ai_build_info_get_dirty() == (strstr(summary, "-dirty") != NULL));
		g_assert_nonnull(ai_build_info_get_source_dir());
		g_assert_true(g_path_is_absolute(ai_build_info_get_source_dir()));
	}
}

/* ----------------------------------------------------------------
 * The generator
 * ---------------------------------------------------------------- */

static void
run(const gchar *cwd, const gchar * const *argv)
{
	g_autoptr(GSubprocessLauncher) launcher =
		g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
		                          G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *err = NULL;

	g_subprocess_launcher_set_cwd(launcher, cwd);
	proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, NULL, &err, &error);
	g_assert_no_error(error);
	if (!g_subprocess_get_successful(proc))
		g_error("%s %s failed: %s", argv[0], argv[1], err);
}

#define GIT(cwd, ...) run((cwd), (const gchar *[]){ "git", __VA_ARGS__, NULL })

static void
stamp(const gchar *out, const gchar *srcdir, const gchar *prefix)
{
	g_autofree gchar *lib = g_strdup_printf("%s/lib", prefix);
	g_autofree gchar *include = g_strdup_printf("%s/include", prefix);
	const gchar *argv[] = { "sh", script, out, srcdir, "1.2.3", prefix, lib, include,
	                        "release", NULL };

	run(srcdir, argv);
}

static gchar *
read_text(const gchar *path)
{
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

/* The value of one #define, unquoted. */
static gchar *
define_of(const gchar *text, const gchar *name)
{
	g_autofree gchar *key = g_strdup_printf("#define %s ", name);
	const gchar *start = strstr(text, key);
	const gchar *end;

	g_assert_nonnull(start);
	start += strlen(key);
	end = strchr(start, '\n');
	if (*start == '"')
		return g_strndup(start + 1, end - start - 2);
	return g_strndup(start, end - start);
}

static gint64
mtime_of(const gchar *path)
{
	GStatBuf st;

	g_assert_cmpint(g_stat(path, &st), ==, 0);
	return (gint64)st.st_mtime;
}

static gchar *
head_of(const gchar *repo)
{
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	gchar *out = NULL;

	proc = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, "git", "-C", repo,
	                        "rev-parse", "HEAD", NULL);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, &out, NULL, &error);
	g_assert_no_error(error);
	return g_strstrip(out);
}

static void
test_stamp_tracks_head(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *file = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *value = NULL;
	g_autofree gchar *head = NULL;
	gint64 written;

	if (script == NULL)
	{
		g_test_skip("the source tree this was built from is not available");
		return;
	}

	root = g_dir_make_tmp("ai-glib-stamp-XXXXXX", &error);
	g_assert_no_error(error);
	repo = g_build_filename(root, "repo", NULL);
	out = g_build_filename(root, "stamp.h", NULL);
	file = g_build_filename(repo, "file", NULL);

	GIT(root, "init", "-q", "-b", "master", repo);
	g_assert_true(g_file_set_contents(file, "one\n", -1, NULL));
	GIT(repo, "add", "file");
	GIT(repo, "commit", "-q", "-m", "one");

	stamp(out, repo, "/opt/ai");
	text = read_text(out);
	head = head_of(repo);
	value = define_of(text, "AI_BUILD_STAMP_COMMIT");
	g_assert_cmpstr(value, ==, head);
	g_clear_pointer(&value, g_free);
	value = define_of(text, "AI_BUILD_STAMP_DIRTY");
	g_assert_cmpstr(value, ==, "0");
	g_clear_pointer(&value, g_free);
	value = define_of(text, "AI_BUILD_STAMP_VERSION");
	g_assert_cmpstr(value, ==, "1.2.3");
	g_clear_pointer(&value, g_free);
	value = define_of(text, "AI_BUILD_STAMP_PREFIX");
	g_assert_cmpstr(value, ==, "/opt/ai");
	g_clear_pointer(&value, g_free);

	/* Nothing changed: not rewritten, not even touched. */
	written = mtime_of(out);
	g_usleep(G_USEC_PER_SEC * 11 / 10);
	stamp(out, repo, "/opt/ai");
	again = read_text(out);
	g_assert_cmpstr(again, ==, text);
	g_assert_cmpint(mtime_of(out), ==, written);
	g_clear_pointer(&again, g_free);

	/* An untracked file is not a dirty tree. */
	{
		g_autofree gchar *untracked = g_build_filename(repo, "untracked", NULL);

		g_assert_true(g_file_set_contents(untracked, "x", -1, NULL));
		stamp(out, repo, "/opt/ai");
		again = read_text(out);
		g_assert_cmpstr(again, ==, text);
		g_clear_pointer(&again, g_free);
	}

	/* An edit is. */
	g_assert_true(g_file_set_contents(file, "two\n", -1, NULL));
	stamp(out, repo, "/opt/ai");
	g_clear_pointer(&text, g_free);
	text = read_text(out);
	value = define_of(text, "AI_BUILD_STAMP_DIRTY");
	g_assert_cmpstr(value, ==, "1");
	g_clear_pointer(&value, g_free);

	/* A commit moves the stamp to the new HEAD, clean again. */
	GIT(repo, "commit", "-q", "-am", "two");
	stamp(out, repo, "/opt/ai");
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&head, g_free);
	text = read_text(out);
	head = head_of(repo);
	value = define_of(text, "AI_BUILD_STAMP_COMMIT");
	g_assert_cmpstr(value, ==, head);
	g_clear_pointer(&value, g_free);
	value = define_of(text, "AI_BUILD_STAMP_DIRTY");
	g_assert_cmpstr(value, ==, "0");
	g_clear_pointer(&value, g_free);

	/* So does an install path. */
	stamp(out, repo, "/usr/local");
	g_clear_pointer(&text, g_free);
	text = read_text(out);
	value = define_of(text, "AI_BUILD_STAMP_PREFIX");
	g_assert_cmpstr(value, ==, "/usr/local");
	g_clear_pointer(&value, g_free);

	/* A path that needs escaping stays one C string. */
	stamp(out, repo, "/opt/we\"ird\\path");
	g_clear_pointer(&text, g_free);
	text = read_text(out);
	g_assert_nonnull(strstr(text, "#define AI_BUILD_STAMP_PREFIX \"/opt/we\\\"ird\\\\path\"\n"));

	g_unlink(out);
	g_unlink(file);
}

/*
 * Outside git: a fresh stamp says so, and an existing one is left alone
 * -- `sudo make install` in a tree git will not read must not overwrite
 * the provenance with "unknown". A tree that is only a subdirectory of
 * some other repository is not a git checkout of its own.
 */
static void
test_stamp_outside_git(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *root = NULL;
	g_autofree gchar *plain = NULL;
	g_autofree gchar *repo = NULL;
	g_autofree gchar *vendored = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *value = NULL;

	if (script == NULL)
	{
		g_test_skip("the source tree this was built from is not available");
		return;
	}

	root = g_dir_make_tmp("ai-glib-stamp-XXXXXX", &error);
	g_assert_no_error(error);
	plain = g_build_filename(root, "plain", NULL);
	repo = g_build_filename(root, "repo", NULL);
	vendored = g_build_filename(repo, "vendored", NULL);
	out = g_build_filename(root, "stamp.h", NULL);
	g_assert_cmpint(g_mkdir(plain, 0755), ==, 0);

	stamp(out, plain, "/opt/ai");
	text = read_text(out);
	value = define_of(text, "AI_BUILD_STAMP_COMMIT");
	g_assert_cmpstr(value, ==, "");
	g_clear_pointer(&value, g_free);

	g_assert_true(g_file_set_contents(out, "/* kept */\n", -1, NULL));
	stamp(out, plain, "/opt/ai");
	g_clear_pointer(&text, g_free);
	text = read_text(out);
	g_assert_cmpstr(text, ==, "/* kept */\n");
	g_unlink(out);

	GIT(root, "init", "-q", "-b", "master", repo);
	g_assert_cmpint(g_mkdir(vendored, 0755), ==, 0);
	GIT(repo, "commit", "-q", "--allow-empty", "-m", "parent project");
	stamp(out, vendored, "/opt/ai");
	g_clear_pointer(&text, g_free);
	text = read_text(out);
	value = define_of(text, "AI_BUILD_STAMP_COMMIT");
	g_assert_cmpstr(value, ==, "");
	g_unlink(out);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *home = NULL;
	const gchar *source_dir;

	g_test_init(&argc, &argv, NULL);

	home = g_dir_make_tmp("ai-glib-build-info-XXXXXX", NULL);
	g_setenv("HOME", home, TRUE);
	g_setenv("GIT_CONFIG_NOSYSTEM", "1", TRUE);
	g_setenv("GIT_AUTHOR_NAME", "Test", TRUE);
	g_setenv("GIT_AUTHOR_EMAIL", "test@example.invalid", TRUE);
	g_setenv("GIT_COMMITTER_NAME", "Test", TRUE);
	g_setenv("GIT_COMMITTER_EMAIL", "test@example.invalid", TRUE);
	g_unsetenv("SOURCE_DATE_EPOCH");
	g_assert_cmpint(g_chdir(home), ==, 0);

	/* The generator is found through the provenance it generates. */
	source_dir = ai_build_info_get_source_dir();
	if (source_dir != NULL)
	{
		script = g_build_filename(source_dir, "build-aux", "gen-build-stamp.sh", NULL);
		if (!g_file_test(script, G_FILE_TEST_IS_REGULAR))
			g_clear_pointer(&script, g_free);
	}

	g_test_add_func("/ai-glib/build-info/api", test_api);
	g_test_add_func("/ai-glib/build-info/stamp-tracks-head", test_stamp_tracks_head);
	g_test_add_func("/ai-glib/build-info/stamp-outside-git", test_stamp_outside_git);

	return g_test_run();
}
