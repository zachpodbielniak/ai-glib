/*
 * test-image-serialize.c - Wire-format tests for image requests
 *
 * These pin down the request bodies the providers actually put on the
 * wire.  Most of them are regressions: image APIs reject parameters their
 * model does not understand, so an extra member is not a cosmetic problem
 * but a failed request.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <json-glib/json-glib.h>

#include "providers/ai-image-shared.h"
#include "core/ai-image-capabilities.h"

static const guchar tiny_png[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };

static AiImage *
make_image(const gchar *role, const gchar *mime)
{
	AiImage *image = ai_image_new_from_data(tiny_png, sizeof tiny_png, mime);

	if (role != NULL)
	{
		ai_image_set_role(image, role);
	}

	return image;
}

/* The GPT Image family: no response_format, no style, its own quality words. */
static AiImageModelInfo *
make_gpt_image_info(void)
{
	AiImageModelInfo *info;
	static const gchar * const qualities[] = {
		"auto", "low", "medium", "high", NULL
	};

	info = ai_image_model_info_new(
		"gpt-image-2", NULL, AI_PROVIDER_OPENAI,
		AI_IMAGE_CAP_REFERENCE_IMAGES | AI_IMAGE_CAP_MULTI_REFERENCE |
		AI_IMAGE_CAP_MASK | AI_IMAGE_CAP_PIXEL_SIZE |
		AI_IMAGE_CAP_TRANSPARENCY | AI_IMAGE_CAP_OUTPUT_FORMAT |
		AI_IMAGE_CAP_QUALITY | AI_IMAGE_CAP_MULTI_COUNT |
		AI_IMAGE_CAP_SAFETY_CONTROL | AI_IMAGE_CAP_INPUT_FIDELITY);
	ai_image_model_info_set_qualities(info, qualities);
	ai_image_model_info_set_max_count(info, 10);
	ai_image_model_info_set_max_reference_images(info, 16);

	return info;
}

/* DALL-E 3: response_format and style, standard/hd quality. */
static AiImageModelInfo *
make_dalle3_info(void)
{
	AiImageModelInfo *info;
	static const gchar * const qualities[] = { "standard", "hd", NULL };

	info = ai_image_model_info_new(
		"dall-e-3", NULL, AI_PROVIDER_OPENAI,
		AI_IMAGE_CAP_PIXEL_SIZE | AI_IMAGE_CAP_QUALITY |
		AI_IMAGE_CAP_STYLE | AI_IMAGE_CAP_URL_RESPONSE);
	ai_image_model_info_set_qualities(info, qualities);

	return info;
}

static JsonObject *
build_openai(AiImageRequest *request, const gchar *model,
             const AiImageModelInfo *info, JsonNode **out_root)
{
	*out_root = ai_image_shared_build_openai_json(request, model, info);

	g_assert_nonnull(*out_root);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(*out_root));

	return json_node_get_object(*out_root);
}

/*
 * The regression that motivated the capability model: GPT Image rejects
 * response_format and style outright, and the default OpenAI image model
 * is a GPT Image one, so sending them broke every request.
 */
static void
test_openai_gpt_image_omits_response_format_and_style(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonObject *obj;

	info = make_gpt_image_info();
	request = ai_image_request_new("a brass telescope");

	/* Set them explicitly; they must still not appear. */
	ai_image_request_set_response_format(request, AI_IMAGE_RESPONSE_BASE64);
	ai_image_request_set_style(request, AI_IMAGE_STYLE_VIVID);

	obj = build_openai(request, "gpt-image-2", info, &root);

	g_assert_false(json_object_has_member(obj, "response_format"));
	g_assert_false(json_object_has_member(obj, "style"));
	g_assert_cmpstr(json_object_get_string_member(obj, "model"), ==,
	                "gpt-image-2");
	g_assert_cmpstr(json_object_get_string_member(obj, "prompt"), ==,
	                "a brass telescope");
}

static void
test_openai_dalle_keeps_response_format_and_style(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonObject *obj;

	info = make_dalle3_info();
	request = ai_image_request_new("a brass telescope");
	ai_image_request_set_style(request, AI_IMAGE_STYLE_NATURAL);
	ai_image_request_set_response_format(request, AI_IMAGE_RESPONSE_BASE64);

	obj = build_openai(request, "dall-e-3", info, &root);

	g_assert_cmpstr(json_object_get_string_member(obj, "response_format"), ==,
	                "b64_json");
	g_assert_cmpstr(json_object_get_string_member(obj, "style"), ==, "natural");
}

