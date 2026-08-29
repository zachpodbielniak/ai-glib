/*
 * test-grok-build-subprocess.c - End-to-end tests for AiGrokBuildClient
 *                                against a stub `grok` binary
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The unit tests in test-grok-build-client.c cover argv/stdin/parse in
 * isolation. These cover the parts only a real subprocess can exercise:
 * that the prompt actually reaches the child through the stdin pipe and
 * is readable as --prompt-file /dev/stdin, that argv[0] is overwritten
 * with the resolved executable without disturbing the rest of argv, that
 * the working directory is applied, that a zero exit status with an error
 * payload is still an error, that the timeout fires, and that the sync,
 * async and streaming paths all agree.
 *
 * The stub is a POSIX shell script whose behaviour is driven by files in
 * its own directory, so each test configures it by writing those files
 * rather than by generating a new script.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "providers/ai-grok-build-client.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-error.h"
#include "model/ai-message.h"
#include "view/ai-conversation.h"
#include "agent/ai-mock-provider.h"

/* ----------------------------------------------------------------
 * Stub harness
 * ---------------------------------------------------------------- */

typedef struct
{
	gchar *dir;    /* scratch directory, removed on teardown */
	gchar *stub;   /* the stub executable */
} Stub;

/*
 * The stub, in full. It records what it was given, optionally stalls or
 * fails, and prints whatever the test staged for this invocation.
 *
 * The invocation counter is what makes the two-call paths testable: the
 * re-prompt after an empty answer, and resuming a captured session.
 *
 * stdin is consumed with `cat` before anything else -- that is both what
 * grok itself does with --prompt-file /dev/stdin and what keeps the
 * parent's write from taking a SIGPIPE.
 *
 * A macro rather than a variable so g_strdup_printf() below sees a string
 * literal and -Wformat-nonliteral stays quiet.
 */
#define STUB_TEMPLATE                                                        \
	"#!/bin/sh\n"                                                        \
	"d='%s'\n"                                                           \
	"n=$(cat \"$d/count\" 2>/dev/null || echo 0)\n"                      \
	"n=$((n+1))\n"                                                       \
	"echo \"$n\" > \"$d/count\"\n"                                       \
	"printf '%%s\\n' \"$*\" >> \"$d/argv.log\"\n"                        \
	"pwd -P >> \"$d/cwd.log\"\n"                                         \
	"cat > \"$d/stdin.$n\"\n"                                            \
	"cat \"$d/stdin.$n\" >> \"$d/stdin.log\"\n"                          \
	"if [ -f \"$d/sleep\" ]; then sleep \"$(cat \"$d/sleep\")\"; fi\n"    \
	"if [ -f \"$d/stderr\" ]; then cat \"$d/stderr\" >&2; fi\n"          \
	"if [ -f \"$d/stdout.$n\" ]; then cat \"$d/stdout.$n\"\n"            \
	"elif [ -f \"$d/stdout\" ]; then cat \"$d/stdout\"; fi\n"            \
	"exit \"$(cat \"$d/exit\" 2>/dev/null || echo 0)\"\n"

static Stub *
stub_new(void)
{
	Stub *stub = g_new0(Stub, 1);
	g_autofree gchar *script = NULL;
	g_autoptr(GError) error = NULL;

	stub->dir = g_dir_make_tmp("ai-glib-grok-XXXXXX", &error);
	g_assert_no_error(error);

	stub->stub = g_build_filename(stub->dir, "grok", NULL);
	script = g_strdup_printf(STUB_TEMPLATE, stub->dir);

	g_file_set_contents(stub->stub, script, -1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(stub->stub, 0700), ==, 0);

	return stub;
}

/* Stage a file in the stub's directory (stdout, stdout.N, stderr, exit, …). */
static void
stub_set(Stub *stub, const gchar *name, const gchar *contents)
{
	g_autofree gchar *path = g_build_filename(stub->dir, name, NULL);
	g_autoptr(GError) error = NULL;

	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);
}

/* Read back one of the stub's logs, or NULL if it never wrote one. */
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
		"grok", "count", "argv.log", "cwd.log", "stdin.log",
		"stdin.1", "stdin.2", "stdin.3", "sleep",
		"stderr", "stdout", "stdout.1", "stdout.2", "stdout.3", "exit",
		NULL
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

/* A client wired to the stub. */
static AiGrokBuildClient *
client_for(Stub *stub)
{
	AiGrokBuildClient *client = ai_grok_build_client_new();

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), stub->stub);

	return client;
}

