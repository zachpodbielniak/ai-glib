/*
 * ai-gui-window.c - The window: sessions on the left, a conversation on the right
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui.h"
#include "ai-gui-agents.h"
#include "ai-gui-approval.h"
#include "ai-gui-chat.h"
#include "ai-gui-commands.h"
#include "ai-gui-dashboard.h"
#include "ai-gui-composer.h"
#include "ai-gui-prefs.h"
#include "ai-gui-quota.h"
#include "ai-gui-session-store.h"
#include "ai-gui-settings.h"
#include "ai-gui-sidebar.h"
#include "ai-gui-style.h"
#include "ai-gui-work.h"

struct _AiGuiWindow
{
	AdwApplicationWindow parent_instance;

	AiGuiOptions      *options;
	AiGuiSettings     *settings;
	AiGuiSessionStore *store;
	AiGuiSession      *session;

	GtkWidget *toasts;
	GtkWidget *split;
	GtkWidget *sidebar;
	GtkWidget *stack;
	GtkWidget *dashboard;
	GtkWidget *chat;
	GtkWidget *composer;
	GtkWidget *title;
	GtkWidget *search_bar;
	GtkWidget *search_entry;
	GtkWidget *banner;
	GtkWidget *links_button;
	GtkWidget *quota;
	GtkWidget *links_popover;
	GtkWidget *links_list;
	GtkWidget *dashboard_button;
	GtkWidget *sidebar_title;

	gulong turn_id;
	gulong approval_id;
	gulong agent_id;
	gulong busy_id;
	gulong builtin_id;
};

G_DEFINE_FINAL_TYPE(AiGuiWindow, ai_gui_window, ADW_TYPE_APPLICATION_WINDOW)

static void window_set_session(AiGuiWindow *self, AiGuiSession *session);

/* ================================================================
 * Toasts
 * ================================================================ */

void
ai_gui_window_toast(
	AiGuiWindow *self,
	const gchar *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list args;

	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);

	adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toasts),
	                            adw_toast_new(text));
}

/* ================================================================
 * Session lifecycle
 * ================================================================ */

static void
on_turn_finished(
	AiGuiSession *session,
	gboolean      success,
	const gchar  *message,
	gpointer      user_data
){
	AiGuiWindow *self = user_data;

	if (!success && message != NULL)
		ai_gui_window_toast(self, "%s", message);

	/* A turn can have linked work through a tool or a /work assignment
	 * that resolved while it ran. */
	ai_gui_window_refresh_links(self);

	/*
	 * Saved at the end of every turn rather than only at shutdown.
	 *
	 * A crash mid-session is exactly when somebody wants the transcript
	 * back, and it is exactly when the shutdown path does not run.
	 */
	{
		g_autoptr(GError) error = NULL;

		if (!ai_gui_session_store_save(self->store, session, &error))
			g_debug("ai-gui: could not save session: %s",
			        error != NULL ? error->message : "unknown");
	}
}

static void
on_builtin_command(
	AiGuiSession    *session,
	AiCommandResult *command,
	gpointer         user_data
){
	AiGuiWindow *self = user_data;

	ai_gui_commands_handle(self, session, command);
	ai_gui_composer_sync_model(AI_GUI_COMPOSER(self->composer));
}

static AiToolApproval
on_approval_requested(
	AiGuiSession *session,
	AiToolUse    *tool_use,
	gpointer      user_data
){
	return ai_gui_approval_ask(GTK_WINDOW(user_data), tool_use);
}

static void
on_agent_finished(
	AiGuiSession *session,
	const gchar  *agent_id,
	gint          state,
	gpointer      user_data
){
	/*
	 * That an agent finished, never what it said.
	 *
	 * Pasting a subagent's answer into a conversation that has not asked
	 * for it is how delegating costs more context than doing the work
	 * inline -- and the same reasoning applies to a person's screen.
	 */
	ai_gui_window_toast(user_data, "Agent %s %s", agent_id,
	                    ai_agent_state_to_string((AiAgentState)state));
}

static void
on_busy_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiWindow *self = user_data;

	ai_gui_composer_set_busy(AI_GUI_COMPOSER(self->composer),
	                         ai_gui_session_get_busy(self->session));
}

static void
on_title_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiWindow *self = user_data;

	if (self->session == NULL)
		return;

	adw_window_title_set_title(ADW_WINDOW_TITLE(self->title),
	                           ai_gui_session_get_title(self->session));
}

static void
window_disconnect_session(AiGuiWindow *self)
{
	if (self->session == NULL)
		return;

	g_clear_signal_handler(&self->turn_id, self->session);
	g_clear_signal_handler(&self->approval_id, self->session);
	g_clear_signal_handler(&self->agent_id, self->session);
	g_clear_signal_handler(&self->busy_id, self->session);
	g_clear_signal_handler(&self->builtin_id, self->session);
	g_signal_handlers_disconnect_by_func(self->session,
		G_CALLBACK(on_title_changed), self);
}

static void
window_update_subtitle(AiGuiWindow *self)
{
	g_autofree gchar *subtitle = NULL;

	if (self->session == NULL)
	{
		adw_window_title_set_title(ADW_WINDOW_TITLE(self->title), "ai-gui");
		adw_window_title_set_subtitle(ADW_WINDOW_TITLE(self->title), "");
		return;
	}

	subtitle = g_strdup_printf("%s · %s",
		ai_gui_session_get_provider_name(self->session),
		ai_gui_session_get_model(self->session) != NULL
			&& *ai_gui_session_get_model(self->session) != '\0'
			? ai_gui_session_get_model(self->session) : "default");

	adw_window_title_set_title(ADW_WINDOW_TITLE(self->title),
	                           ai_gui_session_get_title(self->session));
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(self->title), subtitle);
}

static void
on_provider_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	window_update_subtitle(user_data);
}

