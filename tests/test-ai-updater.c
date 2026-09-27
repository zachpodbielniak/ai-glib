/*
 * test-ai-updater.c - The update check and the update pipeline
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Every case builds a bare "upstream" repository and a clone of it in a
 * temporary directory, and points an AiUpdater at the clone. Nothing
 * here touches the network, the checkout the suite was built from, or a
 * real install: `make` and `sudo` are stub scripts that record their
 * arguments, and the install prefix is a temporary directory.
 *
 * HOME, XDG_STATE_HOME, XDG_CONFIG_HOME and the working directory are
 * sandboxed; the updater's cache and log go under the sandbox, and a
 * suite that read the developer's real state would pass or fail by
 * whose machine ran it.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <unistd.h>

#include "core/ai-error.h"
#include "core/ai-updater.h"

typedef struct
{
	gchar *root;
	gchar *upstream;    /* bare repository */
	gchar *seed;        /* a second clone that pushes "upstream" commits */
	gchar *clone;       /* the checkout the updater is pointed at */
	gchar *prefix;      /* install prefix, writable */
	gchar *state_dir;
	gchar *stub_dir;
	gchar *make_log;
	gchar *build_commit;
	AiUpdater *updater;
} Fixture;

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static gchar *
run_argv(const gchar *cwd, const gchar * const *argv)
{
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	gchar *out = NULL;
	g_autofree gchar *err = NULL;

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_set_cwd(launcher, cwd);
	proc = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8(proc, NULL, NULL, &out, &err, &error);
	g_assert_no_error(error);
	if (!g_subprocess_get_successful(proc))
		g_error("%s failed in %s: %s", argv[1], cwd, err);
	return g_strstrip(out);
}

#define GIT(cwd, ...) \
	g_free(run_argv((cwd), (const gchar *[]){ "git", __VA_ARGS__, NULL }))

static gchar *
git_out(const gchar *cwd, const gchar *arg1, const gchar *arg2)
{
	const gchar *argv[] = { "git", arg1, arg2, NULL };

	return run_argv(cwd, argv);
}

static void
write_file(const gchar *dir, const gchar *name, const gchar *contents)
{
	g_autofree gchar *path = g_build_filename(dir, name, NULL);
	g_autoptr(GError) error = NULL;

	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);
}

static gchar *
read_file(const gchar *path)
{
	gchar *contents = NULL;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return g_strdup("");
	return contents;
}

static void
commit_file(const gchar *repo, const gchar *name, const gchar *contents)
{
	write_file(repo, name, contents);
	GIT(repo, "add", name);
	GIT(repo, "commit", "-q", "-m", name);
}

/* A commit in the seed clone, pushed to upstream. */
static void
push_upstream(Fixture *f, const gchar *name)
{
	commit_file(f->seed, name, name);
	GIT(f->seed, "push", "-q", "origin", "master");
}

