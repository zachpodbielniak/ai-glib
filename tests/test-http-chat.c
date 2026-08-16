/*
 * test-http-chat.c - The HTTP providers' async chat and streaming paths
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * These paths had no test at all until now, and that is exactly why every
 * one of them leaked a GTask -- and, through it, the client, its AiConfig
 * and their strings -- for as long as they did.  test-image-generator.c was
 * the only test that ran a provider against a real server, so only the
 * image-generation leaks were ever caught.
 *
 * The assertions here are deliberately modest.  What earns its keep is that
 * the code *executes* inside a leak-checked binary: `make test ASAN=1` now
 * walks a real request and a real SSE stream through each provider, and any
 * reference dropped on the floor along the way shows up as a failure rather
 * than surviving another year.
 *
 * Both wire formats are covered because they are genuinely different code:
 * OpenAI-style `choices[]` JSON with `data:`-only SSE, and Anthropic's
 * `content[]` with named `event:` frames.
 */

#include <glib.h>
#include <libsoup/soup.h>

#include "ai-glib.h"
#include "core/ai-error.h"

#include "test-server.h"

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

static const gchar *openai_ok_body =
	"{\"id\":\"chatcmpl-1\",\"model\":\"gpt-4o\",\"choices\":[{\"index\":0,"
	"\"message\":{\"role\":\"assistant\",\"content\":\"the reply\"},"
	"\"finish_reason\":\"stop\"}],"
	"\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":7}}";

static const gchar *claude_ok_body =
	"{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\","
	"\"model\":\"claude-sonnet-5\","
	"\"content\":[{\"type\":\"text\",\"text\":\"the reply\"}],"
	"\"stop_reason\":\"end_turn\","
	"\"usage\":{\"input_tokens\":11,\"output_tokens\":7}}";

/* SSE, as each provider actually receives it. */
static const gchar *openai_sse_body =
	"data: {\"id\":\"1\",\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
	"data: {\"id\":\"1\",\"choices\":[{\"delta\":{\"content\":\"strea\"}}]}\n\n"
	"data: {\"id\":\"1\",\"choices\":[{\"delta\":{\"content\":\"ming\"}}]}\n\n"
	"data: {\"id\":\"1\",\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
	"data: [DONE]\n\n";

static const gchar *claude_sse_body =
	"event: message_start\n"
	"data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\","
	"\"model\":\"claude-sonnet-5\",\"usage\":{\"input_tokens\":11}}}\n\n"
	"event: content_block_start\n"
	"data: {\"type\":\"content_block_start\",\"index\":0,"
	"\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
	"event: content_block_delta\n"
	"data: {\"type\":\"content_block_delta\",\"index\":0,"
	"\"delta\":{\"type\":\"text_delta\",\"text\":\"strea\"}}\n\n"
	"event: content_block_delta\n"
	"data: {\"type\":\"content_block_delta\",\"index\":0,"
	"\"delta\":{\"type\":\"text_delta\",\"text\":\"ming\"}}\n\n"
	"event: content_block_stop\n"
	"data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
	"event: message_delta\n"
	"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
	"\"usage\":{\"output_tokens\":7}}\n\n"
	"event: message_stop\n"
	"data: {\"type\":\"message_stop\"}\n\n";

/* ------------------------------------------------------------------ */
/* Clients                                                             */
/* ------------------------------------------------------------------ */

static AiOpenAIClient *
make_openai(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_OPENAI, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_openai_client_new_with_config(config);
}

static AiClaudeClient *
make_claude(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_CLAUDE, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_claude_client_new_with_config(config);
}

/* ------------------------------------------------------------------ */
/* Driving one turn                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
	GMainLoop  *loop;
	AiResponse *response;
	GError     *error;

	GString    *deltas;       /* every ::delta concatenated */
	guint       starts;
	guint       ends;
	gboolean    done;
} Turn;

static Turn *
turn_new(void)
{
	Turn *turn = g_new0(Turn, 1);

	turn->loop = g_main_loop_new(NULL, FALSE);
	turn->deltas = g_string_new(NULL);

	return turn;
}

static void
turn_free(Turn *turn)
{
	g_main_loop_unref(turn->loop);
	g_string_free(turn->deltas, TRUE);
	g_clear_object(&turn->response);
	g_clear_error(&turn->error);
	g_free(turn);
}

static void
on_chat_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Turn *turn = user_data;

	turn->response = ai_provider_chat_finish(AI_PROVIDER(source), result,
	                                         &turn->error);
	turn->done = TRUE;
	g_main_loop_quit(turn->loop);
}

static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Turn *turn = user_data;

	turn->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
	                                                  result, &turn->error);
	turn->done = TRUE;
	g_main_loop_quit(turn->loop);
}