static void
window_set_session(
	AiGuiWindow  *self,
	AiGuiSession *session
){
	if (self->session == session)
		return;

	window_disconnect_session(self);
	g_set_object(&self->session, session);

	ai_gui_chat_view_set_session(AI_GUI_CHAT_VIEW(self->chat), session);
	ai_gui_composer_set_session(AI_GUI_COMPOSER(self->composer), session);

	if (session != NULL)
	{
		self->turn_id = g_signal_connect(session, "turn-finished",
			G_CALLBACK(on_turn_finished), self);
		self->approval_id = g_signal_connect(session, "approval-requested",
			G_CALLBACK(on_approval_requested), self);
		self->agent_id = g_signal_connect(session, "agent-finished",
			G_CALLBACK(on_agent_finished), self);
		self->busy_id = g_signal_connect(session, "notify::busy",
			G_CALLBACK(on_busy_changed), self);
		self->builtin_id = g_signal_connect(session, "builtin-command",
			G_CALLBACK(on_builtin_command), self);
		g_signal_connect(session, "notify::title",
			G_CALLBACK(on_title_changed), self);
		g_signal_connect(session, "notify::provider-name",
			G_CALLBACK(on_provider_changed), self);
		g_signal_connect(session, "notify::model",
			G_CALLBACK(on_provider_changed), self);

		ai_gui_composer_set_busy(AI_GUI_COMPOSER(self->composer),
		                         ai_gui_session_get_busy(session));
		ai_gui_sidebar_select(AI_GUI_SIDEBAR(self->sidebar), session);
	}

	ai_gui_window_refresh_links(self);

	if (self->quota != NULL)
		ai_gui_quota_set_session(AI_GUI_QUOTA(self->quota), session);

	window_update_subtitle(self);
}

static AiGuiSession *
window_new_session(AiGuiWindow *self)
{
	g_autoptr(AiGuiOptions) options = ai_gui_options_copy(self->options);
	g_autoptr(GError) error = NULL;
	AiGuiSession *session;

	/*
	 * In the project somebody is already in, not the one ai-gui was
	 * launched from.
	 *
	 * With the list grouped by project, a Ctrl+N that dropped a new row
	 * into a different group than the one being worked in would read as
	 * the grouping being wrong rather than as the directory being
	 * inherited from the command line.
	 */
	if (self->session != NULL)
	{
		const gchar *directory =
			ai_gui_session_get_working_directory(self->session);

		if (directory != NULL && *directory != '\0')
		{
			g_clear_pointer(&options->working_directory, g_free);
			options->working_directory = g_strdup(directory);
		}
	}

	session = ai_gui_session_new(options, options->provider,
	                             options->model, &error);

	if (session == NULL)
	{
		ai_gui_window_toast(self, "%s",
			error != NULL ? error->message : "could not start a session");
		return NULL;
	}

	ai_gui_session_store_add(self->store, session);
	window_set_session(self, session);
	g_object_unref(session);

	ai_gui_composer_focus(AI_GUI_COMPOSER(self->composer));

	return session;
}

/* ================================================================
 * Sidebar
 * ================================================================ */

static void
on_session_selected(
	AiGuiSidebar *sidebar,
	AiGuiSession *session,
	gpointer      user_data
){
	AiGuiWindow *self = user_data;

	if (session != NULL)
		window_set_session(self, session);

	if (adw_overlay_split_view_get_collapsed(
		ADW_OVERLAY_SPLIT_VIEW(self->split)))
	{
		adw_overlay_split_view_set_show_sidebar(
			ADW_OVERLAY_SPLIT_VIEW(self->split), FALSE);
	}
}

/* ================================================================
 * Composer
 * ================================================================ */

/*
 * Apply the pickers before anything is sent.
 *
 * "Per question" is only true if the switch happens between questions,
 * so it happens here rather than when a menu changes: a switch while a
 * reply is still arriving would move the conversation out from under it.
 *
 * Returns: %FALSE when the question must not be sent after all
 */
static gboolean
window_apply_selection(AiGuiWindow *self)
{
	const gchar *provider = ai_gui_composer_get_selected_provider(
		AI_GUI_COMPOSER(self->composer));
	const gchar *model = ai_gui_composer_get_selected_model(
		AI_GUI_COMPOSER(self->composer));
	g_autoptr(GError) error = NULL;

	if (provider == NULL)
		return TRUE;

	if (g_strcmp0(provider, ai_gui_session_get_provider_id(self->session)) == 0 &&
	    g_strcmp0(model, ai_gui_session_get_model(self->session)) == 0)
	{
		return TRUE;
	}

	/*
	 * A turn in flight owns the provider. Saying so and queueing against
	 * the current one is better than either refusing the text or
	 * pretending the queued question went somewhere it did not.
	 */
	if (ai_gui_session_get_busy(self->session))
	{
		ai_gui_window_toast(self,
			"Still answering — this follow-up uses %s · %s. "
			"Switch once the turn ends.",
			ai_gui_session_get_provider_id(self->session),
			ai_gui_session_get_model(self->session) != NULL
				? ai_gui_session_get_model(self->session) : "default");
		return TRUE;
	}

	if (ai_gui_session_switch_provider(self->session, provider, model, &error))
	{
		ai_gui_composer_sync_model(AI_GUI_COMPOSER(self->composer));
		return TRUE;
	}

	/*
	 * The text stays in the composer. Sending it to the provider they
	 * just moved away from is the one outcome nobody asked for.
	 */
	ai_gui_window_toast(self, "%s", error != NULL
		? error->message : "could not switch provider");

	return FALSE;
}

static void
on_composer_submit(
	AiGuiComposer *composer,
	gpointer       user_data
){
	AiGuiWindow *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	GList *images;

	if (self->session == NULL && window_new_session(self) == NULL)
		return;

	if (!window_apply_selection(self))
		return;

	text = ai_gui_composer_take_text(AI_GUI_COMPOSER(self->composer));
	images = ai_gui_composer_take_images(AI_GUI_COMPOSER(self->composer));

	if ((text == NULL || *text == '\0') && images == NULL)
		return;

	if (!ai_gui_session_send(self->session, text != NULL ? text : "",
	                         images, &error))
	{
		ai_gui_window_toast(self, "%s",
			error != NULL ? error->message : "could not send");
	}

	g_list_free_full(images, g_object_unref);
}

static void
on_composer_notice(
	AiGuiComposer *composer,
	const gchar   *message,
	gpointer       user_data
){
	ai_gui_window_toast(user_data, "%s", message);
}

static void
on_composer_stop(
	AiGuiComposer *composer,
	gpointer       user_data
){
	AiGuiWindow *self = user_data;

	if (self->session != NULL)
		ai_gui_session_cancel(self->session);
}

/* ================================================================
 * Appearance
 * ================================================================ */

/*
 * The chrome follows the stylesheet by itself; the transcript does not.
 * Registered once for the window's lifetime so it also covers the change
 * nobody here initiated -- the desktop going dark at sunset.
 */
static void
on_appearance_changed(gpointer user_data)
{
	AiGuiWindow *self = user_data;

	if (self->chat != NULL)
		ai_gui_chat_view_restyle(AI_GUI_CHAT_VIEW(self->chat));
}

