/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-glib.h"
#include <glib/gstdio.h>
#include <sys/stat.h>

static void
remove_tree(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *name;
	if (dir != NULL)
		while ((name = g_dir_read_name(dir)) != NULL)
		{
			g_autofree gchar *child = g_build_filename(path, name, NULL);
			if (g_file_test(child, G_FILE_TEST_IS_DIR)) remove_tree(child);
			else g_unlink(child);
		}
	g_rmdir(path);
}

static void
test_states(void)
{
	g_autoptr(AiWorkSession) session = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	ai_work_session_update(session, FALSE, FALSE, 0, NULL);
	g_assert_cmpstr(ai_work_session_get_field(session, "status"), ==, "IDLE");
	ai_work_session_update(session, FALSE, FALSE, 0, "DONE");
	g_assert_cmpstr(ai_work_session_get_field(session, "status"), ==, "DONE");
	ai_work_session_update(session, FALSE, FALSE, 1, "DONE");
	g_assert_cmpstr(ai_work_session_get_field(session, "status"), ==, "WORK");
	ai_work_session_update(session, TRUE, TRUE, 1, "ERROR");
	g_assert_cmpstr(ai_work_session_get_field(session, "status"), ==, "INPUT");
	ai_work_session_update(session, FALSE, FALSE, 0, "ERROR");
	g_assert_cmpstr(ai_work_session_get_field(session, "status"), ==, "ERROR");
	ai_work_session_update(session, FALSE, FALSE, 0, "STOPPED");
	g_assert_cmpstr(ai_work_session_get_field(session, "status"), ==, "STOPPED");
}

static void
test_links(void)
{
	g_autoptr(AiWorkSession) session = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_auto(GStrv) links = NULL;
	const gchar *bad[] = {"#42", "file:///tmp/issues/1", "https://user:secret@host/repo/issues/1",
		"https://host/owner/repo/issues/1?token=secret", "https://host/owner/repo/issues/1\033]2;bad",
		"https://host/owner/repo/issues/0", "https://host/owner/repo", "https://host/owner/repo/pull/1#fragment", NULL};
	guint i;
	g_assert_true(ai_work_session_add_link(session, "https://a.example/owner/repo/issues/42", NULL));
	g_assert_true(ai_work_session_add_link(session, "https://b.example/owner/repo/issues/42", NULL));
	g_assert_true(ai_work_session_add_link(session, "https://a.example/owner/repo/issues/42", NULL));
	g_assert_true(ai_work_session_add_link(session, "https://gitlab.com/group/sub/repo/-/merge_requests/3", NULL));
	links = ai_work_session_dup_links(session);
	g_assert_cmpuint(g_strv_length(links), ==, 3);
	for (i = 0; bad[i]; i++)
	{
		g_autoptr(GError) error = NULL;
		g_assert_false(ai_work_session_add_link(session, bad[i], &error));
		g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	}
	g_assert_true(ai_work_session_remove_link(session, links[0]));
	g_assert_false(ai_work_session_remove_link(session, links[0]));
}

static void
test_list_ownership(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-session-ownership-XXXXXX", NULL);
	g_autoptr(AiWorkSession) session = ai_work_session_new(dir);
	g_autoptr(AiWorkSession) retained = NULL;
	g_autolist(AiWorkSession) rows = NULL;
	g_auto(GStrv) links = NULL;
	g_autofree gchar *title = NULL;
	gpointer weak = NULL;
	const gchar *url = "https://example.invalid/team/project/issues/42";

	g_object_set(session, "title", "Retained assignment", NULL);
	g_assert_true(ai_work_session_add_link(session, url, NULL));
	g_assert_true(ai_work_session_save(session, dir, TRUE, NULL));
	rows = ai_work_session_list(dir, NULL);
	g_assert_cmpuint(g_list_length(rows), ==, 1);
	retained = g_object_ref(rows->data);
	weak = retained;
	g_object_add_weak_pointer(G_OBJECT(retained), &weak);
	g_clear_list(&rows, g_object_unref);
	g_assert_nonnull(weak);
	g_object_get(retained, "title", &title, NULL);
	g_assert_cmpstr(title, ==, "Retained assignment");
	links = ai_work_session_dup_links(retained);
	g_clear_object(&retained);
	g_assert_null(weak);
	/* Duplicated links must outlive both the returned list and its objects. */
	g_assert_cmpstr(links[0], ==, url);
	g_assert_null(links[1]);
	remove_tree(dir);
}

