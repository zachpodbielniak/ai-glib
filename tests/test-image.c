/*
 * test-image.c - Unit tests for the AiImage boxed type
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "model/ai-image.h"

/* A one-pixel PNG, so the content sniffer has something real to work on. */
static const guchar tiny_png[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
	0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
	0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
	0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41,
	0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
	0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00,
	0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
	0x42, 0x60, 0x82
};

static void
test_image_from_data(void)
{
	g_autoptr(AiImage) image = NULL;

	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");

	g_assert_nonnull(image);
	g_assert_cmpstr(ai_image_get_mime_type(image), ==, "image/png");
	g_assert_cmpuint(ai_image_get_size(image), ==, sizeof tiny_png);
	g_assert_nonnull(ai_image_get_bytes(image));
}

static void
test_image_mime_defaults_to_png(void)
{
	g_autoptr(AiImage) image = NULL;

	/* A NULL MIME type would serialise as a null JSON member and be
	 * rejected, so it must fall back rather than propagate. */
	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, NULL);

	g_assert_nonnull(image);
	g_assert_cmpstr(ai_image_get_mime_type(image), ==, "image/png");
}

static void
test_image_base64_roundtrip(void)
{
	g_autoptr(AiImage) image = NULL;
	g_autoptr(AiImage) decoded = NULL;
	g_autofree gchar *b64 = NULL;
	g_autofree gchar *b64_again = NULL;

	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");
	b64 = ai_image_dup_base64(image);

	g_assert_nonnull(b64);

	decoded = ai_image_new_from_base64(b64, "image/png");
	g_assert_nonnull(decoded);
	g_assert_cmpuint(ai_image_get_size(decoded), ==, sizeof tiny_png);

	b64_again = ai_image_dup_base64(decoded);
	g_assert_cmpstr(b64, ==, b64_again);
}

static void
test_image_base64_cached(void)
{
	g_autoptr(AiImage) image = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	/* Encoding is cached, but each call must still hand back an
	 * independently-owned copy. */
	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");

	first = ai_image_dup_base64(image);
	second = ai_image_dup_base64(image);

	g_assert_cmpstr(first, ==, second);
	g_assert_true(first != second);
}

static void
test_image_base64_rejects_garbage(void)
{
	AiImage *image;

	image = ai_image_new_from_base64("", "image/png");
	g_assert_null(image);
}

static void
test_image_from_file(void)
{
	g_autoptr(AiImage) image = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	gint fd;

	fd = g_file_open_tmp("ai-glib-test-XXXXXX.png", &path, &error);
	g_assert_no_error(error);
	g_assert_cmpint(fd, >=, 0);
	g_close(fd, NULL);

	g_file_set_contents(path, (const gchar *)tiny_png, sizeof tiny_png,
	                    &error);
	g_assert_no_error(error);

	image = ai_image_new_from_file(path, &error);
	g_assert_no_error(error);
	g_assert_nonnull(image);

	g_assert_cmpstr(ai_image_get_mime_type(image), ==, "image/png");
	g_assert_cmpuint(ai_image_get_size(image), ==, sizeof tiny_png);

	/* The basename is retained because multipart form parts need one, and
	 * OpenAI infers the image format from it. */
	g_assert_nonnull(ai_image_get_filename(image));
	g_assert_true(g_str_has_suffix(ai_image_get_filename(image), ".png"));
	g_assert_nonnull(ai_image_get_uri(image));

	g_unlink(path);
}

static void
test_image_from_file_missing(void)
{
	g_autoptr(GError) error = NULL;
	AiImage *image;

	image = ai_image_new_from_file("/nonexistent/nope.png", &error);

	g_assert_null(image);
	g_assert_nonnull(error);
}

