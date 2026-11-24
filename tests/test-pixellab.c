/*
 * test-pixellab.c - AiPixellabClient
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Hermetic: every request goes to a loopback mock of PixelLab's v2 API on
 * its own thread (tests/media-mock.h).  The shapes it answers with are the
 * OpenAPI document's (api.pixellab.ai/v2/openapi.json, 2026-11-23): base64
 * images in synchronous replies, background job ids in asynchronous ones,
 * GET /background-jobs/{id}, the objects those jobs make, and the
 * documented 401, 402, 422, 423, 429 and 529 answers.  Nothing reaches
 * PixelLab.
 */

#include <string.h>

#include "ai-glib.h"
#include "media-mock.h"

#define TEST_KEY "pl-token-SHOULD-NEVER-APPEAR-5551234"
#define TEST_AUTH "Bearer " TEST_KEY

/* A 1x1 PNG. */
static const guchar tiny_png[] = {
	0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
	0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
	0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
	0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x00, 0x02, 0x00,
	0x00, 0x05, 0x00, 0x01, 0xe9, 0xfa, 0xdc, 0xd8, 0x00, 0x00, 0x00, 0x00,
	0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

static gchar *tiny_png_b64;

/* ---------------------------------------------------------------------- */
/* The mock API                                                           */
/* ---------------------------------------------------------------------- */

static void
reply_json(
	SoupServerMessage *msg,
	guint              status,
	gchar             *body
){
	media_mock_reply(msg, status, body);
	g_free(body);
}

static gchar *
character_body(void)
{
	const gchar *b = media_mock->base_url;
	GString *out = g_string_new(NULL);
	static const gchar * const directions[] = {
		"south", "south-east", "east", "north-east", "north", "north-west", "west", "south-west", NULL
	};
	guint i;

	g_string_append(out, "{\"id\":\"char-1\",\"name\":\"knight\",\"prompt\":\"a knight\","
	                     "\"size\":{\"width\":48,\"height\":48},\"directions\":8,"
	                     "\"created_at\":\"2026-11-23T00:00:00Z\",\"animation_count\":1,"
	                     "\"template_id\":\"mannequin\",\"view\":\"low top-down\","
	                     "\"status\":\"completed\",\"rotation_urls\":{");
	for (i = 0; directions[i] != NULL; i++)
	{
		g_string_append_printf(out, "%s\"%s\":\"%s/storage/%s.png\"", (i > 0) ? "," : "",
		                       directions[i], b, directions[i]);
	}
	g_string_append_printf(out, "},\"animations\":[{\"animation_type\":\"walking\","
	                            "\"animation_group_id\":\"grp-1\",\"directions\":["
	                            "{\"direction\":\"south\",\"frame_count\":2,\"frames\":"
	                            "[\"%s/storage/s0.png\",\"%s/storage/s1.png\"]},"
	                            "{\"direction\":\"east\",\"frame_count\":1,\"frames\":"
	                            "[\"%s/storage/e0.png\"]}]},"
	                            "{\"animation_type\":\"idle\",\"animation_group_id\":\"grp-0\","
	                            "\"directions\":[{\"direction\":\"south\",\"frame_count\":1,"
	                            "\"frames\":[\"%s/storage/idle.png\"]}]}]}",
	                       b, b, b, b);
	return g_string_free(out, FALSE);
}

static void
pixellab_route(
	SoupServerMessage *msg,
	const gchar       *method,
	const gchar       *path,
	const gchar       *body,
	guint              hit
){
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
	const gchar *auth = soup_message_headers_get_one(headers, "Authorization");

	/* Public storage and the spritesheet export: no token expected. */
	if (g_str_has_prefix(path, "/storage/") || g_str_has_suffix(path, "/spritesheet"))
	{
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "application/zip", SOUP_MEMORY_STATIC,
		                                 "PK\x03\x04zip", 7);
		return;
	}

	if (g_strcmp0(auth, TEST_AUTH) != 0)
	{
		reply_json(msg, 401, g_strdup_printf("{\"detail\":\"Invalid API token: %s\"}",
		                                     auth != NULL ? auth : "(none)"));
		return;
	}

	if (g_strcmp0(method, "POST") == 0)
	{
		/* The description picks an error, as the OpenAPI document lists them. */
		if (strstr(body, "\"description\":\"broke\"") != NULL)
		{
			media_mock_reply(msg, 402, "{\"detail\":\"Insufficient credits\"}");
			return;
		}
		if (strstr(body, "\"description\":\"busy\"") != NULL)
		{
			soup_message_headers_replace(soup_server_message_get_response_headers(msg),
			                             "Retry-After", "3");
			media_mock_reply(msg, 429, "{\"detail\":\"Too many requests\"}");
			return;
		}
		if (strstr(body, "\"description\":\"flooded\"") != NULL)
		{
			media_mock_reply(msg, 529, "{\"detail\":\"Rate limit exceeded\"}");
			return;
		}
		if (strstr(body, "\"description\":\"invalid\"") != NULL)
		{
			media_mock_reply(msg, 422, "{\"detail\":[{\"loc\":[\"body\",\"image_size\",\"width\"],"
			                           "\"msg\":\"Input should be less than or equal to 400\","
			                           "\"type\":\"less_than_equal\"}]}");
			return;
		}

		if (g_strcmp0(path, "/create-image-pixflux") == 0 ||
		    g_strcmp0(path, "/create-image-bitforge") == 0 ||
		    g_strcmp0(path, "/rotate") == 0 || g_strcmp0(path, "/inpaint") == 0)
		{
			/* This one carries its own data: prefix. */
			reply_json(msg, 200, g_strdup_printf("{\"image\":{\"type\":\"base64\",\"base64\":"
			                                     "\"data:image/png;base64,%s\",\"format\":\"png\"},"
			                                     "\"usage\":{\"type\":\"usd\",\"usd\":0.007}}",
			                                     tiny_png_b64));
			return;
		}
		if (g_strcmp0(path, "/create-image-pixen") == 0)
		{
			/* A bare payload, billed to a subscription. */
			reply_json(msg, 200, g_strdup_printf("{\"image\":{\"type\":\"base64\",\"base64\":\"%s\","
			                                     "\"format\":\"png\"},\"usage\":{\"type\":"
			                                     "\"generations\",\"usd\":null,\"generations\":1}}",
			                                     tiny_png_b64));
			return;
		}
		if (g_strcmp0(path, "/animate-with-text") == 0)
		{
			reply_json(msg, 200, g_strdup_printf("{\"images\":[{\"type\":\"base64\",\"base64\":\"%s\"},"
			                                     "{\"type\":\"base64\",\"base64\":\"%s\"}],"
			                                     "\"usage\":{\"type\":\"usd\",\"usd\":0.01}}",
			                                     tiny_png_b64, tiny_png_b64));
			return;
		}
		if (g_strcmp0(path, "/create-character-with-8-directions") == 0 ||
		    g_strcmp0(path, "/create-character-with-4-directions") == 0)
		{
			media_mock_reply(msg, 200, "{\"background_job_id\":\"bg-c8\",\"character_id\":\"char-1\","
			                           "\"status\":\"processing\",\"usage\":{\"type\":\"usd\","
			                           "\"usd\":0.02}}");
			return;
		}
		/* As CreateCharacterAnimationResponse has it: no character_id. */
		if (g_strcmp0(path, "/characters/animations") == 0)
		{
			media_mock_reply(msg, 200, "{\"background_job_ids\":[\"bg-a1\",\"bg-a2\"],"
			                           "\"directions\":[\"south\",\"east\"],"
			                           "\"animation_group_id\":\"grp-1\",\"status\":\"processing\"}");
			return;
		}
		if (g_strcmp0(path, "/create-tileset") == 0)
		{
			media_mock_reply(msg, 202, "{\"background_job_id\":\"bg-t\",\"tileset_id\":\"ts-1\","
			                           "\"status\":\"processing\"}");
			return;
		}
		if (g_strcmp0(path, "/create-isometric-tile") == 0)
		{
			media_mock_reply(msg, 202, "{\"background_job_id\":\"bg-i\",\"tile_id\":\"iso-1\","
			                           "\"status\":\"processing\"}");
			return;
		}
		if (g_strcmp0(path, "/map-objects") == 0)
		{
			media_mock_reply(msg, 200, "{\"background_job_id\":\"bg-m\",\"object_id\":\"obj-1\","
			                           "\"status\":\"processing\"}");
			return;
		}
	}

	if (g_strcmp0(method, "DELETE") == 0)
	{
		if (g_strcmp0(path, "/background-jobs/bg-run") == 0)
		{
			media_mock_reply(msg, 200, "{\"id\":\"bg-run\",\"status\":\"failed\","
			                           "\"message\":\"Cancelled\"}");
			return;
		}
		if (g_strcmp0(path, "/background-jobs/bg-done") == 0)
		{
			media_mock_reply(msg, 400, "{\"detail\":\"Job already completed\"}");
			return;
		}
	}

	if (g_strcmp0(method, "GET") == 0)
	{
		if (g_strcmp0(path, "/balance") == 0)
		{
			media_mock_reply(msg, 200, "{\"credits\":{\"type\":\"usd\",\"usd\":12.5},"
			                           "\"subscription\":{\"type\":\"generations\",\"status\":"
			                           "\"active\",\"plan\":\"tier 1\",\"generations\":180,"
			                           "\"total\":2000}}");
			return;
		}
		if (g_strcmp0(path, "/background-jobs/bg-c8") == 0)
		{
			if (hit == 1)
				media_mock_reply(msg, 200, "{\"id\":\"bg-c8\",\"status\":\"processing\","
				                           "\"created_at\":\"x\",\"last_response\":"
				                           "{\"queue_position\":2,\"estimated_wait_seconds\":30}}");
			else if (hit == 2)
				media_mock_reply(msg, 200, "{\"id\":\"bg-c8\",\"status\":\"processing\","
				                           "\"created_at\":\"x\",\"last_response\":{}}");
			else
				media_mock_reply(msg, 200, "{\"id\":\"bg-c8\",\"status\":\"completed\","
				                           "\"created_at\":\"x\",\"usage\":{\"type\":\"usd\","
				                           "\"usd\":0.02},\"last_response\":"
				                           "{\"character_id\":\"char-1\"}}");
			return;
		}
		if (g_strcmp0(path, "/background-jobs/bg-a1") == 0 ||
		    g_strcmp0(path, "/background-jobs/bg-t") == 0 ||
		    g_strcmp0(path, "/background-jobs/bg-i") == 0 ||
		    g_strcmp0(path, "/background-jobs/bg-m2") == 0)
		{
			media_mock_reply(msg, 200, "{\"id\":\"j\",\"status\":\"completed\",\"created_at\":\"x\"}");
			return;
		}
		if (g_strcmp0(path, "/background-jobs/bg-m") == 0)
		{
			media_mock_reply(msg, 200, "{\"id\":\"bg-m\",\"status\":\"failed\",\"created_at\":\"x\","
			                           "\"last_response\":{\"error\":\"Refused by moderation\"}}");
			return;
		}
		if (g_strcmp0(path, "/background-jobs/bg-weird") == 0)
		{
			media_mock_reply(msg, 200, "{\"id\":\"bg-weird\",\"status\":\"teleporting\","
			                           "\"created_at\":\"x\"}");
			return;
		}
		if (g_strcmp0(path, "/characters/char-1") == 0)
		{
			reply_json(msg, 200, character_body());
			return;
		}
		if (g_strcmp0(path, "/tilesets/ts-1") == 0)
		{
			reply_json(msg, 200, g_strdup_printf(
				"{\"tileset\":{\"total_tiles\":1,\"tile_size\":{\"width\":16,\"height\":16},"
				"\"terrain_types\":[\"lower\",\"upper\"],\"tiles\":[{\"id\":\"t1\",\"name\":\"none\","
				"\"image\":{\"type\":\"base64\",\"base64\":\"%s\"},\"corners\":{\"NW\":\"upper\","
				"\"NE\":\"upper\",\"SW\":\"upper\",\"SE\":\"upper\"}}]},\"metadata\":{},"
				"\"sheet\":{\"layout\":\"tileset15\",\"image\":{\"type\":\"base64\",\"base64\":\"%s\"},"
				"\"width\":64,\"height\":64}}", tiny_png_b64, tiny_png_b64));
			return;
		}
		if (g_strcmp0(path, "/isometric-tiles/iso-1") == 0)
		{
			if (hit == 1)
			{
				media_mock_reply(msg, 423, "{\"detail\":\"Still generating\"}");
				return;
			}
			reply_json(msg, 200, g_strdup_printf("{\"image\":{\"type\":\"base64\",\"base64\":\"%s\"}}",
			                                     tiny_png_b64));
			return;
		}
		if (g_strcmp0(path, "/map-objects/obj-2") == 0)
		{
			media_mock_reply(msg, 410, "{\"detail\":\"Generation failed permanently\"}");
			return;
		}
	}

	media_mock_reply(msg, 404, "{\"detail\":\"Not found\"}");
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static AiPixellabClient *
new_client(void)
{
	AiPixellabClient *client = ai_pixellab_client_new_with_api_key(TEST_KEY);

	ai_pixellab_client_set_base_url(client, media_mock->base_url);
	ai_pixellab_client_set_timeout(client, 10);
	return client;
}

static void
assert_no_key(const gchar *text)
{
	g_assert_nonnull(text);
	g_assert_null(strstr(text, TEST_KEY));
}

static void
set_size(
	AiMediaRequest *request,
	gint            width,
	gint            height
){
	g_autofree gchar *json = g_strdup_printf("{\"width\":%d,\"height\":%d}", width, height);
	g_autoptr(JsonNode) size = json_from_string(json, NULL);

	/* set_param_json() copies the node. */
	ai_media_request_set_param_json(request, "image_size", size);
}

static AiMediaRequest *
request_for(
	const gchar *operation,
	const gchar *prompt,
	gint         width,
	gint         height
){
	AiMediaRequest *request = ai_media_request_new(operation, prompt);

	if (width > 0)
	{
		set_size(request, width, height);
	}
	return request;
}

/* ---------------------------------------------------------------------- */
/* Synchronous operations                                                 */
/* ---------------------------------------------------------------------- */

static void
test_sync_image(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(AiMediaRequest) request = request_for("image", "a small red potion", 64, 64);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) body = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *job_id = NULL;
	const gchar *url;

	ai_media_request_set_param_string(request, "outline", "single color black outline");
	ai_media_request_set_param_boolean(request, "no_background", TRUE);

	job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_prefix(job_id, "sync:"));

	/* The prompt is the endpoint's description; ai-glib's own words never go. */
	body = media_mock_body_object("POST /create-image-pixflux");
	g_assert_cmpstr(json_object_get_string_member(body, "description"), ==, "a small red potion");
	g_assert_false(json_object_has_member(body, "prompt"));
	g_assert_false(json_object_has_member(body, "operation"));
	g_assert_cmpstr(json_object_get_string_member(body, "outline"), ==, "single color black outline");
	g_assert_true(json_object_get_boolean_member(body, "no_background"));
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(body, "image_size"),
	                                           "width"), ==, 64);

	/* Finished at submit: poll answers at once, without a request. */
	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(job), ==, 7000);
	g_assert_cmpint(ai_media_job_get_credits(job), ==, AI_MEDIA_COST_UNKNOWN);

	/* Its own "data:image/png;base64," prefix is not doubled. */
	url = ai_media_job_get_result_url(job, "image");
	g_assert_nonnull(url);
	g_assert_true(g_str_has_prefix(url, "data:image/png;base64,iVBOR"));
	g_assert_null(strstr(url + 5, "data:"));

	bytes = media_download(AI_MEDIA_GENERATOR(client), url, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes),
	                tiny_png, sizeof tiny_png);
}

