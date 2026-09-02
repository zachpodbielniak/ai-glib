/*
 * test-cli-malformed.c - The CLI clients against output whose shapes are
 * wrong
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The companion to test-http-malformed.c, for the other half of the
 * library.  Same defect, different input: `json_object_has_member()`
 * answers "is the key present", which is a different question from "is
 * the value the type I am about to read it as".  `"usage": null` and
 * `"part": 5` both pass it, the typed accessor that follows logs a
 * Json-CRITICAL and returns NULL, and that NULL travels into the next
 * accessor for a second one.  GTest makes a critical fatal, and so does
 * `G_DEBUG=fatal-warnings` in production; without either, the turn
 * quietly yields an empty answer.
 *
 * What these parsers read is not a server's reply.  It is a subprocess's
 * stdout and, for the tmux client, a JSONL transcript on disk that
 * `claude` is still appending to --- both untrusted input in this
 * codebase's terms, and neither needs a hostile author to change shape.
 * A CLI that renames a field or moves it from a number to a string is
 * the ordinary case; so is a transcript line read while it is half
 * written.
 *
 * So one table of documents through every CLI parser, both vfuncs, the
 * transcript reader, and one real subprocess.  Each document names the
 * parser it is aimed at, but every document is run through all of them:
 * a parser ignores the keys it does not know, and what earns this file
 * its keep is that a missing type check anywhere shows up here rather
 * than at whichever site somebody happened to fix.
 *
 * The assertion is mostly the *absence* of an abort.  Beyond that, each
 * call must end with exactly one of an answer and an error --- a parse
 * that walked into a NULL and gave up used to produce neither.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "providers/ai-antigravity-client.h"
#include "providers/ai-claude-code-client.h"
#include "providers/ai-claude-tmux-client.h"
#include "providers/ai-claude-tmux-client-internal.h"
#include "providers/ai-cursor-client.h"
#include "providers/ai-grok-build-client.h"
#include "providers/ai-opencode-client.h"
#include "core/ai-cli-client.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "core/ai-streamable.h"
#include "model/ai-message.h"
#include "model/ai-response.h"

/* ------------------------------------------------------------------ */
/* The documents                                                       */
/* ------------------------------------------------------------------ */

/*
 * Every one of these is valid JSON with a member of the wrong type, or a
 * JSON null where an object is read.  None is truncated: an unparseable
 * line is already skipped by every one of these parsers, and it fails at
 * json_parser_load_from_data() rather than at an accessor.
 */
