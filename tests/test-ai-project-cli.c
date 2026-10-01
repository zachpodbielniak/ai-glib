/*
 * test-ai-project-cli.c - `ai project` against a sandboxed registry
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spawns the built `ai` binary with HOME and every XDG directory pointed
 * at a private sandbox, seeds the work registry through the library, and
 * checks what the command prints. A suite that read the developer's real
 * registry would pass or fail by whose machine ran it.
 */

#include <ai-glib.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib/gstdio.h>

static gchar *ai_binary = NULL;
static gchar *sandbox = NULL;
static gchar *registry = NULL;

static gchar *
ai(const gchar *cwd, const gchar *const *args, gint expect_status, gchar **err)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_auto(GStrv)        envp = g_get_environ();
	g_autofree gchar    *out = NULL;
	g_autofree gchar    *errout = NULL;
	g_autoptr(GError)    error = NULL;
	gint                 status = 0;
	guint                i;

	g_ptr_array_add(argv, ai_binary);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	envp = g_environ_setenv(envp, "HOME", sandbox, TRUE);
	envp = g_environ_setenv(envp, "XDG_STATE_HOME", sandbox, TRUE);
	envp = g_environ_setenv(envp, "XDG_CONFIG_HOME", sandbox, TRUE);
	envp = g_environ_setenv(envp, "XDG_DATA_HOME", sandbox, TRUE);

	g_assert_true(g_spawn_sync(cwd != NULL ? cwd : sandbox, (gchar **)argv->pdata, envp, 0,
	                           NULL, NULL, &out, &errout, &status, &error));
	g_assert_no_error(error);
	g_assert_true(WIFEXITED(status));
	if (WEXITSTATUS(status) != expect_status)
		g_error("ai %s exited %d, expected %d\nstdout: %s\nstderr: %s",
		        args[0], WEXITSTATUS(status), expect_status, out, errout);
	if (err != NULL)
		*err = g_steal_pointer(&errout);
	return g_steal_pointer(&out);
}

static void
seed(const gchar *project, const gchar *directory, const gchar *status,
     const gchar *title, gboolean live)
{
	g_autoptr(AiWorkSession) session = g_object_new(AI_TYPE_WORK_SESSION, NULL);
	g_object_set(session, "project", project, "directory", directory, "status", status,
		"title", title, "branch", "main", "provider", "grok-build", "model", "grok-4.7", NULL);
	g_assert_true(ai_work_session_save(session, registry, live, NULL));
}

static void
clear_registry(void)
{
	g_autoptr(GDir) dir = g_dir_open(registry, 0, NULL);
	const gchar *name;
	while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = g_build_filename(registry, name, NULL);
		g_unlink(path);
	}
}

/* Two projects; the sandbox itself stands in for alpha's worktree, so a
 * bare `ai project show` run there resolves by the current directory. */
static void
seed_standard(void)
{
	clear_registry();
	seed("/r/alpha/.git", "/r/alpha", "WORK", "alpha main", TRUE);
	seed("/r/alpha/.git", sandbox, "IDLE", "alpha worktree", FALSE);
	seed("/r/beta/.git", "/r/beta", "INPUT", "beta asks", TRUE);
}

static void
test_empty(void)
{
	const gchar *args[] = { "project", NULL };
	g_autofree gchar *out = NULL;
	clear_registry();
	out = ai(NULL, args, 0, NULL);
	g_assert_nonnull(strstr(out, "No projects yet"));
}

/* Most urgent project first, with what it needs from a person. */
static void
test_list(void)
{
	const gchar *args[] = { "project", "list", NULL };
	g_autofree gchar *out = NULL;
	const gchar *beta, *alpha;
	seed_standard();
	out = ai(NULL, args, 0, NULL);
	beta = strstr(out, "beta");
	alpha = strstr(out, "alpha");
	g_assert_nonnull(beta); g_assert_nonnull(alpha);
	g_assert_true(beta < alpha);
	g_assert_nonnull(strstr(out, "1 needs you"));
	g_assert_nonnull(strstr(out, "1 working"));
	g_assert_nonnull(strstr(out, "/r/alpha"));
}