static void
test_openai_quality_translated_per_family(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) gpt = NULL;
	g_autoptr(AiImageModelInfo) dalle = NULL;
	g_autoptr(JsonNode) gpt_root = NULL;
	g_autoptr(JsonNode) dalle_root = NULL;
	JsonObject *obj;

	gpt = make_gpt_image_info();
	dalle = make_dalle3_info();

	request = ai_image_request_new("a cat");
	ai_image_request_set_quality(request, AI_IMAGE_QUALITY_HD);

	/* The same request reaches each family in the words it accepts. */
	obj = build_openai(request, "gpt-image-2", gpt, &gpt_root);
	g_assert_cmpstr(json_object_get_string_member(obj, "quality"), ==, "high");

	obj = build_openai(request, "dall-e-3", dalle, &dalle_root);
	g_assert_cmpstr(json_object_get_string_member(obj, "quality"), ==, "hd");
}

static void
test_openai_unset_parameters_are_absent(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonObject *obj;

	/* An untouched request must serialise to almost nothing, so each
	 * provider keeps its own defaults rather than ai-glib's guess. */
	info = make_gpt_image_info();
	request = ai_image_request_new("a cat");

	obj = build_openai(request, "gpt-image-2", info, &root);

	g_assert_false(json_object_has_member(obj, "size"));
	g_assert_false(json_object_has_member(obj, "quality"));
	g_assert_false(json_object_has_member(obj, "background"));
	g_assert_false(json_object_has_member(obj, "output_format"));
	g_assert_false(json_object_has_member(obj, "output_compression"));
	g_assert_false(json_object_has_member(obj, "moderation"));
	g_assert_false(json_object_has_member(obj, "n"));
	g_assert_false(json_object_has_member(obj, "user"));
	g_assert_false(json_object_has_member(obj, "stream"));
}

static void
test_openai_full_parameters(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonObject *obj;

	info = make_gpt_image_info();
	request = ai_image_request_new("an icon");

	ai_image_request_set_count(request, 3);
	ai_image_request_set_custom_size(request, "1536x1024");
	ai_image_request_set_background(request, AI_IMAGE_BACKGROUND_TRANSPARENT);
	ai_image_request_set_output_format(request, AI_IMAGE_FORMAT_WEBP);
	ai_image_request_set_output_compression(request, 80);
	ai_image_request_set_moderation(request, AI_IMAGE_MODERATION_LOW);
	ai_image_request_set_user(request, "user-1");

	obj = build_openai(request, "gpt-image-2", info, &root);

	g_assert_cmpint(json_object_get_int_member(obj, "n"), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(obj, "size"), ==, "1536x1024");
	g_assert_cmpstr(json_object_get_string_member(obj, "background"), ==,
	                "transparent");
	g_assert_cmpstr(json_object_get_string_member(obj, "output_format"), ==,
	                "webp");
	g_assert_cmpint(json_object_get_int_member(obj, "output_compression"), ==, 80);
	g_assert_cmpstr(json_object_get_string_member(obj, "moderation"), ==, "low");
	g_assert_cmpstr(json_object_get_string_member(obj, "user"), ==, "user-1");
}

static void
test_openai_extras_passthrough_and_override(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonObject *obj;

	info = make_gpt_image_info();
	request = ai_image_request_new("a cat");

	ai_image_request_set_extra_string(request, "brand_new_param", "value");
	ai_image_request_set_extra(request, "numeric", g_variant_new_int32(7));

	/* Extras are applied last, so they win over a modelled parameter that
	 * serialises to the same member. */
	ai_image_request_set_user(request, "original");
	ai_image_request_set_extra_string(request, "user", "overridden");

	obj = build_openai(request, "gpt-image-2", info, &root);

	g_assert_cmpstr(json_object_get_string_member(obj, "brand_new_param"), ==,
	                "value");
	g_assert_cmpint(json_object_get_int_member(obj, "numeric"), ==, 7);
	g_assert_cmpstr(json_object_get_string_member(obj, "user"), ==, "overridden");
}

