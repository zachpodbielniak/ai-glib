/*
 * test-meshy.c - AiMeshyClient, and AiMediaJob's named results
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Hermetic: every request goes to a loopback mock of Meshy's API on its
 * own thread (tests/media-mock.h).  The mock answers each operation's
 * create, retrieve and delete, the balance and the animation library, and
 * the documented error envelopes.  The bodies, statuses and shapes are
 * from Meshy's OpenAPI document (docs.meshy.ai/openapi.json, 2026-11-23);
 * no request ever reaches Meshy, and Meshy's shared test key has been
 * retired since 2026-09-16 in any case.
 *
 * The key is checked negatively throughout: the mock echoes it in a 401,
 * and no error and no log line may contain it.
 */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "ai-glib.h"
#include "media-mock.h"

#define TEST_KEY "msy_SHOULD-NEVER-APPEAR-0123456789"
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

/* ---------------------------------------------------------------------- */
/* The mock API                                                           */
/* ---------------------------------------------------------------------- */

/* A finished task with every family of output, for the result names. */
static gchar *
full_outputs(const gchar *id)
{
	const gchar *b = media_mock->base_url;

	return g_strdup_printf(
		"{\"id\":\"%s\",\"status\":\"SUCCEEDED\",\"progress\":100,\"created_at\":1,"
		"\"expires_at\":1764000000000,\"consumed_credits\":10,\"task_error\":null,"
		"\"model_urls\":{\"glb\":\"%s/cdn/m.glb\",\"fbx\":\"%s/cdn/m.fbx\","
		"\"obj\":\"%s/cdn/m.obj\",\"usdz\":\"%s/cdn/m.usdz\",\"stl\":\"\"},"
		"\"thumbnail_url\":\"%s/cdn/t.png\",\"alpha_thumbnail_url\":\"%s/cdn/ta.png\","
		"\"texture_urls\":[{\"base_color\":\"%s/cdn/bc.png\",\"normal\":\"%s/cdn/n.png\"},"
		"{\"base_color\":\"%s/cdn/bc2.png\"}]}",
		id, b, b, b, b, b, b, b, b, b);
}

