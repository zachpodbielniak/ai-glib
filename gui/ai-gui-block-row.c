/*
 * ai-gui-block-row.c - One transcript block, as a widget
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-block-row.h"
#include "ai-gui-content.h"
#include "ai-gui-preview.h"
#include "ai-gui-style.h"

struct _AiGuiBlockRow
{
	GtkWidget parent_instance;

	GtkWidget *box;
	GtkWidget *header;
	GtkWidget *icon;
	GtkWidget *kind_label;
	GtkWidget *expand_button;
	GtkWidget *code_button;
	GtkWidget *copy_button;
	GtkWidget *body;
	GtkWidget *attachments;
	GtkWidget *calls;
	GtkWidget *menu;

	AiViewBlock *block;
	gchar       *directory;
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

/* ================================================================
 * Clicking what the transcript is talking about
 * ================================================================ */

static gchar *
row_span_at(
	AiGuiBlockRow *self,
	gdouble        x,
	gdouble        y,
	AiStyleTag    *out_tag
){
	g_autoptr(AiRenderedText) rendered = NULL;
	PangoLayout *layout = gtk_label_get_layout(GTK_LABEL(self->body));
	gint layout_x = 0;
	gint layout_y = 0;
	gint index = 0;
	gint trailing = 0;

	if (layout == NULL || self->block == NULL)
		return NULL;

	gtk_label_get_layout_offsets(GTK_LABEL(self->body), &layout_x, &layout_y);

	/*
	 * Both are in the label's own coordinates -- a GTK4 margin sits
	 * outside the allocation, so it must not be added here -- and Pango
	 * wants the difference scaled, because a layout index is in Pango
	 * units while a pointer is in pixels.
	 */
	if (!pango_layout_xy_to_index(layout,
		((gint)x - layout_x) * PANGO_SCALE,
		((gint)y - layout_y) * PANGO_SCALE, &index, &trailing))
	{
		return NULL;
	}

	rendered = ai_view_block_render(self->block, 0);

	return ai_gui_content_span_at(rendered, (guint)index, out_tag);
}

static void
on_body_pressed(
	GtkGestureClick *gesture,
	gint             n_press,
	gdouble          x,
	gdouble          y,
	gpointer         user_data
){
	AiGuiBlockRow *self = user_data;
	g_autofree gchar *span = NULL;
	g_autofree gchar *path = NULL;
	AiStyleTag tag = AI_STYLE_DEFAULT;

	/*
	 * A single click, and only when nothing is selected. The body is a
	 * selectable label, and stealing the end of a drag would make
	 * copying a file name impossible.
	 */
	if (n_press != 1 ||
	    gtk_label_get_selection_bounds(GTK_LABEL(self->body), NULL, NULL))
	{
		return;
	}

	span = row_span_at(self, x, y, &tag);

	if (span == NULL)
		return;

	if (tag == AI_STYLE_LINK)
	{
		g_autoptr(GtkUriLauncher) launcher = NULL;
		GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));

		g_strstrip(span);
		launcher = gtk_uri_launcher_new(span);
		gtk_uri_launcher_launch(launcher,
			GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, NULL, NULL);
		gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
		return;
	}

	path = ai_gui_content_resolve_path(self->directory, span);

	if (path == NULL)
		return;

	ai_gui_preview_present_file(GTK_WIDGET(self), path);
	gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* The pointer is the only affordance a label has: without it nothing
 * says a file name in the middle of a sentence can be opened. */
static void
on_body_motion(
	GtkEventControllerMotion *controller,
	gdouble                   x,
	gdouble                   y,
	gpointer                  user_data
){
	AiGuiBlockRow *self = user_data;
	g_autofree gchar *span = row_span_at(self, x, y, NULL);

	gtk_widget_set_cursor_from_name(self->body,
	                                span != NULL ? "pointer" : "text");
}

/* ================================================================
 * Attachments, drawn where they were sent
 * ================================================================ */

static void
on_thumbnail_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiImageContent *image = g_object_get_data(G_OBJECT(button), "ai-image");

	ai_gui_preview_present_image(GTK_WIDGET(button), image,
		g_object_get_data(G_OBJECT(button), "ai-name"));
}

