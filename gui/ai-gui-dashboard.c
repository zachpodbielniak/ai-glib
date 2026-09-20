/*
 * ai-gui-dashboard.c - Every ai-glib session on this machine, at a glance
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-dashboard.h"
#include "ai-gui-style.h"
#include "ai-gui-work.h"

/* Slower than a conversation redraw on purpose: these are files on disk,
 * and the registry's own heartbeat is three seconds. */
#define AI_GUI_DASHBOARD_INTERVAL_MS 2000

struct _AiGuiDashboard
{
	GtkWidget parent_instance;

	GtkWidget *box;
	GtkWidget *list;
	GtkWidget *empty;
	GtkWidget *scroller;
	GtkWidget *notice;

	AiGuiSessionStore *store;
	GPtrArray         *rows;
	gchar             *registry;
	gchar             *selected_id;
	GSource           *tick;
	GCancellable      *cancellable;
	gboolean           polling;
};

G_DEFINE_FINAL_TYPE(AiGuiDashboard, ai_gui_dashboard, GTK_TYPE_WIDGET)

enum
{
	SIGNAL_ACTIVATED,
	SIGNAL_RESUME,
	SIGNAL_WORKTREE,
	SIGNAL_PROJECT,
	SIGNAL_CLOSED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static void dashboard_rebuild(AiGuiDashboard *self);

/* ================================================================
 * Helpers
 * ================================================================ */

static const gchar *
row_field(
	AiWorkSession *work,
	const gchar   *name
){
	const gchar *value = ai_work_session_get_field(work, name);

	return value != NULL ? value : "";
}

static void
dashboard_notify(
	AiGuiDashboard *self,
	const gchar    *format,
	...
){
	g_autofree gchar *text = NULL;
	va_list args;

	if (format == NULL)
	{
		gtk_widget_set_visible(self->notice, FALSE);
		return;
	}

	va_start(args, format);
	text = g_strdup_vprintf(format, args);
	va_end(args);

	adw_banner_set_title(ADW_BANNER(self->notice), text);
	gtk_widget_set_visible(self->notice, TRUE);
}

/*
 * A row is this window's own when a local session already carries that
 * record. Comparing ids rather than directories matters: two windows can
 * be open on one checkout, and only one of them is this row.
 */
static AiGuiSession *
dashboard_local_session(
	AiGuiDashboard *self,
	AiWorkSession  *work
){
	guint i;
	guint n;

	if (self->store == NULL)
		return NULL;

	n = ai_gui_session_store_get_n_sessions(self->store);

	for (i = 0; i < n; i++)
	{
		AiGuiSession *session = ai_gui_session_store_get(self->store, i);
		AiWorkSession *theirs = ai_gui_session_get_work(session);

		if (theirs != NULL &&
		    g_strcmp0(ai_work_session_get_id(theirs),
		              ai_work_session_get_id(work)) == 0)
		{
			return session;
		}
	}

	return NULL;
}

/* ================================================================
 * Row actions
 * ================================================================ */

static AiWorkSession *
button_row(GtkWidget *button)
{
	return g_object_get_data(G_OBJECT(button), "ai-work");
}

static void
on_focus_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiDashboard *self = user_data;
	g_autoptr(GError) error = NULL;

	if (!ai_gui_work_focus_pane_finish(result, &error) &&
	    !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
	{
		dashboard_notify(self, "%s", error->message);
	}

	g_object_unref(self);
}

static void
dashboard_activate(
	AiGuiDashboard *self,
	AiWorkSession  *work
){
	AiGuiSession *local = dashboard_local_session(self, work);

	dashboard_notify(self, NULL);

	if (local != NULL)
	{
		g_signal_emit(self, signals[SIGNAL_ACTIVATED], 0, local);
		return;
	}

	/*
	 * Not ours: it belongs to an ai-tui somewhere. The registry is
	 * shared between the front-ends, so this window can put a terminal
	 * session in front of the person without being one.
	 */
	ai_gui_work_focus_pane_async(work, self->cancellable, on_focus_ready,
	                             g_object_ref(self));
}

static void
on_open_clicked(
	GtkButton *button,
	gpointer   user_data
){
	dashboard_activate(user_data, button_row(GTK_WIDGET(button)));
}

static void
on_resume_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiDashboard *self = user_data;

