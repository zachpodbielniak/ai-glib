/*
 * test-http-malformed.c - The five HTTP providers against a server that
 * answers with the wrong shapes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * `json_object_has_member()` answers "is the key present", which is a
 * different question from "is the value the type I am about to read it
 * as".  `"usage": null` and `"content": {}` both pass it, and the typed
 * accessor that follows logs a Json-CRITICAL and returns NULL --- which
 * then travels into the next accessor for another.  Under GTest, and
 * under `G_DEBUG=fatal-warnings` in production, a critical aborts the
 * process; without it the turn silently yields an empty response.
 *
 * A caller reaches this without a hostile server: `ai_config_set_base_url()`
 * points a provider anywhere, which is what this file does, and a proxy,
 * a gateway or a next API version is enough to change a shape.
 *
 * So the table below is deliberately *one* table run through *every*
 * provider, both the chat and the streaming reader.  Each provider reads
 * its own keys and ignores the rest, and a document aimed at one of them
 * is a fine no-op for the others; what earns this file's keep is that a
 * type-check missing at any of the ~130 sites shows up here rather than
 * at whichever one somebody happened to fix.
 *
 * The assertion is mostly the *absence* of an abort, so each case also
 * insists the turn ended with exactly one of a response and an error.
 * A parse that gave up halfway used to produce neither.
 */

#include <glib.h>
#include <libsoup/soup.h>

#include "ai-glib.h"
#include "core/ai-error.h"

#include "test-server.h"

/* ------------------------------------------------------------------ */
/* The documents                                                       */
/* ------------------------------------------------------------------ */

/*
 * Every one of these is valid JSON with a member of the wrong type, or a
 * JSON null where an object is read.  None is truncated or unparseable:
 * that case already has coverage in test-http-chat.c, and it fails much
 * earlier, at the parser rather than at the accessors.
 *
 * Each line names the provider whose parser it is aimed at, but every
 * line is run through all five.
 */
static const gchar *malformed_documents[] = {
	/* Shared: the error envelope, which every provider checks first. */
	"{\"error\":null}",
	"{\"error\":\"boom\"}",
	"{\"error\":{\"message\":5,\"type\":[]}}",
	"{\"error\":[]}",

	/* Shared: usage, read as an object and then for two integers. */
	"{\"usage\":null}",
	"{\"usage\":7}",
	"{\"usage\":{\"input_tokens\":\"eleven\",\"output_tokens\":null}}",
	"{\"usage\":{\"prompt_tokens\":[],\"completion_tokens\":{}}}",

	/* claude: content[] of blocks. */
	"{\"content\":{}}",
	"{\"content\":[1,2,3]}",
	"{\"content\":[null]}",
	"{\"content\":[{\"type\":9,\"text\":[]}]}",
	"{\"content\":[{\"type\":\"tool_use\",\"id\":[],\"name\":{}}]}",

	/* claude streaming: the per-event payloads. */
	"{\"type\":\"message_start\",\"message\":5}",
	"{\"type\":\"content_block_start\",\"content_block\":\"text\"}",
	"{\"type\":\"content_block_delta\",\"delta\":9}",
	"{\"type\":\"message_delta\",\"delta\":[],\"usage\":\"none\"}",

	/* openai and grok: choices[]. */
	"{\"choices\":{}}",
	"{\"choices\":[1,2,3]}",
	"{\"choices\":[{\"message\":5}]}",
	"{\"choices\":[{\"delta\":\"x\",\"finish_reason\":7}]}",
	"{\"choices\":[{\"message\":{\"content\":[],\"tool_calls\":9}}]}",
	"{\"choices\":[{\"message\":{\"tool_calls\":[{\"function\":3}]}}]}",

	/* gemini: candidates[].content.parts[], and camelCase counters. */
	"{\"candidates\":7}",
	"{\"candidates\":[null]}",
	"{\"candidates\":[{\"content\":9}]}",
	"{\"candidates\":[{\"content\":{\"parts\":9}}]}",
	"{\"candidates\":[{\"content\":{\"parts\":[7]}}]}",
	"{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":2}]}}]}",
	"{\"usageMetadata\":\"none\",\"promptFeedback\":[]}",

	/* ollama: one message object, counters at the top level. */
	"{\"message\":5}",
	"{\"message\":{\"content\":[],\"tool_calls\":9}}",
	"{\"message\":{\"tool_calls\":[{\"function\":\"x\"}]}}",
	"{\"done\":\"yes\",\"prompt_eval_count\":\"11\",\"eval_count\":[]}",

	/* Shared: the scalars every provider copies out of the envelope. */
	"{\"id\":[],\"model\":{},\"stop_reason\":7}",
	"{\"id\":null,\"model\":null,\"object\":9}",

	NULL
};

