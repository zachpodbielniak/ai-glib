/*
 * ai-gui-composer.c - The input area: text, attachments and completion
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "ai-gui-composer.h"
#include "ai-gui-style.h"

#define COMPOSER_MAX_CANDIDATES 40

typedef struct
{
	gchar   *path;
	AiImage *image;
} AiGuiAttachment;

struct _AiGuiComposer
{
	GtkWidget parent_instance;

	GtkWidget *box;
	GtkWidget *attachments;
	GtkWidget *attachment_box;
	GtkWidget *scroller;
	GtkWidget *view;
	GtkWidget *toolbar;
	GtkWidget *attach_button;
	GtkWidget *provider_drop;
	GtkWidget *model_drop;
	GtkWidget *send_button;
	GtkWidget *hint_label;

	GtkWidget *popover;
	GtkWidget *popover_list;
	GtkWidget *popover_scroller;

	AiGuiSession *session;
	GPtrArray    *files;
	GPtrArray    *history;
	gint          history_position;
	gchar        *history_draft;

	AiCompletionResult *completion;
	GtkStringList      *providers;
	GtkStringList      *models;
	GCancellable       *models_cancellable;
	gchar              *pending_model;
	gboolean            busy;
	gboolean            updating;
	gboolean            syncing;
};

G_DEFINE_FINAL_TYPE(AiGuiComposer, ai_gui_composer, GTK_TYPE_WIDGET)

enum
{
	SIGNAL_SUBMIT,
	SIGNAL_STOP,
	N_SIGNALS
};

static guint signals[N_SIGNALS];

static void composer_update_completion(AiGuiComposer *self);
static void composer_rebuild_attachments(AiGuiComposer *self);
static void composer_load_models(AiGuiComposer *self);

/* ================================================================
 * Attachments
 * ================================================================ */

static void
attachment_free(gpointer data)
{
	AiGuiAttachment *attachment = data;

	g_free(attachment->path);
	g_clear_pointer(&attachment->image, ai_image_free);
	g_free(attachment);
}

static void
on_attachment_removed(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiComposer *self = user_data;
	gpointer entry = g_object_get_data(G_OBJECT(button), "ai-attachment");

	g_ptr_array_remove(self->files, entry);
	composer_rebuild_attachments(self);
}

static void
composer_rebuild_attachments(AiGuiComposer *self)
{
	GtkWidget *child;
	guint i;

	while ((child = gtk_widget_get_first_child(self->attachment_box)) != NULL)
		gtk_box_remove(GTK_BOX(self->attachment_box), child);

	for (i = 0; i < self->files->len; i++)
	{
		AiGuiAttachment *attachment = g_ptr_array_index(self->files, i);
		g_autofree gchar *name = g_path_get_basename(attachment->path);
		GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
		GtkWidget *icon;
		GtkWidget *label;
		GtkWidget *remove;

		gtk_widget_add_css_class(chip, "card");
		gtk_widget_add_css_class(chip, "ai-attachment");

		icon = gtk_image_new_from_icon_name(attachment->image != NULL
			? "image-x-generic-symbolic" : "text-x-generic-symbolic");
		gtk_box_append(GTK_BOX(chip), icon);

		label = gtk_label_new(name);
		gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars(GTK_LABEL(label), 24);
		gtk_widget_set_tooltip_text(label, attachment->path);
		gtk_box_append(GTK_BOX(chip), label);

		remove = gtk_button_new_from_icon_name("window-close-symbolic");
		gtk_widget_add_css_class(remove, "flat");
		gtk_widget_add_css_class(remove, "circular");
		g_object_set_data(G_OBJECT(remove), "ai-attachment", attachment);
		g_signal_connect(remove, "clicked",
		                 G_CALLBACK(on_attachment_removed), self);
		gtk_box_append(GTK_BOX(chip), remove);

		gtk_box_append(GTK_BOX(self->attachment_box), chip);
	}

	gtk_widget_set_visible(self->attachments, self->files->len > 0);
}

/*
 * An image becomes an attachment; anything else becomes an `@path`.
 *
 * That split is not a simplification -- it is the library's own. Only
 * images have a wire format of their own; a text file reaches the model
 * because the harness layer expands the mention, which also means the
 * model sees the path it was given rather than an anonymous blob.
 */
void
ai_gui_composer_attach_file(
	AiGuiComposer *self,
	GFile         *file
){
	g_autofree gchar *path = NULL;
	g_autoptr(GError) error = NULL;
	AiGuiAttachment *attachment;
	AiImage *image;

	g_return_if_fail(AI_GUI_IS_COMPOSER(self));
	g_return_if_fail(G_IS_FILE(file));

	path = g_file_get_path(file);

	if (path == NULL)
		return;

	image = ai_image_new_from_file(path, &error);

	if (image == NULL)
	{
		g_autofree gchar *quoted = strchr(path, ' ') != NULL
			? g_strdup_printf("@\"%s\"", path)
			: g_strdup_printf("@%s", path);

		g_debug("ai-gui: %s is not an image (%s); mentioning it instead",
		        path, error != NULL ? error->message : "unknown");
		ai_gui_composer_insert(self, quoted);
		return;
	}

	attachment = g_new0(AiGuiAttachment, 1);
	attachment->path = g_steal_pointer(&path);
	attachment->image = image;

	g_ptr_array_add(self->files, attachment);
	composer_rebuild_attachments(self);
}