static void
row_rebuild_attachments(AiGuiBlockRow *self)
{
	GtkWidget *child;
	GList *images;
	GList *iter;
	guint index = 1;

	while ((child = gtk_widget_get_first_child(self->attachments)) != NULL)
		gtk_box_remove(GTK_BOX(self->attachments), child);

	images = self->block != NULL
		? ai_gui_content_get_images(G_OBJECT(self->block)) : NULL;

	gtk_widget_set_visible(self->attachments, images != NULL);

	for (iter = images; iter != NULL; iter = iter->next, index++)
	{
		GtkWidget *thumbnail = ai_gui_preview_thumbnail(iter->data, 120);
		g_autofree gchar *name = g_strdup_printf("attachment-%u.png", index);
		GtkWidget *button;

		if (thumbnail == NULL)
			continue;

		button = gtk_button_new();
		gtk_button_set_child(GTK_BUTTON(button), thumbnail);
		gtk_widget_add_css_class(button, "flat");
		gtk_widget_add_css_class(button, "ai-thumbnail-button");
		gtk_widget_set_tooltip_text(button, "Click to see it full size");
		g_object_set_data_full(G_OBJECT(button), "ai-image",
		                       g_object_ref(iter->data), g_object_unref);
		g_object_set_data_full(G_OBJECT(button), "ai-name",
		                       g_steal_pointer(&name), g_free);
		g_signal_connect(button, "clicked", G_CALLBACK(on_thumbnail_clicked),
		                 NULL);
		gtk_box_append(GTK_BOX(self->attachments), button);
	}
}

/* ================================================================
 * One row per tool call
 * ================================================================ */

static const gchar *
row_call_icon(AiToolCallState state)
{
	switch (state)
	{
		case AI_TOOL_CALL_OK:      return "object-select-symbolic";
		case AI_TOOL_CALL_FAILED:  return "dialog-error-symbolic";
		case AI_TOOL_CALL_DENIED:  return "action-unavailable-symbolic";
		case AI_TOOL_CALL_RUNNING: return "content-loading-symbolic";
		case AI_TOOL_CALL_PENDING:
		default:                   return "media-playback-pause-symbolic";
	}
}

/*
 * A class, not a colour.
 *
 * The stylesheet resolves these to @success_color and friends, which a
 * palette has already redefined -- so a failed call is the theme's red
 * here exactly as it is in the summary line above it, and this file
 * never learns what red is.
 */
static const gchar *
row_call_css(AiToolCallState state)
{
	switch (state)
	{
		case AI_TOOL_CALL_OK:     return "ai-call-ok";
		case AI_TOOL_CALL_FAILED:
		case AI_TOOL_CALL_DENIED: return "ai-call-failed";
		default:                  return "ai-call-pending";
	}
}

static void
on_call_target_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiBlockRow *self = user_data;
	const gchar *target = g_object_get_data(G_OBJECT(button), "ai-target");
	g_autofree gchar *path = ai_gui_content_resolve_path(self->directory,
	                                                     target);

	if (path != NULL)
		ai_gui_preview_present_file(GTK_WIDGET(self), path);
}

/*
 * The per-call detail ai-tui cannot draw.
 *
 * The terminal renders the same calls as styled text, because that is
 * what a terminal has. A window can give each one its own row, its own
 * state, and a target that opens --- so the summary stays a summary and
 * the detail is still one click away rather than one scroll.
 */
