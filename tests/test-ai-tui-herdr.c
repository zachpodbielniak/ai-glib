/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Real AF_UNIX peers exercise the native reporter, including hostile replies.
 * Application tests spawn ai-tui with isolated HOME/config and a fake provider;
 * no model credentials, installed hooks or running herdr session are used.
 */
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "../bin/ai-tui-herdr.h"
#include "test-server.h"

typedef struct
{
	gchar *directory;
	gchar *path;
	GSocket *listener;
	GThread *thread;
	GAsyncQueue *requests;
	gint stopping;
	const gchar *response;
	gboolean fragmented;
	gboolean silent;
} Peer;

static gchar *tui_binary;

/**
 * peer_run:
 * @data: test socket server
 *
 * Record every complete request before responding. A NULL response selects a
 * correlated success; custom responses exercise protocol errors. Each accepted
 * connection has a one-second failsafe independent of the code under test.
 *
 * Returns: %NULL
 */
static gpointer
peer_run(gpointer data)
{
	Peer *peer = data;

	while (!g_atomic_int_get(&peer->stopping))
	{
		g_autoptr(GSocket) socket = NULL;
		g_autoptr(GSocketConnection) connection = NULL;
		g_autoptr(GDataInputStream) input = NULL;
		g_autoptr(JsonParser) parser = json_parser_new();
		g_autofree gchar *request = NULL;
		g_autofree gchar *response = NULL;
		gsize offset;

		if (!g_socket_condition_timed_wait(peer->listener, G_IO_IN, 20000, NULL, NULL))
			continue;
		socket = g_socket_accept(peer->listener, NULL, NULL);
		if (socket == NULL) continue;
		g_socket_set_blocking(socket, TRUE);
		g_socket_set_timeout(socket, 1);
		connection = g_socket_connection_factory_create_connection(socket);
		input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
		request = g_data_input_stream_read_line(input, NULL, NULL, NULL);
		if (request == NULL) continue;
		g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
		g_async_queue_push(peer->requests, g_strdup(request));
		if (peer->silent)
		{
			gint64 deadline = g_get_monotonic_time() + 400000;

			while (!g_atomic_int_get(&peer->stopping) && g_get_monotonic_time() < deadline)
				g_usleep(1000);
			continue;
		}
		if (peer->response != NULL)
		{
			g_auto(GStrv) pieces = g_strsplit(peer->response, "@ID@", -1);

			response = g_strjoinv(ai_json_get_string(json_node_get_object(
				json_parser_get_root(parser)), "id", ""), pieces);
		}
		else
			response = g_strdup_printf("{\"id\":\"%s\",\"result\":{\"type\":\"ok\"}}\n",
				ai_json_get_string(json_node_get_object(json_parser_get_root(parser)), "id", ""));
		for (offset = 0; offset < strlen(response);)
		{
			gssize sent = g_socket_send(socket, response + offset,
				peer->fragmented ? 1 : strlen(response) - offset, NULL, NULL);

			if (sent <= 0) break;
			offset += (gsize)sent;
			if (peer->fragmented) g_usleep(1000);
		}
	}
	return NULL;
}

/**
 * peer_start:
 * @peer: initialized fixture
 *
 * Bind the same path on restart, testing replacement of a dead server socket.
 */
static void
peer_start(Peer *peer)
{
	g_autoptr(GSocketAddress) address = g_unix_socket_address_new(peer->path);
	g_autoptr(GError) error = NULL;

	g_atomic_int_set(&peer->stopping, FALSE);
	peer->listener = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, 0, &error);
	g_assert_no_error(error);
	g_assert_true(g_socket_bind(peer->listener, address, TRUE, &error));
	g_assert_no_error(error);
	g_assert_true(g_socket_listen(peer->listener, &error));
	g_assert_no_error(error);
	peer->thread = g_thread_new("herdr-test-peer", peer_run, peer);
}

/**
 * peer_new:
 * @response: (nullable): literal response, or automatic correlated success
 * @fragmented: write success one byte at a time
 * @silent: accept but do not reply
 *
 * Returns: an isolated socket fixture
 */
