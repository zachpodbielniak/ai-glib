/*
 * test-image-generator.c - End-to-end image generation against a loopback
 *
 * These drive the real providers over HTTP to a local SoupServer, so they
 * assert on the bytes that actually go on the wire rather than on an
 * intermediate builder.  That is the only way to cover the request shape a
 * provider assembles in a static function, and it is where the
 * responseModalities regression would have been caught.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "ai-glib.h"
#include "core/ai-error.h"

/* ------------------------------------------------------------------ */
/* Loopback server                                                     */
/* ------------------------------------------------------------------ */

/*
 * The server runs on its own thread with its own GMainContext.
 *
 * That is not incidental: ai_image_generator_generate_image() deliberately
 * drives a nested loop on a *private* context so it cannot re-enter the
 * caller's, which means a server attached to the default context would
 * never be dispatched while a synchronous request was in flight, and the
 * request would simply time out.  Giving the server its own thread keeps
 * the sync and async paths testable through the same harness.
 */
typedef struct
{
	SoupServer   *server;
	GMainContext *context;
	GMainLoop    *loop;
	GThread      *thread;
	gchar        *base_url;
	GMutex        lock;

	/* What to reply with. */
	guint       status;
	gchar      *body;
	/* Fail this many times with `fail_status` before replying normally,
	 * so retry behaviour can be exercised deterministically. */
	guint       fail_times;
	guint       fail_status;

	/* What was received. */
	guint       hits;
	gchar      *last_body;
	gchar      *last_path;
	gchar      *last_api_key_header;
	gchar      *last_query;
} TServer;

static void
tserver_handler(
	SoupServer        *server,
	SoupServerMessage *msg,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	TServer *ts = user_data;
	SoupMessageBody *request_body;
	const gchar *api_key;

	(void)server;

	g_mutex_lock(&ts->lock);

	ts->hits++;

	g_clear_pointer(&ts->last_path, g_free);
	ts->last_path = g_strdup(path);

	g_clear_pointer(&ts->last_query, g_free);
	if (query != NULL)
	{
		GString *out = g_string_new(NULL);
		GHashTableIter iter;
		gpointer k;
		gpointer v;

		g_hash_table_iter_init(&iter, query);
		while (g_hash_table_iter_next(&iter, &k, &v))
		{
			g_string_append_printf(out, "%s=%s;", (gchar *)k, (gchar *)v);
		}
		ts->last_query = g_string_free(out, FALSE);
	}

	api_key = soup_message_headers_get_one(
		soup_server_message_get_request_headers(msg), "x-goog-api-key");
	g_clear_pointer(&ts->last_api_key_header, g_free);
	ts->last_api_key_header = g_strdup(api_key);

	request_body = soup_server_message_get_request_body(msg);
	g_clear_pointer(&ts->last_body, g_free);
	ts->last_body = g_strndup(request_body->data, request_body->length);

	if (ts->fail_times > 0)
	{
		ts->fail_times--;
		soup_server_message_set_status(msg, ts->fail_status, NULL);
		soup_server_message_set_response(msg, "application/json",
		                                 SOUP_MEMORY_COPY, "{}", 2);
		g_mutex_unlock(&ts->lock);
		return;
	}

	soup_server_message_set_status(msg, ts->status, NULL);
	soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY,
	                                 ts->body, strlen(ts->body));

	g_mutex_unlock(&ts->lock);
}

/* Runs the server's context until tserver_free() quits it. */
static gpointer
tserver_thread(gpointer user_data)
{
	TServer *ts = user_data;

	g_main_context_push_thread_default(ts->context);
	g_main_loop_run(ts->loop);
	g_main_context_pop_thread_default(ts->context);

	return NULL;
}