	dashboard_notify(self, NULL);
	g_signal_emit(self, signals[SIGNAL_RESUME], 0,
	              button_row(GTK_WIDGET(button)));
}

static void
on_worktree_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiDashboard *self = user_data;

	dashboard_notify(self, NULL);
	g_signal_emit(self, signals[SIGNAL_WORKTREE], 0,
	              button_row(GTK_WIDGET(button)));
}

static void
on_link_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiDashboard *self = user_data;
	const gchar *url = g_object_get_data(G_OBJECT(button), "ai-url");
	g_autoptr(GError) error = NULL;

	if (url != NULL && !ai_gui_work_open_url(url, &error))
		dashboard_notify(self, "%s", error->message);
}

/* ================================================================
 * Building the list
 * ================================================================ */

static GtkWidget *
dashboard_status_pill(const gchar *status)
{
	GtkWidget *label = gtk_label_new(status);

	gtk_widget_add_css_class(label, "ai-status");
	gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
	gtk_widget_set_size_request(label, 110, -1);

	/*
	 * One CSS class per state rather than a colour chosen here: the
	 * stylesheet decides what INPUT looks like, exactly as the style
	 * module decides what an AiStyleTag looks like.
	 */
	{
		g_autofree gchar *lowered = g_ascii_strdown(status, -1);
		g_autofree gchar *name = g_strconcat("ai-status-", lowered, NULL);

		gtk_widget_add_css_class(label, name);
	}

	return label;
}