static const gchar *malformed_documents[] = {
	/* Shared: the envelope keys every CLI parser reaches for first. */
	"{\"type\":9}",
	"{\"type\":[],\"session_id\":{},\"sessionID\":7}",
	"{\"error\":null}",
	"{\"error\":\"boom\"}",
	"{\"error\":{\"message\":5,\"name\":[],\"data\":\"x\"}}",
	"{\"error\":{\"data\":{\"message\":9}}}",
	"{\"error\":[]}",

	/* claude-code: the result line, which carries usage and cost. */
	"{\"type\":\"result\",\"usage\":null}",
	"{\"type\":\"result\",\"usage\":7}",
	"{\"type\":\"result\",\"usage\":{\"input_tokens\":\"eleven\","
		"\"output_tokens\":null}}",
	"{\"type\":\"result\",\"result\":[],\"session_id\":{},"
		"\"total_cost_usd\":\"free\"}",
	"{\"type\":\"result\",\"total_cost_usd\":{}}",
	"{\"type\":\"result\",\"subtype\":9,\"is_error\":\"yes\"}",

	/* claude-code: assistant and user lines carry an Anthropic message. */
	"{\"type\":\"assistant\",\"message\":5}",
	"{\"type\":\"assistant\",\"message\":null}",
	"{\"type\":\"assistant\",\"message\":{\"content\":{}}}",
	"{\"type\":\"assistant\",\"message\":{\"content\":[1,2,3]}}",
	"{\"type\":\"assistant\",\"message\":{\"content\":[null]}}",
	"{\"type\":\"assistant\",\"message\":{\"content\":"
		"[{\"type\":9,\"text\":[]}]}}",
	"{\"type\":\"assistant\",\"message\":{\"content\":"
		"[{\"type\":\"tool_use\",\"id\":[],\"name\":{}}]}}",
	"{\"type\":\"assistant\",\"message\":{\"model\":[],\"usage\":\"none\"}}",
	"{\"type\":\"user\",\"message\":{\"content\":"
		"[{\"type\":\"tool_result\",\"tool_use_id\":7,\"content\":5,"
		"\"is_error\":\"yes\"}]}}",
	"{\"type\":\"user\",\"message\":{\"content\":"
		"[{\"type\":\"tool_result\",\"content\":[7,{\"text\":[]}]}]}}",
	"{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":[]}",
	"{\"type\":\"stream_event\",\"event\":9}",
	"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":9}}",

	/* opencode: one part per line, whose state advances. */
	"{\"type\":\"text\",\"part\":null}",
	"{\"type\":\"text\",\"part\":5}",
	"{\"type\":\"text\",\"part\":{\"text\":[]}}",
	"{\"type\":\"tool_use\",\"part\":null}",
	"{\"type\":\"tool_use\",\"part\":{\"tool\":9,\"state\":\"running\"}}",
	"{\"type\":\"tool_use\",\"part\":{\"id\":[],\"callID\":{},"
		"\"state\":{\"status\":7}}}",
	"{\"type\":\"tool_use\",\"part\":{\"state\":{\"status\":\"completed\","
		"\"input\":7,\"output\":[]}}}",
	"{\"type\":\"tool_use\",\"part\":{\"state\":{\"status\":\"completed\","
		"\"input\":{\"command\":[]},\"output\":{}}}}",
	"{\"type\":\"tool_use\",\"part\":{\"state\":{\"status\":\"error\","
		"\"error\":[]}}}",
	"{\"type\":\"step_finish\",\"part\":\"done\"}",
	"{\"type\":\"step_finish\",\"part\":{\"tokens\":\"none\"}}",
	"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":\"x\","
		"\"output\":{}}}}",

	/* grok-build and cursor: camelCase twins of the same keys. */
	"{\"type\":\"result\",\"sessionId\":[],\"stopReason\":9}",
	"{\"type\":\"assistant\",\"delta\":7,\"content_block\":\"text\"}",
	"{\"type\":\"tool_call\",\"toolCall\":9,\"arguments\":\"{\"}",
	"{\"type\":\"usage\",\"usage\":{\"inputTokens\":[],\"outputTokens\":{}}}",

	/* antigravity: steps carry an index and a nested payload. */
	"{\"type\":\"step\",\"step\":9}",
	"{\"type\":\"step\",\"step\":{\"step_index\":\"first\",\"output\":[]}}",

	NULL
};

/*
 * The tmux client reads a transcript rather than a stream, and its keys
 * differ enough from the stdout dialects to be worth their own table:
 * `sessionId` not `session_id`, an inner `message` object, and two
 * entry kinds that decide whether a prompt was accepted at all.
 */
static const gchar *malformed_transcript_lines[] = {
	"{\"type\":9}",
	"{\"type\":\"assistant\"}",
	"{\"type\":\"assistant\",\"message\":null}",
	"{\"type\":\"assistant\",\"message\":5}",
	"{\"type\":\"assistant\",\"message\":[]}",
	"{\"type\":\"assistant\",\"message\":{\"role\":[]}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"content\":{}}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"content\":[1,2,3]}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"content\":[{\"type\":9,\"text\":[]}]}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"content\":[{\"type\":\"text\",\"text\":7}]}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"model\":[],\"stop_reason\":7}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"usage\":null}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"usage\":{\"input_tokens\":\"x\",\"output_tokens\":[]}}}",
	"{\"type\":\"assistant\",\"sessionId\":[],"
		"\"message\":{\"role\":\"assistant\"}}",
	"{\"type\":\"assistant\",\"total_cost_usd\":\"free\","
		"\"message\":{\"role\":\"assistant\"}}",
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"total_cost_usd\":{}}}",
	"{\"type\":\"user\",\"isCompactSummary\":\"yes\"}",
	"{\"type\":\"queue-operation\",\"operation\":[]}",
	NULL
};

/*
 * One well-formed assistant entry, so a transcript built around a
 * malformed line still has the thing parse_jsonl() insists on finding.
 * Without it every case would end in the same "no assistant entry"
 * error and the accumulation past the bad line would go unvisited.
 */
#define GOOD_ASSISTANT_LINE                                                  \
	"{\"type\":\"assistant\",\"sessionId\":\"s-1\","                     \
	"\"message\":{\"role\":\"assistant\",\"model\":\"m\","               \
	"\"stop_reason\":\"end_turn\","                                      \
	"\"content\":[{\"type\":\"text\",\"text\":\"hello\"}],"              \
	"\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}}"