static void
meshy_route(
	SoupServerMessage *msg,
	const gchar       *method,
	const gchar       *path,
	const gchar       *body,
	guint              hit
){
	SoupMessageHeaders *headers = soup_server_message_get_request_headers(msg);
	const gchar *auth = soup_message_headers_get_one(headers, "Authorization");
	g_autofree gchar *reply = NULL;

	/* Storage: no credential expected, none must arrive. */
	if (g_str_has_prefix(path, "/cdn/"))
	{
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "model/gltf-binary", SOUP_MEMORY_STATIC,
		                                 "glTF\0BIN", 8);
		return;
	}

	if (g_strcmp0(auth, TEST_AUTH) != 0)
	{
		reply = g_strdup_printf("{\"message\":\"Invalid API key: %s\"}",
		                        auth != NULL ? auth : "(none)");
		media_mock_reply(msg, 401, reply);
		return;
	}

	/* Creates: the request's name picks an error, as Meshy documents them. */
	if (g_strcmp0(method, "POST") == 0)
	{
		if (strstr(body, "\"name\":\"broke\"") != NULL)
		{
			media_mock_reply(msg, 402, "{\"message\":\"Insufficient credits\"}");
			return;
		}
		if (strstr(body, "\"name\":\"busy\"") != NULL)
		{
			soup_message_headers_replace(soup_server_message_get_response_headers(msg),
			                             "Retry-After", "7");
			media_mock_reply(msg, 429, "{\"message\":\"Rate limit exceeded\"}");
			return;
		}
		if (strstr(body, "\"name\":\"capped\"") != NULL)
		{
			media_mock_reply(msg, 429, "{\"message\":\"NoMorePendingTasks: too many tasks\"}");
			return;
		}
		if (strstr(body, "\"name\":\"down\"") != NULL)
		{
			media_mock_reply(msg, 503, "{\"message\":\"Temporarily unavailable\"}");
			return;
		}
		if (g_strcmp0(path, "/openapi/v2/text-to-3d") == 0)
		{
			/* Recorded per mode too: preview and refine share the path. */
			g_autofree gchar *key = g_strdup_printf("POST %s %s", path,
			                                        strstr(body, "\"refine\"") ? "refine" : "preview");

			g_mutex_lock(&media_mock->lock);
			g_hash_table_insert(media_mock->bodies, g_strdup(key), g_strdup(body));
			g_mutex_unlock(&media_mock->lock);
			media_mock_reply(msg, 202, strstr(body, "\"refine\"") ? "{\"result\":\"rf-1\"}"
			                                                      : "{\"result\":\"pv-1\"}");
			return;
		}
		if (g_strcmp0(path, "/openapi/v1/image-to-3d") == 0)
		{
			media_mock_reply(msg, 202, "{\"result\":\"it-1\"}");
			return;
		}
		if (g_str_has_prefix(path, "/openapi/v1/"))
		{
			reply = g_strdup_printf("{\"result\":\"%s-1\"}", path + strlen("/openapi/v1/"));
			media_mock_reply(msg, 202, reply);
			return;
		}
	}

	if (g_strcmp0(method, "DELETE") == 0)
	{
		if (g_str_has_suffix(path, "/pending"))
		{
			media_mock_reply(msg, 200, "{}");
			return;
		}
		if (g_str_has_suffix(path, "/running"))
		{
			media_mock_reply(msg, 409, "{\"message\":\"Task is in progress\"}");
			return;
		}
	}

	if (g_strcmp0(method, "GET") == 0)
	{
		if (g_strcmp0(path, "/openapi/v1/balance") == 0)
		{
			media_mock_reply(msg, 200, "{\"balance\":1234,\"monthly_credits\":1000,"
			                           "\"permanent_credits\":234,\"monthly_resets_at\":null}");
			return;
		}
		if (g_strcmp0(path, "/openapi/v1/animations/library") == 0)
		{
			media_mock_reply(msg, 200, "[{\"action_id\":1,\"name\":\"Walk\",\"key\":\"walk\","
			                           "\"category\":\"WalkAndRun\",\"sub_category\":\"Walk\","
			                           "\"preview_url\":\"https://x/1.gif\",\"rig_type\":\"biped\"}]");
			return;
		}
		/* A chained Text to 3D job: the preview runs once, then succeeds. */
		if (g_strcmp0(path, "/openapi/v2/text-to-3d/pv-1") == 0 ||
		    g_strcmp0(path, "/openapi/v2/text-to-3d/pv-lost") == 0)
		{
			if (hit == 1 && g_str_has_suffix(path, "pv-1"))
			{
				media_mock_reply(msg, 200, "{\"id\":\"pv-1\",\"status\":\"IN_PROGRESS\","
				                           "\"progress\":40,\"created_at\":1}");
				return;
			}
			media_mock_reply(msg, 200, "{\"id\":\"pv-1\",\"status\":\"SUCCEEDED\","
			                           "\"progress\":100,\"created_at\":1,\"consumed_credits\":20,"
			                           "\"model_urls\":{\"glb\":\"https://x/preview.glb\"}}");
			return;
		}
		if (g_strcmp0(path, "/openapi/v2/text-to-3d/rf-1") == 0)
		{
			reply = full_outputs("rf-1");
			media_mock_reply(msg, 200, reply);
			return;
		}
		if (g_str_has_prefix(path, "/openapi/v1/image-to-3d/"))
		{
			const gchar *id = path + strlen("/openapi/v1/image-to-3d/");

			if (g_strcmp0(id, "pending") == 0)
				reply = g_strdup("{\"id\":\"pending\",\"status\":\"PENDING\",\"progress\":0,\"created_at\":1}");
			else if (g_strcmp0(id, "running") == 0)
				reply = g_strdup("{\"id\":\"running\",\"status\":\"IN_PROGRESS\",\"progress\":5,\"created_at\":1}");
			else if (g_strcmp0(id, "failed") == 0)
				reply = g_strdup("{\"id\":\"failed\",\"status\":\"FAILED\",\"progress\":0,\"created_at\":1,"
				                 "\"consumed_credits\":0,\"task_error\":{\"message\":\"Could not "
				                 "reconstruct a mesh\"}}");
			else if (g_strcmp0(id, "canceled") == 0)
				reply = g_strdup("{\"id\":\"canceled\",\"status\":\"CANCELED\",\"progress\":0,\"created_at\":1}");
			else if (g_strcmp0(id, "weird") == 0)
				reply = g_strdup("{\"id\":\"weird\",\"status\":\"TELEPORTING\",\"progress\":0,\"created_at\":1}");
			else if (g_strcmp0(id, "it-1") == 0)
				reply = full_outputs("it-1");
			else
			{
				media_mock_reply(msg, 404, "{\"message\":\"Task not found\"}");
				return;
			}
			media_mock_reply(msg, 200, reply);
			return;
		}
		if (g_strcmp0(path, "/openapi/v1/rigging/rigging-1") == 0)
		{
			const gchar *b = media_mock->base_url;

			if (hit == 1)
			{
				media_mock_reply(msg, 200, "{\"id\":\"rigging-1\",\"status\":\"PENDING\","
				                           "\"progress\":0,\"created_at\":1}");
				return;
			}
			reply = g_strdup_printf(
				"{\"id\":\"rigging-1\",\"status\":\"SUCCEEDED\",\"progress\":100,\"created_at\":1,"
				"\"consumed_credits\":5,\"result\":{\"rigged_character_glb_url\":\"%s/cdn/r.glb\","
				"\"rigged_character_fbx_url\":\"%s/cdn/r.fbx\",\"basic_animations\":{"
				"\"walking_glb_url\":\"%s/cdn/w.glb\",\"walking_fbx_url\":\"%s/cdn/w.fbx\","
				"\"running_glb_url\":\"%s/cdn/run.glb\"}}}", b, b, b, b, b);
			media_mock_reply(msg, 200, reply);
			return;
		}
		if (g_strcmp0(path, "/openapi/v1/animations/animations-1") == 0)
		{
			reply = g_strdup_printf(
				"{\"id\":\"animations-1\",\"status\":\"SUCCEEDED\",\"progress\":100,\"created_at\":1,"
				"\"consumed_credits\":6,\"result\":{\"animation_glb_url\":\"%s/cdn/a.glb\","
				"\"animation_fbx_url\":\"%s/cdn/a.fbx\"}}",
				media_mock->base_url, media_mock->base_url);
			media_mock_reply(msg, 200, reply);
			return;
		}
	}

	media_mock_reply(msg, 404, "{\"message\":\"Not found\"}");
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static AiMeshyClient *
new_client(void)
{
	AiMeshyClient *client = ai_meshy_client_new_with_api_key(TEST_KEY);

	ai_meshy_client_set_base_url(client, media_mock->base_url);
	ai_meshy_client_set_timeout(client, 10);
	return client;
}

