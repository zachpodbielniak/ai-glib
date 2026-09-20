/*
 * ai-gui-block-row.c - One transcript block, as a widget
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-gui-block-row.h"
#include "ai-gui-style.h"

struct _AiGuiBlockRow
{
	GtkWidget parent_instance;

	GtkWidget *box;
	GtkWidget *header;
	GtkWidget *icon;
	GtkWidget *kind_label;
	GtkWidget *expand_button;
	GtkWidget *copy_button;
	GtkWidget *body;

	AiViewBlock *block;
	gulong       changed_id;
};

G_DEFINE_FINAL_TYPE(AiGuiBlockRow, ai_gui_block_row, GTK_TYPE_WIDGET)

/* ================================================================
 * Per-kind presentation
 * ================================================================ */

typedef struct
{
	AiViewBlockKind  kind;
	const gchar     *icon;
	const gchar     *label;
	const gchar     *css_class;
	gboolean         collapsible;
} AiGuiBlockStyle;

/*
 * The table is the registration, the same pattern AiToolStyle and
 * AiImageModelInfo use: a block kind the library grows needs one row
 * here, and until it has one it renders under the generic entry rather
 * than not at all. A transcript that omitted a block would be lying
 * about what happened.
 */
static const AiGuiBlockStyle BLOCK_STYLES[] = {
	{ AI_VIEW_BLOCK_TURN,     "avatar-default-symbolic",       "You",      "ai-turn",     FALSE },
	{ AI_VIEW_BLOCK_TEXT,     NULL,                            NULL,       NULL,          FALSE },
	{ AI_VIEW_BLOCK_THINKING, "emblem-synchronizing-symbolic", "Thinking", "ai-thinking", TRUE  },
	{ AI_VIEW_BLOCK_TOOL,     "system-run-symbolic",           "Tools",    "ai-tool",     TRUE  },
	{ AI_VIEW_BLOCK_STATUS,   "dialog-information-symbolic",   NULL,       NULL,          FALSE },
	{ AI_VIEW_BLOCK_TODO,     "checkbox-checked-symbolic",     "Plan",     NULL,          TRUE  },
	{ AI_VIEW_BLOCK_AGENT,    "system-users-symbolic",         "Agents",   NULL,          TRUE  }
};

static const AiGuiBlockStyle *
block_style(AiViewBlockKind kind)
{
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(BLOCK_STYLES); i++)
	{
		if (BLOCK_STYLES[i].kind == kind)
			return &BLOCK_STYLES[i];
	}

	return &BLOCK_STYLES[1];
}

/* ================================================================
 * Rendering
 * ================================================================ */

static void
row_apply_css(
	AiGuiBlockRow         *self,
	const AiGuiBlockStyle *style
){
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(BLOCK_STYLES); i++)
	{
		if (BLOCK_STYLES[i].css_class != NULL)
			gtk_widget_remove_css_class(self->box, BLOCK_STYLES[i].css_class);
	}

	gtk_widget_remove_css_class(self->box, "ai-error");

	if (style->css_class != NULL)
		gtk_widget_add_css_class(self->box, style->css_class);
}

void
ai_gui_block_row_refresh(AiGuiBlockRow *self)
{
	g_autoptr(AiRenderedText) rendered = NULL;
	g_autoptr(PangoAttrList) attributes = NULL;
	const AiGuiBlockStyle *style;
	AiViewBlockKind kind;

	g_return_if_fail(AI_GUI_IS_BLOCK_ROW(self));

	if (self->block == NULL)
	{
		gtk_label_set_text(GTK_LABEL(self->body), "");
		gtk_widget_set_visible(self->header, FALSE);
		return;
	}

	kind = ai_view_block_get_kind(self->block);
	style = block_style(kind);

	row_apply_css(self, style);

	if (kind == AI_VIEW_BLOCK_STATUS &&
	    ai_view_status_block_get_status_kind(
		    AI_VIEW_STATUS_BLOCK(self->block)) == AI_VIEW_STATUS_ERROR)
	{
		gtk_widget_add_css_class(self->box, "ai-error");
	}

	/*
	 * Width 0: no wrapping from the library.
	 *
	 * The terminal passes its column count because it has to break the
	 * lines itself. GTK does not -- a wrapping #GtkLabel reflows on every
	 * resize, and text pre-wrapped to yesterday's width would fight with
	 * that and leave ragged lines behind.
	 */
	rendered = ai_view_block_render(self->block, 0);
	attributes = ai_gui_style_attributes(rendered);

	gtk_label_set_text(GTK_LABEL(self->body),
	                   ai_rendered_text_get_text(rendered));
	gtk_label_set_attributes(GTK_LABEL(self->body), attributes);

	gtk_widget_set_visible(self->header,
	                       style->icon != NULL || style->label != NULL);

	if (style->icon != NULL)
		gtk_image_set_from_icon_name(GTK_IMAGE(self->icon), style->icon);

	gtk_widget_set_visible(self->icon, style->icon != NULL);
	gtk_label_set_text(GTK_LABEL(self->kind_label),
	                   style->label != NULL ? style->label : "");
	gtk_widget_set_visible(self->kind_label, style->label != NULL);

	gtk_widget_set_visible(self->expand_button, style->collapsible);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->expand_button),
	                             ai_view_block_get_expanded(self->block));
	gtk_button_set_icon_name(GTK_BUTTON(self->expand_button),
		ai_view_block_get_expanded(self->block)
			? "pan-down-symbolic" : "pan-end-symbolic");
}

