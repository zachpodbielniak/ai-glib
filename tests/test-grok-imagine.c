/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Real loopback HTTP verifies Imagine paths, JSON and private-context polling.
 */
#include "ai-glib.h"
#include "test-server.h"
#include "core/ai-json-util.h"

typedef struct {
	TServer *server;
	const gchar *result;
	gchar *post_body;
	guint polls;
	guint posts;
	guint pending;
	guint post_status;
	guint delay_ms;
	GMutex lock;
	GCond ready;
	gboolean installed;
} VideoServer;

/* Runs only on the server thread; tests read captures after completion. */
static void
video_handler(SoupServer *server, SoupServerMessage *msg, const gchar *path,
	GHashTable *query, gpointer data)
{
	VideoServer *vs = data;
	const gchar *body;
	(void)server;
	(void)query;
	if (strcmp(path, "/v1/videos/generations") == 0) {
		SoupMessageBody *request = soup_server_message_get_request_body(msg);
		vs->posts++;
		g_free(vs->post_body);
		vs->post_body = g_strndup(request->data, request->length);
		body = "{\"request_id\":\"job/1\"}";
		soup_server_message_set_status(msg, vs->post_status, NULL);
	} else {
		vs->polls++;
		body = vs->polls <= vs->pending ? "{\"status\":\"pending\"}" : vs->result;
		soup_server_message_set_status(msg, SOUP_STATUS_OK, NULL);
	}
	if (vs->delay_ms > 0) g_usleep((gulong)vs->delay_ms * 1000);
	soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY, body, strlen(body));
}

static gboolean
install_handler(gpointer data)
{
	VideoServer *vs = data;
	soup_server_add_handler(vs->server->server, "/v1/videos", video_handler, vs, NULL);
	g_mutex_lock(&vs->lock);
	vs->installed = TRUE;
	g_cond_signal(&vs->ready);
	g_mutex_unlock(&vs->lock);
	return G_SOURCE_REMOVE;
}

static void
video_server_init(VideoServer *vs, const gchar *result)
{
	memset(vs, 0, sizeof(*vs));
	g_mutex_init(&vs->lock);
	g_cond_init(&vs->ready);
	vs->server = tserver_new();
	vs->result = result;
	vs->post_status = SOUP_STATUS_OK;
	g_mutex_lock(&vs->lock);
	g_main_context_invoke(vs->server->context, install_handler, vs);
	while (!vs->installed) g_cond_wait(&vs->ready, &vs->lock);
	g_mutex_unlock(&vs->lock);
}

static void
video_server_clear(VideoServer *vs)
{
	tserver_free(vs->server);
	g_free(vs->post_body);
	g_mutex_clear(&vs->lock);
	g_cond_clear(&vs->ready);
}

static AiGrokClient *
client_new(TServer *server)
{
	g_autoptr(AiConfig) config = ai_config_new();
	ai_config_set_base_url(config, AI_PROVIDER_GROK, server->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_GROK, "test-key");
	ai_config_set_max_retries(config, 0);
	return ai_grok_client_new_with_config(config);
}

static const gchar *done_body = "{\"status\":\"done\",\"model\":\"grok-imagine-video-1.5\",\"video\":{\"url\":\"https://example.org/clip.mp4\",\"duration\":8,\"respect_moderation\":true}}";