static void
assert_no_key(const gchar *text)
{
	g_assert_nonnull(text);
	g_assert_null(strstr(text, TEST_KEY));
}

static AiMediaRequest *
request_for(
	const gchar *operation,
	const gchar *prompt
){
	AiMediaRequest *request = ai_media_request_new(operation, prompt);

	return request;
}

/* ---------------------------------------------------------------------- */
/* AiMediaJob                                                             */
/* ---------------------------------------------------------------------- */

static void
test_job_named_results(void)
{
	g_autoptr(AiMediaJob) job = ai_media_job_new("x", AI_MEDIA_JOB_STATE_DONE);
	const gchar * const *names;
	const gchar * const *urls;

	g_assert_cmpint(ai_media_job_get_credits(job), ==, AI_MEDIA_COST_UNKNOWN);
	g_assert_null(ai_media_job_get_result_names(job)[0]);
	g_assert_null(ai_media_job_get_result_url(job, "glb"));

	/* The plain list still works on its own, as ComfyUI and Higgsfield use it. */
	ai_media_job_add_result_url(job, "https://x/plain.png");
	ai_media_job_add_named_result(job, "glb", "https://x/m.glb");
	ai_media_job_add_named_result(job, "thumbnail", "https://x/t.png");
	ai_media_job_add_named_result(job, "glb", "https://x/m2.glb");

	names = ai_media_job_get_result_names(job);
	g_assert_cmpstr(names[0], ==, "glb");
	g_assert_cmpstr(names[1], ==, "thumbnail");
	g_assert_null(names[2]);

	/* A name added twice points at the newer URL; the list keeps both. */
	g_assert_cmpstr(ai_media_job_get_result_url(job, "glb"), ==, "https://x/m2.glb");
	urls = ai_media_job_get_result_urls(job);
	g_assert_cmpuint(ai_media_job_get_n_result_urls(job), ==, 4);
	g_assert_cmpstr(urls[0], ==, "https://x/plain.png");
	g_assert_cmpstr(urls[1], ==, "https://x/m.glb");
	g_assert_cmpstr(urls[3], ==, "https://x/m2.glb");

	ai_media_job_set_credits(job, 35);
	g_assert_cmpint(ai_media_job_get_credits(job), ==, 35);
}

/* ---------------------------------------------------------------------- */
/* Submit                                                                 */
/* ---------------------------------------------------------------------- */

static void
test_submit_bodies(void)
{
	static const struct
	{
		const gchar *operation;
		const gchar *path;
		const gchar *job_id;
	} cases[] = {
		{ "image-to-3d", "/openapi/v1/image-to-3d", "image-to-3d:it-1" },
		{ "multi-image-to-3d", "/openapi/v1/multi-image-to-3d", "multi-image-to-3d:multi-image-to-3d-1" },
		{ "retexture", "/openapi/v1/retexture", "retexture:retexture-1" },
		{ "remesh", "/openapi/v1/remesh", "remesh:remesh-1" },
		{ "rig", "/openapi/v1/rigging", "rig:rigging-1" },
		{ "animate", "/openapi/v1/animations", "animate:animations-1" },
	};
	g_autoptr(AiMeshyClient) client = new_client();
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(AiMediaRequest) request = ai_media_request_new("unused-model", NULL);
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonObject) body = NULL;
		g_autofree gchar *job_id = NULL;
		g_autofree gchar *key = g_strdup_printf("POST %s", cases[i].path);
		g_autofree gchar *auth = NULL;
		g_autofree gchar *nested = NULL;
		g_autoptr(JsonNode) post_process = NULL;

		/* The operation is a parameter here; the model is ignored for it. */
		ai_media_request_set_param_string(request, "operation", cases[i].operation);
		ai_media_request_set_param_string(request, "model_url", "https://example.com/m.glb");
		ai_media_request_set_param_int(request, "target_polycount", 5000);
		post_process = json_from_string("{\"operation_type\":\"change_fps\",\"fps\":24}", NULL);
		ai_media_request_set_param_json(request, "post_process", post_process);

		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(job_id, ==, cases[i].job_id);

		body = media_mock_body_object(key);
		/* ai-glib's word never reaches the wire; Meshy's parameters pass untouched. */
		g_assert_false(json_object_has_member(body, "operation"));
		g_assert_false(json_object_has_member(body, "model"));
		g_assert_cmpstr(json_object_get_string_member(body, "model_url"), ==,
		                "https://example.com/m.glb");
		g_assert_cmpint(json_object_get_int_member(body, "target_polycount"), ==, 5000);
		nested = json_to_string(json_object_get_member(body, "post_process"), FALSE);
		g_assert_nonnull(strstr(nested, "\"fps\":24"));

		auth = media_mock_header(key, "authorization");
		g_assert_cmpstr(auth, ==, TEST_AUTH);
	}

	/* The model names the operation when there is no parameter. */
	{
		g_autoptr(AiMediaRequest) request = request_for("image-to-3d", NULL);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *job_id = NULL;

		ai_media_request_set_image_url(request, "https://example.com/chair.png");
		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(job_id, ==, "image-to-3d:it-1");
	}
}

