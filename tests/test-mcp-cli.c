/*
 * test-mcp-cli.c - Real-process MCP protocol and CLI integration
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <json-glib/json-glib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "core/ai-subprocess-util.h"

#define DEADLINE_MS (8000)

typedef struct
{
	GSubprocess *process;
	GString *buffer;
	gint64 next_id;
	gboolean socket_server;
} Peer;

static gchar *test_executable;
static gchar *ai_executable;
static gchar *tui_executable;
static gchar *fixture_directory;
static const gchar *stub_pid_path;

typedef struct
{
	GMainLoop *loop;
	GSubprocess *process;
	GError *error;
	gboolean timed_out;
} ProcessWait;

/* Wait for process exit without draining stdout: essential for backpressure tests. */
static void
process_waited(GObject *source, GAsyncResult *result, gpointer user_data)
{
	ProcessWait *wait = user_data;
	g_subprocess_wait_finish(G_SUBPROCESS(source), result, &wait->error);
	g_main_loop_quit(wait->loop);
}

static gboolean
process_wait_expired(gpointer user_data)
{
	ProcessWait *wait = user_data;
	wait->timed_out = TRUE;
	g_subprocess_force_exit(wait->process);
	return G_SOURCE_REMOVE;
}

static void
wait_without_reading(GSubprocess *process)
{
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	ProcessWait wait = { loop, process, NULL, FALSE };
	guint timeout = g_timeout_add(DEADLINE_MS, process_wait_expired, &wait);

	g_subprocess_wait_async(process, NULL, process_waited, &wait);
	g_main_loop_run(loop);
	if (!wait.timed_out) g_source_remove(timeout);
	g_assert_false(wait.timed_out);
	g_assert_no_error(wait.error);
}

/* Every subprocess owns isolated ai configuration and a deterministic CLI stub. */
static GSubprocess *
spawn_process(const gchar * const *argv, const gchar *mode)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GError) error = NULL;
	GSubprocess *process;

	g_subprocess_launcher_setenv(launcher, "CLAUDE_CODE_PATH", test_executable, TRUE);
	g_subprocess_launcher_setenv(launcher, "AI_MCP_TEST_STUB", mode != NULL ? mode : "normal", TRUE);
	if (stub_pid_path != NULL)
		g_subprocess_launcher_setenv(launcher, "AI_MCP_TEST_PID_FILE", stub_pid_path, TRUE);
	g_subprocess_launcher_setenv(launcher, "NO_COLOR", "1", TRUE);
	g_subprocess_launcher_setenv(launcher, "TERM", "dumb", TRUE);
	if (fixture_directory != NULL)
	{
		g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", fixture_directory, TRUE);
		g_subprocess_launcher_setenv(launcher, "XDG_STATE_HOME", fixture_directory, TRUE);
		g_subprocess_launcher_setenv(launcher, "XDG_DATA_HOME", fixture_directory, TRUE);
		g_subprocess_launcher_set_cwd(launcher, fixture_directory);
	}
	process = g_subprocess_launcher_spawnv(launcher, argv, &error);
	g_assert_no_error(error);
	g_assert_nonnull(process);
	return process;
}

/* This path closes stdin, drains both pipes, and bounds child shutdown. */
static void
finish_process(GSubprocess *process, gboolean success)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *diagnostic = NULL;

	g_assert_true(ai_subprocess_communicate_utf8_bounded(process, NULL, DEADLINE_MS,
		NULL, &output, &diagnostic, &error));
	g_assert_no_error(error);
	if (g_subprocess_get_successful(process) != success)
		g_error("Unexpected process status: stdout=%s stderr=%s", output, diagnostic);
}

/* Graceful bounded shutdown exercises cleanup of the host's private grants. */
static void
peer_free(Peer *peer)
{
	if (peer == NULL) return;
	if (peer->socket_server) g_subprocess_send_signal(peer->process, SIGTERM);
	finish_process(peer->process, TRUE);
	g_object_unref(peer->process);
	g_string_free(peer->buffer, TRUE);
	g_free(peer);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Peer, peer_free)

static Peer *
peer_new(const gchar * const *argv, const gchar *mode)
{
	Peer *peer = g_new0(Peer, 1);
	peer->process = spawn_process(argv, mode);
	peer->buffer = g_string_new(NULL);
	peer->socket_server = g_strv_contains(argv, "--mcp-socket");
	return peer;
}

/* Small requests stay below pipe capacity; replies are bounded and polled. */
static void
peer_write(Peer *peer, const gchar *data)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(g_output_stream_write_all(g_subprocess_get_stdin_pipe(peer->process),
		data, strlen(data), NULL, NULL, &error));
	g_assert_no_error(error);
}