static void
test_pixen(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(GError) error = NULL;
	guint i;

	/* operation image + model pixen, and the model alone. */
	for (i = 0; i < 2; i++)
	{
		g_autoptr(AiMediaRequest) request = request_for(AI_PIXELLAB_MODEL_PIXEN, "cute dragon",
		                                                128, 128);
		g_autoptr(AiMediaJob) job = NULL;
		g_autoptr(GBytes) bytes = NULL;
		g_autofree gchar *job_id = NULL;

		if (i == 0)
		{
			ai_media_request_set_param_string(request, "operation", "image");
		}
		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(media_mock_hits("POST /create-image-pixen"), ==, i + 1);

		job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
		g_assert_no_error(error);
		/* Billed to the subscription: a generation, no dollar figure. */
		g_assert_cmpint(ai_media_job_get_credits(job), ==, 1);
		g_assert_cmpint(ai_media_job_get_cost_micro_usd(job), ==, AI_MEDIA_COST_UNKNOWN);

		/* A bare payload becomes a data URL too. */
		bytes = media_download(AI_MEDIA_GENERATOR(client),
		                       ai_media_job_get_result_url(job, "image"), &error);
		g_assert_no_error(error);
		g_assert_cmpuint(g_bytes_get_size(bytes), ==, sizeof tiny_png);
	}
}