/* ------------------------------------------------------------------ */
/* The clients                                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
	const gchar *name;
	AiCliClient *(*make)(void);
} CliProvider;

static AiCliClient *make_claude_code(void)
{ return AI_CLI_CLIENT(ai_claude_code_client_new()); }

static AiCliClient *make_opencode(void)
{ return AI_CLI_CLIENT(ai_opencode_client_new()); }

static AiCliClient *make_grok_build(void)
{ return AI_CLI_CLIENT(ai_grok_build_client_new()); }

static AiCliClient *make_cursor(void)
{ return AI_CLI_CLIENT(ai_cursor_client_new()); }

static AiCliClient *make_antigravity(void)
{ return AI_CLI_CLIENT(ai_antigravity_client_new()); }

static const CliProvider PROVIDERS[] = {
	{ "claude-code", make_claude_code },
	{ "opencode",    make_opencode    },
	{ "grok-build",  make_grok_build  },
	{ "cursor",      make_cursor      },
	{ "antigravity", make_antigravity }
};

/* ------------------------------------------------------------------ */
/* Driving the two parse vfuncs                                        */
/* ------------------------------------------------------------------ */

/*
 * One line through parse_stream_events().
 *
 * Session persistence is left on: capturing a session id off the line is
 * one of the reads, and a client that never stores one would not perform
 * it.  A fresh AiResponse per line, because a parser is entitled to
 * assume it owns the one it is handed.
 */
static void
drive_stream_events(AiCliClient *client, const gchar *line, const gchar *what)
{
	AiCliClientClass    *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(AiResponse) response = ai_response_new("", "test-model");
	g_autoptr(GPtrArray)  events =
		g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	g_autoptr(GError)     error = NULL;
	gboolean              ok;

	g_assert_nonnull(klass->parse_stream_events);

	ok = klass->parse_stream_events(client, line, response, events, &error);

	if (ok == (error != NULL))
	{
		g_error("%s: parse_stream_events returned %s and %s an error", what,
		        ok ? "TRUE" : "FALSE", error != NULL ? "set" : "did not set");
	}
}

/* One document through parse_json_output(), which reads the whole stdout. */
static void
drive_json_output(AiCliClient *client, const gchar *output, const gchar *what)
{
	AiCliClientClass    *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(GError)     error = NULL;
	g_autoptr(AiResponse) response = NULL;

	g_assert_nonnull(klass->parse_json_output);

	response = klass->parse_json_output(client, output, &error);

	if ((response != NULL) == (error != NULL))
	{
		g_error("%s: parse_json_output answered neither a response nor an "
		        "error", what);
	}
}

static void
run_table(const CliProvider *provider, gboolean stream)
{
	AiCliClient      *client = provider->make();
	g_autoptr(GString) whole = g_string_new(NULL);
	gsize             i;

	for (i = 0; malformed_documents[i] != NULL; i++)
	{
		g_autofree gchar *what =
			g_strdup_printf("%s <- %s", provider->name,
			                malformed_documents[i]);

		g_string_append(whole, malformed_documents[i]);
		g_string_append_c(whole, '\n');

		if (stream)
			drive_stream_events(client, malformed_documents[i], what);
		else
			drive_json_output(client, malformed_documents[i], what);
	}

	/*
	 * And the whole table as one stdout.  A parser that accumulates
	 * across lines --- opencode's text and tool summary do --- carries
	 * state from one bad document into the next, which the per-document
	 * runs above cannot reach.
	 */
	if (!stream)
	{
		g_autofree gchar *what =
			g_strdup_printf("%s <- the whole table", provider->name);

		drive_json_output(client, whole->str, what);
	}

	g_object_unref(client);
}

static void test_cc_events(void) { run_table(&PROVIDERS[0], TRUE); }
static void test_cc_output(void) { run_table(&PROVIDERS[0], FALSE); }
static void test_oc_events(void) { run_table(&PROVIDERS[1], TRUE); }
static void test_oc_output(void) { run_table(&PROVIDERS[1], FALSE); }
static void test_gb_events(void) { run_table(&PROVIDERS[2], TRUE); }
static void test_gb_output(void) { run_table(&PROVIDERS[2], FALSE); }
static void test_cu_events(void) { run_table(&PROVIDERS[3], TRUE); }
static void test_cu_output(void) { run_table(&PROVIDERS[3], FALSE); }
static void test_agy_events(void) { run_table(&PROVIDERS[4], TRUE); }
static void test_agy_output(void) { run_table(&PROVIDERS[4], FALSE); }