GList *
ai_gui_composer_take_images(AiGuiComposer *self)
{
	GList *images = NULL;
	guint i;

	g_return_val_if_fail(AI_GUI_IS_COMPOSER(self), NULL);

	for (i = 0; i < self->files->len; i++)
	{
		AiGuiAttachment *attachment = g_ptr_array_index(self->files, i);

		if (attachment->image != NULL)
			images = g_list_append(images, ai_image_copy(attachment->image));
	}

	g_ptr_array_set_size(self->files, 0);
	composer_rebuild_attachments(self);

	return images;
}

/* ================================================================
 * Text
 * ================================================================ */

gchar *
ai_gui_composer_get_text(AiGuiComposer *self)
{
	GtkTextBuffer *buffer;
	GtkTextIter start;
	GtkTextIter end;

	g_return_val_if_fail(AI_GUI_IS_COMPOSER(self), NULL);

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view));
	gtk_text_buffer_get_bounds(buffer, &start, &end);

	return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

void
ai_gui_composer_set_text(
	AiGuiComposer *self,
	const gchar   *text
){
	GtkTextBuffer *buffer;

	g_return_if_fail(AI_GUI_IS_COMPOSER(self));

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view));
	self->updating = TRUE;
	gtk_text_buffer_set_text(buffer, text != NULL ? text : "", -1);
	self->updating = FALSE;
}

gchar *
ai_gui_composer_take_text(AiGuiComposer *self)
{
	gchar *text;

	g_return_val_if_fail(AI_GUI_IS_COMPOSER(self), NULL);

	text = ai_gui_composer_get_text(self);
	ai_gui_composer_set_text(self, "");

	if (text != NULL && *text != '\0')
	{
		g_ptr_array_add(self->history, g_strdup(text));
		self->history_position = -1;
		g_clear_pointer(&self->history_draft, g_free);
	}

	return text;
}

void
ai_gui_composer_insert(
	AiGuiComposer *self,
	const gchar   *text
){
	GtkTextBuffer *buffer;
	GtkTextIter iter;
	g_autofree gchar *padded = NULL;

	g_return_if_fail(AI_GUI_IS_COMPOSER(self));

	if (text == NULL || *text == '\0')
		return;

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view));
	gtk_text_buffer_get_iter_at_mark(buffer, &iter,
	                                 gtk_text_buffer_get_insert(buffer));

	/* A mention run into the previous word is a different word. */
	padded = gtk_text_iter_starts_line(&iter) || gtk_text_iter_is_start(&iter)
		? g_strdup_printf("%s ", text)
		: g_strdup_printf(" %s ", text);

	gtk_text_buffer_insert(buffer, &iter, padded, -1);
	gtk_widget_grab_focus(self->view);
}

void
ai_gui_composer_focus(AiGuiComposer *self)
{
	g_return_if_fail(AI_GUI_IS_COMPOSER(self));
	gtk_widget_grab_focus(self->view);
}

/* ================================================================
 * Completion
 * ================================================================ */

/*
 * Byte offsets, like every other offset this library hands out.
 *
 * #GtkTextIter counts characters, so the two conversions live here and
 * nowhere else -- a completion range applied at the wrong offset chops a
 * multi-byte character in half and leaves the buffer holding text no
 * provider will accept.
 */
static guint
composer_cursor_bytes(
	AiGuiComposer  *self,
	gchar         **out_text
){
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view));
	GtkTextIter start;
	GtkTextIter end;
	GtkTextIter cursor;
	g_autofree gchar *text = NULL;
	const gchar *pointer;
	guint offset;

	gtk_text_buffer_get_bounds(buffer, &start, &end);
	text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	gtk_text_buffer_get_iter_at_mark(buffer, &cursor,
	                                 gtk_text_buffer_get_insert(buffer));

	pointer = g_utf8_offset_to_pointer(text, gtk_text_iter_get_offset(&cursor));
	offset = (guint)(pointer - text);

	if (out_text != NULL)
		*out_text = g_steal_pointer(&text);

	return offset;
}

static void
composer_replace_bytes(
	AiGuiComposer *self,
	const gchar   *text,
	guint          start_bytes,
	guint          end_bytes,
	const gchar   *replacement
){
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view));
	GtkTextIter start;
	GtkTextIter end;
	glong start_chars;
	glong end_chars;

	start_chars = g_utf8_pointer_to_offset(text, text + start_bytes);
	end_chars = g_utf8_pointer_to_offset(text, text + end_bytes);

	gtk_text_buffer_get_iter_at_offset(buffer, &start, (gint)start_chars);
	gtk_text_buffer_get_iter_at_offset(buffer, &end, (gint)end_chars);

	self->updating = TRUE;
	gtk_text_buffer_delete(buffer, &start, &end);
	gtk_text_buffer_insert(buffer, &start, replacement, -1);
	self->updating = FALSE;
}

