/*
 * test-image-response.c - Unit tests for AiImageResponse and AiGeneratedImage
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "model/ai-generated-image.h"
#include "model/ai-image-response.h"

/* AiGeneratedImage tests */

static void
test_generated_image_new_from_url(void)
{
	g_autoptr(AiGeneratedImage) image = NULL;

	image = ai_generated_image_new_from_url("https://example.com/image.png");
	ai_generated_image_set_revised_prompt(image, "Revised: a cat in space");

	g_assert_nonnull(image);
	g_assert_cmpstr(ai_generated_image_get_url(image), ==, "https://example.com/image.png");
	g_assert_cmpstr(ai_generated_image_get_revised_prompt(image), ==, "Revised: a cat in space");
	g_assert_null(ai_generated_image_get_base64(image));
}

static void
test_generated_image_new_from_base64(void)
{
	g_autoptr(AiGeneratedImage) image = NULL;

	image = ai_generated_image_new_from_base64(
		"aGVsbG8gd29ybGQ=",  /* "hello world" in base64 */
		"image/png"
	);
	ai_generated_image_set_revised_prompt(image, "Revised prompt");

	g_assert_nonnull(image);
	g_assert_cmpstr(ai_generated_image_get_base64(image), ==, "aGVsbG8gd29ybGQ=");
	g_assert_cmpstr(ai_generated_image_get_mime_type(image), ==, "image/png");
	g_assert_cmpstr(ai_generated_image_get_revised_prompt(image), ==, "Revised prompt");
	g_assert_null(ai_generated_image_get_url(image));
}

static void
test_generated_image_get_bytes(void)
{
	g_autoptr(AiGeneratedImage) image = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	gconstpointer data;
	gsize size;

	/* "hello world" in base64 */
	image = ai_generated_image_new_from_base64(
		"aGVsbG8gd29ybGQ=",
		"image/png"
	);

	bytes = ai_generated_image_get_bytes(image, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bytes);

	data = g_bytes_get_data(bytes, &size);
	g_assert_cmpint(size, ==, 11);  /* "hello world" = 11 bytes */
	g_assert_cmpmem(data, size, "hello world", 11);
}

static void
test_generated_image_copy(void)
{
	g_autoptr(AiGeneratedImage) image = NULL;
	g_autoptr(AiGeneratedImage) copy = NULL;

	image = ai_generated_image_new_from_url("https://example.com/test.png");
	ai_generated_image_set_revised_prompt(image, "test prompt");

	copy = ai_generated_image_copy(image);

	g_assert_nonnull(copy);
	g_assert_true(image != copy);
	g_assert_cmpstr(ai_generated_image_get_url(copy), ==, "https://example.com/test.png");
	g_assert_cmpstr(ai_generated_image_get_revised_prompt(copy), ==, "test prompt");
}

static void
test_generated_image_gtype(void)
{
	GType type;

	type = ai_generated_image_get_type();
	g_assert_true(G_TYPE_IS_BOXED(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiGeneratedImage");
}

/* AiImageResponse tests */

static void
test_image_response_new(void)
{
	g_autoptr(AiImageResponse) response = NULL;

	response = ai_image_response_new("resp-123", 1704067200);

	g_assert_nonnull(response);
	g_assert_cmpstr(ai_image_response_get_id(response), ==, "resp-123");
	g_assert_cmpint(ai_image_response_get_created(response), ==, 1704067200);
}

static void
test_image_response_model(void)
{
	g_autoptr(AiImageResponse) response = NULL;

	response = ai_image_response_new("resp-123", 1704067200);

	g_assert_null(ai_image_response_get_model(response));

	ai_image_response_set_model(response, "dall-e-3");
	g_assert_cmpstr(ai_image_response_get_model(response), ==, "dall-e-3");
}

static void
test_image_response_images(void)
{
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(AiGeneratedImage) image1 = NULL;
	g_autoptr(AiGeneratedImage) image2 = NULL;
	GList *images;

	response = ai_image_response_new("resp-123", 1704067200);

	/* Initially no images */
	images = ai_image_response_get_images(response);
	g_assert_null(images);

	/* Add images */
	image1 = ai_generated_image_new_from_url("https://example.com/1.png");
	image2 = ai_generated_image_new_from_url("https://example.com/2.png");

	ai_image_response_add_image(response, g_steal_pointer(&image1));
	ai_image_response_add_image(response, g_steal_pointer(&image2));

	images = ai_image_response_get_images(response);
	g_assert_nonnull(images);
	g_assert_cmpint(g_list_length(images), ==, 2);

	/* Verify first image */
	g_assert_cmpstr(
		ai_generated_image_get_url((AiGeneratedImage *)images->data),
		==,
		"https://example.com/1.png"
	);
}

static void
test_image_response_copy(void)
{
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(AiImageResponse) copy = NULL;
	g_autoptr(AiGeneratedImage) image = NULL;
	GList *images;

	response = ai_image_response_new("resp-456", 1704067200);
	ai_image_response_set_model(response, "test-model");

	image = ai_generated_image_new_from_url("https://example.com/copy.png");
	ai_generated_image_set_revised_prompt(image, "copy test");
	ai_image_response_add_image(response, g_steal_pointer(&image));

	copy = ai_image_response_copy(response);

	g_assert_nonnull(copy);
	g_assert_true(response != copy);
	g_assert_cmpstr(ai_image_response_get_id(copy), ==, "resp-456");
	g_assert_cmpstr(ai_image_response_get_model(copy), ==, "test-model");

	images = ai_image_response_get_images(copy);
	g_assert_nonnull(images);
	g_assert_cmpint(g_list_length(images), ==, 1);
}

static void
test_image_response_gtype(void)
{
	GType type;

	type = ai_image_response_get_type();
	g_assert_true(G_TYPE_IS_BOXED(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiImageResponse");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	/* AiGeneratedImage tests */
	g_test_add_func("/ai-glib/generated-image/new-from-url", test_generated_image_new_from_url);
	g_test_add_func("/ai-glib/generated-image/new-from-base64", test_generated_image_new_from_base64);
	g_test_add_func("/ai-glib/generated-image/get-bytes", test_generated_image_get_bytes);
	g_test_add_func("/ai-glib/generated-image/copy", test_generated_image_copy);
	g_test_add_func("/ai-glib/generated-image/gtype", test_generated_image_gtype);

	/* AiImageResponse tests */
	g_test_add_func("/ai-glib/image-response/new", test_image_response_new);
	g_test_add_func("/ai-glib/image-response/model", test_image_response_model);
	g_test_add_func("/ai-glib/image-response/images", test_image_response_images);
	g_test_add_func("/ai-glib/image-response/copy", test_image_response_copy);
	g_test_add_func("/ai-glib/image-response/gtype", test_image_response_gtype);

	return g_test_run();
}
