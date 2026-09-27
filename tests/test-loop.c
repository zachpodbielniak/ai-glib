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
	g_clear_pointer(&notice, g_free);
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

/*
 * Self-paced loops used to be dropped on save, following Claude Code. A
 * loop now outlives the process that scheduled it -- the model-paced kind
 * included -- so both come back, and both still expire after 7 days.
 */
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
	g_assert_cmpint(count, ==, 2);
	g_assert_cmpstr(ai_loop_schedule_get_id(loaded, 0), ==, id);
	g_assert_cmpstr(ai_loop_schedule_get_prompt(loaded, 0), ==, "keep me");
	g_assert_false(ai_loop_schedule_id_is_dynamic(loaded, id));
	g_assert_cmpint(ai_loop_schedule_get_fire_us(loaded, 0), ==,
	                ai_loop_schedule_get_fire_us(schedule, 0));
	g_assert_cmpstr(ai_loop_schedule_get_id(loaded, 1), ==, ai_loop_schedule_get_id(schedule, 1));
	g_assert_cmpstr(ai_loop_schedule_get_prompt(loaded, 1), ==, "self paced");
	g_assert_true(ai_loop_schedule_id_is_dynamic(loaded, ai_loop_schedule_get_id(loaded, 1)));

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

	/* A verb alone is a mistake, not a one-word prompt. */
	{
		static const gchar *const verbs[] = { "pause", "resume", "show", "run", "edit", "delete", NULL };
		guint i;

		for (i = 0; verbs[i] != NULL; i++)
		{
			g_autoptr(GError) error = NULL;
			gchar *text = ai_loop_schedule_command(schedule, verbs[i], NULL, NULL, NULL, 1, &error);

			g_assert_null(text);
			g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
			g_assert_nonnull(strstr(error->message, "Usage: /loop"));
		}

		g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 1);
	}
}


static gchar *
goal_command(AiLoopSchedule *schedule, const gchar *arguments, gint64 now_us)
{
	g_autoptr(GError) error = NULL;
	gchar            *notice;

	notice = ai_loop_schedule_goal_command(schedule, arguments, now_us, &error);
	g_assert_no_error(error);
	g_assert_nonnull(notice);
	return notice;
}

static void
expect_error(gchar *notice, GError *error, const gchar *needle)
{
	g_assert_null(notice);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);

	if (strstr(error->message, needle) == NULL)
	{
		g_error("'%s' not in '%s'", needle, error->message);
	}
}

static void
goal_error(AiLoopSchedule *schedule, const gchar *arguments, gint64 now_us, const gchar *needle)
{
	g_autoptr(GError) error = NULL;
	gchar            *notice = ai_loop_schedule_goal_command(schedule, arguments, now_us, &error);

	expect_error(notice, error, needle);
}

