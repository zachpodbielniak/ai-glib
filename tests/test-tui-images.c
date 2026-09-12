/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Exercise the real composer/key dispatcher with an in-memory curses screen,
 * and the binary clipboard reader against this executable as a fake owner. */
#define main ai_tui_program_main
#include "../bin/ai-tui.c"
#undef main
#include <glib/gstdio.h>
#include <sys/stat.h>

static gchar *test_binary;
static const gchar *png_base64 = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aX1sAAAAASUVORK5CYII=";

/* The embedded fixture contains NUL bytes, pinning binary-safe transport. */
static GBytes *
png_bytes(void)
{
	gsize size;
	guchar *data = g_base64_decode(png_base64, &size);

	return g_bytes_new_take(data, size);
}

static AiImageContent *
png_image(void)
{
	g_autoptr(GBytes) bytes = png_bytes();
	g_autoptr(GError) error = NULL;
	AiImageContent *image = tui_image_from_bytes(bytes, &error);

	g_assert_no_error(error);
	return image;
}

typedef struct
{
	gboolean done;
	AiImageContent *image;
	GError *error;
} ClipboardResult;

static void
clipboard_done(GObject *source, GAsyncResult *result, gpointer data)
{
	ClipboardResult *out = data;

	(void)source;
	out->image = g_task_propagate_pointer(G_TASK(result), &out->error);
	out->done = TRUE;
}

/* Success, invalid/empty input, nonzero exit, cancellation and both kinds
 * of timeout are tested without a real clipboard or network connection. */