static void
make_executable(const gchar *path, const gchar *script)
{
	g_autoptr(GError) error = NULL;

	g_file_set_contents(path, script, -1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

/*
 * The stub `make` appends its argv to make.log, one run per line. A file
 * named fail-<target> makes that target exit 2, and sleep-<target> makes
 * it print a line and then sleep, so a cancel has something to cancel.
 */
static void
write_stub_make(Fixture *f)
{
	g_autofree gchar *path = g_build_filename(f->stub_dir, "make", NULL);
	g_autofree gchar *script = g_strdup_printf(
		"#!/bin/sh\n"
		"echo \"$*\" >> '%s'\n"
		"for arg in \"$@\"; do\n"
		"  if [ -e '%s/sleep-'\"$arg\" ]; then echo started \"$arg\"; exec sleep 30; fi\n"
		"  if [ -e '%s/fail-'\"$arg\" ]; then echo \"stub failure in $arg\" >&2; exit 2; fi\n"
		"done\n"
		"echo \"stub make $*\" >&2\n",
		f->make_log, f->stub_dir, f->stub_dir);

	make_executable(path, script);
}

static void
fixture_set_up(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *make_path = NULL;

	f->root = g_dir_make_tmp("ai-glib-updater-XXXXXX", &error);
	g_assert_no_error(error);
	f->upstream = g_build_filename(f->root, "upstream.git", NULL);
	f->seed = g_build_filename(f->root, "seed", NULL);
	f->clone = g_build_filename(f->root, "clone", NULL);
	f->prefix = g_build_filename(f->root, "prefix", NULL);
	f->state_dir = g_build_filename(f->root, "state", NULL);
	f->stub_dir = g_build_filename(f->root, "stubs", NULL);
	f->make_log = g_build_filename(f->root, "make.log", NULL);
	g_assert_cmpint(g_mkdir(f->prefix, 0755), ==, 0);
	g_assert_cmpint(g_mkdir(f->stub_dir, 0755), ==, 0);

	GIT(f->root, "init", "-q", "--bare", "-b", "master", f->upstream);
	GIT(f->root, "clone", "-q", f->upstream, f->seed);
	GIT(f->seed, "checkout", "-q", "-b", "master");
	commit_file(f->seed, "config.mk",
	            "VERSION_MAJOR = 0\nVERSION_MINOR = 3\nVERSION_MICRO = 0\n");
	GIT(f->seed, "push", "-q", "-u", "origin", "master");
	GIT(f->root, "clone", "-q", f->upstream, f->clone);

	f->build_commit = git_out(f->clone, "rev-parse", "HEAD");
	write_stub_make(f);
	make_path = g_build_filename(f->stub_dir, "make", NULL);

	f->updater = g_object_new(AI_TYPE_UPDATER,
	                          "source-dir", f->clone,
	                          "build-commit", f->build_commit,
	                          "build-version", "0.3.0",
	                          "state-dir", f->state_dir,
	                          "make-program", make_path,
	                          "prefix", f->prefix,
	                          "fetch-timeout", 20,
	                          NULL);
}

static gboolean
remove_tree(const gchar *path)
{
	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autoptr(GFileEnumerator) children = NULL;
	GFileInfo *info;

	/* Put back whatever a privilege test took away, or rm fails. */
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
	return g_rmdir(path) == 0;
}

static void
fixture_tear_down(Fixture *f, gconstpointer data)
{
	ai_updater_stop(f->updater);
	g_clear_object(&f->updater);
	remove_tree(f->root);
	g_free(f->root);
	g_free(f->upstream);
	g_free(f->seed);
	g_free(f->clone);
	g_free(f->prefix);
	g_free(f->state_dir);
	g_free(f->stub_dir);
	g_free(f->make_log);
	g_free(f->build_commit);
}

static AiUpdateStatus *
check(Fixture *f, gboolean fetch)
{
	g_autoptr(GError) error = NULL;
	AiUpdateStatus *status = ai_updater_check(f->updater, fetch, NULL, &error);

	g_assert_no_error(error);
	g_assert_nonnull(status);
	return status;
}

/* ----------------------------------------------------------------
 * The check
 * ---------------------------------------------------------------- */

static void
test_up_to_date(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = check(f, TRUE);

	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UP_TO_DATE);
	g_assert_cmpuint(status->behind, ==, 0);
	g_assert_cmpuint(status->ahead, ==, 0);
	g_assert_false(status->dirty);
	g_assert_false(status->fetch_failed);
	g_assert_cmpstr(status->upstream, ==, "origin/master");
	g_assert_cmpstr(status->branch, ==, "master");
	g_assert_cmpstr(status->head_commit, ==, f->build_commit);
	g_assert_cmpint(status->fetched_at, >, 0);
	g_assert_cmpint(status->attempted_at, ==, status->fetched_at);
}

static void
test_behind(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;

	push_upstream(f, "one");
	push_upstream(f, "two");

	/* Nothing has been fetched yet, so without a fetch nothing is known. */
	status = check(f, FALSE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UP_TO_DATE);
	g_clear_pointer(&status, ai_update_status_free);

	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_BEHIND);
	g_assert_cmpuint(status->behind, ==, 2);
	g_assert_cmpuint(status->checkout_behind, ==, 2);
	g_assert_false(status->pending_restart);
}

/* A pull without an install leaves the binary behind, and says so. */
static void
test_behind_binary_only(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;

	push_upstream(f, "one");
	GIT(f->clone, "pull", "-q", "--ff-only");

	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_BEHIND);
	g_assert_cmpuint(status->behind, ==, 1);
	g_assert_cmpuint(status->checkout_behind, ==, 0);
}

static void
test_diverged(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;

	push_upstream(f, "upstream-change");
	commit_file(f->clone, "local-change", "mine");

	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_DIVERGED);
	g_assert_cmpuint(status->ahead, ==, 1);
	g_assert_cmpuint(status->checkout_behind, ==, 1);
}

static void
test_local_changes(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;

	/* Untracked files are not local changes: a build tree is full of them. */
	write_file(f->clone, "untracked", "x");
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UP_TO_DATE);
	g_clear_pointer(&status, ai_update_status_free);

	write_file(f->clone, "config.mk", "edited\n");
	push_upstream(f, "one");
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_LOCAL_CHANGES);
	g_assert_true(status->dirty);
	g_assert_cmpuint(status->behind, ==, 1);
}