/* The sync wrapper's private context must dispatch every pending poll. */
static void
test_video_success(gconstpointer data)
{
	const gchar *operation = data;
	VideoServer vs;
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiVideoRequest) request = ai_video_request_new("A lake at dawn");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(AiImage) image = ai_image_new_from_data("png", 3, "image/png");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	const gchar *voices[] = { "eve", NULL };
	video_server_init(&vs, done_body);
	vs.pending = 2;
	client = client_new(vs.server);
	ai_video_request_set_operation(request, operation);
	ai_video_request_set_poll_interval_ms(request, 1);
	ai_video_request_set_timeout_ms(request, 2000);
	ai_video_request_set_duration(request, 8);
	ai_video_request_set_resolution(request, "720p");
	ai_video_request_set_aspect_ratio(request, "16:9");
	ai_video_request_set_generate_audio(request, AI_TRI_FALSE);
	ai_image_set_role(image, "landscape composition");
	if (strcmp(operation, "image-to-video") == 0)
		ai_video_request_set_image(request, image);
	else if (strcmp(operation, "reference-to-video") == 0) {
		ai_video_request_add_reference_image(request, image);
		ai_video_request_set_voices(request, voices);
	}
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(client), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_cmpstr(ai_video_response_get_request_id(response), ==, "job/1");
	g_assert_cmpfloat(ai_video_response_get_duration(response), ==, 8);
	g_assert_cmpuint(vs.polls, ==, 3);
	g_assert_true(json_parser_load_from_data(parser, vs.post_body, -1, &error));
	g_assert_no_error(error);
	root = ai_json_root_object(parser);
	g_assert_cmpstr(ai_json_get_string(root, "model", NULL), ==, AI_GROK_VIDEO_MODEL_GROK_IMAGINE_1_5);
	g_assert_cmpstr(ai_json_get_string(root, "resolution", NULL), ==, "720p");
	g_assert_cmpint(ai_json_get_int(root, "duration", 0), ==, 8);
	g_assert_false(ai_json_get_boolean(root, "generate_audio", TRUE));
	g_assert_cmpstr(ai_video_request_get_prompt(request), ==, "A lake at dawn");
	if (strcmp(operation, "image-to-video") == 0)
		g_assert_cmpstr(ai_json_get_string(root, "prompt", NULL), ==,
			"A lake at dawn\nStarting image role: landscape composition");
	else if (strcmp(operation, "reference-to-video") == 0)
		g_assert_cmpstr(ai_json_get_string(root, "prompt", NULL), ==,
			"A lake at dawn\nReference image 1 role: landscape composition");
	else
		g_assert_cmpstr(ai_json_get_string(root, "prompt", NULL), ==, "A lake at dawn");
	if (strcmp(operation, "image-to-video") == 0)
		g_assert_cmpstr(ai_json_get_string(ai_json_get_object(root, "image"), "url", NULL), ==, "data:image/png;base64,cG5n");
	if (strcmp(operation, "reference-to-video") == 0) {
		g_assert_nonnull(ai_json_get_array(root, "reference_images"));
		g_assert_cmpstr(ai_json_get_string(ai_json_array_get_object(ai_json_get_array(root, "reference_audios"), 0), "voice_id", NULL), ==, "eve");
	}
	video_server_clear(&vs);
}

static void
test_video_bad_result(gconstpointer data)
{
	VideoServer vs;
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiVideoRequest) request = ai_video_request_new("Lake");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	video_server_init(&vs, data);
	client = client_new(vs.server);
	ai_video_request_set_poll_interval_ms(request, 1);
	ai_video_request_set_timeout_ms(request, 2000);
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_nonnull(error);
	video_server_clear(&vs);
}

static void
test_video_timeout(void)
{
	VideoServer vs;
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiVideoRequest) request = ai_video_request_new("Lake");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	video_server_init(&vs, "{\"status\":\"pending\"}");
	client = client_new(vs.server);
	ai_video_request_set_poll_interval_ms(request, 5000);
	ai_video_request_set_timeout_ms(request, 25);
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	video_server_clear(&vs);
}

/* Cancellation while idle must interrupt a long poll interval immediately. */
static gpointer
cancel_thread(gpointer data)
{
	g_usleep(50000);
	g_cancellable_cancel(G_CANCELLABLE(data));
	return NULL;
}

static void
test_video_cancel(void)
{
	VideoServer vs;
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiVideoRequest) request = ai_video_request_new("Lake");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	GThread *thread;
	gint64 start;
	video_server_init(&vs, done_body);
	client = client_new(vs.server);
	ai_video_request_set_poll_interval_ms(request, 5000);
	ai_video_request_set_timeout_ms(request, 2000);
	thread = g_thread_new("cancel", cancel_thread, cancel);
	start = g_get_monotonic_time();
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(client), request, cancel, &error);
	g_thread_join(thread);
	g_assert_null(response);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_cmpint(g_get_monotonic_time() - start, <, G_TIME_SPAN_SECOND);
	video_server_clear(&vs);
}

static void
test_video_validation(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiGrokClient) client = client_new(server);
	g_autoptr(AiVideoRequest) request = ai_video_request_new("Lake");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	ai_video_request_set_duration(request, 16);
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_cmpuint(server->hits, ==, 0);
	tserver_free(server);
}