static void
composer_hide_completion(AiGuiComposer *self)
{
	g_clear_object(&self->completion);
	gtk_popover_popdown(GTK_POPOVER(self->popover));
}

static void
composer_accept_completion(
	AiGuiComposer *self,
	guint          index
){
	g_autofree gchar *text = NULL;
	const gchar *replacement = NULL;
	gboolean is_directory = FALSE;
	guint start;
	guint end;

	if (self->completion == NULL)
		return;

	if (!ai_completion_result_get_item_fields(self->completion, index,
		&replacement, NULL, NULL, NULL, &is_directory))
	{
		return;
	}

	start = ai_completion_result_get_start(self->completion);
	end = ai_completion_result_get_end(self->completion);

	composer_cursor_bytes(self, &text);

	{
		/* A directory is a step, not an answer: leave the cursor inside it
		 * so the next keystroke keeps completing rather than starting a
		 * word that will never match anything. */
		g_autofree gchar *final = is_directory
			? g_strdup(replacement)
			: g_strdup_printf("%s ", replacement);

		composer_replace_bytes(self, text, start, end, final);
	}

	composer_hide_completion(self);

	if (is_directory)
		composer_update_completion(self);
}

static void
on_candidate_activated(
	GtkListBox    *list,
	GtkListBoxRow *row,
	gpointer       user_data
){
	composer_accept_completion(user_data,
		(guint)gtk_list_box_row_get_index(row));
}

static void
composer_update_completion(AiGuiComposer *self)
{
	g_autofree gchar *text = NULL;
	AiCompletionContext *context;
	GtkWidget *child;
	guint cursor;
	guint n_items;
	guint i;

	if (self->session == NULL || self->updating)
		return;

	context = ai_gui_session_get_completion(self->session);

	if (context == NULL)
		return;

	cursor = composer_cursor_bytes(self, &text);

	g_clear_object(&self->completion);
	self->completion = ai_completion_context_query(context, text, cursor);

	if (self->completion == NULL ||
	    ai_completion_result_get_kind(self->completion) == AI_COMPLETION_NONE ||
	    ai_completion_result_get_n_items(self->completion) == 0)
	{
		composer_hide_completion(self);
		return;
	}

	while ((child = gtk_widget_get_first_child(self->popover_list)) != NULL)
		gtk_list_box_remove(GTK_LIST_BOX(self->popover_list), child);

	n_items = ai_completion_result_get_n_items(self->completion);

	if (n_items > COMPOSER_MAX_CANDIDATES)
		n_items = COMPOSER_MAX_CANDIDATES;

	for (i = 0; i < n_items; i++)
	{
		const gchar *display = NULL;
		const gchar *description = NULL;
		const gchar *origin = NULL;
		gboolean is_directory = FALSE;
		GtkWidget *row;
		GtkWidget *column;
		GtkWidget *title;

		if (!ai_completion_result_get_item_fields(self->completion, i, NULL,
			&display, &description, &origin, &is_directory))
		{
			continue;
		}

		row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
		gtk_widget_set_margin_start(row, 6);
		gtk_widget_set_margin_end(row, 6);
		gtk_widget_set_margin_top(row, 3);
		gtk_widget_set_margin_bottom(row, 3);

		gtk_box_append(GTK_BOX(row),
			gtk_image_new_from_icon_name(is_directory
				? "folder-symbolic" : "text-x-generic-symbolic"));

		column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_set_hexpand(column, TRUE);

		title = gtk_label_new(display);
		gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
		gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
		gtk_box_append(GTK_BOX(column), title);

		if (description != NULL && *description != '\0')
		{
			GtkWidget *subtitle = gtk_label_new(description);

			gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0f);
			gtk_label_set_ellipsize(GTK_LABEL(subtitle),
			                        PANGO_ELLIPSIZE_END);
			gtk_widget_add_css_class(subtitle, "dim-label");
			gtk_widget_add_css_class(subtitle, "ai-session-subtitle");
			gtk_box_append(GTK_BOX(column), subtitle);
		}

		gtk_box_append(GTK_BOX(row), column);

		/*
		 * Origin beside the description rather than folded into it: a
		 * menu truncates, and with them joined the one piece that tells
		 * two same-named commands apart is the first thing to fall off.
		 */
		if (origin != NULL && *origin != '\0')
		{
			GtkWidget *badge = gtk_label_new(origin);

			gtk_widget_add_css_class(badge, "dim-label");
			gtk_widget_add_css_class(badge, "ai-session-subtitle");
			gtk_box_append(GTK_BOX(row), badge);
		}

		gtk_list_box_append(GTK_LIST_BOX(self->popover_list), row);
	}

	{
		GtkListBoxRow *first =
			gtk_list_box_get_row_at_index(GTK_LIST_BOX(self->popover_list), 0);

		if (first != NULL)
			gtk_list_box_select_row(GTK_LIST_BOX(self->popover_list), first);
	}

	gtk_popover_popup(GTK_POPOVER(self->popover));
}