static void
test_not_a_repo(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autofree gchar *plain = g_build_filename(f->root, "plain", NULL);
	g_autofree gchar *missing = g_build_filename(f->root, "missing", NULL);

	g_assert_cmpint(g_mkdir(plain, 0755), ==, 0);
	g_object_set(f->updater, "source-dir", plain, NULL);
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "is not a git checkout"));
	g_clear_pointer(&status, ai_update_status_free);

	g_object_set(f->updater, "source-dir", missing, NULL);
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "does not exist"));
	g_clear_pointer(&status, ai_update_status_free);

	g_object_set(f->updater, "source-dir", NULL, NULL);
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_clear_pointer(&status, ai_update_status_free);

	/* A subdirectory of the right repository is still not its top. */
	{
		g_autofree gchar *sub = g_build_filename(f->clone, "sub", NULL);

		g_assert_cmpint(g_mkdir(sub, 0755), ==, 0);
		g_object_set(f->updater, "source-dir", sub, NULL);
		status = check(f, TRUE);
		g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
		g_assert_nonnull(strstr(status->detail, "not the top"));
	}
}

/* Never guess: a repository that does not contain the build is not it. */
static void
test_other_repo(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autofree gchar *other = g_build_filename(f->root, "other", NULL);

	GIT(f->root, "init", "-q", "-b", "master", other);
	commit_file(other, "file", "unrelated");
	g_object_set(f->updater, "source-dir", other, NULL);

	status = check(f, FALSE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "does not contain"));
	g_clear_pointer(&status, ai_update_status_free);

	/* And a build that did not come from git cannot find its checkout. */
	g_object_set(f->updater, "source-dir", f->clone, "build-commit", NULL, NULL);
	status = check(f, FALSE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "not built from a git checkout"));
}

static void
test_non_tracking_branch(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;

	GIT(f->clone, "checkout", "-q", "-b", "feature");
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "does not track origin/master"));
	g_clear_pointer(&status, ai_update_status_free);

	GIT(f->clone, "checkout", "-q", "--detach");
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "detached"));
	g_clear_pointer(&status, ai_update_status_free);

	/* A configured upstream the branch does not track is refused too. */
	GIT(f->clone, "checkout", "-q", "master");
	g_object_set(f->updater, "upstream", "origin/stable", NULL);
	status = check(f, FALSE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "tracks origin/master, not origin/stable"));
}

/*
 * Offline is normal operation: no error, no warning (GTest makes one
 * fatal), the last known refs still answer, and the throttle still moves
 * so a laptop on a train does not retry on every tick.
 */
static void
test_fetch_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) first = check(f, TRUE);
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autofree gchar *nowhere = g_build_filename(f->root, "nowhere.git", NULL);

	GIT(f->clone, "remote", "set-url", "origin", nowhere);
	g_usleep(G_USEC_PER_SEC * 11 / 10);
	status = check(f, TRUE);
	g_assert_true(status->fetch_failed);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UP_TO_DATE);
	g_assert_cmpint(status->fetched_at, ==, first->fetched_at);
	g_assert_cmpint(status->attempted_at, >, first->attempted_at);
}

/* A fetch that hangs is killed at the timeout, and is a failure. */
static void
test_fetch_timeout(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autofree gchar *upload = g_build_filename(f->stub_dir, "hang-upload-pack", NULL);
	gint64 started;

	make_executable(upload, "#!/bin/sh\nexec sleep 30\n");
	GIT(f->clone, "config", "remote.origin.uploadpack", upload);
	g_object_set(f->updater, "fetch-timeout", 1, NULL);

	started = g_get_monotonic_time();
	status = check(f, TRUE);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
	g_assert_true(status->fetch_failed);
	/* The refs from the clone still answer. */
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UP_TO_DATE);
}

/* ----------------------------------------------------------------
 * Cache and interval
 * ---------------------------------------------------------------- */

static void
test_cache_round_trip(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autoptr(AiUpdater) second = NULL;
	g_autofree gchar *make_path = g_build_filename(f->stub_dir, "make", NULL);
	const AiUpdateStatus *cached;

	push_upstream(f, "one");
	status = check(f, TRUE);

	second = g_object_new(AI_TYPE_UPDATER,
	                      "source-dir", f->clone,
	                      "build-commit", f->build_commit,
	                      "state-dir", f->state_dir,
	                      "make-program", make_path,
	                      NULL);
	g_assert_null(ai_updater_get_status(second));
	g_assert_true(ai_updater_load_cache(second));
	cached = ai_updater_get_status(second);
	g_assert_nonnull(cached);
	g_assert_cmpint(cached->state, ==, AI_UPDATE_STATE_BEHIND);
	g_assert_cmpuint(cached->behind, ==, 1);
	g_assert_cmpint(cached->fetched_at, ==, status->fetched_at);
	g_assert_cmpstr(cached->upstream_commit, ==, status->upstream_commit);

	/* A cache written for another checkout is not this one's. */
	g_object_set(second, "source-dir", f->seed, NULL);
	g_assert_false(ai_updater_load_cache(second));
}

static void
on_status_changed(AiUpdater *updater, gpointer data)
{
	(*(guint *)data)++;
}

/*
 * Starting the monitor with a fresh cache re-reads the local state and
 * does not fetch: a restart is not a reason to hit the remote again.
 */