static GList *
one_message(AiMessage **out_msg)
{
	*out_msg = ai_message_new_user("What is 2+2?");

	return g_list_append(NULL, *out_msg);
}

/* A representative --output-format json payload. */
static const gchar *STUB_JSON_OK =
	"{\n"
	"  \"text\": \"4\",\n"
	"  \"stopReason\": \"end_turn\",\n"
	"  \"sessionId\": \"sess-abc\",\n"
	"  \"usage\": {\"input_tokens\": 11, \"output_tokens\": 2},\n"
	"  \"total_cost_usd\": 0.0004\n"
	"}\n";

typedef struct
{
	GMainLoop *loop;
	gboolean   ok;
	GError    *error;
} ConversationRun;

static void
on_conversation_sent(GObject *source, GAsyncResult *result,
                     gpointer user_data)
{
	ConversationRun *run = user_data;

	run->ok = ai_conversation_send_finish(AI_CONVERSATION(source), result,
	                                      &run->error);
	g_main_loop_quit(run->loop);
}

static void
conversation_send(AiConversation *conversation, const gchar *text,
                  ConversationRun *run)
{
	run->loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_async(conversation, text, NULL,
	                           on_conversation_sent, run);
	g_main_loop_run(run->loop);
	g_main_loop_unref(run->loop);
	run->loop = NULL;
}

/* ----------------------------------------------------------------
 * Synchronous path
 * ---------------------------------------------------------------- */

static void
test_sync_success(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autoptr(AiResponse) resp = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	AiUsage *usage;

	stub_set(stub, "stdout", STUB_JSON_OK);

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_no_error(error);
	g_assert_nonnull(resp);

	text = ai_response_get_text(resp);
	g_assert_cmpstr(text, ==, "4");

	usage = ai_response_get_usage(resp);
	g_assert_nonnull(usage);
	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 11);

	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "sess-abc");

	g_list_free(messages);
	stub_free(stub);
}

/*
 * The load-bearing one: the prompt must arrive on the child's stdin,
 * because that is the only thing --prompt-file /dev/stdin can read. If
 * this breaks, every run still "succeeds" -- against an empty prompt.
 */
static void
test_sync_prompt_reaches_child_stdin(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autoptr(AiResponse) resp = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;

	stub_set(stub, "stdout", STUB_JSON_OK);

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);
	g_assert_no_error(error);

	seen = stub_read(stub, "stdin.log");
	g_assert_nonnull(seen);
	g_assert_nonnull(strstr(seen, "What is 2+2?"));
	/* And the trailer build_stdin appends. */
	g_assert_nonnull(strstr(seen, "plain text response"));

	g_list_free(messages);
	stub_free(stub);
}

/*
 * The cross-provider boundary: history captured from an in-process provider
 * must be projected into the fresh CLI child's stdin, never into --resume.
 */
static void
test_conversation_switch_replays_context_to_child(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiMockProvider) first = ai_mock_provider_new();
	g_autoptr(AiGrokBuildClient) second = client_for(stub);
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(first));
	ConversationRun one = { NULL, FALSE, NULL };
	ConversationRun two = { NULL, FALSE, NULL };
	ConversationRun three = { NULL, FALSE, NULL };
	g_autofree gchar *stdin_seen = NULL;
	g_autofree gchar *delta_seen = NULL;
	g_autofree gchar *argv_seen = NULL;
	GError *error = NULL;

	stub_set(stub, "stdout", STUB_JSON_OK);
	ai_conversation_set_stream(conversation, FALSE);
	ai_mock_provider_push_text(first, "Remembered reply: café.");
	conversation_send(conversation, "First question.", &one);
	g_assert_true(one.ok);
	g_assert_no_error(one.error);

	ai_cli_client_set_session_id(AI_CLI_CLIENT(second), "must-not-leak");
	g_assert_true(ai_conversation_set_provider(
		conversation, G_OBJECT(second), &error));
	g_assert_no_error(error);
	conversation_send(conversation, "Second question.", &two);
	g_assert_true(two.ok);
	g_assert_no_error(two.error);

	stdin_seen = stub_read(stub, "stdin.log");
	g_assert_nonnull(strstr(stdin_seen, "First question."));
	g_assert_nonnull(strstr(stdin_seen,
	                        "Previous assistant response: Remembered reply: café."));
	g_assert_nonnull(strstr(stdin_seen, "Second question."));

	argv_seen = stub_read(stub, "argv.log");
	g_assert_null(strstr(argv_seen, "--resume"));
	g_assert_null(strstr(argv_seen, "must-not-leak"));

	conversation_send(conversation, "Third question.", &three);
	g_assert_true(three.ok);
	g_assert_no_error(three.error);
	delta_seen = stub_read(stub, "stdin.2");
	g_assert_nonnull(strstr(delta_seen, "Third question."));
	g_assert_null(strstr(delta_seen, "First question."));
	g_assert_null(strstr(delta_seen, "Remembered reply"));
	g_assert_null(strstr(delta_seen, "Second question."));

	g_clear_error(&one.error);
	g_clear_error(&two.error);
	g_clear_error(&three.error);
	stub_free(stub);
}

