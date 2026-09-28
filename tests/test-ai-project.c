/*
 * test-ai-project.c - AiProject: the work registry, grouped by project
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ai-tui, ai-gui and `ai project` all read one registry. The label, the
 * grouping and the "what needs me next" order are asserted here once, so
 * no front-end can drift into its own answer. Every case uses a private
 * registry directory: a suite that read the developer's real one would
 * pass or fail by whose machine ran it.
 */
#include <string.h>
#include <glib/gstdio.h>
#include <ai-glib.h>

typedef struct
{
	gchar *dir;
} Fixture;

static void
save_session(Fixture *fx, const gchar *project, const gchar *directory,
	const gchar *status, gboolean live, const gchar *title)
{
	g_autoptr(AiWorkSession) session = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_autoptr(GError) error = NULL;
	g_object_set(session, "project", project, "directory", directory,
		"status", status, "title", title, "branch", "main", NULL);
	g_assert_true(ai_work_session_save(session, fx->dir, live, &error));
	g_assert_no_error(error);
}

static void
fixture_set_up(Fixture *fx, gconstpointer data)
{
	fx->dir = g_dir_make_tmp("ai-project-XXXXXX", NULL);
	g_assert_nonnull(fx->dir);
	(void)data;
}

static void
fixture_tear_down(Fixture *fx, gconstpointer data)
{
	g_autoptr(GDir) dir = g_dir_open(fx->dir, 0, NULL);
	const gchar *name;
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = g_build_filename(fx->dir, name, NULL);
		g_unlink(path);
	}
	g_rmdir(fx->dir);
	g_free(fx->dir);
	(void)data;
}

/* The standard registry: two projects with work, one plain directory. */
static void
seed(Fixture *fx)
{
	save_session(fx, "/r/alpha/.git", "/r/alpha", "WORK", TRUE, "alpha main");
	save_session(fx, "/r/alpha/.git", "/wt/alpha-feature", "IDLE", FALSE, "alpha worktree");
	save_session(fx, "/r/beta/.git", "/r/beta", "INPUT", TRUE, "beta asks");
	save_session(fx, "/home/me/notes", "/home/me/notes", "IDLE", TRUE, "notes");
}

static AiProject *
find_named(GList *projects, const gchar *name)
{
	GList *l;
	for (l = projects; l != NULL; l = l->next)
		if (g_strcmp0(ai_project_get_name(l->data), name) == 0) return l->data;
	return NULL;
}

static void
test_label(void)
{
	const struct { const gchar *path; const gchar *label; } cases[] = {
		{ "/r/alpha/.git", "alpha" },
		{ "/r/alpha/.git/", "alpha" },
		{ "/r/alpha", "alpha" },
		{ "/r/alpha///", "alpha" },
		{ "/srv/bare.git", "bare.git" },
		{ "/.git", "/.git" },
		{ "/", "/" },
		{ "", "Untitled" },
		{ NULL, "Untitled" },
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *label = ai_project_label_for_path(cases[i].path);
		g_assert_cmpstr(label, ==, cases[i].label);
	}
	/* Same label, different identity: order by label, then by path. */
	g_assert_cmpint(ai_project_compare_paths("/a/Alpha/.git", "/b/beta/.git"), <, 0);
	g_assert_cmpint(ai_project_compare_paths("/a/dup/.git", "/b/dup/.git"), <, 0);
	g_assert_cmpint(ai_project_compare_paths("/b/dup/.git", "/a/dup/.git"), >, 0);
	g_assert_cmpint(ai_project_compare_paths("/a/dup/.git", "/a/dup/.git"), ==, 0);
}

static void
test_priority(void)
{
	const gchar *order[] = { "INPUT", "ERROR", "WORK", "DONE", "STOPPED", "IDLE", "DISCONNECTED" };
	guint i;
	for (i = 1; i < G_N_ELEMENTS(order); i++)
		g_assert_cmpint(ai_work_session_status_priority(order[i - 1]), <,
		                ai_work_session_status_priority(order[i]));
	/* An unknown or missing status sorts with the least urgent. */
	g_assert_cmpint(ai_work_session_status_priority("GARBAGE"), ==, ai_work_session_status_priority("DISCONNECTED"));
	g_assert_cmpint(ai_work_session_status_priority(NULL), ==, ai_work_session_status_priority("DISCONNECTED"));
}

static void
test_list(Fixture *fx, gconstpointer data)
{
	g_autolist(AiProject) projects = NULL;
	g_autoptr(GError) error = NULL;
	AiProject *alpha;
	g_autofree gchar *status = NULL;
	guint sessions = 0;
	seed(fx);
	projects = ai_project_list(fx->dir, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_list_length(projects), ==, 3);
	/* Most urgent first: beta is waiting on a person. */
	g_assert_cmpstr(ai_project_get_name(g_list_nth_data(projects, 0)), ==, "beta");
	g_assert_cmpstr(ai_project_get_name(g_list_nth_data(projects, 1)), ==, "alpha");
	g_assert_cmpstr(ai_project_get_name(g_list_nth_data(projects, 2)), ==, "notes");
	alpha = find_named(projects, "alpha");
	g_assert_cmpstr(ai_project_get_id(alpha), ==, "/r/alpha/.git");
	g_assert_cmpstr(ai_project_get_root(alpha), ==, "/r/alpha");
	g_assert_cmpstr(ai_project_get_status(alpha), ==, "WORK");
	g_assert_cmpuint(ai_project_get_session_count(alpha), ==, 2);
	g_assert_cmpuint(ai_project_get_live_count(alpha), ==, 1);
	g_assert_cmpuint(ai_project_get_busy_count(alpha), ==, 1);
	g_assert_cmpuint(ai_project_get_attention_count(alpha), ==, 0);
	g_assert_cmpuint(ai_project_get_attention_count(find_named(projects, "beta")), ==, 1);
	/* Properties are the same answers, for bindings. */
	g_object_get(alpha, "status", &status, "session-count", &sessions, NULL);
	g_assert_cmpstr(status, ==, "WORK");
	g_assert_cmpuint(sessions, ==, 2);
	/* A plain directory is its own root. */
	g_assert_cmpstr(ai_project_get_root(find_named(projects, "notes")), ==, "/home/me/notes");
	(void)data;
}