static void
test_start_honours_interval(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) first = check(f, TRUE);
	g_autofree gchar *nowhere = g_build_filename(f->root, "nowhere.git", NULL);
	guint changes = 0;
	const AiUpdateStatus *status;

	g_object_set(f->updater, "interval", 3600, NULL);
	g_assert_true(ai_updater_load_cache(f->updater));
	/* Any fetch now would fail and show up as fetch_failed. */
	GIT(f->clone, "remote", "set-url", "origin", nowhere);

	g_signal_connect(f->updater, "status-changed", G_CALLBACK(on_status_changed), &changes);
	ai_updater_start(f->updater);
	while (changes == 0)
		g_main_context_iteration(NULL, TRUE);

	status = ai_updater_get_status(f->updater);
	g_assert_false(status->fetch_failed);
	g_assert_cmpint(status->attempted_at, ==, first->attempted_at);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UP_TO_DATE);
}

/* Stopped means stopped: no signal after teardown, even mid-check. */
static void
test_stop_mid_check(Fixture *f, gconstpointer data)
{
	guint changes = 0;
	gint i;

	g_signal_connect(f->updater, "status-changed", G_CALLBACK(on_status_changed), &changes);
	ai_updater_start(f->updater);
	ai_updater_stop(f->updater);
	for (i = 0; i < 200 && ai_updater_is_checking(f->updater); i++)
		g_main_context_iteration(NULL, FALSE), g_usleep(10000);
	while (g_main_context_iteration(NULL, FALSE))
		;
	g_assert_cmpuint(changes, ==, 0);
}

/* ----------------------------------------------------------------
 * The pipeline
 * ---------------------------------------------------------------- */

static void
test_run_installs(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiUpdateStatus) after = NULL;
	g_autofree gchar *upstream_head = NULL;
	g_autofree gchar *head = NULL;
	g_autofree gchar *log = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *state_path = g_build_filename(f->state_dir, "update-status.json", NULL);
	g_auto(GStrv) lines = NULL;
	g_autofree gchar *expected_install = NULL;

	push_upstream(f, "one");
	write_file(f->seed, "config.mk", "VERSION_MAJOR = 0\nVERSION_MINOR = 4\nVERSION_MICRO = 0\n");
	GIT(f->seed, "commit", "-q", "-am", "bump");
	GIT(f->seed, "push", "-q", "origin", "master");
	upstream_head = git_out(f->seed, "rev-parse", "HEAD");

	result = ai_updater_run(f->updater, AI_UPDATE_RUN_NONE, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(result->outcome, ==, AI_UPDATE_OUTCOME_INSTALLED);
	g_assert_cmpstr(result->from_commit, ==, f->build_commit);
	g_assert_cmpstr(result->to_commit, ==, upstream_head);
	g_assert_cmpstr(result->from_version, ==, "0.3.0");
	g_assert_cmpstr(result->to_version, ==, "0.4.0");
	{
		g_autofree gchar *summary = ai_update_result_dup_summary(result);
		g_autofree gchar *expected = g_strdup_printf("Installed 0.3.0 (%.12s) -> 0.4.0 (%.12s) into %s. "
		                                             "Restart to use it.",
		                                             f->build_commit, upstream_head, f->prefix);

		g_assert_cmpstr(summary, ==, expected);
	}

	head = git_out(f->clone, "rev-parse", "HEAD");
	g_assert_cmpstr(head, ==, upstream_head);

	log = read_file(f->make_log);
	lines = g_strsplit(g_strstrip(log), "\n", -1);
	g_assert_cmpuint(g_strv_length(lines), ==, 3);
	g_assert_true(g_str_has_prefix(lines[0], "clean"));
	g_assert_true(g_str_has_prefix(lines[1], "-j"));
	g_assert_nonnull(strstr(lines[1], " all "));
	expected_install = g_strdup_printf("install PREFIX=%s", f->prefix);
	g_assert_true(g_str_has_prefix(lines[2], expected_install));

	/* What was installed is recorded, and this process now knows it is
	 * the old one. */
	state = read_file(state_path);
	g_assert_nonnull(strstr(state, "\"last_update\""));
	g_assert_nonnull(strstr(state, upstream_head));
	after = check(f, FALSE);
	g_assert_cmpint(after->state, ==, AI_UPDATE_STATE_BEHIND);
	g_assert_true(after->pending_restart);
	g_assert_nonnull(result->log_path);
	{
		g_autofree gchar *update_log = read_file(result->log_path);

		g_assert_nonnull(strstr(update_log, "stub make"));
		g_assert_nonnull(strstr(update_log, "0.3.0"));
		g_assert_nonnull(strstr(update_log, "0.4.0"));
	}
}

