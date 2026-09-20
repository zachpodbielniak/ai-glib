/*
 * test-ai-gui-work.c - The dashboard's data layer, without a display
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The registry these read and write is shared with ai-tui, so the
 * ordering assertions here are not decoration: two dashboards that
 * sorted one set of records differently would be two answers to "what
 * needs me next".
 *
 * XDG_STATE_HOME and the working directory are sandboxed. A suite that
 * read the developer's real session registry would pass or fail by
 * whose machine ran it.
 */

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <ai-glib.h>

#include "ai-gui-work.h"

typedef struct
{
	gchar *root;
	gchar *registry;
	gchar *original_cwd;
} Fixture;

static void
fixture_set_up(
	Fixture       *fixture,
	gconstpointer  data
){
	fixture->original_cwd = g_get_current_dir();
	fixture->root = g_dir_make_tmp("ai-gui-work-XXXXXX", NULL);
	g_assert_nonnull(fixture->root);

	fixture->registry = g_build_filename(fixture->root, "sessions", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->registry, 0700), ==, 0);

	g_setenv("HOME", fixture->root, TRUE);
	g_setenv("XDG_STATE_HOME", fixture->root, TRUE);
}

static void
fixture_tear_down(
	Fixture       *fixture,
	gconstpointer  data
){
	if (fixture->original_cwd != NULL)
		g_chdir(fixture->original_cwd);

	g_free(fixture->original_cwd);
	g_free(fixture->root);
	g_free(fixture->registry);
}

/* A record the registry will read back, written the way a live session
 * writes one. */
static AiWorkSession *
fixture_record(
	Fixture     *fixture,
	const gchar *directory,
	const gchar *status,
	gboolean     live,
	const gchar *native
){
	AiWorkSession *work = ai_work_session_new(directory);
	g_autoptr(GError) error = NULL;

	g_assert_nonnull(work);
	g_object_set(work, "status", status, "provider-session",
	             native != NULL ? native : "", NULL);
	g_assert_true(ai_work_session_save(work, fixture->registry, live, &error));
	g_assert_no_error(error);

	return work;
}

/* ---------------------------------------------------------------- */

/*
 * The order is the whole point of the dashboard: a session waiting on a
 * human outranks one that merely failed, which outranks one still
 * working, and a disconnected one is last.
 */
static void
test_priority_order(void)
{
	g_assert_cmpint(ai_gui_work_priority("INPUT"), <,
	                ai_gui_work_priority("ERROR"));
	g_assert_cmpint(ai_gui_work_priority("ERROR"), <,
	                ai_gui_work_priority("WORK"));
	g_assert_cmpint(ai_gui_work_priority("WORK"), <,
	                ai_gui_work_priority("DONE"));
	g_assert_cmpint(ai_gui_work_priority("DONE"), <,
	                ai_gui_work_priority("STOPPED"));
	g_assert_cmpint(ai_gui_work_priority("STOPPED"), <,
	                ai_gui_work_priority("IDLE"));
	g_assert_cmpint(ai_gui_work_priority("IDLE"), <,
	                ai_gui_work_priority("DISCONNECTED"));

	/* An unrecognised state sorts last rather than first: a record this
	 * build does not understand must not jump the queue. */
	g_assert_cmpint(ai_gui_work_priority("SOMETHING-NEW"), ==,
	                ai_gui_work_priority("DISCONNECTED"));
	g_assert_cmpint(ai_gui_work_priority(NULL), ==,
	                ai_gui_work_priority("DISCONNECTED"));
}

static void
test_compare_is_total(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiWorkSession) busy = ai_work_session_new(fixture->root);
	g_autoptr(AiWorkSession) waiting = ai_work_session_new(fixture->root);
	AiWorkSession *left = busy;
	AiWorkSession *right = waiting;

	g_object_set(busy, "status", "WORK", NULL);
	g_object_set(waiting, "status", "INPUT", NULL);

	g_assert_cmpint(ai_gui_work_compare(&left, &right), >, 0);
	g_assert_cmpint(ai_gui_work_compare(&right, &left), <, 0);

	/* Same state, same project: the id breaks the tie, so two rows never
	 * swap places between one refresh and the next. */
	g_object_set(waiting, "status", "WORK", NULL);
	g_assert_cmpint(ai_gui_work_compare(&left, &right), !=, 0);
	g_assert_cmpint(ai_gui_work_compare(&left, &left), ==, 0);
}