static void
test_text_to_3d_chain(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(AiMediaRequest) request = request_for("text-to-3d", "a carved wooden owl");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) preview = NULL;
	g_autoptr(JsonObject) refine = NULL;
	g_autofree gchar *job_id = NULL;
	AiMediaJob *job;

	ai_media_request_set_param_string(request, "ai_model", "meshy-7.1");
	ai_media_request_set_param_string(request, "texture_resolution", "4k");
	ai_media_request_set_param_string(request, "texture_prompt", "weathered oak");
	ai_media_request_set_param_string(request, "texture_model", "meshy-6");
	ai_media_request_set_param_boolean(request, "enable_pbr", TRUE);
	ai_media_request_set_param_string(request, "topology", "quad");

	job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(job_id, ==, "text-to-3d:pv-1");

	/* The preview gets the mesh parameters, and none a preview refuses. */
	preview = media_mock_body_object("POST /openapi/v2/text-to-3d preview");
	g_assert_cmpstr(json_object_get_string_member(preview, "mode"), ==, "preview");
	g_assert_cmpstr(json_object_get_string_member(preview, "prompt"), ==, "a carved wooden owl");
	g_assert_cmpstr(json_object_get_string_member(preview, "ai_model"), ==, "meshy-7.1");
	g_assert_cmpstr(json_object_get_string_member(preview, "topology"), ==, "quad");
	g_assert_false(json_object_has_member(preview, "texture_resolution"));
	g_assert_false(json_object_has_member(preview, "texture_model"));

	/* First poll: the preview is running. */
	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_RUNNING);
	g_assert_cmpstr(ai_media_job_get_provider_status(job), ==, "preview:IN_PROGRESS");
	g_clear_object(&job);

	/* Second: it succeeded, so the refine is created, once. */
	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_QUEUED);
	g_assert_cmpstr(ai_media_job_get_id(job), ==, "text-to-3d:pv-1:rf-1");
	g_assert_cmpstr(ai_media_job_get_provider_status(job), ==, "refine:PENDING");
	g_clear_object(&job);

	refine = media_mock_body_object("POST /openapi/v2/text-to-3d refine");
	g_assert_cmpstr(json_object_get_string_member(refine, "mode"), ==, "refine");
	g_assert_cmpstr(json_object_get_string_member(refine, "preview_task_id"), ==, "pv-1");
	g_assert_cmpstr(json_object_get_string_member(refine, "texture_resolution"), ==, "4k");
	g_assert_cmpstr(json_object_get_string_member(refine, "texture_prompt"), ==, "weathered oak");
	g_assert_cmpstr(json_object_get_string_member(refine, "ai_model"), ==, "meshy-6");
	g_assert_true(json_object_get_boolean_member(refine, "enable_pbr"));
	g_assert_false(json_object_has_member(refine, "prompt"));
	g_assert_false(json_object_has_member(refine, "topology"));

	/* The preview-only id now reaches the refine, with both tasks' credits. */
	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpstr(ai_media_job_get_id(job), ==, "text-to-3d:pv-1:rf-1");
	g_assert_cmpstr(ai_media_job_get_provider_status(job), ==, "refine:SUCCEEDED");
	g_assert_cmpint(ai_media_job_get_credits(job), ==, 30);
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(job), ==, AI_MEDIA_COST_UNKNOWN);
	g_assert_nonnull(ai_media_job_get_result_url(job, "glb"));
	g_clear_object(&job);
	g_assert_cmpuint(media_mock_hits("POST /openapi/v2/text-to-3d"), ==, 2);

	/* Polled again once done, the preview-only id still reaches the refine. */
	job = media_poll(AI_MEDIA_GENERATOR(client), job_id, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_clear_object(&job);

	/* A fresh client resumes from the two-task id. */
	{
		g_autoptr(AiMeshyClient) fresh = new_client();

		job = media_poll(AI_MEDIA_GENERATOR(fresh), "text-to-3d:pv-1:rf-1", &error);
		g_assert_no_error(error);
		g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
		g_clear_object(&job);
	}
	g_assert_cmpuint(media_mock_hits("POST /openapi/v2/text-to-3d"), ==, 2);
}

/* A preview this process did not submit is not refined blind. */
static void
test_text_to_3d_lost_plan(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(GError) error = NULL;
	AiMediaJob *job;
	guint before = media_mock_hits("POST /openapi/v2/text-to-3d");

	job = media_poll(AI_MEDIA_GENERATOR(client), "text-to-3d:pv-lost", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "preview_task_id"));
	g_assert_cmpuint(media_mock_hits("POST /openapi/v2/text-to-3d"), ==, before);
}