/*
 * argv[0] is replaced with the resolved executable at spawn time; the
 * rest of the tail must survive that surgery intact.
 */
static void
test_sync_argv_reaches_child(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autoptr(AiResponse) resp = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;

	stub_set(stub, "stdout", STUB_JSON_OK);
	ai_grok_build_client_set_skip_permissions(client, TRUE);
	ai_cli_client_set_model(AI_CLI_CLIENT(client), "grok-4.5");

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);
	g_assert_no_error(error);

	seen = stub_read(stub, "argv.log");
	g_assert_nonnull(seen);
	g_assert_nonnull(strstr(seen, "--prompt-file /dev/stdin"));
	g_assert_nonnull(strstr(seen, "--output-format json"));
	g_assert_nonnull(strstr(seen, "--model grok-4.5"));
	g_assert_nonnull(strstr(seen, "--permission-mode bypassPermissions"));
	/* The prompt is never an argument. */
	g_assert_null(strstr(seen, "What is 2+2?"));

	g_list_free(messages);
	stub_free(stub);
}

/* working-directory must actually bound the child, not just be stored. */
static void
test_sync_working_directory(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autoptr(AiResponse) resp = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;
	g_autofree gchar *expected = NULL;

	stub_set(stub, "stdout", STUB_JSON_OK);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), stub->dir);

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);
	g_assert_no_error(error);

	seen = stub_read(stub, "cwd.log");
	g_assert_nonnull(seen);
	g_strstrip(seen);
	expected = realpath(stub->dir, NULL);
	g_assert_nonnull(expected);
	g_assert_cmpstr(seen, ==, expected);

	g_list_free(messages);
	stub_free(stub);
}

/*
 * The trap this client exists to avoid: grok reports a rejected argument
 * as a JSON error object on stdout and still exits 0. A run like that must
 * surface as an error, not as a successful empty answer.
 */
static void
test_sync_error_payload_with_zero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	stub_set(stub, "stdout",
	         "{\"type\":\"error\",\"message\":"
	         "\"unknown effort level 'bogus'\"}\n");
	stub_set(stub, "exit", "0\n");

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "unknown effort level"));

	g_list_free(messages);
	stub_free(stub);
}

/* A non-zero exit with nothing parseable reports the status and stderr. */
static void
test_sync_nonzero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	stub_set(stub, "stderr", "Error: not logged in\n");
	stub_set(stub, "exit", "1\n");

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_null(resp);
	g_assert_nonnull(error);
	g_assert_cmpint(error->domain, ==, AI_ERROR);
	g_assert_nonnull(strstr(error->message, "not logged in"));

	g_list_free(messages);
	stub_free(stub);
}

/*
 * A failing CLI usually explains itself in its own format before exiting.
 * The parsed message must win over the exit status, or the caller reads
 * raw JSON -- or an empty stderr -- where a sentence belongs.
 */
static void
test_sync_error_payload_with_nonzero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	stub_set(stub, "stdout",
	         "{\"type\":\"error\",\"message\":\"rate limit exceeded\"}\n");
	stub_set(stub, "exit", "1\n");

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "rate limit exceeded"));
	/* Not the raw payload, and not a bare status line. */
	g_assert_null(strstr(error->message, "\"type\""));
	g_assert_null(strstr(error->message, "exited with status"));

	g_list_free(messages);
	stub_free(stub);
}

/*
 * Output that fails to parse for some other reason must not mask the exit
 * status: the status is all we know, so it is what gets reported.
 */
static void
test_sync_unparseable_output_with_nonzero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	stub_set(stub, "stdout", "Segmentation fault\n");
	stub_set(stub, "exit", "139\n");

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "139"));

	g_list_free(messages);
	stub_free(stub);
}

