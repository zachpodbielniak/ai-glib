/*
 * ai-gui-chat.c - The transcript, on screen
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-block-row.h"
#include "ai-gui-chat.h"
#include "ai-gui-style.h"

struct _AiGuiChatView
{
	GtkWidget parent_instance;

	GtkWidget *box;
	GtkWidget *scroller;
	GtkWidget *list;
	GtkWidget *status;
	GtkWidget *spinner;
	GtkWidget *activity_label;
	GtkWidget *queue_label;
	GtkWidget *bottom_button;
	GtkWidget *empty;

	GtkFilterListModel *filtered;
	GtkFilter          *filter;
	GtkSelectionModel  *selection;

	AiGuiSession *session;
	gchar        *needle;
	GSource      *tick;

	gulong items_id;
	gulong busy_id;
	gulong activity_id;
	gulong queued_id;

	gboolean follow;
};

G_DEFINE_FINAL_TYPE(AiGuiChatView, ai_gui_chat_view, GTK_TYPE_WIDGET)

static void chat_sync_status(AiGuiChatView *self);

/* ================================================================
 * Filtering
 * ================================================================ */

static gboolean
chat_filter_match(
	gpointer item,
	gpointer user_data
){
	AiGuiChatView *self = user_data;
	g_autofree gchar *text = NULL;
	g_autofree gchar *folded = NULL;

	if (self->needle == NULL || *self->needle == '\0')
		return TRUE;

	if (!AI_IS_VIEW_BLOCK(item))
		return FALSE;

	/*
	 * Expanded, so a search finds what a collapsed tool group is hiding.
	 * Somebody looking for a file name they know was edited should not
	 * have to open every group first.
	 */
	{
		g_autoptr(AiRenderedText) rendered =
			ai_view_block_render_expanded(AI_VIEW_BLOCK(item), 0);

		text = g_strdup(ai_rendered_text_get_text(rendered));
	}

	folded = g_utf8_casefold(text != NULL ? text : "", -1);

	return strstr(folded, self->needle) != NULL;
}

void
ai_gui_chat_view_set_search(
	AiGuiChatView *self,
	const gchar   *needle
){
	g_return_if_fail(AI_GUI_IS_CHAT_VIEW(self));

	g_clear_pointer(&self->needle, g_free);

	if (needle != NULL && *needle != '\0')
		self->needle = g_utf8_casefold(needle, -1);

	gtk_filter_changed(self->filter, GTK_FILTER_CHANGE_DIFFERENT);
	chat_sync_status(self);
}

/* ================================================================
 * The list
 * ================================================================ */

static void
on_item_setup(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	gtk_list_item_set_child(GTK_LIST_ITEM(object), ai_gui_block_row_new());
	gtk_list_item_set_activatable(GTK_LIST_ITEM(object), FALSE);
}

static void
on_item_bind(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	AiGuiChatView *self = user_data;
	GtkListItem *item = GTK_LIST_ITEM(object);
	GtkWidget *child = gtk_list_item_get_child(item);
	gpointer block = gtk_list_item_get_item(item);

	if (child == NULL || !AI_IS_VIEW_BLOCK(block))
		return;

	/*
	 * Set before the block, so the first render already knows where a
	 * relative tool target points. Afterwards the row would have to be
	 * refreshed a second time to make its targets clickable.
	 */
	ai_gui_block_row_set_directory(AI_GUI_BLOCK_ROW(child),
		self->session != NULL
			? ai_gui_session_get_working_directory(self->session) : NULL);
	ai_gui_block_row_set_block(AI_GUI_BLOCK_ROW(child), block);
}

static void
on_item_unbind(
	GtkSignalListItemFactory *factory,
	GObject                  *object,
	gpointer                  user_data
){
	GtkWidget *child = gtk_list_item_get_child(GTK_LIST_ITEM(object));

	/*
	 * Unbinding matters here.
	 *
	 * A row holds a handler on the block it is showing so a streaming
	 * delta redraws it. A recycled row that kept the old subscription
	 * would redraw itself with somebody else's block every time that
	 * block grew.
	 */
	if (child != NULL)
		ai_gui_block_row_set_block(AI_GUI_BLOCK_ROW(child), NULL);
}

void
ai_gui_chat_view_scroll_to_bottom(AiGuiChatView *self)
{
	GtkAdjustment *adjustment;

	g_return_if_fail(AI_GUI_IS_CHAT_VIEW(self));

	adjustment = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(self->scroller));

	if (adjustment == NULL)
		return;

	gtk_adjustment_set_value(adjustment,
		gtk_adjustment_get_upper(adjustment)
			- gtk_adjustment_get_page_size(adjustment));

	self->follow = TRUE;
	gtk_widget_set_visible(self->bottom_button, FALSE);
}