static void
test_clipboard(gconstpointer data)
{
	const gchar *mode = data;
	const gchar *argv[] = { test_binary, "--clipboard-fixture", mode, NULL };
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	ClipboardResult out = { FALSE, NULL, NULL };
	gint64 start = g_get_monotonic_time();

	tui_clipboard_read_async(argv, cancel, 500, clipboard_done, &out);
	if (g_str_equal(mode, "cancel")) g_cancellable_cancel(cancel);
	while (!out.done) g_main_context_iteration(NULL, TRUE);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 3000000);
	if (g_str_equal(mode, "png"))
	{
		g_autoptr(GBytes) expected = png_bytes();
		g_assert_no_error(out.error);
		g_assert_nonnull(out.image);
		g_assert_true(g_bytes_equal(expected, ai_image_get_bytes(ai_image_content_get_image(out.image))));
		g_assert_cmpstr(ai_image_get_mime_type(ai_image_content_get_image(out.image)), ==, "image/png");
	}
	else
	{
		g_assert_null(out.image);
		g_assert_nonnull(out.error);
		if (g_str_equal(mode, "oversize"))
			g_assert_error(out.error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
		if (g_str_equal(mode, "cancel") || g_str_has_prefix(mode, "hang"))
			g_assert_error(out.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	}
	g_clear_object(&out.image);
	g_clear_error(&out.error);
}

static void
test_missing_helper(void)
{
	const gchar *argv[] = { "/nonexistent/ai-tui-clipboard", NULL };
	ClipboardResult out = { FALSE, NULL, NULL };

	tui_clipboard_read_async(argv, NULL, 100, clipboard_done, &out);
	while (!out.done) g_main_context_iteration(NULL, TRUE);
	g_assert_null(out.image);
	g_assert_error(out.error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT);
	g_clear_error(&out.error);
}

/* Recognition must follow bytes rather than a filename or declared MIME. */
static void
test_formats(void)
{
	const gchar *signatures[] = { "\377\330\377", "GIF89a", "RIFFxxxxWEBP" };
	const gchar *mimes[] = { "image/jpeg", "image/gif", "image/webp" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(signatures); i++)
	{
		g_autoptr(GBytes) bytes = g_bytes_new_static(signatures[i], strlen(signatures[i]));
		g_autoptr(GError) error = NULL;
		g_autoptr(AiImageContent) image = tui_image_from_bytes(bytes, &error);
		g_assert_no_error(error);
		g_assert_cmpstr(ai_image_get_mime_type(ai_image_content_get_image(image)), ==, mimes[i]);
	}
}

/* Use actual Ctrl+V/Enter dispatch, then inspect the message retained by the
 * conversation. A second send while busy must leave the next draft intact. */
static void
test_composer(void)
{
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(mock));
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	FILE *input = tmpfile();
	FILE *output = tmpfile();
	SCREEN *screen = newterm("xterm", output, input);
	App app = { 0 };
	GList *blocks;
	guint i;

	g_assert_nonnull(screen);
	app.conversation = conversation;
	app.input = g_string_new("What is in this image?");
	app.cursor = (guint)app.input->len;
	app.input_win = newwin(5, 80, 0, 0);
	nodelay(app.input_win, TRUE);
	keypad(app.input_win, TRUE);
	app.history = g_ptr_array_new_with_free_func(g_free);
	app.dump_loop = loop;

	unget_wch(22);
	drain_keys(&app);
	g_assert_true(app.clipboard_pending);
	app_send(&app);
	g_assert_cmpuint(app.input->len, >, 0);
	while (app.clipboard_pending) g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(g_list_length(app.images), ==, 1);
	for (i = 1; i < TUI_IMAGE_MAX_COUNT; i++)
		app.images = g_list_append(app.images, png_image());
	paste_image(&app);
	g_assert_false(app.clipboard_pending);
	g_assert_cmpuint(g_list_length(app.images), ==, TUI_IMAGE_MAX_COUNT);

	ai_mock_provider_push_text(mock, "A pixel");
	ai_mock_provider_push_text(mock, "Another look");
	unget_wch('\n');
	drain_keys(&app);
	g_assert_null(app.images);
	g_assert_true(app.sending);
	g_string_assign(app.input, "next draft");
	app.images = g_list_append(NULL, png_image());
	app_send(&app);
	g_assert_cmpuint(app.input->len, ==, 0);
	g_assert_null(app.images);
	g_assert_cmpuint(g_queue_get_length(&app.send_queue), ==, 1);
	g_main_loop_run(loop);
	g_assert_true(g_queue_is_empty(&app.send_queue));
	blocks = ai_message_get_content_blocks(ai_conversation_get_messages(conversation)->data);
	g_assert_cmpuint(g_list_length(blocks), ==, 5);
	g_assert_true(AI_IS_IMAGE_CONTENT(blocks->next->data));
	g_assert_cmpuint(g_list_length(ai_conversation_get_messages(conversation)), >=, 3);
	g_assert_true(handle_interrupt(&app) == FALSE);
	g_assert_null(app.images);
	g_assert_cmpuint(app.input->len, ==, 0);

	/* Image-only messages and --no-expand use the same structured path. */
	opt_no_expand = TRUE;
	app.images = g_list_append(NULL, png_image());
	ai_mock_provider_push_text(mock, "Another pixel");
	app_send(&app);
	g_assert_true(app.sending);
	g_main_loop_run(loop);
	opt_no_expand = FALSE;
	g_assert_null(app.images);
	g_clear_object(&app.cancellable);
	app_clear_send_queue(&app);
	g_string_free(app.input, TRUE);
	g_ptr_array_unref(app.history);
	delwin(app.input_win);
	endwin();
	delscreen(screen);
	fclose(input);
	fclose(output);
}

/* Unsupported wrappers preserve both parts of the draft, while local
 * commands still work so the user can switch provider without losing images. */
static void
test_draft_rejection(void)
{
	g_autoptr(AiGrokBuildClient) provider = ai_grok_build_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(AiCommandSet) commands = ai_command_set_new(NULL);
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	App app = { 0 };

	app.conversation = conversation;
	app.input = g_string_new("describe");
	app.images = g_list_append(NULL, png_image());
	app.commands = commands;
	app.history = g_ptr_array_new_with_free_func(g_free);
	app.dump_loop = loop;
	ai_conversation_set_command_set(conversation, commands);
	app_send(&app);
	g_assert_false(app.sending);
	g_assert_cmpstr(app.input->str, ==, "describe");
	g_assert_nonnull(app.images);
	g_assert_null(ai_conversation_get_messages(conversation));
	g_string_assign(app.input, "/clear");
	app_send(&app);
	g_assert_true(app.sending);
	g_main_loop_run(loop);
	g_assert_nonnull(app.images);
	g_assert_null(ai_conversation_get_messages(conversation));
	g_clear_object(&app.cancellable);
	g_clear_list(&app.images, g_object_unref);
	app_clear_send_queue(&app);
	g_string_free(app.input, TRUE);
	g_ptr_array_unref(app.history);
}