static GtkWidget *
dashboard_build_row(
	AiGuiDashboard *self,
	AiWorkSession  *work
){
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
	GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *title;
	GtkWidget *detail;
	GtkWidget *activity;
	g_auto(GStrv) links = ai_work_session_dup_links(work);
	g_autofree gchar *detail_text = NULL;
	const gchar *task = row_field(work, "title");
	gboolean mine = dashboard_local_session(self, work) != NULL;

	gtk_widget_set_margin_start(outer, 10);
	gtk_widget_set_margin_end(outer, 10);
	gtk_widget_set_margin_top(outer, 8);
	gtk_widget_set_margin_bottom(outer, 8);

	gtk_box_append(GTK_BOX(outer),
	               dashboard_status_pill(row_field(work, "status")));

	title = gtk_label_new(*task != '\0' ? task : "(untitled)");
	gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(title, "heading");
	gtk_box_append(GTK_BOX(column), title);

	{
		g_autofree gchar *base = g_path_get_basename(row_field(work, "directory"));
		const gchar *branch = row_field(work, "branch");
		const gchar *provider = row_field(work, "provider");
		const gchar *model = row_field(work, "model");

		detail_text = g_strdup_printf("%s%s%s%s%s%s%s%s",
			base,
			*branch != '\0' ? " · " : "", branch,
			*provider != '\0' ? " · " : "", provider,
			*model != '\0' ? " · " : "", model,
			mine ? "  (this window)" : "");
	}

	detail = gtk_label_new(detail_text);
	gtk_label_set_xalign(GTK_LABEL(detail), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_add_css_class(detail, "dim-label");
	gtk_widget_add_css_class(detail, "ai-session-subtitle");
	gtk_box_append(GTK_BOX(column), detail);

	activity = gtk_label_new(row_field(work, "activity"));
	gtk_label_set_xalign(GTK_LABEL(activity), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(activity), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(activity, "dim-label");
	gtk_widget_add_css_class(activity, "ai-session-subtitle");
	gtk_box_append(GTK_BOX(column), activity);

	/*
	 * Links as buttons rather than decoration. Opening one is an
	 * explicit action and the reference number is what somebody reads:
	 * the cached remote state sits beside it, never instead of it.
	 */
	if (links[0] != NULL)
	{
		GtkWidget *strip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		guint i;

		for (i = 0; links[i] != NULL; i++)
		{
			const gchar *state = ai_work_session_get_link_state(work, links[i]);
			const gchar *link_title = ai_work_session_get_link_title(work, links[i]);
			g_autofree gchar *base = g_path_get_basename(links[i]);
			g_autofree gchar *label = g_strdup_printf("#%s%s%s", base,
				state != NULL && *state != '\0' ? " · " : "",
				state != NULL ? state : "");
			GtkWidget *button = gtk_button_new_with_label(label);

			gtk_widget_add_css_class(button, "flat");
			gtk_widget_add_css_class(button, "ai-session-subtitle");
			gtk_widget_set_tooltip_text(button,
				link_title != NULL && *link_title != '\0' ? link_title : links[i]);
			g_object_set_data_full(G_OBJECT(button), "ai-url",
			                       g_strdup(links[i]), g_free);
			g_signal_connect(button, "clicked", G_CALLBACK(on_link_clicked),
			                 self);
			gtk_box_append(GTK_BOX(strip), button);
		}

		gtk_box_append(GTK_BOX(column), strip);
	}

	gtk_widget_set_hexpand(column, TRUE);
	gtk_box_append(GTK_BOX(outer), column);

	/* ---- actions ---- */

	{
		GtkWidget *open = gtk_button_new_from_icon_name("go-next-symbolic");
		GtkWidget *worktree = gtk_button_new_from_icon_name("list-add-symbolic");
		GtkWidget *resume = gtk_button_new_from_icon_name("view-refresh-symbolic");

		gtk_widget_add_css_class(open, "flat");
		gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(open, mine
			? "Switch to this conversation"
			: "Focus this session's tmux pane");
		gtk_widget_set_sensitive(open, mine || ai_gui_work_is_live(work));
		g_object_set_data_full(G_OBJECT(open), "ai-work",
		                       g_object_ref(work), g_object_unref);
		g_signal_connect(open, "clicked", G_CALLBACK(on_open_clicked), self);

		gtk_widget_add_css_class(worktree, "flat");
		gtk_widget_set_valign(worktree, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(worktree,
			"New isolated Git worktree from this checkout");
		g_object_set_data_full(G_OBJECT(worktree), "ai-work",
		                       g_object_ref(work), g_object_unref);
		g_signal_connect(worktree, "clicked", G_CALLBACK(on_worktree_clicked),
		                 self);

		gtk_widget_add_css_class(resume, "flat");
		gtk_widget_set_valign(resume, GTK_ALIGN_CENTER);
		gtk_widget_set_tooltip_text(resume,
			"Resume this disconnected session in a new conversation");
		gtk_widget_set_sensitive(resume, ai_gui_work_can_resume(work));
		g_object_set_data_full(G_OBJECT(resume), "ai-work",
		                       g_object_ref(work), g_object_unref);
		g_signal_connect(resume, "clicked", G_CALLBACK(on_resume_clicked),
		                 self);

		gtk_box_append(GTK_BOX(outer), resume);
		gtk_box_append(GTK_BOX(outer), worktree);
		gtk_box_append(GTK_BOX(outer), open);
	}

	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), outer);
	g_object_set_data_full(G_OBJECT(row), "ai-work", g_object_ref(work),
	                       g_object_unref);

	return row;
}

static GtkWidget *
dashboard_build_header(const gchar *project)
{
	GtkWidget *row = gtk_list_box_row_new();
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *name;
	GtkWidget *path;
	/* Shared with the sidebar's own group headings: one project must not
	 * appear under two names in one window. */
	g_autofree gchar *base = ai_gui_work_project_label(project);

	gtk_widget_set_margin_start(box, 10);
	gtk_widget_set_margin_end(box, 10);
	gtk_widget_set_margin_top(box, 10);
	gtk_widget_set_margin_bottom(box, 2);

	name = gtk_label_new(base);
	gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
	gtk_widget_add_css_class(name, "heading");
	gtk_box_append(GTK_BOX(box), name);

	path = gtk_label_new(project);
	gtk_label_set_xalign(GTK_LABEL(path), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_add_css_class(path, "dim-label");
	gtk_widget_add_css_class(path, "ai-session-subtitle");
	gtk_box_append(GTK_BOX(box), path);

	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
	gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);

	return row;
}

static void
dashboard_rebuild(AiGuiDashboard *self)
{
	GtkWidget *child;
	const gchar *project = NULL;
	guint i;

	while ((child = gtk_widget_get_first_child(self->list)) != NULL)
		gtk_list_box_remove(GTK_LIST_BOX(self->list), child);

	gtk_widget_set_visible(self->empty,
	                       self->rows == NULL || self->rows->len == 0);

	if (self->rows == NULL)
		return;

	for (i = 0; i < self->rows->len; i++)
	{
		AiWorkSession *work = g_ptr_array_index(self->rows, i);
		GtkWidget *row;

		/*
		 * A header whenever the project changes. The list is already
		 * sorted by attention first, so a project can appear more than
		 * once -- which is right: "what needs me" outranks "what is it
		 * part of", and repeating the heading is how the grouping stays
		 * readable without reordering the rows.
		 */
		if (g_strcmp0(project, row_field(work, "project")) != 0)
		{
			project = row_field(work, "project");
			gtk_list_box_append(GTK_LIST_BOX(self->list),
			                    dashboard_build_header(project));
		}

		row = dashboard_build_row(self, work);
		gtk_list_box_append(GTK_LIST_BOX(self->list), row);

		if (g_strcmp0(self->selected_id, ai_work_session_get_id(work)) == 0)
			gtk_list_box_select_row(GTK_LIST_BOX(self->list),
			                        GTK_LIST_BOX_ROW(row));
	}
}

void
ai_gui_dashboard_refresh(AiGuiDashboard *self)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) rows = NULL;

	g_return_if_fail(AI_GUI_IS_DASHBOARD(self));

	rows = ai_gui_work_list(self->registry, &error);

	if (rows == NULL)
	{
		dashboard_notify(self, "%s", error != NULL
			? error->message : "Cannot read the session registry.");
		return;
	}

	g_clear_pointer(&self->rows, g_ptr_array_unref);
	self->rows = g_steal_pointer(&rows);
	dashboard_rebuild(self);
}