static Peer *
peer_new(const gchar *response, gboolean fragmented, gboolean silent)
{
	Peer *peer = g_new0(Peer, 1);

	peer->directory = g_dir_make_tmp("ai-herdr-XXXXXX", NULL);
	g_assert_nonnull(peer->directory);
	peer->path = g_build_filename(peer->directory, "socket", NULL);
	peer->requests = g_async_queue_new_full(g_free);
	peer->response = response;
	peer->fragmented = fragmented;
	peer->silent = silent;
	peer_start(peer);
	return peer;
}

/**
 * peer_stop:
 * @peer: fixture
 *
 * Stop the serving thread before unlinking the socket.
 */
static void
peer_stop(Peer *peer)
{
	g_atomic_int_set(&peer->stopping, TRUE);
	g_thread_join(peer->thread);
	g_clear_object(&peer->listener);
	g_assert_cmpint(g_unlink(peer->path), ==, 0);
}

/**
 * peer_free:
 * @peer: fixture
 *
 * Remove only files owned by this test after all peer I/O has stopped.
 */
static void
peer_free(Peer *peer)
{
	peer_stop(peer);
	g_async_queue_unref(peer->requests);
	g_assert_cmpint(g_rmdir(peer->directory), ==, 0);
	g_free(peer->directory);
	g_free(peer->path);
	g_free(peer);
}

/**
 * peer_expect:
 * @peer: fixture
 * @state: expected state, or %NULL for release
 *
 * Returns: (transfer full): source identity, allowing ownership assertions
 */
static gchar *
peer_expect(Peer *peer, const gchar *state)
{
	g_autofree gchar *line = g_async_queue_timeout_pop(peer->requests, 7000000);
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	JsonObject *params;

	g_assert_nonnull(line);
	g_assert_true(json_parser_load_from_data(parser, line, -1, NULL));
	root = json_node_get_object(json_parser_get_root(parser));
	params = ai_json_get_object(root, "params");
	g_assert_cmpstr(ai_json_get_string(root, "method", NULL), ==,
		state != NULL ? "pane.report_agent" : "pane.release_agent");
	g_assert_cmpstr(ai_json_get_string(params, "agent", NULL), ==, "ai-tui");
	g_assert_cmpstr(ai_json_get_string(params, "state", NULL), ==, state);
	g_assert_false(json_object_has_member(params, "agent_session_id"));
	g_assert_false(json_object_has_member(params, "agent_session_path"));
	g_assert_false(json_object_has_member(params, "seq"));
	g_assert_true(g_str_has_prefix(ai_json_get_string(params, "source", ""), "custom:ai-tui:"));
	return g_strdup(ai_json_get_string(params, "source", NULL));
}

/**
 * test_detection:
 *
 * Incomplete, malformed and unrelated environments must stay inert.
 */
static void
test_detection(void)
{
	const gchar *bad_markers[] = { NULL, "", "0", "true", " 1", "1 " };
	g_autofree gchar *long_path = g_strnfill(300, 'a');
	g_autofree gchar *long_pane = g_strnfill(257, 'x');
	guint i;

	for (i = 0; i < G_N_ELEMENTS(bad_markers); i++)
		g_assert_null(ai_tui_herdr_new(bad_markers[i], "/tmp/herdr.sock", "w1:p1"));
	g_assert_false(ai_tui_herdr_detect("1", NULL, "w1:p1"));
	g_assert_false(ai_tui_herdr_detect("1", "", "w1:p1"));
	g_assert_false(ai_tui_herdr_detect("1", "relative.sock", "w1:p1"));
	long_path[0] = '/';
	g_assert_false(ai_tui_herdr_detect("1", long_path, "w1:p1"));
	g_assert_false(ai_tui_herdr_detect("1", "/tmp/h.sock", NULL));
	g_assert_false(ai_tui_herdr_detect("1", "/tmp/h.sock", ""));
	g_assert_false(ai_tui_herdr_detect("1", "/tmp/h.sock", long_pane));
	g_assert_false(ai_tui_herdr_detect("1", "/tmp/h.sock", "\xff"));
	g_assert_true(ai_tui_herdr_detect("1", "/tmp/h.sock", "w1:p1"));
	ai_tui_herdr_update(NULL, TRUE, TRUE);
	ai_tui_herdr_free(NULL);
}

/**
 * test_lifecycle:
 *
 * Preserve semantic transitions, de-duplicate reports and release the owner.
 */