static const gchar *
argument_value(gchar **argv, const gchar *key)
{
	guint i;

	for (i = 0; argv[i] != NULL; i++)
		if (g_str_equal(argv[i], key)) return argv[i + 1];
	return NULL;
}

/* Codex must see readable, private files with byte-for-byte image content,
 * including --image at resume scope. Old files disappear on the next turn. */
static void
test_codex_files(void)
{
	g_autoptr(AiCodexCliClient) client = ai_codex_cli_client_new();
	g_autoptr(AiMessage) message = ai_message_new_user("look");
	g_autoptr(GBytes) expected = png_bytes();
	g_autoptr(GList) messages = g_list_append(NULL, message);
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *directory = NULL;
	gsize length;
	struct stat info;
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

	ai_message_add_content_block(message, AI_CONTENT_BLOCK(png_image()));
	argv = klass->build_argv(AI_CLI_CLIENT(client), messages, NULL, 0, FALSE);
	g_assert_nonnull(argv);
	path = g_strdup(argument_value(argv, "--image"));
	g_assert_nonnull(path);
	directory = g_path_get_dirname(path);
	g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
	g_assert_cmpmem(contents, length, g_bytes_get_data(expected, NULL), g_bytes_get_size(expected));
	g_assert_cmpint(g_stat(path, &info), ==, 0);
	g_assert_cmpint(info.st_mode & 0777, ==, 0600);
	g_strfreev(argv);
	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "session-test");
	argv = klass->build_argv(AI_CLI_CLIENT(client), messages, NULL, 0, TRUE);
	g_assert_cmpstr(argument_value(argv, "resume"), ==, "session-test");
	g_assert_nonnull(argument_value(argv, "--image"));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(directory, G_FILE_TEST_EXISTS));
	g_free(path);
	path = g_strdup(argument_value(argv, "--image"));
	g_clear_object(&client);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
}

/* Both JSON-input wrappers must put real base64 in the event, not just an
 * image marker in the text projection. Claude requires matching output mode. */
static void
test_cli_event(gconstpointer data)
{
	g_autoptr(GObject) provider = GINT_TO_POINTER(1) == data ?
		G_OBJECT(ai_claude_code_client_new()) : G_OBJECT(ai_antigravity_client_new());
	AiCliClient *client = AI_CLI_CLIENT(provider);
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(AiMessage) message = ai_message_new_user("describe");
	g_autoptr(GList) messages = g_list_append(NULL, message);
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *input = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonObject *event;
	JsonArray *content;
	JsonObject *image;

	ai_message_add_content_block(message, AI_CONTENT_BLOCK(png_image()));
	argv = klass->build_argv(client, messages, NULL, 0, FALSE);
	g_assert_cmpstr(argument_value(argv, "--input-format"), ==, "stream-json");
	g_assert_cmpstr(argument_value(argv, "--output-format"), ==, "stream-json");
	input = klass->build_stdin(client, messages);
	g_assert_true(json_parser_load_from_data(parser, input, -1, &error));
	g_assert_no_error(error);
	event = json_node_get_object(json_parser_get_root(parser));
	g_assert_cmpstr(json_object_get_string_member(event, "type"), ==, "user");
	content = json_object_get_array_member(json_object_get_object_member(event, "message"), "content");
	g_assert_cmpuint(json_array_get_length(content), ==, 2);
	image = json_array_get_object_element(content, 1);
	g_assert_cmpstr(json_object_get_string_member(image, "type"), ==, "image");
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(image, "source"), "data"), ==, png_base64);
	if (AI_IS_CLAUDE_CODE_CLIENT(client))
	{
		g_autoptr(AiResponse) response = klass->parse_json_output(client,
			"{\"type\":\"system\"}\n{\"type\":\"result\",\"result\":\"pixel\",\"session_id\":\"s\"}\n", &error);
		g_assert_no_error(error);
		g_assert_nonnull(response);
	}
}

