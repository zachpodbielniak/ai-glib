/*
 * test-ai-gui-content.c - Attachments, previewable files, fenced code
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The attachment case is a regression test with a history: the window
 * built bare #AiImage payloads and handed them to a library that
 * reference-counts #AiImageContent, so every path through the prompt
 * queue and the conversation type-punned a #GObject. Attaching anything
 * at all did not work. The assertions here are about the *type* the
 * library accepts, not about pixels.
 *
 * No display and no toolkit.
 */

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <ai-glib.h>

#include "ai-gui-content.h"

/* A PNG signature and enough filler to look like a file. Nothing here
 * decodes an image; the sniffer reads the signature and the library
 * carries the bytes. */
static const guint8 PNG_HEADER[] = {
	0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
	0x00, 0x00, 0x00, 0x0d, 'I', 'H', 'D', 'R'
};

static GBytes *
fake_png(gsize size)
{
	guint8 *data = g_malloc0(size);

	g_assert_cmpuint(size, >=, sizeof PNG_HEADER);
	memcpy(data, PNG_HEADER, sizeof PNG_HEADER);

	return g_bytes_new_take(data, size);
}

/* ================================================================
 * Recognising bytes
 * ================================================================ */

static void
test_sniff(void)
{
	static const guint8 JPEG[] = { 0xff, 0xd8, 0xff, 0xe0 };
	static const guint8 GIF[] = { 'G', 'I', 'F', '8', '9', 'a' };
	static const guint8 WEBP[] = {
		'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'
	};
	static const guint8 HTML[] = { '<', 'h', 't', 'm', 'l', '>' };

	g_assert_cmpstr(ai_gui_content_sniff_image(PNG_HEADER,
		sizeof PNG_HEADER), ==, "image/png");
	g_assert_cmpstr(ai_gui_content_sniff_image(JPEG, sizeof JPEG), ==,
	                "image/jpeg");
	g_assert_cmpstr(ai_gui_content_sniff_image(GIF, sizeof GIF), ==,
	                "image/gif");
	g_assert_cmpstr(ai_gui_content_sniff_image(WEBP, sizeof WEBP), ==,
	                "image/webp");

	/*
	 * A file named .png that is really HTML fails somewhere far from
	 * here, with a message about base64, if the extension is trusted.
	 */
	g_assert_null(ai_gui_content_sniff_image(HTML, sizeof HTML));

	/* A truncated signature is not a match. */
	g_assert_null(ai_gui_content_sniff_image(PNG_HEADER, 4));
	g_assert_null(ai_gui_content_sniff_image(NULL, 0));
}

/*
 * The type the library accepts.
 *
 * ai_conversation_send_images_async() checks AI_IS_IMAGE_CONTENT and
 * ai_prompt_queue_push() calls g_object_ref. Both were being handed a
 * boxed AiImage.
 */
static void
test_attachment_is_a_content_block(void)
{
	g_autoptr(GBytes) bytes = fake_png(64);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiImageContent) image = NULL;
	AiImage *payload;

	image = ai_gui_content_image_from_bytes(bytes, &error);
	g_assert_no_error(error);
	g_assert_nonnull(image);
	g_assert_true(AI_IS_IMAGE_CONTENT(image));

	payload = ai_image_content_get_image(image);
	g_assert_nonnull(payload);
	g_assert_cmpstr(ai_image_get_mime_type(payload), ==, "image/png");
	g_assert_cmpuint(ai_image_get_size(payload), ==, 64);
}

/*
 * Through the queue and back, which is the path that reference-counted
 * a pointer that was not a GObject.
 */
static void
test_attachment_survives_the_prompt_queue(void)
{
	g_autoptr(AiPromptQueue) queue = ai_prompt_queue_new();
	g_autoptr(GBytes) bytes = fake_png(32);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiImageContent) image = NULL;
	g_autofree gchar *text = NULL;
	GList *images = NULL;
	GList *popped = NULL;

	image = ai_gui_content_image_from_bytes(bytes, &error);
	g_assert_no_error(error);

	images = g_list_append(NULL, image);
	g_assert_true(ai_prompt_queue_push(queue, "look at this", images, FALSE,
	                                   &error));
	g_assert_no_error(error);
	g_list_free(images);

	text = ai_prompt_queue_pop(queue, &popped);
	g_assert_cmpstr(text, ==, "look at this");
	g_assert_cmpuint(g_list_length(popped), ==, 1);
	g_assert_true(AI_IS_IMAGE_CONTENT(popped->data));

	g_list_free_full(popped, g_object_unref);
}