static void
row_rebuild_calls(AiGuiBlockRow *self)
{
	AiViewToolBlock *tools;
	GtkWidget *child;
	guint i;
	guint n_calls;

	while ((child = gtk_widget_get_first_child(self->calls)) != NULL)
		gtk_box_remove(GTK_BOX(self->calls), child);

	if (self->block == NULL || !AI_IS_VIEW_TOOL_BLOCK(self->block) ||
	    !ai_view_block_get_expanded(self->block))
	{
		gtk_widget_set_visible(self->calls, FALSE);
		return;
	}

	tools = AI_VIEW_TOOL_BLOCK(self->block);
	n_calls = ai_view_tool_block_get_n_calls(tools);
	gtk_widget_set_visible(self->calls, n_calls > 0);

	for (i = 0; i < n_calls; i++)
	{
		AiToolCall *call = ai_view_tool_block_get_call(tools, i);
		AiToolCallState state;
		const gchar *target;
		const gchar *result;
		GtkWidget *row;
		GtkWidget *line;
		GtkWidget *icon;
		GtkWidget *name;

		if (call == NULL)
			continue;

		state = ai_tool_call_get_state(call);
		target = ai_tool_call_get_target(call);
		result = ai_tool_call_get_result(call);

		row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
		gtk_widget_add_css_class(row, "ai-tool-call");

		line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

		icon = gtk_image_new_from_icon_name(row_call_icon(state));
		gtk_widget_add_css_class(icon, row_call_css(state));
		gtk_box_append(GTK_BOX(line), icon);

		name = gtk_label_new(ai_tool_call_get_name(call));
		gtk_widget_add_css_class(name, "heading");
		gtk_box_append(GTK_BOX(line), name);

		if (target != NULL && *target != '\0')
		{
			g_autofree gchar *path =
				ai_gui_content_resolve_path(self->directory, target);

			if (path != NULL)
			{
				GtkWidget *button = gtk_button_new_with_label(target);

				gtk_widget_add_css_class(button, "flat");
				gtk_widget_add_css_class(button, "ai-monospace");
				gtk_widget_set_tooltip_text(button, path);
				g_object_set_data_full(G_OBJECT(button), "ai-target",
				                       g_strdup(target), g_free);
				g_signal_connect(button, "clicked",
				                 G_CALLBACK(on_call_target_clicked), self);
				gtk_box_append(GTK_BOX(line), button);
			}
			else
			{
				GtkWidget *label = gtk_label_new(target);

				gtk_label_set_ellipsize(GTK_LABEL(label),
				                        PANGO_ELLIPSIZE_MIDDLE);
				gtk_widget_add_css_class(label, "ai-monospace");
				gtk_widget_add_css_class(label, "dim-label");
				gtk_widget_set_tooltip_text(label, target);
				gtk_box_append(GTK_BOX(line), label);
			}
		}

		{
			g_autoptr(GString) meta = g_string_new(NULL);
			guint added = ai_tool_call_get_lines_added(call);
			guint removed = ai_tool_call_get_lines_removed(call);
			gint64 duration = ai_tool_call_get_duration_us(call);

			if (added != 0 || removed != 0)
				g_string_append_printf(meta, "+%u −%u  ", added, removed);

			/* Milliseconds under a second, seconds above: "1403 ms" and
			 * "0.4 s" are both harder to read than the other spelling. */
			if (duration > 0)
			{
				if (duration < G_USEC_PER_SEC)
					g_string_append_printf(meta, "%" G_GINT64_FORMAT " ms",
					                       duration / 1000);
				else
					g_string_append_printf(meta, "%.1f s",
					                       (gdouble)duration / G_USEC_PER_SEC);
			}

			if (meta->len > 0)
			{
				GtkWidget *label = gtk_label_new(meta->str);

				gtk_widget_add_css_class(label, "dim-label");
				gtk_widget_add_css_class(label, "ai-session-subtitle");
				gtk_widget_set_hexpand(label, TRUE);
				gtk_widget_set_halign(label, GTK_ALIGN_END);
				gtk_box_append(GTK_BOX(line), label);
			}
		}

		gtk_box_append(GTK_BOX(row), line);

		if (result != NULL && *result != '\0')
		{
			GtkWidget *label = gtk_label_new(result);

			gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
			gtk_label_set_wrap(GTK_LABEL(label), TRUE);
			gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
			gtk_label_set_selectable(GTK_LABEL(label), TRUE);
			gtk_label_set_lines(GTK_LABEL(label), 12);
			gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
			gtk_widget_add_css_class(label, "ai-monospace");
			gtk_widget_add_css_class(label, "ai-tool-result");

			if (ai_tool_call_get_is_error(call))
				gtk_widget_add_css_class(label, "error");

			gtk_box_append(GTK_BOX(row), label);
		}

		gtk_box_append(GTK_BOX(self->calls), row);
	}
}