/* The child fixture asserts that image bytes survive the actual subprocess
 * invocation, in both transport modes and again on session resume. */
static void
test_cli_roundtrip(gconstpointer data)
{
	guint variant = GPOINTER_TO_UINT(data);
	g_autoptr(GObject) provider = variant / 2 == 0 ? G_OBJECT(ai_codex_cli_client_new()) :
		variant / 2 == 1 ? G_OBJECT(ai_claude_code_client_new()) : G_OBJECT(ai_antigravity_client_new());
	g_autoptr(AiConversation) conversation = ai_conversation_new(provider);
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(AiImageContent) image = png_image();
	g_autoptr(GList) images = g_list_append(NULL, image);
	guint i;
	App app = { 0 };

	app.conversation = conversation;
	app.dump_loop = loop;
	ai_conversation_set_stream(conversation, variant % 2);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *text = NULL;
		GList *messages;

		ai_conversation_send_images_async(conversation, NULL, "describe", images, NULL, on_sent, &app);
		g_main_loop_run(loop);
		messages = ai_conversation_get_messages(conversation);
		g_assert_cmpuint(g_list_length(messages), ==, (i + 1) * 2);
		text = ai_message_get_text(g_list_last(messages)->data);
		g_assert_cmpstr(text, ==, "image received");
	}
}

/* Minimal native CLI stand-in. Assertions fail the child instead of claiming
 * a model response when --image, stdin, MIME or session slicing is broken. */
static gint
cli_fixture(gchar **argv)
{
	g_autoptr(GBytes) expected = png_bytes();
	g_autoptr(GString) input = g_string_new(NULL);
	gchar buffer[4096];
	gsize length;
	const gchar *path = argument_value(argv, "--image");

	while ((length = fread(buffer, 1, sizeof(buffer), stdin)) > 0)
		g_string_append_len(input, buffer, (gssize)length);
	if (path != NULL)
	{
		g_autofree gchar *contents = NULL;
		guint i;
		guint count = 0;

		for (i = 0; argv[i] != NULL; i++)
			if (g_str_equal(argv[i], "--image")) count++;
		g_assert_cmpuint(count, ==, 1);
		g_assert_true(g_file_get_contents(path, &contents, &length, NULL));
		g_assert_cmpmem(contents, length, g_bytes_get_data(expected, NULL), g_bytes_get_size(expected));
		g_assert_nonnull(strstr(input->str, "describe"));
		fputs("{\"type\":\"thread.started\",\"thread_id\":\"test-images\"}\n"
		      "{\"type\":\"item.completed\",\"item\":{\"id\":\"1\",\"type\":\"agent_message\",\"text\":\"image received\"}}\n"
		      "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}\n", stdout);
	}
	else
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *event;
		JsonArray *content;
		JsonObject *image;

		g_assert_cmpstr(argument_value(argv, "--input-format"), ==, "stream-json");
		g_assert_cmpstr(argument_value(argv, "--output-format"), ==, "stream-json");
		g_assert_true(json_parser_load_from_data(parser, input->str, -1, NULL));
		event = json_node_get_object(json_parser_get_root(parser));
		content = json_object_get_array_member(json_object_get_object_member(event, "message"), "content");
		g_assert_cmpuint(json_array_get_length(content), ==, 2);
		image = json_object_get_object_member(json_array_get_object_element(content, 1), "source");
		g_assert_cmpstr(json_object_get_string_member(image, "media_type"), ==, "image/png");
		g_assert_cmpstr(json_object_get_string_member(image, "data"), ==, png_base64);
		if (g_str_equal(argv[1], "--input-format"))
			fputs("{\"event\":\"result\",\"result\":{\"status\":\"SUCCESS\",\"response\":\"image received\",\"conversation_id\":\"test-images\"}}\n", stdout);
		else
			fputs("{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"test-images\"}\n"
		      "{\"type\":\"result\",\"result\":\"image received\",\"session_id\":\"test-images\",\"is_error\":false,\"status\":\"SUCCESS\"}\n", stdout);
	}
	return 0;
}