static void on_delta(GObject *s, const gchar *text, gpointer d)
{ (void)s; g_string_append(((Turn *)d)->deltas, text); }

static void on_stream_start(GObject *s, gpointer d)
{ (void)s; ((Turn *)d)->starts++; }

static void on_stream_end(GObject *s, GObject *r, gpointer d)
{ (void)s; (void)r; ((Turn *)d)->ends++; }

/* One non-streaming turn, run to completion. */
static Turn *
chat_once(gpointer client, GCancellable *cancellable)
{
	Turn *turn = turn_new();
	g_autoptr(AiMessage) msg = ai_message_new_user("hello");
	GList *messages = g_list_append(NULL, msg);

	ai_provider_chat_async(AI_PROVIDER(client), messages, "be brief", 64,
	                       NULL, cancellable, on_chat_done, turn);
	g_main_loop_run(turn->loop);

	g_list_free(messages);

	return turn;
}

/* One streaming turn, with the signals collected. */
static Turn *
stream_once(gpointer client, GCancellable *cancellable)
{
	Turn *turn = turn_new();
	g_autoptr(AiMessage) msg = ai_message_new_user("hello");
	GList *messages = g_list_append(NULL, msg);

	g_signal_connect(client, "delta", G_CALLBACK(on_delta), turn);
	g_signal_connect(client, "stream-start", G_CALLBACK(on_stream_start), turn);
	g_signal_connect(client, "stream-end", G_CALLBACK(on_stream_end), turn);

	ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages,
	                                "be brief", 64, NULL, cancellable,
	                                on_stream_done, turn);
	g_main_loop_run(turn->loop);

	g_signal_handlers_disconnect_by_data(client, turn);
	g_list_free(messages);

	return turn;
}

/* ------------------------------------------------------------------ */
/* OpenAI                                                              */
/* ------------------------------------------------------------------ */

static void
test_openai_chat_round_trip(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;
	g_autofree gchar *text = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *auth = NULL;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);
	client = make_openai(ts);

	turn = chat_once(client, NULL);

	g_assert_no_error(turn->error);
	g_assert_nonnull(turn->response);

	text = ai_response_get_text(turn->response);
	g_assert_cmpstr(text, ==, "the reply");

	/* The request really went on the wire, to the right place. */
	g_assert_cmpuint(tserver_hits(ts), ==, 1);
	path = tserver_dup_last_path(ts);
	g_assert_cmpstr(path, ==, "/v1/chat/completions");

	auth = tserver_dup_header(ts, "Authorization");
	g_assert_cmpstr(auth, ==, "Bearer test-key");

	turn_free(turn);
	tserver_free(ts);
}

static void
test_openai_chat_sends_the_prompt(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;
	g_autofree gchar *body = NULL;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);
	client = make_openai(ts);

	turn = chat_once(client, NULL);
	g_assert_no_error(turn->error);

	body = tserver_dup_last_body(ts);
	g_assert_nonnull(body);
	g_assert_nonnull(strstr(body, "hello"));
	g_assert_nonnull(strstr(body, "be brief"));

	turn_free(turn);
	tserver_free(ts);
}

static void
test_openai_chat_usage(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;
	AiUsage *usage;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);
	client = make_openai(ts);

	turn = chat_once(client, NULL);
	g_assert_no_error(turn->error);

	usage = ai_response_get_usage(turn->response);
	g_assert_nonnull(usage);
	g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 11);
	g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 7);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_openai_stream_deltas(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;
	g_autofree gchar *text = NULL;

	tserver_set_response_full(ts, SOUP_STATUS_OK, "text/event-stream",
	                          openai_sse_body);
	client = make_openai(ts);

	turn = stream_once(client, NULL);

	g_assert_no_error(turn->error);
	g_assert_nonnull(turn->response);

	/* The deltas arrived in order and assembled into the answer. */
	g_assert_cmpstr(turn->deltas->str, ==, "streaming");
	g_assert_cmpuint(turn->starts, ==, 1);
	g_assert_cmpuint(turn->ends, ==, 1);

	text = ai_response_get_text(turn->response);
	g_assert_cmpstr(text, ==, "streaming");

	turn_free(turn);
	tserver_free(ts);
}

/* ------------------------------------------------------------------ */
/* Claude                                                              */
/* ------------------------------------------------------------------ */

static void
test_claude_chat_round_trip(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiClaudeClient) client = NULL;
	Turn *turn;
	g_autofree gchar *text = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *key = NULL;

	tserver_set_response(ts, SOUP_STATUS_OK, claude_ok_body);
	client = make_claude(ts);

	turn = chat_once(client, NULL);

	g_assert_no_error(turn->error);
	g_assert_nonnull(turn->response);

	text = ai_response_get_text(turn->response);
	g_assert_cmpstr(text, ==, "the reply");

	path = tserver_dup_last_path(ts);
	g_assert_cmpstr(path, ==, "/v1/messages");

	/* Anthropic authenticates by x-api-key, not Authorization. */
	key = tserver_dup_header(ts, "x-api-key");
	g_assert_cmpstr(key, ==, "test-key");

	turn_free(turn);
	tserver_free(ts);
}