/* ================================================================
 * Copying the code out of an answer
 * ================================================================ */

static gchar *
row_source_text(AiViewBlock *block)
{
	if (AI_IS_VIEW_TEXT_BLOCK(block))
		return g_strdup(ai_view_text_block_get_text(AI_VIEW_TEXT_BLOCK(block)));

	return NULL;
}

static void
on_copy_code_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiBlockRow *self = user_data;
	g_autofree gchar *source = row_source_text(self->block);
	g_auto(GStrv) blocks = ai_gui_content_code_blocks(source);
	g_autofree gchar *joined = NULL;

	if (blocks[0] == NULL)
		return;

	/* Every fenced block, in order, separated by a blank line: an answer
	 * that gives three snippets is usually three steps of one thing. */
	joined = g_strjoinv("\n", blocks);
	gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), joined);
}

/* ================================================================
 * The menu on a block
 * ================================================================ */

static gchar *
row_expanded_text(AiViewBlock *block)
{
	g_autoptr(AiRenderedText) rendered = NULL;

	if (block == NULL)
		return NULL;

	/*
	 * Expanded, whatever the row is showing. Copying a collapsed tool
	 * group would put "Ran 3 commands" on the clipboard, which is the
	 * one thing nobody wants to paste.
	 */
	rendered = ai_view_block_render_expanded(block, 0);

	return g_strdup(ai_rendered_text_get_text(rendered));
}

static void
on_row_copy(
	GSimpleAction *action,
	GVariant      *parameter,
	gpointer       user_data
){
	AiGuiBlockRow *self = user_data;
	g_autofree gchar *text = row_expanded_text(self->block);

	if (text != NULL)
		gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), text);
}

static void
on_row_copy_markdown(
	GSimpleAction *action,
	GVariant      *parameter,
	gpointer       user_data
){
	AiGuiBlockRow *self = user_data;
	g_autofree gchar *source = row_source_text(self->block);
	g_autofree gchar *text = NULL;

	/*
	 * An assistant answer is already markdown; everything else is a
	 * rendering, so it goes in a fence. Pasting a tool summary into a
	 * document as prose would reflow a table of file names.
	 */
	if (source != NULL)
	{
		text = g_steal_pointer(&source);
	}
	else
	{
		g_autofree gchar *rendered = row_expanded_text(self->block);

		if (rendered == NULL)
			return;

		text = g_strdup_printf("```\n%s\n```\n", rendered);
	}

	gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)), text);
}

static void
on_row_saved(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autofree gchar *text = user_data;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) error = NULL;

	file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);

	if (file == NULL)
	{
		g_debug("ai-gui: save dismissed: %s",
		        error != NULL ? error->message : "no file");
		return;
	}

	if (!g_file_replace_contents(file, text, strlen(text), NULL, FALSE,
	                             G_FILE_CREATE_NONE, NULL, NULL, &error))
	{
		g_message("ai-gui: could not save that block: %s", error->message);
	}
}

static void
on_row_save(
	GSimpleAction *action,
	GVariant      *parameter,
	gpointer       user_data
){
	AiGuiBlockRow *self = user_data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
	gchar *text = row_expanded_text(self->block);

	if (text == NULL)
		return;

	gtk_file_dialog_set_title(dialog, "Save this block");
	gtk_file_dialog_set_initial_name(dialog, "block.txt");
	gtk_file_dialog_save(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
	                     NULL, on_row_saved, text);
}

static void
on_row_secondary(
	GtkGestureClick *gesture,
	gint             n_press,
	gdouble          x,
	gdouble          y,
	gpointer         user_data
){
	AiGuiBlockRow *self = user_data;
	GdkRectangle where = { (gint)x, (gint)y, 1, 1 };

	gtk_popover_set_pointing_to(GTK_POPOVER(self->menu), &where);
	gtk_popover_popup(GTK_POPOVER(self->menu));
}