/* Signalled once the server is listening and base_url is known. */
static gboolean
tserver_started(gpointer user_data)
{
	TServer *ts = user_data;
	g_autoptr(GError) error = NULL;
	GSList *uris;

	soup_server_listen_local(ts->server, 0, 0, &error);
	g_assert_no_error(error);

	uris = soup_server_get_uris(ts->server);
	g_assert_nonnull(uris);

	{
		g_autofree gchar *full = g_uri_to_string((GUri *)uris->data);
		gsize len = strlen(full);

		/* Trim the trailing slash; the providers concatenate paths. */
		if (len > 0 && full[len - 1] == '/')
		{
			full[len - 1] = '\0';
		}

		g_mutex_lock(&ts->lock);
		ts->base_url = g_strdup(full);
		g_mutex_unlock(&ts->lock);
	}

	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	return G_SOURCE_REMOVE;
}

static TServer *
tserver_new(void)
{
	TServer *ts = g_new0(TServer, 1);
	guint waited = 0;

	g_mutex_init(&ts->lock);
	ts->status = SOUP_STATUS_OK;
	ts->body = g_strdup("{}");
	ts->fail_status = 500;

	ts->context = g_main_context_new();
	ts->loop = g_main_loop_new(ts->context, FALSE);

	/* Build the server against the server context so its sources attach
	 * there rather than to whatever the test thread is using. */
	g_main_context_push_thread_default(ts->context);
	ts->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(ts->server, NULL, tserver_handler, ts, NULL);
	g_main_context_pop_thread_default(ts->context);

	g_main_context_invoke(ts->context, tserver_started, ts);

	ts->thread = g_thread_new("ai-glib-test-image", tserver_thread, ts);

	/* Wait for the listen to complete on the server thread. */
	while (TRUE)
	{
		g_mutex_lock(&ts->lock);
		if (ts->base_url != NULL)
		{
			g_mutex_unlock(&ts->lock);
			break;
		}
		g_mutex_unlock(&ts->lock);

		g_assert_cmpuint(waited++, <, 5000);
		g_usleep(1000);
	}

	return ts;
}

static void
tserver_set_response(TServer *ts, guint status, const gchar *body)
{
	g_mutex_lock(&ts->lock);
	ts->status = status;
	g_free(ts->body);
	ts->body = g_strdup(body);
	g_mutex_unlock(&ts->lock);
}

static void
tserver_set_failures(TServer *ts, guint times, guint status)
{
	g_mutex_lock(&ts->lock);
	ts->fail_times = times;
	ts->fail_status = status;
	g_mutex_unlock(&ts->lock);
}

static guint
tserver_hits(TServer *ts)
{
	guint hits;

	g_mutex_lock(&ts->lock);
	hits = ts->hits;
	g_mutex_unlock(&ts->lock);

	return hits;
}

/* Snapshot of the captured request, owned by the caller. */
static gchar *
tserver_dup_last_body(TServer *ts)
{
	gchar *out;

	g_mutex_lock(&ts->lock);
	out = g_strdup(ts->last_body);
	g_mutex_unlock(&ts->lock);

	return out;
}

static gchar *
tserver_dup_last_path(TServer *ts)
{
	gchar *out;

	g_mutex_lock(&ts->lock);
	out = g_strdup(ts->last_path);
	g_mutex_unlock(&ts->lock);

	return out;
}

static void
tserver_free(TServer *ts)
{
	g_main_loop_quit(ts->loop);
	g_thread_join(ts->thread);

	soup_server_disconnect(ts->server);
	g_object_unref(ts->server);

	g_main_loop_unref(ts->loop);
	g_main_context_unref(ts->context);

	g_free(ts->base_url);
	g_free(ts->body);
	g_free(ts->last_body);
	g_free(ts->last_path);
	g_free(ts->last_api_key_header);
	g_free(ts->last_query);
	g_mutex_clear(&ts->lock);
	g_free(ts);
}

/* Parse the captured request body as JSON. */
static JsonObject *
tserver_last_json(TServer *ts, JsonParser **out_parser)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = tserver_dup_last_body(ts);

	g_assert_nonnull(body);

	*out_parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(*out_parser, body, -1, &error));
	g_assert_no_error(error);

	return json_node_get_object(json_parser_get_root(*out_parser));
}

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static const guchar tiny_png[] = { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a };

/* A minimal Nano Banana response. */
static const gchar *gemini_ok_body =
	"{\"candidates\":[{\"content\":{\"parts\":["
	"{\"inlineData\":{\"mimeType\":\"image/png\",\"data\":\"aGk=\"}}]}}]}";