static void
window_remember_appearance(AiGuiWindow *self)
{
	g_autoptr(GError) error = NULL;

	if (!ai_gui_settings_save(self->settings, NULL, &error))
	{
		/*
		 * g_debug: a data directory that will not take a write is the
		 * machine, not a bug here, and the only consequence is that the
		 * choice does not survive the next start.
		 */
		g_debug("ai-gui: could not save appearance: %s", error->message);
	}
}

void
ai_gui_window_set_theme(
	AiGuiWindow *self,
	const gchar *name
){
	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	if (!ai_gui_style_set_theme(name))
	{
		ai_gui_window_toast(self, "No theme called '%s'.", name);
		return;
	}

	ai_gui_settings_set_theme(self->settings, name);
	window_remember_appearance(self);
}

void
ai_gui_window_set_color_scheme(
	AiGuiWindow *self,
	const gchar *name
){
	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	if (!ai_gui_style_set_color_scheme(name))
		return;

	ai_gui_settings_set_color_scheme(self->settings, name);
	window_remember_appearance(self);
}

/* ================================================================
 * The dashboard
 * ================================================================ */

void
ai_gui_window_show_dashboard(
	AiGuiWindow *self,
	gboolean     show
){
	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	gtk_stack_set_visible_child_name(GTK_STACK(self->stack),
	                                 show ? "dashboard" : "chat");

	/* Polling stops with the page: a window showing a conversation has
	 * no use for a registry re-read every two seconds. */
	ai_gui_dashboard_set_polling(AI_GUI_DASHBOARD(self->dashboard), show);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->dashboard_button),
	                             show);

	if (!show)
		ai_gui_composer_focus(AI_GUI_COMPOSER(self->composer));
}

gboolean
ai_gui_window_get_dashboard(AiGuiWindow *self)
{
	g_return_val_if_fail(AI_GUI_IS_WINDOW(self), FALSE);

	return g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(self->stack)),
	                 "dashboard") == 0;
}

/*
 * A session in a directory of somebody's choosing.
 *
 * @adopt is the dashboard's resume: the new session takes over the
 * existing record rather than starting a second identity beside it, so
 * the links and the title somebody has already attached survive.
 */
static AiGuiSession *
window_open_session(
	AiGuiWindow   *self,
	const gchar   *directory,
	const gchar   *provider,
	const gchar   *model,
	const gchar   *native_session,
	AiWorkSession *adopt
){
	g_autoptr(AiGuiOptions) options = ai_gui_options_copy(self->options);
	g_autoptr(GError) error = NULL;
	AiGuiSession *session;

	if (directory != NULL && *directory != '\0')
	{
		g_clear_pointer(&options->working_directory, g_free);
		options->working_directory = g_strdup(directory);
	}

	if (provider != NULL && *provider != '\0')
	{
		g_clear_pointer(&options->provider, g_free);
		options->provider = g_strdup(provider);
	}

	if (model != NULL && *model != '\0')
	{
		g_clear_pointer(&options->model, g_free);
		options->model = g_strdup(model);
	}

	/* A named native session beats --continue; they are two answers to
	 * "which session" and this one was clicked on. */
	if (native_session != NULL && *native_session != '\0')
		options->continue_session = FALSE;

	session = ai_gui_session_new(options, options->provider, options->model,
	                             &error);

	if (session == NULL)
	{
		ai_gui_window_toast(self, "%s", error != NULL
			? error->message : "could not start a session");
		return NULL;
	}

	if (native_session != NULL && *native_session != '\0')
	{
		GObject *provider_object = ai_gui_session_get_provider(session);

		if (provider_object != NULL &&
		    g_object_class_find_property(G_OBJECT_GET_CLASS(provider_object),
		                                 "session-id") != NULL)
		{
			g_object_set(provider_object, "session-id", native_session, NULL);
		}
		else
		{
			ai_gui_window_toast(self,
				"%s cannot resume by session id; starting fresh.",
				ai_gui_session_get_provider_name(session));
		}
	}

	if (adopt != NULL)
	{
		g_autoptr(GError) claim_error = NULL;

		if (!ai_gui_session_adopt_work(session, adopt, &claim_error))
		{
			ai_gui_window_toast(self, "%s", claim_error->message);
			g_object_unref(session);
			return NULL;
		}
	}

	ai_gui_session_store_add(self->store, session);
	window_set_session(self, session);
	g_object_unref(session);

	ai_gui_window_show_dashboard(self, FALSE);

	return session;
}

void
ai_gui_window_open_project(
	AiGuiWindow *self,
	const gchar *directory
){
	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	window_open_session(self, directory, NULL, NULL, NULL, NULL);
}

static void
on_project_folder_chosen(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiWindow *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(
		GTK_FILE_DIALOG(source), result, &error);
	g_autofree gchar *path = NULL;

	if (folder == NULL)
	{
		/*
		 * g_debug, not a toast: the overwhelmingly common failure here
		 * is somebody pressing Cancel, and a window that complained
		 * about it would be complaining about being used correctly.
		 */
		g_debug("ai-gui: no project chosen: %s",
		        error != NULL ? error->message : "dismissed");
		g_object_unref(self);
		return;
	}

	path = g_file_get_path(folder);

	if (path == NULL)
	{
		ai_gui_window_toast(self,
			"That folder is not on this machine's filesystem.");
	}
	else
	{
		ai_gui_window_open_project(self, path);
	}

	g_object_unref(self);
}

/*
 * Open a folder as a project: a new session in it, in its own group.
 *
 * Reachable from the window rather than only from the dashboard, because
 * "work on something else now" is a thing somebody does from the
 * conversation they are already in.
 */
static void
action_open_project(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();

	gtk_file_dialog_set_title(dialog, "Open a project");

	/* Start where the current session is, so the chooser opens beside
	 * the work rather than in the home directory. */
	if (self->session != NULL)
	{
		const gchar *directory =
			ai_gui_session_get_working_directory(self->session);

		if (directory != NULL && *directory != '\0')
		{
			g_autoptr(GFile) start = g_file_new_for_path(directory);

			gtk_file_dialog_set_initial_folder(dialog, start);
		}
	}

	gtk_file_dialog_select_folder(dialog, GTK_WINDOW(self), NULL,
	                              on_project_folder_chosen,
	                              g_object_ref(self));
}

static void
on_dashboard_activated(
	AiGuiDashboard *dashboard,
	AiGuiSession   *session,
	gpointer        user_data
){
	AiGuiWindow *self = user_data;

	window_set_session(self, session);
	ai_gui_window_show_dashboard(self, FALSE);
}

static void
on_dashboard_closed(
	AiGuiDashboard *dashboard,
	gpointer        user_data
){
	ai_gui_window_show_dashboard(user_data, FALSE);
}