static void
on_adjustment_changed(
	GtkAdjustment *adjustment,
	gpointer       user_data
){
	AiGuiChatView *self = user_data;
	gdouble value = gtk_adjustment_get_value(adjustment);
	gdouble page = gtk_adjustment_get_page_size(adjustment);
	gdouble upper = gtk_adjustment_get_upper(adjustment);
	gboolean at_bottom = value + page >= upper - 40.0;

	/*
	 * Following is a decision the reader makes by scrolling.
	 *
	 * Scrolling up to read something and being yanked back down by the
	 * next delta is the single most annoying thing a streaming transcript
	 * can do, so the view stops following the moment somebody leaves the
	 * bottom and resumes when they return.
	 */
	self->follow = at_bottom;
	gtk_widget_set_visible(self->bottom_button, !at_bottom);
}

static void
on_value_changed(
	GtkAdjustment *adjustment,
	gpointer       user_data
){
	on_adjustment_changed(adjustment, user_data);
}

static void
on_items_changed(
	GListModel *model,
	guint       position,
	guint       removed,
	guint       added,
	gpointer    user_data
){
	AiGuiChatView *self = user_data;

	gtk_widget_set_visible(self->empty,
	                       g_list_model_get_n_items(model) == 0);

	if (self->follow)
		ai_gui_chat_view_scroll_to_bottom(self);
}

void
ai_gui_chat_view_set_expanded_all(
	AiGuiChatView *self,
	gboolean       expanded
){
	AiTranscript *transcript;
	guint i;
	guint n;

	g_return_if_fail(AI_GUI_IS_CHAT_VIEW(self));

	if (self->session == NULL)
		return;

	transcript = ai_gui_session_get_transcript(self->session);
	n = ai_transcript_get_n_blocks(transcript);

	for (i = 0; i < n; i++)
	{
		AiViewBlock *block = ai_transcript_get_block(transcript, i);

		if (block != NULL)
			ai_view_block_set_expanded(block, expanded);
	}
}

/* ================================================================
 * The status strip
 * ================================================================ */

static void
chat_sync_status(AiGuiChatView *self)
{
	gboolean busy = self->session != NULL
		&& ai_gui_session_get_busy(self->session);
	guint queued = self->session != NULL
		? ai_gui_session_get_queued(self->session) : 0;

	gtk_widget_set_visible(self->status, busy || queued > 0);
	gtk_widget_set_visible(self->spinner, busy);
	gtk_spinner_set_spinning(GTK_SPINNER(self->spinner), busy);

	if (busy)
	{
		const gchar *activity = ai_gui_session_get_activity(self->session);
		gint64 elapsed = ai_conversation_get_activity_elapsed(
			ai_gui_session_get_conversation(self->session));
		g_autofree gchar *text = NULL;

		/* Seconds, from the library's microseconds. Showing the raw
		 * number would be six digits of noise per redraw. */
		text = g_strdup_printf("%s — %ld s",
			activity != NULL && *activity != '\0' ? activity : "Working",
			(glong)(elapsed / G_USEC_PER_SEC));

		gtk_label_set_text(GTK_LABEL(self->activity_label), text);
	}
	else
	{
		gtk_label_set_text(GTK_LABEL(self->activity_label), "");
	}

	if (queued > 0)
	{
		g_autofree gchar *text = g_strdup_printf("%u queued", queued);

		gtk_label_set_text(GTK_LABEL(self->queue_label), text);
	}

	gtk_widget_set_visible(self->queue_label, queued > 0);
}

void
ai_gui_chat_view_restyle(AiGuiChatView *self)
{
	GListModel *model;

	g_return_if_fail(AI_GUI_IS_CHAT_VIEW(self));

	model = gtk_filter_list_model_get_model(self->filtered);

	if (model == NULL)
		return;

	/*
	 * Dropping the model and putting it back is what unbinds and
	 * rebinds every row, and rebinding is the only thing that makes a
	 * row ask ai_gui_style_attributes() again. Reaching into the list
	 * view for its rows is not possible -- a #GtkListView keeps them
	 * behind its item manager, not as children.
	 *
	 * The reference is held across the gap: set_model() with %NULL drops
	 * the list's own, and a transcript freed there would take the
	 * conversation with it.
	 */
	g_object_ref(model);
	gtk_filter_list_model_set_model(self->filtered, NULL);
	gtk_filter_list_model_set_model(self->filtered, model);
	g_object_unref(model);

	if (self->follow)
		ai_gui_chat_view_scroll_to_bottom(self);
}

