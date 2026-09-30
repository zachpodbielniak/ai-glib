/*
 * ai-gui-content.c - What a piece of content is, and how to carry it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include <glib/gstdio.h>

#include "ai-gui-content.h"

/* How much of a file is enough to tell an image from a text file from a
 * binary. Every signature below fits inside it. */
#define AI_GUI_CONTENT_SNIFF_BYTES (512)

const gchar *
ai_gui_content_sniff_image(
	gconstpointer data,
	gsize         size
){
	const guint8 *bytes = data;

	if (bytes == NULL)
		return NULL;

	if (size >= 8 && memcmp(bytes, "\211PNG\r\n\032\n", 8) == 0)
		return "image/png";

	if (size >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
		return "image/jpeg";

	if (size >= 6 && (memcmp(bytes, "GIF87a", 6) == 0 ||
	                  memcmp(bytes, "GIF89a", 6) == 0))
	{
		return "image/gif";
	}

	if (size >= 12 && memcmp(bytes, "RIFF", 4) == 0 &&
	    memcmp(bytes + 8, "WEBP", 4) == 0)
	{
		return "image/webp";
	}

	return NULL;
}

AiImageContent *
ai_gui_content_image_from_bytes(
	GBytes  *bytes,
	GError **error
){
	const gchar *mime;
	gconstpointer data;
	gsize size = 0;

	g_return_val_if_fail(bytes != NULL, NULL);

	data = g_bytes_get_data(bytes, &size);

	if (size == 0)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		                    "That file is empty.");
		return NULL;
	}

	if (size > AI_GUI_CONTENT_MAX_IMAGE_BYTES)
	{
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			"That image is %.1f MiB; attachments are limited to %d MiB.",
			(gdouble)size / (1024.0 * 1024.0),
			AI_GUI_CONTENT_MAX_IMAGE_BYTES / (1024 * 1024));
		return NULL;
	}

	mime = ai_gui_content_sniff_image(data, size);

	if (mime == NULL)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			"Not a PNG, JPEG, GIF or WebP image.");
		return NULL;
	}

	return ai_image_content_new_from_bytes(bytes, mime);
}

AiImageContent *
ai_gui_content_image_from_file(
	const gchar  *path,
	GError      **error
){
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *data = NULL;
	gsize size = 0;
	GStatBuf info;

	g_return_val_if_fail(path != NULL, NULL);

	/*
	 * Bounded before it is read, not after. g_file_get_contents() on a
	 * named pipe or a very large file would have committed the memory
	 * before anybody could refuse it.
	 */
	if (g_stat(path, &info) != 0 || !S_ISREG(info.st_mode))
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
		                    "Not a regular file.");
		return NULL;
	}

	if (info.st_size > AI_GUI_CONTENT_MAX_IMAGE_BYTES)
	{
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
			"That image is %.1f MiB; attachments are limited to %d MiB.",
			(gdouble)info.st_size / (1024.0 * 1024.0),
			AI_GUI_CONTENT_MAX_IMAGE_BYTES / (1024 * 1024));
		return NULL;
	}

	if (!g_file_get_contents(path, &data, &size, error))
		return NULL;

	bytes = g_bytes_new_take(g_steal_pointer(&data), size);

	return ai_gui_content_image_from_bytes(bytes, error);
}