static JsonNode *
peer_line(Peer *peer)
{
	gint64 deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
	GInputStream *input = g_subprocess_get_stdout_pipe(peer->process);

	g_assert_true(G_IS_POLLABLE_INPUT_STREAM(input));
	while (g_get_monotonic_time() < deadline)
	{
		gchar *newline = strchr(peer->buffer->str, '\n');
		gchar data[4096];
		gssize count;
		g_autoptr(GError) error = NULL;
		if (newline != NULL)
		{
			g_autoptr(JsonParser) parser = json_parser_new();
			gsize length = (gsize)(newline - peer->buffer->str);
			g_assert_true(json_parser_load_from_data(parser, peer->buffer->str, length, &error));
			g_assert_no_error(error);
			g_string_erase(peer->buffer, 0, length + 1);
			return json_node_copy(json_parser_get_root(parser));
		}
		count = g_pollable_input_stream_read_nonblocking(G_POLLABLE_INPUT_STREAM(input), data,
			sizeof data, NULL, &error);
		if (count > 0)
		{
			g_string_append_len(peer->buffer, data, count);
			g_assert_cmpuint(peer->buffer->len, <, 2 * 1024 * 1024);
		}
		else if (count < 0 && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK))
			g_usleep(1000);
		else
			g_error("MCP stream ended before its response (%s)", error != NULL ? error->message : "EOF");
	}
	g_error("MCP reply exceeded %d milliseconds", DEADLINE_MS);
	return NULL;
}

/* Ignore notifications while matching request IDs; retain protocol errors. */
static JsonNode *
peer_request(Peer *peer, const gchar *method, const gchar *params)
{
	gint64 id = ++peer->next_id;
	g_autofree gchar *request = g_strdup_printf(
		"{\"jsonrpc\":\"2.0\",\"id\":%" G_GINT64_FORMAT ",\"method\":\"%s\",\"params\":%s}\n",
		id, method, params != NULL ? params : "{}");
	guint i;

	peer_write(peer, request);
	for (i = 0; i < 100; i++)
	{
		g_autoptr(JsonNode) response = peer_line(peer);
		JsonObject *object = json_node_get_object(response);
		if (json_object_has_member(object, "id") && json_object_get_int_member(object, "id") == id)
			return g_steal_pointer(&response);
	}
	g_error("Too many unrelated MCP notifications");
	return NULL;
}

static void
peer_initialize(Peer *peer)
{
	g_autoptr(JsonNode) reply = peer_request(peer, "initialize",
		"{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"ai-mcp-test\",\"version\":\"1\"}}");
	JsonObject *object = json_node_get_object(reply);
	g_assert_true(json_object_has_member(object, "result"));
	g_assert_false(json_object_has_member(object, "error"));
	peer_write(peer, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
}

/* Return tool text, preserving the distinction between RPC and tool errors. */
static gchar *
peer_tool(Peer *peer, const gchar *name, const gchar *arguments, gboolean success)
{
	g_autofree gchar *params = g_strdup_printf("{\"name\":\"%s\",\"arguments\":%s}", name, arguments);
	g_autoptr(JsonNode) reply = peer_request(peer, "tools/call", params);
	JsonObject *object = json_node_get_object(reply);
	JsonObject *result;
	JsonArray *content;
	gboolean failed;

	g_assert_false(json_object_has_member(object, "error"));
	result = json_object_get_object_member(object, "result");
	failed = json_object_has_member(result, "isError") && json_object_get_boolean_member(result, "isError");
	content = json_object_get_array_member(result, "content");
	g_assert_cmpuint(json_array_get_length(content), >, 0);
	if (failed == success)
		g_error("Tool %s unexpected result: %s", name,
			json_object_get_string_member(json_array_get_object_element(content, 0), "text"));
	return g_strdup(json_object_get_string_member(json_array_get_object_element(content, 0), "text"));
}

static JsonNode *
parse_text(const gchar *text)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
	g_assert_no_error(error);
	return json_node_copy(json_parser_get_root(parser));
}