/* ---------------------------------------------------------------------- */
/* Poll                                                                   */
/* ---------------------------------------------------------------------- */

static void
test_status_mapping(void)
{
	static const struct
	{
		const gchar     *id;
		AiMediaJobState  state;
		const gchar     *word;
	} cases[] = {
		{ "image-to-3d:pending", AI_MEDIA_JOB_STATE_QUEUED, "PENDING" },
		{ "image-to-3d:running", AI_MEDIA_JOB_STATE_RUNNING, "IN_PROGRESS" },
		{ "image-to-3d:failed", AI_MEDIA_JOB_STATE_FAILED, "FAILED" },
		{ "image-to-3d:canceled", AI_MEDIA_JOB_STATE_CANCELLED, "CANCELED" },
		{ "image-to-3d:it-1", AI_MEDIA_JOB_STATE_DONE, "SUCCEEDED" },
	};
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(GError) error = NULL;
	AiMediaJob *job;
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		job = media_poll(AI_MEDIA_GENERATOR(client), cases[i].id, &error);
		g_assert_no_error(error);
		g_assert_cmpint(ai_media_job_get_state(job), ==, cases[i].state);
		g_assert_cmpstr(ai_media_job_get_provider_status(job), ==, cases[i].word);
		g_assert_cmpstr(ai_media_job_get_id(job), ==, cases[i].id);
		if (cases[i].state == AI_MEDIA_JOB_STATE_FAILED)
		{
			g_assert_cmpstr(ai_media_job_get_error_message(job), ==, "Could not reconstruct a mesh");
			g_assert_cmpint(ai_media_job_get_credits(job), ==, 0);
		}
		g_clear_object(&job);
	}

	/* An unknown word is an error, never a guess. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "image-to-3d:weird", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	g_assert_nonnull(strstr(error->message, "TELEPORTING"));
	g_clear_error(&error);

	/* A task Meshy no longer has. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "image-to-3d:gone", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "three days"));
}

static void
test_named_results(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autofree gchar *glb = NULL;

	job = media_poll(AI_MEDIA_GENERATOR(client), "image-to-3d:it-1", &error);
	g_assert_no_error(error);

	glb = g_strdup_printf("%s/cdn/m.glb", media_mock->base_url);
	g_assert_cmpstr(ai_media_job_get_result_url(job, "glb"), ==, glb);
	g_assert_nonnull(ai_media_job_get_result_url(job, "fbx"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "obj"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "usdz"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "thumbnail"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "thumbnail:alpha"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "texture:base_color"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "texture:normal"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "texture:base_color:1"));
	/* An empty string is not a file. */
	g_assert_null(ai_media_job_get_result_url(job, "stl"));
	g_assert_cmpint(ai_media_job_get_credits(job), ==, 10);
	/* Unix milliseconds, as ISO 8601. */
	g_assert_true(g_str_has_prefix(ai_media_job_get_expires_at(job), "2025-11-24T16:00:00"));
	g_assert_cmpuint(ai_media_job_get_n_result_urls(job), ==, 9);
}

static void
test_rig_and_animate(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(GError) error = NULL;
	AiMediaJob *job;

	/* Through the wait loop: PENDING, then SUCCEEDED. */
	job = media_wait(AI_MEDIA_GENERATOR(client), "rig:rigging-1", &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_nonnull(ai_media_job_get_result_url(job, "rig:glb"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "rig:fbx"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "anim:walking"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "anim:walking:fbx"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "anim:running"));
	g_assert_null(ai_media_job_get_result_url(job, "anim:running:fbx"));
	g_assert_cmpint(ai_media_job_get_credits(job), ==, 5);
	g_clear_object(&job);

	job = media_poll(AI_MEDIA_GENERATOR(client), "animate:animations-1", &error);
	g_assert_no_error(error);
	g_assert_nonnull(ai_media_job_get_result_url(job, "anim:glb"));
	g_assert_nonnull(ai_media_job_get_result_url(job, "anim:fbx"));
	g_clear_object(&job);
}

/* ---------------------------------------------------------------------- */
/* Errors                                                                 */
/* ---------------------------------------------------------------------- */

static GError *
submit_named(
	AiMeshyClient *client,
	const gchar   *name
){
	g_autoptr(AiMediaRequest) request = request_for("rig", NULL);
	g_autofree gchar *job_id = NULL;
	GError *error = NULL;

	ai_media_request_set_param_string(request, "name", name);
	job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
	g_assert_null(job_id);
	g_assert_nonnull(error);
	assert_no_key(error->message);
	return error;
}

static void
test_error_envelopes(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(AiMeshyClient) wrong = ai_meshy_client_new_with_api_key("msy_wrong-key-but-long");
	GError *error;

	error = submit_named(client, "broke");
	g_assert_error(error, AI_ERROR, AI_ERROR_INSUFFICIENT_QUOTA);
	g_assert_nonnull(strstr(error->message, "HTTP 402"));
	g_assert_nonnull(strstr(error->message, "credits"));
	g_error_free(error);

	error = submit_named(client, "busy");
	g_assert_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "retry after 7 s"));
	g_assert_null(strstr(error->message, "concurrent"));
	g_error_free(error);

	error = submit_named(client, "capped");
	g_assert_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "concurrent tasks"));
	g_assert_nonnull(strstr(error->message, "NoMorePendingTasks"));
	g_assert_null(strstr(error->message, "retry after"));
	g_error_free(error);

	error = submit_named(client, "down");
	g_assert_error(error, AI_ERROR, AI_ERROR_SERVICE_UNAVAILABLE);
	g_error_free(error);

	/* A 401 that echoes the key the client sent: redacted on the way out. */
	ai_meshy_client_set_base_url(wrong, media_mock->base_url);
	ai_meshy_client_set_api_key(wrong, TEST_KEY "-x");
	error = submit_named(wrong, "anything");
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_assert_nonnull(strstr(error->message, "[REDACTED]"));
	g_error_free(error);
}