static gboolean
on_tick(gpointer user_data)
{
	ai_gui_dashboard_refresh(user_data);
	return G_SOURCE_CONTINUE;
}

void
ai_gui_dashboard_set_polling(
	AiGuiDashboard *self,
	gboolean        polling
){
	g_return_if_fail(AI_GUI_IS_DASHBOARD(self));

	if (self->polling == polling)
		return;

	self->polling = polling;

	if (!polling)
	{
		if (self->tick != NULL)
		{
			/* Through the GSource, never an id: an id means something
			 * only inside the context it was attached to. */
			g_source_destroy(self->tick);
			g_clear_pointer(&self->tick, g_source_unref);
		}

		return;
	}

	ai_gui_dashboard_refresh(self);

	if (self->tick == NULL)
	{
		self->tick = g_timeout_source_new(AI_GUI_DASHBOARD_INTERVAL_MS);
		g_source_set_callback(self->tick, on_tick, self, NULL);
		g_source_attach(self->tick, g_main_context_get_thread_default());
	}
}

void
ai_gui_dashboard_set_store(
	AiGuiDashboard    *self,
	AiGuiSessionStore *store
){
	g_return_if_fail(AI_GUI_IS_DASHBOARD(self));

	g_set_object(&self->store, store);
}

void
ai_gui_dashboard_set_registry(
	AiGuiDashboard *self,
	const gchar    *directory
){
	g_return_if_fail(AI_GUI_IS_DASHBOARD(self));

	g_free(self->registry);
	self->registry = g_strdup(directory);
}