AiGuiContentKind
ai_gui_content_classify(
	const gchar  *path,
	const gchar **out_mime
){
	gchar buffer[AI_GUI_CONTENT_SNIFF_BYTES];
	const gchar *mime;
	GStatBuf info;
	FILE *handle;
	gsize read_bytes;

	if (out_mime != NULL)
		*out_mime = NULL;

	if (path == NULL || g_stat(path, &info) != 0 || !S_ISREG(info.st_mode))
		return AI_GUI_CONTENT_UNKNOWN;

	handle = g_fopen(path, "rb");

	if (handle == NULL)
		return AI_GUI_CONTENT_UNKNOWN;

	read_bytes = fread(buffer, 1, sizeof buffer, handle);
	fclose(handle);

	if (read_bytes == 0)
		return AI_GUI_CONTENT_UNKNOWN;

	mime = ai_gui_content_sniff_image(buffer, read_bytes);

	if (mime != NULL)
	{
		if (out_mime != NULL)
			*out_mime = mime;

		return AI_GUI_CONTENT_IMAGE;
	}

	/*
	 * Text is decided by the bytes too. An embedded NUL is the tell: it
	 * is legal in a file and impossible in the strings every widget
	 * downstream assumes, so showing one would truncate the preview
	 * silently at the first one.
	 */
	if (memchr(buffer, '\0', read_bytes) != NULL)
		return AI_GUI_CONTENT_UNKNOWN;

	{
		const gchar *end = NULL;

		if (g_utf8_validate_len(buffer, read_bytes, &end))
			return AI_GUI_CONTENT_TEXT;

		/*
		 * A prefix that ends mid-character is still text: the cut landed
		 * inside a multi-byte sequence, which says nothing about the
		 * file. Anything else invalid does.
		 */
		if (end != NULL && (gsize)(end - buffer) + 4 >= read_bytes &&
		    read_bytes == sizeof buffer)
		{
			return AI_GUI_CONTENT_TEXT;
		}
	}

	return AI_GUI_CONTENT_UNKNOWN;
}

gchar *
ai_gui_content_read_text(
	const gchar  *path,
	gboolean     *out_truncated,
	GError      **error
){
	g_autofree gchar *data = NULL;
	gsize size = 0;
	gsize keep;

	if (out_truncated != NULL)
		*out_truncated = FALSE;

	g_return_val_if_fail(path != NULL, NULL);

	if (!g_file_get_contents(path, &data, &size, error))
		return NULL;

	keep = MIN(size, (gsize)AI_GUI_CONTENT_MAX_TEXT_BYTES);

	if (keep < size && out_truncated != NULL)
		*out_truncated = TRUE;

	/*
	 * Never split a character at the cut. A label handed invalid UTF-8
	 * draws a run of replacement glyphs where the last line should be.
	 */
	while (keep > 0 && !g_utf8_validate_len(data, keep, NULL))
		keep--;

	return g_strndup(data, keep);
}

gchar *
ai_gui_content_resolve_path(
	const gchar *base,
	const gchar *reference
){
	g_autofree gchar *trimmed = NULL;
	g_autofree gchar *expanded = NULL;
	gchar *candidate;
	gsize length;

	if (reference == NULL || *reference == '\0')
		return NULL;

	trimmed = g_strdup(reference);
	g_strstrip(trimmed);

	if (*trimmed == '@')
		memmove(trimmed, trimmed + 1, strlen(trimmed));

	length = strlen(trimmed);

	/* Quotes around a path with a space, as the mention syntax writes
	 * them, and as a tool target echoes them back. */
	if (length >= 2 && (trimmed[0] == '"' || trimmed[0] == '\'') &&
	    trimmed[length - 1] == trimmed[0])
	{
		trimmed[length - 1] = '\0';
		memmove(trimmed, trimmed + 1, length - 1);
		length -= 2;
	}

	/*
	 * Trailing sentence punctuation. A path really ending in one of
	 * these still resolves, because the undecorated spelling is tried
	 * first and only a miss falls through to stripping.
	 */
	if (!g_file_test(trimmed, G_FILE_TEST_EXISTS))
	{
		while (length > 0 && strchr(".,;:)]}", trimmed[length - 1]) != NULL)
			trimmed[--length] = '\0';
	}

	if (*trimmed == '\0')
		return NULL;

	if (trimmed[0] == '~' && (trimmed[1] == '/' || trimmed[1] == '\0'))
	{
		expanded = g_build_filename(g_get_home_dir(),
		                            trimmed[1] != '\0' ? trimmed + 2 : "",
		                            NULL);
	}
	else if (g_path_is_absolute(trimmed))
	{
		expanded = g_strdup(trimmed);
	}
	else if (base != NULL)
	{
		expanded = g_build_filename(base, trimmed, NULL);
	}
	else
	{
		expanded = g_strdup(trimmed);
	}

	if (!g_file_test(expanded, G_FILE_TEST_IS_REGULAR))
		return NULL;

	candidate = g_canonicalize_filename(expanded, NULL);

	return candidate;
}

