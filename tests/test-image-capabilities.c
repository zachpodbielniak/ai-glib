/*
 * test-image-capabilities.c - Capability descriptors and request validation
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "core/ai-image-capabilities.h"
#include "core/ai-error.h"

static const guchar tiny_png[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };

static AiImage *
make_image(const gchar *role)
{
	AiImage *image = ai_image_new_from_data(tiny_png, sizeof tiny_png,
	                                        "image/png");

	if (role != NULL)
	{
		ai_image_set_role(image, role);
	}

	return image;
}

/* A permissive model, so a test can isolate the one thing it cares about. */
static AiImageModelInfo *
make_permissive_model(void)
{
	AiImageModelInfo *info;

	info = ai_image_model_info_new(
		"test-everything", "Test", AI_PROVIDER_OPENAI,
		AI_IMAGE_CAP_REFERENCE_IMAGES | AI_IMAGE_CAP_MULTI_REFERENCE |
		AI_IMAGE_CAP_MASK | AI_IMAGE_CAP_NEGATIVE_PROMPT |
		AI_IMAGE_CAP_SEED | AI_IMAGE_CAP_ASPECT_RATIO |
		AI_IMAGE_CAP_PIXEL_SIZE | AI_IMAGE_CAP_RESOLUTION_TIER |
		AI_IMAGE_CAP_TRANSPARENCY | AI_IMAGE_CAP_OUTPUT_FORMAT |
		AI_IMAGE_CAP_QUALITY | AI_IMAGE_CAP_STYLE |
		AI_IMAGE_CAP_MULTI_COUNT | AI_IMAGE_CAP_VARIATION |
		AI_IMAGE_CAP_UPSCALE | AI_IMAGE_CAP_PARTIAL_STREAMING |
		AI_IMAGE_CAP_SAFETY_CONTROL | AI_IMAGE_CAP_WATERMARK_CONTROL |
		AI_IMAGE_CAP_URL_RESPONSE | AI_IMAGE_CAP_SAMPLING |
		AI_IMAGE_CAP_PROMPT_ENHANCEMENT | AI_IMAGE_CAP_LANGUAGE |
		AI_IMAGE_CAP_INPUT_FIDELITY);

	ai_image_model_info_set_max_count(info, 10);
	ai_image_model_info_set_max_reference_images(info, 8);

	return info;
}

/* A bare model that supports nothing but a prompt. */
static AiImageModelInfo *
make_minimal_model(void)
{
	return ai_image_model_info_new("test-minimal", "Minimal",
	                               AI_PROVIDER_GROK, AI_IMAGE_CAP_NONE);
}

static void
test_model_info_basics(void)
{
	g_autoptr(AiImageModelInfo) info = NULL;

	info = ai_image_model_info_new("m", "Model", AI_PROVIDER_GEMINI,
	                               AI_IMAGE_CAP_SEED | AI_IMAGE_CAP_ASPECT_RATIO);

	g_assert_cmpstr(ai_image_model_info_get_id(info), ==, "m");
	g_assert_cmpstr(ai_image_model_info_get_display_name(info), ==, "Model");
	g_assert_cmpint(ai_image_model_info_get_provider_type(info), ==,
	                AI_PROVIDER_GEMINI);

	g_assert_true(ai_image_model_info_supports(info, AI_IMAGE_CAP_SEED));
	g_assert_false(ai_image_model_info_supports(info, AI_IMAGE_CAP_MASK));

	/* Several bits at once means "all of them". */
	g_assert_true(ai_image_model_info_supports(
		info, AI_IMAGE_CAP_SEED | AI_IMAGE_CAP_ASPECT_RATIO));
	g_assert_false(ai_image_model_info_supports(
		info, AI_IMAGE_CAP_SEED | AI_IMAGE_CAP_MASK));
}

static void
test_model_info_display_name_defaults_to_id(void)
{
	g_autoptr(AiImageModelInfo) info = NULL;

	info = ai_image_model_info_new("bare-id", NULL, AI_PROVIDER_OPENAI,
	                               AI_IMAGE_CAP_NONE);

	g_assert_cmpstr(ai_image_model_info_get_display_name(info), ==, "bare-id");
}

