/*
 * test-ai-loop-cli.c - `ai loop` and `ai goal` against a sandboxed store
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Spawns the built `ai` binary with HOME and every XDG directory pointed
 * at a private sandbox, seeds schedules through the library, and checks
 * both what the command prints and what it leaves on disk.
 */

#include <ai-glib.h>

#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

static gchar *ai_binary = NULL;
static gchar *sandbox = NULL;
static gchar *store = NULL;

static gchar *
ai(const gchar *const *args, gint expect_status, gchar **err)
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
	{
		g_ptr_array_add(argv, (gpointer)args[i]);
	}

	g_ptr_array_add(argv, NULL);
	envp = g_environ_setenv(envp, "HOME", sandbox, TRUE);
	envp = g_environ_setenv(envp, "XDG_STATE_HOME", sandbox, TRUE);
	envp = g_environ_setenv(envp, "XDG_CONFIG_HOME", sandbox, TRUE);
	envp = g_environ_setenv(envp, "XDG_DATA_HOME", sandbox, TRUE);
	envp = g_environ_unsetenv(envp, "AI_LOOP_DISABLE");

	g_assert_true(g_spawn_sync(sandbox, (gchar **)argv->pdata, envp, 0, NULL, NULL,
	                           &out, &errout, &status, &error));
	g_assert_no_error(error);

	if (!g_spawn_check_wait_status(status, NULL) || expect_status != 0)
	{
		g_assert_true(WIFEXITED(status));

		if (WEXITSTATUS(status) != expect_status)
		{
			g_error("ai %s exited %d, expected %d\nstdout: %s\nstderr: %s",
			        args[0], WEXITSTATUS(status), expect_status, out, errout);
		}
	}

	if (err != NULL)
	{
		*err = g_steal_pointer(&errout);
	}

	return g_steal_pointer(&out);
}

static gchar *
path_for(const gchar *owner)
{
	return ai_loop_store_path(store, owner);
}

static AiLoopSchedule *
load(const gchar *owner)
{
	g_autofree gchar *path = path_for(owner);
	g_autoptr(GError) error = NULL;
	AiLoopSchedule   *schedule = ai_loop_schedule_new();

	g_assert_cmpint(ai_loop_schedule_load(schedule, path, g_get_real_time(), &error), >=, 0);
	g_assert_no_error(error);
	return schedule;
}

static void
seed(const gchar *owner, gchar **loop_id, gchar **goal_id)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *path = path_for(owner);
	g_autoptr(GError)         error = NULL;
	gint64                    now = g_get_real_time();

	*loop_id = ai_loop_schedule_add_loop(schedule, (gint64)5 * 60 * G_USEC_PER_SEC,
	                                     "check the deploy", now, NULL, &error);
	g_assert_no_error(error);
	*goal_id = ai_loop_schedule_add_goal(schedule, "the tests pass", 20, 0, now, &error);
	g_assert_no_error(error);
	g_assert_true(ai_loop_schedule_save(schedule, path, &error));
	g_assert_no_error(error);
}

static void
test_empty(void)
{
	const gchar      *args[] = { "loop", "list", NULL };
	g_autofree gchar *out = ai(args, 0, NULL);

	g_assert_nonnull(strstr(out, "No loops."));
}