/* ------------------------------------------------------------------ */
/* The tmux client's transcript                                        */
/* ------------------------------------------------------------------ */

/*
 * parse_jsonl() with the bad line on both sides of the good one.
 *
 * Order matters here in a way it does not for a stream: the parser keeps
 * "the last assistant entry wins", so a document ahead of the good line
 * is read and then overwritten, and one behind it overwrites what the
 * good line established.  Only the second reaches the code that frees
 * and replaces the accumulated strings.
 */
static void
drive_transcript(const gchar *document)
{
	gsize i;

	for (i = 0; i < 2; i++)
	{
		g_autofree gchar     *content =
			i == 0
			? g_strconcat(document, "\n", GOOD_ASSISTANT_LINE, "\n", NULL)
			: g_strconcat(GOOD_ASSISTANT_LINE, "\n", document, "\n", NULL);
		g_autoptr(GError)     error = NULL;
		g_autoptr(AiResponse) response = NULL;
		gdouble               cost = -1.0;

		response = ai_claude_tmux_client_parse_jsonl(content, NULL, &cost,
		                                             &error);

		if ((response != NULL) == (error != NULL))
		{
			g_error("transcript <- %s (order %d): parse_jsonl answered "
			        "neither a response nor an error", document, (int)i);
		}

		/* The good line is in every one of these, so it must be found. */
		g_assert_nonnull(response);
	}
}

static void
test_transcript_parse(void)
{
	g_autoptr(GString) whole = g_string_new(NULL);
	g_autoptr(GError)  error = NULL;
	g_autoptr(AiResponse) response = NULL;
	gsize              i;

	for (i = 0; malformed_transcript_lines[i] != NULL; i++)
	{
		drive_transcript(malformed_transcript_lines[i]);

		g_string_append(whole, malformed_transcript_lines[i]);
		g_string_append_c(whole, '\n');
	}

	g_string_append(whole, GOOD_ASSISTANT_LINE "\n");

	response = ai_claude_tmux_client_parse_jsonl(whole->str, NULL, NULL,
	                                             &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
}

/*
 * The two predicates the turn loop polls with.  Neither returns an
 * error, so the whole assertion is that the answer is a boolean rather
 * than an abort --- which is the failure mode: these run once every
 * poll interval against a file `claude` is still writing.
 */
static void
test_transcript_predicates(void)
{
	g_autoptr(GString) whole = g_string_new(NULL);
	gsize i;

	for (i = 0; malformed_transcript_lines[i] != NULL; i++)
	{
		ai_claude_tmux_client_jsonl_has_accepted_prompt(
			malformed_transcript_lines[i]);
		ai_claude_tmux_client_jsonl_has_terminal_stop(
			malformed_transcript_lines[i]);

		g_string_append(whole, malformed_transcript_lines[i]);
		g_string_append_c(whole, '\n');
	}

	ai_claude_tmux_client_jsonl_has_accepted_prompt(whole->str);
	ai_claude_tmux_client_jsonl_has_terminal_stop(whole->str);
}

/* ------------------------------------------------------------------ */
/* One real subprocess                                                 */
/* ------------------------------------------------------------------ */

/*
 * The tables above call the parsers directly, which is where the type
 * checks are.  This one goes the whole way --- a script on disk, spawned
 * by the client, writing the same documents to a pipe --- because a
 * synthetic driver removes the platform, and the platform here is a
 * child process whose stdout is read a line at a time.
 */
typedef struct
{
	GMainLoop  *loop;
	AiResponse *response;
	GError     *error;
} Turn;

static void
on_stream_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Turn *turn = user_data;

	turn->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
	                                                  result, &turn->error);
	g_main_loop_quit(turn->loop);
}

static void
on_chat_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Turn *turn = user_data;

	turn->response = ai_provider_chat_finish(AI_PROVIDER(source), result,
	                                         &turn->error);
	g_main_loop_quit(turn->loop);
}

/*
 * One child process, its stdout set to @stdout_body.
 *
 * The streaming path reads that a line at a time; the chat path hands
 * the whole of it to parse_json_output() as one document. So the caller
 * decides what the child writes, and the chat cases run one document per
 * child --- feeding the concatenation would only ever reach the first.
 */
