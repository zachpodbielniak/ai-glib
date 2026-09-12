/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Exercise the real image transport and CLI with an isolated agy fixture.
 * No test uses the developer's account, files or network. */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>

static const gchar *png_base64 =
	"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1sAAAAASUVORK5CYII=";

typedef struct
{
	gchar *home;
	gchar *stub;
	AiAntigravityClient *client;
} Fixture;

static void
remove_tree(const gchar *path)
{
	g_autoptr(GDir) dir = NULL;
	const gchar *name;

	/* Never follow the deliberate symlinks in the adversarial fixtures. */
	if (!g_file_test(path, G_FILE_TEST_IS_SYMLINK) && g_file_test(path, G_FILE_TEST_IS_DIR))
	{
		dir = g_dir_open(path, 0, NULL);
		while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
		{
			g_autofree gchar *child = g_build_filename(path, name, NULL);
			remove_tree(child);
		}
		g_assert_cmpint(g_rmdir(path), ==, 0);
	}
	else
		g_assert_cmpint(g_remove(path), ==, 0);
}

static void
setup(Fixture *f, gconstpointer mode)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *script = NULL;

	/* Copy the fixture so its executable bit is independent of checkout. */
	f->home = g_dir_make_tmp("ai-glib-agy-image-XXXXXX", &error);
	g_assert_no_error(error);
	f->stub = g_build_filename(f->home, "agy", NULL);
	g_assert_true(g_file_get_contents("tests/fixtures/agy-image-stub.bash", &script, NULL, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_set_contents(f->stub, script, -1, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(f->stub, 0700), ==, 0);
	f->client = ai_antigravity_client_new();
	ai_cli_client_set_executable_path(AI_CLI_CLIENT(f->client), f->stub);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(f->client), f->home);
	ai_cli_client_set_env(AI_CLI_CLIENT(f->client), "HOME", f->home);
	ai_cli_client_set_env(AI_CLI_CLIENT(f->client), "AI_TEST_IMAGE_MODE", mode != NULL ? mode : "success");
}

static void
teardown(Fixture *f, gconstpointer mode)
{
	(void)mode;
	g_clear_object(&f->client);
	remove_tree(f->home);
	g_free(f->home);
	g_free(f->stub);
}

static void
assert_image(AiImageResponse *response)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree guchar *expected = NULL;
	gsize length = 0;
	AiGeneratedImage *image;

	/* Verify bytes, not merely a success status or plausible pathname. */
	g_assert_nonnull(response);
	g_assert_cmpuint(ai_image_response_get_image_count(response), ==, 1);
	image = ai_image_response_get_image(response, 0);
	g_assert_cmpstr(ai_generated_image_get_mime_type(image), ==, "image/png");
	bytes = ai_generated_image_get_bytes(image, &error);
	g_assert_no_error(error);
	expected = g_base64_decode(png_base64, &length);
	g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), expected, length);
}

static void
on_progress(AiImageGenerator *generator, guint completed, guint total, gpointer data)
{
	guint *count = data;

	(void)generator;
	/* The sync wrapper drives a private context; progress belongs there. */
	g_assert_true(g_main_context_is_owner(g_main_context_get_thread_default()));
	g_assert_cmpuint(completed, ==, 1);
	g_assert_cmpuint(total, ==, 1);
	(*count)++;
}