/* Child selection owner: stdout is binary, errors/oversize/deadlocks are
 * intentional. No shell, credentials, real clipboard or network is used. */
static gint
clipboard_fixture(const gchar *mode)
{
	g_autoptr(GBytes) bytes = png_bytes();
	gsize size;
	gconstpointer data = g_bytes_get_data(bytes, &size);

	if (g_str_equal(mode, "png")) return fwrite(data, 1, size, stdout) == size ? 0 : 1;
	if (g_str_equal(mode, "bad")) { fputs("text, not an image", stdout); return 0; }
	if (g_str_equal(mode, "empty")) return 0;
	if (g_str_equal(mode, "fail")) return 1;
	if (g_str_equal(mode, "oversize"))
	{
		guint i;
		gchar buffer[65536] = { 0 };
		for (i = 0; i < 82; i++) fwrite(buffer, 1, sizeof(buffer), stdout);
		return 0;
	}
	if (g_str_equal(mode, "hang-after-eof")) close(STDOUT_FILENO);
	g_usleep(10000000);
	return 0;
}

int
main(int argc, char **argv)
{
	const gchar *modes[] = { "png", "bad", "empty", "fail", "oversize", "hang", "hang-after-eof", "cancel" };
	g_autofree gchar *directory = NULL;
	g_autofree gchar *helper = NULL;
	g_autofree gchar *path = NULL;
	guint i;
	gint result;

	if (argc > 2 && g_str_equal(argv[1], "--clipboard-fixture")) return clipboard_fixture(argv[2]);
	if (argc > 1 && g_str_equal(argv[1], "--no-newline")) return clipboard_fixture("png");
	if (argc > 1 && (g_str_equal(argv[1], "--ask-for-approval") || g_str_equal(argv[1], "--print") || g_str_equal(argv[1], "--input-format"))) return cli_fixture(argv);
	test_binary = g_canonicalize_filename(argv[0], NULL);
	directory = g_dir_make_tmp("ai-tui-images-test-XXXXXX", NULL);
	helper = g_build_filename(directory, "wl-paste", NULL);
	g_assert_cmpint(symlink(test_binary, helper), ==, 0);
	path = g_strconcat(directory, ":", g_getenv("PATH"), NULL);
	g_setenv("PATH", path, TRUE);
	g_setenv("WAYLAND_DISPLAY", "hermetic-test", TRUE);
	g_setenv("CODEX_PATH", test_binary, TRUE);
	g_setenv("CLAUDE_CODE_PATH", test_binary, TRUE);
	g_setenv("AGY_PATH", test_binary, TRUE);
	g_setenv("HOME", directory, TRUE);
	g_setenv("XDG_CONFIG_HOME", directory, TRUE);
	g_setenv("XDG_STATE_HOME", directory, TRUE);
	setlocale(LC_ALL, "C.UTF-8");
	g_test_init(&argc, &argv, NULL);
	for (i = 0; i < G_N_ELEMENTS(modes); i++)
	{
		g_autofree gchar *name = g_strconcat("/tui-images/clipboard/", modes[i], NULL);
		g_test_add_data_func(name, modes[i], test_clipboard);
	}
	g_test_add_func("/tui-images/missing-helper", test_missing_helper);
	g_test_add_func("/tui-images/formats", test_formats);
	g_test_add_func("/tui-images/composer", test_composer);
	g_test_add_func("/tui-images/draft-rejection", test_draft_rejection);
	g_test_add_func("/tui-images/codex-files", test_codex_files);
	g_test_add_data_func("/tui-images/claude-event", GINT_TO_POINTER(1), test_cli_event);
	g_test_add_data_func("/tui-images/agy-event", GINT_TO_POINTER(2), test_cli_event);
	for (i = 0; i < 6; i++)
	{
		g_autofree gchar *name = g_strdup_printf("/tui-images/cli-roundtrip/%u", i);
		g_test_add_data_func(name, GUINT_TO_POINTER(i), test_cli_roundtrip);
	}
	result = g_test_run();
	g_unlink(helper);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
	g_free(test_binary);
	return result;
}