/*
 * Reference roles have no field in any wire format, so they are folded
 * into the prompt -- the only channel the model reads.
 */
static void
test_prompt_roles_folded_in(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImage) style = NULL;
	g_autoptr(AiImage) subject = NULL;
	g_autofree gchar *prompt = NULL;

	request = ai_image_request_new("combine these into a poster");
	style = make_image("style", "image/png");
	subject = make_image("subject", "image/jpeg");

	ai_image_request_add_reference_image(request, style);
	ai_image_request_add_reference_image(request, subject);

	prompt = ai_image_shared_prompt_with_roles(request);

	g_assert_nonnull(strstr(prompt, "1. style"));
	g_assert_nonnull(strstr(prompt, "2. subject"));
	g_assert_nonnull(strstr(prompt, "combine these into a poster"));
}

static void
test_prompt_unchanged_without_roles(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImage) image = NULL;
	g_autofree gchar *prompt = NULL;

	/* Unlabelled references are passed through in order and the prompt is
	 * left exactly as the caller wrote it. */
	request = ai_image_request_new("a cat");
	image = make_image(NULL, "image/png");
	ai_image_request_add_reference_image(request, image);

	prompt = ai_image_shared_prompt_with_roles(request);

	g_assert_cmpstr(prompt, ==, "a cat");
}

static void
test_gemini_parts_text_first_then_references(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImage) a = NULL;
	g_autoptr(AiImage) b = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonArray *parts;
	JsonObject *part;
	JsonObject *inline_data;

	request = ai_image_request_new("a poster");
	a = make_image(NULL, "image/png");
	b = make_image(NULL, "image/jpeg");
	ai_image_request_add_reference_image(request, a);
	ai_image_request_add_reference_image(request, b);

	builder = json_builder_new();
	ai_image_shared_build_gemini_parts(builder, request);
	root = json_builder_get_root(builder);

	g_assert_true(JSON_NODE_HOLDS_ARRAY(root));
	parts = json_node_get_array(root);

	/* Text part, then one inline_data part per reference, in order. */
	g_assert_cmpuint(json_array_get_length(parts), ==, 3);

	part = json_array_get_object_element(parts, 0);
	g_assert_true(json_object_has_member(part, "text"));

	part = json_array_get_object_element(parts, 1);
	g_assert_true(json_object_has_member(part, "inline_data"));
	inline_data = json_object_get_object_member(part, "inline_data");
	g_assert_cmpstr(json_object_get_string_member(inline_data, "mime_type"), ==,
	                "image/png");
	g_assert_nonnull(json_object_get_string_member(inline_data, "data"));

	part = json_array_get_object_element(parts, 2);
	inline_data = json_object_get_object_member(part, "inline_data");
	g_assert_cmpstr(json_object_get_string_member(inline_data, "mime_type"), ==,
	                "image/jpeg");
}

static void
test_gemini_parts_without_references(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonArray *parts;

	request = ai_image_request_new("a cat");

	builder = json_builder_new();
	ai_image_shared_build_gemini_parts(builder, request);
	root = json_builder_get_root(builder);

	parts = json_node_get_array(root);
	g_assert_cmpuint(json_array_get_length(parts), ==, 1);
}

/* Collect the Content-Disposition names of every multipart part. */
static gchar *
multipart_part_names(SoupMultipart *multipart)
{
	g_autoptr(GString) out = g_string_new(NULL);
	gint i;

	for (i = 0; i < soup_multipart_get_length(multipart); i++)
	{
		SoupMessageHeaders *headers = NULL;
		GBytes *body = NULL;
		g_autofree gchar *name = NULL;
		GHashTable *params = NULL;

		soup_multipart_get_part(multipart, i, &headers, &body);

		if (soup_message_headers_get_content_disposition(headers, NULL,
		                                                 &params))
		{
			name = g_strdup(g_hash_table_lookup(params, "name"));
			g_hash_table_unref(params);
		}

		if (out->len > 0)
		{
			g_string_append_c(out, ',');
		}
		g_string_append(out, name != NULL ? name : "?");
	}

	return g_strdup(out->str);
}