/* Pause, resume, run, delete and edit, by id and by prefix. */
static void
test_edit_pause_resume_delete(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	g_autofree gchar         *id = NULL;
	g_autofree gchar         *line = NULL;
	g_autofree gchar         *prefix = NULL;
	gchar                    *notice;
	gint64                    now = 1000 * G_USEC_PER_SEC;
	gint64                    fire;

	g_free(command(schedule, "5m check the deploy", now));
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	prefix = g_strndup(id, 4);
	fire = ai_loop_schedule_get_fire_us(schedule, 0);

	line = g_strdup_printf("pause %s", prefix);
	notice = command(schedule, line, now);
	g_assert_nonnull(strstr(notice, "Paused loop"));
	g_free(notice);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_PAUSED);
	g_assert_null(ai_loop_schedule_due(schedule, fire + (gint64)3600 * G_USEC_PER_SEC));
	g_assert_cmpint(ai_loop_schedule_get_next_fire_us(schedule), ==, 0);

	/* Resuming after missed slots skips them: the next slot is ahead. */
	g_clear_pointer(&line, g_free);
	line = g_strdup_printf("resume %s", id);
	g_free(command(schedule, line, fire + (gint64)3 * 3600 * G_USEC_PER_SEC));
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_ACTIVE);
	g_assert_cmpint(ai_loop_schedule_get_fire_us(schedule, 0), >, fire + (gint64)3 * 3600 * G_USEC_PER_SEC);

	/* Edit the interval and the prompt; the rest of the line is the prompt. */
	g_clear_pointer(&line, g_free);
	line = g_strdup_printf("edit %s --every 2h --prompt look at --every thing", id);
	notice = command(schedule, line, now);
	g_assert_nonnull(strstr(notice, "Updated loop"));
	g_free(notice);
	g_assert_cmpstr(ai_loop_schedule_get_cron(schedule, 0), ==, "0 */2 * * *");
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 0), ==, "look at --every thing");

	g_clear_pointer(&line, g_free);
	line = g_strdup_printf("edit %s --self-paced", id);
	g_free(command(schedule, line, now));
	g_assert_true(ai_loop_schedule_id_is_dynamic(schedule, id));
	g_assert_null(ai_loop_schedule_get_cron(schedule, 0));

	/* A bad option leaves the loop alone. */
	g_clear_pointer(&line, g_free);
	line = g_strdup_printf("edit %s --every soon", id);
	notice = ai_loop_schedule_command(schedule, line, NULL, NULL, NULL, now, &error);
	expect_error(notice, error, "--every needs an interval");
	g_clear_error(&error);
	g_assert_true(ai_loop_schedule_id_is_dynamic(schedule, id));

	/* Run-now on a paused loop: due once, still paused after. */
	g_assert_true(ai_loop_schedule_pause(schedule, id, NULL));
	g_assert_true(ai_loop_schedule_run_now(schedule, id, now, NULL));
	g_assert_cmpstr(ai_loop_schedule_due(schedule, now), ==, id);
	g_assert_true(ai_loop_schedule_note_fired(schedule, id, now, NULL));
	g_free(ai_loop_schedule_complete(schedule, id, "LOOP_NEXT: 5m", now));
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_PAUSED);
	g_assert_null(ai_loop_schedule_due(schedule, now + (gint64)3600 * G_USEC_PER_SEC));

	g_clear_pointer(&line, g_free);
	line = g_strdup_printf("delete %s", id);
	notice = command(schedule, line, now);
	g_assert_nonnull(strstr(notice, "Deleted loop"));
	g_free(notice);
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);
}

/* A typo is an error naming the ids that exist; a short prefix that
 * matches two is an error naming both; a goal id is not a loop id. */
static void
test_ids_are_unambiguous(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	g_autofree gchar         *loop_id = NULL;
	g_autofree gchar         *goal_id = NULL;
	g_autofree gchar         *line = NULL;
	gchar                    *notice;

	g_free(command(schedule, "10m one", 1));
	loop_id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	goal_id = ai_loop_schedule_add_goal(schedule, "two", 0, 0, 1, NULL);

	notice = ai_loop_schedule_command(schedule, "pause deadbeef", NULL, NULL, NULL, 1, &error);
	expect_error(notice, error, "No loop 'deadbeef'. Valid ids:");
	g_assert_nonnull(strstr(error->message, loop_id));
	g_assert_null(strstr(error->message, goal_id));
	g_clear_error(&error);

	line = g_strdup_printf("pause %s", goal_id);
	notice = ai_loop_schedule_command(schedule, line, NULL, NULL, NULL, 1, &error);
	expect_error(notice, error, "is a goal, not a loop");
	g_clear_error(&error);

	/* Two ids with a shared prefix, set by hand. */
	g_assert_null(ai_loop_schedule_resolve_id(schedule, "zz", &error));
	g_assert_nonnull(strstr(error->message, "Valid ids"));
	g_clear_error(&error);

	{
		g_autofree gchar *path = g_build_filename(sandbox, "twins.json", NULL);
		g_autoptr(AiLoopSchedule) twins = ai_loop_schedule_new();
		const gchar *data =
			"{\"version\":2,\"tasks\":["
			"{\"id\":\"abc00001\",\"kind\":\"goal\",\"state\":\"active\",\"condition\":\"a\","
			" \"turns\":0,\"max_turns\":5,\"max_duration_us\":3600000000,\"created_us\":1},"
			"{\"id\":\"abc00002\",\"kind\":\"goal\",\"state\":\"active\",\"condition\":\"b\","
			" \"turns\":0,\"max_turns\":5,\"max_duration_us\":3600000000,\"created_us\":1}]}";

		g_assert_true(g_file_set_contents(path, data, -1, NULL));
		g_assert_cmpint(ai_loop_schedule_load(twins, path, 2, &error), ==, 2);
		g_assert_null(ai_loop_schedule_resolve_id(twins, "abc0", &error));
		g_assert_nonnull(strstr(error->message, "matches more than one: abc00001, abc00002"));
		g_clear_error(&error);
		g_autofree gchar *full = ai_loop_schedule_resolve_id(twins, "abc00002", &error);
		g_assert_cmpstr(full, ==, "abc00002");
	}
}