static void
test_success(Fixture *f, gconstpointer mode)
{
	g_autoptr(AiImageRequest) request = ai_image_request_new("mountains \"at dawn\"\nby a lake");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *argv = NULL;
	g_autofree gchar *input = NULL;
	guint progress = 0;

	(void)mode;
	g_object_set(f->client, "continue-session", TRUE, "skip-permissions", FALSE,
		"sandbox", TRUE, "print-timeout", "42s", NULL);
	ai_cli_client_set_session_id(AI_CLI_CLIENT(f->client), "existing-chat");
	ai_cli_client_set_model(AI_CLI_CLIENT(f->client), "chat-model");
	ai_image_request_set_model(request, "image-agent-model");
	ai_image_request_set_aspect_ratio(request, "16:9");
	g_signal_connect(f->client, "image-progress", G_CALLBACK(on_progress), &progress);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(f->client), request, NULL, &error);
	g_assert_no_error(error);
	assert_image(response);
	g_assert_cmpuint(progress, ==, 1);
	g_assert_cmpstr(ai_image_response_get_model(response), ==, "image-agent-model");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(f->client)), ==, "existing-chat");
	g_assert_cmpstr(ai_cli_client_get_model(AI_CLI_CLIENT(f->client)), ==, "chat-model");
	path = g_build_filename(f->home, "argv", NULL);
	g_assert_true(g_file_get_contents(path, &argv, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(argv, "--input-format\nstream-json\n"));
	g_assert_nonnull(strstr(argv, "--output-format\nstream-json\n"));
	g_assert_nonnull(strstr(argv, "--model\nimage-agent-model\n"));
	g_assert_nonnull(strstr(argv, "--print-timeout\n42s\n"));
	g_assert_nonnull(strstr(argv, "--sandbox\n"));
	g_assert_null(strstr(argv, "--conversation"));
	g_assert_null(strstr(argv, "--continue"));
	g_assert_null(strstr(argv, "--dangerously-skip-permissions"));
	g_assert_null(strstr(argv, "mountains"));
	g_free(path);
	path = g_build_filename(f->home, "input", NULL);
	g_assert_true(g_file_get_contents(path, &input, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(input, "generate_image"));
	g_assert_nonnull(strstr(input, "16:9"));
	g_assert_nonnull(strstr(input, "mountains \\\"at dawn\\\"\\nby a lake"));
}

static void
test_failure(Fixture *f, gconstpointer mode)
{
	g_autoptr(AiImageRequest) request = ai_image_request_new("mountains");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;

	/* Every malformed/tool/file failure must be a GError, never a critical
	 * or an empty successful image response. */
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(f->client), request, NULL, &error);
	g_assert_null(response);
	g_assert_nonnull(error);
	if (g_str_equal(mode, "error") || g_str_equal(mode, "terminal-error") ||
		g_str_equal(mode, "exit") || g_str_equal(mode, "malformed"))
		g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	else
		g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
	if (g_str_equal(mode, "error"))
	{
		g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
		g_assert_nonnull(strstr(error->message, "backend refused image"));
	}
}