static void
test_list_human_and_json(void)
{
	g_autofree gchar     *loop_id = NULL;
	g_autofree gchar     *goal_id = NULL;
	g_autofree gchar     *out = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject           *root;
	JsonArray            *sessions;
	JsonObject           *session;
	JsonArray            *entries;
	const gchar          *human[] = { "loop", "list", NULL };
	const gchar          *json[] = { "goal", "list", "--json", NULL };
	gint                  lock;

	seed("sess-json", &loop_id, &goal_id);
	out = ai(human, 0, NULL);
	g_assert_nonnull(strstr(out, "sess-json"));
	g_assert_nonnull(strstr(out, loop_id));
	g_assert_nonnull(strstr(out, "loop  active  every 5m, next in"));
	/* `ai loop list` lists loops; goals are `ai goal list`. */
	g_assert_null(strstr(out, goal_id));

	/* A process holding the claim is reported as running it. */
	lock = ai_loop_store_claim(store, "sess-json", NULL);
	g_assert_cmpint(lock, >=, 0);
	g_clear_pointer(&out, g_free);
	out = ai(json, 0, NULL);
	close(lock);

	g_assert_true(json_parser_load_from_data(parser, out, -1, NULL));
	root = json_node_get_object(json_parser_get_root(parser));
	sessions = json_object_get_array_member(root, "sessions");
	g_assert_cmpuint(json_array_get_length(sessions), >=, 1);
	session = NULL;

	{
		guint i;

		for (i = 0; i < json_array_get_length(sessions); i++)
		{
			JsonObject *candidate = json_array_get_object_element(sessions, i);

			if (g_strcmp0(json_object_get_string_member(candidate, "session"), "sess-json") == 0)
			{
				session = candidate;
			}
		}
	}

	g_assert_nonnull(session);
	g_assert_true(json_object_get_boolean_member(session, "running"));
	entries = json_object_get_array_member(session, "entries");
	g_assert_cmpuint(json_array_get_length(entries), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(entries, 0), "id"), ==, goal_id);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(entries, 0), "state"), ==, "active");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(entries, 0), "kind"), ==, "goal");
}

static void
test_edit_verbs(void)
{
	g_autofree gchar         *loop_id = NULL;
	g_autofree gchar         *goal_id = NULL;
	g_autofree gchar         *prefix = NULL;
	g_autofree gchar         *out = NULL;
	g_autofree gchar         *err = NULL;
	g_autoptr(AiLoopSchedule) schedule = NULL;
	gint                      index;

	seed("sess-edit", &loop_id, &goal_id);
	prefix = g_strndup(loop_id, 5);

	{
		const gchar *args[] = { "loop", "pause", prefix, NULL };
		out = ai(args, 0, NULL);
		g_assert_nonnull(strstr(out, "Paused loop"));
		g_assert_nonnull(strstr(out, "not running"));
	}

	schedule = load("sess-edit");
	index = ai_loop_schedule_find(schedule, loop_id);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, (guint)index), ==, AI_LOOP_STATE_PAUSED);
	g_clear_object(&schedule);

	{
		const gchar *args[] = { "loop", "edit", loop_id, "--every", "2h", "--prompt", "look", "again", NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 0, NULL);
	}

	{
		const gchar *args[] = { "goal", "edit", goal_id, "--turns", "5", NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 0, NULL);
	}

	{
		const gchar *args[] = { "goal", "run", goal_id, NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 0, NULL);
	}

	schedule = load("sess-edit");
	index = ai_loop_schedule_find(schedule, loop_id);
	g_assert_cmpstr(ai_loop_schedule_get_cron(schedule, (guint)index), ==, "0 */2 * * *");
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, (guint)index), ==, "look again");
	index = ai_loop_schedule_find(schedule, goal_id);
	g_assert_cmpuint(ai_loop_schedule_get_max_turns(schedule, (guint)index), ==, 5);
	g_assert_cmpstr(ai_loop_schedule_due(schedule, g_get_real_time()), ==, goal_id);
	g_clear_object(&schedule);

	{
		const gchar *args[] = { "goal", "show", goal_id, NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 0, NULL);
		g_assert_nonnull(strstr(out, "Condition:  the tests pass"));
		g_assert_nonnull(strstr(out, "5 turns"));
	}

	/* A typo lists the ids that exist, on stderr, and fails. */
	{
		const gchar *args[] = { "loop", "resume", "deadbeef", NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 1, &err);
		g_assert_nonnull(strstr(err, "No loop 'deadbeef'. Valid ids:"));
		g_assert_nonnull(strstr(err, loop_id));
	}

	/* `all` needs a session; with one, it applies there only. */
	{
		const gchar *args[] = { "loop", "delete", "all", NULL };
		g_clear_pointer(&out, g_free);
		g_clear_pointer(&err, g_free);
		out = ai(args, 2, &err);
		g_assert_nonnull(strstr(err, "--session"));
	}

	{
		const gchar *args[] = { "loop", "delete", "all", "--session", "sess-edit", NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 0, NULL);
		g_assert_nonnull(strstr(out, "Deleted 1 scheduled loop"));
	}

	schedule = load("sess-edit");
	g_assert_cmpint(ai_loop_schedule_find(schedule, loop_id), ==, -1);
	g_assert_cmpint(ai_loop_schedule_find(schedule, goal_id), >=, 0);
}

