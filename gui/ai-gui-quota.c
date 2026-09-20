/*
 * ai-gui-quota.c - What is left of the account, in the header bar
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <math.h>

#include "core/ai-quota.h"

#include "ai-gui-quota.h"

/*
 * Asked more often than a query is allowed.
 *
 * ai_quota_refresh() enforces the one-minute minimum itself; this tick
 * only has to be frequent enough that the indicator reappears promptly
 * after a provider switch or after the window comes forward.
 */
#define AI_GUI_QUOTA_TICK_MS (20 * 1000)

/* Three, like the terminal panel. A header popover that grew with the
 * provider's imagination would stop being glanceable. */
#define AI_GUI_QUOTA_MAX_ROWS (3)

struct _AiGuiQuota
{
	GtkWidget parent_instance;

	GtkWidget *button;
	GtkWidget *label;
	GtkWidget *popover;
	GtkWidget *list;

	AiGuiSession *session;
	AiQuota       quota;
	GSource      *tick;
	gboolean      active;
};

G_DEFINE_FINAL_TYPE(AiGuiQuota, ai_gui_quota, GTK_TYPE_WIDGET)

static void quota_rebuild(AiGuiQuota *self);

/* ================================================================
 * The indicator
 * ================================================================ */

static void
quota_sync_button(AiGuiQuota *self)
{
	gdouble lowest = ai_quota_lowest(&self->quota);
	g_autofree gchar *percent = ai_quota_format_percent(lowest);

	/*
	 * Hidden rather than shown empty when there is nothing to report.
	 * A permanently blank indicator beside an HTTP provider, which has no
	 * account report at all, is chrome that teaches somebody to ignore it.
	 */
	if (percent == NULL)
	{
		gtk_widget_set_visible(self->button, FALSE);
		return;
	}

	gtk_widget_set_visible(self->button, TRUE);
	gtk_label_set_text(GTK_LABEL(self->label), percent);

	gtk_widget_remove_css_class(self->button, "ai-quota-low");
	gtk_widget_remove_css_class(self->button, "ai-quota-spent");
	gtk_widget_remove_css_class(self->button, "dim-label");

	if (lowest <= 10)
		gtk_widget_add_css_class(self->button, "ai-quota-spent");
	else if (lowest <= 25)
		gtk_widget_add_css_class(self->button, "ai-quota-low");

	/* A stale figure is still worth showing -- it is the last thing that
	 * was true -- but it must not look freshly fetched. */
	if (self->quota.failed)
		gtk_widget_add_css_class(self->button, "dim-label");

	{
		g_autofree gchar *tip = g_strdup_printf("%s · %s remaining",
			ai_quota_heading(&self->quota), percent);

		gtk_widget_set_tooltip_text(self->button, tip);
	}
}

/* ================================================================
 * The popover
 * ================================================================ */

static GtkWidget *
quota_build_row(JsonObject *row)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
	GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gdouble fraction = ai_quota_fraction(row);
	g_autofree gchar *percent = ai_quota_format_percent(ai_quota_remaining(row));
	const gchar *reset = ai_quota_row_reset(row);
	GtkWidget *label;

	label = gtk_label_new(ai_quota_row_label(row));
	gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
	gtk_widget_set_hexpand(label, TRUE);
	gtk_box_append(GTK_BOX(heading), label);

	label = gtk_label_new(percent != NULL ? percent : "Unavailable");
	gtk_widget_add_css_class(label, "numeric");
	gtk_box_append(GTK_BOX(heading), label);
	gtk_box_append(GTK_BOX(box), heading);

	/*
	 * A bar only where the direction is known. Drawing an empty one for
	 * a row whose percentage nobody could work out would say "you have
	 * none left", which is a different claim from "we do not know".
	 */
	if (fraction >= 0)
	{
		GtkWidget *level = gtk_level_bar_new_for_interval(0.0, 1.0);

		gtk_level_bar_set_value(GTK_LEVEL_BAR(level), fraction);
		gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(level),
		                               GTK_LEVEL_BAR_OFFSET_LOW, 0.25);
		gtk_level_bar_add_offset_value(GTK_LEVEL_BAR(level),
		                               GTK_LEVEL_BAR_OFFSET_HIGH, 1.0);
		gtk_widget_set_hexpand(level, TRUE);
		gtk_box_append(GTK_BOX(box), level);
	}

	if (reset != NULL)
	{
		g_autofree gchar *text = g_strdup_printf("Resets %s", reset);

		label = gtk_label_new(text);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_widget_add_css_class(label, "ai-session-subtitle");
		gtk_box_append(GTK_BOX(box), label);
	}

	return box;
}

static void
quota_rebuild(AiGuiQuota *self)
{
	JsonArray *entries = ai_quota_entries(&self->quota);
	GtkWidget *child;
	guint count = entries != NULL ? json_array_get_length(entries) : 0;
	guint shown;
	guint i;

	quota_sync_button(self);

	while ((child = gtk_widget_get_first_child(self->list)) != NULL)
		gtk_box_remove(GTK_BOX(self->list), child);

	{
		GtkWidget *heading = gtk_label_new(ai_quota_heading(&self->quota));

		gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
		gtk_widget_add_css_class(heading, "heading");
		gtk_box_append(GTK_BOX(self->list), heading);
	}

	if (count == 0)
	{
		GtkWidget *label = gtk_label_new(self->quota.pending
			? "Loading…"
			: "No account report for this provider.");

		gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_box_append(GTK_BOX(self->list), label);
		return;
	}

	shown = MIN(count, (guint)AI_GUI_QUOTA_MAX_ROWS);

	for (i = 0; i < shown; i++)
	{
		JsonObject *row = ai_quota_object(json_array_get_element(entries, i));

		gtk_box_append(GTK_BOX(self->list), quota_build_row(row));
	}

	if (shown < count)
	{
		g_autofree gchar *more = g_strdup_printf(
			"+%u more — ai --usage", count - shown);
		GtkWidget *label = gtk_label_new(more);

		gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
		gtk_widget_add_css_class(label, "dim-label");
		gtk_widget_add_css_class(label, "ai-session-subtitle");
		gtk_box_append(GTK_BOX(self->list), label);
	}
}

