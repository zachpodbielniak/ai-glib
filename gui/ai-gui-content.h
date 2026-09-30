/*
 * ai-gui-content.h - What a piece of content is, and how to carry it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Attachments, previewable files and fenced code: three questions the
 * window asks about bytes rather than about widgets, which is why they
 * are here and not in the toolkit layer. tests/test-ai-gui-content.c
 * links this directly, without a display.
 */

#pragma once

#include <gio/gio.h>

#include <ai-glib.h>

G_BEGIN_DECLS

/**
 * AI_GUI_CONTENT_MAX_IMAGE_BYTES:
 *
 * The encoded size one attachment may reach.
 *
 * The same five mebibytes ai-tui allows, because the limit is about
 * what a provider will accept and base64 expands by a third, not about
 * what a terminal can draw.
 */
#define AI_GUI_CONTENT_MAX_IMAGE_BYTES (5 * 1024 * 1024)

/**
 * AI_GUI_CONTENT_MAX_IMAGES:
 *
 * How many images one message may carry, matching ai-tui.
 */
#define AI_GUI_CONTENT_MAX_IMAGES (4)

/**
 * AI_GUI_CONTENT_MAX_TEXT_BYTES:
 *
 * How much of a file the preview reads.
 *
 * A preview is for looking at, so it is bounded and says when it has
 * been cut. Reading a gigabyte to show the first screen of it would
 * freeze the window at the moment somebody clicked something.
 */
#define AI_GUI_CONTENT_MAX_TEXT_BYTES (1024 * 1024)

/**
 * AiGuiContentKind:
 * @AI_GUI_CONTENT_UNKNOWN: nothing this window can show inline
 * @AI_GUI_CONTENT_IMAGE: something #GdkTexture will decode
 * @AI_GUI_CONTENT_TEXT: valid UTF-8 with no embedded NUL
 *
 * What a path can be shown as.
 */
typedef enum
{
	AI_GUI_CONTENT_UNKNOWN = 0,
	AI_GUI_CONTENT_IMAGE,
	AI_GUI_CONTENT_TEXT
} AiGuiContentKind;

/**
 * ai_gui_content_sniff_image:
 * @data: (array length=size) (element-type guint8): the first bytes
 * @size: how many
 *
 * Recognises an image by its signature.
 *
 * By signature and never by extension or by what a clipboard owner
 * claims: a `.png` that is actually HTML, pasted into a provider as an
 * image, fails somewhere far from here with a message about base64.
 *
 * Returns: (nullable): a static MIME string, or %NULL
 */
const gchar *
ai_gui_content_sniff_image(
	gconstpointer data,
	gsize         size
);

/**
 * ai_gui_content_image_from_bytes:
 * @bytes: the encoded image
 * @error: (out) (optional): why it was refused
 *
 * Wraps @bytes as a content block a conversation will accept.
 *
 * An #AiImageContent, not a bare #AiImage: the whole library pipeline
 * --- the prompt queue, the message, the provider serialisers --- takes
 * the content block and reference-counts it. Handing it the boxed
 * payload instead type-punned a #GObject and was the reason attaching
 * anything at all did not work.
 *
 * Returns: (transfer full) (nullable): the block
 */
AiImageContent *
ai_gui_content_image_from_bytes(
	GBytes  *bytes,
	GError **error
);

/**
 * ai_gui_content_image_from_file:
 * @path: a file to attach
 * @error: (out) (optional): why it was refused
 *
 * Returns: (transfer full) (nullable): the block
 */
AiImageContent *
ai_gui_content_image_from_file(
	const gchar  *path,
	GError      **error
);

/**
 * ai_gui_content_classify:
 * @path: (nullable): a file
 * @out_mime: (out) (optional) (transfer none) (nullable): its image MIME
 *
 * Returns: what @path can be previewed as
 */
AiGuiContentKind
ai_gui_content_classify(
	const gchar  *path,
	const gchar **out_mime
);

/**
 * ai_gui_content_read_text:
 * @path: a file
 * @out_truncated: (out) (optional): whether the file went on
 * @error: (out) (optional): where a read failure goes
 *
 * Reads at most %AI_GUI_CONTENT_MAX_TEXT_BYTES, never splitting a UTF-8
 * character at the cut.
 *
 * Returns: (transfer full) (nullable): the text
 */
gchar *
ai_gui_content_read_text(
	const gchar  *path,
	gboolean     *out_truncated,
	GError      **error
);

/**
 * ai_gui_content_resolve_path:
 * @base: (nullable): the directory a relative reference is relative to
 * @reference: text from the transcript
 *
 * Turns a run of styled text into a file that exists, or nothing.
 *
 * The transcript marks `@mentions` and tool targets, and both arrive
 * decorated: a leading `@`, quotes around a path with a space, a
 * trailing comma from the sentence it sat in. Stripping those here is
 * what lets one click handler serve both.
 *
 * Returns: (transfer full) (nullable): an existing path
 */
gchar *
ai_gui_content_resolve_path(
	const gchar *base,
	const gchar *reference
);

/**
 * ai_gui_content_code_blocks:
 * @markdown: (nullable): a block's source text
 *
 * Extracts fenced code blocks, without their fences.
 *
 * Used for the copy action and nothing else. Rendering still goes
 * through the view layer, so a fence this misreads costs a clipboard
 * button and never the transcript.
 *
 * Returns: (transfer full) (array zero-terminated=1): the blocks
 */
gchar **
ai_gui_content_code_blocks(const gchar *markdown);

/**
 * ai_gui_content_span_at:
 * @rendered: a block's rendering
 * @index: a byte offset into its text
 * @out_tag: (out) (optional): the span's style role
 *
 * The styled run covering @index, when it names something openable.
 *
 * Which runs those are is the view layer's decision, not a guess: a
 * tool target is the file a call acted on, a mention is a file somebody
 * referred to, and a link is a URL. Re-deriving "this looks like a
 * path" from the prose around it would be inventing an answer the
 * library already has.
 *
 * Returns: (transfer full) (nullable): the run's text, or %NULL
 */
gchar *
ai_gui_content_span_at(
	AiRenderedText *rendered,
	guint           index,
	AiStyleTag     *out_tag
);

/**
 * ai_gui_content_attach_images:
 * @object: a transcript block
 * @images: (element-type AiImageContent) (nullable): what was sent with it
 *
 * Remembers the attachments a turn carried.
 *
 * The transcript records `[Images attached]` and deliberately not the
 * bytes --- a view block is text and spans, and embedding a megabyte of
 * PNG in one would have to survive every export and every save. The
 * window keeps them beside the block instead, for as long as the block
 * lives, which is exactly how long a thumbnail is worth drawing.
 */
void
ai_gui_content_attach_images(
	GObject *object,
	GList   *images
);

/**
 * ai_gui_content_get_images:
 * @object: a transcript block
 *
 * Returns: (transfer none) (element-type AiImageContent) (nullable): the
 *   attachments, or %NULL
 */
GList *
ai_gui_content_get_images(GObject *object);

G_END_DECLS
