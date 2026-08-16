/*
 * test-cli-stream-reader.c - Tests for the shared CLI streaming reader
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * ai_cli_client_stream_run_async() replaced three near-verbatim copies of
 * the same NDJSON read loop, one in each of the claude-code, opencode and
 * grok-build clients. The copies had drifted -- only one of them failed the
 * task on a mid-stream parse error, and none enforced the process deadline
 * the synchronous path has always had -- so these tests pin the behaviour
 * that is now shared by all of them.
 *
 * The subject is a purpose-built AiCliClient subclass rather than a real
 * provider, so the reader is exercised without also exercising anybody's
 * wire format. Per-provider translation is covered in test-cli-events.c.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "core/ai-cli-client.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "core/ai-event-source.h"
#include "core/ai-streamable.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

/* ----------------------------------------------------------------
 * Stub harness -- a shell script driven by files in its own directory
 * ---------------------------------------------------------------- */

typedef struct
{
	gchar *dir;
	gchar *stub;
} Stub;

#define STUB_TEMPLATE                                                        \
	"#!/bin/sh\n"                                                        \
	"d='%s'\n"                                                           \
	"printf '%%s\\n' \"$*\" >> \"$d/argv.log\"\n"                        \
	"pwd -P >> \"$d/cwd.log\"\n"                                       \
	"if [ -f \"$d/wants_stdin\" ]; then cat >> \"$d/stdin.log\"; fi\n"    \
	"if [ -f \"$d/sleep\" ]; then sleep \"$(cat \"$d/sleep\")\"; fi\n"    \
	"if [ -f \"$d/stderr\" ]; then cat \"$d/stderr\" >&2; fi\n"          \
	"if [ -f \"$d/stdout\" ]; then cat \"$d/stdout\"; fi\n"              \
	"if [ -f \"$d/raw\" ]; then printf '%%s' \"$(cat \"$d/raw\")\"; fi\n" \
	"exit \"$(cat \"$d/exit\" 2>/dev/null || echo 0)\"\n"

static Stub *
stub_new(void)
{
	Stub *stub = g_new0(Stub, 1);
	g_autofree gchar *script = NULL;
	g_autoptr(GError) error = NULL;

	stub->dir = g_dir_make_tmp("ai-glib-stream-XXXXXX", &error);
	g_assert_no_error(error);

	stub->stub = g_build_filename(stub->dir, "cli", NULL);
	script = g_strdup_printf(STUB_TEMPLATE, stub->dir);

	g_file_set_contents(stub->stub, script, -1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(stub->stub, 0700), ==, 0);

	return stub;
}

static void
stub_set(Stub *stub, const gchar *name, const gchar *contents)
{
	g_autofree gchar *path = g_build_filename(stub->dir, name, NULL);
	g_autoptr(GError) error = NULL;

	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);
}

static gchar *
stub_read(Stub *stub, const gchar *name)
{
	g_autofree gchar *path = g_build_filename(stub->dir, name, NULL);
	gchar *contents = NULL;

	if (!g_file_get_contents(path, &contents, NULL, NULL))
		return NULL;

	return contents;
}

static void
stub_free(Stub *stub)
{
	const gchar *names[] = {
		"cli", "argv.log", "cwd.log", "stdin.log", "wants_stdin",
		"sleep", "stderr", "stdout", "raw", "exit", NULL
	};
	gsize i;

	for (i = 0; names[i] != NULL; i++)
	{
		g_autofree gchar *path = g_build_filename(stub->dir, names[i], NULL);
		g_remove(path);
	}

	g_rmdir(stub->dir);
	g_free(stub->dir);
	g_free(stub->stub);
	g_free(stub);
}

/* ----------------------------------------------------------------
 * A minimal AiCliClient whose parser the test controls
 * ---------------------------------------------------------------- */

#define TEST_TYPE_CLI (test_cli_get_type())
G_DECLARE_FINAL_TYPE(TestCli, test_cli, TEST, CLI, AiCliClient)