/* Success with an empty stdout is a parse error, not an empty answer. */
static void
test_sync_no_output(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);

	g_list_free(messages);
	stub_free(stub);
}

/*
 * process-timeout-ms must actually bound the wait. An unbounded
 * communicate() is the hang class that can freeze a caller forever.
 */
static void
test_sync_timeout(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	stub_set(stub, "stdout", STUB_JSON_OK);
	stub_set(stub, "sleep", "30\n");
	ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(client), 300);

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);

	g_list_free(messages);
	stub_free(stub);
}

/* A second turn resumes the session the first one captured. */
static void
test_sync_session_resume(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autoptr(AiResponse) first = NULL;
	g_autoptr(AiResponse) second = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;

	stub_set(stub, "stdout", STUB_JSON_OK);
	ai_cli_client_set_system_prompt(AI_CLI_CLIENT(client), "SYSTEM");

	first = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                                NULL, &error);
	g_assert_no_error(error);

	second = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                                 NULL, &error);
	g_assert_no_error(error);

	seen = stub_read(stub, "argv.log");
	g_assert_nonnull(seen);
	/* First turn primes the session, second resumes it. */
	g_assert_nonnull(strstr(seen, "--system-prompt-override SYSTEM"));
	g_assert_nonnull(strstr(seen, "--resume sess-abc"));

	g_list_free(messages);
	stub_free(stub);
}

/* A missing executable is named, not silently retried. */
static void
test_sync_executable_not_found(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client),
	                                  "/nonexistent/grok");

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
	                               NULL, &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND);

	g_list_free(messages);
	stub_free(stub_new());   /* no-op; keeps the harness symmetric */
}

/* GROK_PATH is honoured when no explicit executable-path is set. */
static void
test_grok_path_env(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *resolved = NULL;

	g_setenv("GROK_PATH", stub->stub, TRUE);
	resolved = ai_cli_client_resolve_executable(AI_CLI_CLIENT(client), &error);
	g_unsetenv("GROK_PATH");

	g_assert_no_error(error);
	g_assert_cmpstr(resolved, ==, stub->stub);

	stub_free(stub);
}

/* ----------------------------------------------------------------
 * Asynchronous path
 * ---------------------------------------------------------------- */

typedef struct
{
	GMainLoop  *loop;
	AiResponse *response;
	GError     *error;
	GString    *deltas;
	gboolean    stream_started;
	gboolean    stream_ended;
} AsyncCtx;

static gboolean
bail_out(gpointer user_data)
{
	(void)user_data;
	g_error("test timed out waiting for the stub subprocess");
	return G_SOURCE_REMOVE;
}

static void
async_ctx_init(AsyncCtx *ctx)
{
	ctx->loop = g_main_loop_new(NULL, FALSE);
	ctx->response = NULL;
	ctx->error = NULL;
	ctx->deltas = g_string_new("");
	ctx->stream_started = FALSE;
	ctx->stream_ended = FALSE;
}

static void
async_ctx_clear(AsyncCtx *ctx)
{
	g_clear_object(&ctx->response);
	g_clear_error(&ctx->error);
	g_string_free(ctx->deltas, TRUE);
	g_main_loop_unref(ctx->loop);
}

static void
async_ctx_run(AsyncCtx *ctx)
{
	guint guard = g_timeout_add_seconds(30, bail_out, NULL);

	g_main_loop_run(ctx->loop);
	g_source_remove(guard);
}

static void
on_chat_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	AsyncCtx *ctx = user_data;

	ctx->response = ai_provider_chat_finish(AI_PROVIDER(source), result,
	                                        &ctx->error);
	g_main_loop_quit(ctx->loop);
}

static void
test_async_success(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autofree gchar *text = NULL;
	g_autofree gchar *seen = NULL;
	AsyncCtx ctx;

	stub_set(stub, "stdout", STUB_JSON_OK);
	async_ctx_init(&ctx);

	ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 4096,
	                       NULL, NULL, on_chat_done, &ctx);
	async_ctx_run(&ctx);

	g_assert_no_error(ctx.error);
	g_assert_nonnull(ctx.response);
	text = ai_response_get_text(ctx.response);
	g_assert_cmpstr(text, ==, "4");

	/* Same stdin contract as the sync path. */
	seen = stub_read(stub, "stdin.log");
	g_assert_nonnull(strstr(seen, "What is 2+2?"));

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