/* ------------------------------------------------------------------ */
/* The providers                                                       */
/* ------------------------------------------------------------------ */

static gpointer
make_compatible(TServer *ts)
{
	return g_object_new(AI_TYPE_OPENAI_COMPATIBLE_CLIENT,
	                    "base-url", ts->base_url, "api-key", "test-key",
	                    "model", "custom-model", NULL);
}

static gpointer
make_openai(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_OPENAI, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_openai_client_new_with_config(config);
}

static gpointer
make_claude(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_CLAUDE, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_claude_client_new_with_config(config);
}

static gpointer
make_grok(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_GROK, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_GROK, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_grok_client_new_with_config(config);
}

static gpointer
make_gemini(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_GEMINI, ts->base_url);
	ai_config_set_api_key(config, AI_PROVIDER_GEMINI, "test-key");
	ai_config_set_max_retries(config, 0);

	return ai_gemini_client_new_with_config(config);
}

static gpointer
make_ollama(TServer *ts)
{
	g_autoptr(AiConfig) config = ai_config_new();

	ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, ts->base_url);
	ai_config_set_max_retries(config, 0);

	return ai_ollama_client_new_with_config(config);
}

/*
 * Wrap one document in each provider's streaming envelope.
 *
 * claude names its events, so the same payload is offered under every
 * name its reader dispatches on --- the type checks are per branch, and a
 * document delivered only as `message_start` would leave four branches
 * unvisited.
 */
static gchar *
frame_claude(const gchar *document)
{
	return g_strdup_printf(
		"event: message_start\ndata: %s\n\n"
		"event: content_block_start\ndata: %s\n\n"
		"event: content_block_delta\ndata: %s\n\n"
		"event: content_block_stop\ndata: %s\n\n"
		"event: message_delta\ndata: %s\n\n"
		"event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n",
		document, document, document, document, document);
}

/* openai and grok share a dialect: bare `data:` frames, then [DONE]. */
static gchar *
frame_openai(const gchar *document)
{
	return g_strdup_printf("data: %s\n\ndata: [DONE]\n\n", document);
}

static gchar *
frame_gemini(const gchar *document)
{
	return g_strdup_printf("data: %s\n\n", document);
}

/* ollama is NDJSON: one object per line, no prefix. */
static gchar *
frame_ollama(const gchar *document)
{
	return g_strdup_printf("%s\n{\"done\":true,\"done_reason\":\"stop\"}\n",
	                       document);
}

typedef struct
{
	const gchar *name;
	gpointer   (*make)(TServer *ts);
	gchar     *(*frame)(const gchar *document);
	const gchar *stream_type;
} Provider;

static const Provider PROVIDERS[] = {
	{ "openai", make_openai, frame_openai, "text/event-stream" },
	{ "compatible", make_compatible, frame_openai, "text/event-stream" },
	{ "claude", make_claude, frame_claude, "text/event-stream" },
	{ "grok",   make_grok,   frame_openai, "text/event-stream" },
	{ "gemini", make_gemini, frame_gemini, "text/event-stream" },
	{ "ollama", make_ollama, frame_ollama, "application/x-ndjson" }
};

/* ------------------------------------------------------------------ */
/* Driving one turn                                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
	GMainLoop  *loop;
	AiResponse *response;
	GError     *error;
} Turn;

static void
on_chat_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Turn *turn = user_data;

	turn->response = ai_provider_chat_finish(AI_PROVIDER(source), result,
	                                         &turn->error);
	g_main_loop_quit(turn->loop);
}

static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Turn *turn = user_data;

	turn->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
	                                                  result, &turn->error);
	g_main_loop_quit(turn->loop);
}

/*
 * Run one turn and assert it *finished*.
 *
 * Not much of an assertion on its own, and that is the point: the failure
 * this file is about is an abort inside json-glib, so the useful evidence
 * is the test reaching this line at all.  Exactly one of a response and
 * an error is the part worth stating, because a parse that walked into a
 * NULL and gave up produced neither.
 */