struct _TestCli
{
	AiCliClient parent_instance;

	gchar   *stdin_data;      /* what build_stdin hands back, or NULL */
	gboolean fail_on_bang;    /* treat a line starting with '!' as an error */
	gboolean use_legacy;      /* exercise the parse_stream_line fallback */
	GPtrArray *argv_seen;     /* the argv build_argv produced */
};

static void test_cli_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(TestCli, test_cli, AI_TYPE_CLI_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              test_cli_streamable_init))

/*
 * Each line is one directive:
 *   t:<text>   a text delta
 *   u:<id>     a tool call starting
 *   s:<text>   a status
 *   !<msg>     a parse failure, when fail_on_bang is set
 * Anything else is ignored, the way a real parser ignores lines it does
 * not recognise.
 */
static gboolean
test_cli_parse_stream_events(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	GPtrArray    *out_events,
	GError      **error)
{
	TestCli *self = TEST_CLI(client);

	(void)response;

	if (line == NULL || line[0] == '\0')
		return TRUE;

	if (line[0] == '!' && self->fail_on_bang)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		            "stub reported: %s", line + 1);
		return FALSE;
	}

	if (g_str_has_prefix(line, "t:"))
	{
		g_ptr_array_add(out_events, ai_event_new_text_delta(line + 2));
	}
	else if (g_str_has_prefix(line, "u:"))
	{
		g_autoptr(AiToolUse) tu =
			ai_tool_use_new_from_json_string(line + 2, "bash", "{}");
		g_ptr_array_add(out_events, ai_event_new_tool_started(tu));
	}
	else if (g_str_has_prefix(line, "s:"))
	{
		g_ptr_array_add(out_events, ai_event_new_status(line + 2));
	}

	return TRUE;
}

/* The pre-event contract, so the compatibility fallback can be tested. */
static gboolean
test_cli_parse_stream_line(
	AiCliClient  *client,
	const gchar  *line,
	AiResponse   *response,
	gchar       **delta_text,
	GError      **error)
{
	TestCli *self = TEST_CLI(client);

	(void)response;
	*delta_text = NULL;

	if (line != NULL && line[0] == '!' && self->fail_on_bang)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
		            "stub reported: %s", line + 1);
		return FALSE;
	}

	if (line != NULL && g_str_has_prefix(line, "t:"))
		*delta_text = g_strdup(line + 2);

	return TRUE;
}

static gchar **
test_cli_build_argv(
	AiCliClient *client,
	GList       *messages,
	const gchar *system_prompt,
	gint         max_tokens,
	gboolean     streaming)
{
	TestCli *self = TEST_CLI(client);
	GPtrArray *args = g_ptr_array_new();

	(void)messages;
	(void)system_prompt;
	(void)max_tokens;

	g_ptr_array_add(args, g_strdup("placeholder"));
	g_ptr_array_add(args, g_strdup(streaming ? "--stream" : "--once"));
	g_ptr_array_add(args, NULL);

	if (self->argv_seen != NULL)
		g_ptr_array_unref(self->argv_seen);
	self->argv_seen = NULL;

	return (gchar **)g_ptr_array_free(args, FALSE);
}

static gchar *
test_cli_build_stdin(AiCliClient *client, GList *messages)
{
	TestCli *self = TEST_CLI(client);

	(void)messages;

	return g_strdup(self->stdin_data);
}

static void
test_cli_chat_stream_async(
	AiStreamable        *streamable,
	GList               *messages,
	const gchar         *system_prompt,
	gint                 max_tokens,
	GList               *tools,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data)
{
	(void)tools;

	ai_cli_client_stream_run_async(AI_CLI_CLIENT(streamable), messages,
	                               system_prompt, max_tokens,
	                               cancellable, callback, user_data);
}

static AiResponse *
test_cli_chat_stream_finish(
	AiStreamable  *streamable,
	GAsyncResult  *result,
	GError       **error)
{
	return ai_cli_client_stream_run_finish(AI_CLI_CLIENT(streamable),
	                                       result, error);
}