/* The zero-exit error payload must fail the async path too. */
static void
test_async_error_payload_with_zero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AsyncCtx ctx;

	stub_set(stub, "stdout", "{\"type\":\"error\",\"message\":\"nope\"}\n");
	async_ctx_init(&ctx);

	ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 4096,
	                       NULL, NULL, on_chat_done, &ctx);
	async_ctx_run(&ctx);

	g_assert_null(ctx.response);
	g_assert_error(ctx.error, AI_ERROR, AI_ERROR_CLI_EXECUTION);

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

/*
 * An answer with no text means the model ended on tool calls. With a
 * session to resume, the client re-prompts for a summary and returns that.
 */
static void
test_async_reprompt_on_empty_text(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autofree gchar *text = NULL;
	g_autofree gchar *seen = NULL;
	AsyncCtx ctx;

	/* First call: tool work, no text, but a session id. */
	stub_set(stub, "stdout.1",
	         "{\"text\":\"\",\"sessionId\":\"sess-retry\"}\n");
	/* Second call: the summary the re-prompt asks for. */
	stub_set(stub, "stdout.2",
	         "{\"text\":\"I ran the tests.\",\"sessionId\":\"sess-retry\"}\n");

	async_ctx_init(&ctx);

	ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 4096,
	                       NULL, NULL, on_chat_done, &ctx);
	async_ctx_run(&ctx);
	g_test_assert_expected_messages();

	g_assert_no_error(ctx.error);
	text = ai_response_get_text(ctx.response);
	g_assert_cmpstr(text, ==, "I ran the tests.");

	/* The re-prompt resumed the same session. */
	seen = stub_read(stub, "argv.log");
	g_assert_nonnull(strstr(seen, "--resume sess-retry"));

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

/*
 * With no session there is nothing to resume, so the re-prompt cannot
 * start and the caller gets the placeholder rather than an empty response.
 */
static void
test_async_reprompt_cannot_start(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autofree gchar *text = NULL;
	AsyncCtx ctx;

	stub_set(stub, "stdout", "{\"text\":\"\"}\n");
	async_ctx_init(&ctx);

	ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 4096,
	                       NULL, NULL, on_chat_done, &ctx);
	async_ctx_run(&ctx);
	g_test_assert_expected_messages();

	g_assert_no_error(ctx.error);
	text = ai_response_get_text(ctx.response);
	g_assert_nonnull(strstr(text, "completed tool operations"));

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

/* ----------------------------------------------------------------
 * Streaming path
 * ---------------------------------------------------------------- */

static void
on_stream_delta(GObject *source, const gchar *delta, gpointer user_data)
{
	AsyncCtx *ctx = user_data;
	(void)source;
	g_string_append(ctx->deltas, delta);
}

static void
on_stream_start(GObject *source, gpointer user_data)
{
	AsyncCtx *ctx = user_data;
	(void)source;
	ctx->stream_started = TRUE;
}

static void
on_stream_end(GObject *source, AiResponse *response, gpointer user_data)
{
	AsyncCtx *ctx = user_data;
	(void)source;
	(void)response;
	ctx->stream_ended = TRUE;
}

static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	AsyncCtx *ctx = user_data;

	ctx->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
	                                                 result, &ctx->error);
	g_main_loop_quit(ctx->loop);
}

/* The real NDJSON shape, abbreviated: init, thinking, text, result. */
static const gchar *STUB_NDJSON =
	"{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"sess-stream\"}\n"
	"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
	"\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hm\"}}}\n"
	"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
	"\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"HEL\"}}}\n"
	"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
	"\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"LO\"}}}\n"
	"{\"type\":\"assistant\",\"message\":{\"type\":\"message\",\"content\":"
	"[{\"type\":\"text\",\"text\":\"HELLO\"}]}}\n"
	"{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,"
	"\"result\":\"HELLO\",\"stop_reason\":\"end_turn\","
	"\"usage\":{\"input_tokens\":5,\"output_tokens\":1},"
	"\"total_cost_usd\":0.0001,\"session_id\":\"sess-stream\"}\n";

