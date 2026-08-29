/*
 * test-antigravity-subprocess.c - End-to-end tests for AiAntigravityClient
 *                                 against a stub `agy` binary
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The unit tests in test-antigravity-client.c cover argv/stdin/parse in
 * isolation. These cover the parts only a real subprocess can exercise:
 * that the prompt actually reaches the child through the stdin pipe as a
 * stream-json user event, that argv[0] is overwritten with the resolved
 * executable, that the working directory is applied, that a status=ERROR
 * envelope is still an error, and that the sync, async and streaming
 * paths all agree.
 */

#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "providers/ai-antigravity-client.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-error.h"
#include "model/ai-message.h"

typedef struct
{
	gchar *dir;
	gchar *stub;
} Stub;

#define STUB_TEMPLATE                                                        \
	"#!/bin/sh\n"                                                        \
	"d='%s'\n"                                                           \
	"n=$(cat \"$d/count\" 2>/dev/null || echo 0)\n"                      \
	"n=$((n+1))\n"                                                       \
	"echo \"$n\" > \"$d/count\"\n"                                       \
	"printf '%%s\\n' \"$*\" >> \"$d/argv.log\"\n"                        \
	"pwd -P >> \"$d/cwd.log\"\n"                                         \
	"cat >> \"$d/stdin.log\"\n"                                          \
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

	stub->dir = g_dir_make_tmp("ai-glib-agy-XXXXXX", &error);
	g_assert_no_error(error);

	stub->stub = g_build_filename(stub->dir, "agy", NULL);
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
		"agy", "count", "argv.log", "cwd.log", "stdin.log", "sleep",
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

static AiAntigravityClient *
client_for(Stub *stub)
{
	AiAntigravityClient *client = ai_antigravity_client_new();

	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), stub->stub);

	return client;
}

static GList *
one_message(AiMessage **out_msg)
{
	*out_msg = ai_message_new_user("What is 2+2?");

	return g_list_append(NULL, *out_msg);
}

static const gchar *STUB_JSON_OK =
	"{\n"
	"  \"conversation_id\": \"sess-abc\",\n"
	"  \"status\": \"SUCCESS\",\n"
	"  \"response\": \"4\",\n"
	"  \"usage\": {\"input_tokens\": 11, \"output_tokens\": 2}\n"
	"}\n";

static void
test_sync_success(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
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

static void
test_sync_prompt_reaches_child_stdin(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
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
	g_assert_nonnull(strstr(seen, "\"event\":\"user\""));
	g_assert_nonnull(strstr(seen, "plain text response"));

	g_list_free(messages);
	stub_free(stub);
}

static void
test_sync_argv_reaches_child(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autoptr(AiResponse) resp = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;

	stub_set(stub, "stdout", STUB_JSON_OK);
	ai_antigravity_client_set_skip_permissions(client, TRUE);
	ai_cli_client_set_model(AI_CLI_CLIENT(client),
				AI_ANTIGRAVITY_MODEL_CLAUDE_SONNET_4_6);

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
				       NULL, &error);
	g_assert_no_error(error);

	seen = stub_read(stub, "argv.log");
	g_assert_nonnull(seen);
	g_assert_nonnull(strstr(seen, "--input-format stream-json"));
	g_assert_nonnull(strstr(seen, "--output-format stream-json"));
	g_assert_nonnull(strstr(seen, "--model claude-sonnet-4-6"));
	g_assert_nonnull(strstr(seen, "--dangerously-skip-permissions"));
	g_assert_null(strstr(seen, "What is 2+2?"));
	/* --print-timeout is expected; a lone --print flag is not. */
	g_assert_null(strstr(seen, "--print "));

	g_list_free(messages);
	stub_free(stub);
}

static void
test_sync_working_directory(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
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

static void
test_sync_error_payload_with_zero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AiResponse *resp;
	g_autoptr(GError) error = NULL;

	stub_set(stub, "stdout",
		 "{\"status\":\"ERROR\",\"response\":\"\","
		 "\"error\":\"invalid model selection\"}\n");
	stub_set(stub, "exit", "0\n");

	resp = ai_cli_client_chat_sync(AI_CLI_CLIENT(client), messages,
				       NULL, &error);

	g_assert_null(resp);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "invalid model selection"));

	g_list_free(messages);
	stub_free(stub);
}

