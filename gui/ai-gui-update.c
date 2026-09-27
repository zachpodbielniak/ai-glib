/*
 * ai-gui-update.c - The update banner's state, and /update, for ai-gui
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * GTK-free; see the header. Every completion holds a reference to this
 * object and checks @shut_down before saying anything, so a window
 * closed mid-build hears nothing afterwards -- the run itself is
 * cancelled and its make killed.
 */

#include "ai-gui-update.h"

struct _AiGuiUpdate
{
	GObject       parent_instance;

	AiUpdater    *updater;
	GCancellable *cancel;
	gulong        status_id;
	gulong        step_id;
	gboolean      running;
	guint         checking;
	gboolean      shut_down;
};

G_DEFINE_FINAL_TYPE(AiGuiUpdate, ai_gui_update, G_TYPE_OBJECT)

enum
{
	SIGNAL_CHANGED,
	SIGNAL_MESSAGE,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
update_say(AiGuiUpdate *self, const gchar *text)
{
	if (!self->shut_down)
		g_signal_emit(self, signals[SIGNAL_MESSAGE], 0, text);
}

static void
update_changed(AiGuiUpdate *self)
{
	if (!self->shut_down)
		g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static void
on_status_changed(AiUpdater *updater, gpointer data)
{
	update_changed(data);
}

static void
on_step(AiUpdater *updater, const gchar *step, gpointer data)
{
	g_autofree gchar *text = g_strdup_printf("==> %s", step);

	update_say(data, text);
}

static void
ai_gui_update_dispose(GObject *object)
{
	AiGuiUpdate *self = AI_GUI_UPDATE(object);

	ai_gui_update_shutdown(self);
	g_clear_object(&self->updater);
	g_clear_object(&self->cancel);
	G_OBJECT_CLASS(ai_gui_update_parent_class)->dispose(object);
}

static void
ai_gui_update_class_init(AiGuiUpdateClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = ai_gui_update_dispose;

	/* The banner text or the busy flag may have changed. */
	signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
	/* A line for the transcript: a step, a refusal, an outcome. */
	signals[SIGNAL_MESSAGE] = g_signal_new("message", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
ai_gui_update_init(AiGuiUpdate *self)
{
	self->cancel = g_cancellable_new();
}

AiGuiUpdate *
ai_gui_update_new(AiUpdater *updater)
{
	AiGuiUpdate *self;

	g_return_val_if_fail(AI_IS_UPDATER(updater), NULL);

	self = g_object_new(AI_GUI_TYPE_UPDATE, NULL);
	self->updater = g_object_ref(updater);
	self->status_id = g_signal_connect(updater, "status-changed",
	                                   G_CALLBACK(on_status_changed), self);
	self->step_id = g_signal_connect(updater, "step", G_CALLBACK(on_step), self);
	return self;
}

/* The background check, unless the config or the environment said no. */
void
ai_gui_update_start(AiGuiUpdate *self)
{
	g_autoptr(AiConfig) config = ai_config_new();

	g_return_if_fail(AI_GUI_IS_UPDATE(self));

	if (!self->shut_down && ai_updater_checks_enabled(config))
		ai_updater_start(self->updater);
}

/* Stop, cancel, disconnect. Safe to call twice; nothing is said after. */
void
ai_gui_update_shutdown(AiGuiUpdate *self)
{
	g_return_if_fail(AI_GUI_IS_UPDATE(self));

	if (self->shut_down)
		return;
	self->shut_down = TRUE;
	g_cancellable_cancel(self->cancel);
	if (self->updater != NULL)
	{
		ai_updater_stop(self->updater);
		g_clear_signal_handler(&self->status_id, self->updater);
		g_clear_signal_handler(&self->step_id, self->updater);
	}
}

/* A run or an explicit check is in flight. */
gboolean
ai_gui_update_get_busy(AiGuiUpdate *self)
{
	g_return_val_if_fail(AI_GUI_IS_UPDATE(self), FALSE);
	return !self->shut_down && (self->running || self->checking > 0);
}

/* What the banner says, or NULL to hide it. */
gchar *
ai_gui_update_dup_banner(AiGuiUpdate *self)
{
	const AiUpdateStatus *status;
	g_autofree gchar *badge = NULL;

	g_return_val_if_fail(AI_GUI_IS_UPDATE(self), NULL);

	if (self->shut_down)
		return NULL;
	if (self->running)
		return g_strdup("Updating ai-glib…");
	status = ai_updater_get_status(self->updater);
	badge = ai_update_status_dup_badge(status);
	return badge != NULL ? ai_update_status_dup_summary(status) : NULL;
}

static void
on_checked(GObject *source, GAsyncResult *res, gpointer data)
{
	g_autoptr(AiGuiUpdate) self = data;
	g_autoptr(AiUpdateStatus) status = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;

	self->checking--;
	status = ai_updater_check_finish(AI_UPDATER(source), res, &error);
	if (self->shut_down)
		return;
	summary = status != NULL ? ai_update_status_dup_summary(status)
	                         : g_strdup_printf("Update check failed: %s", error->message);
	update_say(self, summary);
	update_changed(self);
}

/* /update status: always fetches -- somebody asked. */
void
ai_gui_update_check(AiGuiUpdate *self)
{
	g_autofree gchar *version = ai_build_info_dup_summary();
	g_autofree gchar *line = g_strdup_printf("ai-gui %s", version);

	g_return_if_fail(AI_GUI_IS_UPDATE(self));

	if (self->shut_down)
		return;
	update_say(self, line);
	update_say(self, "Checking for updates…");
	self->checking++;
	ai_updater_check_async(self->updater, TRUE, self->cancel, on_checked, g_object_ref(self));
}

static void
on_run_done(GObject *source, GAsyncResult *res, gpointer data)
{
	g_autoptr(AiGuiUpdate) self = data;
	g_autoptr(AiUpdateResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *summary = NULL;

	self->running = FALSE;
	result = ai_updater_run_finish(AI_UPDATER(source), res, &error);
	if (self->shut_down)
		return;
	summary = result != NULL ? ai_update_result_dup_summary(result) : g_strdup(error->message);
	update_say(self, summary);
	update_changed(self);
}

/*
 * /update, or the banner's button. @turn_running is the window's answer
 * for every session it holds: an update under a streaming turn is
 * refused in the same words ai-tui uses.
 */
void
ai_gui_update_run(AiGuiUpdate *self, gboolean turn_running)
{
	g_return_if_fail(AI_GUI_IS_UPDATE(self));

	if (self->shut_down)
		return;
	if (self->running)
	{
		update_say(self, "An update is already running.");
		return;
	}
	if (turn_running)
	{
		g_autofree gchar *refusal = ai_update_status_dup_refusal(NULL, TRUE);

		update_say(self, refusal);
		return;
	}

	self->running = TRUE;
	update_say(self, "Updating ai-glib. Nothing is installed until the build succeeds.");
	update_changed(self);
	ai_updater_run_async(self->updater, AI_UPDATE_RUN_NONE, self->cancel, on_run_done,
	                     g_object_ref(self));
}