static void
test_validation(Fixture *f, gconstpointer mode)
{
	g_autoptr(AiImageRequest) request = ai_image_request_new("mountains");
	g_autoptr(AiImageResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = g_build_filename(f->home, "input", NULL);
	const AiImageModelInfo *info;

	(void)mode;
	info = ai_image_generator_get_model_info(AI_IMAGE_GENERATOR(f->client), NULL);
	g_assert_nonnull(info);
	g_assert_cmpuint(ai_image_model_info_get_max_count(info), ==, 1);
	ai_image_request_set_count(request, 2);
	g_assert_false(ai_image_request_validate(request, info, AI_IMAGE_VALIDATE_STRICT, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	/* Lenient generation clamps a private copy, preserving the request. */
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(f->client), request, NULL, &error);
	g_assert_no_error(error);
	assert_image(response);
	g_assert_cmpint(ai_image_request_get_count(request), ==, 2);
	g_clear_pointer(&response, ai_image_response_free);
	g_assert_cmpint(g_remove(path), ==, 0);
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_EDIT);
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(f->client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_clear_error(&error);
	ai_image_request_set_operation(request, AI_IMAGE_OPERATION_GENERATE);
	{
		g_autoptr(AiImage) reference = ai_image_new_from_base64(png_base64, "image/png");
		ai_image_request_add_reference_image(request, reference);
	}
	response = ai_image_generator_generate_image(AI_IMAGE_GENERATOR(f->client), request, NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
}

typedef struct
{
	GMainLoop *loop;
	AiImageResponse *response;
	GError *error;
	guint *pending;
} AsyncResult;

static void
on_done(GObject *source, GAsyncResult *result, gpointer data)
{
	AsyncResult *done = data;

	done->response = ai_image_generator_generate_image_finish(AI_IMAGE_GENERATOR(source), result, &done->error);
	if (done->pending == NULL || --(*done->pending) == 0)
		g_main_loop_quit(done->loop);
}

static gboolean
cancel_run(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
	return G_SOURCE_REMOVE;
}

static void
test_async(Fixture *f, gconstpointer mode)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GMainLoop) loop = g_main_loop_new(context, FALSE);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(AiImageRequest) request = ai_image_request_new("mountains");
	g_autoptr(GSource) timer = NULL;
	AsyncResult done = { 0 };

	/* Release the request immediately and iterate only a private context. */
	done.loop = loop;
	g_main_context_push_thread_default(context);
	if (g_str_equal(mode, "cancel"))
	{
		ai_cli_client_set_env(AI_CLI_CLIENT(f->client), "AI_TEST_IMAGE_MODE", "sleep");
		timer = g_timeout_source_new(100);
		g_source_set_callback(timer, cancel_run, cancellable, NULL);
		g_source_attach(timer, context);
	}
	else if (g_str_equal(mode, "timeout"))
	{
		ai_cli_client_set_env(AI_CLI_CLIENT(f->client), "AI_TEST_IMAGE_MODE", "sleep");
		ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(f->client), 100);
	}
	else if (g_str_equal(mode, "precancel"))
		g_cancellable_cancel(cancellable);
	ai_image_generator_generate_image_async(AI_IMAGE_GENERATOR(f->client), request, cancellable, on_done, &done);
	g_clear_pointer(&request, ai_image_request_free);
	g_main_loop_run(loop);
	g_main_context_pop_thread_default(context);
	if (g_str_equal(mode, "success"))
	{
		g_assert_no_error(done.error);
		assert_image(done.response);
	}
	else
	{
		g_assert_null(done.response);
		g_assert_nonnull(done.error);
		if (g_str_equal(mode, "cancel") || g_str_equal(mode, "precancel"))
			g_assert_error(done.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
		else
			g_assert_error(done.error, AI_ERROR, AI_ERROR_TIMEOUT);
		if (g_str_equal(mode, "precancel"))
		{
			g_autofree gchar *input = g_build_filename(f->home, "input", NULL);
			g_assert_false(g_file_test(input, G_FILE_TEST_EXISTS));
		}
	}
	g_clear_pointer(&done.response, ai_image_response_free);
	g_clear_error(&done.error);
}

static void
test_concurrent(Fixture *f, gconstpointer mode)
{
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GMainLoop) loop = g_main_loop_new(context, FALSE);
	g_autoptr(AiImageRequest) request = ai_image_request_new("mountains");
	AsyncResult results[2] = { { 0 }, { 0 } };
	guint pending = 2;
	guint i;

	(void)mode;
	/* Two overlapping calls must own separate prompts and artifact names. */
	g_main_context_push_thread_default(context);
	for (i = 0; i < 2; i++)
	{
		results[i].loop = loop;
		results[i].pending = &pending;
		ai_image_generator_generate_image_async(AI_IMAGE_GENERATOR(f->client),
			request, NULL, on_done, &results[i]);
	}
	g_main_loop_run(loop);
	g_main_context_pop_thread_default(context);
	for (i = 0; i < 2; i++)
	{
		g_assert_no_error(results[i].error);
		assert_image(results[i].response);
	}
	g_assert_cmpstr(ai_image_response_get_id(results[0].response), !=,
		ai_image_response_get_id(results[1].response));
	for (i = 0; i < 2; i++)
		ai_image_response_free(results[i].response);
}