static void
test_sync_nonzero_exit(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
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

static void
test_sync_timeout(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
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

static void
test_sync_session_resume(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
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
	g_assert_nonnull(strstr(seen, "--conversation sess-abc"));

	g_list_free(messages);
	stub_free(stub);
}

static void
test_agy_path_env(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *resolved = NULL;

	g_setenv("AGY_PATH", stub->stub, TRUE);
	resolved = ai_cli_client_resolve_executable(AI_CLI_CLIENT(client), &error);
	g_unsetenv("AGY_PATH");

	g_assert_no_error(error);
	g_assert_cmpstr(resolved, ==, stub->stub);

	stub_free(stub);
}

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
	g_autoptr(AiAntigravityClient) client = client_for(stub);
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

	seen = stub_read(stub, "stdin.log");
	g_assert_nonnull(strstr(seen, "What is 2+2?"));

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

static void
test_async_error_payload(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	AsyncCtx ctx;

	stub_set(stub, "stdout",
		 "{\"status\":\"ERROR\",\"error\":\"nope\"}\n");
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

static void
test_async_reprompt_on_empty_text(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autofree gchar *text = NULL;
	g_autofree gchar *seen = NULL;
	AsyncCtx ctx;

	stub_set(stub, "stdout.1",
		 "{\"status\":\"SUCCESS\",\"response\":\"\","
		 "\"conversation_id\":\"sess-retry\"}\n");
	stub_set(stub, "stdout.2",
		 "{\"status\":\"SUCCESS\",\"response\":\"I ran the tests.\","
		 "\"conversation_id\":\"sess-retry\"}\n");

	async_ctx_init(&ctx);

	ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 4096,
			       NULL, NULL, on_chat_done, &ctx);
	async_ctx_run(&ctx);

	g_assert_no_error(ctx.error);
	text = ai_response_get_text(ctx.response);
	g_assert_cmpstr(text, ==, "I ran the tests.");

	seen = stub_read(stub, "argv.log");
	g_assert_nonnull(strstr(seen, "--conversation sess-retry"));

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

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

static const gchar *STUB_NDJSON =
	"{\"event\":\"init\",\"conversation_id\":\"sess-stream\",\"init\":{}}\n"
	"{\"event\":\"step_update\",\"step_update\":{"
	"\"conversation_id\":\"sess-stream\",\"step_index\":2,"
	"\"state\":\"ACTIVE\",\"step_type\":\"agent_response\","
	"\"text_delta\":\"HEL\"}}\n"
	"{\"event\":\"step_update\",\"step_update\":{"
	"\"conversation_id\":\"sess-stream\",\"step_index\":2,"
	"\"state\":\"DONE\",\"step_type\":\"agent_response\","
	"\"text_delta\":\"LO\"}}\n"
	"{\"event\":\"result\",\"result\":{"
	"\"conversation_id\":\"sess-stream\",\"status\":\"SUCCESS\","
	"\"response\":\"HELLO\","
	"\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}}\n";

static void
test_stream_success(void)
{
	Stub *stub = stub_new();
	g_autoptr(AiAntigravityClient) client = client_for(stub);
	g_autoptr(AiMessage) msg = NULL;
	GList *messages = one_message(&msg);
	g_autofree gchar *text = NULL;
	AsyncCtx ctx;

	stub_set(stub, "stdout", STUB_NDJSON);
	async_ctx_init(&ctx);

	g_signal_connect(client, "delta", G_CALLBACK(on_stream_delta), &ctx);
	g_signal_connect(client, "stream-start", G_CALLBACK(on_stream_start), &ctx);
	g_signal_connect(client, "stream-end", G_CALLBACK(on_stream_end), &ctx);

	ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages, NULL,
					4096, NULL, NULL, on_stream_done, &ctx);
	async_ctx_run(&ctx);

	g_assert_no_error(ctx.error);
	g_assert_nonnull(ctx.response);
	g_assert_true(ctx.stream_started);
	g_assert_true(ctx.stream_ended);
	g_assert_cmpstr(ctx.deltas->str, ==, "HELLO");

	text = ai_response_get_text(ctx.response);
	g_assert_cmpstr(text, ==, "HELLO");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
			==, "sess-stream");

	async_ctx_clear(&ctx);
	g_list_free(messages);
	stub_free(stub);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/antigravity-subprocess/sync/success",
			test_sync_success);
	g_test_add_func("/ai-glib/antigravity-subprocess/sync/stdin",
			test_sync_prompt_reaches_child_stdin);
	g_test_add_func("/ai-glib/antigravity-subprocess/sync/argv",
			test_sync_argv_reaches_child);
	g_test_add_func("/ai-glib/antigravity-subprocess/sync/cwd",
			test_sync_working_directory);
	g_test_add_func("/ai-glib/antigravity-subprocess/sync/error-zero-exit",
			test_sync_error_payload_with_zero_exit);
	g_test_add_func("/ai-glib/antigravity-subprocess/sync/nonzero-exit",
			test_sync_nonzero_exit);
	g_test_add_func("/ai-glib/antigravity-subprocess/sync/timeout",
			test_sync_timeout);
	g_test_add_func("/ai-glib/antigravity-subprocess/sync/resume",
			test_sync_session_resume);
	g_test_add_func("/ai-glib/antigravity-subprocess/agy-path",
			test_agy_path_env);

	g_test_add_func("/ai-glib/antigravity-subprocess/async/success",
			test_async_success);
	g_test_add_func("/ai-glib/antigravity-subprocess/async/error",
			test_async_error_payload);
	g_test_add_func("/ai-glib/antigravity-subprocess/async/reprompt",
			test_async_reprompt_on_empty_text);

	g_test_add_func("/ai-glib/antigravity-subprocess/stream/success",
			test_stream_success);

	return g_test_run();
}