static void
test_animate_frames(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(AiMediaRequest) request = request_for("animate", "human mage", 64, 64);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autofree gchar *job_id = NULL;

	ai_media_request_set_param_string(request, "action", "walk");
	job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_no_error(error);

	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(ai_media_job_get_result_url(job, "frame:0"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "frame:1"));
	g_assert_null(ai_media_job_get_result_url(job, "frame:2"));
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(job), ==, 10000);
}

/* ---------------------------------------------------------------------- */
/* Local refusals                                                         */
/* ---------------------------------------------------------------------- */

static void
test_size_limits(void)
{
	static const struct
	{
		const gchar *operation;
		gint         width;
		gint         height;
		const gchar *expect;
	} refused[] = {
		{ "image", 401, 64, "16 to 400" },
		{ "image", 8, 8, "16 to 400" },
		{ "pixen", 18, 18, "divisible by 4" },
		{ "pixen", 772, 64, "16 to 768" },
		{ "bitforge", 201, 32, "16 to 200" },
		{ "rotate", 300, 300, "16 to 200" },
		{ "inpaint", 15, 64, "16 to 200" },
		{ "animate", 32, 32, "only 64x64" },
		{ "character-4", 129, 64, "16 to 128" },
		{ "character-8", 64, 200, "16 to 128" },
		{ "isometric-tile", 65, 32, "16 to 64" },
		{ "map-object", 31, 64, "32 to 400" },
		{ "map-object", 401, 64, "32 to 400" },
		{ "image", 0, 0, "needs image_size" },
	};
	g_autoptr(AiPixellabClient) client = new_client();
	guint i;

	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		/* rotate takes no prompt, and would be refused for that first. */
		g_autoptr(AiMediaRequest) request = request_for(refused[i].operation,
		                                                g_strcmp0(refused[i].operation, "rotate") != 0
		                                                    ? "x" : NULL,
		                                                refused[i].width, refused[i].height);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *job_id = NULL;

		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		if (strstr(error->message, refused[i].expect) == NULL)
		{
			g_test_message("%s %dx%d: %s", refused[i].operation, refused[i].width,
			               refused[i].height, error->message);
		}
		g_assert_nonnull(strstr(error->message, refused[i].expect));
	}

	/* Nothing above was sent. */
	g_assert_cmpuint(media_mock_hits("POST /rotate"), ==, 0);
	g_assert_cmpuint(media_mock_hits("POST /create-isometric-tile"), ==, 0);
	g_assert_cmpuint(media_mock_hits("POST /map-objects"), ==, 0);

	/* A map object's size is optional; the limit applies only when it is given. */
	{
		g_autoptr(AiMediaRequest) request = request_for("map-object", "wooden chest", 0, 0);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);

		g_assert_no_error(error);
		g_assert_cmpstr(job_id, ==, "map-object:obj-1:-:bg-m");
	}
}