/* A real CLI stub consumes the host-written config and invokes its bridge. */
static gint
stub_main(gint argc, gchar **argv)
{
	const gchar *mode = g_getenv("AI_MCP_TEST_STUB");
	const gchar *config = NULL;
	gboolean stream = FALSE;
	gboolean instructions = FALSE;
	gint i;

	if (g_str_equal(mode, "wait"))
	{
		const gchar *pid_path = g_getenv("AI_MCP_TEST_PID_FILE");
		if (pid_path != NULL)
		{
			g_autofree gchar *pid_text = g_strdup_printf("%ld\n", (long)getpid());
			g_autoptr(GError) error = NULL;
			g_assert_true(g_file_set_contents(pid_path, pid_text, -1, &error));
			g_assert_no_error(error);
		}
		g_usleep(30 * G_USEC_PER_SEC);
		return 0;
	}
	for (i = 1; i + 1 < argc; i++)
	{
		if (g_str_equal(argv[i], "--mcp-config")) config = argv[i + 1];
		if (g_str_equal(argv[i], "--output-format")) stream = g_str_equal(argv[i + 1], "stream-json");
		if (strstr(argv[i], "host todo_write") != NULL) instructions = TRUE;
		if (g_str_equal(argv[i], "--system-prompt-file") || g_str_equal(argv[i], "--append-system-prompt-file"))
		{
			g_autofree gchar *prompt = NULL;
			g_autoptr(GError) error = NULL;
			g_assert_true(g_file_get_contents(argv[i + 1], &prompt, NULL, &error));
			g_assert_no_error(error);
			if (strstr(prompt, "host todo_write") != NULL) instructions = TRUE;
		}
	}
	if (g_str_equal(mode, "no-inject"))
		g_assert_null(config);
	else
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autoptr(GError) error = NULL;
		JsonObject *server;
		JsonArray *args;
		const gchar *bridge_argv[5];
		g_autoptr(Peer) bridge = NULL;
		g_autofree gchar *result = NULL;

		g_assert_nonnull(config);
		g_assert_true(instructions);
		g_assert_true(json_parser_load_from_file(parser, config, &error));
		g_assert_no_error(error);
		server = json_object_get_object_member(json_object_get_object_member(
			json_node_get_object(json_parser_get_root(parser)), "mcpServers"), "ai_host");
		args = json_object_get_array_member(server, "args");
		g_assert_cmpuint(json_array_get_length(args), ==, 2);
		bridge_argv[0] = json_object_get_string_member(server, "command");
		bridge_argv[1] = json_array_get_string_element(args, 0);
		bridge_argv[2] = json_array_get_string_element(args, 1);
		bridge_argv[3] = NULL;
		g_assert_cmpstr(bridge_argv[1], ==, "--mcp-connect");
		bridge = peer_new(bridge_argv, "normal");
		peer_initialize(bridge);
		result = peer_tool(bridge, "todo_write",
			"{\"todos\":[{\"content\":\"MCP integration task\",\"active_form\":\"Updating MCP integration\",\"status\":\"in_progress\"}]}", TRUE);
		/* Closing bridge stdin exercises EOF cancellation without killing the host. */
		finish_process(bridge->process, TRUE);
		g_clear_object(&bridge->process);
		g_string_free(bridge->buffer, TRUE);
		g_free(g_steal_pointer(&bridge));
	}
	if (stream)
		g_print("{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"MCP bridge exercised\"}]}}\n");
	g_print("{\"type\":\"result\",\"subtype\":\"success\",\"result\":\"MCP bridge exercised\",\"session_id\":\"mcp-fixture-session\"}\n");
	return 0;
}

/* No grant means no tools, for both front ends; EOF exits cleanly. */
static void
test_stdio_default(gconstpointer data)
{
	const gchar *binary = data;
	const gchar *argv[] = { binary, "--provider", "claude-code", "--mcp-server", NULL };
	g_autoptr(Peer) peer = peer_new(argv, NULL);
	g_autoptr(JsonNode) reply = NULL;
	JsonArray *tools;

	peer_initialize(peer);
	reply = peer_request(peer, "tools/list", "{}");
	tools = json_object_get_array_member(json_object_get_object_member(json_node_get_object(reply), "result"), "tools");
	g_assert_cmpuint(json_array_get_length(tools), ==, 0);
	finish_process(peer->process, TRUE);
	g_clear_object(&peer->process);
	g_string_free(peer->buffer, TRUE);
	g_free(g_steal_pointer(&peer));
}

/* Explicit tool lists compose, deduplicate, and deny unknown tools by protocol. */
static void
test_stdio_allowlist(gconstpointer data)
{
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-no-inject",
		"--mcp-tools", "todo_read,todo_write", "--mcp-tools", "todo_read,conversation_status", NULL };
	g_autoptr(Peer) peer = peer_new(argv, NULL);
	g_autoptr(JsonNode) reply = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *object;
	JsonArray *tools;

	peer_initialize(peer);
	reply = peer_request(peer, "tools/list", "{}");
	tools = json_object_get_array_member(json_object_get_object_member(json_node_get_object(reply), "result"), "tools");
	g_assert_cmpuint(json_array_get_length(tools), ==, 3);
	g_clear_pointer(&reply, json_node_unref);
	reply = peer_request(peer, "tools/call", "{\"name\":\"conversation_send\",\"arguments\":{\"prompt\":\"not granted\"}}");
	object = json_node_get_object(reply);
	g_assert_true(json_object_has_member(object, "error") ||
		json_object_get_boolean_member(json_object_get_object_member(object, "result"), "isError"));
	text = peer_tool(peer, "conversation_status", "{}", TRUE);
	{
		g_autoptr(JsonNode) status = parse_text(text);
		g_assert_false(json_object_get_boolean_member(json_node_get_object(status), "busy"));
	}
}