static void
test_run_build_failure_installs_nothing(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *log = NULL;

	push_upstream(f, "one");
	write_file(f->stub_dir, "fail-all", "");

	result = ai_updater_run(f->updater, AI_UPDATE_RUN_NONE, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "nothing was installed"));
	log = read_file(f->make_log);
	g_assert_null(strstr(log, "install"));
}

static void
test_run_tests_gate_install(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *log = NULL;

	push_upstream(f, "one");
	write_file(f->stub_dir, "fail-test", "");
	g_object_set(f->updater, "run-tests", TRUE, NULL);

	result = ai_updater_run(f->updater, AI_UPDATE_RUN_NONE, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	log = read_file(f->make_log);
	g_assert_nonnull(strstr(log, "test"));
	g_assert_null(strstr(log, "install"));
}

static void
assert_refused(Fixture *f, const gchar *fragment)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = git_out(f->clone, "rev-parse", "HEAD");
	g_autofree gchar *after = NULL;
	g_autofree gchar *log = NULL;

	result = ai_updater_run(f->updater, AI_UPDATE_RUN_NONE, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, fragment));
	after = git_out(f->clone, "rev-parse", "HEAD");
	g_assert_cmpstr(before, ==, after);
	log = read_file(f->make_log);
	g_assert_cmpstr(log, ==, "");
}

static void
test_run_refusals(Fixture *f, gconstpointer data)
{
	/* Nothing to do is a refusal too: no rebuild for nothing. */
	assert_refused(f, "Already up to date");

	push_upstream(f, "one");
	write_file(f->clone, "config.mk", "edited\n");
	assert_refused(f, "local changes");
	GIT(f->clone, "checkout", "-q", "--", "config.mk");

	commit_file(f->clone, "local", "mine");
	assert_refused(f, "Diverged");
	GIT(f->clone, "reset", "-q", "--hard", "HEAD~1");

	GIT(f->clone, "checkout", "-q", "-b", "feature");
	assert_refused(f, "does not track");
}

static void
make_prefix_readonly(Fixture *f)
{
	g_autofree gchar *bin = g_build_filename(f->prefix, "bin", NULL);

	g_assert_cmpint(g_mkdir(bin, 0555), ==, 0);
}

