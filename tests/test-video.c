/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-glib.h"
#include <glib/gstdio.h>
#include "test-server.h"

typedef struct { GObject parent; } EmptyVideo;
typedef struct { GObjectClass parent; } EmptyVideoClass;
static void empty_video_iface_init(AiVideoGeneratorInterface *iface);
G_DEFINE_TYPE_WITH_CODE(EmptyVideo, empty_video, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_VIDEO_GENERATOR, empty_video_iface_init))

/* Deliberately incomplete implementor exercises recoverable vfunc errors. */
static void empty_video_class_init(EmptyVideoClass *klass) { (void)klass; }
static void empty_video_init(EmptyVideo *self) { (void)self; }
static void empty_video_iface_init(AiVideoGeneratorInterface *iface) { (void)iface; }

/* Boxed copies must own strings, images, reference lists and voice arrays. */
static void
test_request_ownership(void)
{
	g_autoptr(AiVideoRequest) request = ai_video_request_new("animate");
	g_autoptr(AiVideoRequest) copy = NULL;
	g_autoptr(AiImage) image = ai_image_new_from_data("image", 5, "image/png");
	const gchar *voices[] = { "eve", "rex", NULL };
	const gchar * const *copied_voices;

	g_assert_cmpint(ai_video_request_get_duration(request), ==, -1);
	g_assert_cmpint(ai_video_request_get_generate_audio(request), ==, AI_TRI_UNSET);
	g_assert_cmpuint(ai_video_request_get_timeout_ms(request), ==, 600000);
	ai_video_request_set_model(request, "video-model");
	ai_video_request_set_operation(request, "reference-to-video");
	ai_video_request_set_aspect_ratio(request, "16:9");
	ai_video_request_set_resolution(request, "720p");
	ai_video_request_set_duration(request, 6);
	ai_video_request_set_generate_audio(request, AI_TRI_FALSE);
	ai_video_request_set_poll_interval_ms(request, 25);
	ai_video_request_set_timeout_ms(request, 100);
	ai_video_request_set_voices(request, voices);
	ai_video_request_set_image(request, image);
	ai_video_request_add_reference_image(request, image);
	copy = (AiVideoRequest *)g_boxed_copy(AI_TYPE_VIDEO_REQUEST, request);
	/* Self-assignment must copy before freeing its borrowed input. */
	ai_video_request_set_model(copy, ai_video_request_get_model(copy));
	ai_video_request_set_image(copy, ai_video_request_get_image(copy));
	ai_video_request_set_voices(copy, ai_video_request_get_voices(copy));
	ai_image_set_mime_type(image, "image/jpeg");
	g_clear_pointer(&request, ai_video_request_free);
	g_clear_pointer(&image, ai_image_free);
	g_assert_cmpstr(ai_video_request_get_prompt(copy), ==, "animate");
	g_assert_cmpstr(ai_video_request_get_model(copy), ==, "video-model");
	g_assert_cmpstr(ai_video_request_get_operation(copy), ==, "reference-to-video");
	g_assert_cmpstr(ai_video_request_get_aspect_ratio(copy), ==, "16:9");
	g_assert_cmpstr(ai_video_request_get_resolution(copy), ==, "720p");
	g_assert_cmpint(ai_video_request_get_duration(copy), ==, 6);
	g_assert_cmpint(ai_video_request_get_generate_audio(copy), ==, AI_TRI_FALSE);
	g_assert_cmpuint(ai_video_request_get_poll_interval_ms(copy), ==, 25);
	g_assert_cmpuint(ai_video_request_get_timeout_ms(copy), ==, 100);
	g_assert_cmpstr(ai_image_get_mime_type(ai_video_request_get_image(copy)), ==, "image/png");
	g_assert_cmpuint(ai_video_request_get_reference_image_count(copy), ==, 1);
	g_assert_cmpstr(ai_image_get_mime_type((AiImage *)ai_video_request_get_reference_images(copy)->data), ==, "image/png");
	copied_voices = ai_video_request_get_voices(copy);
	g_assert_cmpstr(copied_voices[0], ==, "eve");
	g_assert_cmpstr(copied_voices[1], ==, "rex");
	g_assert_null(copied_voices[2]);
}

