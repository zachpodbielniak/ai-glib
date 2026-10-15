/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-gui-queue.h"

typedef struct {
	AiGuiSession *session;
	AdwDialog *dialog;
	GtkWidget *error;
	GPtrArray *buffers;
	GPtrArray *removed;
} QueueEditor;

static void
queue_closed(AdwDialog *dialog, gpointer data)
{
	QueueEditor *editor = data;
	ai_gui_session_resume_queue(editor->session);
	g_object_unref(editor->session);
	g_ptr_array_unref(editor->buffers);
	g_ptr_array_unref(editor->removed);
	g_free(editor);
}

static void
queue_cancel(GtkButton *button, gpointer data)
{
	adw_dialog_close(((QueueEditor *)data)->dialog);
}

static void
queue_save(GtkButton *button, gpointer data)
{
	QueueEditor *editor = data;
	AiPromptQueue *queue = ai_gui_session_get_queue(editor->session);
	g_autoptr(AiPromptQueue) check = ai_prompt_queue_new();
	g_autoptr(GPtrArray) texts = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	guint i;
	/* Validate the entire edit before mutating anything. Entries arriving
	 * while the dialog is open append after these and remain untouched. */
	g_object_set(check, "max-length", 4096u, NULL);
	for (i = 0; i < editor->buffers->len; i++) {
		GtkTextBuffer *buffer = g_ptr_array_index(editor->buffers, i);
		GtkTextIter start, end;
		GList *images = NULL;
		gchar *text;
		gtk_text_buffer_get_bounds(buffer, &start, &end);
		text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
		g_ptr_array_add(texts, text);
		if (gtk_check_button_get_active(g_ptr_array_index(editor->removed, i))) continue;
		if (ai_prompt_queue_get(queue, i, &images) == NULL ||
		    !ai_prompt_queue_push(check, text, images, text[0] == '/', &error)) {
			gtk_label_set_text(GTK_LABEL(editor->error), error != NULL ? error->message : "The queue changed; close and reopen it.");
			return;
		}
	}
	for (i = texts->len; i > 0; i--) {
		const gchar *text = g_ptr_array_index(texts, i - 1);
		if (gtk_check_button_get_active(g_ptr_array_index(editor->removed, i - 1)))
			ai_prompt_queue_remove(queue, i - 1);
		else
			ai_prompt_queue_replace(queue, i - 1, text, text[0] == '/', NULL);
	}
	adw_dialog_close(editor->dialog);
}

void
ai_gui_queue_present(GtkWidget *parent, AiGuiSession *session)
{
	QueueEditor *editor;
	AiPromptQueue *queue = ai_gui_session_get_queue(session);
	GtkWidget *box, *scroll, *rows, *bar, *save, *cancel;
	guint i, n = ai_prompt_queue_get_length(queue);
	if (ai_prompt_queue_get_paused(queue)) return;
	ai_prompt_queue_set_paused(queue, TRUE);
	editor = g_new0(QueueEditor, 1);
	editor->session = g_object_ref(session);
	editor->buffers = g_ptr_array_new_with_free_func(g_object_unref);
	editor->removed = g_ptr_array_new();
	editor->dialog = adw_dialog_new();
	adw_dialog_set_title(editor->dialog, "Queued messages");
	adw_dialog_set_content_width(editor->dialog, 640);
	adw_dialog_set_content_height(editor->dialog, 520);
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_box_append(GTK_BOX(box), adw_header_bar_new());
	gtk_box_append(GTK_BOX(box), gtk_label_new("Sending is paused while you edit. Attachments stay with each message."));
	rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_widget_set_margin_start(rows, 16);
	gtk_widget_set_margin_end(rows, 16);
	for (i = 0; i < n; i++) {
		GList *images = NULL;
		const gchar *text = ai_prompt_queue_get(queue, i, &images);
		g_autofree gchar *label = g_strdup_printf("Message %u%s", i + 1, images != NULL ? " · images attached" : "");
		GtkWidget *view = gtk_text_view_new();
		GtkWidget *remove = gtk_check_button_new_with_label("Remove this message");
		GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
		gtk_box_append(GTK_BOX(rows), gtk_label_new(label));
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
		gtk_text_buffer_set_text(buffer, text, -1);
		gtk_widget_set_size_request(view, -1, 100);
		gtk_accessible_update_property(GTK_ACCESSIBLE(view), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
		gtk_box_append(GTK_BOX(rows), view);
		gtk_box_append(GTK_BOX(rows), remove);
		g_ptr_array_add(editor->buffers, g_object_ref(buffer));
		g_ptr_array_add(editor->removed, remove);
	}
	if (n == 0) gtk_box_append(GTK_BOX(rows), gtk_label_new("No queued messages."));
	scroll = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), rows);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_box_append(GTK_BOX(box), scroll);
	editor->error = gtk_label_new("");
	gtk_label_set_wrap(GTK_LABEL(editor->error), TRUE);
	gtk_widget_add_css_class(editor->error, "error");
	gtk_box_append(GTK_BOX(box), editor->error);
	bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_halign(bar, GTK_ALIGN_END);
	gtk_widget_set_margin_end(bar, 16);
	gtk_widget_set_margin_bottom(bar, 16);
	cancel = gtk_button_new_with_label("Cancel");
	save = gtk_button_new_with_label("Save and resume");
	gtk_widget_add_css_class(save, "suggested-action");
	gtk_box_append(GTK_BOX(bar), cancel);
	gtk_box_append(GTK_BOX(bar), save);
	gtk_box_append(GTK_BOX(box), bar);
	g_signal_connect(cancel, "clicked", G_CALLBACK(queue_cancel), editor);
	g_signal_connect(save, "clicked", G_CALLBACK(queue_save), editor);
	g_signal_connect(editor->dialog, "closed", G_CALLBACK(queue_closed), editor);
	adw_dialog_set_child(editor->dialog, box);
	adw_dialog_present(editor->dialog, parent);
}
