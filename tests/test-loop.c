/*
 * test-loop.c - /loop scheduling, independent of a terminal
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <ai-glib.h>

#include <time.h>

#include <glib/gstdio.h>

static gchar *sandbox = NULL;

static void
rm_rf(const gchar *path)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", path);
	g_spawn_command_line_sync(cmd, NULL, NULL, NULL, NULL);
}

static void
setup_sandbox(void)
{
	sandbox = g_dir_make_tmp("ai-loop-XXXXXX", NULL);
	g_assert_nonnull(sandbox);
}

static void
teardown_sandbox(void)
{
	gchar *saved;

	saved = g_strdup(g_getenv("AI_LOOP_DISABLE"));
	g_unsetenv("AI_LOOP_DISABLE");
	rm_rf(sandbox);
	g_clear_pointer(&sandbox, g_free);

	if (saved != NULL)
	{
		g_setenv("AI_LOOP_DISABLE", saved, TRUE);
		g_free(saved);
	}
}

static gchar *
command(AiLoopSchedule *schedule, const gchar *arguments, gint64 now_us)
{
	g_autoptr(GError) error = NULL;
	gchar            *notice;

	notice = ai_loop_schedule_command(schedule, arguments, sandbox,
	                                  sandbox, sandbox, now_us, &error);
	g_assert_no_error(error);
	g_assert_nonnull(notice);
	return notice;
}

static void
assert_minute_aligned(gint64 nominal_us, gint step)
{
	time_t    sec = (time_t)(nominal_us / G_USEC_PER_SEC);
	struct tm when;

	g_assert_true(localtime_r(&sec, &when) != NULL);
	g_assert_cmpint(when.tm_sec, ==, 0);
	g_assert_cmpint(when.tm_min % step, ==, 0);
}

static void
test_fixed_interval_and_rounding(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *notice = NULL;
	gint64                    now = 1 * G_USEC_PER_SEC;
	gint64                    nominal;
	gint64                    fire;
	gint64                    interval;

	notice = command(schedule, "5m check the deploy", now);
	g_assert_nonnull(strstr(notice, "every 5m"));
	g_assert_nonnull(strstr(notice, "*/5 * * * *"));
	g_assert_nonnull(strstr(notice, "check the deploy"));
	g_assert_null(strstr(notice, "Rounded"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 1);
	nominal = ai_loop_schedule_get_nominal_us(schedule, 0);
	fire = ai_loop_schedule_get_fire_us(schedule, 0);
	interval = ai_loop_schedule_get_interval_us(schedule, 0);
	g_assert_cmpint(nominal, >, now);
	g_assert_cmpint(nominal - now, <=, 5 * 60 * G_USEC_PER_SEC);
	assert_minute_aligned(nominal, 5);
	g_assert_cmpint(fire, >=, nominal);
	g_assert_cmpint(fire - nominal, <=, (interval / 2));
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 0), ==, "check the deploy");

	g_clear_pointer(&notice, g_free);
	ai_loop_schedule_clear(schedule);
	notice = command(schedule, "7m ping", now);
	g_assert_nonnull(strstr(notice, "Rounded 7m to every 6m"));
	g_assert_cmpstr(ai_loop_schedule_get_cron(schedule, 0), ==, "*/6 * * * *");

	g_clear_pointer(&notice, g_free);
	ai_loop_schedule_clear(schedule);
	notice = command(schedule, "8m ping", now);
	g_assert_nonnull(strstr(notice, "Rounded 8m to every 10m"));

	g_clear_pointer(&notice, g_free);
	ai_loop_schedule_clear(schedule);
	notice = command(schedule, "90m ping", now);
	g_assert_nonnull(strstr(notice, "Rounded 90m to every 2h"));
	g_assert_cmpstr(ai_loop_schedule_get_cron(schedule, 0), ==, "0 */2 * * *");

	g_clear_pointer(&notice, g_free);
	ai_loop_schedule_clear(schedule);
	notice = command(schedule, "30s ping", now);
	g_assert_nonnull(strstr(notice, "Rounded 30s to every 1m"));

	g_clear_pointer(&notice, g_free);
	ai_loop_schedule_clear(schedule);
	notice = command(schedule, "check logs every 2 hours", now);
	g_assert_nonnull(strstr(notice, "every 2h"));
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 0), ==, "check logs");

	/* A leading interval wins, and the trailing clause stays in the prompt. */
	g_clear_pointer(&notice, g_free);
	ai_loop_schedule_clear(schedule);
	notice = command(schedule, "5m check every 2 hours", now);
	g_assert_nonnull(strstr(notice, "every 5m"));
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 0), ==,
	                "check every 2 hours");
}