static const gchar *openai_ok_body =
	"{\"created\":1,\"data\":[{\"b64_json\":\"aGk=\"}]}";

static AiGeminiClient *
make_gemini(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_GEMINI, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_GEMINI, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_gemini_client_new_with_config(config);
}

static AiOpenAIClient *
make_openai(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_OPENAI, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_openai_client_new_with_config(config);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/*
 * The regression.  generationConfig carries responseModalities, and
 * without it Nano Banana answers a request for a picture with text.  It
 * used to be emitted only when the size was neither AUTO nor 1024, so a
 * request that left the size alone -- the common case -- was broken.
 */
static void
test_gemini_always_sends_response_modalities(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiGeminiClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;
	JsonObject *config;
	JsonArray *modalities;

	tserver_set_response(ts, SOUP_STATUS_OK, gemini_ok_body);

	client = make_gemini(ts);
	request = ai_image_request_new("a brass telescope");
	/* Deliberately untouched: no size, no aspect ratio. */

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);

	root = tserver_last_json(ts, &parser);
	g_assert_true(json_object_has_member(root, "generationConfig"));

	config = json_object_get_object_member(root, "generationConfig");
	g_assert_true(json_object_has_member(config, "responseModalities"));

	modalities = json_object_get_array_member(config, "responseModalities");
	g_assert_cmpuint(json_array_get_length(modalities), ==, 2);
	g_assert_cmpstr(json_array_get_string_element(modalities, 1), ==, "IMAGE");

	/* imageConfig.aspectRatio is always present too, defaulting square. */
	g_assert_true(json_object_has_member(config, "imageConfig"));

	tserver_free(ts);
}

static void
test_gemini_sends_reference_images(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiGeminiClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(AiImage) style = NULL;
	g_autoptr(AiImage) subject = NULL;
	JsonObject *root;
	JsonArray *contents;
	JsonArray *parts;
	JsonObject *part;

	tserver_set_response(ts, SOUP_STATUS_OK, gemini_ok_body);

	client = make_gemini(ts);
	request = ai_image_request_new("combine these into a poster");
	ai_image_request_set_model(request,
	                           AI_GEMINI_IMAGE_MODEL_NANO_BANANA_PRO);

	style = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");
	ai_image_set_role(style, "style");
	subject = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/jpeg");
	ai_image_set_role(subject, "subject");

	ai_image_request_add_reference_image(request, style);
	ai_image_request_add_reference_image(request, subject);

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);

	root = tserver_last_json(ts, &parser);
	contents = json_object_get_array_member(root, "contents");
	parts = json_object_get_array_member(
		json_array_get_object_element(contents, 0), "parts");

	/* Text part plus one inline_data part per reference. */
	g_assert_cmpuint(json_array_get_length(parts), ==, 3);

	part = json_array_get_object_element(parts, 1);
	g_assert_true(json_object_has_member(part, "inline_data"));

	/* Roles reach the model through the prompt, since the wire format has
	 * no field for them. */
	part = json_array_get_object_element(parts, 0);
	g_assert_nonnull(strstr(json_object_get_string_member(part, "text"),
	                        "style"));
	g_assert_nonnull(strstr(json_object_get_string_member(part, "text"),
	                        "subject"));

	tserver_free(ts);
}

/*
 * The key belongs in a header.  A URL-embedded credential leaks into proxy
 * logs and crash reports.
 */
static void
test_gemini_api_key_in_header_not_url(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiGeminiClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;

	tserver_set_response(ts, SOUP_STATUS_OK, gemini_ok_body);

	client = make_gemini(ts);
	request = ai_image_request_new("a cat");

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);
	g_assert_no_error(error);

	g_mutex_lock(&ts->lock);
	g_assert_cmpstr(ts->last_api_key_header, ==, "test-key");
	if (ts->last_query != NULL)
	{
		g_assert_null(strstr(ts->last_query, "test-key"));
	}
	g_mutex_unlock(&ts->lock);

	tserver_free(ts);
}