static void
on_dashboard_project(
	AiGuiDashboard *dashboard,
	const gchar    *directory,
	gpointer        user_data
){
	ai_gui_window_open_project(user_data, directory);
}

static void
on_dashboard_resume(
	AiGuiDashboard *dashboard,
	AiWorkSession  *work,
	gpointer        user_data
){
	AiGuiWindow *self = user_data;

	if (!ai_gui_work_can_resume(work))
	{
		ai_gui_window_toast(self,
			"Only a disconnected session with a native provider session "
			"id can be resumed.");
		return;
	}

	window_open_session(self,
		ai_work_session_get_field(work, "directory"),
		ai_work_session_get_field(work, "provider"),
		ai_work_session_get_field(work, "model"),
		ai_work_session_get_field(work, "provider-session"),
		work);
}

static void
on_worktree_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiWindow *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = ai_gui_work_create_worktree_finish(result, &error);

	if (path == NULL)
	{
		ai_gui_window_toast(self, "%s", error != NULL
			? error->message : "could not create the worktree");
	}
	else
	{
		/*
		 * The checkout is kept whatever happens next. Removing it on a
		 * failure to open a session would delete a branch somebody may
		 * already have work in; cleanup is Git's usual commands.
		 */
		ai_gui_window_toast(self, "Worktree at %s", path);
		window_open_session(self, path, NULL, NULL, NULL, NULL);
	}

	g_object_unref(self);
}

static void
on_dashboard_worktree(
	AiGuiDashboard *dashboard,
	AiWorkSession  *work,
	gpointer        user_data
){
	AiGuiWindow *self = user_data;

	ai_gui_window_toast(self, "Creating a worktree…");
	ai_gui_work_create_worktree_async(
		ai_work_session_get_field(work, "directory"), NULL,
		on_worktree_ready, g_object_ref(self));
}

/* ================================================================
 * Linked work
 * ================================================================ */

static void
on_link_row_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiWindow *self = user_data;
	const gchar *url = g_object_get_data(G_OBJECT(button), "ai-url");
	g_autoptr(GError) error = NULL;

	if (url != NULL && !ai_gui_work_open_url(url, &error))
		ai_gui_window_toast(self, "%s", error->message);
}

void
ai_gui_window_attach_files(
	AiGuiWindow        *self,
	const gchar *const *paths
){
	gsize i;

	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	if (paths == NULL)
		return;

	for (i = 0; paths[i] != NULL; i++)
	{
		g_autoptr(GFile) file = g_file_new_for_commandline_arg(paths[i]);

		ai_gui_composer_attach_file(AI_GUI_COMPOSER(self->composer), file);
	}

	ai_gui_composer_focus(AI_GUI_COMPOSER(self->composer));
}

void
ai_gui_window_refresh_links(AiGuiWindow *self)
{
	AiWorkSession *work;
	GtkWidget *child;
	g_auto(GStrv) links = NULL;
	guint i;

	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	while ((child = gtk_widget_get_first_child(self->links_list)) != NULL)
		gtk_box_remove(GTK_BOX(self->links_list), child);

	work = self->session != NULL ? ai_gui_session_get_work(self->session)
	                             : NULL;

	if (work == NULL)
	{
		gtk_widget_set_visible(self->links_button, FALSE);
		return;
	}

	links = ai_work_session_dup_links(work);
	gtk_widget_set_visible(self->links_button, links[0] != NULL);

	for (i = 0; links[i] != NULL; i++)
	{
		const gchar *state = ai_work_session_get_link_state(work, links[i]);
		const gchar *title = ai_work_session_get_link_title(work, links[i]);
		g_autofree gchar *number = g_path_get_basename(links[i]);
		g_autofree gchar *label = NULL;
		GtkWidget *button;

		/*
		 * Number, cached state, then title -- the order ai-tui uses, so
		 * the same link reads the same way in both. The state is cached
		 * and says so by being beside the number rather than replacing
		 * it: a stale "open" must not look like a fresh fetch.
		 */
		label = g_strdup_printf("#%s%s%s\n%s", number,
			state != NULL && *state != '\0' ? "  ·  " : "",
			state != NULL ? state : "",
			title != NULL && *title != '\0' ? title : links[i]);

		button = gtk_button_new_with_label(label);
		gtk_widget_add_css_class(button, "flat");
		gtk_button_set_can_shrink(GTK_BUTTON(button), TRUE);
		gtk_widget_set_tooltip_text(button, links[i]);
		g_object_set_data_full(G_OBJECT(button), "ai-url",
		                       g_strdup(links[i]), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(on_link_row_clicked),
		                 self);

		{
			GtkWidget *content = gtk_button_get_child(GTK_BUTTON(button));

			if (GTK_IS_LABEL(content))
			{
				gtk_label_set_xalign(GTK_LABEL(content), 0.0f);
				gtk_label_set_ellipsize(GTK_LABEL(content),
				                        PANGO_ELLIPSIZE_END);
			}
		}

		gtk_box_append(GTK_BOX(self->links_list), button);
	}
}

/* ================================================================
 * Actions
 * ================================================================ */

static void
action_new_session(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	window_new_session(AI_GUI_WINDOW(widget));
}

static void
action_stop(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);

	if (self->session != NULL)
		ai_gui_session_cancel(self->session);
}

static void
action_focus_composer(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	ai_gui_composer_focus(AI_GUI_COMPOSER(AI_GUI_WINDOW(widget)->composer));
}

static void
action_toggle_sidebar(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW(self->split);

	adw_overlay_split_view_set_show_sidebar(split,
		!adw_overlay_split_view_get_show_sidebar(split));
}

static void
action_dashboard(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);

	ai_gui_window_show_dashboard(self, !ai_gui_window_get_dashboard(self));
}

static void
action_cycle_theme(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	const gchar *next = ai_gui_style_cycle_theme();

	ai_gui_settings_set_theme(self->settings, next);
	window_remember_appearance(self);
	ai_gui_window_toast(self, "Theme: %s", next);
}

static void
action_search(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	gboolean showing = gtk_search_bar_get_search_mode(
		GTK_SEARCH_BAR(self->search_bar));

	gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(self->search_bar),
	                               !showing);

	if (!showing)
		gtk_widget_grab_focus(self->search_entry);
}

static void
action_expand_all(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	ai_gui_chat_view_set_expanded_all(
		AI_GUI_CHAT_VIEW(AI_GUI_WINDOW(widget)->chat), TRUE);
}

