/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <glib.h>
#include <glib/gstdio.h>
#include "ai-glib.h"

/* Hermetic subprocess fixture: no credentials and no live generation. */
typedef struct {
	gchar *dir;
	AiGrokBuildClient *client;
} Fixture;

static void
stage(Fixture *fixture, const gchar *name, const gchar *text)
{
	g_autofree gchar *path = g_build_filename(fixture->dir, name, NULL);
	g_autoptr(GError) error = NULL;
	g_assert_true(g_file_set_contents(path, text, -1, &error));
	g_assert_no_error(error);
}

static gchar *
read_stage(Fixture *fixture, const gchar *name)
{
	g_autofree gchar *path = g_build_filename(fixture->dir, name, NULL);
	gchar *text = NULL;
	g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
	return text;
}

static void
setup(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = NULL;
	(void)data;
	fixture->dir = g_dir_make_tmp("ai-grok-media-XXXXXX", NULL);
	g_assert_nonnull(fixture->dir);
	stage(fixture, "grok",
		"#!/bin/bash\nset -eu\nd=\"${0%/*}\"\n"
		"printf '%s\\n' \"$@\" > \"$d/args\"\ncat > \"$d/input\"\n"
		"input=$(<\"$d/input\")\n"
		"pattern='(/[^\"]*ai-grok-media-input-[^\"]+/reference-[0-9]+[.][a-z]+)'\n"
		"if [[ $input =~ $pattern ]]; then\n"
		"  printf '%s' \"${BASH_REMATCH[1]}\" > \"$d/reference-path\"\n"
		"  cat \"${BASH_REMATCH[1]}\" > \"$d/reference-bytes\"\nfi\n"
		"if [[ -f \"$d/stall\" ]]; then exec sleep 30; fi\ncat \"$d/output\"\n");
	path = g_build_filename(fixture->dir, "grok", NULL);
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	fixture->client = ai_grok_build_client_new();
	ai_cli_client_set_executable_path(AI_CLI_CLIENT(fixture->client), path);
	ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(fixture->client), 2000);
	ai_cli_client_set_session_id(AI_CLI_CLIENT(fixture->client), "chat-session-preserved");
	g_object_set(fixture->client, "tools", "read_file", "allowed-tools", "read_file", "continue-session", TRUE, NULL);
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GDir) dir = g_dir_open(fixture->dir, 0, NULL);
	const gchar *name;
	(void)data;
	g_clear_object(&fixture->client);
	while ((name = g_dir_read_name(dir)) != NULL) {
		g_autofree gchar *path = g_build_filename(fixture->dir, name, NULL);
		g_assert_cmpint(g_unlink(path), ==, 0);
	}
	g_clear_pointer(&dir, g_dir_close);
	g_assert_cmpint(g_rmdir(fixture->dir), ==, 0);
	g_free(fixture->dir);
}

static void
stage_result(Fixture *fixture, const gchar *tool, gboolean fail, gboolean malformed)
{
	g_autofree gchar *path = g_build_filename(fixture->dir, "artifact", NULL);
	g_autofree gchar *output = NULL;
	stage(fixture, "artifact", "media-bytes");
	output = g_strdup_printf(
		"{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"media-1\",\"name\":\"%s\",\"input\":{}}]}}\n"
		"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"media-1\",\"is_error\":%s,\"content\":{\"type\":\"ImageGen\",\"absolute_path\":%s\"%s\"%s}}]}}\n"
		"{\"type\":\"result\",\"is_error\":false,\"result\":\"done\"}\n",
		tool, fail ? "true" : "false", malformed ? "[" : "", path, malformed ? "]" : "");
	stage(fixture, "output", output);
}