static void
test_gemini_reference_rejected_by_imagen(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiGeminiClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImage) image = NULL;
	g_autoptr(GError) error = NULL;
	AiImageResponse *response;

	client = make_gemini(ts);
	request = ai_image_request_new("a cat");
	ai_image_request_set_model(request, AI_GEMINI_IMAGE_MODEL_IMAGEN_4);

	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");
	ai_image_request_add_reference_image(request, image);

	/* Caught locally by the capability table -- no round trip spent. */
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);

	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_cmpuint(tserver_hits(ts), ==, 0);

	tserver_free(ts);
}

static void
test_openai_gpt_image_body_omits_response_format(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);

	client = make_openai(ts);
	request = ai_image_request_new("a gear icon");
	/* Default model is a GPT Image one. */

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);

	root = tserver_last_json(ts, &parser);
	g_assert_false(json_object_has_member(root, "response_format"));
	g_assert_false(json_object_has_member(root, "style"));

	{
		g_autofree gchar *path = tserver_dup_last_path(ts);
		g_assert_cmpstr(path, ==, "/v1/images/generations");
	}

	tserver_free(ts);
}

static void
test_openai_edit_uses_multipart_endpoint(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiImage) image = NULL;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);

	client = make_openai(ts);
	request = ai_image_request_new("make it blue");
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_EDIT);

	image = ai_image_new_from_data(tiny_png, sizeof tiny_png, "image/png");
	ai_image_request_add_reference_image(request, image);

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);
	g_assert_no_error(error);

	{
		g_autofree gchar *path = tserver_dup_last_path(ts);
		g_assert_cmpstr(path, ==, "/v1/images/edits");
	}
	{
		g_autofree gchar *body = tserver_dup_last_body(ts);
		g_assert_nonnull(strstr(body, "image[]"));
	}

	tserver_free(ts);
}

static void
test_retry_then_success(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGeminiClient) client = NULL;
	g_autoptr(AiConfig) config = NULL;

	tserver_set_response(ts, SOUP_STATUS_OK, gemini_ok_body);
	tserver_set_failures(ts, 2, 503);

	config = ai_config_new();
	ai_config_set_base_url(config, AI_PROVIDER_GEMINI, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_GEMINI, "k");
	ai_config_set_max_retries(config, 3);
	client = ai_gemini_client_new_with_config(config);

	request = ai_image_request_new("a cat");

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);

	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_cmpuint(tserver_hits(ts), ==, 3);

	tserver_free(ts);
}

static void
test_no_retry_on_client_error(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGeminiClient) client = NULL;
	g_autoptr(AiConfig) config = NULL;
	AiImageResponse *response;

	/* A 400 will fail identically next time, so it must not be retried. */
	tserver_set_response(ts, 400, "{\"error\":{\"message\":\"bad prompt\"}}");

	config = ai_config_new();
	ai_config_set_base_url(config, AI_PROVIDER_GEMINI, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_GEMINI, "k");
	ai_config_set_max_retries(config, 3);
	client = ai_gemini_client_new_with_config(config);

	request = ai_image_request_new("a cat");

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);

	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_cmpuint(tserver_hits(ts), ==, 1);

	/* The provider's own explanation survives into the message. */
	g_assert_nonnull(strstr(error->message, "bad prompt"));

	tserver_free(ts);
}

static void
test_rate_limited_maps_to_error(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGeminiClient) client = NULL;
	AiImageResponse *response;

	tserver_set_response(ts, 429, "{}");

	client = make_gemini(ts);
	request = ai_image_request_new("a cat");

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);

	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED);

	tserver_free(ts);
}

static void
test_gemini_text_only_response_errors(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGeminiClient) client = NULL;
	AiImageResponse *response;

	/* The model answered with prose instead of a picture; that must be a
	 * clear error rather than an empty success. */
	tserver_set_response(ts, SOUP_STATUS_OK,
	                     "{\"candidates\":[{\"content\":{\"parts\":"
	                     "[{\"text\":\"I cannot draw that\"}]}}]}");

	client = make_gemini(ts);
	request = ai_image_request_new("a cat");

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);

	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);

	tserver_free(ts);
}