/* No terminal: say the exact command, never hang on a password prompt. */
static void
test_run_needs_privilege(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *log = NULL;

	if (geteuid() == 0)
	{
		g_test_skip("root can write anywhere");
		return;
	}

	push_upstream(f, "one");
	make_prefix_readonly(f);

	result = ai_updater_run(f->updater, AI_UPDATE_RUN_NONE, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(result->outcome, ==, AI_UPDATE_OUTCOME_NEEDS_PRIVILEGE);
	g_assert_true(g_str_has_prefix(result->privileged_command, "sudo "));
	g_assert_nonnull(strstr(result->privileged_command, f->clone));
	g_assert_nonnull(strstr(result->privileged_command, " install PREFIX="));
	log = read_file(f->make_log);
	g_assert_nonnull(strstr(log, "all"));
	g_assert_null(strstr(log, "install"));
}

/* With a terminal, the install goes through sudo -- a stub here. */
static void
test_run_interactive_sudo(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sudo = g_build_filename(f->stub_dir, "sudo", NULL);
	g_autofree gchar *sudo_log = g_build_filename(f->root, "sudo.log", NULL);
	g_autofree gchar *script = NULL;
	g_autofree gchar *log = NULL;

	if (geteuid() == 0)
	{
		g_test_skip("root can write anywhere");
		return;
	}

	script = g_strdup_printf("#!/bin/sh\necho \"$*\" >> '%s'\nexec \"$@\"\n", sudo_log);
	make_executable(sudo, script);
	push_upstream(f, "one");
	make_prefix_readonly(f);
	g_object_set(f->updater, "sudo-program", sudo, NULL);

	result = ai_updater_run(f->updater, AI_UPDATE_RUN_INTERACTIVE, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(result->outcome, ==, AI_UPDATE_OUTCOME_INSTALLED);
	log = read_file(sudo_log);
	g_assert_nonnull(strstr(log, " install PREFIX="));
	g_clear_pointer(&log, g_free);
	log = read_file(f->make_log);
	g_assert_nonnull(strstr(log, "install PREFIX="));
}

typedef struct
{
	GPtrArray      *lines;
	GPtrArray      *steps;
	AiUpdateResult *result;
	GError         *error;
	gboolean        done;
	GCancellable   *cancel_on_start;
} AsyncRun;

static void
on_output(AiUpdater *updater, const gchar *line, gpointer data)
{
	AsyncRun *run = data;

	g_ptr_array_add(run->lines, g_strdup(line));
	if (run->cancel_on_start != NULL && g_str_has_prefix(line, "started"))
		g_cancellable_cancel(run->cancel_on_start);
}

static void
on_step(AiUpdater *updater, const gchar *step, gpointer data)
{
	g_ptr_array_add(((AsyncRun *)data)->steps, g_strdup(step));
}

static void
on_run_done(GObject *source, GAsyncResult *res, gpointer data)
{
	AsyncRun *run = data;

	run->result = ai_updater_run_finish(AI_UPDATER(source), res, &run->error);
	run->done = TRUE;
}

static void
test_run_async_streams(Fixture *f, gconstpointer data)
{
	AsyncRun run = { g_ptr_array_new_with_free_func(g_free),
	                 g_ptr_array_new_with_free_func(g_free), NULL, NULL, FALSE, NULL };
	gboolean saw_build = FALSE;
	guint i;

	push_upstream(f, "one");
	g_signal_connect(f->updater, "output", G_CALLBACK(on_output), &run);
	g_signal_connect(f->updater, "step", G_CALLBACK(on_step), &run);
	ai_updater_run_async(f->updater, AI_UPDATE_RUN_NONE, NULL, on_run_done, &run);
	while (!run.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(run.error);
	g_assert_cmpint(run.result->outcome, ==, AI_UPDATE_OUTCOME_INSTALLED);
	for (i = 0; i < run.lines->len; i++)
		saw_build |= g_str_has_prefix(g_ptr_array_index(run.lines, i), "stub make -j");
	g_assert_true(saw_build);
	g_assert_cmpuint(run.steps->len, >=, 5);

	ai_update_result_free(run.result);
	g_ptr_array_unref(run.lines);
	g_ptr_array_unref(run.steps);
}

static void
test_run_cancel(Fixture *f, gconstpointer data)
{
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	AsyncRun run = { g_ptr_array_new_with_free_func(g_free),
	                 g_ptr_array_new_with_free_func(g_free), NULL, NULL, FALSE, NULL };
	g_autofree gchar *log = NULL;
	gint64 started = g_get_monotonic_time();

	push_upstream(f, "one");
	write_file(f->stub_dir, "sleep-all", "");
	run.cancel_on_start = cancellable;
	g_signal_connect(f->updater, "output", G_CALLBACK(on_output), &run);
	ai_updater_run_async(f->updater, AI_UPDATE_RUN_NONE, cancellable, on_run_done, &run);
	while (!run.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_error(run.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_null(run.result);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 20 * G_USEC_PER_SEC);
	log = read_file(f->make_log);
	g_assert_null(strstr(log, "install"));

	g_clear_error(&run.error);
	g_ptr_array_unref(run.lines);
	g_ptr_array_unref(run.steps);
}

static void
test_status_json(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autoptr(AiUpdateStatus) back = NULL;
	g_autoptr(JsonNode) node = NULL;

	push_upstream(f, "one");
	status = check(f, TRUE);
	node = ai_update_status_to_json(status);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "state"), ==, "behind");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(node), "behind"), ==, 1);
	g_assert_nonnull(json_object_get_string_member(json_node_get_object(node), "summary"));

	back = ai_update_status_from_json(node);
	g_assert_nonnull(back);
	g_assert_cmpint(back->state, ==, status->state);
	g_assert_cmpstr(back->upstream_commit, ==, status->upstream_commit);
	g_assert_cmpint(back->fetched_at, ==, status->fetched_at);
}

/* Malformed state files cost themselves: no critical, no status. */
static void
test_cache_malformed(Fixture *f, gconstpointer data)
{
	const gchar *bodies[] = {
		"", "null", "[]", "{\"status\":7}", "{\"status\":{\"state\":7,\"behind\":\"x\"}}",
		"{not json", "{\"version\":1,\"status\":null}",
	};
	g_autofree gchar *path = g_build_filename(f->state_dir, "update-status.json", NULL);
	guint i;

	g_assert_cmpint(g_mkdir_with_parents(f->state_dir, 0700), ==, 0);
	for (i = 0; i < G_N_ELEMENTS(bodies); i++)
	{
		g_assert_true(g_file_set_contents(path, bodies[i], -1, NULL));
		g_assert_false(ai_updater_load_cache(f->updater));
	}
}

/* Names that would be read as options never reach an argv. */
static void
test_option_like_names(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;

	g_object_set(f->updater, "upstream", "--upload-pack=touch/master", NULL);
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "is not a usable REMOTE/BRANCH"));
	g_clear_pointer(&status, ai_update_status_free);

	g_object_set(f->updater, "upstream", "origin/-x", NULL);
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_clear_pointer(&status, ai_update_status_free);

	g_object_set(f->updater, "upstream", NULL, "build-commit", "--all", NULL);
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_nonnull(strstr(status->detail, "is not a commit id"));
	g_clear_pointer(&status, ai_update_status_free);

	/* A state file naming something odd as the last install is ignored. */
	g_object_set(f->updater, "build-commit", f->build_commit, NULL);
	g_assert_cmpint(g_mkdir_with_parents(f->state_dir, 0700), ==, 0);
	write_file(f->state_dir, "update-status.json",
	           "{\"version\":1,\"last_update\":{\"to_commit\":\"--output=/tmp/x\","
	           "\"prefix\":\"/nowhere\"}}");
	push_upstream(f, "one");
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_BEHIND);
	g_assert_false(status->pending_restart);
}