static void
on_block_changed(
	AiViewBlock *block,
	gpointer     user_data
){
	ai_gui_block_row_refresh(user_data);
}

void
ai_gui_block_row_set_block(
	AiGuiBlockRow *self,
	AiViewBlock   *block
){
	g_return_if_fail(AI_GUI_IS_BLOCK_ROW(self));

	if (self->block == block)
		return;

	if (self->block != NULL)
		g_clear_signal_handler(&self->changed_id, self->block);

	g_clear_object(&self->block);

	if (block != NULL)
	{
		self->block = g_object_ref(block);
		self->changed_id = g_signal_connect(block, "changed",
			G_CALLBACK(on_block_changed), self);
	}

	ai_gui_block_row_refresh(self);
}

AiViewBlock *
ai_gui_block_row_get_block(AiGuiBlockRow *self)
{
	g_return_val_if_fail(AI_GUI_IS_BLOCK_ROW(self), NULL);
	return self->block;
}

/* ================================================================
 * Actions
 * ================================================================ */

static void
on_expand_toggled(
	GtkToggleButton *button,
	gpointer         user_data
){
	AiGuiBlockRow *self = user_data;

	if (self->block == NULL)
		return;

	/* Setting it emits ::changed, which refreshes this row. */
	ai_view_block_set_expanded(self->block,
	                           gtk_toggle_button_get_active(button));
}

static void
on_copy_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiBlockRow *self = user_data;
	GdkClipboard *clipboard;
	g_autofree gchar *text = NULL;

	if (self->block == NULL)
		return;

	/*
	 * Expanded, whatever the row is showing.
	 *
	 * Copying a collapsed tool group would put "Ran 3 commands" on the
	 * clipboard, which is the one thing nobody wants to paste.
	 */
	{
		g_autoptr(AiRenderedText) rendered =
			ai_view_block_render_expanded(self->block, 0);

		text = g_strdup(ai_rendered_text_get_text(rendered));
	}

	clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
	gdk_clipboard_set_text(clipboard, text);
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

GtkWidget *
ai_gui_block_row_new(void)
{
	return g_object_new(AI_GUI_TYPE_BLOCK_ROW, NULL);
}

static void
ai_gui_block_row_dispose(GObject *object)
{
	AiGuiBlockRow *self = AI_GUI_BLOCK_ROW(object);

	if (self->block != NULL)
		g_clear_signal_handler(&self->changed_id, self->block);

	g_clear_object(&self->block);
	g_clear_pointer(&self->box, gtk_widget_unparent);

	G_OBJECT_CLASS(ai_gui_block_row_parent_class)->dispose(object);
}

static void
ai_gui_block_row_class_init(AiGuiBlockRowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = ai_gui_block_row_dispose;

	gtk_widget_class_set_layout_manager_type(widget_class,
	                                         GTK_TYPE_BIN_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "aiblockrow");
}

static void
ai_gui_block_row_init(AiGuiBlockRow *self)
{
	self->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(self->box, "ai-block");
	gtk_widget_set_parent(self->box, GTK_WIDGET(self));

	self->header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	self->icon = gtk_image_new();
	gtk_box_append(GTK_BOX(self->header), self->icon);

	self->kind_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(self->kind_label, "dim-label");
	gtk_label_set_xalign(GTK_LABEL(self->kind_label), 0.0f);
	gtk_box_append(GTK_BOX(self->header), self->kind_label);

	{
		GtkWidget *spacer = gtk_label_new(NULL);

		gtk_widget_set_hexpand(spacer, TRUE);
		gtk_box_append(GTK_BOX(self->header), spacer);
	}

	self->expand_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(self->expand_button),
	                         "pan-end-symbolic");
	gtk_widget_add_css_class(self->expand_button, "flat");
	gtk_widget_set_tooltip_text(self->expand_button, "Expand or collapse");
	g_signal_connect(self->expand_button, "toggled",
	                 G_CALLBACK(on_expand_toggled), self);
	gtk_box_append(GTK_BOX(self->header), self->expand_button);

	self->copy_button = gtk_button_new_from_icon_name("edit-copy-symbolic");
	gtk_widget_add_css_class(self->copy_button, "flat");
	gtk_widget_set_tooltip_text(self->copy_button, "Copy this block");
	g_signal_connect(self->copy_button, "clicked",
	                 G_CALLBACK(on_copy_clicked), self);
	gtk_box_append(GTK_BOX(self->header), self->copy_button);

	gtk_box_append(GTK_BOX(self->box), self->header);

	self->body = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(self->body), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(self->body), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_selectable(GTK_LABEL(self->body), TRUE);
	gtk_label_set_xalign(GTK_LABEL(self->body), 0.0f);
	gtk_widget_set_halign(self->body, GTK_ALIGN_FILL);
	gtk_widget_add_css_class(self->body, "ai-block-body");
	gtk_box_append(GTK_BOX(self->box), self->body);
}