/* One bad record costs itself; the rest load. A file that is not JSON,
 * the wrong version, or not UTF-8 is an error, never a crash. */
static void
test_corrupt_file(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	g_autofree gchar         *path = g_build_filename(sandbox, "corrupt.json", NULL);
	gint64                    now = 2 * G_USEC_PER_SEC;
	const gchar              *mixed =
		"{\"version\":2,\"revision\":7,\"tasks\":["
		"42, null, \"string\", {},"
		"{\"id\":\"not-hex!\",\"kind\":\"goal\",\"state\":\"active\"},"
		"{\"id\":\"00000001\",\"kind\":\"goal\",\"state\":\"bogus\",\"condition\":\"x\"},"
		"{\"id\":\"00000002\",\"kind\":\"goal\",\"state\":\"active\",\"condition\":\"x\",\"max_turns\":9999},"
		"{\"id\":\"00000003\",\"kind\":\"goal\",\"state\":\"active\",\"condition\":{},\"max_turns\":3},"
		"{\"id\":\"00000004\",\"kind\":\"alien\",\"state\":\"active\"},"
		"{\"id\":\"00000005\",\"kind\":\"loop\",\"state\":\"met\",\"self_paced\":true},"
		"{\"id\":\"00000006\",\"kind\":\"loop\",\"state\":\"active\",\"self_paced\":true,"
		" \"prompt\":7,\"expires_us\":\"soon\"},"
		"{\"id\":\"0000000a\",\"kind\":\"goal\",\"state\":\"expired\",\"condition\":\"good goal\","
		" \"turns\":3,\"max_turns\":3,\"max_duration_us\":3600000000,\"created_us\":1,"
		" \"reason\":\"not met after 3 turns\"},"
		"{\"id\":\"0000000b\",\"kind\":\"loop\",\"state\":\"paused\",\"self_paced\":true,"
		" \"prompt\":\"good loop\",\"expires_us\":999999999999999,\"fire_us\":5},"
		"{\"id\":\"0000000b\",\"kind\":\"loop\",\"state\":\"active\",\"self_paced\":true,"
		" \"prompt\":\"duplicate\",\"expires_us\":999999999999999}"
		"]}";

	g_assert_true(g_file_set_contents(path, mixed, -1, NULL));
	g_assert_cmpint(ai_loop_schedule_load(schedule, path, now, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpstr(ai_loop_schedule_get_condition(schedule, 0), ==, "good goal");
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_EXPIRED);
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 1), ==, "good loop");
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 1), ==, AI_LOOP_STATE_PAUSED);

	g_assert_true(g_file_set_contents(path, "{not json", -1, NULL));
	g_assert_cmpint(ai_loop_schedule_load(schedule, path, now, &error), ==, -1);
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_assert_true(g_file_set_contents(path, "{\"version\":99,\"tasks\":[]}", -1, NULL));
	g_assert_cmpint(ai_loop_schedule_load(schedule, path, now, &error), ==, -1);
	g_assert_nonnull(strstr(error->message, "version 99"));
	g_clear_error(&error);

	g_assert_true(g_file_set_contents(path, "null", -1, NULL));
	g_assert_cmpint(ai_loop_schedule_load(schedule, path, now, &error), ==, -1);
	g_clear_error(&error);

	g_assert_true(g_file_set_contents(path, "{\"version\":2,\"tasks\":[]}\xff", -1, NULL));
	g_assert_cmpint(ai_loop_schedule_load(schedule, path, now, &error), ==, -1);
	g_assert_nonnull(strstr(error->message, "UTF-8"));
	g_clear_error(&error);

	/* A version 1 file from before goals existed still loads. */
	g_assert_true(g_file_set_contents(path,
		"{\"version\":1,\"tasks\":[{\"id\":\"1234abcd\",\"cron\":\"*/5 * * * *\","
		"\"cadence\":\"every 5m\",\"created_us\":1,\"expires_us\":999999999999999,"
		"\"nominal_us\":300000000,\"fire_us\":300000000,\"interval_us\":300000000,"
		"\"prompt\":\"old\"}]}", -1, NULL));
	g_assert_cmpint(ai_loop_schedule_load(schedule, path, now, &error), ==, 1);
	g_assert_cmpstr(ai_loop_schedule_get_prompt(schedule, 0), ==, "old");
	g_assert_cmpint(ai_loop_schedule_get_kind(schedule, 0), ==, AI_LOOP_KIND_LOOP);
}