/* Generation references select JSON edits; multi-image shape differs from OpenAI. */
static void
test_image_edit(gconstpointer data)
{
	guint count = GPOINTER_TO_UINT(data);
	TServer *server = tserver_new();
	g_autoptr(AiGrokClient) client = client_new(server);
	g_autoptr(AiImageRequest) request = ai_image_request_new("Restyle");
	g_autoptr(AiImage) image = ai_image_new_from_data("png", 3, "image/png");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	guint i;
	tserver_set_response(server, SOUP_STATUS_OK, "{\"data\":[{\"b64_json\":\"cG5n\"}]}");
	for (i = 0; i < count; i++) ai_image_request_add_reference_image(request, image);
	ai_image_request_set_aspect_ratio(request, "16:9");
	ai_image_request_set_resolution(request, AI_IMAGE_RESOLUTION_2K);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_cmpstr(server->last_path, ==, count > 0 ? "/v1/images/edits" : "/v1/images/generations");
	g_assert_true(json_parser_load_from_data(parser, server->last_body, -1, &error));
	g_assert_no_error(error);
	root = ai_json_root_object(parser);
	g_assert_cmpstr(ai_json_get_string(root, "resolution", NULL), ==, "2k");
	g_assert_cmpstr(ai_json_get_string(root, "aspect_ratio", NULL), ==, "16:9");
	g_assert_false(json_object_has_member(root, "size"));
	if (count == 1) g_assert_nonnull(ai_json_get_object(root, "image"));
	if (count > 1) g_assert_cmpuint(json_array_get_length(ai_json_get_array(root, "images")), ==, count);
	tserver_free(server);
}

/* A server error creating a job must not trigger a duplicate POST. */
static void
test_video_post_failure(void)
{
	VideoServer vs;
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiVideoRequest) request = ai_video_request_new("Lake");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	video_server_init(&vs, done_body);
	vs.post_status = SOUP_STATUS_INTERNAL_SERVER_ERROR;
	client = client_new(vs.server);
	ai_config_set_max_retries(ai_client_get_config(AI_CLIENT(client)), 3);
	ai_video_request_set_timeout_ms(request, 1000);
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR);
	g_assert_cmpuint(vs.polls, ==, 0);
	g_assert_cmpuint(vs.posts, ==, 1);
	video_server_clear(&vs);
}

static void
test_video_active_timeout(void)
{
	VideoServer vs;
	g_autoptr(AiGrokClient) client = NULL;
	g_autoptr(AiVideoRequest) request = ai_video_request_new("Lake");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	video_server_init(&vs, done_body);
	vs.delay_ms = 150;
	client = client_new(vs.server);
	ai_video_request_set_timeout_ms(request, 25);
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	video_server_clear(&vs);
}

/* Normalization is provider-local, leaving the reusable request untouched. */
static void
test_image_projection(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiGrokClient) client = client_new(server);
	g_autoptr(AiImageRequest) request = ai_image_request_new("Lake");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	tserver_set_response(server, SOUP_STATUS_OK, "{\"data\":[{\"b64_json\":\"cG5n\"}]}");
	ai_image_request_set_size(request, AI_IMAGE_SIZE_1024);
	ai_image_request_set_quality(request, AI_IMAGE_QUALITY_MEDIUM);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(client), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_cmpint(ai_image_request_get_size(request), ==, AI_IMAGE_SIZE_1024);
	g_assert_true(json_parser_load_from_data(parser, server->last_body, -1, &error));
	g_assert_no_error(error);
	root = ai_json_root_object(parser);
	g_assert_false(json_object_has_member(root, "size"));
	g_assert_cmpstr(ai_json_get_string(root, "quality", NULL), ==, "medium");
	tserver_free(server);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/grok-imagine/video/generate", "generate", test_video_success);
	g_test_add_data_func("/grok-imagine/video/image", "image-to-video", test_video_success);
	g_test_add_data_func("/grok-imagine/video/reference", "reference-to-video", test_video_success);
	g_test_add_data_func("/grok-imagine/video/null", "null", test_video_bad_result);
	g_test_add_data_func("/grok-imagine/video/malformed", "{\"status\":12}", test_video_bad_result);
	g_test_add_data_func("/grok-imagine/video/missing-url", "{\"status\":\"done\",\"video\":false}", test_video_bad_result);
	g_test_add_data_func("/grok-imagine/video/failed", "{\"status\":\"failed\",\"error\":{\"message\":\"failed\"}}", test_video_bad_result);
	g_test_add_data_func("/grok-imagine/video/expired", "{\"status\":\"expired\"}", test_video_bad_result);
	g_test_add_func("/grok-imagine/video/timeout", test_video_timeout);
	g_test_add_func("/grok-imagine/video/active-timeout", test_video_active_timeout);
	g_test_add_func("/grok-imagine/video/post-failure", test_video_post_failure);
	g_test_add_func("/grok-imagine/image/projection", test_image_projection);
	g_test_add_func("/grok-imagine/video/cancel", test_video_cancel);
	g_test_add_func("/grok-imagine/video/validation", test_video_validation);
	g_test_add_data_func("/grok-imagine/image/generate", GUINT_TO_POINTER(0), test_image_edit);
	g_test_add_data_func("/grok-imagine/image/edit", GUINT_TO_POINTER(1), test_image_edit);
	g_test_add_data_func("/grok-imagine/image/multi-edit", GUINT_TO_POINTER(2), test_image_edit);
	return g_test_run();
}