static void
test_prompt_routing(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *job_id = NULL;

	/* rotate has no description: a prompt is refused, not dropped. */
	{
		g_autoptr(AiMediaRequest) request = request_for("rotate", "turn around", 64, 64);

		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "takes no prompt"));
		g_clear_error(&error);
	}

	/* character-animate: the prompt is the motion. */
	{
		g_autoptr(AiMediaRequest) request = request_for("character-animate", "swings a sword", 0, 0);
		g_autoptr(JsonObject) body = NULL;

		ai_media_request_set_param_string(request, "character_id", "char-1");
		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(job_id, ==, "character-animate:char-1:grp-1:bg-a1,bg-a2");
		body = media_mock_body_object("POST /characters/animations");
		g_assert_cmpstr(json_object_get_string_member(body, "action_description"), ==, "swings a sword");
		g_assert_cmpstr(json_object_get_string_member(body, "character_id"), ==, "char-1");
		g_assert_false(json_object_has_member(body, "prompt"));
		g_clear_pointer(&job_id, g_free);
	}

	/* An explicit description wins over the prompt. */
	{
		g_autoptr(AiMediaRequest) request = request_for("bitforge", "the prompt", 32, 32);
		g_autoptr(JsonObject) body = NULL;

		ai_media_request_set_param_string(request, "description", "the parameter");
		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_no_error(error);
		body = media_mock_body_object("POST /create-image-bitforge");
		g_assert_cmpstr(json_object_get_string_member(body, "description"), ==, "the parameter");
	}
}