static void
test_sessions(Fixture *fx, gconstpointer data)
{
	g_autolist(AiProject) projects = NULL;
	g_autoptr(GPtrArray) sessions = NULL;
	seed(fx);
	projects = ai_project_list(fx->dir, NULL);
	sessions = ai_project_dup_sessions(find_named(projects, "alpha"));
	g_assert_cmpuint(sessions->len, ==, 2);
	/* Working before disconnected: the dashboard's own order. */
	g_assert_cmpstr(ai_work_session_get_field(g_ptr_array_index(sessions, 0), "title"), ==, "alpha main");
	g_assert_cmpstr(ai_work_session_get_field(g_ptr_array_index(sessions, 1), "status"), ==, "DISCONNECTED");
	g_assert_cmpint(ai_work_session_compare(g_ptr_array_index(sessions, 0), g_ptr_array_index(sessions, 1)), <, 0);
	g_assert_cmpint(ai_work_session_compare(g_ptr_array_index(sessions, 1), g_ptr_array_index(sessions, 0)), >, 0);
	g_assert_cmpint(ai_work_session_compare(g_ptr_array_index(sessions, 0), g_ptr_array_index(sessions, 0)), ==, 0);
	(void)data;
}

static void
test_find(Fixture *fx, gconstpointer data)
{
	g_autolist(AiProject) projects = NULL;
	const gchar *queries[] = { "alpha", "ALPHA", "/r/alpha/.git", "/r/alpha", "/r/alpha/src/deep",
		"/wt/alpha-feature", "/wt/alpha-feature/sub", NULL };
	guint i;
	seed(fx);
	save_session(fx, "/a/dup/.git", "/a/dup", "IDLE", TRUE, "one");
	save_session(fx, "/b/dup/.git", "/b/dup", "IDLE", TRUE, "two");
	projects = ai_project_list(fx->dir, NULL);
	for (i = 0; queries[i] != NULL; i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AiProject) found = ai_project_find(projects, queries[i], &error);
		g_assert_no_error(error);
		g_assert_cmpstr(ai_project_get_id(found), ==, "/r/alpha/.git");
	}
	{
		g_autoptr(GError) error = NULL;
		g_assert_null(ai_project_find(projects, "gamma", &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "gamma"));
	}
	{
		/* Two projects called "dup": name both rather than pick one. */
		g_autoptr(GError) error = NULL;
		g_assert_null(ai_project_find(projects, "dup", &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "/a/dup"));
		g_assert_nonnull(strstr(error->message, "/b/dup"));
	}
	{
		/* A path settles what a name cannot. */
		g_autoptr(AiProject) found = ai_project_find(projects, "/b/dup", NULL);
		g_assert_cmpstr(ai_project_get_id(found), ==, "/b/dup/.git");
	}
	{
		/* A sibling that only shares a prefix is not inside the root. */
		g_autoptr(GError) error = NULL;
		g_assert_null(ai_project_find(projects, "/r/alphabet", &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	}
	(void)data;
}

static void
test_robust(Fixture *fx, gconstpointer data)
{
	g_autolist(AiProject) projects = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *missing = g_build_filename(fx->dir, "does-not-exist", NULL);
	g_autofree gchar *garbage = g_build_filename(fx->dir, "0b4a1c3e-9f1d-4a4e-9b3a-1d2e3f4a5b6c", NULL);
	/* No registry yet is an empty answer, not an error. */
	g_assert_null(ai_project_list(missing, &error));
	g_assert_no_error(error);
	/* A corrupt record costs itself and nothing else. */
	g_assert_true(g_file_set_contents(garbage, "\x01\x02 not a key file", -1, NULL));
	save_session(fx, "", "/tmp/orphan", "IDLE", TRUE, "no project field");
	seed(fx);
	projects = ai_project_list(fx->dir, &error);
	g_assert_no_error(error);
	/* A record without a project groups under its own directory. */
	g_assert_cmpuint(g_list_length(projects), ==, 4);
	g_assert_nonnull(find_named(projects, "orphan"));
	g_assert_null(ai_project_find(projects, "", NULL));
	g_assert_null(ai_project_find(NULL, "alpha", NULL));
	(void)data;
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/project/label", test_label);
	g_test_add_func("/project/priority", test_priority);
	g_test_add("/project/list", Fixture, NULL, fixture_set_up, test_list, fixture_tear_down);
	g_test_add("/project/sessions", Fixture, NULL, fixture_set_up, test_sessions, fixture_tear_down);
	g_test_add("/project/find", Fixture, NULL, fixture_set_up, test_find, fixture_tear_down);
	g_test_add("/project/robust", Fixture, NULL, fixture_set_up, test_robust, fixture_tear_down);
	return g_test_run();
}