static void
test_openai_multipart_multi_reference(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(SoupMultipart) multipart = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiImage) a = NULL;
	g_autoptr(AiImage) b = NULL;
	g_autofree gchar *names = NULL;

	info = make_gpt_image_info();
	request = ai_image_request_new("combine");
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_EDIT);

	a = make_image(NULL, "image/png");
	b = make_image(NULL, "image/png");
	ai_image_request_add_reference_image(request, a);
	ai_image_request_add_reference_image(request, b);

	multipart = ai_image_shared_build_openai_multipart(request, "gpt-image-2",
	                                                   info, &error);
	g_assert_no_error(error);
	g_assert_nonnull(multipart);

	/* Models taking several references expect the repeated image[] form. */
	names = multipart_part_names(multipart);
	g_assert_nonnull(strstr(names, "image[]"));
	g_assert_nonnull(strstr(names, "prompt"));
	g_assert_nonnull(strstr(names, "model"));
}

static void
test_openai_multipart_single_reference_and_mask(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(SoupMultipart) multipart = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiImage) image = NULL;
	g_autoptr(AiImage) mask = NULL;
	g_autofree gchar *names = NULL;

	/* DALL-E 2 takes a bare `image` plus an optional mask; sending
	 * `image[]` there is rejected. */
	info = ai_image_model_info_new(
		"dall-e-2", NULL, AI_PROVIDER_OPENAI,
		AI_IMAGE_CAP_REFERENCE_IMAGES | AI_IMAGE_CAP_MASK |
		AI_IMAGE_CAP_URL_RESPONSE | AI_IMAGE_CAP_PIXEL_SIZE);
	ai_image_model_info_set_max_reference_images(info, 1);

	request = ai_image_request_new("erase the sky");
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_EDIT);

	image = make_image(NULL, "image/png");
	mask = make_image(NULL, "image/png");
	ai_image_request_add_reference_image(request, image);
	ai_image_request_set_mask(request, mask);

	multipart = ai_image_shared_build_openai_multipart(request, "dall-e-2",
	                                                   info, &error);
	g_assert_no_error(error);

	names = multipart_part_names(multipart);
	g_assert_null(strstr(names, "image[]"));
	g_assert_nonnull(strstr(names, "image"));
	g_assert_nonnull(strstr(names, "mask"));
}

static void
test_openai_multipart_variation_has_no_prompt(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(SoupMultipart) multipart = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiImage) image = NULL;
	g_autofree gchar *names = NULL;

	info = ai_image_model_info_new(
		"dall-e-2", NULL, AI_PROVIDER_OPENAI,
		AI_IMAGE_CAP_REFERENCE_IMAGES | AI_IMAGE_CAP_VARIATION |
		AI_IMAGE_CAP_URL_RESPONSE);
	ai_image_model_info_set_max_reference_images(info, 1);

	request = ai_image_request_new("ignored");
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_VARIATION);
	image = make_image(NULL, "image/png");
	ai_image_request_add_reference_image(request, image);

	multipart = ai_image_shared_build_openai_multipart(request, "dall-e-2",
	                                                   info, &error);
	g_assert_no_error(error);

	/* The variations endpoint takes no prompt at all. */
	names = multipart_part_names(multipart);
	g_assert_null(strstr(names, "prompt"));
}

static void
test_openai_multipart_requires_reference(void)
{
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageModelInfo) info = NULL;
	g_autoptr(GError) error = NULL;
	SoupMultipart *multipart;

	info = make_gpt_image_info();
	request = ai_image_request_new("edit nothing");
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_EDIT);

	multipart = ai_image_shared_build_openai_multipart(request, "gpt-image-2",
	                                                   info, &error);

	g_assert_null(multipart);
	g_assert_nonnull(error);
}