static gboolean
composer_completion_visible(AiGuiComposer *self)
{
	return self->completion != NULL &&
		gtk_widget_get_visible(self->popover);
}

static void
composer_move_selection(
	AiGuiComposer *self,
	gint           delta
){
	GtkListBox *list = GTK_LIST_BOX(self->popover_list);
	GtkListBoxRow *selected = gtk_list_box_get_selected_row(list);
	GtkListBoxRow *next;
	gint index = selected != NULL ? gtk_list_box_row_get_index(selected) : 0;

	next = gtk_list_box_get_row_at_index(list, index + delta);

	if (next == NULL)
		return;

	gtk_list_box_select_row(list, next);

	/*
	 * Keep it on screen without taking focus.
	 *
	 * The text view still owns the caret -- a popover that grabbed focus
	 * would send the next keystroke somewhere nobody is looking -- and a
	 * #GtkListBox only scrolls for the focus it does not have, so the
	 * adjustment is moved by hand.
	 */
	{
		GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(
			GTK_SCROLLED_WINDOW(self->popover_scroller));
		graphene_rect_t bounds;

		if (adjustment != NULL &&
		    gtk_widget_compute_bounds(GTK_WIDGET(next), self->popover_list,
		                              &bounds))
		{
			gdouble value = gtk_adjustment_get_value(adjustment);
			gdouble page = gtk_adjustment_get_page_size(adjustment);
			gdouble top = (gdouble)bounds.origin.y;
			gdouble bottom = top + (gdouble)bounds.size.height;

			if (top < value)
				gtk_adjustment_set_value(adjustment, top);
			else if (bottom > value + page)
				gtk_adjustment_set_value(adjustment, bottom - page);
		}
	}
}

/* ================================================================
 * Input history
 * ================================================================ */

static void
composer_history_step(
	AiGuiComposer *self,
	gint           delta
){
	gint position;

	if (self->history->len == 0)
		return;

	if (self->history_position < 0)
	{
		if (delta > 0)
			return;

		g_clear_pointer(&self->history_draft, g_free);
		self->history_draft = ai_gui_composer_get_text(self);
		position = (gint)self->history->len - 1;
	}
	else
	{
		position = self->history_position + delta;
	}

	if (position < 0)
		position = 0;

	if (position >= (gint)self->history->len)
	{
		self->history_position = -1;
		ai_gui_composer_set_text(self, self->history_draft);
		return;
	}

	self->history_position = position;
	ai_gui_composer_set_text(self,
		g_ptr_array_index(self->history, (guint)position));
}

/* ================================================================
 * Keys
 * ================================================================ */

static gboolean
on_key_pressed(
	GtkEventControllerKey *controller,
	guint                  keyval,
	guint                  keycode,
	GdkModifierType        state,
	gpointer               user_data
){
	AiGuiComposer *self = user_data;
	gboolean shift = (state & GDK_SHIFT_MASK) != 0;
	gboolean control = (state & GDK_CONTROL_MASK) != 0;

	if (composer_completion_visible(self))
	{
		switch (keyval)
		{
			case GDK_KEY_Escape:
				composer_hide_completion(self);
				return TRUE;
			case GDK_KEY_Down:
				composer_move_selection(self, 1);
				return TRUE;
			case GDK_KEY_Up:
				composer_move_selection(self, -1);
				return TRUE;
			case GDK_KEY_Tab:
			case GDK_KEY_Return:
			case GDK_KEY_KP_Enter:
			{
				GtkListBoxRow *selected = gtk_list_box_get_selected_row(
					GTK_LIST_BOX(self->popover_list));

				composer_accept_completion(self, selected != NULL
					? (guint)gtk_list_box_row_get_index(selected) : 0);
				return TRUE;
			}
			default:
				break;
		}
	}

	if (keyval == GDK_KEY_Tab && !shift && !control)
	{
		composer_update_completion(self);
		return composer_completion_visible(self);
	}

	if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
	{
		/*
		 * Enter sends, Shift+Enter makes a line.
		 *
		 * The opposite of a text editor and the same as every chat
		 * client, which is what this is. Ctrl+Enter sends too, because
		 * somebody coming from the other convention will try it.
		 */
		if (shift)
			return FALSE;

		g_signal_emit(self, signals[SIGNAL_SUBMIT], 0);
		return TRUE;
	}

	/*
	 * Ctrl+backslash, forwarded rather than left to the window.
	 *
	 * #GtkTextView binds it to delete-from-cursor(WHITESPACE) and
	 * consumes it, so the window's own binding never sees the key while
	 * the composer has focus -- which is every time somebody would press
	 * it. ai-tui uses this key for the dashboard and one habit should
	 * cover both front-ends.
	 */
	if (control && !shift &&
	    (keyval == GDK_KEY_backslash || keyval == GDK_KEY_bar))
	{
		gtk_widget_activate_action(GTK_WIDGET(self), "win.dashboard", NULL);
		return TRUE;
	}

	if (control && (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down))
	{
		composer_history_step(self, keyval == GDK_KEY_Up ? -1 : 1);
		return TRUE;
	}

	if (keyval == GDK_KEY_Escape && self->busy)
	{
		g_signal_emit(self, signals[SIGNAL_STOP], 0);
		return TRUE;
	}

	return FALSE;
}