/* Bad invocations fail before starting a provider or consuming a prompt. */
static void
test_cli_errors(gconstpointer data)
{
	const gchar *binary = data;
	const gchar *cases[][6] = {
		{ "--mcp-server", "unexpected prompt", NULL },
		{ "--mcp-server", "--mcp-tools", "unknown_tool", NULL },
		{ "--mcp-server", "--mcp-all-tools", "--mcp-tools", "unknown_tool", NULL },
		{ "--mcp-server", "--mcp-tools", "todo_read,", NULL },
		{ "--mcp-server", "--mcp-connect", "/tmp/absent.sock", NULL },
		{ "--mcp-list-tools", "--mcp-all-tools", NULL },
		{ "--mcp-no-inject", NULL },
		{ "--mcp-connect", "relative.sock", NULL },
		{ "--mcp-server", "--mcp-socket", "relative.sock", NULL },
		{ "--mcp-server", "--mcp-socket", "/tmp/ai-mcp-insecure-parent.sock", NULL }
	};
	guint i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		const gchar *argv[12] = { binary, "--provider", "claude-code", NULL };
		g_autoptr(GSubprocess) process = NULL;
		guint j;
		for (j = 0; cases[i][j] != NULL; j++) argv[3 + j] = cases[i][j];
		process = spawn_process(argv, NULL);
		finish_process(process, FALSE);
	}
}

/* Listing is independent of provider authentication and matches the all-tools grant. */
static void
test_catalog(gconstpointer data)
{
	const gchar *argv[] = { data, "--mcp-list-tools", NULL };
	const gchar *server_argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-all-tools", "--mcp-no-inject", NULL };
	g_autoptr(GSubprocess) process = spawn_process(argv, NULL);
	g_autofree gchar *output = NULL;
	g_autofree gchar *diagnostic = NULL;
	g_auto(GStrv) names = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(Peer) peer = NULL;
	g_autoptr(JsonNode) reply = NULL;
	JsonArray *tools;
	guint i;

	g_assert_true(ai_subprocess_communicate_utf8_bounded(process, NULL, DEADLINE_MS, NULL, &output, &diagnostic, &error));
	g_assert_no_error(error);
	g_assert_true(g_subprocess_get_successful(process));
	names = g_strsplit(g_strstrip(output), "\n", -1);
	g_assert_cmpuint(g_strv_length(names), ==, 11);
	peer = peer_new(server_argv, NULL);
	peer_initialize(peer);
	reply = peer_request(peer, "tools/list", "{}");
	tools = json_object_get_array_member(json_object_get_object_member(json_node_get_object(reply), "result"), "tools");
	g_assert_cmpuint(json_array_get_length(tools), ==, g_strv_length(names));
	for (i = 0; i < json_array_get_length(tools); i++)
		g_assert_true(g_strv_contains((const gchar * const *)names,
			json_object_get_string_member(json_array_get_object_element(tools, i), "name")));
}

/* Wait only for a short-lived socket bind; the containing directory is private. */
static void
wait_socket(const gchar *path)
{
	gint64 deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
	while (!g_file_test(path, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline) g_usleep(1000);
	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
}

/* Two independent bridges address one session; invalid writes preserve old state. */
static void
test_socket_shared(gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("ai MCP quoted-XXXXXX", &error);
	g_autofree gchar *path = g_build_filename(directory, "host ' quoted.sock", NULL);
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-socket", path,
		"--mcp-all-tools", "--mcp-no-inject", NULL };
	const gchar *bridge_argv[] = { data, "--mcp-connect", path, NULL };
	g_autoptr(Peer) server = NULL;
	g_autoptr(Peer) first = NULL;
	g_autoptr(Peer) second = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	g_autoptr(JsonNode) malformed = NULL;

	g_assert_no_error(error);
	server = peer_new(argv, NULL);
	wait_socket(path);
	first = peer_new(bridge_argv, NULL);
	second = peer_new(bridge_argv, NULL);
	g_test_message("Initialize first socket bridge");
	peer_initialize(first);
	g_test_message("Initialize second socket bridge");
	peer_initialize(second);
	g_test_message("Write shared TODO state");
	text = peer_tool(first, "todo_write", "{\"todos\":[{\"content\":\"Shared task\",\"status\":\"pending\"}]}", TRUE);
	g_clear_pointer(&text, g_free);
	text = peer_tool(second, "todo_write", "{\"todos\":[{\"content\":\"Lost task\",\"status\":\"invalid\"}]}", FALSE);
	g_clear_pointer(&text, g_free);
	text = peer_tool(second, "todo_read", "{}", TRUE);
	g_assert_nonnull(strstr(text, "Shared task"));
	g_assert_null(strstr(text, "Lost task"));
	peer_write(first, "not-json\n");
	g_test_message("Read malformed-frame error");
	malformed = peer_line(first);
	g_assert_true(json_object_has_member(json_node_get_object(malformed), "error"));
	g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
		json_node_get_object(malformed), "error"), "code"), ==, -32700);
	g_clear_pointer(&text, g_free);
	text = peer_tool(first, "conversation_status", "{}", TRUE);
	parsed = parse_text(text);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(parsed), "todos"), ==, 1);
	g_clear_pointer(&first, peer_free);
	g_clear_pointer(&second, peer_free);
	g_subprocess_send_signal(server->process, SIGTERM);
	finish_process(server->process, TRUE);
	g_clear_object(&server->process);
	g_string_free(server->buffer, TRUE);
	g_free(g_steal_pointer(&server));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/* A pre-existing path must survive refusal byte-for-byte. */