static void
test_image_from_file_empty(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	AiImage *image;
	gint fd;

	fd = g_file_open_tmp("ai-glib-empty-XXXXXX.png", &path, &error);
	g_assert_no_error(error);
	g_close(fd, NULL);

	/* An empty file would serialise to an empty inline_data payload and
	 * come back as an opaque provider error; catch it locally instead. */
	image = ai_image_new_from_file(path, &error);

	g_assert_null(image);
	g_assert_nonnull(error);

	g_unlink(path);
}

static void
test_image_role_and_dimensions(void)
{
	g_autoptr(AiImage) image = NULL;

	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");

	g_assert_null(ai_image_get_role(image));
	g_assert_cmpint(ai_image_get_width(image), ==, 0);

	ai_image_set_role(image, "style");
	ai_image_set_dimensions(image, 1024, 768);

	g_assert_cmpstr(ai_image_get_role(image), ==, "style");
	g_assert_cmpint(ai_image_get_width(image), ==, 1024);
	g_assert_cmpint(ai_image_get_height(image), ==, 768);
}

static void
test_image_copy(void)
{
	g_autoptr(AiImage) image = NULL;
	g_autoptr(AiImage) copy = NULL;

	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/webp");
	ai_image_set_role(image, "subject");
	ai_image_set_filename(image, "subject.webp");
	ai_image_set_dimensions(image, 640, 480);

	copy = ai_image_copy(image);

	g_assert_nonnull(copy);
	g_assert_cmpstr(ai_image_get_mime_type(copy), ==, "image/webp");
	g_assert_cmpstr(ai_image_get_role(copy), ==, "subject");
	g_assert_cmpstr(ai_image_get_filename(copy), ==, "subject.webp");
	g_assert_cmpint(ai_image_get_width(copy), ==, 640);
	g_assert_cmpuint(ai_image_get_size(copy), ==, sizeof tiny_png);

	/* Mutating the copy must not disturb the original. */
	ai_image_set_role(copy, "background");
	g_assert_cmpstr(ai_image_get_role(image), ==, "subject");
}

static void
test_image_copy_null(void)
{
	g_assert_null(ai_image_copy(NULL));
}

static void
test_image_save_to_file(void)
{
	g_autoptr(AiImage) image = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	gsize length = 0;
	gint fd;

	fd = g_file_open_tmp("ai-glib-save-XXXXXX.png", &path, &error);
	g_assert_no_error(error);
	g_close(fd, NULL);

	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");

	g_assert_true(ai_image_save_to_file(image, path, &error));
	g_assert_no_error(error);

	g_file_get_contents(path, &contents, &length, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(length, ==, sizeof tiny_png);
	g_assert_cmpint(memcmp(contents, tiny_png, length), ==, 0);

	g_unlink(path);
}

static void
test_image_gtype(void)
{
	GType type;

	type = ai_image_get_type();
	g_assert_true(G_TYPE_IS_BOXED(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiImage");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/image/from-data", test_image_from_data);
	g_test_add_func("/ai-glib/image/mime-defaults", test_image_mime_defaults_to_png);
	g_test_add_func("/ai-glib/image/base64-roundtrip", test_image_base64_roundtrip);
	g_test_add_func("/ai-glib/image/base64-cached", test_image_base64_cached);
	g_test_add_func("/ai-glib/image/base64-garbage", test_image_base64_rejects_garbage);
	g_test_add_func("/ai-glib/image/from-file", test_image_from_file);
	g_test_add_func("/ai-glib/image/from-file-missing", test_image_from_file_missing);
	g_test_add_func("/ai-glib/image/from-file-empty", test_image_from_file_empty);
	g_test_add_func("/ai-glib/image/role-dimensions", test_image_role_and_dimensions);
	g_test_add_func("/ai-glib/image/copy", test_image_copy);
	g_test_add_func("/ai-glib/image/copy-null", test_image_copy_null);
	g_test_add_func("/ai-glib/image/save-to-file", test_image_save_to_file);
	g_test_add_func("/ai-glib/image/gtype", test_image_gtype);

	return g_test_run();
}