static void
test_list_is_sorted(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiWorkSession) idle = NULL;
	g_autoptr(AiWorkSession) attention = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GError) error = NULL;

	idle = fixture_record(fixture, fixture->root, "IDLE", TRUE, NULL);
	attention = fixture_record(fixture, fixture->root, "INPUT", TRUE, NULL);

	rows = ai_gui_work_list(fixture->registry, &error);
	g_assert_no_error(error);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 2);

	g_assert_cmpstr(ai_work_session_get_field(g_ptr_array_index(rows, 0),
	                                          "status"), ==, "INPUT");
	g_assert_cmpstr(ai_work_session_get_field(g_ptr_array_index(rows, 1),
	                                          "status"), ==, "IDLE");
}

/*
 * A record whose heartbeat was never claimed to be live comes back
 * DISCONNECTED however it was saved. That is what makes a window that
 * has gone away visible rather than simply absent.
 */
static void
test_liveness_and_resume(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiWorkSession) gone = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	AiWorkSession *row;

	gone = fixture_record(fixture, fixture->root, "DONE", FALSE, "native-42");

	rows = ai_gui_work_list(fixture->registry, NULL);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 1);

	row = g_ptr_array_index(rows, 0);
	g_assert_false(ai_gui_work_is_live(row));
	g_assert_true(ai_gui_work_can_resume(row));
}

static void
test_resume_needs_a_native_id(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiWorkSession) gone = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	gone = fixture_record(fixture, fixture->root, "DONE", FALSE, NULL);

	rows = ai_gui_work_list(fixture->registry, NULL);
	g_assert_nonnull(rows);
	g_assert_cmpuint(rows->len, ==, 1);

	/*
	 * Disconnected but with nothing to resume *from*: its transcript was
	 * never the provider's to keep, so offering the action would promise
	 * a recovery that cannot happen.
	 */
	g_assert_false(ai_gui_work_can_resume(g_ptr_array_index(rows, 0)));
}

/*
 * Two front-ends must not both believe they own one record: each would
 * overwrite the other's heartbeat and the row would flicker between two
 * states. ai-tui takes this same lock, so the exclusion spans both.
 */
static void
test_claim_is_exclusive(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = g_uuid_string_random();
	gint first;
	gint second;

	first = ai_gui_work_claim(fixture->registry, id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(first, >=, 0);

	second = ai_gui_work_claim(fixture->registry, id, &error);
	g_assert_cmpint(second, <, 0);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY);

	g_close(first, NULL);
	g_clear_error(&error);

	/* Released with the descriptor, so a restart can take it again. */
	second = ai_gui_work_claim(fixture->registry, id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(second, >=, 0);
	g_close(second, NULL);
}

/* ---------------------------------------------------------------- */

typedef struct
{
	GMainLoop *loop;
	gchar     *path;
	GError    *error;
} AsyncResult;

static void
on_worktree_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	AsyncResult *out = data;

	out->path = ai_gui_work_create_worktree_finish(result, &out->error);
	g_main_loop_quit(out->loop);
}

static gboolean
run_git(
	const gchar *directory,
	const gchar *first,
	...
){
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GSubprocess) child = NULL;
	const gchar *argument;
	va_list args;

	g_ptr_array_add(argv, (gpointer)"git");
	g_ptr_array_add(argv, (gpointer)"-C");
	g_ptr_array_add(argv, (gpointer)directory);
	g_ptr_array_add(argv, (gpointer)first);

	va_start(args, first);

	while ((argument = va_arg(args, const gchar *)) != NULL)
		g_ptr_array_add(argv, (gpointer)argument);

	va_end(args);
	g_ptr_array_add(argv, NULL);

	child = g_subprocess_newv((const gchar * const *)argv->pdata,
	                          G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
	                          G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);

	return child != NULL && g_subprocess_wait_check(child, NULL, NULL);
}