static void
on_buffer_changed(
	GtkTextBuffer *buffer,
	gpointer       user_data
){
	composer_update_completion(user_data);
}

/* ================================================================
 * Toolbar
 * ================================================================ */

static void
on_files_chosen(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiComposer *self = user_data;
	g_autoptr(GListModel) files = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	guint n;

	files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source),
	                                             result, &error);

	if (files == NULL)
	{
		/* Dismissing a file chooser is not a failure to report. */
		g_debug("ai-gui: no files chosen: %s",
		        error != NULL ? error->message : "dismissed");
		return;
	}

	n = g_list_model_get_n_items(files);

	for (i = 0; i < n; i++)
	{
		g_autoptr(GFile) file = g_list_model_get_item(files, i);

		ai_gui_composer_attach_file(self, file);
	}
}

static void
on_attach_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiComposer *self = user_data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));

	gtk_file_dialog_set_title(dialog, "Attach files");

	if (self->session != NULL)
	{
		g_autoptr(GFile) directory = g_file_new_for_path(
			ai_gui_session_get_working_directory(self->session));

		gtk_file_dialog_set_initial_folder(dialog, directory);
	}

	gtk_file_dialog_open_multiple(dialog, GTK_IS_WINDOW(root)
		? GTK_WINDOW(root) : NULL, NULL, on_files_chosen, self);
}

static void
on_send_clicked(
	GtkButton *button,
	gpointer   user_data
){
	AiGuiComposer *self = user_data;

	g_signal_emit(self, signals[self->busy ? SIGNAL_STOP : SIGNAL_SUBMIT], 0);
}

/* ================================================================
 * Drag and drop
 * ================================================================ */

static gboolean
on_drop(
	GtkDropTarget *target,
	const GValue  *value,
	gdouble        x,
	gdouble        y,
	gpointer       user_data
){
	AiGuiComposer *self = user_data;

	if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
	{
		GSList *files = g_value_get_boxed(value);
		GSList *iter;

		for (iter = files; iter != NULL; iter = iter->next)
			ai_gui_composer_attach_file(self, iter->data);

		return TRUE;
	}

	if (G_VALUE_HOLDS(value, G_TYPE_FILE))
	{
		ai_gui_composer_attach_file(self, g_value_get_object(value));
		return TRUE;
	}

	if (G_VALUE_HOLDS(value, G_TYPE_STRING))
	{
		ai_gui_composer_insert(self, g_value_get_string(value));
		return TRUE;
	}

	return FALSE;
}

/* ================================================================
 * Provider and model, per question
 * ================================================================ */

/*
 * The pickers choose where the *next* question goes.
 *
 * Nothing is switched while they are being used: a provider switch mid
 * turn would move the conversation out from under a reply that is still
 * arriving. The window applies the selection immediately before it
 * sends, which is what makes "per question" true rather than
 * approximately true.
 */

static guint
composer_string_position(
	GtkStringList *list,
	const gchar   *text
){
	guint n = g_list_model_get_n_items(G_LIST_MODEL(list));
	guint i;

	if (text == NULL)
		return GTK_INVALID_LIST_POSITION;

	for (i = 0; i < n; i++)
	{
		if (g_strcmp0(gtk_string_list_get_string(list, i), text) == 0)
			return i;
	}

	return GTK_INVALID_LIST_POSITION;
}

const gchar *
ai_gui_composer_get_selected_provider(AiGuiComposer *self)
{
	guint selected;

	g_return_val_if_fail(AI_GUI_IS_COMPOSER(self), NULL);

	selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->provider_drop));

	if (selected == GTK_INVALID_LIST_POSITION)
		return NULL;

	return gtk_string_list_get_string(self->providers, selected);
}

const gchar *
ai_gui_composer_get_selected_model(AiGuiComposer *self)
{
	guint selected;

	g_return_val_if_fail(AI_GUI_IS_COMPOSER(self), NULL);

	selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(self->model_drop));

	if (selected == GTK_INVALID_LIST_POSITION)
		return NULL;

	return gtk_string_list_get_string(self->models, selected);
}

/* Replace the whole list rather than splice: the previous provider's
 * models must not survive into the next provider's menu even for the
 * moment before the answer arrives. */