static void
test_openai_parse_response(void)
{
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	AiGeneratedImage *image;
	const gchar *json =
		"{\"created\":123,\"data\":["
		"{\"b64_json\":\"aGk=\",\"revised_prompt\":\"a revised cat\"},"
		"{\"url\":\"https://example.invalid/a.png\"}]}";

	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
	g_assert_no_error(error);

	response = ai_image_shared_parse_openai_response(
		json_parser_get_root(parser), "gpt-image-2", &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_cmpuint(ai_image_response_get_image_count(response), ==, 2);
	g_assert_cmpint(ai_image_response_get_created(response), ==, 123);
	g_assert_cmpstr(ai_image_response_get_model(response), ==, "gpt-image-2");

	image = ai_image_response_get_image(response, 0);
	g_assert_true(ai_generated_image_is_base64(image));
	g_assert_cmpstr(ai_generated_image_get_revised_prompt(image), ==,
	                "a revised cat");

	image = ai_image_response_get_image(response, 1);
	g_assert_true(ai_generated_image_is_url(image));
}

static void
test_openai_parse_error_response(void)
{
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	AiImageResponse *response;
	const gchar *json =
		"{\"error\":{\"type\":\"image_generation_user_error\","
		"\"message\":\"content policy\"}}";

	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));

	response = ai_image_shared_parse_openai_response(
		json_parser_get_root(parser), NULL, &error);

	g_assert_null(response);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "content policy"));
}

static void
test_status_to_error_quotes_body(void)
{
	g_autoptr(GError) error = NULL;
	const gchar *body = "{\"error\":{\"message\":\"prompt was rejected\"}}";

	/* The provider's own explanation is the useful part of a failure. */
	ai_image_shared_status_to_error(400, body, strlen(body), &error);

	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "prompt was rejected"));
}

static void
test_status_to_error_codes(void)
{
	g_autoptr(GError) unauthorized = NULL;
	g_autoptr(GError) forbidden = NULL;
	g_autoptr(GError) limited = NULL;
	g_autoptr(GError) server = NULL;

	ai_image_shared_status_to_error(401, NULL, 0, &unauthorized);
	ai_image_shared_status_to_error(403, NULL, 0, &forbidden);
	ai_image_shared_status_to_error(429, NULL, 0, &limited);
	ai_image_shared_status_to_error(503, NULL, 0, &server);

	g_assert_error(unauthorized, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_assert_error(forbidden, AI_ERROR, AI_ERROR_PERMISSION_DENIED);
	g_assert_error(limited, AI_ERROR, AI_ERROR_RATE_LIMITED);
	g_assert_error(server, AI_ERROR, AI_ERROR_SERVER_ERROR);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/image-serialize/openai/gpt-image-omits",
	                test_openai_gpt_image_omits_response_format_and_style);
	g_test_add_func("/image-serialize/openai/dalle-keeps",
	                test_openai_dalle_keeps_response_format_and_style);
	g_test_add_func("/image-serialize/openai/quality-translated",
	                test_openai_quality_translated_per_family);
	g_test_add_func("/image-serialize/openai/unset-absent",
	                test_openai_unset_parameters_are_absent);
	g_test_add_func("/image-serialize/openai/full-parameters",
	                test_openai_full_parameters);
	g_test_add_func("/image-serialize/openai/extras",
	                test_openai_extras_passthrough_and_override);
	g_test_add_func("/image-serialize/prompt/roles-folded",
	                test_prompt_roles_folded_in);
	g_test_add_func("/image-serialize/prompt/no-roles",
	                test_prompt_unchanged_without_roles);
	g_test_add_func("/image-serialize/gemini/parts-order",
	                test_gemini_parts_text_first_then_references);
	g_test_add_func("/image-serialize/gemini/parts-no-refs",
	                test_gemini_parts_without_references);
	g_test_add_func("/image-serialize/multipart/multi-reference",
	                test_openai_multipart_multi_reference);
	g_test_add_func("/image-serialize/multipart/single-and-mask",
	                test_openai_multipart_single_reference_and_mask);
	g_test_add_func("/image-serialize/multipart/variation-no-prompt",
	                test_openai_multipart_variation_has_no_prompt);
	g_test_add_func("/image-serialize/multipart/requires-reference",
	                test_openai_multipart_requires_reference);
	g_test_add_func("/image-serialize/openai/parse", test_openai_parse_response);
	g_test_add_func("/image-serialize/openai/parse-error",
	                test_openai_parse_error_response);
	g_test_add_func("/image-serialize/status/quotes-body",
	                test_status_to_error_quotes_body);
	g_test_add_func("/image-serialize/status/codes", test_status_to_error_codes);

	return g_test_run();
}