static void
test_cli(Fixture *f, gconstpointer mode)
{
	g_autofree gchar *self_path = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *test_dir = g_path_get_dirname(self_path);
	g_autofree gchar *binary = g_build_filename(test_dir, "..", "bin", "ai", NULL);
	g_autofree gchar *output = g_build_filename(f->home, "output.png", NULL);
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stdout_data = NULL;
	g_autofree gchar *stderr_data = NULL;
	g_autofree gchar *bytes = NULL;
	g_autofree guchar *expected = NULL;
	gsize length;
	gsize expected_length;

	/* Launch the actual CLI, using its public AGY_PATH override. */
	g_subprocess_launcher_setenv(launcher, "HOME", f->home, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", f->home, TRUE);
	g_subprocess_launcher_setenv(launcher, "AGY_PATH", f->stub, TRUE);
	if (g_str_equal(mode, "list"))
		process = g_subprocess_launcher_spawn(launcher, &error, binary,
			"--list-image-models", "-p", "agy", NULL);
	else if (g_str_equal(mode, "strict"))
		process = g_subprocess_launcher_spawn(launcher, &error, binary,
			"--image-gen", "-p", "agy", "--strict", "--seed", "42",
			"-m", AI_ANTIGRAVITY_MODEL_GEMINI_3_7_FLASH_LOW, "mountains", NULL);
	else
		process = g_subprocess_launcher_spawn(launcher, &error, binary,
			"--image-gen", "-p", "antigravity", "-o", output,
			"--aspect", "16:9", "mountains", NULL);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_data, &stderr_data, &error));
	g_assert_no_error(error);
	g_test_message("CLI stderr: %s", stderr_data);
	if (g_str_equal(mode, "strict"))
	{
		g_autofree gchar *input = g_build_filename(f->home, "input", NULL);
		g_assert_false(g_subprocess_get_successful(process));
		g_assert_nonnull(strstr(stderr_data, "seed"));
		g_assert_false(g_file_test(input, G_FILE_TEST_EXISTS));
		return;
	}
	g_assert_true(g_subprocess_get_successful(process));
	if (g_str_equal(mode, "list"))
	{
		g_assert_nonnull(strstr(stdout_data, "generate_image"));
		g_assert_nonnull(strstr(stdout_data, AI_ANTIGRAVITY_DEFAULT_MODEL));
	}
	else
	{
		g_assert_nonnull(strstr(stdout_data, output));
		g_assert_true(g_file_get_contents(output, &bytes, &length, &error));
		g_assert_no_error(error);
		expected = g_base64_decode(png_base64, &expected_length);
		g_assert_cmpmem(bytes, length, expected, expected_length);
	}
}

int
main(int argc, char **argv)
{
	const gchar *failures[] = { "error", "terminal-error", "exit", "malformed", "missing",
		"nonimage", "symlink", "hardlink", "fifo", "oversized", "stale", "dirlink",
		"multiple", "wrong-name", "mismatch", "traversal", "noresult" };
	const gchar *async_modes[] = { "success", "cancel", "precancel", "timeout" };
	guint i;

	g_test_init(&argc, &argv, NULL);
	g_test_add("/ai-glib/antigravity-image/success", Fixture, "success", setup, test_success, teardown);
	g_test_add("/ai-glib/antigravity-image/validation", Fixture, "success", setup, test_validation, teardown);
	g_test_add("/ai-glib/antigravity-image/concurrent", Fixture, "success", setup, test_concurrent, teardown);
	for (i = 0; i < G_N_ELEMENTS(failures); i++)
	{
		g_autofree gchar *path = g_strconcat("/ai-glib/antigravity-image/failure/", failures[i], NULL);
		g_test_add(path, Fixture, failures[i], setup, test_failure, teardown);
	}
	for (i = 0; i < G_N_ELEMENTS(async_modes); i++)
	{
		g_autofree gchar *path = g_strconcat("/ai-glib/antigravity-image/async/", async_modes[i], NULL);
		g_test_add(path, Fixture, async_modes[i], setup, test_async, teardown);
	}
	g_test_add("/ai-glib/antigravity-image/cli/generate", Fixture, "success", setup, test_cli, teardown);
	g_test_add("/ai-glib/antigravity-image/cli/list", Fixture, "list", setup, test_cli, teardown);
	g_test_add("/ai-glib/antigravity-image/cli/strict", Fixture, "strict", setup, test_cli, teardown);
	return g_test_run();
}
