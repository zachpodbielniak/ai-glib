/*
 * ai-tui-update.h - The update badge and /update, for ai-tui
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by ai-tui.c after App, say() and the terminal helpers. The
 * check and the pipeline are AiUpdater's and the sentences are
 * core/ai-update-status.h's; this file decides only where they go in a
 * terminal:
 *
 *  - a badge on the status line, only when an update exists;
 *  - `/update status`, answered on a worker thread;
 *  - `/update`, which hands the terminal over exactly as ^G hands it to
 *    $EDITOR, so sudo can prompt and make's output scrolls by in the
 *    open; then it waits for Enter and takes the screen back.
 *
 * Under --dump there is no terminal to hand over, so /update runs
 * without one and stops before a privileged install, printing the
 * command -- the same outcome ai-gui has.
 */

#pragma once

#include "core/ai-updater.h"

static void
tui_update_on_status(AiUpdater *updater, gpointer data)
{
	app_schedule_redraw(data);
}

/* The updater, created on first use. The background monitor is started
 * separately, and only when checks are enabled. */
static AiUpdater *
tui_update_updater(App *app)
{
	if (app->updater == NULL)
	{
		g_autoptr(AiConfig) config = ai_config_new();

		app->updater = ai_updater_new(config);
		g_signal_connect(app->updater, "status-changed",
		                 G_CALLBACK(tui_update_on_status), app);
	}
	return app->updater;
}

static void
tui_update_start(App *app)
{
	g_autoptr(AiConfig) config = ai_config_new();

	if (!ai_updater_checks_enabled(config))
		return;
	ai_updater_start(tui_update_updater(app));
}

/* Teardown: stop, cancel, drain, disconnect -- in that order, so no
 * callback outlives the App it points at. */
static void
tui_update_stop(App *app)
{
	if (app->update_cancel != NULL)
		g_cancellable_cancel(app->update_cancel);
	if (app->updater != NULL)
		ai_updater_stop(app->updater);
	/* The monitor's own check too: a process that exited before the
	 * worker noticed the cancel would leave git running for nobody. */
	while (app->update_pending > 0 ||
	       (app->updater != NULL && ai_updater_is_checking(app->updater)))
		g_main_context_iteration(NULL, TRUE);
	g_clear_object(&app->update_cancel);
	if (app->updater != NULL)
		g_signal_handlers_disconnect_by_data(app->updater, app);
	g_clear_object(&app->updater);
}

static gchar *
tui_update_badge(App *app)
{
	if (app->updater == NULL)
		return NULL;
	return ai_update_status_dup_badge(ai_updater_get_status(app->updater));
}

static void
tui_update_say_version(App *app)
{
	g_autofree gchar *summary = ai_build_info_dup_summary();

	say(app, "ai-tui %s", summary);
}

static void
tui_update_checked(GObject *source, GAsyncResult *res, gpointer data)
{
	App *app = data;
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;

	app->update_pending--;
	status = ai_updater_check_finish(AI_UPDATER(source), res, &error);
	if (status == NULL)
	{
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			say(app, "Update check failed: %s", error->message);
		return;
	}
	summary = ai_update_status_dup_summary(status);
	say(app, "%s", summary);
	app_schedule_redraw(app);
}

static void
tui_update_on_step(AiUpdater *updater, const gchar *step, gpointer data)
{
	say(data, "==> %s", step);
}

static void
tui_update_on_output(AiUpdater *updater, const gchar *line, gpointer data)
{
	g_print("%s\n", line);
}

/* /update with no terminal: the --dump path. */
static void
tui_update_run_detached(App *app)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;
	AiUpdater *updater = tui_update_updater(app);
	gulong step;

	step = g_signal_connect(updater, "step", G_CALLBACK(tui_update_on_step), app);
	result = ai_updater_run(updater, AI_UPDATE_RUN_NONE, NULL, &error);
	g_signal_handler_disconnect(updater, step);
	if (result == NULL)
	{
		say(app, "%s", error->message);
		return;
	}
	summary = ai_update_result_dup_summary(result);
	say(app, "%s", summary);
}

/* /update on a terminal: hand it over, run, wait for Enter, take it back. */
static void
tui_update_run_interactive(App *app)
{
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;
	AiUpdater *updater = tui_update_updater(app);
	gulong output;
	gint c;

	def_prog_mode();
	fputs("\033[?2004l", stdout);
	fflush(stdout);
	tui_mouse_disable();
	endwin();

	g_print("\nUpdating ai-glib. ^C stops it; nothing is installed until the build succeeds.\n\n");
	output = g_signal_connect(updater, "output", G_CALLBACK(tui_update_on_output), app);
	result = ai_updater_run(updater, AI_UPDATE_RUN_INTERACTIVE, NULL, &error);
	g_signal_handler_disconnect(updater, output);
	summary = result != NULL ? ai_update_result_dup_summary(result) : g_strdup(error->message);
	g_print("\n%s\n\nPress Enter to return to ai-tui.", summary);
	fflush(stdout);
	do
		c = getchar();
	while (c != '\n' && c != EOF);
	clearerr(stdin);

	reset_prog_mode();
	fputs("\033[?2004h", stdout);
	fflush(stdout);
	tui_mouse_enable();
	on_resize(app);
	clearok(curscr, TRUE);
	say(app, "%s", summary);
}

/*
 * /update [status]. A running turn refuses the run -- it would lose the
 * terminal and have its binary replaced underneath it -- and says so in
 * the same words ai-gui uses.
 */
static void
tui_update_command(App *app, const gchar *arguments)
{
	g_autofree gchar *args = g_strstrip(g_strdup(arguments != NULL ? arguments : ""));

	if (g_str_equal(args, "status"))
	{
		tui_update_say_version(app);
		if (app->dump_loop != NULL)
		{
			g_autoptr(AiUpdateStatus) status = NULL;
			g_autoptr(GError) error = NULL;
			g_autofree gchar *summary = NULL;

			status = ai_updater_check(tui_update_updater(app), TRUE, NULL, &error);
			summary = status != NULL ? ai_update_status_dup_summary(status)
			                         : g_strdup(error->message);
			say(app, "%s", summary);
			return;
		}
		say(app, "Checking for updates…");
		if (app->update_cancel == NULL)
			app->update_cancel = g_cancellable_new();
		app->update_pending++;
		ai_updater_check_async(tui_update_updater(app), TRUE, app->update_cancel,
		                       tui_update_checked, app);
		return;
	}

	if (args[0] != '\0')
	{
		say(app, "Usage: /update [status]");
		return;
	}

	if (ai_conversation_get_busy(app->conversation) || app->update_pending > 0)
	{
		g_autofree gchar *refusal = ai_update_status_dup_refusal(
			NULL, ai_conversation_get_busy(app->conversation));

		say(app, "%s", app->update_pending > 0 ? "An update check is running; try again in a moment."
		                                       : refusal);
		return;
	}

	if (app->dump_loop != NULL)
		tui_update_run_detached(app);
	else
		tui_update_run_interactive(app);
}