static void
test_socket_existing(gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("ai-mcp-existing-XXXXXX", &error);
	g_autofree gchar *path = g_build_filename(directory, "host.sock", NULL);
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-socket", path, NULL };
	g_autoptr(GSubprocess) process = NULL;
	g_autofree gchar *contents = NULL;

	g_assert_no_error(error);
	g_assert_true(g_file_set_contents(path, "owned data", -1, &error));
	g_assert_no_error(error);
	process = spawn_process(argv, NULL);
	finish_process(process, FALSE);
	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(contents, ==, "owned data");
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/* Accepted turns are asynchronous; busy mutations fail and cancellation settles. */
static void
test_turn_control(gconstpointer data)
{
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-all-tools", "--mcp-no-inject", NULL };
	g_autoptr(Peer) peer = peer_new(argv, "wait");
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	gint64 deadline;
	gboolean busy = TRUE;

	peer_initialize(peer);
	text = peer_tool(peer, "conversation_send", "{\"prompt\":\"Wait until cancelled\"}", TRUE);
	parsed = parse_text(text);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(parsed), "operation_id"), ==, 1);
	g_clear_pointer(&text, g_free);
	text = peer_tool(peer, "conversation_send", "{\"prompt\":\"Must not replace\"}", FALSE);
	g_assert_nonnull(strstr(text, "busy"));
	g_clear_pointer(&text, g_free);
	text = peer_tool(peer, "conversation_clear", "{}", FALSE);
	g_assert_nonnull(strstr(text, "busy"));
	g_clear_pointer(&text, g_free);
	text = peer_tool(peer, "conversation_cancel", "{}", TRUE);
	deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
	while (busy && g_get_monotonic_time() < deadline)
	{
		g_clear_pointer(&text, g_free);
		g_clear_pointer(&parsed, json_node_unref);
		text = peer_tool(peer, "conversation_status", "{}", TRUE);
		parsed = parse_text(text);
		busy = json_object_get_boolean_member(json_node_get_object(parsed), "busy");
		if (busy) g_usleep(10000);
	}
	g_assert_false(busy);
	g_clear_pointer(&text, g_free);
	text = peer_tool(peer, "conversation_clear", "{}", TRUE);
	g_clear_pointer(&text, g_free);
	text = peer_tool(peer, "conversation_transcript", "{}", TRUE);
	g_clear_pointer(&parsed, json_node_unref);
	parsed = parse_text(text);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(parsed), "messages")), ==, 0);
}

/* The model-side stub connects back while the controller's turn is processing. */
static void
test_injected_turn(gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("ai MCP executable-XXXXXX", &error);
	g_autofree gchar *path = g_build_filename(directory, "ai \"host\"", NULL);
	g_autoptr(GFile) link = g_file_new_for_path(path);
	const gchar *argv[] = { path, "--provider", "claude-code", "--mcp-server", "--mcp-all-tools", NULL };
	g_autoptr(Peer) peer = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	gint64 deadline;
	gboolean busy = TRUE;

	g_assert_no_error(error);
	g_assert_true(g_file_make_symbolic_link(link, data, NULL, &error));
	g_assert_no_error(error);
	peer = peer_new(argv, NULL);
	peer_initialize(peer);
	text = peer_tool(peer, "conversation_send", "{\"prompt\":\"Update our host TODO panel\"}", TRUE);
	deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
	while (busy && g_get_monotonic_time() < deadline)
	{
		g_clear_pointer(&text, g_free);
		g_clear_pointer(&parsed, json_node_unref);
		text = peer_tool(peer, "conversation_status", "{}", TRUE);
		parsed = parse_text(text);
		busy = json_object_get_boolean_member(json_node_get_object(parsed), "busy");
		if (busy) g_usleep(10000);
	}
	g_assert_false(busy);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(parsed), "operation_state"), ==, "completed");
	g_clear_pointer(&text, g_free);
	text = peer_tool(peer, "todo_read", "{}", TRUE);
	g_assert_nonnull(strstr(text, "MCP integration task"));
	g_clear_pointer(&text, g_free);
	text = peer_tool(peer, "conversation_transcript", "{}", TRUE);
	g_assert_nonnull(strstr(text, "MCP bridge exercised"));
	g_clear_pointer(&peer, peer_free);
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/* The actual headless TUI renderer must show the MCP-updated TODOS panel. */
static void
test_tui_dump(void)
{
	const gchar *argv[] = { tui_executable, "--provider", "claude-code", "--mcp-tools", "todo_write,todo_read",
		"--dump", "Update our TODO panel", NULL };
	g_autoptr(GSubprocess) process = spawn_process(argv, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *diagnostic = NULL;

	g_assert_true(ai_subprocess_communicate_utf8_bounded(process, NULL, DEADLINE_MS, NULL, &output, &diagnostic, &error));
	g_assert_no_error(error);
	if (!g_subprocess_get_successful(process)) g_error("TUI MCP dump failed: %s", diagnostic);
	g_assert_nonnull(strstr(output, "MCP bridge exercised"));
	/* Active tasks display active_form rather than the pending content label. */
	g_assert_nonnull(strstr(output, "Updating MCP integration"));
}

/* No-inject keeps external tools available without passing them to the CLI. */
static void
test_no_inject(gconstpointer data)
{
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-all-tools", "--mcp-no-inject", NULL };
	g_autoptr(Peer) peer = peer_new(argv, "no-inject");
	g_autofree gchar *text = NULL;
	g_autoptr(JsonNode) status = NULL;
	gint64 deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
	gboolean busy = TRUE;

	peer_initialize(peer);
	text = peer_tool(peer, "conversation_send", "{\"prompt\":\"No injected configuration\"}", TRUE);
	while (busy && g_get_monotonic_time() < deadline)
	{
		g_clear_pointer(&text, g_free);
		g_clear_pointer(&status, json_node_unref);
		text = peer_tool(peer, "conversation_status", "{}", TRUE);
		status = parse_text(text);
		busy = json_object_get_boolean_member(json_node_get_object(status), "busy");
		if (busy) g_usleep(10000);
	}
	g_assert_false(busy);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(status), "operation_state"), ==, "completed");
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(status), "todos"), ==, 0);
}