static void
test_claude_chat_stop_reason(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiClaudeClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, SOUP_STATUS_OK, claude_ok_body);
	client = make_claude(ts);

	turn = chat_once(client, NULL);
	g_assert_no_error(turn->error);

	g_assert_cmpint(ai_response_get_stop_reason(turn->response), ==,
	                AI_STOP_REASON_END_TURN);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_claude_stream_deltas(void)
{
	/*
	 * Anthropic's SSE is a different shape from OpenAI's -- named event:
	 * frames rather than bare data: lines -- and a different decoder, so it
	 * needs its own pass.
	 */
	TServer *ts = tserver_new();
	g_autoptr(AiClaudeClient) client = NULL;
	Turn *turn;
	g_autofree gchar *text = NULL;

	tserver_set_response_full(ts, SOUP_STATUS_OK, "text/event-stream",
	                          claude_sse_body);
	client = make_claude(ts);

	turn = stream_once(client, NULL);

	g_assert_no_error(turn->error);
	g_assert_nonnull(turn->response);

	g_assert_cmpstr(turn->deltas->str, ==, "streaming");
	g_assert_cmpuint(turn->starts, ==, 1);
	g_assert_cmpuint(turn->ends, ==, 1);

	text = ai_response_get_text(turn->response);
	g_assert_cmpstr(text, ==, "streaming");

	turn_free(turn);
	tserver_free(ts);
}

/* ------------------------------------------------------------------ */
/* Failure paths                                                       */
/* ------------------------------------------------------------------ */

static void
test_chat_http_error(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, 500, "{\"error\":{\"message\":\"boom\"}}");
	client = make_openai(ts);

	turn = chat_once(client, NULL);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_chat_unauthorized(void)
{
	/* 401 has its own mapping, and a caller branches on the code. */
	TServer *ts = tserver_new();
	g_autoptr(AiClaudeClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, 401,
	                     "{\"type\":\"error\",\"error\":"
	                     "{\"type\":\"authentication_error\",\"message\":\"nope\"}}");
	client = make_claude(ts);

	turn = chat_once(client, NULL);

	g_assert_null(turn->response);
	g_assert_error(turn->error, AI_ERROR, AI_ERROR_INVALID_API_KEY);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_chat_rate_limited(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiClaudeClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, 429,
	                     "{\"type\":\"error\",\"error\":"
	                     "{\"type\":\"rate_limit_error\",\"message\":\"slow\"}}");
	client = make_claude(ts);

	turn = chat_once(client, NULL);

	g_assert_null(turn->response);
	g_assert_error(turn->error, AI_ERROR, AI_ERROR_RATE_LIMITED);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_chat_malformed_json(void)
{
	/*
	 * A 200 whose body is not JSON at all. The parser has to fail rather
	 * than crash -- server output is untrusted even when the status is not.
	 */
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, SOUP_STATUS_OK, "not json {{{");
	client = make_openai(ts);

	turn = chat_once(client, NULL);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_chat_empty_body(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, SOUP_STATUS_OK, "");
	client = make_openai(ts);

	turn = chat_once(client, NULL);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_chat_null_json_body(void)
{
	/*
	 * A bare `null` parses fine and yields a NULL root, which every parser
	 * used to hand straight to JSON_NODE_HOLDS_OBJECT() -- a critical, and
	 * fatal under GTest. Kept here as a live regression check.
	 */
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, SOUP_STATUS_OK, "null");
	client = make_openai(ts);

	turn = chat_once(client, NULL);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_stream_http_error(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	Turn *turn;

	tserver_set_response(ts, 500, "{\"error\":{\"message\":\"boom\"}}");
	client = make_openai(ts);

	turn = stream_once(client, NULL);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);

	/* Nothing was streamed, so no end was claimed either. */
	g_assert_cmpuint(turn->ends, ==, 0);

	turn_free(turn);
	tserver_free(ts);
}

/* ------------------------------------------------------------------ */
/* Cancellation                                                        */
/* ------------------------------------------------------------------ */

static gboolean
cancel_now(gpointer user_data)
{
	g_cancellable_cancel(G_CANCELLABLE(user_data));

	return G_SOURCE_REMOVE;
}