static void
test_gemini_imagen_predictions_envelope(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGeminiClient) client = NULL;

	tserver_set_response(ts, SOUP_STATUS_OK,
	                     "{\"predictions\":[{\"bytesBase64Encoded\":\"aGk=\","
	                     "\"mimeType\":\"image/png\"}]}");

	client = make_gemini(ts);
	request = ai_image_request_new("a cat");
	ai_image_request_set_model(request, AI_GEMINI_IMAGE_MODEL_IMAGEN_4);

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(ai_image_response_get_image_count(response), ==, 1);
	{
		g_autofree gchar *path = tserver_dup_last_path(ts);
		g_assert_cmpstr(path, ==,
		                "/v1beta/models/imagen-4.0-generate-001:predict");
	}

	tserver_free(ts);
}

static void
test_gemini_generated_images_envelope(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiImageRequest) request = NULL;
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiGeminiClient) client = NULL;

	/* The third envelope, which the parser previously did not handle. */
	tserver_set_response(ts, SOUP_STATUS_OK,
	                     "{\"generatedImages\":[{\"bytesBase64Encoded\":\"aGk=\"}]}");

	client = make_gemini(ts);
	request = ai_image_request_new("a cat");

	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client),
	                                             request, NULL, &error);

	g_assert_no_error(error);
	g_assert_cmpuint(ai_image_response_get_image_count(response), ==, 1);

	tserver_free(ts);
}

typedef struct
{
	GMainLoop *loop;
	GBytes    *bytes;
	GError    *error;
} LoadData;

static void
on_bytes_loaded(GObject *source, GAsyncResult *result, gpointer user_data)
{
	LoadData *data = user_data;

	(void)source;

	data->bytes = ai_generated_image_load_bytes_finish(NULL, result,
	                                                   &data->error);
	g_main_loop_quit(data->loop);
}

static void
test_load_bytes_from_base64(void)
{
	g_autoptr(AiGeneratedImage) image = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	LoadData data = { NULL, NULL, NULL };

	/* Inline payloads short-circuit; no network involved. */
	image = ai_generated_image_new_from_base64("aGk=", "image/png");
	loop = g_main_loop_new(NULL, FALSE);
	data.loop = loop;

	ai_generated_image_load_bytes_async(image, NULL, on_bytes_loaded, &data);
	g_main_loop_run(loop);

	g_assert_no_error(data.error);
	g_assert_nonnull(data.bytes);
	g_assert_cmpuint(g_bytes_get_size(data.bytes), ==, 2);

	g_bytes_unref(data.bytes);
}

static void
test_load_bytes_from_url(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiGeneratedImage) image = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autofree gchar *url = NULL;
	LoadData data = { NULL, NULL, NULL };

	/* The default response format is a URL, so this path has to work or
	 * generated images cannot be saved at all. */
	tserver_set_response(ts, SOUP_STATUS_OK, "PNGDATA");

	url = g_strconcat(ts->base_url, "/image.png", NULL);
	image = ai_generated_image_new_from_url(url);

	loop = g_main_loop_new(NULL, FALSE);
	data.loop = loop;

	ai_generated_image_load_bytes_async(image, NULL, on_bytes_loaded, &data);
	g_main_loop_run(loop);

	g_assert_no_error(data.error);
	g_assert_nonnull(data.bytes);
	g_assert_cmpuint(g_bytes_get_size(data.bytes), ==, strlen("PNGDATA"));

	g_bytes_unref(data.bytes);
	tserver_free(ts);
}