/* ================================================================
 * Toolbar
 * ================================================================ */

static void
on_row_activated(
	GtkListBox    *list,
	GtkListBoxRow *row,
	gpointer       user_data
){
	AiWorkSession *work = g_object_get_data(G_OBJECT(row), "ai-work");

	if (work != NULL)
		dashboard_activate(user_data, work);
}

static void
on_row_selected(
	GtkListBox    *list,
	GtkListBoxRow *row,
	gpointer       user_data
){
	AiGuiDashboard *self = user_data;
	AiWorkSession *work = row != NULL
		? g_object_get_data(G_OBJECT(row), "ai-work") : NULL;

	g_clear_pointer(&self->selected_id, g_free);

	if (work != NULL)
		self->selected_id = g_strdup(ai_work_session_get_id(work));
}

static void
on_project_chosen(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiDashboard *self = user_data;
	g_autoptr(GFile) folder = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;

	folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
	                                              result, &error);

	if (folder == NULL)
	{
		g_debug("ai-gui: no project chosen: %s",
		        error != NULL ? error->message : "dismissed");
		g_object_unref(self);
		return;
	}

	path = g_file_get_path(folder);

	if (path != NULL)
		g_signal_emit(self, signals[SIGNAL_PROJECT], 0, path);

	g_object_unref(self);
}

static void
on_open_project(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiDashboard *self = user_data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));

	gtk_file_dialog_set_title(dialog, "Open a project");
	gtk_file_dialog_select_folder(dialog,
		GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
		on_project_chosen, g_object_ref(self));
}

static void
on_close_clicked(
	GtkButton *button,
	gpointer   user_data
){
	g_signal_emit(user_data, signals[SIGNAL_CLOSED], 0);
}