static void
action_collapse_all(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	ai_gui_chat_view_set_expanded_all(
		AI_GUI_CHAT_VIEW(AI_GUI_WINDOW(widget)->chat), FALSE);
}

static void
action_preferences(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);

	if (self->session != NULL)
		ai_gui_prefs_present(widget, self->session);
}

static void
action_agents(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);

	if (self->session != NULL)
		ai_gui_agents_present(widget, self->session);
}

static void
action_clear(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);

	if (self->session == NULL)
		return;

	ai_gui_session_clear(self->session);
	ai_gui_window_toast(self, "Conversation cleared");
}

static void
action_pin(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);

	if (self->session == NULL)
		return;

	ai_gui_session_set_pinned(self->session,
		!ai_gui_session_get_pinned(self->session));
}

static void
on_rename_response(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiWindow *self = user_data;
	GtkWidget *entry = g_object_get_data(source, "ai-entry");
	const gchar *response;

	response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source),
	                                          result);

	if (g_strcmp0(response, "rename") != 0 || self->session == NULL)
		return;

	ai_gui_session_set_title(self->session,
	                         gtk_editable_get_text(GTK_EDITABLE(entry)));
}

static void
action_rename(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	AdwDialog *dialog;
	GtkWidget *entry;

	if (self->session == NULL)
		return;

	dialog = adw_alert_dialog_new("Rename session", NULL);
	entry = gtk_entry_new();
	gtk_editable_set_text(GTK_EDITABLE(entry),
	                      ai_gui_session_get_title(self->session));
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), entry);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
		"cancel", "Cancel", "rename", "Rename", NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
		"rename", ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "rename");
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
	g_object_set_data(G_OBJECT(dialog), "ai-entry", entry);

	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), widget, NULL,
	                        on_rename_response, self);
}

static void
on_delete_response(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiWindow *self = user_data;
	const gchar *response;
	AiGuiSession *doomed = self->session;

	response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source),
	                                          result);

	if (g_strcmp0(response, "delete") != 0 || doomed == NULL)
		return;

	/*
	 * Cancelled before it is dropped.
	 *
	 * A turn in flight holds a reference of its own, so the session
	 * survives until the callback runs either way -- cancelling makes
	 * that sooner rather than leaving a deleted session still billing.
	 */
	ai_gui_session_cancel(doomed);
	g_object_ref(doomed);
	ai_gui_session_store_remove(self->store, doomed);
	window_set_session(self, ai_gui_session_store_get(self->store, 0));
	g_object_unref(doomed);
}

static void
action_delete(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	AdwDialog *dialog;

	if (self->session == NULL)
		return;

	dialog = adw_alert_dialog_new("Delete this session?",
		"The transcript on disk goes with it. A provider's own session "
		"record is not touched.");
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
		"cancel", "Cancel", "delete", "Delete", NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
		"delete", ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

	adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), widget, NULL,
	                        on_delete_response, self);
}

/* The extension picks the format, so there is one control rather than
 * two that can disagree about what is being written. */
static AiExportFormat
format_for_path(const gchar *path)
{
	if (g_str_has_suffix(path, ".md") || g_str_has_suffix(path, ".markdown"))
		return AI_EXPORT_FORMAT_MARKDOWN;

	if (g_str_has_suffix(path, ".org"))
		return AI_EXPORT_FORMAT_ORG;

	return AI_EXPORT_FORMAT_TEXT;
}

static void
on_export_chosen(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiWindow *self = user_data;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;

	file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result,
	                                   &error);

	if (file == NULL)
	{
		g_debug("ai-gui: export dismissed: %s",
		        error != NULL ? error->message : "no file");
		return;
	}

	path = g_file_get_path(file);

	if (path == NULL || self->session == NULL)
		return;

	text = ai_gui_session_export(self->session, format_for_path(path));

	if (g_file_set_contents(path, text != NULL ? text : "", -1, &error))
		ai_gui_window_toast(self, "Exported to %s", path);
	else
		ai_gui_window_toast(self, "%s",
			error != NULL ? error->message : "could not write the file");
}

static void
action_export(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	g_autoptr(GtkFileDialog) dialog = NULL;
	g_autofree gchar *suggestion = NULL;

	if (self->session == NULL)
		return;

	dialog = gtk_file_dialog_new();
	suggestion = g_strdup_printf("%s.org",
	                             ai_gui_session_get_title(self->session));
	g_strdelimit(suggestion, "/", '-');

	gtk_file_dialog_set_title(dialog, "Export transcript");
	gtk_file_dialog_set_initial_name(dialog, suggestion);
	gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL, on_export_chosen,
	                     self);
}

void
ai_gui_window_export(
	AiGuiWindow *self,
	const gchar *arguments
){
	g_auto(GStrv) parts = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	AiExportFormat format = AI_EXPORT_FORMAT_TEXT;
	const gchar *path = NULL;

	g_return_if_fail(AI_GUI_IS_WINDOW(self));

	if (self->session == NULL)
		return;

	/*
	 * `/export markdown notes.md`, `/export notes.org`, `/save x.txt`.
	 * A leading word that names a format is one; otherwise the whole
	 * argument is the path and the extension decides, which is the rule
	 * the file chooser already follows.
	 */
	parts = g_strsplit(arguments != NULL ? g_strstrip((gchar *)arguments) : "",
	                   " ", 2);

	if (parts[0] != NULL && ai_export_format_from_string(parts[0], &format))
		path = parts[1] != NULL ? g_strstrip(parts[1]) : NULL;
	else if (parts[0] != NULL && *parts[0] != '\0')
		path = arguments;

	if (path == NULL || *path == '\0')
	{
		gtk_widget_activate_action(GTK_WIDGET(self), "win.export", NULL);
		return;
	}

	if (parts[1] == NULL || !*parts[1])
		format = format_for_path(path);

	text = ai_gui_session_export(self->session, format);

	if (g_file_set_contents(path, text != NULL ? text : "", -1, &error))
		ai_gui_window_toast(self, "Exported to %s", path);
	else
		ai_gui_window_toast(self, "%s", error->message);
}

static void
action_copy_transcript(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AiGuiWindow *self = AI_GUI_WINDOW(widget);
	g_autofree gchar *text = NULL;

	if (self->session == NULL)
		return;

	text = ai_gui_session_export(self->session, AI_EXPORT_FORMAT_MARKDOWN);
	gdk_clipboard_set_text(gtk_widget_get_clipboard(widget), text);
	ai_gui_window_toast(self, "Transcript copied");
}

