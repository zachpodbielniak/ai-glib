/*
 * ai-gui-sidebar.c - The session list, grouped by project
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-sidebar.h"
#include "ai-gui-work.h"

typedef struct
{
	GtkWidget *title;
	GtkWidget *subtitle;
	GtkWidget *spinner;
	GtkWidget *pin;

	AiGuiSession *session;
	gulong        notify_id;
} AiGuiSessionRow;

struct _AiGuiSidebar
{
	GtkWidget parent_instance;

	GtkWidget *box;
	GtkWidget *search;
	GtkWidget *list;
	GtkWidget *empty;

	AiGuiSessionStore  *store;
	GtkFilterListModel *filtered;
	GtkFilter          *filter;
	GtkSortListModel   *sorted;
	GtkSorter          *sorter;
	GtkSorter          *sections;
	GtkSingleSelection *selection;

	/*
	 * Every session in the store, against the handler that re-sorts when
	 * one of the keys this list is ordered by moves. #GtkSortListModel
	 * re-sorts when its *sorter* changes and never because an item did,
	 * so without this a session that finished a turn -- or landed in a
	 * different project after a /cwd -- would stay where it was until
	 * something else happened to disturb the model.
	 */
	GHashTable *watched;

	gchar    *needle;
	gboolean  selecting;
	gulong    store_id;
};

G_DEFINE_FINAL_TYPE(AiGuiSidebar, ai_gui_sidebar, GTK_TYPE_WIDGET)