static void
test_persistence(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-work-session-XXXXXX", NULL);
	g_autoptr(AiWorkSession) session = ai_work_session_new(dir);
	g_autoptr(AiWorkSession) other = ai_work_session_new(dir);
	g_autolist(AiWorkSession) rows = NULL;
	g_autofree gchar *path = g_build_filename(dir, ai_work_session_get_id(session), NULL);
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_autofree gchar *data = NULL;
	GStatBuf st;
	guint i;
	g_assert_cmpstr(ai_work_session_get_field(session, "project"), ==, dir);
	g_object_set(session, "title", "Fix Unicode café\033 title", NULL);
	g_assert_true(ai_work_session_add_link(session, "https://example.com/owner/repo/pulls/2", NULL));
	ai_work_session_update(session, TRUE, FALSE, 0, NULL);
	g_assert_true(ai_work_session_save(session, dir, TRUE, NULL));
	g_assert_true(ai_work_session_save(other, dir, FALSE, NULL));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	rows = ai_work_session_list(dir, NULL);
	g_assert_cmpuint(g_list_length(rows), ==, 2);
	for (i = 0; i < g_list_length(rows); i++)
	{
		AiWorkSession *row = g_list_nth_data(rows, i);
		if (g_str_equal(ai_work_session_get_id(row), ai_work_session_get_id(session)))
		{
			g_auto(GStrv) links = ai_work_session_dup_links(row);
			g_assert_cmpstr(ai_work_session_get_field(row, "status"), ==, "WORK");
			g_assert_cmpstr(links[0], ==, "https://example.com/owner/repo/pulls/2");
			g_assert_null(strchr(ai_work_session_get_field(row, "title"), '\033'));
		}
		else g_assert_cmpstr(ai_work_session_get_field(row, "status"), ==, "DISCONNECTED");
	}
	g_assert_true(g_key_file_load_from_file(file, path, 0, NULL));
	g_key_file_set_int64(file, "session", "heartbeat", g_get_real_time() - 16 * G_USEC_PER_SEC);
	data = g_key_file_to_data(file, NULL, NULL);
	g_assert_true(g_file_set_contents(path, data, -1, NULL));
	g_clear_list(&rows, g_object_unref);
	rows = ai_work_session_list(dir, NULL);
	for (i = 0; i < g_list_length(rows); i++)
		g_assert_cmpstr(ai_work_session_get_field(g_list_nth_data(rows, i), "status"), ==, "DISCONNECTED");
	g_assert_true(g_file_set_contents(path, "[session]\nversion=99\n", -1, NULL));
	g_clear_list(&rows, g_object_unref);
	rows = ai_work_session_list(dir, NULL);
	g_assert_cmpuint(g_list_length(rows), ==, 1);
	remove_tree(dir);
}

static void
test_config(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-dashboard-config-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "config.yaml", NULL);
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(GError) error = NULL;
	gboolean enabled = FALSE;
	g_assert_true(g_file_set_contents(path, "apps:\n  ai-tui:\n    open-dashboard-on-load: true\n", -1, NULL));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_no_error(error);
	g_object_get(config, "open-dashboard-on-load", &enabled, NULL);
	g_assert_true(enabled);
	g_assert_true(g_file_set_contents(path, "apps:\n  ai-tui:\n    open-dashboard-on-load: [true]\n", -1, NULL));
	g_assert_false(ai_config_load_from_file(config, path, &error));
	g_assert_nonnull(error);
	g_object_get(config, "open-dashboard-on-load", &enabled, NULL);
	g_assert_true(enabled);
	/* The preference is per application: ai-gui has a dashboard of its own
	 * and each front-end may want a different default. Reading it only for
	 * ai-tui's index meant apps.ai-gui.open-dashboard-on-load validated and
	 * was then silently discarded. */
	g_clear_error(&error);
	g_assert_true(g_file_set_contents(path,
		"apps:\n  ai-tui:\n    open-dashboard-on-load: false\n"
		"  ai-gui:\n    open-dashboard-on-load: true\n", -1, NULL));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_no_error(error);
	g_assert_true(ai_config_get_app_dashboard(config, "ai-gui"));
	g_assert_false(ai_config_get_app_dashboard(config, "ai-tui"));
	g_object_get(config, "open-dashboard-on-load", &enabled, NULL);
	g_assert_false(enabled);
	g_assert_false(ai_config_get_app_dashboard(config, "ai"));
	remove_tree(dir);
}

static gchar *fetch_text;
static GError *fetch_error;
static gboolean fetch_done;

static void
fetched(GObject *source, GAsyncResult *result, gpointer data)
{
	(void)data;
	fetch_text = ai_work_session_refresh_link_finish(AI_WORK_SESSION(source), result, &fetch_error);
	fetch_done = TRUE;
}