static void
test_lifecycle(void)
{
	Peer *peer = peer_new(NULL, FALSE, FALSE);
	AiTuiHerdr *reporter = ai_tui_herdr_new("1", peer->path, "w1:p1");
	const gchar *states[] = { "idle", "working", "blocked", "working", "idle", NULL };
	g_autofree gchar *source = g_strdup(reporter->source);
	guint i;

	ai_tui_herdr_update(reporter, TRUE, FALSE);
	ai_tui_herdr_update(reporter, TRUE, FALSE);
	ai_tui_herdr_update(reporter, TRUE, TRUE);
	ai_tui_herdr_update(reporter, FALSE, TRUE);
	ai_tui_herdr_update(reporter, TRUE, FALSE);
	ai_tui_herdr_update(reporter, FALSE, FALSE);
	ai_tui_herdr_free(reporter);
	for (i = 0; i < G_N_ELEMENTS(states); i++)
	{
		g_autofree gchar *actual_source = peer_expect(peer, states[i]);

		g_assert_cmpstr(actual_source, ==, source);
	}
	g_assert_null(g_async_queue_try_pop(peer->requests));
	reporter = ai_tui_herdr_new("1", peer->path, "w1:p1");
	g_assert_cmpstr(reporter->source, !=, source);
	ai_tui_herdr_free(reporter);
	peer_free(peer);
}

/**
 * test_request:
 * @data: literal adversarial response, or %NULL for fragmented success
 *
 * Validate wire replies without GLib criticals and round-trip escaped pane IDs.
 */
static void
test_request(gconstpointer data)
{
	const gchar *response = data;
	Peer *peer = peer_new(response, response == NULL, FALSE);
	AiTuiHerdr reporter = { 0 };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *request = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	gboolean success;

	reporter.socket_path = peer->path;
	reporter.pane_id = "pane\"\\\n雪";
	reporter.source = "custom:ai-tui:test";
	success = ai_tui_herdr_request(&reporter, "working", &error);
	if (response == NULL)
	{
		g_assert_true(success);
		g_assert_no_error(error);
	}
	else
	{
		g_assert_false(success);
		g_assert_nonnull(error);
	}
	request = g_async_queue_timeout_pop(peer->requests, 1000000);
	g_assert_nonnull(request);
	g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
	g_assert_cmpstr(ai_json_get_string(ai_json_get_object(
		json_node_get_object(json_parser_get_root(parser)), "params"), "pane_id", NULL),
		==, reporter.pane_id);
	peer_free(peer);
}

/**
 * test_deadline:
 *
 * Silent peers and queue floods must not block the caller or delay exit forever.
 */
static void
test_deadline(void)
{
	Peer *peer = peer_new(NULL, FALSE, TRUE);
	AiTuiHerdr *reporter = ai_tui_herdr_new("1", peer->path, "w1:p1");
	gint64 started = g_get_monotonic_time();
	guint i;

	for (i = 0; i < 100000; i++)
		ai_tui_herdr_update(reporter, i % 2, FALSE);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 1000000);
	g_mutex_lock(&reporter->mutex);
	g_assert_cmpuint(g_queue_get_length(&reporter->queue), <=, AI_TUI_HERDR_QUEUE_LIMIT);
	g_mutex_unlock(&reporter->mutex);
	started = g_get_monotonic_time();
	ai_tui_herdr_free(reporter);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 1500000);
	peer_free(peer);
}

/**
 * test_restart:
 *
 * Refresh idle state after server replacement without another UI event.
 */
static void
test_restart(void)
{
	Peer *peer = peer_new(NULL, FALSE, FALSE);
	AiTuiHerdr *reporter = ai_tui_herdr_new("1", peer->path, "w1:p1");
	g_autofree gchar *source = peer_expect(peer, "idle");
	g_autofree gchar *refreshed = NULL;

	peer_stop(peer);
	peer_start(peer);
	refreshed = peer_expect(peer, "idle");
	g_assert_cmpstr(refreshed, ==, source);
	ai_tui_herdr_free(reporter);
	peer_free(peer);
}

/**
 * test_unavailable:
 *
 * Missing sockets and regular files fail without changing filesystem contents.
 */