static void
drive(gpointer client, gboolean stream, const gchar *what)
{
	Turn                 turn = { NULL, NULL, NULL };
	g_autoptr(AiMessage) msg = ai_message_new_user("hello");
	GList               *messages = g_list_append(NULL, msg);

	turn.loop = g_main_loop_new(NULL, FALSE);

	if (stream)
	{
		ai_streamable_chat_stream_async(AI_STREAMABLE(client), messages,
		                                NULL, 64, NULL, NULL,
		                                on_stream_done, &turn);
	}
	else
	{
		ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 64,
		                       NULL, NULL, on_chat_done, &turn);
	}

	g_main_loop_run(turn.loop);

	if ((turn.response != NULL) == (turn.error != NULL))
	{
		g_error("%s: %s answered neither a response nor an error", what,
		        stream ? "stream" : "chat");
	}

	g_list_free(messages);
	g_main_loop_unref(turn.loop);
	g_clear_object(&turn.response);
	g_clear_error(&turn.error);
}

/*
 * One server and one client per provider and path, with the canned reply
 * changed between turns.  A server per document would be 370 threads and
 * 370 listening sockets for a file whose subject is a parser.
 */
static void
run_table(const Provider *provider, gboolean stream)
{
	TServer  *ts = tserver_new();
	gpointer  client = provider->make(ts);
	gsize     i;

	for (i = 0; malformed_documents[i] != NULL; i++)
	{
		g_autofree gchar *what =
			g_strdup_printf("%s <- %s", provider->name,
			                malformed_documents[i]);

		if (stream)
		{
			g_autofree gchar *body =
				provider->frame(malformed_documents[i]);

			tserver_set_response_full(ts, SOUP_STATUS_OK,
			                          provider->stream_type, body);
		}
		else
		{
			tserver_set_response(ts, SOUP_STATUS_OK,
			                     malformed_documents[i]);
		}

		drive(client, stream, what);
	}

	g_object_unref(client);
	tserver_free(ts);
}

static void test_openai_chat(void) { run_table(&PROVIDERS[0], FALSE); }
static void test_openai_stream(void) { run_table(&PROVIDERS[0], TRUE); }
static void test_claude_chat(void) { run_table(&PROVIDERS[1], FALSE); }
static void test_claude_stream(void) { run_table(&PROVIDERS[1], TRUE); }
static void test_grok_chat(void) { run_table(&PROVIDERS[2], FALSE); }
static void test_grok_stream(void) { run_table(&PROVIDERS[2], TRUE); }
static void test_gemini_chat(void) { run_table(&PROVIDERS[3], FALSE); }
static void test_gemini_stream(void) { run_table(&PROVIDERS[3], TRUE); }
static void test_ollama_chat(void) { run_table(&PROVIDERS[4], FALSE); }
static void test_ollama_stream(void) { run_table(&PROVIDERS[4], TRUE); }

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/*
	 * g_test_init() already does this; saying it here is what makes the
	 * file legible, because the criticals are the subject rather than an
	 * incidental annoyance.  It is also what `G_DEBUG=fatal-warnings`
	 * does to a real program, which is the configuration this bug turns
	 * into an abort rather than an empty answer.
	 */
	g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL |
	                       G_LOG_LEVEL_WARNING);

	g_test_add_func("/ai-glib/http-malformed/openai/chat", test_openai_chat);
	g_test_add_func("/ai-glib/http-malformed/openai/stream",
	                test_openai_stream);
	g_test_add_func("/ai-glib/http-malformed/claude/chat", test_claude_chat);
	g_test_add_func("/ai-glib/http-malformed/claude/stream",
	                test_claude_stream);
	g_test_add_func("/ai-glib/http-malformed/grok/chat", test_grok_chat);
	g_test_add_func("/ai-glib/http-malformed/grok/stream", test_grok_stream);
	g_test_add_func("/ai-glib/http-malformed/gemini/chat", test_gemini_chat);
	g_test_add_func("/ai-glib/http-malformed/gemini/stream",
	                test_gemini_stream);
	g_test_add_func("/ai-glib/http-malformed/ollama/chat", test_ollama_chat);
	g_test_add_func("/ai-glib/http-malformed/ollama/stream",
	                test_ollama_stream);

	return g_test_run();
}