/* An omitted provider inherits the host; AGENTS tracks an actual CLI process. */
static void
test_background_agents(gconstpointer data)
{
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-all-tools", "--mcp-no-inject", NULL };
	guint scenario;

	for (scenario = 0; scenario < 2; scenario++)
	{
		g_autoptr(Peer) peer = peer_new(argv, scenario == 0 ? "no-inject" : "wait");
		g_autofree gchar *text = NULL;
		g_autofree gchar *agent_id = NULL;
		g_autofree gchar *arguments = NULL;
		g_autoptr(JsonNode) status = NULL;
		const gchar *begin;
		const gchar *end;
		gint64 deadline;
		gboolean settled = FALSE;

		peer_initialize(peer);
		text = peer_tool(peer, "agent_spawn",
			"{\"prompt\":\"Run the background fixture\",\"description\":\"MCP background execution\"}", TRUE);
		g_assert_true(g_str_has_prefix(text, "Started agent '"));
		begin = strchr(text, '\'') + 1;
		end = strchr(begin, '\'');
		g_assert_nonnull(end);
		agent_id = g_strndup(begin, (gsize)(end - begin));
		arguments = g_strdup_printf("{\"agent_id\":\"%s\"}", agent_id);
		if (scenario == 1)
		{
			g_clear_pointer(&text, g_free);
			text = peer_tool(peer, "conversation_status", "{}", TRUE);
			status = parse_text(text);
			g_assert_cmpint(json_object_get_int_member(json_node_get_object(status), "agents_live"), ==, 1);
			g_clear_pointer(&text, g_free);
			text = peer_tool(peer, "agent_result", arguments, FALSE);
			g_clear_pointer(&text, g_free);
			text = peer_tool(peer, "agent_cancel", arguments, TRUE);
			g_assert_nonnull(strstr(text, "Stopped agent"));
		}
		deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
		while (!settled && g_get_monotonic_time() < deadline)
		{
			g_clear_pointer(&text, g_free);
			text = peer_tool(peer, "agent_status", arguments, TRUE);
			g_assert_nonnull(strstr(text, "MCP background execution"));
			if (strstr(text, "  failed  ") != NULL) g_error("Background fixture failed: %s", text);
			settled = strstr(text, scenario == 0 ? "  done  " : "  cancelled  ") != NULL;
			if (!settled) g_usleep(10000);
		}
		g_assert_true(settled);
		if (scenario == 0)
		{
			g_clear_pointer(&text, g_free);
			text = peer_tool(peer, "agent_result", arguments, TRUE);
			g_assert_nonnull(strstr(text, "MCP bridge exercised"));
			g_clear_pointer(&text, g_free);
			text = peer_tool(peer, "agent_status", "{}", TRUE);
			g_assert_cmpstr(text, ==, "No background agents.");
		}
		g_clear_pointer(&text, g_free);
		g_clear_pointer(&status, json_node_unref);
		text = peer_tool(peer, "conversation_status", "{}", TRUE);
		status = parse_text(text);
		g_assert_cmpint(json_object_get_int_member(json_node_get_object(status), "agents_live"), ==, 0);
	}
}