/* Offline before the first fetch: unavailable, and the reason kept. */
static void
test_never_fetched(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autofree gchar *nowhere = g_build_filename(f->root, "nowhere.git", NULL);

	GIT(f->clone, "update-ref", "-d", "refs/remotes/origin/master");
	GIT(f->clone, "remote", "set-url", "origin", nowhere);
	status = check(f, TRUE);
	g_assert_cmpint(status->state, ==, AI_UPDATE_STATE_UNAVAILABLE);
	g_assert_true(status->fetch_failed);
	g_assert_nonnull(strstr(status->detail, "has never been fetched"));
	g_assert_nonnull(strstr(status->detail, "nowhere.git"));
}

/* An install that had the typelib gets a new one, not a stale one. */
static void
test_run_keeps_gir(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *gir_dir = g_build_filename(f->prefix, "lib", "girepository-1.0", NULL);
	g_autofree gchar *log = NULL;
	g_auto(GStrv) lines = NULL;

	g_assert_cmpint(g_mkdir_with_parents(gir_dir, 0755), ==, 0);
	write_file(gir_dir, "AiGlib-1.0.typelib", "");
	push_upstream(f, "one");

	result = ai_updater_run(f->updater, AI_UPDATE_RUN_NONE, NULL, &error);
	g_assert_no_error(error);
	log = read_file(f->make_log);
	lines = g_strsplit(g_strstrip(log), "\n", -1);
	g_assert_cmpuint(g_strv_length(lines), ==, 3);
	g_assert_nonnull(strstr(lines[1], "GIR=1"));
	g_assert_nonnull(strstr(lines[2], "GIR=1"));
}

/*
 * pkexec for the install step alone. The stub records its argv, reads
 * stdin (closed, so it cannot wait on a prompt) and exits with whatever
 * the case stages in pkexec-exit, else runs the command.
 */
static AiUpdateResult *
run_polkit(Fixture *f, const gchar *exit_code, gchar **pk_log, GError **error)
{
	g_autofree gchar *pkexec = g_build_filename(f->stub_dir, "pkexec", NULL);
	g_autofree gchar *log_path = g_build_filename(f->root, "pkexec.log", NULL);
	g_autofree gchar *script = NULL;
	AiUpdateResult *result;

	script = g_strdup_printf("#!/bin/sh\n"
	                         "echo \"$*\" >> '%s'\n"
	                         "read password\n"
	                         "%s\n"
	                         "exec \"$@\"\n",
	                         log_path,
	                         exit_code != NULL ? exit_code : "");
	make_executable(pkexec, script);
	push_upstream(f, "one");
	make_prefix_readonly(f);
	g_object_set(f->updater, "pkexec-program", pkexec, NULL);
	result = ai_updater_run(f->updater, AI_UPDATE_RUN_POLKIT, NULL, error);
	*pk_log = read_file(log_path);
	return result;
}

static void
test_run_polkit_granted(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pk_log = NULL;
	g_autofree gchar *make_log = NULL;
	g_autofree gchar *expected = NULL;

	if (geteuid() == 0)
	{
		g_test_skip("root can write anywhere");
		return;
	}
	result = run_polkit(f, NULL, &pk_log, &error);
	g_assert_no_error(error);
	g_assert_cmpint(result->outcome, ==, AI_UPDATE_OUTCOME_INSTALLED);
	expected = g_strdup_printf(" -C %s install PREFIX=%s", f->clone, f->prefix);
	g_assert_nonnull(strstr(pk_log, expected));
	/* Only the install is elevated: one pkexec run, not the build. */
	g_assert_null(strchr(g_strstrip(pk_log), '\n'));
	g_assert_null(strstr(pk_log, " all "));
	make_log = read_file(f->make_log);
	g_assert_nonnull(strstr(make_log, "install PREFIX="));
}

static void
assert_polkit_declined(Fixture *f, const gchar *exit_code)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pk_log = NULL;
	g_autofree gchar *make_log = NULL;
	gint64 started = g_get_monotonic_time();

	result = run_polkit(f, exit_code, &pk_log, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 15 * G_USEC_PER_SEC);
	g_assert_cmpint(result->outcome, ==, AI_UPDATE_OUTCOME_NEEDS_PRIVILEGE);
	g_assert_true(g_str_has_prefix(result->privileged_command, "sudo "));
	g_assert_nonnull(strstr(pk_log, " install "));
	make_log = read_file(f->make_log);
	g_assert_null(strstr(make_log, "install"));
}