static void
test_cli_streamable_init(AiStreamableInterface *iface)
{
	iface->chat_stream_async = test_cli_chat_stream_async;
	iface->chat_stream_finish = test_cli_chat_stream_finish;
}

static void
test_cli_finalize(GObject *object)
{
	TestCli *self = TEST_CLI(object);

	g_clear_pointer(&self->stdin_data, g_free);
	g_clear_pointer(&self->argv_seen, g_ptr_array_unref);

	G_OBJECT_CLASS(test_cli_parent_class)->finalize(object);
}

static void
test_cli_class_init(TestCliClass *klass)
{
	AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

	G_OBJECT_CLASS(klass)->finalize = test_cli_finalize;

	cli_class->build_argv = test_cli_build_argv;
	cli_class->build_stdin = test_cli_build_stdin;
	cli_class->parse_stream_events = test_cli_parse_stream_events;
	cli_class->parse_stream_line = test_cli_parse_stream_line;
}

static void
test_cli_init(TestCli *self)
{
	self->stdin_data = NULL;
	self->fail_on_bang = TRUE;
	self->use_legacy = FALSE;
}

/* ----------------------------------------------------------------
 * Run harness
 * ---------------------------------------------------------------- */

typedef struct
{
	GMainLoop  *loop;
	AiResponse *response;
	GError     *error;

	GPtrArray  *events;      /* every AiEvent seen on ::event */
	GString    *deltas;      /* every ::delta concatenated */
	guint       starts;      /* ::stream-start count */
	guint       ends;        /* ::stream-end count */
	guint       tool_uses;   /* ::tool-use count */
} Run;

static Run *
run_new(void)
{
	Run *run = g_new0(Run, 1);

	run->loop = g_main_loop_new(NULL, FALSE);
	run->events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	run->deltas = g_string_new("");

	return run;
}

static void
run_free(Run *run)
{
	g_main_loop_unref(run->loop);
	g_ptr_array_unref(run->events);
	g_string_free(run->deltas, TRUE);
	g_clear_object(&run->response);
	g_clear_error(&run->error);
	g_free(run);
}

static void on_event(AiEventSource *s, AiEvent *e, gpointer d)
{ (void)s; g_ptr_array_add(((Run *)d)->events, ai_event_ref(e)); }

static void on_delta(GObject *s, const gchar *text, gpointer d)
{ (void)s; g_string_append(((Run *)d)->deltas, text); }

static void on_start(GObject *s, gpointer d)
{ (void)s; ((Run *)d)->starts++; }

static void on_end(GObject *s, GObject *resp, gpointer d)
{ (void)s; (void)resp; ((Run *)d)->ends++; }

static void on_tool_use(GObject *s, GObject *tu, gpointer d)
{ (void)s; (void)tu; ((Run *)d)->tool_uses++; }

static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Run *run = user_data;

	run->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
	                                                 result, &run->error);
	g_main_loop_quit(run->loop);
}

/* Build a client wired to @stub, run one streaming turn, collect everything. */
static Run *
stream_once(TestCli *client, GCancellable *cancellable)
{
	Run *run = run_new();
	g_autoptr(AiMessage) msg = ai_message_new_user("go");
	GList *messages = g_list_append(NULL, msg);

	g_signal_connect(client, "event", G_CALLBACK(on_event), run);
	g_signal_connect(client, "delta", G_CALLBACK(on_delta), run);
	g_signal_connect(client, "stream-start", G_CALLBACK(on_start), run);
	g_signal_connect(client, "stream-end", G_CALLBACK(on_end), run);
	g_signal_connect(client, "tool-use", G_CALLBACK(on_tool_use), run);

	ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages, NULL,
	                                1024, NULL, cancellable,
	                                on_stream_done, run);
	g_main_loop_run(run->loop);

	g_list_free(messages);
	g_signal_handlers_disconnect_by_data(client, run);

	return run;
}

