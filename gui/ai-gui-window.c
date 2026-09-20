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
#include "ai-gui-composer.h"
#include "ai-gui-prefs.h"
#include "ai-gui-session-store.h"
#include "ai-gui-sidebar.h"
#include "ai-gui-style.h"

struct _AiGuiWindow
{
	AdwApplicationWindow parent_instance;

	AiGuiOptions      *options;
	AiGuiSessionStore *store;
	AiGuiSession      *session;

	GtkWidget *toasts;
	GtkWidget *split;
	GtkWidget *sidebar;
	GtkWidget *chat;
	GtkWidget *composer;
	GtkWidget *title;
	GtkWidget *search_bar;
	GtkWidget *search_entry;
	GtkWidget *banner;

	gulong turn_id;
	gulong approval_id;
	gulong agent_id;
	gulong busy_id;
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

	window_update_subtitle(self);
}

static AiGuiSession *
window_new_session(AiGuiWindow *self)
{
	g_autoptr(GError) error = NULL;
	AiGuiSession *session;

	session = ai_gui_session_new(self->options, self->options->provider,
	                             self->options->model, &error);

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

	g_list_free_full(images, (GDestroyNotify)ai_image_free);
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
	g_menu_append(session, "Rename…", "win.rename");
	g_menu_append(session, "Pin or unpin", "win.pin");
	g_menu_append(session, "Clear conversation", "win.clear");
	g_menu_append(session, "Export…", "win.export");
	g_menu_append(session, "Copy transcript", "win.copy-transcript");
	g_menu_append(session, "Delete session…", "win.delete");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(session));

	g_menu_append(view, "Find in conversation", "win.search");
	g_menu_append(view, "Expand everything", "win.expand-all");
	g_menu_append(view, "Collapse everything", "win.collapse-all");
	g_menu_append(view, "Background agents…", "win.agents");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

	g_menu_append(app, "Preferences", "win.preferences");
	g_menu_append(app, "About ai-gui", "win.about");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

	g_object_unref(session);
	g_object_unref(view);
	g_object_unref(app);

	return G_MENU_MODEL(menu);
}

static GtkWidget *
window_build_sidebar(AiGuiWindow *self)
{
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();
	GtkWidget *new_button =
		gtk_button_new_from_icon_name("tab-new-symbolic");

	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
		adw_window_title_new("Sessions", NULL));
	gtk_widget_set_tooltip_text(new_button, "New session (Ctrl+N)");
	gtk_actionable_set_action_name(GTK_ACTIONABLE(new_button),
	                               "win.new-session");
	adw_header_bar_pack_start(ADW_HEADER_BAR(header), new_button);

	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

	self->sidebar = ai_gui_sidebar_new(self->store);
	g_signal_connect(self->sidebar, "session-selected",
	                 G_CALLBACK(on_session_selected), self);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->sidebar);

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
	gtk_box_append(GTK_BOX(content), self->composer);

	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);

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

	restored = ai_gui_session_store_load(self->store, self->options);

	if (restored > 0)
		window_set_session(self, ai_gui_session_store_get(self->store, 0));
	else
		window_new_session(self);

	return self;
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

static void
ai_gui_window_dispose(GObject *object)
{
	AiGuiWindow *self = AI_GUI_WINDOW(object);

	window_disconnect_session(self);
	g_clear_object(&self->session);
	g_clear_object(&self->store);
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
	gtk_widget_class_install_action(widget_class, "win.stop", NULL,
	                                action_stop);
	gtk_widget_class_install_action(widget_class, "win.focus-composer", NULL,
	                                action_focus_composer);
	gtk_widget_class_install_action(widget_class, "win.toggle-sidebar", NULL,
	                                action_toggle_sidebar);
	gtk_widget_class_install_action(widget_class, "win.search", NULL,
	                                action_search);
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

	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_n,
		GDK_CONTROL_MASK, "win.new-session", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_f,
		GDK_CONTROL_MASK, "win.search", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_l,
		GDK_CONTROL_MASK, "win.focus-composer", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_comma,
		GDK_CONTROL_MASK, "win.preferences", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_F9,
		0, "win.toggle-sidebar", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_e,
		GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.export", NULL);
	gtk_widget_class_add_binding_action(widget_class, GDK_KEY_g,
		GDK_CONTROL_MASK | GDK_SHIFT_MASK, "win.agents", NULL);
}

static void
ai_gui_window_init(AiGuiWindow *self)
{
}