static void
test_unavailable(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-herdr-missing-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "socket", NULL);
	g_autofree gchar *contents = NULL;
	guint i;

	for (i = 0; i < 2; i++)
	{
		g_autoptr(AiTuiHerdr) reporter = ai_tui_herdr_new("1", path, "w1:p1");

		g_assert_nonnull(reporter);
		ai_tui_herdr_update(reporter, TRUE, FALSE);
		/* Join before creating the file: the first iteration must actually
		 * encounter ENOENT instead of racing with regular-file creation. */
		g_clear_pointer(&reporter, ai_tui_herdr_free);
		if (i == 0) g_assert_true(g_file_set_contents(path, "untouched", -1, NULL));
	}
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_cmpstr(contents, ==, "untouched");
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_cmpint(g_rmdir(directory), ==, 0);
}

/**
 * launch_tui:
 * @peer: fake herdr
 * @extra: additional flags and prompt
 * @master: optional PTY master output; NULL selects dump mode
 * @http: optional loopback HTTP provider instead of the stub CLI
 *
 * Returns: isolated ai-tui subprocess; the stub verifies hook suppression
 */
static GSubprocess *
launch_tui(Peer *peer, const gchar * const *extra, gint *master, TServer *http)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		master == NULL ? G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE : 0);
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *stub = g_build_filename(peer->directory, "grok", NULL);
	g_autofree gchar *executable = g_strconcat("executable-path=", stub, NULL);
	const gchar *script = "#!/bin/bash\n"
		"expected_env=0\nexpected_pane=''\n"
		"if [[ ${AI_HERDR_TEST_OPT_OUT:-} == 1 ]]\n"
		"then\n expected_env=1\n expected_pane=w1:p1\nfi\n"
		"if [[ ${HERDR_ENV:-} != \"$expected_env\" || ${HERDR_PANE_ID:-} != \"$expected_pane\" ]]\n"
		"then\n echo 'child hook context leaked' >&2\n exit 1\nfi\n"
		"printf '%s\\n' '{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"herdr test reply\"}}}'\n"
		"printf '%s\\n' '{\"type\":\"result\",\"result\":\"herdr test reply\",\"session_id\":\"test\"}'\n";
	guint i;
	GSubprocess *process;

	g_assert_true(g_file_set_contents(stub, script, -1, NULL));
	g_assert_cmpint(g_chmod(stub, 0700), ==, 0);
	g_subprocess_launcher_set_cwd(launcher, peer->directory);
	g_subprocess_launcher_setenv(launcher, "HOME", peer->directory, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", peer->directory, TRUE);
	g_subprocess_launcher_setenv(launcher, "HERDR_ENV", "1", TRUE);
	g_subprocess_launcher_setenv(launcher, "HERDR_SOCKET_PATH", peer->path, TRUE);
	g_subprocess_launcher_setenv(launcher, "HERDR_PANE_ID", "w1:p1", TRUE);
	g_subprocess_launcher_setenv(launcher, "TERM", "xterm-256color", TRUE);
	g_subprocess_launcher_setenv(launcher, "AI_HERDR_TEST_OPT_OUT", "0", TRUE);
	g_subprocess_launcher_unsetenv(launcher, "ANTHROPIC_API_KEY");
	g_subprocess_launcher_unsetenv(launcher, "OPENAI_API_KEY");
	g_ptr_array_add(argv, tui_binary);
	g_ptr_array_add(argv, "-p");
	if (http == NULL)
	{
		g_ptr_array_add(argv, "grok-build");
		g_ptr_array_add(argv, "--set");
		g_ptr_array_add(argv, executable);
	}
	else
	{
		g_subprocess_launcher_setenv(launcher, "OPENAI_API_KEY", "test-only", TRUE);
		g_subprocess_launcher_setenv(launcher, "OPENAI_BASE_URL", http->base_url, TRUE);
		g_ptr_array_add(argv, "openai");
		g_ptr_array_add(argv, "--no-stream");
		g_ptr_array_add(argv, "--local-tools");
	}
	g_ptr_array_add(argv, "--no-agents");
	g_ptr_array_add(argv, "--no-animation");
	for (i = 0; extra[i] != NULL; i++)
	{
		if (g_str_equal(extra[i], "--no-herdr"))
			g_subprocess_launcher_setenv(launcher, "AI_HERDR_TEST_OPT_OUT", "1", TRUE);
		g_ptr_array_add(argv, (gpointer)extra[i]);
	}
	g_ptr_array_add(argv, NULL);
	if (master != NULL)
	{
		gint slave;
		struct winsize size = { 24, 100, 0, 0 };

		g_assert_cmpint(openpty(master, &slave, NULL, NULL, &size), ==, 0);
		g_assert_cmpint(fcntl(*master, F_SETFL, O_NONBLOCK), ==, 0);
		g_subprocess_launcher_take_stdin_fd(launcher, dup(slave));
		g_subprocess_launcher_take_stdout_fd(launcher, dup(slave));
		g_subprocess_launcher_take_stderr_fd(launcher, slave);
	}
	process = g_subprocess_launcher_spawnv(launcher, (const gchar * const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_assert_nonnull(process);
	return process;
}

/**
 * test_application_dump:
 *
 * The real binary publishes each stage without a native CLI resume identity.
 */
static void
test_application_dump(void)
{
	Peer *peer = peer_new(NULL, FALSE, FALSE);
	const gchar *args[] = { "--dump", "hello", NULL };
	g_autoptr(GSubprocess) process = launch_tui(peer, args, NULL, NULL);
	g_autofree gchar *output = NULL;
	g_autofree gchar *errors = NULL;
	g_autofree gchar *stub = g_build_filename(peer->directory, "grok", NULL);
	const gchar *states[] = { "idle", "working", "idle", NULL };
	guint i;

	g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &output, &errors, NULL));
	g_assert_true(g_subprocess_get_successful(process));
	g_test_message("dump stdout: %s; stderr: %s", output, errors);
	g_assert_nonnull(strstr(output, "herdr test reply"));
	g_assert_cmpstr(errors, ==, "");
	for (i = 0; i < G_N_ELEMENTS(states); i++)
	{
		g_autofree gchar *source = peer_expect(peer, states[i]);
	}
	g_assert_cmpint(g_unlink(stub), ==, 0);
	peer_free(peer);
}