/* ---------------------------------------------------------------------- */
/* Background jobs                                                        */
/* ---------------------------------------------------------------------- */

static void
test_character(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(AiMediaRequest) request = request_for("character-8", "a knight in plate", 48, 48);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) zip = NULL;
	g_autofree gchar *job_id = NULL;
	g_autofree gchar *sheet = NULL;
	g_autofree gchar *auth = NULL;
	AiMediaJob *job;

	job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(job_id, ==, "character-8:char-1:-:bg-c8");

	/* Waiting for a GPU is queued; generating is running. */
	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_QUEUED);
	g_assert_cmpstr(ai_media_job_get_provider_status(job), ==, "processing");
	/* The submit's usage stands in until the job reports its own. */
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(job), ==, 20000);
	g_clear_object(&job);

	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_RUNNING);
	g_clear_object(&job);

	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(job), ==, 20000);
	g_assert_nonnull(ai_media_job_get_result_url(job, "south"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "north-west"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "east"));
	sheet = g_strdup_printf("%s/characters/char-1/spritesheet", media_mock->base_url);
	g_assert_cmpstr(ai_media_job_get_result_url(job, "spritesheet"), ==, sheet);
	g_assert_cmpuint(ai_media_job_get_n_result_urls(job), ==, 9);

	/* The export is public by character id: fetched without the token. */
	zip = media_download(AI_MEDIA_GENERATOR(client), sheet, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(zip, NULL), 4, "PK\x03\x04", 4);
	auth = media_mock_header("GET /characters/char-1/spritesheet", "authorization");
	g_assert_null(auth);
	g_clear_object(&job);
}