static void
test_attachment_limits(void)
{
	g_autoptr(GBytes) empty = g_bytes_new_static("", 0);
	g_autoptr(GBytes) huge = fake_png(AI_GUI_CONTENT_MAX_IMAGE_BYTES + 1);
	g_autoptr(GBytes) text = g_bytes_new_static("not an image", 12);
	g_autoptr(GError) error = NULL;

	g_assert_null(ai_gui_content_image_from_bytes(empty, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	/* The message has to name the limit, or it is just a refusal. */
	g_assert_null(ai_gui_content_image_from_bytes(huge, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
	g_assert_nonnull(strstr(error->message, "5 MiB"));
	g_clear_error(&error);

	g_assert_null(ai_gui_content_image_from_bytes(text, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	g_clear_error(&error);
}

/* ================================================================
 * Files
 * ================================================================ */

typedef struct
{
	gchar *root;
} Fixture;

static void
fixture_set_up(
	Fixture       *fixture,
	gconstpointer  data
){
	fixture->root = g_dir_make_tmp("ai-gui-content-XXXXXX", NULL);
	g_assert_nonnull(fixture->root);
}

static void
fixture_tear_down(
	Fixture       *fixture,
	gconstpointer  data
){
	g_free(fixture->root);
}

static gchar *
fixture_write(
	Fixture       *fixture,
	const gchar   *name,
	gconstpointer  data,
	gssize         size
){
	gchar *path = g_build_filename(fixture->root, name, NULL);

	g_assert_true(g_file_set_contents(path, data, size, NULL));

	return path;
}

static void
test_classify(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autofree gchar *image = fixture_write(fixture, "shot.png",
		(const gchar *)PNG_HEADER, sizeof PNG_HEADER);
	g_autofree gchar *text = fixture_write(fixture, "notes.txt",
		"hello — héllo\n", -1);
	g_autofree gchar *binary = fixture_write(fixture, "blob.bin",
		"ab\0cd", 5);
	g_autofree gchar *missing =
		g_build_filename(fixture->root, "nope", NULL);
	const gchar *mime = NULL;

	g_assert_cmpint(ai_gui_content_classify(image, &mime), ==,
	                AI_GUI_CONTENT_IMAGE);
	g_assert_cmpstr(mime, ==, "image/png");

	g_assert_cmpint(ai_gui_content_classify(text, NULL), ==,
	                AI_GUI_CONTENT_TEXT);

	/*
	 * An embedded NUL is what says "not text": it is legal in a file
	 * and impossible in the strings every widget downstream assumes, so
	 * a preview would truncate silently at the first one.
	 */
	g_assert_cmpint(ai_gui_content_classify(binary, NULL), ==,
	                AI_GUI_CONTENT_UNKNOWN);

	g_assert_cmpint(ai_gui_content_classify(missing, NULL), ==,
	                AI_GUI_CONTENT_UNKNOWN);
	g_assert_cmpint(ai_gui_content_classify(fixture->root, NULL), ==,
	                AI_GUI_CONTENT_UNKNOWN);
	g_assert_cmpint(ai_gui_content_classify(NULL, NULL), ==,
	                AI_GUI_CONTENT_UNKNOWN);
}

static void
test_read_text_is_bounded(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autofree gchar *small = fixture_write(fixture, "small.txt", "abc\n", -1);
	g_autofree gchar *shown = NULL;
	g_autofree gchar *big_path = NULL;
	g_autofree gchar *big = NULL;
	gboolean truncated = TRUE;

	shown = ai_gui_content_read_text(small, &truncated, NULL);
	g_assert_cmpstr(shown, ==, "abc\n");
	g_assert_false(truncated);
	g_clear_pointer(&shown, g_free);

	/*
	 * Past the limit the read stops and says so, and never in the middle
	 * of a character: a label handed invalid UTF-8 draws replacement
	 * glyphs where the last line should be.
	 */
	{
		g_autoptr(GString) content = g_string_new(NULL);

		while (content->len < AI_GUI_CONTENT_MAX_TEXT_BYTES + 64)
			g_string_append(content, "héllo wörld ");

		big = g_strdup(content->str);
		big_path = fixture_write(fixture, "big.txt", big, -1);
	}

	shown = ai_gui_content_read_text(big_path, &truncated, NULL);
	g_assert_nonnull(shown);
	g_assert_true(truncated);
	g_assert_cmpuint(strlen(shown), <=, AI_GUI_CONTENT_MAX_TEXT_BYTES);
	g_assert_true(g_utf8_validate(shown, -1, NULL));
}

/*
 * The transcript decorates what it marks: a leading `@`, quotes around a
 * path with a space, a comma from the sentence it sat in. One click
 * handler serves mentions and tool targets because this strips all of
 * them.
 */
static void
test_resolve_path(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autofree gchar *file = fixture_write(fixture, "notes.txt", "x", -1);
	g_autofree gchar *spaced = fixture_write(fixture, "two words.txt", "x", -1);
	g_autofree gchar *resolved = NULL;
	g_autofree gchar *canonical = g_canonicalize_filename(file, NULL);

	resolved = ai_gui_content_resolve_path(fixture->root, "notes.txt");
	g_assert_cmpstr(resolved, ==, canonical);
	g_clear_pointer(&resolved, g_free);

	resolved = ai_gui_content_resolve_path(NULL, file);
	g_assert_cmpstr(resolved, ==, canonical);
	g_clear_pointer(&resolved, g_free);

	resolved = ai_gui_content_resolve_path(fixture->root, "@notes.txt");
	g_assert_cmpstr(resolved, ==, canonical);
	g_clear_pointer(&resolved, g_free);

	resolved = ai_gui_content_resolve_path(fixture->root, "notes.txt,");
	g_assert_cmpstr(resolved, ==, canonical);
	g_clear_pointer(&resolved, g_free);

	resolved = ai_gui_content_resolve_path(fixture->root,
	                                       "@\"two words.txt\"");
	g_assert_nonnull(resolved);
	g_assert_true(g_str_has_suffix(resolved, "two words.txt"));
	g_clear_pointer(&resolved, g_free);

	/* Nothing that is not a file it can show. */
	g_assert_null(ai_gui_content_resolve_path(fixture->root, "absent.txt"));
	g_assert_null(ai_gui_content_resolve_path(fixture->root, fixture->root));
	g_assert_null(ai_gui_content_resolve_path(fixture->root, ""));
	g_assert_null(ai_gui_content_resolve_path(fixture->root, NULL));
	g_assert_null(ai_gui_content_resolve_path(fixture->root, "@"));

	(void)spaced;
}

/* ================================================================
 * Fenced code
 * ================================================================ */

static void
test_code_blocks(void)
{
	g_auto(GStrv) none = ai_gui_content_code_blocks("just prose\n");
	g_auto(GStrv) one = NULL;
	g_auto(GStrv) two = NULL;
	g_auto(GStrv) open = NULL;
	g_auto(GStrv) tilde = NULL;
	g_auto(GStrv) empty = ai_gui_content_code_blocks(NULL);

	g_assert_null(none[0]);
	g_assert_null(empty[0]);

	one = ai_gui_content_code_blocks(
		"Try this:\n```c\nint main(void);\n```\nDone.\n");
	g_assert_cmpstr(one[0], ==, "int main(void);\n");
	g_assert_null(one[1]);

	two = ai_gui_content_code_blocks(
		"```\nfirst\n```\nprose\n```sh\nsecond\n```\n");
	g_assert_cmpstr(two[0], ==, "first\n");
	g_assert_cmpstr(two[1], ==, "second\n");
	g_assert_null(two[2]);

	/*
	 * A fence that never closed is still the code somebody wants --
	 * which is the state every streaming answer is in while it arrives.
	 */
	open = ai_gui_content_code_blocks("```py\nstill typing\n");
	g_assert_cmpstr(open[0], ==, "still typing\n");
	g_assert_null(open[1]);

	tilde = ai_gui_content_code_blocks("~~~\ntilde fence\n~~~\n");
	g_assert_cmpstr(tilde[0], ==, "tilde fence\n");
	g_assert_null(tilde[1]);
}

/* ================================================================
 * Which run of bytes was clicked
 * ================================================================ */

/*
 * The half of click-to-preview that is not a Pango hit test.
 *
 * A window is needed to turn a pointer into a byte offset; everything
 * after that -- which span covers it, and whether that span names
 * something openable -- is this, and it is where the mistakes live.
 */
static void
test_span_at(void)
{
	g_autoptr(AiRenderedText) rendered = ai_rendered_text_new();
	g_autofree gchar *hit = NULL;
	AiStyleTag tag = AI_STYLE_DEFAULT;
	guint prose;
	guint mention;
	guint after;

	ai_rendered_text_append(rendered, "look at ", AI_STYLE_DEFAULT);
	prose = 2;
	mention = ai_rendered_text_get_length(rendered) + 1;
	ai_rendered_text_append(rendered, "@src/main.c", AI_STYLE_MENTION);
	after = ai_rendered_text_get_length(rendered) + 2;
	ai_rendered_text_append(rendered, " please", AI_STYLE_DEFAULT);

	hit = ai_gui_content_span_at(rendered, mention, &tag);
	g_assert_cmpstr(hit, ==, "@src/main.c");
	g_assert_cmpint(tag, ==, AI_STYLE_MENTION);
	g_clear_pointer(&hit, g_free);

	/* Ordinary prose is not a near miss for the path further along the
	 * line: it is nothing. */
	g_assert_null(ai_gui_content_span_at(rendered, prose, NULL));
	g_assert_null(ai_gui_content_span_at(rendered, after, NULL));

	/* Past the end is not a crash. */
	g_assert_null(ai_gui_content_span_at(rendered,
		ai_rendered_text_get_length(rendered), NULL));
	g_assert_null(ai_gui_content_span_at(rendered, 99999, NULL));
}

static void
test_span_at_openable_tags(void)
{
	static const AiStyleTag OPENABLE[] = {
		AI_STYLE_TOOL_TARGET, AI_STYLE_MENTION, AI_STYLE_LINK
	};
	static const AiStyleTag PLAIN[] = {
		AI_STYLE_DEFAULT, AI_STYLE_CODE, AI_STYLE_HEADING,
		AI_STYLE_TOOL_NAME, AI_STYLE_ERROR
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(OPENABLE); i++)
	{
		g_autoptr(AiRenderedText) rendered = ai_rendered_text_new();
		g_autofree gchar *hit = NULL;

		ai_rendered_text_append(rendered, "thing", OPENABLE[i]);
		hit = ai_gui_content_span_at(rendered, 1, NULL);
		g_assert_cmpstr(hit, ==, "thing");
	}

	/*
	 * A tool *name* is not a file and a code run is not a path.
	 * Offering to open either would be a click that does nothing, or
	 * worse, opens something unrelated that happens to exist.
	 */
	for (i = 0; i < G_N_ELEMENTS(PLAIN); i++)
	{
		g_autoptr(AiRenderedText) rendered = ai_rendered_text_new();

		ai_rendered_text_append(rendered, "thing", PLAIN[i]);
		g_assert_null(ai_gui_content_span_at(rendered, 1, NULL));
	}
}

/* ================================================================
 * Attachments, remembered beside their block
 * ================================================================ */

static void
test_images_ride_with_their_block(void)
{
	g_autoptr(AiViewBlock) turn = ai_view_turn_block_new("look at this");
	g_autoptr(GBytes) bytes = fake_png(32);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiImageContent) image = NULL;
	GList *images = NULL;
	GList *read_back;

	g_assert_null(ai_gui_content_get_images(G_OBJECT(turn)));

	image = ai_gui_content_image_from_bytes(bytes, &error);
	g_assert_no_error(error);
	images = g_list_append(NULL, image);

	ai_gui_content_attach_images(G_OBJECT(turn), images);

	/*
	 * The block took its own reference: the caller's list is about to be
	 * handed to the conversation, which takes references too, and then
	 * freed.
	 */
	g_list_free(images);

	read_back = ai_gui_content_get_images(G_OBJECT(turn));
	g_assert_cmpuint(g_list_length(read_back), ==, 1);
	g_assert_true(AI_IS_IMAGE_CONTENT(read_back->data));
	g_assert_true(read_back->data == (gpointer)image);
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-gui/content/sniff", test_sniff);
	g_test_add_func("/ai-gui/content/attachment-is-a-content-block",
	                test_attachment_is_a_content_block);
	g_test_add_func("/ai-gui/content/attachment-survives-the-prompt-queue",
	                test_attachment_survives_the_prompt_queue);
	g_test_add_func("/ai-gui/content/attachment-limits", test_attachment_limits);
	g_test_add_func("/ai-gui/content/code-blocks", test_code_blocks);
	g_test_add_func("/ai-gui/content/span-at", test_span_at);
	g_test_add_func("/ai-gui/content/span-at-openable-tags",
	                test_span_at_openable_tags);
	g_test_add_func("/ai-gui/content/images-ride-with-their-block",
	                test_images_ride_with_their_block);

	g_test_add("/ai-gui/content/classify", Fixture, NULL,
	           fixture_set_up, test_classify, fixture_tear_down);
	g_test_add("/ai-gui/content/read-text-is-bounded", Fixture, NULL,
	           fixture_set_up, test_read_text_is_bounded, fixture_tear_down);
	g_test_add("/ai-gui/content/resolve-path", Fixture, NULL,
	           fixture_set_up, test_resolve_path, fixture_tear_down);

	return g_test_run();
}