/* Completed result metadata must survive releasing the provider's copy. */
static void
test_response_ownership(void)
{
	g_autoptr(AiVideoResponse) response = ai_video_response_new("https://example.test/video.mp4", "model");
	g_autoptr(AiVideoResponse) copy = NULL;
	ai_video_response_set_request_id(response, "request");
	ai_video_response_set_duration(response, 6.25);
	copy = (AiVideoResponse *)g_boxed_copy(AI_TYPE_VIDEO_RESPONSE, response);
	ai_video_response_set_request_id(copy, ai_video_response_get_request_id(copy));
	g_clear_pointer(&response, ai_video_response_free);
	g_assert_cmpstr(ai_video_response_get_url(copy), ==, "https://example.test/video.mp4");
	g_assert_cmpstr(ai_video_response_get_model(copy), ==, "model");
	g_assert_cmpstr(ai_video_response_get_request_id(copy), ==, "request");
	g_assert_cmpfloat(ai_video_response_get_duration(copy), ==, 6.25);
}

/* Large local artifacts must stream exactly, and cancellation must leave an
 * existing output intact instead of truncating it before the first read. */
static void
test_save_file(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autofree gchar *dir = g_dir_make_tmp("ai-video-XXXXXX", &error);
	g_autofree gchar *source = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *uri = NULL;
	g_autofree gchar *data = g_malloc(200000);
	g_autofree gchar *saved = NULL;
	gsize size;
	g_assert_no_error(error);
	source = g_build_filename(dir, "source.mp4", NULL);
	target = g_build_filename(dir, "target.mp4", NULL);
	memset(data, 'v', 200000);
	g_assert_true(g_file_set_contents(source, data, 200000, &error));
	g_assert_no_error(error);
	uri = g_filename_to_uri(source, NULL, &error);
	g_assert_no_error(error);
	response = ai_video_response_new(uri, "test");
	g_assert_true(ai_video_response_save_to_file(response, target, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_get_contents(target, &saved, &size, &error));
	g_assert_no_error(error);
	g_assert_cmpmem(data, 200000, saved, size);
	g_clear_pointer(&saved, g_free);
	g_cancellable_cancel(cancellable);
	g_assert_false(ai_video_response_save_to_file(response, target, cancellable, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_clear_error(&error);
	g_assert_true(g_file_get_contents(target, &saved, &size, &error));
	g_assert_no_error(error);
	g_assert_cmpmem(data, 200000, saved, size);
	g_assert_cmpint(g_remove(source), ==, 0);
	g_assert_cmpint(g_remove(target), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* HTTP artifacts must be downloaded without API credentials; a later HTTP
 * failure must not replace a previously completed destination. */
static void
test_save_http(void)
{
	TServer *server = tserver_new();
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("ai-video-http-XXXXXX", &error);
	g_autofree gchar *path = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *saved = NULL;
	g_autofree gchar *authorization = NULL;
	g_assert_no_error(error);
	path = g_build_filename(dir, "saved.mp4", NULL);
	url = g_strconcat(server->base_url, "/artifact.mp4", NULL);
	response = ai_video_response_new(url, "model");
	tserver_set_response(server, SOUP_STATUS_OK, "video-bytes");
	g_assert_true(ai_video_response_save_to_file(response, path, NULL, &error));
	g_assert_no_error(error);
	authorization = tserver_dup_header(server, "authorization");
	g_assert_null(authorization);
	g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(saved, ==, "video-bytes");
	g_clear_pointer(&saved, g_free);
	tserver_set_response(server, SOUP_STATUS_FORBIDDEN, "expired");
	g_assert_false(ai_video_response_save_to_file(response, path, NULL, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_NETWORK_ERROR);
	g_clear_error(&error);
	g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(saved, ==, "video-bytes");
	g_assert_cmpuint(tserver_hits(server), ==, 2);
	tserver_free(server);
	g_assert_cmpint(g_remove(path), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* URI validation is a recoverable input error, never a libsoup critical. */
static void
test_save_invalid(void)
{
	g_autoptr(AiVideoResponse) response = ai_video_response_new("ftp://example.test/a", NULL);
	g_autoptr(GError) error = NULL;
	g_assert_false(ai_video_response_save_to_file(response, "/unused", NULL, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
}

/* The synchronous wrapper must complete even for an incomplete implementor. */
static void
test_missing_vfunc(void)
{
	g_autoptr(GObject) object = g_object_new(empty_video_get_type(), NULL);
	g_autoptr(AiVideoRequest) request = ai_video_request_new("prompt");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_assert_null(ai_video_generator_get_default_model(AI_VIDEO_GENERATOR(object)));
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(object), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/video/request-ownership", test_request_ownership);
	g_test_add_func("/video/response-ownership", test_response_ownership);
	g_test_add_func("/video/save-file", test_save_file);
	g_test_add_func("/video/save-http", test_save_http);
	g_test_add_func("/video/save-invalid", test_save_invalid);
	g_test_add_func("/video/missing-vfunc", test_missing_vfunc);
	return g_test_run();
}