static void
test_character_animate(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;

	/*
	 * One job per direction; bg-a2 has been cleaned up (404), which is
	 * not a failure.  A fresh client resumes from the id alone.
	 */
	job = media_poll(AI_MEDIA_GENERATOR(client), "character-animate:char-1:grp-1:bg-a1,bg-a2",
	                 &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpuint(media_mock_hits("GET /background-jobs/bg-a2"), >=, 1);
	g_assert_nonnull(ai_media_job_get_result_url(job, "south:0"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "south:1"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "east:0"));
	/* Only this animation's group, not the idle one beside it. */
	g_assert_cmpuint(ai_media_job_get_n_result_urls(job), ==, 4);
	g_assert_nonnull(ai_media_job_get_result_url(job, "spritesheet"));
	g_clear_object(&job);

	/* Without a group, every animation, named by its type. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "character-animate:char-1:-:bg-a1", &error);
	g_assert_no_error(error);
	g_assert_nonnull(ai_media_job_get_result_url(job, "walking:south:1"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "idle:south:0"));
}

static void
test_tileset(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(AiMediaRequest) request = ai_media_request_new("tileset-topdown", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autoptr(GBytes) data = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GBytes) sheet = NULL;
	g_autofree gchar *job_id = NULL;
	g_autofree gchar *query = NULL;

	ai_media_request_set_param_string(request, "lower_description", "ocean");
	ai_media_request_set_param_string(request, "upper_description", "sand");
	job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(job_id, ==, "tileset-topdown:ts-1:-:bg-t");

	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);

	/* Asked with a layout, or the reply carries no sheet. */
	query = media_mock_query("GET /tilesets/ts-1");
	g_assert_cmpstr(query, ==, "layout=tileset15");

	sheet = media_download(AI_MEDIA_GENERATOR(client), ai_media_job_get_result_url(job, "sheet"),
	                       &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(sheet), ==, sizeof tiny_png);

	/* The tiles and their corners, as JSON, for placing them. */
	g_assert_true(g_str_has_prefix(ai_media_job_get_result_url(job, "tileset"),
	                               "data:application/json;base64,"));
	data = media_download(AI_MEDIA_GENERATOR(client), ai_media_job_get_result_url(job, "tileset"),
	                      &error);
	g_assert_no_error(error);
	g_assert_true(json_parser_load_from_data(parser, g_bytes_get_data(data, NULL),
	                                         (gssize)g_bytes_get_size(data), NULL));
	g_assert_true(json_object_has_member(json_node_get_object(json_parser_get_root(parser)),
	                                     "tileset"));
}

static void
test_isometric_locked(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(GError) error = NULL;
	AiMediaJob *job;

	/* 423 while the tile is stored: running, not an error. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "isometric-tile:iso-1:-:bg-i", &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_RUNNING);
	g_clear_object(&job);

	job = media_poll(AI_MEDIA_GENERATOR(client), "isometric-tile:iso-1:-:bg-i", &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_nonnull(ai_media_job_get_result_url(job, "image"));
	g_clear_object(&job);
}

static void
test_failures(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(GError) error = NULL;
	AiMediaJob *job;

	/* A failed background job is a failed job, with its reason. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "map-object:obj-1:-:bg-m", &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_FAILED);
	g_assert_cmpstr(ai_media_job_get_error_message(job), ==, "Refused by moderation");
	g_clear_object(&job);

	/* 410 Gone from the object: failed for good. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "map-object:obj-2:-:bg-m2", &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_FAILED);
	g_assert_cmpstr(ai_media_job_get_error_message(job), ==, "Generation failed permanently");
	g_clear_object(&job);

	/* An unknown word is an error, never a guess. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "map-object:obj-1:-:bg-weird", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	g_assert_nonnull(strstr(error->message, "teleporting"));
}

/* ---------------------------------------------------------------------- */
/* Errors                                                                 */
/* ---------------------------------------------------------------------- */

static GError *
submit_described(
	AiPixellabClient *client,
	const gchar      *description
){
	g_autoptr(AiMediaRequest) request = request_for("image", description, 32, 32);
	g_autofree gchar *job_id = NULL;
	GError *error = NULL;

	job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_null(job_id);
	g_assert_nonnull(error);
	assert_no_key(error->message);
	return error;
}

static void
test_error_envelopes(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(AiPixellabClient) wrong = ai_pixellab_client_new_with_api_key(TEST_KEY "-x");
	GError *error;

	error = submit_described(client, "broke");
	g_assert_error(error, AI_ERROR, AI_ERROR_INSUFFICIENT_QUOTA);
	g_assert_nonnull(strstr(error->message, "insufficient credits"));
	g_error_free(error);

	error = submit_described(client, "busy");
	g_assert_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "HTTP 429"));
	g_assert_nonnull(strstr(error->message, "retry after 3 s"));
	g_error_free(error);

	error = submit_described(client, "flooded");
	g_assert_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "HTTP 529"));
	g_assert_nonnull(strstr(error->message, "rate limit exceeded"));
	g_error_free(error);

	/* FastAPI's validation list, flattened to the field and the reason. */
	error = submit_described(client, "invalid");
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "width: Input should be less than or equal to 400"));
	g_error_free(error);

	/* A 401 that echoes the token: redacted. */
	ai_pixellab_client_set_base_url(wrong, media_mock->base_url);
	error = submit_described(wrong, "anything");
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_assert_nonnull(strstr(error->message, "[REDACTED]"));
	g_error_free(error);
}