static void
test_daily_uses_local_clock(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *notice = NULL;
	g_autofree gchar         *cron = NULL;
	gint64                    now = g_get_real_time();
	time_t                    sec = (time_t)(now / G_USEC_PER_SEC);
	struct tm                 when;

	g_assert_true(localtime_r(&sec, &when) != NULL);
	cron = g_strdup_printf("%d %d * * *", when.tm_min, when.tm_hour);
	notice = command(schedule, "1d tidy the branch", now);
	g_assert_nonnull(strstr(notice, "daily at"));
	g_assert_cmpstr(ai_loop_schedule_get_cron(schedule, 0), ==, cron);
	g_assert_cmpint(ai_loop_schedule_get_nominal_us(schedule, 0), >, now);
}

static void
test_dynamic_reschedule_stop_and_fallback(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *notice = NULL;
	g_autofree gchar         *prompt = NULL;
	g_autofree gchar         *id = NULL;
	gint64                    now = 50 * G_USEC_PER_SEC;

	notice = command(schedule, "check whether CI passed", now);
	g_assert_nonnull(strstr(notice, "self-paced"));
	g_assert_true(ai_loop_schedule_id_is_dynamic(schedule, ai_loop_schedule_get_id(schedule, 0)));
	g_assert_cmpint(ai_loop_schedule_get_fire_us(schedule, 0), <=, now);
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	prompt = ai_loop_schedule_dup_prompt(schedule, id, sandbox, sandbox, sandbox);
	g_assert_nonnull(strstr(prompt, "check whether CI passed"));
	g_assert_nonnull(strstr(prompt, "LOOP_NEXT:"));
	g_assert_null(strstr(prompt, "Do not start a new initiative."));
	g_assert_cmpstr(ai_loop_schedule_due(schedule, now), ==, id);
	g_assert_true(ai_loop_schedule_note_fired(schedule, id, now, NULL));
	g_assert_null(ai_loop_schedule_due(schedule, now));

	g_clear_pointer(&notice, g_free);
	notice = ai_loop_schedule_complete(schedule, id,
	                                   "Still running\nLOOP_NEXT: 5m waiting on CI\n",
	                                   now);
	g_assert_nonnull(strstr(notice, "waits 5m"));
	g_assert_nonnull(strstr(notice, "waiting on CI"));
	g_assert_cmpint(ai_loop_schedule_get_fire_us(schedule, 0), ==,
	                now + 5 * 60 * G_USEC_PER_SEC);

	g_clear_pointer(&notice, g_free);
	notice = ai_loop_schedule_complete(schedule, id, "LOOP_NEXT: 3h too long",
	                                   now);
	g_assert_nonnull(strstr(notice, "waits 60m"));

	g_clear_pointer(&notice, g_free);
	notice = ai_loop_schedule_complete(schedule, id, "CI is green\nLOOP_STOP: done",
	                                   now);
	g_assert_nonnull(strstr(notice, "stopped"));
	g_assert_nonnull(strstr(notice, "done"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);

	g_clear_pointer(&notice, g_free);
	g_clear_pointer(&id, g_free);
	notice = command(schedule, "watch the build", now);
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	g_clear_pointer(&notice, g_free);
	notice = ai_loop_schedule_complete(schedule, id, "no marker here", now);
	g_assert_nonnull(strstr(notice, "20m"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 1);
	g_clear_pointer(&notice, g_free);
	notice = ai_loop_schedule_complete(schedule, id, "still nothing", now + G_USEC_PER_SEC);
	g_assert_nonnull(strstr(notice, "stopped"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);

	g_clear_pointer(&prompt, g_free);
	prompt = ai_loop_default_prompt(NULL, NULL, NULL, NULL, NULL);
	g_assert_nonnull(strstr(prompt, "Do not start a new initiative."));
}

static void
test_stop_waiting_leaves_fixed_loops(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *notice = NULL;
	gint64                    now = 10 * G_USEC_PER_SEC;

	g_free(command(schedule, "15m fixed task", now));
	g_free(command(schedule, "watch it", now));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 2);
	notice = ai_loop_schedule_stop_waiting(schedule);
	g_assert_nonnull(strstr(notice, "Stopped 1 self-paced loop"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 1);
	g_assert_false(ai_loop_schedule_id_is_dynamic(schedule, ai_loop_schedule_get_id(schedule, 0)));
	g_clear_pointer(&notice, g_free);
	g_assert_null(ai_loop_schedule_stop_waiting(schedule));
}

static void
test_list_cancel_and_cap(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *notice = NULL;
	g_autofree gchar         *id = NULL;
	g_autoptr(GError)         error = NULL;
	gint64                    now = 3 * G_USEC_PER_SEC;
	guint                     i;

	notice = command(schedule, "list", now);
	g_assert_cmpstr(notice, ==, "No scheduled loops.");
	g_clear_pointer(&notice, g_free);
	notice = command(schedule, "5m alpha", now);
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	g_clear_pointer(&notice, g_free);
	notice = command(schedule, "list", now);
	g_assert_nonnull(strstr(notice, id));
	g_assert_nonnull(strstr(notice, "alpha"));
	g_clear_pointer(&notice, g_free);
	{
		g_autofree gchar *line = g_strdup_printf("cancel %s", id);
		notice = command(schedule, line, now);
	}
	g_assert_nonnull(strstr(notice, "Cancelled loop"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);

	for (i = 0; i < 50; i++)
	{
		g_autofree gchar *line = g_strdup_printf("60m job %u", i);
		g_free(command(schedule, line, now));
	}

	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 50);
	notice = ai_loop_schedule_command(schedule, "60m one more", sandbox,
	                                  sandbox, sandbox, now, &error);
	g_assert_null(notice);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_pointer(&notice, g_free);
	g_clear_error(&error);
	g_free(command(schedule, "cancel all", now));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);
}

static void
test_missed_fire_is_once_and_expiry_is_final(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *id = NULL;
	g_autofree gchar         *notice = NULL;
	gint64                    now = 100 * G_USEC_PER_SEC;
	gint64                    fire;
	gint64                    expires;

	g_free(command(schedule, "5m poll", now));
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	fire = ai_loop_schedule_get_fire_us(schedule, 0);
	g_assert_null(ai_loop_schedule_due(schedule, fire - 1));
	g_assert_cmpstr(ai_loop_schedule_due(schedule, fire + (gint64)2 * 60 * 60 * G_USEC_PER_SEC), ==, id);
	g_assert_true(ai_loop_schedule_note_fired(schedule, id,
	                                         fire + (gint64)2 * 60 * 60 * G_USEC_PER_SEC,
	                                         NULL));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 1);
	g_assert_cmpint(ai_loop_schedule_get_fire_us(schedule, 0), >,
	                fire + (gint64)2 * 60 * 60 * G_USEC_PER_SEC);

	expires = ai_loop_schedule_get_expires_us(schedule, 0);
	g_assert_cmpint(expires - ai_loop_schedule_get_nominal_us(schedule, 0), >, 0);
	g_assert_true(ai_loop_schedule_note_fired(schedule, id, expires, &notice));
	g_assert_nonnull(strstr(notice, "last time"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);
}

static void
test_disabled(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	gchar                    *notice;

	g_setenv("AI_LOOP_DISABLE", "1", TRUE);
	g_assert_true(ai_loop_schedule_is_disabled());
	notice = ai_loop_schedule_command(schedule, "5m x", sandbox, sandbox,
	                                  sandbox, 1, &error);
	g_assert_null(notice);
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
	g_assert_null(ai_loop_schedule_due(schedule, G_MAXINT64));
	g_unsetenv("AI_LOOP_DISABLE");
	g_assert_false(ai_loop_schedule_is_disabled());
}

static gchar *
read_prompt(const gchar *cfg, const gchar *house, gchar **origin, gboolean *truncated)
{
	g_free(*origin);
	*origin = NULL;
	return ai_loop_default_prompt(sandbox, cfg, house, origin, truncated);
}

static void
test_default_prompt_files(void)
{
	g_autofree gchar *project = g_build_filename(sandbox, ".ai-glib", NULL);
	g_autofree gchar *claude = g_build_filename(sandbox, ".claude", NULL);
	g_autofree gchar *config = g_build_filename(sandbox, "cfg", "ai-glib", NULL);
	g_autofree gchar *home = g_build_filename(sandbox, "home", ".claude", NULL);
	g_autofree gchar *cfg = g_build_filename(sandbox, "cfg", NULL);
	g_autofree gchar *house = g_build_filename(sandbox, "home", NULL);
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *origin = NULL;
	g_autofree gchar *padding = NULL;
	gboolean          truncated = FALSE;

	g_mkdir_with_parents(project, 0700);
	g_mkdir_with_parents(claude, 0700);
	g_mkdir_with_parents(config, 0700);
	g_mkdir_with_parents(home, 0700);
	path = g_build_filename(home, "loop.md", NULL);
	g_assert_true(g_file_set_contents(path, "from home\n", -1, NULL));
	g_clear_pointer(&path, g_free);
	path = g_build_filename(config, "loop.md", NULL);
	g_assert_true(g_file_set_contents(path, "from config\n", -1, NULL));
	g_clear_pointer(&path, g_free);
	path = g_build_filename(claude, "loop.md", NULL);
	g_assert_true(g_file_set_contents(path, "from claude\n", -1, NULL));
	g_clear_pointer(&path, g_free);
	path = g_build_filename(project, "loop.md", NULL);
	g_assert_true(g_file_set_contents(path, "from project\n", -1, NULL));

	text = read_prompt(cfg, house, &origin, &truncated);
	g_assert_cmpstr(text, ==, "from project\n");
	g_assert_cmpstr(origin, ==, ".ai-glib/loop.md");
	g_assert_false(truncated);

	g_assert_cmpint(g_unlink(path), ==, 0);
	g_clear_pointer(&text, g_free);
	text = read_prompt(cfg, house, &origin, &truncated);
	g_assert_cmpstr(text, ==, "from claude\n");
	g_assert_cmpstr(origin, ==, ".claude/loop.md");

	{
		g_autofree gchar *claude_path = g_build_filename(claude, "loop.md", NULL);
		g_assert_cmpint(g_unlink(claude_path), ==, 0);
	}
	g_clear_pointer(&text, g_free);
	text = read_prompt(cfg, house, &origin, &truncated);
	g_assert_cmpstr(text, ==, "from config\n");

	padding = g_strnfill(24999, 'a');
	{
		g_autofree gchar *huge = g_strconcat(padding, "éMORE", NULL);
		g_autofree gchar *cfg_file = g_build_filename(config, "loop.md", NULL);

		g_assert_true(g_file_set_contents(cfg_file, huge, -1, NULL));
	}
	g_clear_pointer(&text, g_free);
	g_clear_pointer(&origin, g_free);
	text = ai_loop_default_prompt(NULL, cfg, house, &origin, &truncated);
	g_assert_true(truncated);
	g_assert_true(g_utf8_validate(text, -1, NULL));
	g_assert_cmpuint(strlen(text), ==, 24999);
}

static void
test_save_load_drops_dynamic_and_expired(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(AiLoopSchedule) loaded = ai_loop_schedule_new();
	g_autofree gchar         *path = g_build_filename(sandbox, "loops.json", NULL);
	g_autofree gchar         *id = NULL;
	g_autoptr(GError)         error = NULL;
	gint64                    now = g_get_real_time();
	gint                      count;

	g_free(command(schedule, "5m keep me", now));
	g_free(command(schedule, "self paced", now));
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	g_assert_true(ai_loop_schedule_save(schedule, path, &error));
	g_assert_no_error(error);
	count = ai_loop_schedule_load(loaded, path, now, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count, ==, 1);
	g_assert_cmpstr(ai_loop_schedule_get_id(loaded, 0), ==, id);
	g_assert_cmpstr(ai_loop_schedule_get_prompt(loaded, 0), ==, "keep me");
	g_assert_false(ai_loop_schedule_id_is_dynamic(loaded, id));
	g_assert_cmpint(ai_loop_schedule_get_fire_us(loaded, 0), ==,
	                ai_loop_schedule_get_fire_us(schedule, 0));

	count = ai_loop_schedule_load(loaded, path,
	                              now + (gint64)8 * 24 * 60 * 60 * G_USEC_PER_SEC, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count, ==, 0);

	{
		g_autofree gchar *target = g_build_filename(sandbox, "real.json", NULL);
		g_autofree gchar *link = g_build_filename(sandbox, "link.json", NULL);

		g_file_set_contents(target, "{}\n", -1, NULL);
		g_assert_cmpint(symlink(target, link), ==, 0);
		g_assert_false(ai_loop_schedule_save(schedule, link, &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_cmpint(ai_loop_schedule_load(loaded, link, now, &error), ==, -1);
		g_assert_nonnull(error);
	}
}

static void
test_subcommands_that_are_prompts(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *notice = NULL;

	notice = command(schedule, "list the deploys", 1);
	g_assert_nonnull(strstr(notice, "self-paced"));
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 0), ==, "list the deploys");
	ai_loop_schedule_clear(schedule);
	g_clear_pointer(&notice, g_free);
	notice = command(schedule, "cancel the job", 1);
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 0), ==, "cancel the job");
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	setup_sandbox();
	g_test_add_func("/ai-glib/loop/fixed", test_fixed_interval_and_rounding);
	g_test_add_func("/ai-glib/loop/daily", test_daily_uses_local_clock);
	g_test_add_func("/ai-glib/loop/dynamic", test_dynamic_reschedule_stop_and_fallback);
	g_test_add_func("/ai-glib/loop/stop-waiting", test_stop_waiting_leaves_fixed_loops);
	g_test_add_func("/ai-glib/loop/list-cancel-cap", test_list_cancel_and_cap);
	g_test_add_func("/ai-glib/loop/missed-and-expiry", test_missed_fire_is_once_and_expiry_is_final);
	g_test_add_func("/ai-glib/loop/disabled", test_disabled);
	g_test_add_func("/ai-glib/loop/prompt-files", test_default_prompt_files);
	g_test_add_func("/ai-glib/loop/save-load", test_save_load_drops_dynamic_and_expired);
	g_test_add_func("/ai-glib/loop/prompt-words", test_subcommands_that_are_prompts);
	{
		gint status = g_test_run();
		teardown_sandbox();
		return status;
	}
}
