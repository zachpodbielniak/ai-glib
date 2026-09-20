/*
 * ai-gui-preview.c - Looking at what the conversation is talking about
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-gui-content.h"
#include "ai-gui-preview.h"

/* ================================================================
 * Shared chrome
 * ================================================================ */

typedef struct
{
	GBytes *bytes;
	gchar  *name;
} PreviewSave;

static void
preview_save_free(
	gpointer  data,
	GClosure *closure
){
	PreviewSave *save = data;

	g_clear_pointer(&save->bytes, g_bytes_unref);
	g_free(save->name);
	g_free(save);
}

static void
on_save_finished(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	PreviewSave *save = user_data;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GError) error = NULL;
	gconstpointer data;
	gsize size = 0;

	file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);

	if (file == NULL)
	{
		/* Dismissing a chooser is not a failure to report. */
		g_debug("ai-gui: save dismissed: %s",
		        error != NULL ? error->message : "no file");
		return;
	}

	data = g_bytes_get_data(save->bytes, &size);

	if (!g_file_replace_contents(file, data, size, NULL, FALSE,
	                             G_FILE_CREATE_NONE, NULL, NULL, &error))
	{
		g_message("ai-gui: could not save: %s", error->message);
	}
}

static void
on_save_clicked(
	GtkButton *button,
	gpointer   user_data
){
	PreviewSave *save = user_data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));

	gtk_file_dialog_set_title(dialog, "Save a copy");
	gtk_file_dialog_set_initial_name(dialog, save->name);
	gtk_file_dialog_save(dialog, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
	                     NULL, on_save_finished, save);
}

/* GClosureNotify takes the closure as well, so g_free cannot be cast to
 * it without inventing a second argument the compiler is right to
 * object to. */
static void
preview_free_string(
	gpointer  data,
	GClosure *closure
){
	g_free(data);
}

static void
on_open_externally(
	GtkButton *button,
	gpointer   user_data
){
	const gchar *path = user_data;
	g_autoptr(GFile) file = g_file_new_for_path(path);
	g_autofree gchar *uri = g_file_get_uri(file);
	g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(uri);
	GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(button));

	gtk_uri_launcher_launch(launcher,
	                        GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL,
	                        NULL, NULL, NULL);
}

static AdwDialog *
preview_dialog_new(
	const gchar  *title,
	const gchar  *subtitle,
	GtkWidget   **out_content
){
	AdwDialog *dialog = adw_dialog_new();
	GtkWidget *toolbar = adw_toolbar_view_new();
	GtkWidget *header = adw_header_bar_new();

	adw_dialog_set_title(dialog, title);
	adw_dialog_set_content_width(dialog, 900);
	adw_dialog_set_content_height(dialog, 700);

	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
		adw_window_title_new(title, subtitle));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);

	g_object_set_data(G_OBJECT(dialog), "ai-header", header);
	*out_content = toolbar;

	return dialog;
}

static void
preview_add_action(
	AdwDialog *dialog,
	GtkWidget *button
){
	GtkWidget *header = g_object_get_data(G_OBJECT(dialog), "ai-header");

	adw_header_bar_pack_end(ADW_HEADER_BAR(header), button);
}

/* ================================================================
 * Images
 * ================================================================ */

GtkWidget *
ai_gui_preview_thumbnail(
	AiImageContent *image,
	gint            size
){
	g_autoptr(GdkTexture) texture = NULL;
	g_autoptr(GError) error = NULL;
	AiImage *payload;
	GBytes *bytes;
	GtkWidget *picture;

	g_return_val_if_fail(AI_IS_IMAGE_CONTENT(image), NULL);

	payload = ai_image_content_get_image(image);
	bytes = payload != NULL ? ai_image_get_bytes(payload) : NULL;

	if (bytes == NULL)
		return NULL;

	texture = gdk_texture_new_from_bytes(bytes, &error);

	if (texture == NULL)
	{
		/*
		 * g_debug: bytes that GDK will not decode came from a clipboard
		 * or a file somebody else wrote, so this is not a bug here --
		 * and the caller falls back to a plain chip.
		 */
		g_debug("ai-gui: cannot decode an attachment: %s",
		        error != NULL ? error->message : "unknown");
		return NULL;
	}

	picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_COVER);
	gtk_widget_set_size_request(picture, size, size);
	gtk_widget_set_overflow(picture, GTK_OVERFLOW_HIDDEN);
	gtk_widget_add_css_class(picture, "ai-thumbnail");

	return picture;
}

static void
preview_present_texture(
	GtkWidget   *parent,
	GdkTexture  *texture,
	GBytes      *bytes,
	const gchar *name,
	const gchar *subtitle
){
	GtkWidget *content = NULL;
	AdwDialog *dialog = preview_dialog_new(name != NULL ? name : "Image",
	                                       subtitle, &content);
	GtkWidget *scroller = gtk_scrolled_window_new();
	GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));

	/*
	 * Scaled down to fit and never up. Blowing a 64-pixel icon across a
	 * 900-pixel dialog shows its interpolation, not its content.
	 */
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_SCALE_DOWN);
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	gtk_widget_set_margin_start(picture, 12);
	gtk_widget_set_margin_end(picture, 12);
	gtk_widget_set_margin_top(picture, 12);
	gtk_widget_set_margin_bottom(picture, 12);

	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), picture);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content), scroller);
	adw_dialog_set_child(dialog, content);

	if (bytes != NULL)
	{
		PreviewSave *save = g_new0(PreviewSave, 1);
		GtkWidget *button =
			gtk_button_new_from_icon_name("document-save-symbolic");

		save->bytes = g_bytes_ref(bytes);
		save->name = g_strdup(name != NULL ? name : "image.png");

		gtk_widget_add_css_class(button, "flat");
		gtk_widget_set_tooltip_text(button, "Save a copy");
		g_signal_connect_data(button, "clicked", G_CALLBACK(on_save_clicked),
		                      save, preview_save_free, 0);
		preview_add_action(dialog, button);
	}

	adw_dialog_present(dialog, parent);
}