static void
run_spawned_cli(const gchar *stdout_body, gboolean stream)
{
	g_autoptr(GError)     error = NULL;
	g_autofree gchar     *dir = NULL;
	g_autofree gchar     *script_path = NULL;
	g_autofree gchar     *script = NULL;
	g_autoptr(AiClaudeCodeClient) client = NULL;
	g_autoptr(AiMessage)  message = NULL;
	GList                *messages = NULL;
	Turn                  turn = { NULL, NULL, NULL };

	dir = g_dir_make_tmp("ai-glib-cli-malformed-XXXXXX", &error);
	g_assert_no_error(error);

	/*
	 * cat >/dev/null first: the client pipes the prompt in, and a child
	 * that exits without draining it turns this into a race with
	 * SIGPIPE rather than a test of the parser.
	 */
	script_path = g_build_filename(dir, "claude", NULL);
	script = g_strdup_printf("#!/bin/sh\ncat >/dev/null\ncat <<'EOF'\n%s\n"
	                         "EOF\n", stdout_body);

	g_file_set_contents(script_path, script, -1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(script_path, 0700), ==, 0);

	client = ai_claude_code_client_new();
	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), script_path);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), dir);

	message = ai_message_new_user("hello");
	messages = g_list_append(NULL, message);

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
		g_error("spawned CLI %s <- %s: answered neither a response nor an "
		        "error", stream ? "stream" : "chat", stdout_body);
	}

	g_list_free(messages);
	g_main_loop_unref(turn.loop);
	g_clear_object(&turn.response);
	g_clear_error(&turn.error);

	g_assert_cmpint(g_unlink(script_path), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* Every document as one NDJSON stream, through one child. */
static void
test_spawned_cli_stream(void)
{
	g_autoptr(GString) body = g_string_new(NULL);
	gsize              i;

	for (i = 0; malformed_documents[i] != NULL; i++)
	{
		g_string_append(body, malformed_documents[i]);
		g_string_append_c(body, '\n');
	}

	run_spawned_cli(body->str, TRUE);
}

/* And one child per document for the whole-stdout parser. */
static void
test_spawned_cli_chat(void)
{
	gsize i;

	for (i = 0; malformed_documents[i] != NULL; i++)
	{
		run_spawned_cli(malformed_documents[i], FALSE);
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	/*
	 * g_test_init() already does this.  Saying it again is what makes
	 * the file legible: the criticals are the subject rather than an
	 * incidental annoyance, and this is what `G_DEBUG=fatal-warnings`
	 * does to a real program --- the configuration in which this defect
	 * aborts rather than quietly answering nothing.
	 */
	g_log_set_always_fatal(G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL |
	                       G_LOG_LEVEL_WARNING);

	g_test_add_func("/ai-glib/cli-malformed/claude-code/events",
	                test_cc_events);
	g_test_add_func("/ai-glib/cli-malformed/claude-code/output",
	                test_cc_output);
	g_test_add_func("/ai-glib/cli-malformed/opencode/events", test_oc_events);
	g_test_add_func("/ai-glib/cli-malformed/opencode/output", test_oc_output);
	g_test_add_func("/ai-glib/cli-malformed/grok-build/events",
	                test_gb_events);
	g_test_add_func("/ai-glib/cli-malformed/grok-build/output",
	                test_gb_output);
	g_test_add_func("/ai-glib/cli-malformed/cursor/events", test_cu_events);
	g_test_add_func("/ai-glib/cli-malformed/cursor/output", test_cu_output);
	g_test_add_func("/ai-glib/cli-malformed/antigravity/events",
	                test_agy_events);
	g_test_add_func("/ai-glib/cli-malformed/antigravity/output",
	                test_agy_output);

	g_test_add_func("/ai-glib/cli-malformed/tmux/transcript",
	                test_transcript_parse);
	g_test_add_func("/ai-glib/cli-malformed/tmux/predicates",
	                test_transcript_predicates);

	/*
	 * "spawned-cli", not "subprocess": GTest reserves a path segment
	 * spelled `subprocess` for g_test_trap_subprocess(), and quietly
	 * leaves such a test out of an ordinary run --- the group markers
	 * are printed, the plan counts one fewer, and the exit status is
	 * still 0.  This test was written with that name and did not run.
	 */
	g_test_add_func("/ai-glib/cli-malformed/spawned-cli/stream",
	                test_spawned_cli_stream);
	g_test_add_func("/ai-glib/cli-malformed/spawned-cli/chat",
	                test_spawned_cli_chat);

	return g_test_run();
}