/**
 * test_application_inert:
 * @data: informational, native-launch or opt-out option
 *
 * These modes must send no reports. Opt-out also preserves child hook context.
 */
static void
test_application_inert(gconstpointer data)
{
	Peer *peer = peer_new(NULL, FALSE, FALSE);
	const gchar *args[] = { data, NULL };
	const gchar *opt_out_args[] = { "--no-herdr", "--dump", "hello", NULL };
	gboolean opt_out = g_str_equal(data, "--no-herdr");
	g_autoptr(GSubprocess) process = launch_tui(peer, opt_out ? opt_out_args : args, NULL, NULL);
	g_autofree gchar *stub = g_build_filename(peer->directory, "grok", NULL);
	g_autofree gchar *output = NULL;

	g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &output, NULL, NULL));
	g_assert_true(g_subprocess_get_successful(process));
	if (opt_out) g_assert_nonnull(strstr(output, "herdr test reply"));
	g_assert_null(g_async_queue_try_pop(peer->requests));
	g_assert_cmpint(g_unlink(stub), ==, 0);
	peer_free(peer);
}

/**
 * test_application_terminal:
 *
 * Exercise startup, turn completion and SIGTERM release on a real PTY.
 */
static void
test_application_terminal(void)
{
	Peer *peer = peer_new(NULL, FALSE, FALSE);
	const gchar *args[] = { "hello", NULL };
	gint master;
	g_autoptr(GSubprocess) process = launch_tui(peer, args, &master, NULL);
	g_autofree gchar *stub = g_build_filename(peer->directory, "grok", NULL);
	const gchar *states[] = { "idle", "working", "idle" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(states); i++)
	{
		g_autofree gchar *source = peer_expect(peer, states[i]);
		gchar buffer[8192];

		while (read(master, buffer, sizeof(buffer)) > 0) { }
	}
	g_subprocess_send_signal(process, SIGTERM);
	{
		g_autofree gchar *source = peer_expect(peer, NULL);
	}
	g_assert_true(g_subprocess_wait_check(process, NULL, NULL));
	close(master);
	g_assert_cmpint(g_unlink(stub), ==, 0);
	peer_free(peer);
}

/**
 * test_application_approval:
 * @data: deliberate approval key, or "terminate" for shutdown in the modal loop
 *
 * A real loopback provider asks for a harmless bash tool. The fixture waits for
 * blocked before changing its next response and answering, proving the actual
 * approval handler (including denial and cancellation) reports both edges.
 */