static gboolean
on_tick(gpointer user_data)
{
	AiGuiChatView *self = user_data;

	chat_sync_status(self);

	if (self->session != NULL && ai_gui_session_get_busy(self->session))
		return G_SOURCE_CONTINUE;

	/*
	 * A callback returning G_SOURCE_REMOVE drops the held reference
	 * itself, so teardown does not destroy a source that has already
	 * finished. The project's main-context note says why the source is
	 * held as a pointer at all.
	 */
	g_clear_pointer(&self->tick, g_source_unref);

	return G_SOURCE_REMOVE;
}

static void
chat_start_tick(AiGuiChatView *self)
{
	if (self->tick != NULL)
		return;

	self->tick = g_timeout_source_new(500);
	g_source_set_callback(self->tick, on_tick, self, NULL);
	g_source_attach(self->tick, g_main_context_get_thread_default());
}

static void
chat_stop_tick(AiGuiChatView *self)
{
	if (self->tick == NULL)
		return;

	/*
	 * Destroyed through the #GSource, never through an id.
	 *
	 * A source id means something only inside the context it was
	 * attached to, and every context allocates from 1 -- so an id from a
	 * private context very likely names a different source on the global
	 * default. See the main-context note in AGENTS.md.
	 */
	g_source_destroy(self->tick);
	g_clear_pointer(&self->tick, g_source_unref);
}

static void
on_session_state_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiChatView *self = user_data;

	if (self->session != NULL && ai_gui_session_get_busy(self->session))
		chat_start_tick(self);

	chat_sync_status(self);
}

/* ================================================================
 * Session binding
 * ================================================================ */

void
ai_gui_chat_view_set_session(
	AiGuiChatView *self,
	AiGuiSession  *session
){
	AiTranscript *transcript;

	g_return_if_fail(AI_GUI_IS_CHAT_VIEW(self));

	if (self->session == session)
		return;

	if (self->session != NULL)
	{
		g_clear_signal_handler(&self->busy_id, self->session);
		g_clear_signal_handler(&self->activity_id, self->session);
		g_clear_signal_handler(&self->queued_id, self->session);
		g_clear_signal_handler(&self->items_id,
			ai_gui_session_get_transcript(self->session));
	}

	chat_stop_tick(self);
	g_set_object(&self->session, session);

	if (session == NULL)
	{
		gtk_filter_list_model_set_model(self->filtered, NULL);
		gtk_widget_set_visible(self->empty, TRUE);
		chat_sync_status(self);
		return;
	}

	transcript = ai_gui_session_get_transcript(session);
	gtk_filter_list_model_set_model(self->filtered, G_LIST_MODEL(transcript));

	self->items_id = g_signal_connect(transcript, "items-changed",
		G_CALLBACK(on_items_changed), self);
	self->busy_id = g_signal_connect(session, "notify::busy",
		G_CALLBACK(on_session_state_changed), self);
	self->activity_id = g_signal_connect(session, "notify::activity",
		G_CALLBACK(on_session_state_changed), self);
	self->queued_id = g_signal_connect(session, "notify::queued",
		G_CALLBACK(on_session_state_changed), self);

	gtk_widget_set_visible(self->empty,
		ai_transcript_get_n_blocks(transcript) == 0);

	self->follow = TRUE;
	ai_gui_chat_view_scroll_to_bottom(self);

	if (ai_gui_session_get_busy(session))
		chat_start_tick(self);

	chat_sync_status(self);
}

