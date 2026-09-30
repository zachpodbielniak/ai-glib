/*
 * ai-gui-preview.h - Looking at what the conversation is talking about
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include "ai-gui.h"

G_BEGIN_DECLS

/**
 * ai_gui_preview_present_file:
 * @parent: the widget to present over
 * @path: an existing file
 *
 * Shows a file without leaving the conversation.
 *
 * Images are drawn, text is shown in a monospace view, and anything
 * else offers to hand the file to the desktop. Deciding by the bytes
 * rather than by the extension matters here more than anywhere: a
 * transcript talks about files somebody else named.
 */
void
ai_gui_preview_present_file(
	GtkWidget   *parent,
	const gchar *path
);

/**
 * ai_gui_preview_present_image:
 * @parent: the widget to present over
 * @image: (transfer none): the attachment
 * @name: (nullable): what to call it
 *
 * Shows an image that has no file behind it — one pasted from the
 * clipboard, or one read back out of a message.
 */
void
ai_gui_preview_present_image(
	GtkWidget      *parent,
	AiImageContent *image,
	const gchar    *name
);

/**
 * ai_gui_preview_thumbnail:
 * @image: (transfer none): the attachment
 * @size: the longest edge, in pixels
 *
 * Returns: (transfer full) (nullable): a widget showing @image, or %NULL
 *   if the bytes will not decode
 */
GtkWidget *
ai_gui_preview_thumbnail(
	AiImageContent *image,
	gint            size
);

G_END_DECLS