/* ================================================================
 * Refreshing
 * ================================================================ */

static void
on_quota_changed(gpointer user_data)
{
	quota_rebuild(user_data);
}

static gboolean
quota_poll(AiGuiQuota *self)
{
	GObject *provider = self->session != NULL
		? ai_gui_session_get_provider(self->session) : NULL;

	if (ai_quota_refresh(&self->quota, provider, self->active))
		quota_rebuild(self);

	return G_SOURCE_CONTINUE;
}

static gboolean
on_tick(gpointer user_data)
{
	return quota_poll(user_data);
}

static void
quota_set_polling(
	AiGuiQuota *self,
	gboolean    polling
){
	if (polling == (self->tick != NULL))
		return;

	if (!polling)
	{
		/* Through the #GSource, never an id: an id means something only
		 * inside the context it was attached to. */
		g_source_destroy(self->tick);
		g_clear_pointer(&self->tick, g_source_unref);
		return;
	}

	self->tick = g_timeout_source_new(AI_GUI_QUOTA_TICK_MS);
	g_source_set_callback(self->tick, on_tick, self, NULL);
	g_source_attach(self->tick, g_main_context_get_thread_default());
}

void
ai_gui_quota_set_active(
	AiGuiQuota *self,
	gboolean    active
){
	g_return_if_fail(AI_GUI_IS_QUOTA(self));

	if (self->active == active)
		return;

	self->active = active;
	quota_set_polling(self, active);

	if (active)
		quota_poll(self);
}

void
ai_gui_quota_set_session(
	AiGuiQuota   *self,
	AiGuiSession *session
){
	g_return_if_fail(AI_GUI_IS_QUOTA(self));

	if (self->session == session)
		return;

	g_set_object(&self->session, session);

	/*
	 * ai_quota_refresh() notices the identity change by itself, but only
	 * when it next runs. Polling now means the indicator does not carry
	 * the previous session's number across the switch.
	 */
	quota_poll(self);
	quota_rebuild(self);
}

void
ai_gui_quota_shutdown(AiGuiQuota *self)
{
	g_return_if_fail(AI_GUI_IS_QUOTA(self));

	quota_set_polling(self, FALSE);
	ai_quota_stop(&self->quota);
	ai_quota_drain(&self->quota);
	ai_quota_clear(&self->quota);
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

GtkWidget *
ai_gui_quota_new(void)
{
	return g_object_new(AI_GUI_TYPE_QUOTA, NULL);
}

static void
ai_gui_quota_dispose(GObject *object)
{
	AiGuiQuota *self = AI_GUI_QUOTA(object);

	/*
	 * Idempotent: the window calls ai_gui_quota_shutdown() on close, and
	 * this covers a widget destroyed without one. Draining here is safe
	 * only because stop() has already refused further callbacks.
	 */
	quota_set_polling(self, FALSE);
	ai_quota_stop(&self->quota);
	ai_quota_drain(&self->quota);
	ai_quota_clear(&self->quota);

	g_clear_object(&self->session);
	g_clear_pointer(&self->button, gtk_widget_unparent);

	G_OBJECT_CLASS(ai_gui_quota_parent_class)->dispose(object);
}

static void
ai_gui_quota_class_init(AiGuiQuotaClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = ai_gui_quota_dispose;

	gtk_widget_class_set_layout_manager_type(widget_class,
	                                         GTK_TYPE_BIN_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "aiquota");
}

static void
ai_gui_quota_init(AiGuiQuota *self)
{
	GtkWidget *content;
	GtkWidget *scroller;

	self->quota.changed = on_quota_changed;
	self->quota.user_data = self;

	content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_append(GTK_BOX(content),
		gtk_image_new_from_icon_name("battery-good-symbolic"));

	self->label = gtk_label_new(NULL);
	gtk_widget_add_css_class(self->label, "numeric");
	gtk_box_append(GTK_BOX(content), self->label);

	self->button = gtk_menu_button_new();
	gtk_menu_button_set_child(GTK_MENU_BUTTON(self->button), content);
	gtk_widget_add_css_class(self->button, "flat");
	gtk_widget_set_visible(self->button, FALSE);
	gtk_widget_set_parent(self->button, GTK_WIDGET(self));

	self->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(self->list, 4);
	gtk_widget_set_margin_end(self->list, 4);
	gtk_widget_set_size_request(self->list, 280, -1);

	scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(scroller), TRUE);
	gtk_scrolled_window_set_max_content_height(
		GTK_SCROLLED_WINDOW(scroller), 420);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), self->list);

	self->popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(self->popover), scroller);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->button), self->popover);

	quota_rebuild(self);
}