static TestCli *
client_for(Stub *stub)
{
	TestCli *client = g_object_new(TEST_TYPE_CLI, NULL);

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), stub->stub);
	ai_cli_client_set_model(AI_CLI_CLIENT(client), "stub-model");

	return client;
}

/* Count events of one kind. */
static guint
count_kind(Run *run, AiEventKind kind)
{
	guint i, n = 0;

	for (i = 0; i < run->events->len; i++)
	{
		if (ai_event_get_kind(g_ptr_array_index(run->events, i)) == kind)
			n++;
	}

	return n;
}

/* ----------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------- */

static void
test_basic_stream(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *text = NULL;

	stub_set(stub, "stdout", "t:Hello \nt:world\n");
	client = client_for(stub);

	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	g_assert_nonnull(run->response);

	/* Deltas are accumulated into the response when the parser adds none. */
	text = ai_response_get_text(run->response);
	g_assert_cmpstr(text, ==, "Hello world");

	/* Both signal families fire, from one place. */
	g_assert_cmpstr(run->deltas->str, ==, "Hello world");
	g_assert_cmpuint(count_kind(run, AI_EVENT_TEXT_DELTA), ==, 2);
	g_assert_cmpuint(count_kind(run, AI_EVENT_STREAM_END), ==, 1);

	/* stream-start fires exactly once, on the first delta. */
	g_assert_cmpuint(run->starts, ==, 1);
	g_assert_cmpuint(run->ends, ==, 1);

	run_free(run);
	stub_free(stub);
}

static void
test_streaming_flag_reaches_argv(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *argv = NULL;

	stub_set(stub, "stdout", "t:hi\n");
	client = client_for(stub);

	run = stream_once(client, NULL);
	g_assert_no_error(run->error);

	/* build_argv is called with streaming=TRUE, not the sync default. */
	argv = stub_read(stub, "argv.log");
	g_assert_nonnull(argv);
	g_assert_true(g_str_has_prefix(argv, "--stream"));

	run_free(run);
	stub_free(stub);
}

static void
test_tool_use_signal_is_emitted(void)
{
	/*
	 * ::tool-use was declared on both AiCliClient and AiStreamable and
	 * emitted by nothing at all. This is the test that it now fires.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;

	stub_set(stub, "stdout", "t:running\nu:toolu_01\nt:done\n");
	client = client_for(stub);

	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	g_assert_cmpuint(run->tool_uses, ==, 1);
	g_assert_cmpuint(count_kind(run, AI_EVENT_TOOL_STARTED), ==, 1);

	run_free(run);
	stub_free(stub);
}

static void
test_event_order_preserved(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;

	stub_set(stub, "stdout", "t:a\nu:toolu_01\ns:thinking\nt:b\n");
	client = client_for(stub);

	run = stream_once(client, NULL);
	g_assert_no_error(run->error);

	/* Four parsed events plus the synthesised stream end. */
	g_assert_cmpuint(run->events->len, ==, 5);
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(run->events, 0)), ==,
	                AI_EVENT_TEXT_DELTA);
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(run->events, 1)), ==,
	                AI_EVENT_TOOL_STARTED);
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(run->events, 2)), ==,
	                AI_EVENT_STATUS);
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(run->events, 3)), ==,
	                AI_EVENT_TEXT_DELTA);
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(run->events, 4)), ==,
	                AI_EVENT_STREAM_END);

	run_free(run);
	stub_free(stub);
}

static void
test_events_are_labelled(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;

	stub_set(stub, "stdout", "t:x\n");
	client = client_for(stub);

	run = stream_once(client, NULL);
	g_assert_no_error(run->error);

	g_assert_cmpstr(ai_event_get_source(g_ptr_array_index(run->events, 0)),
	                ==, "TestCli");

	run_free(run);
	stub_free(stub);
}