enum
{
	SIGNAL_SELECTED,
	SIGNAL_ACTIVATED,
	SIGNAL_GROUPING_CHANGED,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

/* ================================================================
 * Ordering and grouping
 * ================================================================ */

/*
 * Project, then pinned, then most recently touched.
 *
 * The project key comes first because it is what the sections are cut
 * on: #GtkSortListModel slices the *sorted* sequence, so a section
 * sorter that disagreed with the leading key of this one would produce
 * one project under several headings.
 */
static gint
sidebar_order(
	gconstpointer a,
	gconstpointer b,
	gpointer      user_data
){
	AiGuiSession *first = (AiGuiSession *)a;
	AiGuiSession *second = (AiGuiSession *)b;
	gint order;

	order = ai_gui_work_project_compare(
		ai_gui_session_get_project(first),
		ai_gui_session_get_project(second));

	if (order != 0)
		return order;

	if (ai_gui_session_get_pinned(first) !=
	    ai_gui_session_get_pinned(second))
	{
		return ai_gui_session_get_pinned(first) ? -1 : 1;
	}

	if (ai_gui_session_get_updated_at(first) !=
	    ai_gui_session_get_updated_at(second))
	{
		return ai_gui_session_get_updated_at(first)
			> ai_gui_session_get_updated_at(second) ? -1 : 1;
	}

	order = g_strcmp0(ai_gui_session_get_title(first),
	                  ai_gui_session_get_title(second));

	/* Never zero for two different sessions, and two really can share a
	 * title -- an order that calls two rows interchangeable lets them
	 * swap places on every redraw. The id is the one key that cannot
	 * collide. */
	return order != 0 ? order
		: g_strcmp0(ai_gui_session_get_id(first),
		            ai_gui_session_get_id(second));
}

static gint
sidebar_section(
	gconstpointer a,
	gconstpointer b,
	gpointer      user_data
){
	return ai_gui_work_project_compare(
		ai_gui_session_get_project((AiGuiSession *)a),
		ai_gui_session_get_project((AiGuiSession *)b));
}

static void
sidebar_regroup(AiGuiSidebar *self)
{
	/*
	 * Both sorters, because the sections are recut from the section
	 * sorter and the rows from the other one. Telling only the first
	 * leaves a row under the heading it has just left.
	 */
	gtk_sorter_changed(self->sorter, GTK_SORTER_CHANGE_DIFFERENT);
	gtk_sorter_changed(self->sections, GTK_SORTER_CHANGE_DIFFERENT);
	gtk_filter_changed(self->filter, GTK_FILTER_CHANGE_DIFFERENT);

	g_signal_emit(self, signals[SIGNAL_GROUPING_CHANGED], 0);
}

static void
on_watched_notify(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	const gchar *name = g_param_spec_get_name(pspec);

	/*
	 * Only the keys the order is built from. ::busy and ::activity fire
	 * many times a second while a turn streams, and re-sorting the whole
	 * model on each of them would be work nobody can see.
	 */
	if (g_strcmp0(name, "project") == 0 ||
	    g_strcmp0(name, "pinned") == 0 ||
	    g_strcmp0(name, "updated-at") == 0 ||
	    g_strcmp0(name, "title") == 0)
	{
		sidebar_regroup(user_data);
	}
}

static void
sidebar_drop_watch(
	gpointer key,
	gpointer value,
	gpointer user_data
){
	g_signal_handler_disconnect(key, (gulong)GPOINTER_TO_SIZE(value));
}

/*
 * Disconnect before forgetting.
 *
 * The table owns a reference to each session but not its handler, so
 * clearing it alone leaves every handler connected: the next rewatch
 * adds a second one to each surviving session, and a session removed
 * from the store keeps calling into a sidebar that has stopped listing
 * it.
 */
static void
sidebar_unwatch_all(AiGuiSidebar *self)
{
	if (self->watched == NULL)
		return;

	g_hash_table_foreach(self->watched, sidebar_drop_watch, NULL);
	g_hash_table_remove_all(self->watched);
}

/*
 * Reconnect every session in the store.
 *
 * Wholesale rather than a diff against what changed: a window holds a
 * handful of sessions, and bookkeeping that drifts would leave a row
 * frozen in the wrong group -- which looks like the grouping being
 * broken rather than like a missed signal.
 */
static void
sidebar_rewatch(AiGuiSidebar *self)
{
	guint i;
	guint n;

	sidebar_unwatch_all(self);

	if (self->store == NULL)
		return;

	n = g_list_model_get_n_items(G_LIST_MODEL(self->store));

	for (i = 0; i < n; i++)
	{
		AiGuiSession *session =
			ai_gui_session_store_get(self->store, i);
		gulong handler;

		if (session == NULL)
			continue;

		handler = g_signal_connect(session, "notify",
			G_CALLBACK(on_watched_notify), self);
		g_hash_table_insert(self->watched, g_object_ref(session),
		                    GSIZE_TO_POINTER((gsize)handler));
	}
}

static void
on_store_items_changed(
	GListModel *model,
	guint       position,
	guint       removed,
	guint       added,
	gpointer    user_data
){
	AiGuiSidebar *self = user_data;

	sidebar_rewatch(self);

	/*
	 * Deliberately *not* sidebar_regroup().
	 *
	 * The sort and filter models place an inserted item themselves.
	 * Invalidating their sorters from inside the underlying model's
	 * ::items-changed emission re-enters them mid-update, and the list
	 * view draws the result: one session rendered as two rows, which
	 * looks like the store having duplicated it.
	 *
	 * Only the heading above the list needs telling, because its count
	 * is the one thing neither model derives.
	 */
	g_signal_emit(self, signals[SIGNAL_GROUPING_CHANGED], 0);
}

gchar *
ai_gui_sidebar_describe(AiGuiSidebar *self)
{
	g_autoptr(GHashTable) projects = NULL;
	guint sessions;
	guint i;

	g_return_val_if_fail(AI_GUI_IS_SIDEBAR(self), NULL);

	projects = g_hash_table_new(g_str_hash, g_str_equal);
	sessions = self->store != NULL
		? g_list_model_get_n_items(G_LIST_MODEL(self->store)) : 0;

	for (i = 0; i < sessions; i++)
	{
		AiGuiSession *session = ai_gui_session_store_get(self->store, i);

		if (session != NULL)
		{
			g_hash_table_add(projects,
				(gpointer)ai_gui_session_get_project(session));
		}
	}

	if (sessions == 0)
		return g_strdup("No sessions");

	return g_strdup_printf("%u session%s · %u project%s",
		sessions, sessions == 1 ? "" : "s",
		g_hash_table_size(projects),
		g_hash_table_size(projects) == 1 ? "" : "s");
}

/* ================================================================
 * Rows
 * ================================================================ */

static void
row_free(gpointer data)
{
	AiGuiSessionRow *row = data;

	if (row->session != NULL)
		g_clear_signal_handler(&row->notify_id, row->session);

	g_clear_object(&row->session);
	g_free(row);
}

static void
row_sync(AiGuiSessionRow *row)
{
	g_autofree gchar *subtitle = NULL;
	g_autofree gchar *when = NULL;
	gboolean busy;

	if (row->session == NULL)
		return;

	busy = ai_gui_session_get_busy(row->session);
	when = ai_gui_format_relative_time(
		ai_gui_session_get_updated_at(row->session));

	/*
	 * Provider, model and time, in that order.
	 *
	 * The model is the thing somebody is most often checking they are on
	 * the right one of, and a sidebar row is narrow -- so it goes before
	 * the timestamp, which ellipsises away first.
	 */
	subtitle = g_strdup_printf("%s · %s · %s",
		ai_gui_session_get_provider_name(row->session),
		ai_gui_session_get_model(row->session) != NULL
			&& *ai_gui_session_get_model(row->session) != '\0'
			? ai_gui_session_get_model(row->session) : "default",
		when);

	gtk_label_set_text(GTK_LABEL(row->title),
	                   ai_gui_session_get_title(row->session));
	gtk_label_set_text(GTK_LABEL(row->subtitle), subtitle);
	gtk_widget_set_visible(row->spinner, busy);
	gtk_spinner_set_spinning(GTK_SPINNER(row->spinner), busy);
	gtk_widget_set_visible(row->pin,
	                       ai_gui_session_get_pinned(row->session));
}

static void
on_session_notify(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	row_sync(user_data);
}

static void
on_row_setup(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	AiGuiSessionRow *row = g_new0(AiGuiSessionRow, 1);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	gtk_widget_set_margin_start(box, 8);
	gtk_widget_set_margin_end(box, 8);
	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);