/* Oversized frames terminate the stdio server instead of wedging its main loop. */
static void
test_oversized_frame(gconstpointer data)
{
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", NULL };
	g_autoptr(Peer) peer = peer_new(argv, NULL);
	g_autofree gchar *frame = g_strnfill(1024 * 1024 + 2, 'x');
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *diagnostic = NULL;
	gboolean communicated;

	peer_initialize(peer);
	frame[1024 * 1024 + 1] = '\n';
	communicated = ai_subprocess_communicate_utf8_bounded(peer->process, frame, DEADLINE_MS,
		NULL, &output, &diagnostic, &error);
	/* The reader may close while the final bytes are still being written. */
	if (!communicated) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE);
	wait_without_reading(peer->process);
	g_assert_true(g_subprocess_get_if_exited(peer->process));
	g_assert_cmpint(g_subprocess_get_exit_status(peer->process), !=, 0);
	g_clear_object(&peer->process);
	g_string_free(peer->buffer, TRUE);
	g_free(g_steal_pointer(&peer));
}

/* Closing stdin must stop a bridge even while its stdout pipe is completely full. */
static void
test_bridge_backpressure(gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("ai-mcp-pressure-XXXXXX", &error);
	g_autofree gchar *path = g_build_filename(directory, "host.sock", NULL);
	const gchar *argv[] = { data, "--provider", "claude-code", "--mcp-server", "--mcp-socket", path,
		"--mcp-tools", "todo_read,todo_write", "--mcp-no-inject", NULL };
	const gchar *bridge_argv[] = { data, "--mcp-connect", path, NULL };
	g_autoptr(Peer) server = NULL;
	g_autoptr(Peer) writer = NULL;
	g_autoptr(Peer) blocked = NULL;
	g_autofree gchar *content = g_strnfill(16384, 'A');
	g_autofree gchar *arguments = g_strdup_printf("{\"todos\":[{\"content\":\"%s\",\"status\":\"pending\"}]}", content);
	g_autofree gchar *text = NULL;
	gint fd;
	gint capacity;
	gint pending = 0;
	gint64 deadline;

	g_assert_no_error(error);
	server = peer_new(argv, NULL);
	wait_socket(path);
	writer = peer_new(bridge_argv, NULL);
	peer_initialize(writer);
	text = peer_tool(writer, "todo_write", arguments, TRUE);
	blocked = peer_new(bridge_argv, NULL);
	peer_initialize(blocked);
	fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(g_subprocess_get_stdout_pipe(blocked->process)));
	capacity = fcntl(fd, F_SETPIPE_SZ, 4096);
	g_assert_cmpint(capacity, >, 0);
	g_assert_cmpint(capacity, <, 16384);
	peer_write(blocked, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"todo_read\",\"arguments\":{}}}\n");
	deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
	while (pending < capacity && g_get_monotonic_time() < deadline)
	{
		g_assert_cmpint(ioctl(fd, FIONREAD, &pending), ==, 0);
		if (pending < capacity) g_usleep(1000);
	}
	g_assert_cmpint(pending, ==, capacity);
	g_assert_true(g_output_stream_close(g_subprocess_get_stdin_pipe(blocked->process), NULL, &error));
	g_assert_no_error(error);
	wait_without_reading(blocked->process);
	g_assert_true(g_subprocess_get_successful(blocked->process));
	g_clear_object(&blocked->process);
	g_string_free(blocked->buffer, TRUE);
	g_free(g_steal_pointer(&blocked));
	g_clear_pointer(&writer, peer_free);
	g_clear_pointer(&server, peer_free);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/* Orphaned zombies await PID 1; they cannot retain a running provider session. */
static gboolean
fixture_process_running(pid_t pid)
{
	g_autofree gchar *path = g_strdup_printf("/proc/%ld/stat", (long)pid);
	g_autofree gchar *contents = NULL;
	const gchar *end;

	if (kill(pid, 0) < 0 && errno == ESRCH) return FALSE;
	if (!g_file_get_contents(path, &contents, NULL, NULL)) return FALSE;
	end = strrchr(contents, ')');
	g_assert_nonnull(end);
	return end[1] == ' ' && end[2] != 'Z' && end[2] != 'X';
}