static void
on_refresh_clicked(
	GtkButton *button,
	gpointer   user_data
){
	dashboard_notify(user_data, NULL);
	ai_gui_dashboard_refresh(user_data);
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

GtkWidget *
ai_gui_dashboard_new(void)
{
	return g_object_new(AI_GUI_TYPE_DASHBOARD, NULL);
}

static void
ai_gui_dashboard_dispose(GObject *object)
{
	AiGuiDashboard *self = AI_GUI_DASHBOARD(object);

	ai_gui_dashboard_set_polling(self, FALSE);
	g_cancellable_cancel(self->cancellable);
	g_clear_object(&self->cancellable);
	g_clear_object(&self->store);
	g_clear_pointer(&self->box, gtk_widget_unparent);

	G_OBJECT_CLASS(ai_gui_dashboard_parent_class)->dispose(object);
}

static void
ai_gui_dashboard_finalize(GObject *object)
{
	AiGuiDashboard *self = AI_GUI_DASHBOARD(object);

	g_clear_pointer(&self->rows, g_ptr_array_unref);
	g_free(self->registry);
	g_free(self->selected_id);

	G_OBJECT_CLASS(ai_gui_dashboard_parent_class)->finalize(object);
}

static void
ai_gui_dashboard_class_init(AiGuiDashboardClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = ai_gui_dashboard_dispose;
	object_class->finalize = ai_gui_dashboard_finalize;

	gtk_widget_class_set_layout_manager_type(widget_class,
	                                         GTK_TYPE_BIN_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "aidashboard");

	/**
	 * AiGuiDashboard::session-activated:
	 * @self: the dashboard
	 * @session: one of this window's own sessions
	 */
	signals[SIGNAL_ACTIVATED] = g_signal_new("session-activated",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, AI_GUI_TYPE_SESSION);

	/**
	 * AiGuiDashboard::resume-requested:
	 * @self: the dashboard
	 * @work: a disconnected record naming a native provider session
	 */
	signals[SIGNAL_RESUME] = g_signal_new("resume-requested",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, AI_TYPE_WORK_SESSION);

	/**
	 * AiGuiDashboard::worktree-requested:
	 * @self: the dashboard
	 * @work: the record whose checkout to branch from
	 */
	signals[SIGNAL_WORKTREE] = g_signal_new("worktree-requested",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, AI_TYPE_WORK_SESSION);

	/**
	 * AiGuiDashboard::project-requested:
	 * @self: the dashboard
	 * @directory: where to open a new conversation
	 */
	signals[SIGNAL_PROJECT] = g_signal_new("project-requested",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, G_TYPE_STRING);

	/**
	 * AiGuiDashboard::closed:
	 * @self: the dashboard
	 */
	signals[SIGNAL_CLOSED] = g_signal_new("closed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
ai_gui_dashboard_init(AiGuiDashboard *self)
{
	GtkWidget *toolbar;
	GtkWidget *overlay;

	self->cancellable = g_cancellable_new();

	self->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_parent(self->box, GTK_WIDGET(self));

	toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(toolbar, 12);
	gtk_widget_set_margin_end(toolbar, 12);
	gtk_widget_set_margin_top(toolbar, 8);
	gtk_widget_set_margin_bottom(toolbar, 8);

	{
		GtkWidget *heading = gtk_label_new("Sessions on this machine");

		gtk_widget_add_css_class(heading, "title-4");
		gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
		gtk_widget_set_hexpand(heading, TRUE);
		gtk_box_append(GTK_BOX(toolbar), heading);
	}

	{
		GtkWidget *project = gtk_button_new_with_label("Open project…");
		GtkWidget *refresh =
			gtk_button_new_from_icon_name("view-refresh-symbolic");
		GtkWidget *close =
			gtk_button_new_from_icon_name("go-previous-symbolic");

		gtk_widget_set_tooltip_text(project,
			"Start a conversation in another directory");
		g_signal_connect(project, "clicked", G_CALLBACK(on_open_project), self);
		gtk_box_append(GTK_BOX(toolbar), project);

		gtk_widget_add_css_class(refresh, "flat");
		gtk_widget_set_tooltip_text(refresh, "Re-read the registry now");
		g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_clicked),
		                 self);
		gtk_box_append(GTK_BOX(toolbar), refresh);

		gtk_widget_add_css_class(close, "flat");
		gtk_widget_set_tooltip_text(close,
			"Back to the conversation (Ctrl+backslash)");
		g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), self);
		gtk_box_append(GTK_BOX(toolbar), close);
	}

	gtk_box_append(GTK_BOX(self->box), toolbar);

	self->notice = adw_banner_new("");
	gtk_widget_set_visible(self->notice, FALSE);
	gtk_box_append(GTK_BOX(self->box), self->notice);

	self->list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->list),
	                                GTK_SELECTION_SINGLE);
	gtk_widget_add_css_class(self->list, "navigation-sidebar");
	g_signal_connect(self->list, "row-activated",
	                 G_CALLBACK(on_row_activated), self);
	g_signal_connect(self->list, "row-selected",
	                 G_CALLBACK(on_row_selected), self);

	self->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller),
	                              self->list);
	gtk_widget_set_vexpand(self->scroller, TRUE);

	overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), self->scroller);

	self->empty = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty),
	                              "view-grid-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(self->empty), "No sessions yet");
	adw_status_page_set_description(ADW_STATUS_PAGE(self->empty),
		"Every ai-gui window and every ai-tui on this machine registers "
		"here. Open a project to start one.");
	gtk_widget_set_can_target(self->empty, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->empty);

	gtk_box_append(GTK_BOX(self->box), overlay);
}