static void
test_empty_output(void)
{
	/*
	 * A child that says nothing at all still completes: EOF with a response
	 * object is success with empty text, not an error.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *text = NULL;

	client = client_for(stub);
	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	g_assert_nonnull(run->response);

	/* No content blocks at all, which ai_response_get_text reports as NULL. */
	text = ai_response_get_text(run->response);
	g_assert_null(text);

	/* Nothing was streamed, so stream-start never fires. */
	g_assert_cmpuint(run->starts, ==, 0);
	g_assert_cmpuint(run->ends, ==, 1);

	run_free(run);
	stub_free(stub);
}

static void
test_blank_and_unknown_lines(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *text = NULL;

	stub_set(stub, "stdout", "\nnoise\n\nt:kept\n   \n");
	client = client_for(stub);

	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	text = ai_response_get_text(run->response);
	g_assert_cmpstr(text, ==, "kept");

	run_free(run);
	stub_free(stub);
}

static void
test_final_line_without_newline(void)
{
	/*
	 * A CLI killed mid-flush, or one that simply does not terminate its
	 * last line, must not have that line dropped.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *text = NULL;

	stub_set(stub, "raw", "t:trailing");
	client = client_for(stub);

	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	text = ai_response_get_text(run->response);
	g_assert_cmpstr(text, ==, "trailing");

	run_free(run);
	stub_free(stub);
}

static void
test_very_long_line(void)
{
	/*
	 * GDataInputStream buffers 4 KiB by default and doubles as it scans for
	 * the newline. A single assistant message with a large content array
	 * routinely exceeds that, and truncating one would corrupt a turn
	 * without failing it.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	g_autoptr(GString) payload = g_string_new("t:");
	Run *run;
	g_autofree gchar *text = NULL;
	gsize i;

	for (i = 0; i < 100000; i++)
		g_string_append_c(payload, 'x');
	g_string_append_c(payload, '\n');

	stub_set(stub, "stdout", payload->str);
	client = client_for(stub);

	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	text = ai_response_get_text(run->response);
	g_assert_cmpuint(strlen(text), ==, 100000);

	run_free(run);
	stub_free(stub);
}

static void
test_parse_error_fails_the_task(void)
{
	/*
	 * Before the hoist only grok-build did this; claude-code and opencode
	 * read on to EOF and handed back an empty response, so a CLI that
	 * reported an error mid-stream looked like a successful empty answer.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;

	stub_set(stub, "stdout", "t:partial\n!model not found\nt:never read\n");
	client = client_for(stub);

	run = stream_once(client, NULL);

	g_assert_error(run->error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_null(run->response);
	g_assert_true(strstr(run->error->message, "model not found") != NULL);

	/* The turn failed, so no stream-end is claimed. */
	g_assert_cmpuint(run->ends, ==, 0);

	run_free(run);
	stub_free(stub);
}

static void
test_stdin_is_written_and_closed(void)
{
	/*
	 * A CLI reading its prompt back through --prompt-file /dev/stdin needs
	 * the EOF; without the close it waits forever for more input.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *seen = NULL;

	stub_set(stub, "wants_stdin", "1");
	stub_set(stub, "stdout", "t:ok\n");

	client = client_for(stub);
	client->stdin_data = g_strdup("the whole prompt");

	run = stream_once(client, NULL);
	g_assert_no_error(run->error);

	seen = stub_read(stub, "stdin.log");
	g_assert_cmpstr(seen, ==, "the whole prompt");

	run_free(run);
	stub_free(stub);
}

static void
test_no_stdin_when_build_stdin_returns_null(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *seen = NULL;

	stub_set(stub, "stdout", "t:ok\n");

	client = client_for(stub);
	client->stdin_data = NULL;   /* build_stdin hands back NULL */

	run = stream_once(client, NULL);
	g_assert_no_error(run->error);

	seen = stub_read(stub, "stdin.log");
	g_assert_null(seen);

	run_free(run);
	stub_free(stub);
}