static void
test_model_info_copy(void)
{
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(AiImageModelInfo) copy = NULL;
	static const gchar * const ratios[] = { "1:1", "16:9", NULL };

	info = make_permissive_model();
	ai_image_model_info_set_aspect_ratios(info, ratios);
	ai_image_model_info_set_notes(info, "notes");

	copy = ai_image_model_info_copy(info);

	g_assert_cmpstr(ai_image_model_info_get_id(copy), ==, "test-everything");
	g_assert_cmpuint(ai_image_model_info_get_max_count(copy), ==, 10);
	g_assert_cmpuint(ai_image_model_info_get_max_reference_images(copy), ==, 8);
	g_assert_cmpstr(ai_image_model_info_get_aspect_ratios(copy)[1], ==, "16:9");
	g_assert_cmpstr(ai_image_model_info_get_notes(copy), ==, "notes");

	g_assert_null(ai_image_model_info_copy(NULL));
}

static void
test_capabilities_to_string(void)
{
	g_autofree gchar *rendered = NULL;
	g_autofree gchar *empty = NULL;

	rendered = ai_image_capabilities_to_string(
		AI_IMAGE_CAP_SEED | AI_IMAGE_CAP_MASK);

	g_assert_nonnull(rendered);
	g_assert_nonnull(strstr(rendered, "seed"));
	g_assert_nonnull(strstr(rendered, "mask"));

	empty = ai_image_capabilities_to_string(AI_IMAGE_CAP_NONE);
	g_assert_cmpstr(empty, ==, "");
}

/*
 * Quality translation is what keeps a caller from having to know which
 * OpenAI family it is talking to.
 */
static void
test_quality_mapping(void)
{
	g_autoptr(AiImageModelInfo) gpt = NULL;
	g_autoptr(AiImageModelInfo) dalle = NULL;
	static const gchar * const gpt_q[] = { "auto", "low", "medium", "high", NULL };
	static const gchar * const dalle_q[] = { "standard", "hd", NULL };

	gpt = ai_image_model_info_new("gpt", NULL, AI_PROVIDER_OPENAI,
	                              AI_IMAGE_CAP_QUALITY);
	ai_image_model_info_set_qualities(gpt, gpt_q);

	dalle = ai_image_model_info_new("dalle", NULL, AI_PROVIDER_OPENAI,
	                                AI_IMAGE_CAP_QUALITY);
	ai_image_model_info_set_qualities(dalle, dalle_q);

	/* Exact matches pass straight through. */
	g_assert_cmpstr(ai_image_model_info_map_quality(gpt, AI_IMAGE_QUALITY_HIGH),
	                ==, "high");
	g_assert_cmpstr(ai_image_model_info_map_quality(dalle, AI_IMAGE_QUALITY_HD),
	                ==, "hd");

	/* Cross-family requests land on the equivalent tier rather than
	 * failing: HD against GPT Image becomes "high". */
	g_assert_cmpstr(ai_image_model_info_map_quality(gpt, AI_IMAGE_QUALITY_HD),
	                ==, "high");
	g_assert_cmpstr(ai_image_model_info_map_quality(dalle, AI_IMAGE_QUALITY_HIGH),
	                ==, "hd");
	g_assert_cmpstr(ai_image_model_info_map_quality(gpt, AI_IMAGE_QUALITY_STANDARD),
	                ==, "medium");
	g_assert_cmpstr(ai_image_model_info_map_quality(dalle, AI_IMAGE_QUALITY_MEDIUM),
	                ==, "standard");

	/* DALL-E has no "low" tier, so it degrades to standard. */
	g_assert_cmpstr(ai_image_model_info_map_quality(dalle, AI_IMAGE_QUALITY_LOW),
	                ==, "standard");

	/* AUTO is the unset sentinel: always omit, even where the model
	 * accepts a literal "auto". */
	g_assert_null(ai_image_model_info_map_quality(gpt, AI_IMAGE_QUALITY_AUTO));
	g_assert_null(ai_image_model_info_map_quality(dalle, AI_IMAGE_QUALITY_AUTO));
}