/*
 * One table, so the dialog and docs/gui.org cannot drift apart.
 *
 * A window with this many verbs needs somewhere to list them; a person
 * who has to read the source to learn that Ctrl+T exists will never
 * learn that Ctrl+T exists.
 */
typedef struct
{
	const gchar *group;
	const gchar *keys;
	const gchar *what;
} AiGuiShortcut;

static const AiGuiShortcut SHORTCUTS[] = {
	{ "Conversation", "Enter",            "Send, or queue a follow-up while a turn runs" },
	{ "Conversation", "Shift+Enter",      "New line" },
	{ "Conversation", "Esc",              "Stop the turn in flight" },
	{ "Conversation", "Tab",              "Complete the / or @ at the cursor" },
	{ "Conversation", "Ctrl+Up / Down",   "Walk the prompts you have sent" },
	{ "Conversation", "Ctrl+L",           "Focus the message box" },

	{ "Attachments",  "Ctrl+O",           "Attach files" },
	{ "Attachments",  "Ctrl+V",           "Paste an image from the clipboard" },
	{ "Attachments",  "Ctrl+Shift+V",     "Paste an image even when text is also on the clipboard" },
	{ "Attachments",  "Click a thumbnail", "See it full size, and save a copy" },
	{ "Attachments",  "Click a file name", "Preview the file the transcript is talking about" },

	{ "Session",      "Ctrl+N",           "New session, in the project you are in" },
	{ "Session",      "Ctrl+Shift+O",     "Open a folder as a project" },
	{ "Session",      "F9",               "Show or hide the session list" },
	{ "Session",      "Ctrl+F",           "Find in this conversation" },
	{ "Session",      "Ctrl+Shift+E",     "Export the transcript" },
	{ "Session",      "Right-click a block", "Copy it, copy it as Markdown, or save it" },

	{ "Window",       "Ctrl+backslash",   "Project dashboard, and back" },
	{ "Window",       "Ctrl+Shift+G",     "Background agents" },
	{ "Window",       "Ctrl+T",           "Next theme" },
	{ "Window",       "Ctrl+comma",       "Preferences" },
	{ "Window",       "Ctrl+question",    "This list" }
};

static void
action_shortcuts(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AdwPreferencesDialog *dialog =
		ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
	AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
	AdwPreferencesGroup *group = NULL;
	const gchar *current = NULL;
	gsize i;

	adw_dialog_set_title(ADW_DIALOG(dialog), "Keyboard shortcuts");
	adw_preferences_page_set_title(page, "Shortcuts");

	for (i = 0; i < G_N_ELEMENTS(SHORTCUTS); i++)
	{
		GtkWidget *row;
		GtkWidget *keys;

		if (g_strcmp0(current, SHORTCUTS[i].group) != 0)
		{
			current = SHORTCUTS[i].group;
			group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
			adw_preferences_group_set_title(group, current);
			adw_preferences_page_add(page, group);
		}

		row = adw_action_row_new();
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
		                              SHORTCUTS[i].what);

		keys = gtk_label_new(SHORTCUTS[i].keys);
		gtk_widget_add_css_class(keys, "dim-label");
		gtk_widget_add_css_class(keys, "ai-monospace");
		gtk_widget_set_valign(keys, GTK_ALIGN_CENTER);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), keys);

		adw_preferences_group_add(group, row);
	}

	adw_preferences_dialog_add(dialog, page);
	adw_dialog_present(ADW_DIALOG(dialog), widget);
}

static void
action_about(
	GtkWidget   *widget,
	const gchar *name,
	GVariant    *parameter
){
	AdwDialog *about = adw_about_dialog_new();

	adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "ai-gui");
	adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about),
	                                      "chat-message-new-symbolic");
	adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), AI_GLIB_VERSION_STRING);
	adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "ai-glib");
	adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about),
	                                  GTK_LICENSE_AGPL_3_0);
	adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
		"A desktop front-end over ai-glib's conversation and harness "
		"layers — the same model ai-tui draws in a terminal.");
	adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about),
		"https://gitlab.com/zachpodbielniak/ai-glib");

	adw_dialog_present(about, widget);
}

/* ================================================================
 * Search
 * ================================================================ */

static void
on_search_changed(
	GtkSearchEntry *entry,
	gpointer        user_data
){
	AiGuiWindow *self = user_data;

	ai_gui_chat_view_set_search(AI_GUI_CHAT_VIEW(self->chat),
		gtk_editable_get_text(GTK_EDITABLE(entry)));
}

/* ================================================================
 * Shutdown
 * ================================================================ */

/*
 * A report is a CLI subprocess. A window behind a browser has no
 * business spawning one every minute, so polling follows the window the
 * way ai-tui's follows the visible panel.
 */
static void
on_active_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiWindow *self = AI_GUI_WINDOW(object);

	if (self->quota != NULL)
	{
		ai_gui_quota_set_active(AI_GUI_QUOTA(self->quota),
			gtk_window_is_active(GTK_WINDOW(self)));
	}
}

static gboolean
on_close_request(
	GtkWindow *window,
	gpointer   user_data
){
	AiGuiWindow *self = AI_GUI_WINDOW(window);

	/*
	 * The draft belongs with the session, and the composer is holding
	 * the current one. Detaching first is what makes it write back.
	 */
	ai_gui_composer_set_session(AI_GUI_COMPOSER(self->composer), NULL);

	/* Reporting work is cancelled and drained while the widget tree is
	 * still whole; a drain iterates the main context and a half-torn
	 * window is not somewhere to re-enter. */
	if (self->quota != NULL)
		ai_gui_quota_shutdown(AI_GUI_QUOTA(self->quota));

	ai_gui_session_store_save_all(self->store);

	return FALSE;
}

/* ================================================================
 * Construction
 * ================================================================ */

static GMenuModel *
window_build_menu(void)
{
	GMenu *menu = g_menu_new();
	GMenu *session = g_menu_new();
	GMenu *view = g_menu_new();
	GMenu *app = g_menu_new();

	g_menu_append(session, "New session", "win.new-session");
	g_menu_append(session, "Open project…", "win.open-project");
	g_menu_append(session, "Rename…", "win.rename");
	g_menu_append(session, "Pin or unpin", "win.pin");
	g_menu_append(session, "Clear conversation", "win.clear");
	g_menu_append(session, "Export…", "win.export");
	g_menu_append(session, "Copy transcript", "win.copy-transcript");
	g_menu_append(session, "Delete session…", "win.delete");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(session));

	g_menu_append(view, "Project dashboard", "win.dashboard");
	g_menu_append(view, "Next theme", "win.cycle-theme");
	g_menu_append(view, "Find in conversation", "win.search");
	g_menu_append(view, "Expand everything", "win.expand-all");
	g_menu_append(view, "Collapse everything", "win.collapse-all");
	g_menu_append(view, "Background agents…", "win.agents");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

	g_menu_append(app, "Keyboard shortcuts", "win.shortcuts");
	g_menu_append(app, "Preferences", "win.preferences");
	g_menu_append(app, "About ai-gui", "win.about");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

	g_object_unref(session);
	g_object_unref(view);
	g_object_unref(app);

	return G_MENU_MODEL(menu);
}