	row->title = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(row->title), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(row->title), PANGO_ELLIPSIZE_END);
	gtk_box_append(GTK_BOX(column), row->title);

	row->subtitle = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(row->subtitle), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(row->subtitle), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(row->subtitle, "dim-label");
	gtk_widget_add_css_class(row->subtitle, "ai-session-subtitle");
	gtk_box_append(GTK_BOX(column), row->subtitle);

	gtk_widget_set_hexpand(column, TRUE);
	gtk_box_append(GTK_BOX(box), column);

	row->pin = gtk_image_new_from_icon_name("view-pin-symbolic");
	gtk_widget_set_visible(row->pin, FALSE);
	gtk_box_append(GTK_BOX(box), row->pin);

	row->spinner = gtk_spinner_new();
	gtk_widget_set_visible(row->spinner, FALSE);
	gtk_box_append(GTK_BOX(box), row->spinner);

	g_object_set_data_full(G_OBJECT(box), "ai-session-row", row, row_free);
	gtk_list_item_set_child(GTK_LIST_ITEM(object), box);
}

static void
on_row_bind(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	GtkListItem *item = GTK_LIST_ITEM(object);
	GtkWidget *child = gtk_list_item_get_child(item);
	AiGuiSessionRow *row;
	gpointer session;

	if (child == NULL)
		return;

	row = g_object_get_data(G_OBJECT(child), "ai-session-row");
	session = gtk_list_item_get_item(item);

	if (row == NULL || !AI_GUI_IS_SESSION(session))
		return;

	g_set_object(&row->session, session);

	/*
	 * One handler for every property the row shows.
	 *
	 * ::notify with no detail rather than four detailed connections: the
	 * row redraws two labels and a spinner, and the cost of doing that
	 * for a property it does not show is smaller than the cost of
	 * forgetting to connect a property it does.
	 */
	row->notify_id = g_signal_connect(session, "notify",
		G_CALLBACK(on_session_notify), row);

	row_sync(row);
}