static void
composer_set_models(
	AiGuiComposer      *self,
	const gchar *const *names,
	const gchar        *selected
){
	guint position;

	self->syncing = TRUE;
	gtk_string_list_splice(self->models, 0,
		g_list_model_get_n_items(G_LIST_MODEL(self->models)), names);

	/*
	 * A model the provider does not advertise is still the model this
	 * session is on -- `--set model=...`, or a list that simply does not
	 * include it. Dropping it would silently move the next question to
	 * whatever happened to be first.
	 */
	if (selected != NULL && *selected != '\0' &&
	    composer_string_position(self->models, selected) ==
		    GTK_INVALID_LIST_POSITION)
	{
		gtk_string_list_append(self->models, selected);
	}

	position = composer_string_position(self->models, selected);
	gtk_drop_down_set_selected(GTK_DROP_DOWN(self->model_drop),
		position != GTK_INVALID_LIST_POSITION ? position : 0);
	self->syncing = FALSE;

	gtk_widget_set_sensitive(self->model_drop,
		g_list_model_get_n_items(G_LIST_MODEL(self->models)) > 0);
}

static void
on_models_ready(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	AiGuiComposer *self = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *wanted = g_strdup(self->pending_model);
	GList *models;
	GList *iter;

	models = ai_provider_list_models_finish(AI_PROVIDER(source), result,
	                                        &error);

	if (error != NULL)
	{
		/*
		 * g_debug: a provider that cannot list models is an absent CLI,
		 * a missing key or no network -- none of them this program's
		 * bug, and the picker still offers the current model.
		 */
		g_debug("ai-gui: cannot list models: %s", error->message);
	}

	for (iter = models; iter != NULL; iter = iter->next)
		g_ptr_array_add(names, g_strdup(iter->data));

	g_list_free_full(models, g_free);
	g_ptr_array_add(names, NULL);

	composer_set_models(self, (const gchar * const *)names->pdata, wanted);
	g_object_unref(self);
}

static void
composer_load_models(AiGuiComposer *self)
{
	g_autoptr(AiConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	GObject *provider;
	const gchar *name = ai_gui_composer_get_selected_provider(self);
	const gchar *current[] = { NULL };

	g_cancellable_cancel(self->models_cancellable);
	g_clear_object(&self->models_cancellable);
	self->models_cancellable = g_cancellable_new();

	if (name == NULL)
		return;

	/*
	 * A throwaway provider object, because the session has not switched
	 * yet and must not be switched merely to populate a menu.
	 */
	config = ai_config_new();
	provider = ai_provider_factory_new_from_string(name, config, &error);

	if (provider == NULL)
	{
		g_debug("ai-gui: cannot build %s to list its models: %s", name,
		        error != NULL ? error->message : "unknown");
		composer_set_models(self, current, self->pending_model);
		return;
	}

	/*
	 * Not every provider implements the listing vfunc -- claude-tmux
	 * does not -- and ai_provider_list_models_async() answers a missing
	 * one with a critical, which is fatal under fatal-warnings. Asking
	 * the interface first is the check.
	 */
	if (AI_PROVIDER_GET_IFACE(AI_PROVIDER(provider))->list_models_async == NULL)
	{
		composer_set_models(self, current, self->pending_model);
		g_object_unref(provider);
		return;
	}

	ai_provider_list_models_async(AI_PROVIDER(provider),
	                              self->models_cancellable, on_models_ready,
	                              g_object_ref(self));
	g_object_unref(provider);
}

static void
on_provider_selected(
	GObject    *drop,
	GParamSpec *pspec,
	gpointer    user_data
){
	AiGuiComposer *self = user_data;

	if (self->syncing)
		return;

	/* The session's model is meaningless on another provider, so the
	 * menu starts from that provider's own default. */
	g_clear_pointer(&self->pending_model, g_free);
	composer_load_models(self);
}

void
ai_gui_composer_sync_model(AiGuiComposer *self)
{
	const gchar *provider;
	guint position;

	g_return_if_fail(AI_GUI_IS_COMPOSER(self));

	if (self->session == NULL)
		return;

	provider = ai_gui_session_get_provider_id(self->session);
	position = composer_string_position(self->providers, provider);

	self->syncing = TRUE;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(self->provider_drop),
		position != GTK_INVALID_LIST_POSITION ? position : 0);
	self->syncing = FALSE;

	g_clear_pointer(&self->pending_model, g_free);
	self->pending_model = g_strdup(ai_gui_session_get_model(self->session));
	composer_load_models(self);
}

/* ================================================================
 * Session binding
 * ================================================================ */

void
ai_gui_composer_set_session(
	AiGuiComposer *self,
	AiGuiSession  *session
){
	g_return_if_fail(AI_GUI_IS_COMPOSER(self));

	if (self->session == session)
		return;

	/* The draft belongs to the conversation it was aimed at. */
	if (self->session != NULL)
	{
		g_autofree gchar *draft = ai_gui_composer_get_text(self);

		g_object_set_data_full(G_OBJECT(self->session), "ai-gui-draft",
		                       g_steal_pointer(&draft), g_free);
	}

	composer_hide_completion(self);
	g_set_object(&self->session, session);

	if (session != NULL)
	{
		ai_gui_composer_set_text(self,
			g_object_get_data(G_OBJECT(session), "ai-gui-draft"));
	}
	else
	{
		ai_gui_composer_set_text(self, "");
	}

	ai_gui_composer_sync_model(self);
	self->history_position = -1;
}