static void
test_json(void)
{
	const gchar *list[] = { "project", "--json", NULL };
	const gchar *show[] = { "project", "show", "ALPHA", "--json", NULL };
	g_autofree gchar *out = NULL;
	g_autoptr(JsonNode) node = NULL;
	JsonObject *root, *project;
	JsonArray *items;
	seed_standard();
	out = ai(NULL, list, 0, NULL);
	node = json_from_string(out, NULL);
	g_assert_nonnull(node);
	root = json_node_get_object(node);
	g_assert_cmpint(json_object_get_int_member(root, "schema_version"), ==, 1);
	items = json_object_get_array_member(root, "projects");
	g_assert_cmpuint(json_array_get_length(items), ==, 2);
	project = json_array_get_object_element(items, 1);
	g_assert_cmpstr(json_object_get_string_member(project, "name"), ==, "alpha");
	g_assert_cmpstr(json_object_get_string_member(project, "id"), ==, "/r/alpha/.git");
	g_assert_cmpstr(json_object_get_string_member(project, "root"), ==, "/r/alpha");
	g_assert_cmpstr(json_object_get_string_member(project, "status"), ==, "WORK");
	g_assert_cmpint(json_object_get_int_member(project, "session_count"), ==, 2);
	g_assert_cmpint(json_object_get_int_member(project, "live_count"), ==, 1);
	g_assert_false(json_object_has_member(project, "sessions"));
	g_clear_pointer(&out, g_free); g_clear_pointer(&node, json_node_unref);
	/* show takes a name case-insensitively and lists the sessions. */
	out = ai(NULL, show, 0, NULL);
	node = json_from_string(out, NULL);
	project = json_object_get_object_member(json_node_get_object(node), "project");
	items = json_object_get_array_member(project, "sessions");
	g_assert_cmpuint(json_array_get_length(items), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(items, 0), "title"), ==, "alpha main");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(items, 1), "status"), ==, "DISCONNECTED");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(items, 1), "model"), ==, "grok-4.7");
}

/* A path anywhere inside a project, or no argument at all. */
static void
test_show_by_path(void)
{
	const gchar *by_path[] = { "project", "show", "/r/beta/src/lib", NULL };
	const gchar *here[] = { "project", "show", NULL };
	g_autofree gchar *out = NULL;
	g_autofree gchar *sub = g_build_filename(sandbox, "src", NULL);
	seed_standard();
	out = ai(NULL, by_path, 0, NULL);
	g_assert_nonnull(strstr(out, "beta asks"));
	g_assert_null(strstr(out, "alpha main"));
	g_clear_pointer(&out, g_free);
	g_mkdir_with_parents(sub, 0700);
	out = ai(sub, here, 0, NULL);
	g_assert_nonnull(strstr(out, "alpha worktree"));
	g_assert_nonnull(strstr(out, "alpha main"));
	g_rmdir(sub);
}

/* Unknown names fail loudly; misuse is a usage error, not a crash. */
static void
test_errors(void)
{
	const gchar *unknown[] = { "project", "show", "gamma", NULL };
	const gchar *verb[] = { "project", "frobnicate", NULL };
	const gchar *extra[] = { "project", "show", "a", "b", NULL };
	const gchar *help[] = { "project", "--help", NULL };
	g_autofree gchar *err = NULL;
	g_autofree gchar *out = NULL;
	seed_standard();
	seed("/a/dup/.git", "/a/dup", "IDLE", "one", TRUE);
	seed("/b/dup/.git", "/b/dup", "IDLE", "two", TRUE);
	g_free(ai(NULL, unknown, 1, &err));
	g_assert_nonnull(strstr(err, "gamma"));
	g_clear_pointer(&err, g_free);
	{
		/* Two projects share a name: both are named, neither is picked. */
		const gchar *dup[] = { "project", "show", "dup", NULL };
		g_free(ai(NULL, dup, 1, &err));
		g_assert_nonnull(strstr(err, "/a/dup"));
		g_assert_nonnull(strstr(err, "/b/dup"));
		g_clear_pointer(&err, g_free);
	}
	g_free(ai(NULL, verb, 2, &err));
	g_assert_nonnull(strstr(err, "Usage"));
	g_clear_pointer(&err, g_free);
	g_free(ai(NULL, extra, 2, &err));
	g_clear_pointer(&err, g_free);
	out = ai(NULL, help, 0, NULL);
	g_assert_nonnull(strstr(out, "ai project show"));
}

int
main(int argc, char **argv)
{
	g_autofree gchar *base = NULL;
	gint              status;

	g_test_init(&argc, &argv, NULL);
	base = g_path_get_dirname(argv[0]);
	{
		g_autofree gchar *relative = g_build_filename(base, "..", "bin", "ai", NULL);
		ai_binary = g_canonicalize_filename(relative, NULL);
	}
	sandbox = g_dir_make_tmp("ai-project-cli-XXXXXX", NULL);
	registry = g_build_filename(sandbox, "ai-glib", "sessions", NULL);
	g_mkdir_with_parents(registry, 0700);

	g_test_add_func("/ai-glib/project-cli/empty", test_empty);
	g_test_add_func("/ai-glib/project-cli/list", test_list);
	g_test_add_func("/ai-glib/project-cli/json", test_json);
	g_test_add_func("/ai-glib/project-cli/show-by-path", test_show_by_path);
	g_test_add_func("/ai-glib/project-cli/errors", test_errors);
	status = g_test_run();

	clear_registry();
	g_rmdir(registry);
	{
		g_autofree gchar *state = g_build_filename(sandbox, "ai-glib", NULL);
		g_rmdir(state);
	}
	g_rmdir(sandbox);
	g_free(registry);
	g_free(sandbox);
	g_free(ai_binary);
	return status;
}