void
ai_gui_preview_present_image(
	GtkWidget      *parent,
	AiImageContent *image,
	const gchar    *name
){
	g_autoptr(GdkTexture) texture = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *subtitle = NULL;
	AiImage *payload;
	GBytes *bytes;

	g_return_if_fail(AI_IS_IMAGE_CONTENT(image));

	payload = ai_image_content_get_image(image);
	bytes = payload != NULL ? ai_image_get_bytes(payload) : NULL;

	if (bytes == NULL)
		return;

	texture = gdk_texture_new_from_bytes(bytes, &error);

	if (texture == NULL)
	{
		g_message("ai-gui: cannot show that attachment: %s",
		          error != NULL ? error->message : "unreadable");
		return;
	}

	subtitle = g_strdup_printf("%d × %d · %s · %.0f KiB",
		gdk_texture_get_width(texture), gdk_texture_get_height(texture),
		ai_image_get_mime_type(payload) != NULL
			? ai_image_get_mime_type(payload) : "image",
		(gdouble)g_bytes_get_size(bytes) / 1024.0);

	preview_present_texture(parent, texture, bytes, name, subtitle);
}

/* ================================================================
 * Files
 * ================================================================ */

static void
preview_present_text(
	GtkWidget   *parent,
	const gchar *path
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *name = g_path_get_basename(path);
	g_autoptr(GError) error = NULL;
	GtkWidget *content = NULL;
	AdwDialog *dialog;
	GtkWidget *scroller;
	GtkWidget *view;
	GtkWidget *box;
	gboolean truncated = FALSE;

	text = ai_gui_content_read_text(path, &truncated, &error);

	if (text == NULL)
	{
		g_message("ai-gui: cannot read %s: %s", path, error->message);
		return;
	}

	dialog = preview_dialog_new(name, path, &content);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	if (truncated)
	{
		GtkWidget *banner = adw_banner_new(
			"Showing the first megabyte. The file goes on.");

		adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
		gtk_box_append(GTK_BOX(box), banner);
	}

	view = gtk_text_view_new();
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 8);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), 8);
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
	                         text, -1);

	scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
	gtk_widget_set_vexpand(scroller, TRUE);
	gtk_box_append(GTK_BOX(box), scroller);

	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content), box);
	adw_dialog_set_child(dialog, content);

	{
		GtkWidget *button =
			gtk_button_new_from_icon_name("document-open-symbolic");

		gtk_widget_add_css_class(button, "flat");
		gtk_widget_set_tooltip_text(button, "Open in the desktop's editor");
		g_signal_connect_data(button, "clicked",
		                      G_CALLBACK(on_open_externally),
		                      g_strdup(path), preview_free_string, 0);
		preview_add_action(dialog, button);
	}

	adw_dialog_present(dialog, parent);
}

static void
preview_present_unknown(
	GtkWidget   *parent,
	const gchar *path
){
	g_autofree gchar *name = g_path_get_basename(path);
	GtkWidget *content = NULL;
	AdwDialog *dialog = preview_dialog_new(name, path, &content);
	GtkWidget *status = adw_status_page_new();
	GtkWidget *button = gtk_button_new_with_label("Open externally");

	adw_status_page_set_icon_name(ADW_STATUS_PAGE(status),
	                              "text-x-generic-symbolic");
	adw_status_page_set_title(ADW_STATUS_PAGE(status), "Nothing to show");
	adw_status_page_set_description(ADW_STATUS_PAGE(status),
		"This is not an image and not text this window can read.");

	gtk_widget_add_css_class(button, "suggested-action");
	gtk_widget_add_css_class(button, "pill");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	g_signal_connect_data(button, "clicked", G_CALLBACK(on_open_externally),
	                      g_strdup(path), preview_free_string, 0);
	adw_status_page_set_child(ADW_STATUS_PAGE(status), button);

	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(content), status);
	adw_dialog_set_child(dialog, content);
	adw_dialog_present(dialog, parent);
}

void
ai_gui_preview_present_file(
	GtkWidget   *parent,
	const gchar *path
){
	g_return_if_fail(path != NULL);

	switch (ai_gui_content_classify(path, NULL))
	{
		case AI_GUI_CONTENT_IMAGE:
		{
			g_autoptr(GdkTexture) texture = NULL;
			g_autoptr(GError) error = NULL;
			g_autofree gchar *name = g_path_get_basename(path);
			g_autofree gchar *subtitle = NULL;

			texture = gdk_texture_new_from_filename(path, &error);

			if (texture == NULL)
			{
				g_message("ai-gui: cannot decode %s: %s", path,
				          error->message);
				preview_present_unknown(parent, path);
				return;
			}

			subtitle = g_strdup_printf("%d × %d · %s",
				gdk_texture_get_width(texture),
				gdk_texture_get_height(texture), path);
			preview_present_texture(parent, texture, NULL, name, subtitle);
			return;
		}

		case AI_GUI_CONTENT_TEXT:
			preview_present_text(parent, path);
			return;

		case AI_GUI_CONTENT_UNKNOWN:
		default:
			preview_present_unknown(parent, path);
			return;
	}
}
