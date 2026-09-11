/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <ai-glib.h>
#include <string.h>

#define TUI_IMAGE_MAX_BYTES (5 * 1024 * 1024)
#define TUI_IMAGE_MAX_COUNT (4)

/* Clipboard payloads never pass through UTF-8 strings. Recognize the binary
 * signature rather than trusting a selection owner's MIME advertisement. */
static AiImageContent *
tui_image_from_bytes(
	GBytes *bytes,
	GError **error
){
	gsize size;
	const guint8 *data = g_bytes_get_data(bytes, &size);
	const gchar *mime = NULL;

	if (size > TUI_IMAGE_MAX_BYTES)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
		                    "Image exceeds the 5 MiB attachment limit");
		return NULL;
	}
	if (size >= 8 && memcmp(data, "\211PNG\r\n\032\n", 8) == 0)
		mime = "image/png";
	else if (size >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
		mime = "image/jpeg";
	else if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
		mime = "image/gif";
	else if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0)
		mime = "image/webp";
	if (mime == NULL)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                    "Clipboard does not contain a PNG, JPEG, GIF or WebP image");
		return NULL;
	}
	return ai_image_content_new_from_bytes(bytes, mime);
}

typedef struct
{
	GSubprocess *process;
	GByteArray *bytes;
	GSource *deadline;
	GError *error;
} TuiClipboard;

/* Sources retain their original context through a pointer, never an id. */
static void
tui_clipboard_free(gpointer data)
{
	TuiClipboard *clipboard = data;

	g_clear_object(&clipboard->process);
	g_byte_array_unref(clipboard->bytes);
	if (clipboard->deadline != NULL)
	{
		g_source_destroy(clipboard->deadline);
		g_source_unref(clipboard->deadline);
	}
	g_clear_error(&clipboard->error);
	g_free(clipboard);
}

static gboolean
tui_clipboard_timeout(gpointer data)
{
	GTask *task = data;
	TuiClipboard *clipboard = g_task_get_task_data(task);

	g_cancellable_cancel(g_task_get_cancellable(task));
	g_subprocess_force_exit(clipboard->process);
	return G_SOURCE_REMOVE;
}

/* Reap even on cancellation or oversized input before completing the task. */
static void
tui_clipboard_waited(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	TuiClipboard *clipboard = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	AiImageContent *image;

	g_subprocess_wait_finish(G_SUBPROCESS(source), result, &error);
	if (clipboard->error != NULL)
		g_task_return_error(task, g_steal_pointer(&clipboard->error));
	else if (error != NULL)
		g_task_return_error(task, g_steal_pointer(&error));
	else if (!g_subprocess_get_successful(clipboard->process))
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
		                       "Image clipboard read failed; copy an image and check wl-paste/xclip access to this desktop");
	else
	{
		bytes = g_bytes_new(clipboard->bytes->data, clipboard->bytes->len);
		image = tui_image_from_bytes(bytes, &error);
		if (image != NULL)
			g_task_return_pointer(task, image, g_object_unref);
		else
			g_task_return_error(task, g_steal_pointer(&error));
	}
	g_object_unref(task);
}

static void tui_clipboard_read(GTask *task);

/* Incremental reads cap memory even if a broken clipboard owner streams
 * forever. The deadline also bounds owners which never close the pipe. */
static void
tui_clipboard_read_done(GObject *source, GAsyncResult *result, gpointer data)
{
	GTask *task = data;
	TuiClipboard *clipboard = g_task_get_task_data(task);
	g_autoptr(GBytes) chunk = NULL;
	gsize size = 0;
	const guint8 *buffer;

	chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
	                                        &clipboard->error);
	buffer = chunk != NULL ? g_bytes_get_data(chunk, &size) : NULL;
	if (size > TUI_IMAGE_MAX_BYTES - clipboard->bytes->len)
		g_set_error_literal(&clipboard->error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
		                    "Image exceeds the 5 MiB attachment limit");
	if (clipboard->error == NULL && size > 0)
	{
		g_byte_array_append(clipboard->bytes, buffer, (guint)size);
		tui_clipboard_read(task);
		return;
	}
	if (clipboard->error != NULL)
		g_subprocess_force_exit(clipboard->process);
	g_subprocess_wait_async(clipboard->process, NULL, tui_clipboard_waited, task);
}

static void
tui_clipboard_read(GTask *task)
{
	TuiClipboard *clipboard = g_task_get_task_data(task);

	g_input_stream_read_bytes_async(g_subprocess_get_stdout_pipe(clipboard->process),
	                               65536, G_PRIORITY_DEFAULT,
	                               g_task_get_cancellable(task),
	                               tui_clipboard_read_done, task);
}

/* argv is explicit so tests can substitute a hermetic selection owner.
 * No shell, image files, display server or provider is involved in tests. */
static void
tui_clipboard_read_async(
	const gchar * const *argv,
	GCancellable *cancellable,
	guint timeout_ms,
	GAsyncReadyCallback callback,
	gpointer user_data
){
	GTask *task;
	TuiClipboard *clipboard;
	g_autoptr(GCancellable) token = cancellable != NULL ? g_object_ref(cancellable) : g_cancellable_new();
	g_autoptr(GError) error = NULL;

	task = g_task_new(NULL, token, callback, user_data);
	clipboard = g_new0(TuiClipboard, 1);
	clipboard->bytes = g_byte_array_new();
	g_task_set_task_data(task, clipboard, tui_clipboard_free);
	clipboard->process = g_subprocess_newv(argv,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
	if (clipboard->process == NULL)
	{
		g_task_return_error(task, g_steal_pointer(&error));
		g_object_unref(task);
		return;
	}
	clipboard->deadline = g_timeout_source_new(timeout_ms);
	g_source_set_callback(clipboard->deadline, tui_clipboard_timeout,
	                      task, NULL);
	g_source_attach(clipboard->deadline, g_task_get_context(task));
	tui_clipboard_read(task);
}