static void
test_fetch(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-work-fetch-XXXXXX", NULL);
	g_autofree gchar *stub = g_build_filename(dir, "gh", NULL);
	g_autofree gchar *old_path = g_strdup(g_getenv("PATH"));
	g_autofree gchar *path = g_strconcat(dir, ":", old_path, NULL);
	g_autoptr(AiWorkSession) session = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	const gchar *url = "https://github.com/example/repo/issues/12";
	g_assert_true(g_file_set_contents(stub, "#!/bin/sh\nprintf '%s' '{\"title\":\"Repair parser\",\"state\":\"OPEN\",\"body\":\"Must preserve UTF-8\"}'\n", -1, NULL));
	g_assert_cmpint(g_chmod(stub, 0700), ==, 0);
	g_setenv("PATH", path, TRUE);
	g_assert_true(ai_work_session_add_link(session, url, NULL));
	fetch_done = FALSE;
	ai_work_session_refresh_link_async(session, url, NULL, fetched, NULL);
	while (!fetch_done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(fetch_error);
	g_assert_nonnull(strstr(fetch_text, "Must preserve UTF-8"));
	g_assert_cmpstr(ai_work_session_get_link_title(session, url), ==, "Repair parser");
	g_clear_pointer(&fetch_text, g_free);
	g_assert_true(g_file_set_contents(stub, "#!/bin/sh\nprintf '%s' '{\"title\":7,\"state\":false}'\n", -1, NULL));
	fetch_done = FALSE;
	ai_work_session_refresh_link_async(session, url, NULL, fetched, NULL);
	while (!fetch_done) g_main_context_iteration(NULL, TRUE);
	g_assert_error(fetch_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error(&fetch_error);
	g_assert_cmpstr(ai_work_session_get_link_title(session, url), ==, "Repair parser");
	g_cancellable_cancel(cancel);
	fetch_done = FALSE;
	ai_work_session_refresh_link_async(session, url, cancel, fetched, NULL);
	while (!fetch_done) g_main_context_iteration(NULL, TRUE);
	g_assert_error(fetch_error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&fetch_error);
	g_setenv("PATH", old_path, TRUE);
	remove_tree(dir);
}

static void
run_git(const gchar *directory, const gchar * const *args)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GSubprocess) child = NULL;
	guint i;
	g_ptr_array_add(argv, "git"); g_ptr_array_add(argv, "-C"); g_ptr_array_add(argv, (gpointer)directory);
	g_ptr_array_add(argv, "-c"); g_ptr_array_add(argv, "core.hooksPath=/dev/null");
	g_ptr_array_add(argv, "-c"); g_ptr_array_add(argv, "commit.gpgsign=false");
	for (i = 0; args[i] != NULL; i++) g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	child = g_subprocess_newv((const gchar * const *)argv->pdata,
		G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
	g_assert_nonnull(child); g_assert_true(g_subprocess_wait_check(child, NULL, NULL));
}

static void
test_project_identity(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-project-identity-XXXXXX", NULL);
	g_autofree gchar *repo = g_build_filename(dir, "repo", NULL);
	g_autofree gchar *tree = g_build_filename(dir, "task", NULL);
	g_autoptr(AiWorkSession) first = NULL;
	g_autoptr(AiWorkSession) second = NULL;
	const gchar *init[] = {"init", "--template=", "-q", repo, NULL};
	const gchar *commit[] = {"-c", "user.name=Test", "-c", "user.email=test@example.invalid",
		"commit", "--allow-empty", "-qm", "initial", NULL};
	const gchar *worktree[] = {"worktree", "add", "-b", "task", tree, NULL};
	run_git(dir, init); run_git(repo, commit); run_git(repo, worktree);
	first = ai_work_session_new(repo); second = ai_work_session_new(tree);
	g_assert_cmpstr(ai_work_session_get_field(first, "project"), ==, ai_work_session_get_field(second, "project"));
	g_assert_cmpstr(ai_work_session_get_field(first, "directory"), !=, ai_work_session_get_field(second, "directory"));
	g_assert_cmpstr(ai_work_session_get_field(second, "branch"), ==, "task");
	g_assert_cmpstr(ai_work_session_get_id(first), !=, ai_work_session_get_id(second));
	remove_tree(dir);
}

static void
test_failed_write(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-session-error-XXXXXX", NULL);
	g_autofree gchar *file = g_build_filename(dir, "not-a-directory", NULL);
	g_autofree gchar *path = g_build_filename(file, "sessions", NULL);
	g_autoptr(AiWorkSession) session = ai_work_session_new(dir);
	g_autoptr(GError) error = NULL;
	g_assert_true(g_file_set_contents(file, "existing content", -1, NULL));
	g_assert_false(ai_work_session_save(session, path, TRUE, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_null(ai_work_session_list(file, &error));
	g_assert_nonnull(error);
	remove_tree(dir);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/work-session/states", test_states);
	g_test_add_func("/work-session/project-identity", test_project_identity);
	g_test_add_func("/work-session/failed-write", test_failed_write);
	g_test_add_func("/work-session/links", test_links);
	g_test_add_func("/work-session/persistence", test_persistence);
	g_test_add_func("/work-session/list-ownership", test_list_ownership);
	g_test_add_func("/work-session/config", test_config);
	g_test_add_func("/work-session/fetch", test_fetch);
	return g_test_run();
}