static const GActionEntry ROW_ACTIONS[] = {
	{ "copy", on_row_copy, NULL, NULL, NULL, { 0 } },
	{ "copy-markdown", on_row_copy_markdown, NULL, NULL, NULL, { 0 } },
	{ "save", on_row_save, NULL, NULL, NULL, { 0 } }
};

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
		gtk_widget_set_visible(self->attachments, FALSE);
		gtk_widget_set_visible(self->calls, FALSE);
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

	{
		g_autofree gchar *source = row_source_text(self->block);
		g_auto(GStrv) blocks = ai_gui_content_code_blocks(source);

		/* Only when there is code to copy: a button that does nothing
		 * is worse than no button. */
		gtk_widget_set_visible(self->code_button, blocks[0] != NULL);

		if (blocks[0] != NULL)
		{
			gtk_widget_set_visible(self->header, TRUE);
			gtk_widget_set_tooltip_text(self->code_button,
				blocks[1] != NULL ? "Copy every code block"
				                  : "Copy the code block");
		}
	}

	row_rebuild_attachments(self);
	row_rebuild_calls(self);
}

void
ai_gui_block_row_set_directory(
	AiGuiBlockRow *self,
	const gchar   *directory
){
	g_return_if_fail(AI_GUI_IS_BLOCK_ROW(self));

	if (g_strcmp0(self->directory, directory) == 0)
		return;

	g_free(self->directory);
	self->directory = g_strdup(directory);
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
	g_clear_pointer(&self->directory, g_free);
	g_clear_pointer(&self->menu, gtk_widget_unparent);
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

	self->code_button = gtk_button_new_from_icon_name("code-symbolic");
	gtk_widget_add_css_class(self->code_button, "flat");
	gtk_widget_set_visible(self->code_button, FALSE);
	g_signal_connect(self->code_button, "clicked",
	                 G_CALLBACK(on_copy_code_clicked), self);
	gtk_box_append(GTK_BOX(self->header), self->code_button);

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

	{
		GtkGesture *click = gtk_gesture_click_new();
		GtkEventController *motion = gtk_event_controller_motion_new();

		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
		                              GDK_BUTTON_PRIMARY);
		g_signal_connect(click, "released", G_CALLBACK(on_body_pressed), self);
		gtk_widget_add_controller(self->body, GTK_EVENT_CONTROLLER(click));

		g_signal_connect(motion, "motion", G_CALLBACK(on_body_motion), self);
		gtk_widget_add_controller(self->body, motion);
	}

	/* ---- what a turn carried ---- */

	self->attachments = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(self->attachments, 6);
	gtk_widget_set_visible(self->attachments, FALSE);
	gtk_box_append(GTK_BOX(self->box), self->attachments);

	/* ---- one row per tool call, when the group is open ---- */

	self->calls = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_set_margin_top(self->calls, 6);
	gtk_widget_set_margin_start(self->calls, 8);
	gtk_widget_set_visible(self->calls, FALSE);
	gtk_box_append(GTK_BOX(self->box), self->calls);

	/* ---- the menu on a block ---- */

	{
		g_autoptr(GSimpleActionGroup) actions = g_simple_action_group_new();
		g_autoptr(GMenu) model = g_menu_new();
		GtkGesture *secondary = gtk_gesture_click_new();

		g_action_map_add_action_entries(G_ACTION_MAP(actions), ROW_ACTIONS,
		                                G_N_ELEMENTS(ROW_ACTIONS), self);
		gtk_widget_insert_action_group(GTK_WIDGET(self), "row",
		                               G_ACTION_GROUP(actions));

		g_menu_append(model, "Copy", "row.copy");
		g_menu_append(model, "Copy as Markdown", "row.copy-markdown");
		g_menu_append(model, "Save as…", "row.save");

		self->menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
		gtk_widget_set_parent(self->menu, GTK_WIDGET(self));
		gtk_popover_set_has_arrow(GTK_POPOVER(self->menu), FALSE);
		gtk_widget_set_halign(self->menu, GTK_ALIGN_START);

		gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(secondary),
		                              GDK_BUTTON_SECONDARY);
		g_signal_connect(secondary, "pressed", G_CALLBACK(on_row_secondary),
		                 self);
		gtk_widget_add_controller(GTK_WIDGET(self),
		                          GTK_EVENT_CONTROLLER(secondary));
	}
}