static void
test_worktree(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autofree gchar *repository = g_build_filename(fixture->root, "repo", NULL);
	g_autofree gchar *file = NULL;
	AsyncResult out;

	if (g_find_program_in_path("git") == NULL)
	{
		g_test_skip("git is not installed");
		return;
	}

	g_assert_cmpint(g_mkdir_with_parents(repository, 0700), ==, 0);
	g_assert_true(run_git(repository, "init", "-q", NULL));
	g_assert_true(run_git(repository, "config", "user.email", "t@example",
	                      NULL));
	g_assert_true(run_git(repository, "config", "user.name", "t", NULL));

	file = g_build_filename(repository, "README", NULL);
	g_assert_true(g_file_set_contents(file, "hello\n", -1, NULL));
	g_assert_true(run_git(repository, "add", "README", NULL));
	g_assert_true(run_git(repository, "commit", "-qm", "first", NULL));

	out.loop = g_main_loop_new(NULL, FALSE);
	out.path = NULL;
	out.error = NULL;

	ai_gui_work_create_worktree_async(repository, NULL, on_worktree_ready,
	                                  &out);
	g_main_loop_run(out.loop);

	g_assert_no_error(out.error);
	g_assert_nonnull(out.path);

	/* An adjacent checkout, not the one branched from: uncommitted work
	 * in the source is deliberately left where it is. */
	g_assert_cmpstr(out.path, !=, repository);
	g_assert_true(g_file_test(out.path, G_FILE_TEST_IS_DIR));

	{
		g_autofree gchar *copied = g_build_filename(out.path, "README", NULL);

		g_assert_true(g_file_test(copied, G_FILE_TEST_EXISTS));
	}

	g_free(out.path);
	g_main_loop_unref(out.loop);
}

/*
 * A directory that is not a checkout fails with git's own words rather
 * than a summary: "not a git repository" is actionable, "could not
 * create a worktree" is not.
 */
static void
test_worktree_refuses_a_plain_directory(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autofree gchar *plain = g_build_filename(fixture->root, "plain", NULL);
	AsyncResult out;

	if (g_find_program_in_path("git") == NULL)
	{
		g_test_skip("git is not installed");
		return;
	}

	g_assert_cmpint(g_mkdir_with_parents(plain, 0700), ==, 0);

	out.loop = g_main_loop_new(NULL, FALSE);
	out.path = NULL;
	out.error = NULL;

	ai_gui_work_create_worktree_async(plain, NULL, on_worktree_ready, &out);
	g_main_loop_run(out.loop);

	g_assert_null(out.path);
	g_assert_nonnull(out.error);
	g_assert_nonnull(strstr(out.error->message, "git worktree add failed"));

	g_clear_error(&out.error);
	g_main_loop_unref(out.loop);
}

/* Registration is asynchronous because ai_work_session_new() runs git,
 * and its own documentation says to keep that off a UI thread. */
static void
on_new_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      data
){
	AsyncResult *out = data;

	out->path = (gchar *)ai_gui_work_new_finish(result, &out->error);
	g_main_loop_quit(out->loop);
}

static void
test_new_is_asynchronous(
	Fixture       *fixture,
	gconstpointer  data
){
	AsyncResult out;
	AiWorkSession *work;

	out.loop = g_main_loop_new(NULL, FALSE);
	out.path = NULL;
	out.error = NULL;

	ai_gui_work_new_async(fixture->root, NULL, on_new_ready, &out);
	g_main_loop_run(out.loop);

	g_assert_no_error(out.error);
	work = (AiWorkSession *)out.path;
	g_assert_nonnull(work);
	g_assert_nonnull(ai_work_session_get_id(work));
	g_assert_cmpstr(ai_work_session_get_field(work, "status"), ==, "IDLE");

	g_object_unref(work);
	g_main_loop_unref(out.loop);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-gui/work/priority-order", test_priority_order);

	g_test_add("/ai-gui/work/compare-is-total", Fixture, NULL,
	           fixture_set_up, test_compare_is_total, fixture_tear_down);
	g_test_add("/ai-gui/work/list-is-sorted", Fixture, NULL,
	           fixture_set_up, test_list_is_sorted, fixture_tear_down);
	g_test_add("/ai-gui/work/liveness-and-resume", Fixture, NULL,
	           fixture_set_up, test_liveness_and_resume, fixture_tear_down);
	g_test_add("/ai-gui/work/resume-needs-a-native-id", Fixture, NULL,
	           fixture_set_up, test_resume_needs_a_native_id,
	           fixture_tear_down);
	g_test_add("/ai-gui/work/claim-is-exclusive", Fixture, NULL,
	           fixture_set_up, test_claim_is_exclusive, fixture_tear_down);
	g_test_add("/ai-gui/work/worktree", Fixture, NULL,
	           fixture_set_up, test_worktree, fixture_tear_down);
	g_test_add("/ai-gui/work/worktree-refuses-a-plain-directory", Fixture,
	           NULL, fixture_set_up, test_worktree_refuses_a_plain_directory,
	           fixture_tear_down);
	g_test_add("/ai-gui/work/new-is-asynchronous", Fixture, NULL,
	           fixture_set_up, test_new_is_asynchronous, fixture_tear_down);

	return g_test_run();
}