/* Goals round-trip with their state, bounds and reason, and the file is
 * written atomically at mode 0600. */
static void
test_goal_round_trip(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(AiLoopSchedule) loaded = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	g_autofree gchar         *path = g_build_filename(sandbox, "state", "goals.json", NULL);
	g_autofree gchar         *id = NULL;
	g_autofree gchar         *notice = NULL;
	GStatBuf                  status;
	gint64                    now = 10 * G_USEC_PER_SEC;

	notice = goal_command(schedule, "the docs build --turns 7 --time 45m", now);
	g_assert_nonnull(strstr(notice, "at most 7 turns or 45m"));
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));
	g_assert_true(ai_loop_schedule_note_fired(schedule, id, now, NULL));
	g_free(ai_loop_schedule_goal_complete(schedule, id, "GOAL_NOT_MET: two warnings", NULL, now));
	g_assert_true(ai_loop_schedule_pause(schedule, id, NULL));
	g_assert_true(ai_loop_schedule_save(schedule, path, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_stat(path, &status), ==, 0);
	g_assert_cmpint(status.st_mode & 0777, ==, 0600);

	g_assert_cmpint(ai_loop_schedule_load(loaded, path, now, &error), ==, 1);
	g_assert_cmpint(ai_loop_schedule_get_kind(loaded, 0), ==, AI_LOOP_KIND_GOAL);
	g_assert_cmpint(ai_loop_schedule_get_state(loaded, 0), ==, AI_LOOP_STATE_PAUSED);
	g_assert_cmpstr(ai_loop_schedule_get_condition(loaded, 0), ==, "the docs build");
	g_assert_cmpstr(ai_loop_schedule_get_reason(loaded, 0), ==, "two warnings");
	g_assert_cmpuint(ai_loop_schedule_get_turns(loaded, 0), ==, 1);
	g_assert_cmpuint(ai_loop_schedule_get_max_turns(loaded, 0), ==, 7);
	g_assert_cmpint(ai_loop_schedule_get_deadline_us(loaded, 0), ==, now + (gint64)45 * 60 * G_USEC_PER_SEC);
	g_assert_true(ai_loop_schedule_get_revision(loaded) == ai_loop_schedule_get_revision(schedule));
	g_assert_true(ai_loop_schedule_get_revision(loaded) != 0);
}