static void
test_refused_locally(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(AiPixellabClient) keyless = ai_pixellab_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *job_id = NULL;
	AiMediaJob *job;

	{
		g_autoptr(AiMediaRequest) request = request_for("ui-asset", "x", 32, 32);

		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "character-8"));
		g_clear_error(&error);
	}

	/* Ids are spliced into paths. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "character-8:../balance:-:bg-c8", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	job = media_poll(AI_MEDIA_GENERATOR(client), "image:x:-:bg-c8", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* A synchronous job belongs to the client that made it. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "sync:not-ours", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	{
		g_autoptr(AiMediaRequest) request = request_for("image", "x", 32, 32);

		ai_pixellab_client_set_base_url(keyless, media_mock->base_url);
		job_id = media_submit(AI_MEDIA_GENERATOR(keyless), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
		g_clear_error(&error);
	}

	/* No estimate endpoint for these operations. */
	g_assert_false(ai_media_generator_can_estimate(AI_MEDIA_GENERATOR(client)));
}

/* ---------------------------------------------------------------------- */
/* Cancel, balance, input images                                          */
/* ---------------------------------------------------------------------- */

static void
test_cancel(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(AiMediaRequest) request = request_for("image", "x", 32, 32);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *sync_id = NULL;

	g_assert_true(media_cancel(AI_MEDIA_GENERATOR(client), "map-object:obj-9:-:bg-run", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(media_mock_hits("DELETE /background-jobs/bg-run"), ==, 1);

	g_assert_false(media_cancel(AI_MEDIA_GENERATOR(client), "map-object:obj-9:-:bg-done", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "already finished"));
	g_clear_error(&error);

	sync_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_no_error(error);
	g_assert_false(media_cancel(AI_MEDIA_GENERATOR(client), sync_id, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "nothing to cancel"));
}

static void
test_balance(void)
{
	g_autoptr(AiPixellabClient) client = new_client();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *status = NULL;
	gint64 micro_usd = 0;
	gdouble generations = 0;
	gdouble total = 0;

	ai_pixellab_client_balance_async(client, NULL, media_store_result, &result);
	media_run_until(&result);
	g_assert_true(ai_pixellab_client_balance_finish(client, result, &micro_usd, &generations,
	                                                &total, &status, &error));
	g_assert_no_error(error);
	g_assert_cmpint(micro_usd, ==, 12500000);
	g_assert_cmpfloat(generations, ==, 180);
	g_assert_cmpfloat(total, ==, 2000);
	g_assert_cmpstr(status, ==, "active");
}

static void
test_image_node(void)
{
	g_autoptr(GBytes) png = g_bytes_new_static(tiny_png, sizeof tiny_png);
	g_autoptr(GBytes) text = g_bytes_new_static("plain text", 10);
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *image;

	node = ai_pixellab_image_node_new(png, &error);
	g_assert_no_error(error);
	image = json_node_get_object(node);
	g_assert_cmpstr(json_object_get_string_member(image, "type"), ==, "base64");
	g_assert_cmpstr(json_object_get_string_member(image, "format"), ==, "png");
	g_assert_cmpstr(json_object_get_string_member(image, "base64"), ==, tiny_png_b64);
	g_clear_pointer(&node, json_node_unref);

	node = ai_pixellab_image_node_new(text, &error);
	g_assert_null(node);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_logs_clean(void)
{
	g_assert_cmpuint(media_log_length(), >, 0);
	g_assert_true(media_log_contains("pixellab:"));
	g_assert_false(media_log_contains(TEST_KEY));
}

int
main(
	int    argc,
	char **argv
){
	int status;

	tiny_png_b64 = g_base64_encode(tiny_png, sizeof tiny_png);

	media_log_capture_begin();
	g_test_init(&argc, &argv, NULL);
	media_log_capture_handler();
	media_mock_start(pixellab_route);

	g_test_add_func("/pixellab/sync-image", test_sync_image);
	g_test_add_func("/pixellab/pixen", test_pixen);
	g_test_add_func("/pixellab/animate-frames", test_animate_frames);
	g_test_add_func("/pixellab/size-limits", test_size_limits);
	g_test_add_func("/pixellab/prompt-routing", test_prompt_routing);
	g_test_add_func("/pixellab/character", test_character);
	g_test_add_func("/pixellab/character-animate", test_character_animate);
	g_test_add_func("/pixellab/tileset", test_tileset);
	g_test_add_func("/pixellab/isometric-locked", test_isometric_locked);
	g_test_add_func("/pixellab/failures", test_failures);
	g_test_add_func("/pixellab/error-envelopes", test_error_envelopes);
	g_test_add_func("/pixellab/refused-locally", test_refused_locally);
	g_test_add_func("/pixellab/cancel", test_cancel);
	g_test_add_func("/pixellab/balance", test_balance);
	g_test_add_func("/pixellab/image-node", test_image_node);
	g_test_add_func("/pixellab/logs-clean", test_logs_clean);

	status = g_test_run();

	media_mock_stop();
	g_string_free(media_log_text, TRUE);
	g_free(tiny_png_b64);
	return status;
}