static void
test_refused_locally(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(AiMeshyClient) keyless = ai_meshy_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *job_id = NULL;
	g_autofree gchar *huge = NULL;
	AiMediaJob *job;
	guint before = media_mock_hits("POST /openapi/v1/image-to-3d");

	/* An operation Meshy does not have, named in full. */
	{
		g_autoptr(AiMediaRequest) request = request_for("text-to-voxel", NULL);

		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "image-to-3d"));
		g_clear_error(&error);
	}

	/* Per-task webhooks are not a Meshy feature. */
	{
		g_autoptr(AiMediaRequest) request = request_for("image-to-3d", NULL);

		ai_media_request_set_webhook_url(request, "https://example.com/hook");
		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
		g_clear_error(&error);
	}

	/* An inline image over 20 MB is refused before upload. */
	{
		g_autoptr(AiMediaRequest) request = request_for("image-to-3d", NULL);
		gsize length = (AI_MESHY_MAX_IMAGE_BYTES / 3 + 16) * 4;
		gchar *payload = g_malloc(length + 1);

		memset(payload, 'A', length);
		payload[length] = '\0';
		huge = g_strconcat("data:image/png;base64,", payload, NULL);
		g_free(payload);
		ai_media_request_set_image_url(request, huge);
		job_id = media_submit(AI_MEDIA_GENERATOR(client), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "20 MB"));
		g_clear_error(&error);
	}
	g_assert_cmpuint(media_mock_hits("POST /openapi/v1/image-to-3d"), ==, before);

	/* Ids are spliced into paths: anything else is refused. */
	job = media_poll(AI_MEDIA_GENERATOR(client), "image-to-3d:../balance", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	job = media_poll(AI_MEDIA_GENERATOR(client), "rig:a:b", &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* No key, no request. */
	{
		g_autoptr(AiMediaRequest) request = request_for("rig", NULL);

		ai_meshy_client_set_base_url(keyless, media_mock->base_url);
		job_id = media_submit(AI_MEDIA_GENERATOR(keyless), request, &error);
		g_assert_null(job_id);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
		g_clear_error(&error);
	}
}

/* ---------------------------------------------------------------------- */
/* Cancel, download, estimate, balance, library, images                   */
/* ---------------------------------------------------------------------- */

static void
test_cancel(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_media_generator_can_cancel(AI_MEDIA_GENERATOR(client)));
	g_assert_true(media_cancel(AI_MEDIA_GENERATOR(client), "image-to-3d:pending", &error));
	g_assert_no_error(error);
	g_assert_cmpuint(media_mock_hits("DELETE /openapi/v1/image-to-3d/pending"), ==, 1);

	/* Meshy does not delete a task whose credits are spent; the error says so. */
	g_assert_false(media_cancel(AI_MEDIA_GENERATOR(client), "image-to-3d:running", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "HTTP 409"));
	g_assert_nonnull(strstr(error->message, "IN_PROGRESS"));
}

static void
test_download(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *url = g_strdup_printf("%s/cdn/m.glb", media_mock->base_url);
	g_autofree gchar *auth = NULL;
	g_autofree gchar *data_url = NULL;

	bytes = media_download(AI_MEDIA_GENERATOR(client), url, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(bytes), ==, 8);
	/* Storage never sees the key. */
	auth = media_mock_header("GET /cdn/m.glb", "authorization");
	g_assert_null(auth);
	g_clear_pointer(&bytes, g_bytes_unref);

	data_url = g_strdup("data:image/png;base64,aGVsbG8=");
	bytes = media_download(AI_MEDIA_GENERATOR(client), data_url, &error);
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), "hello", 5);
	g_clear_pointer(&bytes, g_bytes_unref);

	bytes = media_download(AI_MEDIA_GENERATOR(client), "file:///etc/passwd", &error);
	g_assert_null(bytes);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static gint64
estimate(
	const gchar *operation,
	const gchar *json_params,
	GError     **error
){
	g_autoptr(AiMediaRequest) request = ai_media_request_new(operation, NULL);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *params;
	GList *names;
	GList *iter;

	g_assert_true(json_parser_load_from_data(parser, json_params, -1, NULL));
	params = json_node_get_object(json_parser_get_root(parser));
	names = json_object_get_members(params);
	for (iter = names; iter != NULL; iter = iter->next)
	{
		/* set_param_json() copies the node. */
		ai_media_request_set_param_json(request, iter->data,
		                                json_object_get_member(params, iter->data));
	}
	g_list_free(names);

	return ai_meshy_estimate_credits(operation, request, error);
}