AiGuiSession *
ai_gui_chat_view_get_session(AiGuiChatView *self)
{
	g_return_val_if_fail(AI_GUI_IS_CHAT_VIEW(self), NULL);
	return self->session;
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

GtkWidget *
ai_gui_chat_view_new(void)
{
	return g_object_new(AI_GUI_TYPE_CHAT_VIEW, NULL);
}

static void
ai_gui_chat_view_dispose(GObject *object)
{
	AiGuiChatView *self = AI_GUI_CHAT_VIEW(object);

	if (self->session != NULL)
	{
		g_clear_signal_handler(&self->busy_id, self->session);
		g_clear_signal_handler(&self->activity_id, self->session);
		g_clear_signal_handler(&self->queued_id, self->session);
		g_clear_signal_handler(&self->items_id,
			ai_gui_session_get_transcript(self->session));
	}

	chat_stop_tick(self);
	g_clear_object(&self->session);
	g_clear_object(&self->filter);
	g_clear_pointer(&self->box, gtk_widget_unparent);

	G_OBJECT_CLASS(ai_gui_chat_view_parent_class)->dispose(object);
}

static void
ai_gui_chat_view_finalize(GObject *object)
{
	AiGuiChatView *self = AI_GUI_CHAT_VIEW(object);

	g_free(self->needle);

	G_OBJECT_CLASS(ai_gui_chat_view_parent_class)->finalize(object);
}

static void
ai_gui_chat_view_class_init(AiGuiChatViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = ai_gui_chat_view_dispose;
	object_class->finalize = ai_gui_chat_view_finalize;

	gtk_widget_class_set_layout_manager_type(widget_class,
	                                         GTK_TYPE_BIN_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "aichatview");
}

static void
ai_gui_chat_view_init(AiGuiChatView *self)
{
	GtkListItemFactory *factory;
	GtkAdjustment *adjustment;
	GtkWidget *overlay;

	self->follow = TRUE;

	self->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_parent(self->box, GTK_WIDGET(self));

	self->filter = GTK_FILTER(gtk_custom_filter_new(chat_filter_match, self,
	                                                NULL));
	self->filtered = gtk_filter_list_model_new(NULL,
	                                           g_object_ref(self->filter));
	self->selection = GTK_SELECTION_MODEL(
		gtk_no_selection_new(G_LIST_MODEL(self->filtered)));

	factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(on_item_setup), self);
	g_signal_connect(factory, "bind", G_CALLBACK(on_item_bind), self);
	g_signal_connect(factory, "unbind", G_CALLBACK(on_item_unbind), self);

	self->list = gtk_list_view_new(self->selection, factory);
	gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(self->list), FALSE);
	gtk_widget_add_css_class(self->list, "ai-transcript");
	gtk_widget_add_css_class(self->list, "navigation-sidebar");
	gtk_widget_set_vexpand(self->list, TRUE);

	self->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller),
	                              self->list);
	gtk_widget_set_vexpand(self->scroller, TRUE);

	adjustment = gtk_scrolled_window_get_vadjustment(
		GTK_SCROLLED_WINDOW(self->scroller));
	g_signal_connect(adjustment, "value-changed",
	                 G_CALLBACK(on_value_changed), self);
	g_signal_connect(adjustment, "changed",
	                 G_CALLBACK(on_adjustment_changed), self);

	overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), self->scroller);

	self->empty = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(self->empty),
	                              "chat-message-new-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(self->empty), "Ask something");
	adw_status_page_set_description(ADW_STATUS_PAGE(self->empty),
		"Type below. @ mentions a file, / runs a command, "
		"and dropped files become attachments.");
	gtk_widget_set_can_target(self->empty, FALSE);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->empty);

	self->bottom_button =
		gtk_button_new_from_icon_name("go-bottom-symbolic");
	gtk_widget_add_css_class(self->bottom_button, "osd");
	gtk_widget_add_css_class(self->bottom_button, "circular");
	gtk_widget_set_halign(self->bottom_button, GTK_ALIGN_END);
	gtk_widget_set_valign(self->bottom_button, GTK_ALIGN_END);
	gtk_widget_set_margin_end(self->bottom_button, 16);
	gtk_widget_set_margin_bottom(self->bottom_button, 16);
	gtk_widget_set_tooltip_text(self->bottom_button, "Jump to the newest");
	gtk_widget_set_visible(self->bottom_button, FALSE);
	g_signal_connect_swapped(self->bottom_button, "clicked",
		G_CALLBACK(ai_gui_chat_view_scroll_to_bottom), self);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->bottom_button);

	gtk_box_append(GTK_BOX(self->box), overlay);

	/* ---- status strip ---- */

	self->status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_widget_add_css_class(self->status, "ai-activity");
	gtk_widget_set_margin_start(self->status, 12);
	gtk_widget_set_margin_end(self->status, 12);
	gtk_widget_set_margin_top(self->status, 4);
	gtk_widget_set_margin_bottom(self->status, 4);
	gtk_widget_set_visible(self->status, FALSE);

	self->spinner = gtk_spinner_new();
	gtk_box_append(GTK_BOX(self->status), self->spinner);

	self->activity_label = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(self->activity_label),
	                        PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(self->activity_label), 0.0f);
	gtk_widget_set_hexpand(self->activity_label, TRUE);
	gtk_box_append(GTK_BOX(self->status), self->activity_label);

	self->queue_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(self->queue_label, "dim-label");
	gtk_widget_set_visible(self->queue_label, FALSE);
	gtk_box_append(GTK_BOX(self->status), self->queue_label);

	gtk_box_append(GTK_BOX(self->box), self->status);
}