static void
test_add(void)
{
	g_autofree gchar         *out = NULL;
	g_autofree gchar         *err = NULL;
	g_autoptr(AiLoopSchedule) schedule = NULL;

	{
		const gchar *args[] = { "goal", "add", "the docs build", NULL };
		out = ai(args, 2, &err);
		g_assert_nonnull(strstr(err, "--session"));
	}

	{
		const gchar *args[] = { "goal", "add", "--session", "sess-add", "the docs build", "--turns", "3", NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 0, NULL);
		g_assert_nonnull(strstr(out, "at most 3 turns"));
	}

	{
		const gchar *args[] = { "loop", "add", "--session", "sess-add", "15m", "check", "CI", NULL };
		g_clear_pointer(&out, g_free);
		out = ai(args, 0, NULL);
		g_assert_nonnull(strstr(out, "every 15m"));
	}

	{
		const gchar *args[] = { "loop", "add", "--session", "../x", "15m", "y", NULL };
		g_clear_pointer(&out, g_free);
		g_clear_pointer(&err, g_free);
		out = ai(args, 1, &err);
	}

	/* The shell refuses what the slash command refuses. */
	{
		const gchar *args[] = { "loop", "add", "--session", "sess-add", "10m", "/clear", NULL };
		g_clear_pointer(&out, g_free);
		g_clear_pointer(&err, g_free);
		out = ai(args, 1, &err);
		g_assert_nonnull(strstr(err, "/clear cannot run on a schedule"));
	}

	schedule = load("sess-add");
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 2);
	g_assert_cmpstr(ai_loop_schedule_get_condition(schedule, 0), ==, "the docs build");
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 1), ==, "check CI");
}

static void
test_corrupt_store_is_reported_not_fatal(void)
{
	g_autofree gchar *path = path_for("sess-broken");
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	const gchar      *args[] = { "loop", "list", NULL };

	g_mkdir_with_parents(store, 0700);
	g_assert_true(g_file_set_contents(path, "{garbage", -1, NULL));
	out = ai(args, 0, &err);
	g_assert_nonnull(strstr(err, "sess-broken"));
	/* The good sessions still list. */
	g_assert_nonnull(strstr(out, "sess-json"));
	g_unlink(path);
}

static void
test_help(void)
{
	const gchar      *args[] = { "goal", "--help", NULL };
	g_autofree gchar *out = ai(args, 0, NULL);

	g_assert_nonnull(strstr(out, "ai goal add --session ID CONDITION"));
	g_assert_nonnull(strstr(out, "--json"));
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
	sandbox = g_dir_make_tmp("ai-loop-cli-XXXXXX", NULL);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_STATE_HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	store = g_build_filename(sandbox, "ai-glib", "sessions", "loops", NULL);
	g_assert_cmpint(g_chdir(sandbox), ==, 0);

	g_test_add_func("/ai-glib/loop-cli/empty", test_empty);
	g_test_add_func("/ai-glib/loop-cli/list", test_list_human_and_json);
	g_test_add_func("/ai-glib/loop-cli/edit", test_edit_verbs);
	g_test_add_func("/ai-glib/loop-cli/add", test_add);
	g_test_add_func("/ai-glib/loop-cli/corrupt", test_corrupt_store_is_reported_not_fatal);
	g_test_add_func("/ai-glib/loop-cli/help", test_help);
	status = g_test_run();

	{
		g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", sandbox);
		g_chdir("/");
		g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
	}

	g_free(store);
	g_free(sandbox);
	g_free(ai_binary);
	return status;
}