static void
test_estimate_credits(void)
{
	static const struct
	{
		const gchar *operation;
		const gchar *params;
		gint64       credits;
	} cases[] = {
		/* Text to 3D: preview plus refine, or one step alone. */
		{ "text-to-3d", "{}", 30 },
		{ "text-to-3d", "{\"ai_model\":\"meshy-6\"}", 30 },
		{ "text-to-3d", "{\"ai_model\":\"meshy-6-lite\"}", 15 },
		{ "text-to-3d", "{\"ai_model\":\"meshy-t2.5\"}", 15 },
		{ "text-to-3d", "{\"mode\":\"preview\",\"ai_model\":\"meshy-7.1\"}", 20 },
		{ "text-to-3d", "{\"mode\":\"preview\",\"ai_model\":\"meshy-t2\"}", 5 },
		{ "text-to-3d", "{\"mode\":\"refine\"}", 10 },
		{ "text-to-3d", "{\"mode\":\"refine\",\"texture_resolution\":\"8k\"}", 15 },
		{ "text-to-3d", "{\"texture_resolution\":\"8k\"}", 35 },
		{ "text-to-3d", "{\"mode\":\"preview\",\"geometry_resolution\":\"2k\"}", 25 },
		{ "text-to-3d", "{\"geometry_resolution\":\"4k\"}", 35 },
		/* Image to 3D, per model and texture resolution. */
		{ "image-to-3d", "{}", 30 },
		{ "image-to-3d", "{\"should_texture\":false}", 20 },
		{ "image-to-3d", "{\"texture_resolution\":\"4k\"}", 30 },
		{ "image-to-3d", "{\"texture_resolution\":\"8k\"}", 35 },
		{ "image-to-3d", "{\"hd_texture\":true}", 30 },
		{ "image-to-3d", "{\"ai_model\":\"meshy-6-lite\"}", 15 },
		{ "image-to-3d", "{\"ai_model\":\"meshy-6-lite\",\"should_texture\":false}", 5 },
		{ "image-to-3d", "{\"ai_model\":\"meshy-6-lite\",\"texture_model\":\"meshy-7\","
		                 "\"texture_resolution\":\"8k\"}", 20 },
		{ "image-to-3d", "{\"ai_model\":\"meshy-t2\",\"texture_resolution\":\"4k\"}", 15 },
		{ "image-to-3d", "{\"ai_model\":\"meshy-t2.5\",\"texture_resolution\":\"8k\"}", 20 },
		{ "image-to-3d", "{\"geometry_resolution\":\"2k\"}", 35 },
		{ "multi-image-to-3d", "{\"ai_model\":\"meshy-6\",\"texture_resolution\":\"8k\"}", 35 },
		{ "multi-image-to-3d", "{\"ai_model\":\"meshy-6-lite\"}", 15 },
		/* Fixed prices. */
		{ "retexture", "{}", 10 },
		{ "retexture", "{\"texture_resolution\":\"8k\"}", 15 },
		{ "remesh", "{}", 5 },
		{ "rig", "{}", 5 },
		{ "animate", "{\"action_id\":12}", 3 },
		{ "animate", "{\"action_ids\":[1,2,3,4]}", 12 },
	};
	static const struct
	{
		const gchar *operation;
		const gchar *params;
	} refused[] = {
		{ "image-to-3d", "{\"ai_model\":\"meshy-6-lite\",\"texture_resolution\":\"4k\"}" },
		{ "image-to-3d", "{\"texture_model\":\"meshy-n1\",\"texture_resolution\":\"8k\"}" },
		{ "image-to-3d", "{\"ai_model\":\"meshy-6\",\"geometry_resolution\":\"2k\"}" },
		{ "multi-image-to-3d", "{\"ai_model\":\"meshy-t2\"}" },
		{ "text-to-3d", "{\"ai_model\":\"meshy-99\"}" },
		{ "animate", "{\"action_ids\":[1,2,3,4,5,6,7,8,9,10,11]}" },
		{ "convert", "{}" },
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(GError) error = NULL;
		gint64 credits = estimate(cases[i].operation, cases[i].params, &error);

		if (credits != cases[i].credits)
		{
			g_test_message("%s %s", cases[i].operation, cases[i].params);
		}
		g_assert_no_error(error);
		g_assert_cmpint(credits, ==, cases[i].credits);
	}
	for (i = 0; i < G_N_ELEMENTS(refused); i++)
	{
		g_autoptr(GError) error = NULL;

		g_assert_cmpint(estimate(refused[i].operation, refused[i].params, &error), ==, -1);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	}
}