static void
test_application_approval(gconstpointer data)
{
	Peer *peer = peer_new(NULL, FALSE, FALSE);
	TServer *http = tserver_new();
	const gchar *answer = data;
	const gchar *args[] = { "hello", NULL };
	g_autoptr(GSubprocess) process = NULL;
	g_autofree gchar *stub = g_build_filename(peer->directory, "grok", NULL);
	const gchar *states[] = { "idle", "working", "blocked" };
	gint master;
	guint i;

	tserver_set_response(http, 200,
		"{\"id\":\"test\",\"choices\":[{\"message\":{\"role\":\"assistant\","
		"\"tool_calls\":[{\"id\":\"call1\",\"type\":\"function\",\"function\":{"
		"\"name\":\"bash\",\"arguments\":\"{\\\"command\\\":\\\"printf approved\\\"}\"}}]},"
		"\"finish_reason\":\"tool_calls\"}]}");
	process = launch_tui(peer, args, &master, http);
	for (i = 0; i < G_N_ELEMENTS(states); i++)
	{
		g_autofree gchar *source = peer_expect(peer, states[i]);
		gchar buffer[8192];

		while (read(master, buffer, sizeof(buffer)) > 0) { }
	}
	tserver_set_response(http, 200,
		"{\"id\":\"test\",\"choices\":[{\"message\":{\"role\":\"assistant\","
		"\"content\":\"finished\"},\"finish_reason\":\"stop\"}]}");
	if (g_str_equal(answer, "terminate"))
		g_subprocess_send_signal(process, SIGTERM);
	else
	{
		g_autofree gchar *working = NULL;
		g_autofree gchar *idle = NULL;

		g_assert_cmpint(write(master, answer, strlen(answer)), ==, (gssize)strlen(answer));
		working = peer_expect(peer, "working");
		idle = peer_expect(peer, "idle");
		g_subprocess_send_signal(process, SIGTERM);
	}
	/* Terminating in the modal handler may publish working/idle as it unwinds;
	 * the final request must release the owner, never leave blocked behind. */
	for (i = 0; i < 4; i++)
	{
		g_autofree gchar *line = g_async_queue_timeout_pop(peer->requests, 3000000);
		g_autoptr(JsonParser) parser = json_parser_new();

		g_assert_nonnull(line);
		g_assert_true(json_parser_load_from_data(parser, line, -1, NULL));
		if (g_strcmp0(ai_json_get_string(json_node_get_object(json_parser_get_root(parser)),
			"method", NULL), "pane.release_agent") == 0) break;
	}
	g_assert_cmpuint(i, <, 4);
	g_assert_true(g_subprocess_wait_check(process, NULL, NULL));
	close(master);
	tserver_free(http);
	g_assert_cmpint(g_unlink(stub), ==, 0);
	peer_free(peer);
}

/**
 * test_application_failure:
 * @data: whether to cancel a slow request instead of returning an HTTP error
 *
 * Both error and cancellation must return to idle, allowing the next prompt.
 */
static void
test_application_failure(gconstpointer data)
{
	Peer *peer = peer_new(NULL, FALSE, FALSE);
	TServer *http = tserver_new();
	const gchar *args[] = { "hello", NULL };
	g_autoptr(GSubprocess) process = NULL;
	g_autofree gchar *stub = g_build_filename(peer->directory, "grok", NULL);
	g_autofree gchar *source = NULL;
	gint master;

	tserver_set_response(http, 400, "{\"error\":{\"message\":\"test rejection\"}}");
	if (data != NULL) tserver_set_delay(http, 1000);
	process = launch_tui(peer, args, &master, http);
	source = peer_expect(peer, "idle");
	g_free(g_steal_pointer(&source));
	source = peer_expect(peer, "working");
	if (data != NULL) g_subprocess_send_signal(process, SIGINT);
	g_free(g_steal_pointer(&source));
	source = peer_expect(peer, "idle");
	g_subprocess_send_signal(process, SIGTERM);
	g_free(g_steal_pointer(&source));
	source = peer_expect(peer, NULL);
	g_assert_true(g_subprocess_wait_check(process, NULL, NULL));
	close(master);
	tserver_free(http);
	g_assert_cmpint(g_unlink(stub), ==, 0);
	peer_free(peer);
}

/**
 * test_application_unavailable:
 *
 * ncurses is optional in ai-glib. Keep socket coverage on builds that omit
 * ai-tui, while explicitly reporting that application coverage was skipped.
 */
static void
test_application_unavailable(void)
{
	g_test_skip("ai-tui is not built; install ncurses-devel to exercise the application");
}