static void
test_stream_success(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autofree gchar *text = NULL;
	g_autofree gchar *seen = NULL;
	AsyncCtx ctx;

	stub_set(stub, "stdout", STUB_NDJSON);
	async_ctx_init(&ctx);

	g_signal_connect(client, "stream-start",
	                 G_CALLBACK(on_stream_start), &ctx);
	g_signal_connect(client, "delta", G_CALLBACK(on_stream_delta), &ctx);
	g_signal_connect(client, "stream-end", G_CALLBACK(on_stream_end), &ctx);

	ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages, NULL,
	                                4096, NULL, NULL, on_stream_done, &ctx);
	async_ctx_run(&ctx);

	g_assert_no_error(ctx.error);
	g_assert_nonnull(ctx.response);

	g_assert_true(ctx.stream_started);
	g_assert_true(ctx.stream_ended);

	/*
	 * Exactly the text deltas: the thinking delta is dropped and the
	 * whole-message assistant line is not counted a second time.
	 */
	g_assert_cmpstr(ctx.deltas->str, ==, "HELLO");

	text = ai_response_get_text(ctx.response);
	g_assert_cmpstr(text, ==, "HELLO");

	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "sess-stream");

	/* Streaming asks for the partial-message events. */
	seen = stub_read(stub, "argv.log");
	g_assert_nonnull(strstr(seen, "--output-format streaming-messages-json"));
	g_assert_nonnull(strstr(seen, "--include-partial-messages"));

	/* And the prompt still went down the pipe. */
	g_free(seen);
	seen = stub_read(stub, "stdin.log");
	g_assert_nonnull(strstr(seen, "What is 2+2?"));

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

/*
 * A stream that carries no deltas still yields the answer: the result
 * line back-fills it.
 */
static void
test_stream_result_only(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autofree gchar *text = NULL;
	AsyncCtx ctx;

	stub_set(stub, "stdout",
	         "{\"type\":\"result\",\"result\":\"only-final\","
	         "\"session_id\":\"s\"}\n");
	async_ctx_init(&ctx);

	ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages, NULL,
	                                4096, NULL, NULL, on_stream_done, &ctx);
	async_ctx_run(&ctx);

	g_assert_no_error(ctx.error);
	text = ai_response_get_text(ctx.response);
	g_assert_cmpstr(text, ==, "only-final");

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

/* An error line mid-stream fails the operation rather than ending empty. */
static void
test_stream_error_line(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiGrokBuildClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AsyncCtx ctx;

	stub_set(stub, "stdout",
	         "{\"type\":\"system\",\"subtype\":\"init\"}\n"
	         "{\"type\":\"error\",\"message\":\"stream blew up\"}\n");
	async_ctx_init(&ctx);

	ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages, NULL,
	                                4096, NULL, NULL, on_stream_done, &ctx);
	async_ctx_run(&ctx);

	g_assert_null(ctx.response);
	g_assert_error(ctx.error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(ctx.error->message, "stream blew up"));

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/grok-build-subprocess/sync/success",
	                test_sync_success);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/prompt-via-stdin",
	                test_sync_prompt_reaches_child_stdin);
	g_test_add_func("/ai-glib/grok-build-subprocess/conversation-switch",
	                test_conversation_switch_replays_context_to_child);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/argv",
	                test_sync_argv_reaches_child);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/working-directory",
	                test_sync_working_directory);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/error-zero-exit",
	                test_sync_error_payload_with_zero_exit);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/nonzero-exit",
	                test_sync_nonzero_exit);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/error-nonzero-exit",
	                test_sync_error_payload_with_nonzero_exit);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/unparseable-nonzero-exit",
	                test_sync_unparseable_output_with_nonzero_exit);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/no-output",
	                test_sync_no_output);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/timeout",
	                test_sync_timeout);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/session-resume",
	                test_sync_session_resume);
	g_test_add_func("/ai-glib/grok-build-subprocess/sync/executable-not-found",
	                test_sync_executable_not_found);
	g_test_add_func("/ai-glib/grok-build-subprocess/grok-path-env",
	                test_grok_path_env);

	g_test_add_func("/ai-glib/grok-build-subprocess/async/success",
	                test_async_success);
	g_test_add_func("/ai-glib/grok-build-subprocess/async/error-zero-exit",
	                test_async_error_payload_with_zero_exit);
	g_test_add_func("/ai-glib/grok-build-subprocess/async/reprompt",
	                test_async_reprompt_on_empty_text);
	g_test_add_func("/ai-glib/grok-build-subprocess/async/reprompt-cannot-start",
	                test_async_reprompt_cannot_start);

	g_test_add_func("/ai-glib/grok-build-subprocess/stream/success",
	                test_stream_success);
	g_test_add_func("/ai-glib/grok-build-subprocess/stream/result-only",
	                test_stream_result_only);
	g_test_add_func("/ai-glib/grok-build-subprocess/stream/error-line",
	                test_stream_error_line);

	return g_test_run();
}