/* The interface answers in money, which Meshy does not quote. */
static void
test_estimate_interface(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(AiMediaRequest) request = request_for("image-to-3d", NULL);
	g_autoptr(AiMediaRequest) bad = request_for("image-to-3d", NULL);
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GAsyncResult) bad_result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 micro_usd = 0;

	ai_media_generator_estimate_async(AI_MEDIA_GENERATOR(client), request, NULL,
	                                  media_store_result, &result);
	media_run_until(&result);
	g_assert_true(ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(client), result,
	                                                 &micro_usd, &error));
	g_assert_no_error(error);
	g_assert_cmpint(micro_usd, ==, AI_MEDIA_COST_UNKNOWN);

	ai_media_request_set_param_string(bad, "ai_model", "meshy-6-lite");
	ai_media_request_set_param_string(bad, "texture_resolution", "8k");
	ai_media_generator_estimate_async(AI_MEDIA_GENERATOR(client), bad, NULL,
	                                  media_store_result, &bad_result);
	media_run_until(&bad_result);
	g_assert_false(ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(client), bad_result,
	                                                  &micro_usd, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_balance_and_library(void)
{
	g_autoptr(AiMeshyClient) client = new_client();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GAsyncResult) library_result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) library = NULL;
	g_autofree gchar *query = NULL;
	gint64 balance = 0;
	gint64 monthly = 0;
	gint64 permanent = 0;
	JsonObject *first;

	ai_meshy_client_balance_async(client, NULL, media_store_result, &result);
	media_run_until(&result);
	g_assert_true(ai_meshy_client_balance_finish(client, result, &balance, &monthly,
	                                             &permanent, &error));
	g_assert_no_error(error);
	g_assert_cmpint(balance, ==, 1234);
	g_assert_cmpint(monthly, ==, 1000);
	g_assert_cmpint(permanent, ==, 234);

	ai_meshy_client_list_animations_async(client, "WalkAndRun", "slow walk", NULL,
	                                      media_store_result, &library_result);
	media_run_until(&library_result);
	library = ai_meshy_client_list_animations_finish(client, library_result, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(library));
	first = json_array_get_object_element(json_node_get_array(library), 0);
	g_assert_cmpint(json_object_get_int_member(first, "action_id"), ==, 1);

	query = media_mock_query("GET /openapi/v1/animations/library");
	g_assert_cmpstr(query, ==, "category=WalkAndRun&search=slow%20walk");
}

static void
test_image_data_uri(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("meshy-XXXXXX", &error);
	g_autofree gchar *png = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *big = NULL;
	g_autofree gchar *uri = NULL;
	gint fd;

	g_assert_no_error(error);
	png = g_build_filename(dir, "tiny.dat", NULL);
	text = g_build_filename(dir, "note.png", NULL);
	big = g_build_filename(dir, "big.png", NULL);

	/* The format comes from the bytes, whatever the name says. */
	g_assert_true(g_file_set_contents(png, (const gchar *)tiny_png, sizeof tiny_png, NULL));
	uri = ai_meshy_image_data_uri(png, &error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_prefix(uri, "data:image/png;base64,iVBORw0KGgo"));

	g_assert_true(g_file_set_contents(text, "not an image", -1, NULL));
	g_clear_pointer(&uri, g_free);
	uri = ai_meshy_image_data_uri(text, &error);
	g_assert_null(uri);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* A sparse 21 MB file: refused from its size, before it is read. */
	fd = g_open(big, O_CREAT | O_WRONLY, 0600);
	g_assert_cmpint(fd, >=, 0);
	g_assert_cmpint(ftruncate(fd, AI_MESHY_MAX_IMAGE_BYTES + 1024 * 1024), ==, 0);
	close(fd);
	uri = ai_meshy_image_data_uri(big, &error);
	g_assert_null(uri);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "20 MB"));
	g_clear_error(&error);

	media_remove_dir(dir);
}

/* The key appears in no log line the run produced. */
static void
test_logs_clean(void)
{
	g_assert_cmpuint(media_log_length(), >, 0);
	g_assert_true(media_log_contains("meshy:"));
	g_assert_false(media_log_contains(TEST_KEY));
}

int
main(
	int    argc,
	char **argv
){
	int status;

	media_log_capture_begin();
	g_test_init(&argc, &argv, NULL);
	media_log_capture_handler();
	media_mock_start(meshy_route);

	g_test_add_func("/media/job-named-results", test_job_named_results);
	g_test_add_func("/meshy/submit-bodies", test_submit_bodies);
	g_test_add_func("/meshy/text-to-3d-chain", test_text_to_3d_chain);
	g_test_add_func("/meshy/text-to-3d-lost-plan", test_text_to_3d_lost_plan);
	g_test_add_func("/meshy/status-mapping", test_status_mapping);
	g_test_add_func("/meshy/named-results", test_named_results);
	g_test_add_func("/meshy/rig-and-animate", test_rig_and_animate);
	g_test_add_func("/meshy/error-envelopes", test_error_envelopes);
	g_test_add_func("/meshy/refused-locally", test_refused_locally);
	g_test_add_func("/meshy/cancel", test_cancel);
	g_test_add_func("/meshy/download", test_download);
	g_test_add_func("/meshy/estimate-credits", test_estimate_credits);
	g_test_add_func("/meshy/estimate-interface", test_estimate_interface);
	g_test_add_func("/meshy/balance-and-library", test_balance_and_library);
	g_test_add_func("/meshy/image-data-uri", test_image_data_uri);
	g_test_add_func("/meshy/logs-clean", test_logs_clean);

	status = g_test_run();

	media_mock_stop();
	g_string_free(media_log_text, TRUE);
	return status;
}
