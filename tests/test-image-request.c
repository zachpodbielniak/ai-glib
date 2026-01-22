/*
 * test-image-request.c - Unit tests for AiImageRequest
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "model/ai-image-request.h"

static void
test_image_request_new(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("a cat in space");
	g_assert_nonnull(request);
	g_assert_cmpstr(ai_image_request_get_prompt(request), ==, "a cat in space");
}

static void
test_image_request_model(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	/* Default model should be NULL */
	g_assert_null(ai_image_request_get_model(request));

	ai_image_request_set_model(request, "dall-e-3");
	g_assert_cmpstr(ai_image_request_get_model(request), ==, "dall-e-3");
}

static void
test_image_request_size(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	/* Default should be AUTO */
	g_assert_cmpint(ai_image_request_get_size(request), ==, AI_IMAGE_SIZE_AUTO);

	ai_image_request_set_size(request, AI_IMAGE_SIZE_1024);
	g_assert_cmpint(ai_image_request_get_size(request), ==, AI_IMAGE_SIZE_1024);

	ai_image_request_set_size(request, AI_IMAGE_SIZE_1024_1792);
	g_assert_cmpint(ai_image_request_get_size(request), ==, AI_IMAGE_SIZE_1024_1792);
}

static void
test_image_request_custom_size(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	g_assert_null(ai_image_request_get_custom_size(request));

	ai_image_request_set_custom_size(request, "800x600");
	g_assert_cmpstr(ai_image_request_get_custom_size(request), ==, "800x600");
	g_assert_cmpint(ai_image_request_get_size(request), ==, AI_IMAGE_SIZE_CUSTOM);
}

static void
test_image_request_quality(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	/* Default should be AUTO */
	g_assert_cmpint(ai_image_request_get_quality(request), ==, AI_IMAGE_QUALITY_AUTO);

	ai_image_request_set_quality(request, AI_IMAGE_QUALITY_HD);
	g_assert_cmpint(ai_image_request_get_quality(request), ==, AI_IMAGE_QUALITY_HD);
}

static void
test_image_request_style(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	/* Default should be AUTO */
	g_assert_cmpint(ai_image_request_get_style(request), ==, AI_IMAGE_STYLE_AUTO);

	ai_image_request_set_style(request, AI_IMAGE_STYLE_VIVID);
	g_assert_cmpint(ai_image_request_get_style(request), ==, AI_IMAGE_STYLE_VIVID);
}

static void
test_image_request_count(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	/* Default should be 1 */
	g_assert_cmpint(ai_image_request_get_count(request), ==, 1);

	ai_image_request_set_count(request, 4);
	g_assert_cmpint(ai_image_request_get_count(request), ==, 4);
}

static void
test_image_request_response_format(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	/* Default should be URL */
	g_assert_cmpint(ai_image_request_get_response_format(request), ==, AI_IMAGE_RESPONSE_URL);

	ai_image_request_set_response_format(request, AI_IMAGE_RESPONSE_BASE64);
	g_assert_cmpint(ai_image_request_get_response_format(request), ==, AI_IMAGE_RESPONSE_BASE64);
}

static void
test_image_request_user(void)
{
	g_autoptr(AiImageRequest) request = NULL;

	request = ai_image_request_new("test prompt");

	g_assert_null(ai_image_request_get_user(request));

	ai_image_request_set_user(request, "user-123");
	g_assert_cmpstr(ai_image_request_get_user(request), ==, "user-123");
}

static void
test_image_request_copy(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageRequest) copy = NULL;

	request = ai_image_request_new("copy test");
	ai_image_request_set_model(request, "test-model");
	ai_image_request_set_size(request, AI_IMAGE_SIZE_512);
	ai_image_request_set_quality(request, AI_IMAGE_QUALITY_HD);
	ai_image_request_set_style(request, AI_IMAGE_STYLE_NATURAL);
	ai_image_request_set_count(request, 2);

	copy = ai_image_request_copy(request);

	g_assert_nonnull(copy);
	g_assert_true(request != copy);
	g_assert_cmpstr(ai_image_request_get_prompt(copy), ==, "copy test");
	g_assert_cmpstr(ai_image_request_get_model(copy), ==, "test-model");
	g_assert_cmpint(ai_image_request_get_size(copy), ==, AI_IMAGE_SIZE_512);
	g_assert_cmpint(ai_image_request_get_quality(copy), ==, AI_IMAGE_QUALITY_HD);
	g_assert_cmpint(ai_image_request_get_style(copy), ==, AI_IMAGE_STYLE_NATURAL);
	g_assert_cmpint(ai_image_request_get_count(copy), ==, 2);
}

static void
test_image_request_gtype(void)
{
	GType type;

	type = ai_image_request_get_type();
	g_assert_true(G_TYPE_IS_BOXED(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiImageRequest");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/image-request/new", test_image_request_new);
	g_test_add_func("/ai-glib/image-request/model", test_image_request_model);
	g_test_add_func("/ai-glib/image-request/size", test_image_request_size);
	g_test_add_func("/ai-glib/image-request/custom-size", test_image_request_custom_size);
	g_test_add_func("/ai-glib/image-request/quality", test_image_request_quality);
	g_test_add_func("/ai-glib/image-request/style", test_image_request_style);
	g_test_add_func("/ai-glib/image-request/count", test_image_request_count);
	g_test_add_func("/ai-glib/image-request/response-format", test_image_request_response_format);
	g_test_add_func("/ai-glib/image-request/user", test_image_request_user);
	g_test_add_func("/ai-glib/image-request/copy", test_image_request_copy);
	g_test_add_func("/ai-glib/image-request/gtype", test_image_request_gtype);

	return g_test_run();
}