static void
test_chat_cancelled_mid_request(void)
{
	/*
	 * The server stalls, the caller gives up. This is the path where a
	 * leaked task is easiest to introduce, because the completion handler
	 * takes an early return.
	 */
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Turn *turn;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);
	tserver_set_delay(ts, 400);
	client = make_openai(ts);

	g_timeout_add(60, cancel_now, cancellable);

	turn = chat_once(client, cancellable);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_stream_cancelled_mid_request(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiClaudeClient) client = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Turn *turn;

	tserver_set_response_full(ts, SOUP_STATUS_OK, "text/event-stream",
	                          claude_sse_body);
	tserver_set_delay(ts, 400);
	client = make_claude(ts);

	g_timeout_add(60, cancel_now, cancellable);

	turn = stream_once(client, cancellable);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);

	turn_free(turn);
	tserver_free(ts);
}

static void
test_chat_cancelled_before_start(void)
{
	/* Already cancelled: the request should not reach the server at all. */
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Turn *turn;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);
	client = make_openai(ts);

	g_cancellable_cancel(cancellable);
	turn = chat_once(client, cancellable);

	g_assert_null(turn->response);
	g_assert_nonnull(turn->error);
	g_assert_cmpuint(tserver_hits(ts), ==, 0);

	turn_free(turn);
	tserver_free(ts);
}

/* ------------------------------------------------------------------ */
/* Repetition                                                          */
/* ------------------------------------------------------------------ */

static void
test_many_turns_on_one_client(void)
{
	/*
	 * A per-turn leak is easy to miss when a test does one turn: 184 bytes
	 * reads like noise. Ten on one client makes it unmistakable, and also
	 * covers reusing a client rather than building a fresh one each time.
	 */
	TServer *ts = tserver_new();
	g_autoptr(AiOpenAIClient) client = NULL;
	guint i;

	tserver_set_response(ts, SOUP_STATUS_OK, openai_ok_body);
	client = make_openai(ts);

	for (i = 0; i < 10; i++)
	{
		Turn *turn = chat_once(client, NULL);
		g_autofree gchar *text = NULL;

		g_assert_no_error(turn->error);
		text = ai_response_get_text(turn->response);
		g_assert_cmpstr(text, ==, "the reply");

		turn_free(turn);
	}

	g_assert_cmpuint(tserver_hits(ts), ==, 10);

	tserver_free(ts);
}

static void
test_many_streams_on_one_client(void)
{
	TServer *ts = tserver_new();
	g_autoptr(AiClaudeClient) client = NULL;
	guint i;

	tserver_set_response_full(ts, SOUP_STATUS_OK, "text/event-stream",
	                          claude_sse_body);
	client = make_claude(ts);

	for (i = 0; i < 5; i++)
	{
		Turn *turn = stream_once(client, NULL);

		g_assert_no_error(turn->error);
		g_assert_cmpstr(turn->deltas->str, ==, "streaming");

		turn_free(turn);
	}

	tserver_free(ts);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/http-chat/openai/round-trip",
	                test_openai_chat_round_trip);
	g_test_add_func("/ai-glib/http-chat/openai/sends-prompt",
	                test_openai_chat_sends_the_prompt);
	g_test_add_func("/ai-glib/http-chat/openai/usage", test_openai_chat_usage);
	g_test_add_func("/ai-glib/http-chat/openai/stream", test_openai_stream_deltas);

	g_test_add_func("/ai-glib/http-chat/claude/round-trip",
	                test_claude_chat_round_trip);
	g_test_add_func("/ai-glib/http-chat/claude/stop-reason",
	                test_claude_chat_stop_reason);
	g_test_add_func("/ai-glib/http-chat/claude/stream", test_claude_stream_deltas);

	g_test_add_func("/ai-glib/http-chat/error/http-500", test_chat_http_error);
	g_test_add_func("/ai-glib/http-chat/error/unauthorized", test_chat_unauthorized);
	g_test_add_func("/ai-glib/http-chat/error/rate-limited", test_chat_rate_limited);
	g_test_add_func("/ai-glib/http-chat/error/malformed-json",
	                test_chat_malformed_json);
	g_test_add_func("/ai-glib/http-chat/error/empty-body", test_chat_empty_body);
	g_test_add_func("/ai-glib/http-chat/error/null-body", test_chat_null_json_body);
	g_test_add_func("/ai-glib/http-chat/error/stream-http-500",
	                test_stream_http_error);

	g_test_add_func("/ai-glib/http-chat/cancel/chat",
	                test_chat_cancelled_mid_request);
	g_test_add_func("/ai-glib/http-chat/cancel/stream",
	                test_stream_cancelled_mid_request);
	g_test_add_func("/ai-glib/http-chat/cancel/before-start",
	                test_chat_cancelled_before_start);

	g_test_add_func("/ai-glib/http-chat/repeat/chat", test_many_turns_on_one_client);
	g_test_add_func("/ai-glib/http-chat/repeat/stream",
	                test_many_streams_on_one_client);

	return g_test_run();
}