static void
on_row_unbind(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	GtkWidget *child = gtk_list_item_get_child(GTK_LIST_ITEM(object));
	AiGuiSessionRow *row;

	if (child == NULL)
		return;

	row = g_object_get_data(G_OBJECT(child), "ai-session-row");

	if (row == NULL || row->session == NULL)
		return;

	g_clear_signal_handler(&row->notify_id, row->session);
	g_clear_object(&row->session);
}

/* ================================================================
 * Section headers
 * ================================================================ */

static void
on_header_setup(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *name = gtk_label_new(NULL);
	GtkWidget *path = gtk_label_new(NULL);

	gtk_widget_set_margin_start(box, 10);
	gtk_widget_set_margin_end(box, 10);
	gtk_widget_set_margin_top(box, 10);
	gtk_widget_set_margin_bottom(box, 2);

	gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(name, "heading");
	gtk_box_append(GTK_BOX(box), name);

	/* Ellipsised in the middle: two worktrees of one repository differ
	 * at the end of the path, which is the part worth keeping. */
	gtk_label_set_xalign(GTK_LABEL(path), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(path), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_add_css_class(path, "dim-label");
	gtk_widget_add_css_class(path, "ai-session-subtitle");
	gtk_box_append(GTK_BOX(box), path);

	g_object_set_data(G_OBJECT(box), "ai-header-name", name);
	g_object_set_data(G_OBJECT(box), "ai-header-path", path);
	gtk_list_header_set_child(GTK_LIST_HEADER(object), box);
}

static void
on_header_bind(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	GtkListHeader *header = GTK_LIST_HEADER(object);
	GtkWidget *child = gtk_list_header_get_child(header);
	gpointer session = gtk_list_header_get_item(header);
	const gchar *project;
	g_autofree gchar *label = NULL;

	if (child == NULL || !AI_GUI_IS_SESSION(session))
		return;

	project = ai_gui_session_get_project(session);
	label = ai_gui_work_project_label(project);

	gtk_label_set_text(
		GTK_LABEL(g_object_get_data(G_OBJECT(child), "ai-header-name")),
		label);
	gtk_label_set_text(
		GTK_LABEL(g_object_get_data(G_OBJECT(child), "ai-header-path")),
		project);
	gtk_widget_set_tooltip_text(child, project);
}

/* ================================================================
 * Filtering
 * ================================================================ */

static gboolean
sidebar_filter_match(
	gpointer item,
	gpointer user_data
){
	AiGuiSidebar *self = user_data;
	g_autofree gchar *haystack = NULL;
	g_autofree gchar *folded = NULL;

	if (self->needle == NULL || *self->needle == '\0')
		return TRUE;

	if (!AI_GUI_IS_SESSION(item))
		return FALSE;

	{
		const gchar *project = ai_gui_session_get_project(item);
		g_autofree gchar *label = ai_gui_work_project_label(project);

		/* The project label and its path both, so "ai-glib" and
		 * "~/source/projects/ai-glib" are each a way of narrowing the
		 * list to one group. */
		haystack = g_strdup_printf("%s %s %s %s %s",
			ai_gui_session_get_title(item),
			ai_gui_session_get_provider_name(item),
			ai_gui_session_get_model(item) != NULL
				? ai_gui_session_get_model(item) : "",
			label, project);
	}
	folded = g_utf8_casefold(haystack, -1);

	return strstr(folded, self->needle) != NULL;
}

static void
on_search_changed(
	GtkSearchEntry *entry,
	gpointer        user_data
){
	AiGuiSidebar *self = user_data;
	const gchar *text = gtk_editable_get_text(GTK_EDITABLE(entry));

	g_clear_pointer(&self->needle, g_free);

	if (text != NULL && *text != '\0')
		self->needle = g_utf8_casefold(text, -1);

	gtk_filter_changed(self->filter, GTK_FILTER_CHANGE_DIFFERENT);
}

void
ai_gui_sidebar_focus_search(AiGuiSidebar *self)
{
	g_return_if_fail(AI_GUI_IS_SIDEBAR(self));
	gtk_widget_grab_focus(self->search);
}

/* ================================================================
 * Selection
 * ================================================================ */

static void
on_selection_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiSidebar *self = user_data;
	gpointer selected;

	if (self->selecting)
		return;

	selected = gtk_single_selection_get_selected_item(self->selection);

	g_signal_emit(self, signals[SIGNAL_SELECTED], 0,
	              AI_GUI_IS_SESSION(selected) ? selected : NULL);
}