static void
test_model_tables_exposed(void)
{
	g_autoptr(AiOpenAIClient) openai = ai_openai_client_new();
	g_autoptr(AiGeminiClient) gemini = ai_gemini_client_new();
	g_autoptr(AiGrokClient) grok = ai_grok_client_new();
	const AiImageModelInfo *info;
	GList *models;

	models = ai_image_generator_list_image_models(AI_IMAGE_GENERATOR(openai));
	g_assert_cmpuint(g_list_length(models), >=, 5);
	g_list_free_full(models, (GDestroyNotify)ai_image_model_info_free);

	models = ai_image_generator_list_image_models(AI_IMAGE_GENERATOR(gemini));
	g_assert_cmpuint(g_list_length(models), >=, 5);
	g_list_free_full(models, (GDestroyNotify)ai_image_model_info_free);

	models = ai_image_generator_list_image_models(AI_IMAGE_GENERATOR(grok));
	g_assert_cmpuint(g_list_length(models), >=, 3);
	g_list_free_full(models, (GDestroyNotify)ai_image_model_info_free);

	/* Nano Banana Pro is the multi-reference workhorse. */
	info = ai_image_generator_get_model_info(
		AI_IMAGE_GENERATOR(gemini), AI_GEMINI_IMAGE_MODEL_NANO_BANANA_PRO);
	g_assert_nonnull(info);
	g_assert_true(ai_image_model_info_supports(info,
	                                           AI_IMAGE_CAP_MULTI_REFERENCE));
	g_assert_cmpuint(ai_image_model_info_get_max_reference_images(info), ==, 14);

	/* GPT Image must not advertise URL responses, which is what keeps
	 * response_format out of its request bodies. */
	info = ai_image_generator_get_model_info(AI_IMAGE_GENERATOR(openai),
	                                         AI_OPENAI_IMAGE_MODEL_GPT_IMAGE_2);
	g_assert_nonnull(info);
	g_assert_false(ai_image_model_info_supports(info,
	                                            AI_IMAGE_CAP_URL_RESPONSE));
	g_assert_false(ai_image_model_info_supports(info, AI_IMAGE_CAP_STYLE));

	/* An unknown model is "unknown", not "unsupported". */
	g_assert_null(ai_image_generator_get_model_info(
		AI_IMAGE_GENERATOR(openai), "gpt-image-released-tomorrow"));
}

static void
test_supported_sizes_falls_back_to_model_table(void)
{
	g_autoptr(AiGeminiClient) gemini = ai_gemini_client_new();
	GList *sizes;

	/* Gemini implements no get_supported_sizes vfunc, so the generic
	 * fallback derives the answer from its default model's ratios. */
	sizes = ai_image_generator_get_supported_sizes(AI_IMAGE_GENERATOR(gemini));

	g_assert_nonnull(sizes);
	g_assert_cmpstr((const gchar *)sizes->data, ==, "1:1");

	g_list_free_full(sizes, g_free);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/image-generator/gemini/response-modalities",
	                test_gemini_always_sends_response_modalities);
	g_test_add_func("/image-generator/gemini/reference-images",
	                test_gemini_sends_reference_images);
	g_test_add_func("/image-generator/gemini/api-key-header",
	                test_gemini_api_key_in_header_not_url);
	g_test_add_func("/image-generator/gemini/imagen-rejects-references",
	                test_gemini_reference_rejected_by_imagen);
	g_test_add_func("/image-generator/gemini/text-only-response",
	                test_gemini_text_only_response_errors);
	g_test_add_func("/image-generator/gemini/predictions-envelope",
	                test_gemini_imagen_predictions_envelope);
	g_test_add_func("/image-generator/gemini/generated-images-envelope",
	                test_gemini_generated_images_envelope);
	g_test_add_func("/image-generator/openai/omits-response-format",
	                test_openai_gpt_image_body_omits_response_format);
	g_test_add_func("/image-generator/openai/edit-multipart",
	                test_openai_edit_uses_multipart_endpoint);
	g_test_add_func("/image-generator/retry/then-success",
	                test_retry_then_success);
	g_test_add_func("/image-generator/retry/not-on-client-error",
	                test_no_retry_on_client_error);
	g_test_add_func("/image-generator/retry/rate-limited",
	                test_rate_limited_maps_to_error);
	g_test_add_func("/image-generator/load-bytes/base64",
	                test_load_bytes_from_base64);
	g_test_add_func("/image-generator/load-bytes/url", test_load_bytes_from_url);
	g_test_add_func("/image-generator/models/tables", test_model_tables_exposed);
	g_test_add_func("/image-generator/models/sizes-fallback",
	                test_supported_sizes_falls_back_to_model_table);

	return g_test_run();
}