/**
 * main:
 * @argc: argument count
 * @argv: command-line arguments
 *
 * Register independent failure modes with visible GTest names.
 *
 * Returns: the GTest exit status
 */
int
main(int argc, char **argv)
{
	g_autofree gchar *directory = g_path_get_dirname(argv[0]);
	g_autofree gchar *relative = g_build_filename(directory, "..", "bin", "ai-tui", NULL);
	g_autofree gchar *oversized = g_strnfill(AI_TUI_HERDR_REPLY_LIMIT + 1, ' ');
	gint result;

	g_test_init(&argc, &argv, NULL);
	tui_binary = g_canonicalize_filename(relative, NULL);
	g_test_add_func("/herdr/detection", test_detection);
	g_test_add_func("/herdr/lifecycle", test_lifecycle);
	g_test_add_data_func("/herdr/protocol/fragmented-success", NULL, test_request);
	g_test_add_data_func("/herdr/protocol/empty", "", test_request);
	g_test_add_data_func("/herdr/protocol/malformed", "not JSON\n", test_request);
	g_test_add_data_func("/herdr/protocol/non-object", "[]\n", test_request);
	g_test_add_data_func("/herdr/protocol/null", "null\n", test_request);
	g_test_add_data_func("/herdr/protocol/wrong-id", "{\"id\":\"wrong\",\"result\":{\"type\":\"ok\"}}\n", test_request);
	g_test_add_data_func("/herdr/protocol/error", "{\"error\":{\"code\":\"pane_not_found\"}}\n", test_request);
	g_test_add_data_func("/herdr/protocol/error-with-result", "{\"id\":\"@ID@\",\"result\":{\"type\":\"ok\"},\"error\":{\"code\":\"test\"}}\n", test_request);
	g_test_add_data_func("/herdr/protocol/unknown-result", "{\"id\":\"@ID@\",\"result\":{\"type\":\"future\"}}\n", test_request);
	g_test_add_data_func("/herdr/protocol/null-result", "{\"id\":\"@ID@\",\"result\":null}\n", test_request);
	g_test_add_data_func("/herdr/protocol/wrong-result-type", "{\"id\":\"@ID@\",\"result\":{\"type\":[]}}\n", test_request);
	g_test_add_data_func("/herdr/protocol/wrong-types", "{\"id\":[],\"result\":7}\n", test_request);
	g_test_add_data_func("/herdr/protocol/truncated", "{\"result\":", test_request);
	g_test_add_data_func("/herdr/protocol/oversized", oversized, test_request);
	g_test_add_func("/herdr/deadline-and-overload", test_deadline);
	g_test_add_func("/herdr/restart-refresh", test_restart);
	g_test_add_func("/herdr/unavailable", test_unavailable);
	if (g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_add_func("/herdr/application/dump", test_application_dump);
		g_test_add_func("/herdr/application/terminal", test_application_terminal);
		g_test_add_data_func("/herdr/application/approval-allow", "y", test_application_approval);
		g_test_add_data_func("/herdr/application/approval-deny", "n", test_application_approval);
		g_test_add_data_func("/herdr/application/approval-deny-all", "d", test_application_approval);
		g_test_add_data_func("/herdr/application/approval-escape", "\033", test_application_approval);
		g_test_add_data_func("/herdr/application/approval-terminate", "terminate", test_application_approval);
		g_test_add_data_func("/herdr/application/provider-error", NULL, test_application_failure);
		g_test_add_data_func("/herdr/application/cancel", "cancel", test_application_failure);
		g_test_add_data_func("/herdr/application/help", "--help", test_application_inert);
		g_test_add_data_func("/herdr/application/version", "--version", test_application_inert);
		g_test_add_data_func("/herdr/application/license", "--license", test_application_inert);
		g_test_add_data_func("/herdr/application/themes", "--list-themes", test_application_inert);
		g_test_add_data_func("/herdr/application/dry-run", "--dry-run", test_application_inert);
		g_test_add_data_func("/herdr/application/native-launch-print", "--launch-cmd-print", test_application_inert);
		g_test_add_data_func("/herdr/application/opt-out", "--no-herdr", test_application_inert);
	}
	else
		g_test_add_func("/herdr/application/unavailable", test_application_unavailable);
	result = g_test_run();
	g_free(tui_binary);
	return result;
}