/* Bounds and sizes are refused, not clamped silently. */
static void
test_goal_bounds_and_commands(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autoptr(GError)         error = NULL;
	g_autofree gchar         *huge = g_strnfill(9000, 'x');
	g_autofree gchar         *id = NULL;
	g_autofree gchar         *line = NULL;
	gchar                    *notice;
	guint                     i;

	goal_error(schedule, "x --turns 500", 1, "at most 200 turns");
	goal_error(schedule, "x --time 30s", 1, "between one minute and 7 days");
	goal_error(schedule, "x --time 8d", 1, "between one minute and 7 days");
	goal_error(schedule, huge, 1, "limited to");
	goal_error(schedule, "--turns 3", 1, "needs a condition");
	goal_error(schedule, "stop", 1, "Usage: /goal stop");
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 0);

	notice = goal_command(schedule, "", 1);
	g_assert_cmpstr(notice, ==, "No goals.");
	g_free(notice);

	/* Verbs followed by more than one word are conditions. */
	notice = goal_command(schedule, "stop the bleeding", 1);
	g_assert_nonnull(strstr(notice, "Goal"));
	g_free(notice);
	g_assert_cmpstr(ai_loop_schedule_get_condition(schedule, 0), ==, "stop the bleeding");
	id = g_strdup(ai_loop_schedule_get_id(schedule, 0));

	line = g_strdup_printf("stop %s", id);
	notice = goal_command(schedule, line, 2);
	g_assert_nonnull(strstr(notice, "Stopped goal"));
	g_free(notice);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_STOPPED);
	g_assert_nonnull(strstr(ai_loop_schedule_get_reason(schedule, 0), "stopped by the user"));

	/* A finished goal cannot be revived by resume or run. */
	g_clear_pointer(&line, g_free);
	line = g_strdup_printf("run %s", id);
	goal_error(schedule, line, 3, "is stopped");
	g_clear_pointer(&line, g_free);
	line = g_strdup_printf("edit %s --turns 50", id);
	goal_error(schedule, line, 3, "start a new goal");

	for (i = 0; i < 20; i++)
	{
		g_autofree gchar *text = g_strdup_printf("goal %u", i);
		g_free(goal_command(schedule, text, 4));
	}

	goal_error(schedule, "one too many", 4, "at most 20 unfinished goals");

	/* Loop interval floor: under a minute rounds up to one, and says so. */
	notice = command(schedule, "10s fast", 4);
	g_assert_nonnull(strstr(notice, "Rounded 10s to every 1m"));
	g_free(notice);
}

/* What every frontend shows comes from here. */
static void
test_vocabulary(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *summary = NULL;
	g_autofree gchar         *json = NULL;
	g_autofree gchar         *line = NULL;
	g_autofree gchar         *details = NULL;
	g_autoptr(JsonParser)     parser = json_parser_new();
	JsonArray                *array;
	gchar                    *text;
	gint64                    now = 1000 * G_USEC_PER_SEC;

	g_assert_cmpstr(ai_loop_state_to_string(AI_LOOP_STATE_ACTIVE), ==, "active");
	g_assert_cmpstr(ai_loop_state_to_string(AI_LOOP_STATE_PAUSED), ==, "paused");
	g_assert_cmpstr(ai_loop_state_to_string(AI_LOOP_STATE_MET), ==, "met");
	g_assert_cmpstr(ai_loop_state_to_string(AI_LOOP_STATE_FAILED), ==, "failed");
	g_assert_cmpstr(ai_loop_state_to_string(AI_LOOP_STATE_STOPPED), ==, "stopped");
	g_assert_cmpstr(ai_loop_state_to_string(AI_LOOP_STATE_EXPIRED), ==, "expired");
	g_assert_false(ai_loop_state_is_final(AI_LOOP_STATE_PAUSED));
	g_assert_true(ai_loop_state_is_final(AI_LOOP_STATE_EXPIRED));

	text = ai_loop_format_duration(30 * G_USEC_PER_SEC);
	g_assert_cmpstr(text, ==, "<1m");
	g_free(text);
	text = ai_loop_format_duration((gint64)125 * 60 * G_USEC_PER_SEC);
	g_assert_cmpstr(text, ==, "2h 5m");
	g_free(text);
	text = ai_loop_format_duration((gint64)50 * 3600 * G_USEC_PER_SEC);
	g_assert_cmpstr(text, ==, "2d 2h");
	g_free(text);
	text = ai_loop_format_relative(-5);
	g_assert_cmpstr(text, ==, "now");
	g_free(text);
	text = ai_loop_format_relative((gint64)4 * 60 * G_USEC_PER_SEC);
	g_assert_cmpstr(text, ==, "in 4m");
	g_free(text);

	g_assert_null(ai_loop_schedule_dup_summary(schedule, now));
	g_free(command(schedule, "check the deploy", now));
	g_free(ai_loop_schedule_add_goal(schedule, "the tests pass", 20, 0, now, NULL));
	summary = ai_loop_schedule_dup_summary(schedule, now);
	g_assert_cmpstr(summary, ==, "1 loop, 1 goal, next now");

	line = ai_loop_schedule_dup_line(schedule, 1, now);
	g_assert_nonnull(strstr(line, "goal  active  turn 0/20, 2h left  the tests pass"));
	details = ai_loop_schedule_dup_details(schedule, 0, now);
	g_assert_nonnull(strstr(details, "Trigger:    prompt"));
	g_assert_nonnull(strstr(details, "Cadence:    self-paced"));

	json = ai_loop_schedule_dup_json(schedule, now);
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	array = json_node_get_array(json_parser_get_root(parser));
	g_assert_cmpuint(json_array_get_length(array), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(array, 1), "state"), ==, "active");
	g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(array, 1), "kind"), ==, "goal");
	g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(array, 1), "max_turns"), ==, 20);
}