AiGuiSession *
ai_gui_composer_get_session(AiGuiComposer *self)
{
	g_return_val_if_fail(AI_GUI_IS_COMPOSER(self), NULL);
	return self->session;
}

void
ai_gui_composer_set_busy(
	AiGuiComposer *self,
	gboolean       busy
){
	g_return_if_fail(AI_GUI_IS_COMPOSER(self));

	self->busy = busy;

	gtk_button_set_icon_name(GTK_BUTTON(self->send_button),
		busy ? "process-stop-symbolic" : "document-send-symbolic");
	gtk_widget_set_tooltip_text(self->send_button,
		busy ? "Stop this turn (Esc)" : "Send (Enter)");

	if (busy)
		gtk_widget_add_css_class(self->send_button, "destructive-action");
	else
		gtk_widget_remove_css_class(self->send_button, "destructive-action");

	gtk_label_set_text(GTK_LABEL(self->hint_label), busy
		? "Working — Enter queues a follow-up, Esc stops"
		: "Enter sends · Shift+Enter newline · Tab completes / and @");
}

/* ================================================================
 * GObject boilerplate
 * ================================================================ */

GtkWidget *
ai_gui_composer_new(void)
{
	return g_object_new(AI_GUI_TYPE_COMPOSER, NULL);
}

static void
ai_gui_composer_dispose(GObject *object)
{
	AiGuiComposer *self = AI_GUI_COMPOSER(object);

	g_cancellable_cancel(self->models_cancellable);
	g_clear_object(&self->models_cancellable);
	g_clear_object(&self->providers);
	g_clear_object(&self->models);
	g_clear_object(&self->completion);
	g_clear_object(&self->session);
	g_clear_pointer(&self->popover, gtk_widget_unparent);
	g_clear_pointer(&self->box, gtk_widget_unparent);

	G_OBJECT_CLASS(ai_gui_composer_parent_class)->dispose(object);
}

static void
ai_gui_composer_finalize(GObject *object)
{
	AiGuiComposer *self = AI_GUI_COMPOSER(object);

	g_clear_pointer(&self->files, g_ptr_array_unref);
	g_clear_pointer(&self->history, g_ptr_array_unref);
	g_free(self->history_draft);
	g_free(self->pending_model);

	G_OBJECT_CLASS(ai_gui_composer_parent_class)->finalize(object);
}