static void
test_run_polkit_dismissed(Fixture *f, gconstpointer data)
{
	if (geteuid() == 0) { g_test_skip("root can write anywhere"); return; }
	assert_polkit_declined(f, "exit 126");
}

static void
test_run_polkit_no_agent(Fixture *f, gconstpointer data)
{
	if (geteuid() == 0) { g_test_skip("root can write anywhere"); return; }
	/* A text agent would read the password from stdin; it is closed, so
	 * this fails at once instead of hanging. */
	assert_polkit_declined(f, "[ -n \"$password\" ] || exit 127");
}

/* A failed install under pkexec is a failure, not a request for help. */
static void
test_run_polkit_install_fails(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *pk_log = NULL;

	if (geteuid() == 0) { g_test_skip("root can write anywhere"); return; }
	result = run_polkit(f, "exit 2", &pk_log, &error);
	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	/* Something may already be copied; saying otherwise would be a lie. */
	g_assert_nonnull(strstr(error->message, "may have left the install incomplete"));
	g_assert_null(strstr(error->message, "nothing was installed"));
}

/* No pkexec on the machine: straight to the command. */
static void
test_run_polkit_absent(Fixture *f, gconstpointer data)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;

	if (geteuid() == 0) { g_test_skip("root can write anywhere"); return; }
	push_upstream(f, "one");
	make_prefix_readonly(f);
	g_object_set(f->updater, "pkexec-program", NULL, NULL);
	result = ai_updater_run(f->updater, AI_UPDATE_RUN_POLKIT, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(result->outcome, ==, AI_UPDATE_OUTCOME_NEEDS_PRIVILEGE);
}

int
main(int argc, char *argv[])
{
	g_autofree gchar *home = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *config = NULL;

	g_test_init(&argc, &argv, NULL);

	home = g_dir_make_tmp("ai-glib-updater-home-XXXXXX", NULL);
	state = g_build_filename(home, "state", NULL);
	config = g_build_filename(home, "config", NULL);
	g_setenv("HOME", home, TRUE);
	g_setenv("XDG_STATE_HOME", state, TRUE);
	g_setenv("XDG_CONFIG_HOME", config, TRUE);
	g_setenv("GIT_CONFIG_NOSYSTEM", "1", TRUE);
	g_setenv("GIO_USE_VFS", "local", TRUE);
	g_setenv("GIT_AUTHOR_NAME", "Test", TRUE);
	g_setenv("GIT_AUTHOR_EMAIL", "test@example.invalid", TRUE);
	g_setenv("GIT_COMMITTER_NAME", "Test", TRUE);
	g_setenv("GIT_COMMITTER_EMAIL", "test@example.invalid", TRUE);
	g_unsetenv("AI_GLIB_SOURCE_DIR");
	g_unsetenv("AI_GLIB_NO_UPDATE_CHECK");
	g_assert_cmpint(g_chdir(home), ==, 0);

#define ADD(path, fn) \
	g_test_add("/ai-glib/updater/" path, Fixture, NULL, fixture_set_up, fn, fixture_tear_down)

	ADD("check/up-to-date", test_up_to_date);
	ADD("check/behind", test_behind);
	ADD("check/behind-binary-only", test_behind_binary_only);
	ADD("check/diverged", test_diverged);
	ADD("check/local-changes", test_local_changes);
	ADD("check/not-a-repo", test_not_a_repo);
	ADD("check/other-repo", test_other_repo);
	ADD("check/non-tracking-branch", test_non_tracking_branch);
	ADD("check/fetch-failure", test_fetch_failure);
	ADD("check/fetch-timeout", test_fetch_timeout);
	ADD("check/option-like-names", test_option_like_names);
	ADD("check/never-fetched", test_never_fetched);
	ADD("run/keeps-gir", test_run_keeps_gir);
	ADD("cache/round-trip", test_cache_round_trip);
	ADD("cache/malformed", test_cache_malformed);
	ADD("cache/json", test_status_json);
	ADD("monitor/start-honours-interval", test_start_honours_interval);
	ADD("monitor/stop-mid-check", test_stop_mid_check);
	ADD("run/installs", test_run_installs);
	ADD("run/build-failure", test_run_build_failure_installs_nothing);
	ADD("run/tests-gate-install", test_run_tests_gate_install);
	ADD("run/refusals", test_run_refusals);
	ADD("run/needs-privilege", test_run_needs_privilege);
	ADD("run/interactive-sudo", test_run_interactive_sudo);
	ADD("run/polkit-granted", test_run_polkit_granted);
	ADD("run/polkit-dismissed", test_run_polkit_dismissed);
	ADD("run/polkit-no-agent", test_run_polkit_no_agent);
	ADD("run/polkit-install-fails", test_run_polkit_install_fails);
	ADD("run/polkit-absent", test_run_polkit_absent);
	ADD("run/async-streams", test_run_async_streams);
	ADD("run/cancel", test_run_cancel);

	return g_test_run();
}