static void
test_nonzero_exit_after_valid_events(void)
{
	/*
	 * The stream is the source of truth. A child that produced a complete
	 * answer and then exited nonzero has still answered -- grok's own CLI
	 * exits 0 on some errors and nonzero on others, so the exit status
	 * cannot be the signal either way.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *text = NULL;

	stub_set(stub, "stdout", "t:answered anyway\n");
	stub_set(stub, "exit", "3");

	client = client_for(stub);
	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	text = ai_response_get_text(run->response);
	g_assert_cmpstr(text, ==, "answered anyway");

	run_free(run);
	stub_free(stub);
}

static void
test_stderr_only_with_nonzero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;
	g_autofree gchar *text = NULL;

	stub_set(stub, "stderr", "something went wrong\n");
	stub_set(stub, "exit", "1");

	client = client_for(stub);
	run = stream_once(client, NULL);

	/*
	 * Nothing arrived on stdout, so the reader completes with an empty
	 * response rather than inventing an error it cannot describe. The
	 * caller sees an empty answer; the synchronous path is the one that
	 * reports exit status.
	 */
	g_assert_no_error(run->error);
	text = ai_response_get_text(run->response);
	g_assert_null(text);

	run_free(run);
	stub_free(stub);
}

static void
test_timeout_kills_the_child(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;

	stub_set(stub, "sleep", "30");
	stub_set(stub, "stdout", "t:too late\n");

	client = client_for(stub);
	ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(client), 250);

	run = stream_once(client, NULL);

	/*
	 * The synchronous path has always had this deadline; none of the three
	 * streaming readers did, so a CLI wedged on a dead connection held the
	 * stream open indefinitely.
	 */
	g_assert_error(run->error, AI_ERROR, AI_ERROR_TIMEOUT);
	g_assert_null(run->response);

	run_free(run);
	stub_free(stub);
}

static void
test_timeout_zero_disables(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	Run *run;

	stub_set(stub, "stdout", "t:fine\n");

	client = client_for(stub);
	ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(client), 0);

	run = stream_once(client, NULL);
	g_assert_no_error(run->error);

	run_free(run);
	stub_free(stub);
}

static gboolean
cancel_soon(gpointer user_data)
{
	g_cancellable_cancel(G_CANCELLABLE(user_data));
	return G_SOURCE_REMOVE;
}

static void
test_cancellation(void)
{
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Run *run;

	stub_set(stub, "sleep", "30");
	stub_set(stub, "stdout", "t:too late\n");

	client = client_for(stub);
	g_timeout_add(150, cancel_soon, cancellable);

	run = stream_once(client, cancellable);

	/*
	 * A cancelled read completes the task through the GTask's own
	 * cancellation, and must do so exactly once -- the reader deliberately
	 * does not also return the error itself.
	 */
	g_assert_error(run->error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_null(run->response);

	run_free(run);
	stub_free(stub);
}

static void
test_legacy_parse_stream_line_fallback(void)
{
	/*
	 * A subclass that never implements parse_stream_events still works: the
	 * base wraps its delta in a text event. That is how every CLI wrapper
	 * behaved before events existed, so the fallback must be faithful.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	AiCliClientClass *klass;
	Run *run;
	g_autofree gchar *text = NULL;

	stub_set(stub, "stdout", "t:legacy \nu:toolu_01\nt:path\n");
	client = client_for(stub);

	/* Drop the modern vfunc, as an unmigrated subclass would leave it. */
	klass = AI_CLI_CLIENT_GET_CLASS(client);
	klass->parse_stream_events = NULL;

	run = stream_once(client, NULL);

	g_assert_no_error(run->error);
	text = ai_response_get_text(run->response);
	g_assert_cmpstr(text, ==, "legacy path");

	/* Text only -- the old contract could not express a tool call. */
	g_assert_cmpuint(count_kind(run, AI_EVENT_TEXT_DELTA), ==, 2);
	g_assert_cmpuint(count_kind(run, AI_EVENT_TOOL_STARTED), ==, 0);
	g_assert_cmpuint(run->tool_uses, ==, 0);

	/* Restore, so test ordering cannot matter. */
	klass->parse_stream_events = test_cli_parse_stream_events;

	run_free(run);
	stub_free(stub);
}

static void
test_missing_executable(void)
{
	g_autoptr(TestCli) client = g_object_new(TEST_TYPE_CLI, NULL);
	Run *run;

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client),
	                                  "/nonexistent/definitely-not-here");

	run = stream_once(client, NULL);

	g_assert_error(run->error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND);
	g_assert_null(run->response);

	run_free(run);
}