static void
on_row_activated(
	GtkListView *list,
	guint        position,
	gpointer     user_data
){
	AiGuiSidebar *self = user_data;
	g_autoptr(AiGuiSession) session =
		g_list_model_get_item(G_LIST_MODEL(self->sorted), position);

	if (session != NULL)
		g_signal_emit(self, signals[SIGNAL_ACTIVATED], 0, session);
}

AiGuiSession *
ai_gui_sidebar_get_selected(AiGuiSidebar *self)
{
	gpointer selected;

	g_return_val_if_fail(AI_GUI_IS_SIDEBAR(self), NULL);

	selected = gtk_single_selection_get_selected_item(self->selection);

	return AI_GUI_IS_SESSION(selected) ? selected : NULL;
}

void
ai_gui_sidebar_select(
	AiGuiSidebar *self,
	AiGuiSession *session
){
	guint i;
	guint n;

	g_return_if_fail(AI_GUI_IS_SIDEBAR(self));

	if (session == NULL)
		return;

	n = g_list_model_get_n_items(G_LIST_MODEL(self->sorted));

	for (i = 0; i < n; i++)
	{
		g_autoptr(AiGuiSession) candidate =
			g_list_model_get_item(G_LIST_MODEL(self->sorted), i);

		if (candidate == session)
		{
			/*
			 * Guarded so selecting from code does not bounce back out as
			 * a selection change the window then acts on again. The
			 * window is already doing whatever asked for this.
			 */
			self->selecting = TRUE;
			gtk_single_selection_set_selected(self->selection, i);
			self->selecting = FALSE;
			return;
		}
	}
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

GtkWidget *
ai_gui_sidebar_new(AiGuiSessionStore *store)
{
	AiGuiSidebar *self;

	g_return_val_if_fail(AI_GUI_IS_SESSION_STORE(store), NULL);

	self = g_object_new(AI_GUI_TYPE_SIDEBAR, NULL);
	self->store = g_object_ref(store);
	self->store_id = g_signal_connect(store, "items-changed",
		G_CALLBACK(on_store_items_changed), self);
	gtk_filter_list_model_set_model(self->filtered, G_LIST_MODEL(store));

	sidebar_rewatch(self);

	return GTK_WIDGET(self);
}

static void
ai_gui_sidebar_dispose(GObject *object)
{
	AiGuiSidebar *self = AI_GUI_SIDEBAR(object);

	sidebar_unwatch_all(self);
	g_clear_pointer(&self->watched, g_hash_table_unref);

	if (self->store != NULL)
		g_clear_signal_handler(&self->store_id, self->store);

	g_clear_object(&self->store);
	g_clear_object(&self->filter);
	g_clear_object(&self->sorter);
	g_clear_object(&self->sections);
	g_clear_pointer(&self->box, gtk_widget_unparent);

	G_OBJECT_CLASS(ai_gui_sidebar_parent_class)->dispose(object);
}

static void
ai_gui_sidebar_finalize(GObject *object)
{
	AiGuiSidebar *self = AI_GUI_SIDEBAR(object);

	g_free(self->needle);

	G_OBJECT_CLASS(ai_gui_sidebar_parent_class)->finalize(object);
}

static void
ai_gui_sidebar_class_init(AiGuiSidebarClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = ai_gui_sidebar_dispose;
	object_class->finalize = ai_gui_sidebar_finalize;

	gtk_widget_class_set_layout_manager_type(widget_class,
	                                         GTK_TYPE_BIN_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "aisidebar");

	/**
	 * AiGuiSidebar::session-selected:
	 * @self: the sidebar
	 * @session: (nullable): what is now selected
	 */
	signals[SIGNAL_SELECTED] = g_signal_new("session-selected",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, AI_GUI_TYPE_SESSION);

	/**
	 * AiGuiSidebar::session-activated:
	 * @self: the sidebar
	 * @session: the row that was double-clicked or activated
	 */
	signals[SIGNAL_ACTIVATED] = g_signal_new("session-activated",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 1, AI_GUI_TYPE_SESSION);

	/**
	 * AiGuiSidebar::grouping-changed:
	 * @self: the sidebar
	 *
	 * The list was re-grouped: a session moved project, or one arrived
	 * or left. The header above the list counts both.
	 */
	signals[SIGNAL_GROUPING_CHANGED] = g_signal_new("grouping-changed",
		G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
		G_TYPE_NONE, 0);
}

static void
ai_gui_sidebar_init(AiGuiSidebar *self)
{
	GtkListItemFactory *factory;
	GtkListItemFactory *headers;
	GtkWidget *scroller;

	self->watched = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                      g_object_unref, NULL);

	self->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_parent(self->box, GTK_WIDGET(self));

	self->search = gtk_search_entry_new();
	gtk_widget_set_margin_start(self->search, 6);
	gtk_widget_set_margin_end(self->search, 6);
	gtk_widget_set_margin_top(self->search, 6);
	gtk_widget_set_margin_bottom(self->search, 6);
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(self->search),
	                                      "Search sessions");
	g_signal_connect(self->search, "search-changed",
	                 G_CALLBACK(on_search_changed), self);
	gtk_box_append(GTK_BOX(self->box), self->search);

	self->filter = GTK_FILTER(gtk_custom_filter_new(sidebar_filter_match,
	                                                self, NULL));
	self->filtered = gtk_filter_list_model_new(NULL,
	                                           g_object_ref(self->filter));

	/*
	 * Filter, then sort, then select. Sorting last is what lets the
	 * sections be cut from what is actually on screen -- a search that
	 * emptied a project would otherwise leave its heading behind.
	 */
	self->sorter = GTK_SORTER(gtk_custom_sorter_new(sidebar_order, NULL,
	                                                NULL));
	self->sections = GTK_SORTER(gtk_custom_sorter_new(sidebar_section, NULL,
	                                                  NULL));
	self->sorted = gtk_sort_list_model_new(G_LIST_MODEL(self->filtered),
	                                       g_object_ref(self->sorter));
	/* transfer none, unlike the constructor just above -- the sidebar
	 * keeps its own reference so it can invalidate the sorter later. */
	gtk_sort_list_model_set_section_sorter(self->sorted, self->sections);

	self->selection = gtk_single_selection_new(
		G_LIST_MODEL(self->sorted));
	gtk_single_selection_set_autoselect(self->selection, FALSE);
	gtk_single_selection_set_can_unselect(self->selection, TRUE);
	g_signal_connect(self->selection, "notify::selected-item",
	                 G_CALLBACK(on_selection_changed), self);

	headers = gtk_signal_list_item_factory_new();
	g_signal_connect(headers, "setup", G_CALLBACK(on_header_setup), self);
	g_signal_connect(headers, "bind", G_CALLBACK(on_header_bind), self);

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(on_row_setup), self);
	g_signal_connect(factory, "bind", G_CALLBACK(on_row_bind), self);
	g_signal_connect(factory, "unbind", G_CALLBACK(on_row_unbind), self);

	self->list = gtk_list_view_new(GTK_SELECTION_MODEL(self->selection),
	                               factory);
	gtk_list_view_set_header_factory(GTK_LIST_VIEW(self->list), headers);
	g_object_unref(headers);
	gtk_widget_add_css_class(self->list, "navigation-sidebar");
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(self->list), FALSE);
	g_signal_connect(self->list, "activate",
	                 G_CALLBACK(on_row_activated), self);

	scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), self->list);
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_box_append(GTK_BOX(self->box), scroller);
}