gchar **
ai_gui_content_code_blocks(const gchar *markdown)
{
	g_autoptr(GPtrArray) blocks = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) lines = NULL;
	g_autoptr(GString) current = NULL;
	guint i;

	if (markdown == NULL)
	{
		g_ptr_array_add(blocks, NULL);
		return (gchar **)g_ptr_array_free(g_steal_pointer(&blocks), FALSE);
	}

	lines = g_strsplit(markdown, "\n", -1);

	for (i = 0; lines[i] != NULL; i++)
	{
		const gchar *line = lines[i];
		const gchar *scan = line;

		/*
		 * g_strsplit() on text ending in a newline yields a final empty
		 * element that is not a line. Keeping it would add a blank line
		 * to every block whose source ends the way all of them do.
		 */
		if (lines[i + 1] == NULL && *line == '\0')
			continue;

		/* Up to three spaces of indent, as CommonMark allows. */
		while (*scan == ' ' && scan - line < 3)
			scan++;

		if (g_str_has_prefix(scan, "```") || g_str_has_prefix(scan, "~~~"))
		{
			if (current == NULL)
			{
				current = g_string_new(NULL);
			}
			else
			{
				g_ptr_array_add(blocks, g_string_free(
					g_steal_pointer(&current), FALSE));
			}

			continue;
		}

		if (current != NULL)
		{
			g_string_append(current, line);
			g_string_append_c(current, '\n');
		}
	}

	/*
	 * A fence that never closed is still the code somebody wants to
	 * copy --- which is exactly the state a streaming answer is in
	 * while it is still arriving.
	 */
	if (current != NULL && current->len > 0)
		g_ptr_array_add(blocks, g_string_free(g_steal_pointer(&current), FALSE));

	g_ptr_array_add(blocks, NULL);

	return (gchar **)g_ptr_array_free(g_steal_pointer(&blocks), FALSE);
}

/* ================================================================
 * Which run of bytes was clicked
 * ================================================================ */

static gboolean
content_tag_is_openable(AiStyleTag tag)
{
	return tag == AI_STYLE_TOOL_TARGET || tag == AI_STYLE_MENTION ||
	       tag == AI_STYLE_LINK;
}

gchar *
ai_gui_content_span_at(
	AiRenderedText *rendered,
	guint           index,
	AiStyleTag     *out_tag
){
	const gchar *text;
	guint n_spans;
	guint i;

	g_return_val_if_fail(rendered != NULL, NULL);

	text = ai_rendered_text_get_text(rendered);
	n_spans = ai_rendered_text_get_n_spans(rendered);

	if (text == NULL || index >= ai_rendered_text_get_length(rendered))
		return NULL;

	for (i = 0; i < n_spans; i++)
	{
		AiStyleTag tag = AI_STYLE_DEFAULT;
		guint start = 0;
		guint len = 0;

		if (!ai_rendered_text_get_span(rendered, i, &start, &len, &tag))
			continue;

		if (index < start || index >= start + len)
			continue;

		/*
		 * The first span covering the offset decides, openable or not.
		 * Spans do not overlap, so a later one cannot cover it too --
		 * and falling through to keep looking would let a click on
		 * ordinary prose find a path further along the line.
		 */
		if (!content_tag_is_openable(tag))
			return NULL;

		if (out_tag != NULL)
			*out_tag = tag;

		return g_strndup(text + start, len);
	}

	return NULL;
}

/* ================================================================
 * Attachments, remembered beside their block
 * ================================================================ */

#define AI_GUI_CONTENT_IMAGE_KEY "ai-gui-images"

static void
content_images_free(gpointer data)
{
	g_list_free_full(data, g_object_unref);
}

void
ai_gui_content_attach_images(
	GObject *object,
	GList   *images
){
	GList *copy = NULL;
	GList *iter;

	g_return_if_fail(G_IS_OBJECT(object));

	if (images == NULL)
		return;

	/*
	 * A reference each, held by the block. The caller's list is its own
	 * --- it is about to be handed to the conversation, which takes its
	 * own references too.
	 */
	for (iter = images; iter != NULL; iter = iter->next)
		copy = g_list_append(copy, g_object_ref(iter->data));

	g_object_set_data_full(object, AI_GUI_CONTENT_IMAGE_KEY, copy,
	                       content_images_free);
}

GList *
ai_gui_content_get_images(GObject *object)
{
	g_return_val_if_fail(G_IS_OBJECT(object), NULL);

	return g_object_get_data(object, AI_GUI_CONTENT_IMAGE_KEY);
}