/* "4 sessions · 2 projects", under the sidebar's own heading. */
static void
window_sync_sidebar_title(AiGuiWindow *self)
{
	g_autofree gchar *summary = NULL;

	if (self->sidebar == NULL || self->sidebar_title == NULL)
		return;

	summary = ai_gui_sidebar_describe(AI_GUI_SIDEBAR(self->sidebar));
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(self->sidebar_title),
	                              summary);
}

static void
on_sidebar_grouping_changed(
	AiGuiSidebar *sidebar,
	gpointer      user_data
){
	window_sync_sidebar_title(user_data);
}

static GtkWidget *
window_build_sidebar(AiGuiWindow *self)
{
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	GtkWidget *new_button =
		gtk_button_new_from_icon_name("tab-new-symbolic");
	GtkWidget *project_button =
		gtk_button_new_from_icon_name("folder-open-symbolic");

	self->sidebar_title = adw_window_title_new("Sessions", NULL);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
	                                self->sidebar_title);
	gtk_widget_set_tooltip_text(new_button,
		"New session in this project (Ctrl+N)");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(new_button),
	                               "win.new-session");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), new_button);

	gtk_widget_set_tooltip_text(project_button,
		"Open another folder as a project (Ctrl+Shift+O)");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(project_button),
	                               "win.open-project");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), project_button);

	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

	self->sidebar = ai_gui_sidebar_new(self->store);
	g_signal_connect(self->sidebar, "session-selected",
	                 G_CALLBACK(on_session_selected), self);
	g_signal_connect(self->sidebar, "grouping-changed",
	                 G_CALLBACK(on_sidebar_grouping_changed), self);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->sidebar);

	window_sync_sidebar_title(self);

	return toolbar;
}

static GtkWidget *
window_build_content(AiGuiWindow *self)
{
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	GtkWidget *menu_button = gtk_menu_button_new();
	GtkWidget *search_button =
		gtk_button_new_from_icon_name("system-search-symbolic");
	GtkWidget *sidebar_button =
		gtk_button_new_from_icon_name("sidebar-show-symbolic");
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	g_autoptr(GMenuModel) menu = window_build_menu();

	self->title = adw_window_title_new("ai-gui", NULL);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), self->title);

	gtk_widget_set_tooltip_text(sidebar_button, "Show or hide sessions (F9)");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(sidebar_button),
	                               "win.toggle-sidebar");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebar_button);

	gtk_widget_set_tooltip_text(search_button, "Find in conversation (Ctrl+F)");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(search_button), "win.search");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), search_button);

	self->dashboard_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(self->dashboard_button),
	                         "view-grid-symbolic");
	gtk_widget_set_tooltip_text(self->dashboard_button,
		"Project dashboard (Ctrl+backslash)");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(self->dashboard_button),
	                               "win.dashboard");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), self->dashboard_button);

	/*
	 * Linked work lives in the header rather than a side panel.
	 *
	 * ai-tui puts it above usage and todos because it has a side panel
	 * to put it in; a window has a header bar, and a count that is
	 * always visible is what makes somebody notice the link is there.
	 */
	self->links_button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(self->links_button),
	                              "web-browser-symbolic");
	gtk_widget_set_tooltip_text(self->links_button, "Linked issues and PRs");
	gtk_widget_set_visible(self->links_button, FALSE);

	self->links_popover = gtk_popover_new();
	self->links_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_size_request(self->links_list, 360, -1);
	gtk_popover_set_child(GTK_POPOVER(self->links_popover), self->links_list);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->links_button),
	                            self->links_popover);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), self->links_button);

	/*
	 * Account quota, where ai-tui puts it under the provider in its
	 * session panel. The header bar is the window's equivalent of that
	 * always-visible strip, and the indicator hides itself when the
	 * provider has no account report to give.
	 */
	self->quota = ai_gui_quota_new();
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), self->quota);

	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
	                              "open-menu-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), menu);
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

	self->search_bar = gtk_search_bar_new();
	self->search_entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->search_entry),
	                                      "Find in this conversation");
	gtk_widget_set_hexpand(self->search_entry, TRUE);
	gtk_search_bar_set_child(GTK_SEARCH_BAR(self->search_bar),
	                         self->search_entry);
	gtk_search_bar_connect_entry(GTK_SEARCH_BAR(self->search_bar),
	                             GTK_EDITABLE(self->search_entry));
	g_signal_connect(self->search_entry, "search-changed",
	                 G_CALLBACK(on_search_changed), self);
	gtk_box_append(GTK_BOX(content), self->search_bar);

	self->banner = adw_banner_new("");
	gtk_box_append(GTK_BOX(content), self->banner);

	self->chat = ai_gui_chat_view_new();
	gtk_widget_set_vexpand(self->chat, TRUE);
	gtk_box_append(GTK_BOX(content), self->chat);

	self->composer = ai_gui_composer_new();
	gtk_widget_set_margin_start(self->composer, 12);
	gtk_widget_set_margin_end(self->composer, 12);
	gtk_widget_set_margin_bottom(self->composer, 12);
	g_signal_connect(self->composer, "submit",
	                 G_CALLBACK(on_composer_submit), self);
	g_signal_connect(self->composer, "stop",
	                 G_CALLBACK(on_composer_stop), self);
	g_signal_connect(self->composer, "notice",
	                 G_CALLBACK(on_composer_notice), self);
	gtk_box_append(GTK_BOX(content), self->composer);

	self->stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(self->stack),
	                              GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(self->stack), content, "chat");

	self->dashboard = ai_gui_dashboard_new();
	g_signal_connect(self->dashboard, "session-activated",
	                 G_CALLBACK(on_dashboard_activated), self);
	g_signal_connect(self->dashboard, "resume-requested",
	                 G_CALLBACK(on_dashboard_resume), self);
	g_signal_connect(self->dashboard, "worktree-requested",
	                 G_CALLBACK(on_dashboard_worktree), self);
	g_signal_connect(self->dashboard, "project-requested",
	                 G_CALLBACK(on_dashboard_project), self);
	g_signal_connect(self->dashboard, "closed",
	                 G_CALLBACK(on_dashboard_closed), self);
	gtk_stack_add_named(GTK_STACK(self->stack), self->dashboard, "dashboard");

	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->stack);

	return toolbar;
}