static void
test_validate_null_info_passes(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(GError) error = NULL;

	/* An unknown model must be passed through untouched, so a model
	 * released after this build still works. */
	request = ai_image_request_new("a cat");
	ai_image_request_set_seed(request, 42);

	g_assert_true(ai_image_request_validate(request, NULL,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_no_error(error);
	g_assert_cmpint(ai_image_request_get_seed(request), ==, 42);
}

static void
test_validate_lenient_drops(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	info = make_minimal_model();
	request = ai_image_request_new("a cat");

	ai_image_request_set_seed(request, 7);
	ai_image_request_set_negative_prompt(request, "text");
	ai_image_request_set_aspect_ratio(request, "16:9");
	ai_image_request_set_background(request, AI_IMAGE_BACKGROUND_TRANSPARENT);
	ai_image_request_set_output_format(request, AI_IMAGE_FORMAT_WEBP);
	ai_image_request_set_quality(request, AI_IMAGE_QUALITY_HD);
	ai_image_request_set_style(request, AI_IMAGE_STYLE_VIVID);
	ai_image_request_set_language(request, "en");
	ai_image_request_set_watermark(request, AI_TRI_FALSE);
	ai_image_request_set_input_fidelity(request, AI_IMAGE_FIDELITY_HIGH);
	ai_image_request_set_partial_images(request, 2);
	ai_image_request_set_temperature(request, 0.5);

	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_no_error(error);

	/* Everything the model cannot express is reset, leaving a request it
	 * will accept. */
	g_assert_cmpint(ai_image_request_get_seed(request), ==, -1);
	g_assert_null(ai_image_request_get_negative_prompt(request));
	g_assert_null(ai_image_request_get_aspect_ratio(request));
	g_assert_cmpint(ai_image_request_get_background(request), ==,
	                AI_IMAGE_BACKGROUND_AUTO);
	g_assert_cmpint(ai_image_request_get_output_format(request), ==,
	                AI_IMAGE_FORMAT_AUTO);
	g_assert_cmpint(ai_image_request_get_quality(request), ==,
	                AI_IMAGE_QUALITY_AUTO);
	g_assert_cmpint(ai_image_request_get_style(request), ==,
	                AI_IMAGE_STYLE_AUTO);
	g_assert_null(ai_image_request_get_language(request));
	g_assert_cmpint(ai_image_request_get_watermark(request), ==, AI_TRI_UNSET);
	g_assert_cmpint(ai_image_request_get_input_fidelity(request), ==,
	                AI_IMAGE_FIDELITY_AUTO);
	g_assert_cmpint(ai_image_request_get_partial_images(request), ==, -1);
	g_assert_cmpfloat(ai_image_request_get_temperature(request), <, 0.0);
}

static void
test_validate_strict_errors(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	info = make_minimal_model();
	request = ai_image_request_new("a cat");
	ai_image_request_set_seed(request, 7);

	g_assert_false(ai_image_request_validate(request, info,
	                                         AI_IMAGE_VALIDATE_STRICT,
	                                         &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);

	/* Strict mode leaves the request untouched so the caller can report
	 * exactly what it asked for. */
	g_assert_cmpint(ai_image_request_get_seed(request), ==, 7);
}

static void
test_validate_response_format_falls_back(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	/* A model that only returns inline bytes cannot honour a URL request,
	 * but the caller gets the image either way, so fall back rather than
	 * fail. */
	info = make_minimal_model();
	request = ai_image_request_new("a cat");
	ai_image_request_set_response_format(request, AI_IMAGE_RESPONSE_URL);

	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_cmpint(ai_image_request_get_response_format(request), ==,
	                AI_IMAGE_RESPONSE_BASE64);
}

static void
test_validate_count_clamped(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	info = ai_image_model_info_new("m", NULL, AI_PROVIDER_OPENAI,
	                               AI_IMAGE_CAP_MULTI_COUNT);
	ai_image_model_info_set_max_count(info, 4);

	request = ai_image_request_new("a cat");
	ai_image_request_set_count(request, 99);

	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_cmpint(ai_image_request_get_count(request), ==, 4);
}

static void
test_validate_count_forced_to_one(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	info = make_minimal_model();
	request = ai_image_request_new("a cat");
	ai_image_request_set_count(request, 5);

	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_cmpint(ai_image_request_get_count(request), ==, 1);
}

/*
 * References are the one thing never dropped silently: quietly discarding
 * one changes the image in a way the caller cannot observe.
 */
static void
test_validate_references_unsupported_is_error(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(AiImage) image = NULL;
	g_autoptr(GError) error = NULL;

	info = make_minimal_model();
	request = ai_image_request_new("a cat");
	image = make_image(NULL);
	ai_image_request_add_reference_image(request, image);

	/* Even in lenient mode. */
	g_assert_false(ai_image_request_validate(request, info,
	                                         AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_validate_multi_reference_requires_capability(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(AiImage) a = NULL;
	g_autoptr(AiImage) b = NULL;
	g_autoptr(GError) error = NULL;

	info = ai_image_model_info_new("single-ref", NULL, AI_PROVIDER_GROK,
	                               AI_IMAGE_CAP_REFERENCE_IMAGES);
	ai_image_model_info_set_max_reference_images(info, 1);

	request = ai_image_request_new("combine these");
	a = make_image("style");
	b = make_image("subject");
	ai_image_request_add_reference_image(request, a);

	/* One is fine. */
	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_no_error(error);

	/* Two is not. */
	ai_image_request_add_reference_image(request, b);
	g_assert_false(ai_image_request_validate(request, info,
	                                         AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_validate_reference_limit(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	guint i;

	info = ai_image_model_info_new(
		"capped", NULL, AI_PROVIDER_GEMINI,
		AI_IMAGE_CAP_REFERENCE_IMAGES | AI_IMAGE_CAP_MULTI_REFERENCE);
	ai_image_model_info_set_max_reference_images(info, 3);

	request = ai_image_request_new("combine");

	for (i = 0; i < 4; i++)
	{
		g_autoptr(AiImage) image = make_image(NULL);
		ai_image_request_add_reference_image(request, image);
	}

	g_assert_false(ai_image_request_validate(request, info,
	                                         AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_validate_mask_needs_reference(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(AiImage) mask = NULL;
	g_autoptr(GError) error = NULL;

	info = make_permissive_model();
	request = ai_image_request_new("edit");
	mask = make_image(NULL);
	ai_image_request_set_mask(request, mask);

	g_assert_false(ai_image_request_validate(request, info,
	                                         AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_validate_unsupported_operation_is_error(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	/* Silently generating from scratch when a variation was asked for
	 * would return a plausible-looking but entirely wrong image. */
	info = make_minimal_model();
	request = ai_image_request_new("vary this");
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_VARIATION);

	g_assert_false(ai_image_request_validate(request, info,
	                                         AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_validate_transparent_jpeg_is_error(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	/* Self-contradictory however capable the model is: JPEG has no alpha
	 * channel, so this would silently return an opaque image. */
	info = make_permissive_model();
	request = ai_image_request_new("a logo");
	ai_image_request_set_background(request, AI_IMAGE_BACKGROUND_TRANSPARENT);
	ai_image_request_set_output_format(request, AI_IMAGE_FORMAT_JPEG);

	g_assert_false(ai_image_request_validate(request, info,
	                                         AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_validate_unknown_aspect_ratio_dropped(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	static const gchar * const ratios[] = { "1:1", "16:9", NULL };

	info = ai_image_model_info_new("m", NULL, AI_PROVIDER_GEMINI,
	                               AI_IMAGE_CAP_ASPECT_RATIO);
	ai_image_model_info_set_aspect_ratios(info, ratios);

	request = ai_image_request_new("a cat");
	ai_image_request_set_aspect_ratio(request, "7:3");

	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_null(ai_image_request_get_aspect_ratio(request));

	/* A supported one survives. */
	ai_image_request_set_aspect_ratio(request, "16:9");
	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_cmpstr(ai_image_request_get_aspect_ratio(request), ==, "16:9");
}

static void
test_validate_permissive_model_keeps_everything(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;

	info = make_permissive_model();
	request = ai_image_request_new("a cat");

	ai_image_request_set_seed(request, 11);
	ai_image_request_set_negative_prompt(request, "text");
	ai_image_request_set_resolution(request, AI_IMAGE_RESOLUTION_4K);
	ai_image_request_set_output_format(request, AI_IMAGE_FORMAT_WEBP);
	ai_image_request_set_background(request, AI_IMAGE_BACKGROUND_TRANSPARENT);

	g_assert_true(ai_image_request_validate(request, info,
	                                        AI_IMAGE_VALIDATE_NONE, &error));
	g_assert_no_error(error);

	g_assert_cmpint(ai_image_request_get_seed(request), ==, 11);
	g_assert_cmpstr(ai_image_request_get_negative_prompt(request), ==, "text");
	g_assert_cmpint(ai_image_request_get_resolution(request), ==,
	                AI_IMAGE_RESOLUTION_4K);
	g_assert_cmpint(ai_image_request_get_output_format(request), ==,
	                AI_IMAGE_FORMAT_WEBP);
	g_assert_cmpint(ai_image_request_get_background(request), ==,
	                AI_IMAGE_BACKGROUND_TRANSPARENT);
}

static void
test_gtype(void)
{
	g_assert_true(G_TYPE_IS_BOXED(ai_image_model_info_get_type()));
	g_assert_cmpstr(g_type_name(ai_image_model_info_get_type()), ==,
	                "AiImageModelInfo");
	g_assert_true(G_TYPE_IS_FLAGS(ai_image_capabilities_get_type()));
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/image-caps/model-info", test_model_info_basics);
	g_test_add_func("/ai-glib/image-caps/display-name-default",
	                test_model_info_display_name_defaults_to_id);
	g_test_add_func("/ai-glib/image-caps/copy", test_model_info_copy);
	g_test_add_func("/ai-glib/image-caps/to-string", test_capabilities_to_string);
	g_test_add_func("/ai-glib/image-caps/quality-mapping", test_quality_mapping);
	g_test_add_func("/ai-glib/image-caps/validate/null-info",
	                test_validate_null_info_passes);
	g_test_add_func("/ai-glib/image-caps/validate/lenient-drops",
	                test_validate_lenient_drops);
	g_test_add_func("/ai-glib/image-caps/validate/strict-errors",
	                test_validate_strict_errors);
	g_test_add_func("/ai-glib/image-caps/validate/response-format-fallback",
	                test_validate_response_format_falls_back);
	g_test_add_func("/ai-glib/image-caps/validate/count-clamped",
	                test_validate_count_clamped);
	g_test_add_func("/ai-glib/image-caps/validate/count-forced-to-one",
	                test_validate_count_forced_to_one);
	g_test_add_func("/ai-glib/image-caps/validate/references-unsupported",
	                test_validate_references_unsupported_is_error);
	g_test_add_func("/ai-glib/image-caps/validate/multi-reference",
	                test_validate_multi_reference_requires_capability);
	g_test_add_func("/ai-glib/image-caps/validate/reference-limit",
	                test_validate_reference_limit);
	g_test_add_func("/ai-glib/image-caps/validate/mask-needs-reference",
	                test_validate_mask_needs_reference);
	g_test_add_func("/ai-glib/image-caps/validate/unsupported-operation",
	                test_validate_unsupported_operation_is_error);
	g_test_add_func("/ai-glib/image-caps/validate/transparent-jpeg",
	                test_validate_transparent_jpeg_is_error);
	g_test_add_func("/ai-glib/image-caps/validate/unknown-aspect-ratio",
	                test_validate_unknown_aspect_ratio_dropped);
	g_test_add_func("/ai-glib/image-caps/validate/permissive-keeps-all",
	                test_validate_permissive_model_keeps_everything);
	g_test_add_func("/ai-glib/image-caps/gtype", test_gtype);

	return g_test_run();
}