/* A goal that ran out of time while nothing was running is ended by the
 * reap with its reason, and a paused loop past its seven days goes. */
static void
test_reap(void)
{
	g_autoptr(AiLoopSchedule) schedule = ai_loop_schedule_new();
	g_autofree gchar         *notice = NULL;
	gint64                    now = 1000 * G_USEC_PER_SEC;
	gint64                    week = (gint64)7 * 24 * 3600 * G_USEC_PER_SEC;

	g_free(ai_loop_schedule_add_goal(schedule, "g", 0, 0, now, NULL));
	g_free(command(schedule, "30m l", now));
	g_assert_true(ai_loop_schedule_pause(schedule, ai_loop_schedule_get_id(schedule, 1), NULL));
	g_assert_null(ai_loop_schedule_reap(schedule, now + 1));
	notice = ai_loop_schedule_reap(schedule, now + week);
	g_assert_nonnull(strstr(notice, "expired: not met within 2h, its time bound, after 0 turns"));
	g_assert_nonnull(strstr(notice, "loops end 7 days after they are created"));
	g_assert_cmpuint(ai_loop_schedule_get_n_tasks(schedule), ==, 1);
	g_assert_cmpint(ai_loop_schedule_get_state(schedule, 0), ==, AI_LOOP_STATE_EXPIRED);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	setup_sandbox();
	/* Nothing here reads them, and nothing may: a test that found the
	 * developer's own loop.md would pass or fail by whose machine ran it. */
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_setenv("XDG_STATE_HOME", sandbox, TRUE);
	g_assert_cmpint(g_chdir(sandbox), ==, 0);
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
	g_test_add_func("/ai-glib/loop/edit-pause-resume-delete", test_edit_pause_resume_delete);
	g_test_add_func("/ai-glib/loop/ids", test_ids_are_unambiguous);
	g_test_add_func("/ai-glib/loop/corrupt-file", test_corrupt_file);
	g_test_add_func("/ai-glib/loop/goal-round-trip", test_goal_round_trip);
	g_test_add_func("/ai-glib/loop/goal-bounds", test_goal_bounds_and_commands);
	g_test_add_func("/ai-glib/loop/vocabulary", test_vocabulary);
	g_test_add_func("/ai-glib/loop/reap", test_reap);
	{
		gint status = g_test_run();
		teardown_sandbox();
		return status;
	}
}