AiGuiWindow *
ai_gui_window_new(
	AdwApplication *app,
	AiGuiOptions   *options
){
	AiGuiWindow *self;
	guint restored;

	g_return_val_if_fail(ADW_IS_APPLICATION(app), NULL);
	g_return_val_if_fail(options != NULL, NULL);

	self = g_object_new(AI_GUI_TYPE_WINDOW, "application", app, NULL);
	self->options = ai_gui_options_copy(options);
	self->settings = ai_gui_settings_load(NULL);
	ai_gui_style_add_changed(on_appearance_changed, self);
	self->store = ai_gui_session_store_new(NULL);

	self->split = adw_overlay_split_view_new();
	adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(self->split),
	                                   window_build_sidebar(self));
	adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(self->split),
	                                   window_build_content(self));
	adw_overlay_split_view_set_sidebar_width_fraction(
		ADW_OVERLAY_SPLIT_VIEW(self->split), 0.26);
	adw_overlay_split_view_set_max_sidebar_width(
		ADW_OVERLAY_SPLIT_VIEW(self->split), 380.0);

	self->toasts = adw_toast_overlay_new();
	adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toasts), self->split);
	adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
	                                   self->toasts);

	gtk_window_set_default_size(GTK_WINDOW(self), 1180, 820);
	gtk_window_set_title(GTK_WINDOW(self), "ai-gui");

	/*
	 * A breakpoint rather than a hard-coded layout: on a phone-sized
	 * window the session list becomes an overlay, which is the whole
	 * reason AdwOverlaySplitView is the container here.
	 */
	{
		AdwBreakpoint *breakpoint = adw_breakpoint_new(
			adw_breakpoint_condition_parse("max-width: 720px"));

		adw_breakpoint_add_setters(breakpoint, G_OBJECT(self->split),
		                           "collapsed", TRUE, NULL);
		adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(self),
		                                      breakpoint);
	}

	g_signal_connect(self, "close-request", G_CALLBACK(on_close_request),
	                 NULL);
	g_signal_connect(self, "notify::is-active", G_CALLBACK(on_active_changed),
	                 NULL);

	ai_gui_dashboard_set_store(AI_GUI_DASHBOARD(self->dashboard), self->store);

	restored = ai_gui_session_store_load(self->store, self->options);

	if (restored > 0)
		window_set_session(self, ai_gui_session_store_get(self->store, 0));
	else
		window_new_session(self);

	/*
	 * The dashboard on an otherwise bare launch, exactly as ai-tui does
	 * it: a prompt, a continuation or an explicit native session all
	 * mean somebody already said which conversation they wanted.
	 */
	if (options->dashboard)
		ai_gui_window_show_dashboard(self, TRUE);

	return self;
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

static void
ai_gui_window_dispose(GObject *object)
{
	AiGuiWindow *self = AI_GUI_WINDOW(object);

	ai_gui_style_remove_changed(on_appearance_changed, self);
	window_disconnect_session(self);
	g_clear_object(&self->session);
	g_clear_object(&self->store);
	g_clear_pointer(&self->settings, ai_gui_settings_free);
	g_clear_pointer(&self->options, ai_gui_options_free);

	G_OBJECT_CLASS(ai_gui_window_parent_class)->dispose(object);
}

static void
ai_gui_window_class_init(AiGuiWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = ai_gui_window_dispose;

	gtk_widget_class_install_action(widget_class, "win.new-session", NULL,
	                                action_new_session);
	gtk_widget_class_install_action(widget_class, "win.open-project", NULL,
	                                action_open_project);
	gtk_widget_class_install_action(widget_class, "win.stop", NULL,
	                                action_stop);
	gtk_widget_class_install_action(widget_class, "win.focus-composer", NULL,
	                                action_focus_composer);
	gtk_widget_class_install_action(widget_class, "win.toggle-sidebar", NULL,
	                                action_toggle_sidebar);
	gtk_widget_class_install_action(widget_class, "win.search", NULL,
	                                action_search);
	gtk_widget_class_install_action(widget_class, "win.dashboard", NULL,
	                                action_dashboard);
	gtk_widget_class_install_action(widget_class, "win.cycle-theme", NULL,
	                                action_cycle_theme);
	gtk_widget_class_install_action(widget_class, "win.expand-all", NULL,
	                                action_expand_all);
	gtk_widget_class_install_action(widget_class, "win.collapse-all", NULL,
	                                action_collapse_all);
	gtk_widget_class_install_action(widget_class, "win.preferences", NULL,
	                                action_preferences);
	gtk_widget_class_install_action(widget_class, "win.agents", NULL,
	                                action_agents);
	gtk_widget_class_install_action(widget_class, "win.clear", NULL,
	                                action_clear);
	gtk_widget_class_install_action(widget_class, "win.pin", NULL,
	                                action_pin);
	gtk_widget_class_install_action(widget_class, "win.rename", NULL,
	                                action_rename);
	gtk_widget_class_install_action(widget_class, "win.delete", NULL,
	                                action_delete);
	gtk_widget_class_install_action(widget_class, "win.export", NULL,
	                                action_export);
	gtk_widget_class_install_action(widget_class, "win.copy-transcript", NULL,
	                                action_copy_transcript);
	gtk_widget_class_install_action(widget_class, "win.about", NULL,
	                                action_about);
	gtk_widget_class_install_action(widget_class, "win.shortcuts", NULL,
	                                action_shortcuts);

	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_n,
		GDK_CONTROL_MASK, "win.new-session", NULL);
	/* Ctrl+Shift+O: plain Ctrl+O attaches files to the message. */
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_o,
		GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.open-project", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_f,
		GDK_CONTROL_MASK, "win.search", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_l,
		GDK_CONTROL_MASK, "win.focus-composer", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_comma,
		GDK_CONTROL_MASK, "win.preferences", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_F9,
		0, "win.toggle-sidebar", NULL);
	/* The keys ai-tui uses, so one habit covers both front-ends. */
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_backslash,
		GDK_CONTROL_MASK, "win.dashboard", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_t,
		GDK_CONTROL_MASK, "win.cycle-theme", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_question,
		GDK_CONTROL_MASK, "win.shortcuts", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_e,
		GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.export", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_g,
		GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.agents", NULL);
}

static void
ai_gui_window_init(AiGuiWindow *self)
{
}