/* Wait for the real worker PID before closing its owning host, avoiding a spawn race. */
static void
check_agent_shutdown(const gchar *binary, gboolean cancel_first)
{
	const gchar *argv[] = { binary, "--provider", "claude-code", "--mcp-server", "--mcp-all-tools", "--mcp-no-inject", NULL };
	g_autofree gchar *pid_path = g_build_filename(fixture_directory, "slow-provider.pid", NULL);
	g_autofree gchar *pid_text = NULL;
	g_autofree gchar *reply = NULL;
	g_autoptr(Peer) peer = NULL;
	g_autoptr(GError) error = NULL;
	gint64 deadline;
	pid_t pid;
	gboolean running;

	stub_pid_path = pid_path;
	peer = peer_new(argv, "wait");
	stub_pid_path = NULL;
	peer_initialize(peer);
	reply = peer_tool(peer, "agent_spawn", "{\"prompt\":\"Remain active until host shutdown\"}", TRUE);
	deadline = g_get_monotonic_time() + DEADLINE_MS * 1000;
	while (!g_file_test(pid_path, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < deadline)
		g_usleep(1000);
	g_assert_true(g_file_get_contents(pid_path, &pid_text, NULL, &error));
	g_assert_no_error(error);
	pid = (pid_t)g_ascii_strtoll(pid_text, NULL, 10);
	g_assert_cmpint(pid, >, 1);
	g_assert_true(fixture_process_running(pid));
	if (cancel_first)
	{
		g_clear_pointer(&reply, g_free);
		reply = peer_tool(peer, "agent_cancel", "{\"agent_id\":\"all\"}", TRUE);
	}
	/* Do not poll terminal agent state: close immediately after the cancel reply. */
	g_clear_pointer(&peer, peer_free);
	deadline = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;
	do
	{
		running = fixture_process_running(pid);
		if (running) g_usleep(10000);
	} while (running && g_get_monotonic_time() < deadline);
	/* A failing regression must still stop its own known slow fixture. */
	if (running) kill(pid, SIGTERM);
	g_assert_cmpint(g_unlink(pid_path), ==, 0);
	if (running) g_test_message("Provider PID %ld survived host exit for two seconds (cancel_first=%d)", (long)pid, cancel_first);
	g_assert_false(running);
}

static void
test_agent_shutdown_active(gconstpointer data)
{
	check_agent_shutdown(data, FALSE);
}

static void
test_agent_shutdown_cancelled(gconstpointer data)
{
	check_agent_shutdown(data, TRUE);
}

/* Remove only our temporary fixture tree; never follow directory symlinks. */
static void
remove_fixture(const gchar *path)
{
	g_autoptr(GDir) directory = g_dir_open(path, 0, NULL);
	const gchar *name;
	if (directory == NULL) return;
	while ((name = g_dir_read_name(directory)) != NULL)
	{
		g_autofree gchar *child = g_build_filename(path, name, NULL);
		if (!g_file_test(child, G_FILE_TEST_IS_SYMLINK) && g_file_test(child, G_FILE_TEST_IS_DIR))
			remove_fixture(child);
		else
			g_assert_cmpint(g_unlink(child), ==, 0);
	}
	g_clear_pointer(&directory, g_dir_close);
	g_assert_cmpint(g_rmdir(path), ==, 0);
}

int
main(int argc, char **argv)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = NULL;
	const gchar *binaries[2];
	guint i;
	gint result;

	test_executable = g_file_read_link("/proc/self/exe", &error);
	g_assert_no_error(error);
	if (g_getenv("AI_MCP_TEST_STUB") != NULL) return stub_main(argc, argv);
	fixture_directory = g_dir_make_tmp("ai-mcp-cli-fixture-XXXXXX", &error);
	g_assert_no_error(error);
	directory = g_path_get_dirname(test_executable);
	ai_executable = g_canonicalize_filename("../bin/ai", directory);
	tui_executable = g_canonicalize_filename("../bin/ai-tui", directory);
	g_test_init(&argc, &argv, NULL);
	binaries[0] = ai_executable;
	binaries[1] = tui_executable;
	for (i = 0; i < 2; i++)
	{
		const gchar *prefix = i == 0 ? "/mcp/cli/ai" : "/mcp/cli/tui";
		g_autofree gchar *path = NULL;
#define ADD_CASE(suffix, function) \
		g_clear_pointer(&path, g_free); \
		path = g_strconcat(prefix, suffix, NULL); \
		g_test_add_data_func(path, binaries[i], function)
		ADD_CASE("/stdio-default", test_stdio_default);
		ADD_CASE("/allowlist", test_stdio_allowlist);
		ADD_CASE("/errors", test_cli_errors);
		ADD_CASE("/catalog", test_catalog);
		ADD_CASE("/socket-shared", test_socket_shared);
		ADD_CASE("/socket-existing", test_socket_existing);
		ADD_CASE("/turn-control", test_turn_control);
		ADD_CASE("/injected-turn", test_injected_turn);
		ADD_CASE("/no-inject", test_no_inject);
		ADD_CASE("/background-agents", test_background_agents);
		ADD_CASE("/oversized-frame", test_oversized_frame);
		ADD_CASE("/bridge-backpressure", test_bridge_backpressure);
		ADD_CASE("/agent-shutdown-active", test_agent_shutdown_active);
		ADD_CASE("/agent-shutdown-cancelled", test_agent_shutdown_cancelled);
#undef ADD_CASE
	}
	g_test_add_func("/mcp/cli/tui/dump", test_tui_dump);
	result = g_test_run();
	remove_fixture(fixture_directory);
	g_free(fixture_directory);
	g_free(test_executable);
	g_free(ai_executable);
	g_free(tui_executable);
	return result;
}