static void
test_working_directory_applied(void)
{
	/*
	 * The spawn vfunc is shared with the synchronous path now, so the
	 * working directory has to reach a streaming child as well.
	 */
	Stub *stub = stub_new();
	g_autoptr(TestCli) client = NULL;
	g_autofree gchar *tmp = g_dir_make_tmp("ai-glib-cwd-XXXXXX", NULL);
	g_autofree gchar *real_tmp = realpath(tmp, NULL);
	g_autofree gchar *cwd_log = NULL;
	Run *run;

	stub_set(stub, "stdout", "t:ok\n");
	client = client_for(stub);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), tmp);

	run = stream_once(client, NULL);
	g_assert_no_error(run->error);

	/* The stub reports its own `pwd -P`, so this is the child's view. */
	cwd_log = stub_read(stub, "cwd.log");
	g_assert_nonnull(cwd_log);
	g_strchomp(cwd_log);
	g_assert_cmpstr(cwd_log, ==, real_tmp != NULL ? real_tmp : tmp);

	g_rmdir(tmp);
	run_free(run);
	stub_free(stub);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/cli-stream/basic", test_basic_stream);
	g_test_add_func("/ai-glib/cli-stream/streaming-flag",
	                test_streaming_flag_reaches_argv);
	g_test_add_func("/ai-glib/cli-stream/tool-use-signal",
	                test_tool_use_signal_is_emitted);
	g_test_add_func("/ai-glib/cli-stream/event-order", test_event_order_preserved);
	g_test_add_func("/ai-glib/cli-stream/events-labelled", test_events_are_labelled);
	g_test_add_func("/ai-glib/cli-stream/empty-output", test_empty_output);
	g_test_add_func("/ai-glib/cli-stream/blank-and-unknown-lines",
	                test_blank_and_unknown_lines);
	g_test_add_func("/ai-glib/cli-stream/final-line-without-newline",
	                test_final_line_without_newline);
	g_test_add_func("/ai-glib/cli-stream/very-long-line", test_very_long_line);
	g_test_add_func("/ai-glib/cli-stream/parse-error-fails",
	                test_parse_error_fails_the_task);
	g_test_add_func("/ai-glib/cli-stream/stdin-written",
	                test_stdin_is_written_and_closed);
	g_test_add_func("/ai-glib/cli-stream/no-stdin",
	                test_no_stdin_when_build_stdin_returns_null);
	g_test_add_func("/ai-glib/cli-stream/nonzero-exit-after-events",
	                test_nonzero_exit_after_valid_events);
	g_test_add_func("/ai-glib/cli-stream/stderr-only",
	                test_stderr_only_with_nonzero_exit);
	g_test_add_func("/ai-glib/cli-stream/timeout", test_timeout_kills_the_child);
	g_test_add_func("/ai-glib/cli-stream/timeout-zero-disables",
	                test_timeout_zero_disables);
	g_test_add_func("/ai-glib/cli-stream/cancellation", test_cancellation);
	g_test_add_func("/ai-glib/cli-stream/legacy-fallback",
	                test_legacy_parse_stream_line_fallback);
	g_test_add_func("/ai-glib/cli-stream/missing-executable",
	                test_missing_executable);
	g_test_add_func("/ai-glib/cli-stream/working-directory",
	                test_working_directory_applied);

	return g_test_run();
}