static void
test_image(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiImageRequest) request = ai_image_request_new("blue fox");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *input = NULL;
	g_autofree gchar *tools = NULL;
	g_autoptr(GBytes) bytes = NULL;
	gboolean continuation;
	gboolean edit = GPOINTER_TO_INT(data);
	stage_result(fixture, edit ? "image_edit" : "image_gen", FALSE, FALSE);
	if (edit) {
		g_autoptr(AiImage) reference = ai_image_new_from_data("ref", 3, "image/png");
		ai_image_set_role(reference, "style");
		ai_image_request_add_reference_image(request, reference);
		ai_image_request_set_operation(request, AI_IMAGE_OPERATION_EDIT);
	}
	ai_image_request_set_aspect_ratio(request, "16:9");
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(fixture->client), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_cmpuint(ai_image_response_get_image_count(response), ==, 1);
	bytes = ai_generated_image_get_bytes(ai_image_response_get_image(response, 0), &error);
	g_assert_no_error(error);
	g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), "media-bytes", 11);
	args = read_stage(fixture, "args");
	input = read_stage(fixture, "input");
	g_assert_nonnull(strstr(args, edit ? "--tools\nimage_edit\n" : "--tools\nimage_gen\n"));
	g_assert_nonnull(strstr(args, "--output-format\nstreaming-messages-json\n"));
	g_assert_null(strstr(args, "--resume"));
	g_assert_null(strstr(args, "--continue"));
	g_assert_nonnull(strstr(input, "blue fox"));
	g_assert_nonnull(strstr(input, "16:9"));
	if (edit) {
		g_autofree gchar *reference_path = read_stage(fixture, "reference-path");
		g_autofree gchar *reference_bytes = read_stage(fixture, "reference-bytes");
		g_assert_cmpstr(reference_bytes, ==, "ref");
		g_assert_true(g_str_has_suffix(reference_path, ".png"));
		g_assert_false(g_file_test(reference_path, G_FILE_TEST_EXISTS));
		g_assert_null(strstr(input, "data:image/"));
		g_assert_nonnull(strstr(input, "Reference image 0 (<IMAGE_0>) role: style"));
	}
	g_object_get(fixture->client, "tools", &tools, "continue-session", &continuation, NULL);
	g_assert_cmpstr(tools, ==, "read_file");
	g_assert_true(continuation);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(fixture->client)), ==, "chat-session-preserved");
}

static void
test_video(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiVideoRequest) request = ai_video_request_new("camera pans");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *input = NULL;
	gint mode = GPOINTER_TO_INT(data);
	const gchar *voices[] = { "eve", NULL };
	stage_result(fixture, mode == 2 ? "reference_to_video" : "image_to_video", FALSE, FALSE);
	if (mode == 1) {
		g_autoptr(AiImage) image = ai_image_new_from_data("ref", 3, "image/png");
		ai_image_set_role(image, "first frame");
		ai_video_request_set_operation(request, "image-to-video");
		ai_video_request_set_image(request, image);
	} else if (mode == 2) {
		g_autoptr(AiImage) image = ai_image_new_from_data("ref", 3, "image/png");
		ai_image_set_role(image, "subject");
		ai_video_request_add_reference_image(request, image);
		ai_video_request_set_operation(request, "reference-to-video");
		ai_video_request_set_voices(request, voices);
		ai_video_request_set_aspect_ratio(request, "16:9");
	}
	ai_video_request_set_duration(request, 6);
	ai_video_request_set_resolution(request, "720p");
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(fixture->client), request, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	g_assert_true(g_str_has_prefix(ai_video_response_get_url(response), "file:///"));
	args = read_stage(fixture, "args");
	input = read_stage(fixture, "input");
	g_assert_nonnull(strstr(args, mode == 2 ? "--tools\nreference_to_video\n" :
		(mode == 1 ? "--tools\nimage_to_video\n" : "--tools\nimage_gen,image_to_video\n")));
	g_assert_nonnull(strstr(input, "resolution_name"));
	g_assert_nonnull(strstr(input, "720p"));
	if (mode == 1) {
		g_autofree gchar *reference_path = read_stage(fixture, "reference-path");
		g_autofree gchar *reference_bytes = read_stage(fixture, "reference-bytes");
		g_assert_cmpstr(reference_bytes, ==, "ref");
		g_assert_false(g_file_test(reference_path, G_FILE_TEST_EXISTS));
	}
	if (mode == 1)
		g_assert_nonnull(strstr(input, "Starting image role: first frame"));
	if (mode == 2) {
		g_assert_nonnull(strstr(input, "eve"));
		g_assert_nonnull(strstr(input, "Reference image 0 (<IMAGE_0>) role: subject"));
	}
}