static void
ai_gui_composer_class_init(AiGuiComposerClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

	object_class->dispose = ai_gui_composer_dispose;
	object_class->finalize = ai_gui_composer_finalize;

	gtk_widget_class_set_layout_manager_type(widget_class,
	                                         GTK_TYPE_BIN_LAYOUT);
	gtk_widget_class_set_css_name(widget_class, "aicomposer");

	/**
	 * AiGuiComposer::submit:
	 *
	 * The person asked for what they typed to be sent.
	 *
	 * Carries nothing: the window pulls the text and the attachments with
	 * ai_gui_composer_take_text() and ai_gui_composer_take_images().
	 * A signal carrying a #GList of boxed images would need a marshaller
	 * and a copy rule for something the receiver is about to consume.
	 */
	signals[SIGNAL_SUBMIT] = g_signal_new("submit", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

	/**
	 * AiGuiComposer::stop:
	 *
	 * The person asked for the turn in flight to be cancelled.
	 */
	signals[SIGNAL_STOP] = g_signal_new("stop", G_TYPE_FROM_CLASS(klass),
		G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ai_gui_composer_init(AiGuiComposer *self)
{
	GtkEventController *keys;
	GtkDropTarget *drop;
	GtkTextBuffer *buffer;
	GType drop_types[3];

	self->files = g_ptr_array_new_with_free_func(attachment_free);
	self->history = g_ptr_array_new_with_free_func(g_free);
	self->history_position = -1;

	self->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_widget_add_css_class(self->box, "ai-composer");
	gtk_widget_add_css_class(self->box, "card");
	gtk_widget_set_parent(self->box, GTK_WIDGET(self));

	/* ---- attachments ---- */

	self->attachments = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->attachments),
	                               GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
	gtk_widget_set_visible(self->attachments, FALSE);

	self->attachment_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->attachments),
	                              self->attachment_box);
	gtk_box_append(GTK_BOX(self->box), self->attachments);

	/* ---- text ---- */

	self->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_max_content_height(
		GTK_SCROLLED_WINDOW(self->scroller), 260);
	gtk_scrolled_window_set_propagate_natural_height(
		GTK_SCROLLED_WINDOW(self->scroller), TRUE);

	self->view = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->view), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(self->view), 6);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(self->view), 6);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(self->view), 8);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(self->view), 8);
	gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(self->view), FALSE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->scroller),
	                              self->view);
	gtk_box_append(GTK_BOX(self->box), self->scroller);

	buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->view));
	g_signal_connect(buffer, "changed", G_CALLBACK(on_buffer_changed), self);

	keys = gtk_event_controller_key_new();
	gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
	gtk_widget_add_controller(self->view, keys);

	drop = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
	drop_types[0] = GDK_TYPE_FILE_LIST;
	drop_types[1] = G_TYPE_FILE;
	drop_types[2] = G_TYPE_STRING;
	gtk_drop_target_set_gtypes(drop, drop_types, G_N_ELEMENTS(drop_types));
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
	gtk_widget_add_controller(self->box, GTK_EVENT_CONTROLLER(drop));

	/* ---- toolbar ---- */

	self->toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_start(self->toolbar, 4);
	gtk_widget_set_margin_end(self->toolbar, 4);
	gtk_widget_set_margin_bottom(self->toolbar, 4);

	self->attach_button =
		gtk_button_new_from_icon_name("mail-attachment-symbolic");
	gtk_widget_add_css_class(self->attach_button, "flat");
	gtk_widget_set_tooltip_text(self->attach_button,
	                            "Attach files (images are sent, others mentioned)");
	g_signal_connect(self->attach_button, "clicked",
	                 G_CALLBACK(on_attach_clicked), self);
	gtk_box_append(GTK_BOX(self->toolbar), self->attach_button);

	/* ---- provider and model, for the next question ---- */

	{
		GEnumClass *klass = g_type_class_ref(AI_TYPE_PROVIDER_TYPE);
		guint i;

		self->providers = gtk_string_list_new(NULL);

		/*
		 * Straight off the enum, so a provider added to the library
		 * appears here without anybody remembering to come back --
		 * the same reason the preferences dialog reads it.
		 */
		for (i = 0; i < klass->n_values; i++)
			gtk_string_list_append(self->providers, klass->values[i].value_nick);

		g_type_class_unref(klass);
	}

	self->models = gtk_string_list_new(NULL);

	self->provider_drop = gtk_drop_down_new(
		G_LIST_MODEL(g_object_ref(self->providers)), NULL);
	gtk_widget_add_css_class(self->provider_drop, "flat");
	gtk_widget_set_tooltip_text(self->provider_drop,
	                            "Provider for the next question");
	g_signal_connect(self->provider_drop, "notify::selected",
	                 G_CALLBACK(on_provider_selected), self);
	gtk_box_append(GTK_BOX(self->toolbar), self->provider_drop);

	self->model_drop = gtk_drop_down_new(
		G_LIST_MODEL(g_object_ref(self->models)), NULL);
	gtk_widget_add_css_class(self->model_drop, "flat");
	gtk_widget_set_tooltip_text(self->model_drop,
	                            "Model for the next question");
	/*
	 * Searchable, because a provider can advertise a hundred models and
	 * a scroll through all of them is not a choice anybody makes twice.
	 * The expression is what a #GtkDropDown searches against.
	 */
	gtk_drop_down_set_expression(GTK_DROP_DOWN(self->model_drop),
		gtk_property_expression_new(GTK_TYPE_STRING_OBJECT, NULL, "string"));
	gtk_drop_down_set_enable_search(GTK_DROP_DOWN(self->model_drop), TRUE);
	gtk_widget_set_sensitive(self->model_drop, FALSE);
	gtk_box_append(GTK_BOX(self->toolbar), self->model_drop);

	self->hint_label = gtk_label_new(NULL);
	gtk_widget_add_css_class(self->hint_label, "dim-label");
	gtk_widget_add_css_class(self->hint_label, "ai-session-subtitle");
	gtk_label_set_ellipsize(GTK_LABEL(self->hint_label), PANGO_ELLIPSIZE_END);
	gtk_label_set_xalign(GTK_LABEL(self->hint_label), 0.0f);
	gtk_widget_set_hexpand(self->hint_label, TRUE);
	gtk_box_append(GTK_BOX(self->toolbar), self->hint_label);

	self->send_button =
		gtk_button_new_from_icon_name("document-send-symbolic");
	gtk_widget_add_css_class(self->send_button, "suggested-action");
	gtk_widget_add_css_class(self->send_button, "circular");
	g_signal_connect(self->send_button, "clicked",
	                 G_CALLBACK(on_send_clicked), self);
	gtk_box_append(GTK_BOX(self->toolbar), self->send_button);

	gtk_box_append(GTK_BOX(self->box), self->toolbar);

	/* ---- completion popover ---- */

	self->popover = gtk_popover_new();
	gtk_popover_set_autohide(GTK_POPOVER(self->popover), FALSE);
	gtk_popover_set_position(GTK_POPOVER(self->popover), GTK_POS_TOP);
	gtk_widget_set_parent(self->popover, GTK_WIDGET(self));

	self->popover_scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(self->popover_scroller),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_size_request(self->popover_scroller, 420, 240);

	self->popover_list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->popover_list),
	                                GTK_SELECTION_SINGLE);
	g_signal_connect(self->popover_list, "row-activated",
	                 G_CALLBACK(on_candidate_activated), self);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(self->popover_scroller),
	                              self->popover_list);
	gtk_popover_set_child(GTK_POPOVER(self->popover), self->popover_scroller);

	ai_gui_composer_set_busy(self, FALSE);
}