static void
test_errors(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiImageRequest) request = ai_image_request_new("fox");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	gint mode = GPOINTER_TO_INT(data);
	stage_result(fixture, mode == 3 ? "read_file" : "image_gen", mode == 0, mode == 1);
	if (mode == 2)
		stage(fixture, "output", "{\"type\":\"result\",\"result\":\"Saved image at /tmp/fiction.png\"}\n");
	if (mode == 4)
		ai_image_request_set_count(request, 2);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(fixture->client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
}

static void
test_cancel(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiImageRequest) request = ai_image_request_new("fox");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	g_cancellable_cancel(cancellable);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(fixture->client), request, cancellable, &error);
	g_assert_null(response);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
test_video_invalid(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiVideoRequest) request = ai_video_request_new("fox");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	if (GPOINTER_TO_INT(data) == 0) {
		ai_video_request_set_duration(request, 7);
	} else {
		g_autoptr(AiImage) image = ai_image_new_from_data("ref", 3, "image/png");
		ai_video_request_set_operation(request, "generate");
		ai_video_request_set_image(request, image);
	}
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(fixture->client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}


/* Unsupported image operations must fail before a subprocess is launched. */
static void
test_image_invalid(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiImageRequest) request = ai_image_request_new("fox");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	ai_image_request_set_operation(request, (AiImageOperation)GPOINTER_TO_INT(data));
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(fixture->client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_timeout(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiVideoRequest) request = ai_video_request_new("fox");
	g_autoptr(AiVideoResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	{
		g_autoptr(AiImage) image = ai_image_new_from_data("ref", 3, "image/png");
		ai_video_request_set_image(request, image);
	}
	stage(fixture, "stall", "");
	ai_video_request_set_timeout_ms(request, 150);
	response = ai_video_generator_generate_video(AI_VIDEO_GENERATOR(fixture->client), request, NULL, &error);
	g_assert_null(response);
	g_assert_nonnull(error);
	{
		g_autofree gchar *reference_path = read_stage(fixture, "reference-path");
		g_assert_false(g_file_test(reference_path, G_FILE_TEST_EXISTS));
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/grok/media/image-gen", Fixture, NULL, setup, test_image, teardown);
	g_test_add("/grok/media/image-edit", Fixture, GINT_TO_POINTER(1), setup, test_image, teardown);
	g_test_add("/grok/media/text-video", Fixture, NULL, setup, test_video, teardown);
	g_test_add("/grok/media/image-video", Fixture, GINT_TO_POINTER(1), setup, test_video, teardown);
	g_test_add("/grok/media/reference-video", Fixture, GINT_TO_POINTER(2), setup, test_video, teardown);
	g_test_add("/grok/media/tool-error", Fixture, NULL, setup, test_errors, teardown);
	g_test_add("/grok/media/malformed", Fixture, GINT_TO_POINTER(1), setup, test_errors, teardown);
	g_test_add("/grok/media/prose-rejected", Fixture, GINT_TO_POINTER(2), setup, test_errors, teardown);
	g_test_add("/grok/media/unrelated-tool", Fixture, GINT_TO_POINTER(3), setup, test_errors, teardown);
	g_test_add("/grok/media/short-count", Fixture, GINT_TO_POINTER(4), setup, test_errors, teardown);
	g_test_add("/grok/media/cancel", Fixture, NULL, setup, test_cancel, teardown);
	g_test_add("/grok/media/invalid-video", Fixture, NULL, setup, test_video_invalid, teardown);
	g_test_add("/grok/media/contradictory-video", Fixture, GINT_TO_POINTER(1), setup, test_video_invalid, teardown);
	g_test_add("/grok/media/variation-rejected", Fixture, GINT_TO_POINTER(AI_IMAGE_OPERATION_VARIATION), setup, test_image_invalid, teardown);
	g_test_add("/grok/media/upscale-rejected", Fixture, GINT_TO_POINTER(AI_IMAGE_OPERATION_UPSCALE), setup, test_image_invalid, teardown);
	g_test_add("/grok/media/timeout", Fixture, NULL, setup, test_timeout, teardown);
	return g_test_run();
}
