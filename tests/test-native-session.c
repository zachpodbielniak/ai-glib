/*
 * test-native-session.c - Reading a wrapped CLI's own transcript
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Two halves, and both matter.
 *
 * The parser cases drive ai_native_session_read_file() against transcript
 * text written here, so a format's shape is asserted without a harness
 * installed and without this developer's own history on disk.  The
 * malformed table runs with criticals fatal, and most of what it asserts
 * is that the process is still running: these files come from other
 * programs, and one bad line must not take a session with it.
 *
 * The locator cases sandbox HOME and the working directory, per the rule
 * in CLAUDE.md.  A suite that read the real ~/.claude would pass or fail
 * by whose machine ran it.
 */

#include <string.h>
#include <utime.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "ai-glib.h"

typedef struct
{
	gchar *root;    /* the sandboxed HOME */
	gchar *cwd;     /* the sandboxed working directory */
} Fixture;

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	fixture->root = g_dir_make_tmp("ai-native-XXXXXX", NULL);
	g_assert_nonnull(fixture->root);

	fixture->cwd = g_build_filename(fixture->root, "project", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->cwd, 0700), ==, 0);
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *command = g_strdup_printf("rm -rf %s", fixture->root);

	g_spawn_command_line_sync(command, NULL, NULL, NULL, &error);
	g_clear_pointer(&fixture->root, g_free);
	g_clear_pointer(&fixture->cwd, g_free);
}

/* Write @text to a file under @directory, creating the directory. */
static gchar *
write_file(const gchar *directory, const gchar *name, const gchar *text)
{
	gchar *path;

	g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
	path = g_build_filename(directory, name, NULL);
	g_assert_true(g_file_set_contents(path, text, -1, NULL));

	return path;
}

/* Concatenate the whole history into one searchable string. */
static gchar *
flatten(AiNativeSession *session)
{
	g_autoptr(GString) out = g_string_new(NULL);
	GList            *iter;

	for (iter = ai_native_session_get_messages(session);
	     iter != NULL;
	     iter = iter->next)
	{
		AiMessage        *message = AI_MESSAGE(iter->data);
		g_autofree gchar *text = ai_message_get_text(message);

		g_string_append_printf(out, "<%d>%s\n",
		                       (gint)ai_message_get_role(message),
		                       text != NULL ? text : "");
	}

	return g_strdup(out->str);
}

/* ================================================================
 * claude-code
 * ================================================================ */

#define CLAUDE_USER(text) \
	"{\"type\":\"user\",\"message\":{\"role\":\"user\"," \
	"\"content\":[{\"type\":\"text\",\"text\":\"" text "\"}]}}\n"

#define CLAUDE_ASSISTANT(text) \
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\"," \
	"\"content\":[{\"type\":\"text\",\"text\":\"" text "\"}]}}\n"

static void
test_claude_basic(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		CLAUDE_USER("remember the word amber")
		CLAUDE_ASSISTANT("Noted.")
		CLAUDE_USER("what word?"));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("claude-code", path, "sid",
	                                      &error);
	g_assert_no_error(error);
	g_assert_nonnull(session);

	/* Three records, three messages: the roles alternate, so nothing
	 * coalesces. */
	g_assert_cmpint(g_list_length(ai_native_session_get_messages(session)),
	                ==, 3);
	g_assert_false(ai_native_session_get_compacted(session));
	g_assert_cmpstr(ai_native_session_get_session_id(session), ==, "sid");
	g_assert_cmpstr(ai_native_session_get_provider(session), ==,
	                "claude-code");

	text = flatten(session);
	g_assert_nonnull(g_strstr_len(text, -1, "amber"));
}

/*
 * A thinking block is dropped and a tool exchange is labelled.  This is
 * the difference the whole feature exists for: ai-glib never saw the tool
 * call, and a switch that carried only "Noted." would lose the work.
 */
static void
test_claude_tools(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		CLAUDE_USER("read the parser")
		"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"content\":["
		"{\"type\":\"thinking\",\"thinking\":\"SECRETREASONING\"},"
		"{\"type\":\"tool_use\",\"id\":\"call-7\",\"name\":\"read_file\","
		"\"input\":{\"path\":\"src/parser.c\"}}]}}\n"
		"{\"type\":\"user\",\"message\":{\"role\":\"user\","
		"\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call-7\","
		"\"is_error\":false,\"content\":\"int main(void);\"}]}}\n"
		CLAUDE_ASSISTANT("It declares main."));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_no_error(error);

	text = flatten(session);
	g_assert_nonnull(g_strstr_len(text, -1,
		"[Tool call: read_file; id=call-7;"));
	g_assert_nonnull(g_strstr_len(text, -1, "src/parser.c"));
	g_assert_nonnull(g_strstr_len(text, -1,
		"[Tool result: read_file"));
	g_assert_nonnull(g_strstr_len(text, -1, "int main(void);"));

	/* Another model cannot continue this model's reasoning, and several
	 * providers reject it outright. */
	g_assert_null(g_strstr_len(text, -1, "SECRETREASONING"));
}

/* Everything before the boundary goes; the summary record itself stays,
 * because it *is* the compacted history. */
static void
test_claude_compaction(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		CLAUDE_USER("ANCIENT first request")
		CLAUDE_ASSISTANT("ANCIENT first answer")
		"{\"type\":\"user\",\"isCompactSummary\":true,"
		"\"message\":{\"role\":\"user\",\"content\":"
		"[{\"type\":\"text\",\"text\":\"SUMMARY of what came before\"}]}}\n"
		CLAUDE_ASSISTANT("Carrying on.")
		CLAUDE_USER("RECENT question"));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_no_error(error);

	g_assert_true(ai_native_session_get_compacted(session));
	g_assert_cmpuint(ai_native_session_get_dropped(session), ==, 2);

	text = flatten(session);
	g_assert_null(g_strstr_len(text, -1, "ANCIENT"));
	g_assert_nonnull(g_strstr_len(text, -1, "SUMMARY of what came before"));
	g_assert_nonnull(g_strstr_len(text, -1, "RECENT question"));
}

/* The *last* boundary wins.  A session compacted twice must not be
 * restored from the first one, which would carry back everything the
 * second compaction decided to drop. */
static void
test_claude_compaction_twice(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		CLAUDE_USER("ANCIENT")
		"{\"type\":\"user\",\"isCompactSummary\":true,"
		"\"message\":{\"role\":\"user\",\"content\":"
		"[{\"type\":\"text\",\"text\":\"FIRSTSUMMARY\"}]}}\n"
		CLAUDE_USER("MIDDLE")
		"{\"type\":\"system\",\"subtype\":\"compact_boundary\","
		"\"message\":{\"role\":\"user\",\"content\":\"SECONDSUMMARY\"}}\n"
		CLAUDE_USER("RECENT"));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ai_native_session_get_compacted(session));

	text = flatten(session);
	g_assert_null(g_strstr_len(text, -1, "ANCIENT"));
	g_assert_null(g_strstr_len(text, -1, "FIRSTSUMMARY"));
	g_assert_null(g_strstr_len(text, -1, "MIDDLE"));
	g_assert_nonnull(g_strstr_len(text, -1, "RECENT"));
}

/* A subagent's transcript is interleaved into the parent file and is not
 * part of this conversation. */
static void
test_claude_sidechain(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		CLAUDE_USER("main question")
		"{\"type\":\"assistant\",\"isSidechain\":true,"
		"\"message\":{\"role\":\"assistant\",\"content\":"
		"[{\"type\":\"text\",\"text\":\"SUBAGENTCHATTER\"}]}}\n"
		CLAUDE_ASSISTANT("main answer"));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_no_error(error);

	text = flatten(session);
	g_assert_null(g_strstr_len(text, -1, "SUBAGENTCHATTER"));
	g_assert_nonnull(g_strstr_len(text, -1, "main answer"));
}

/* ================================================================
 * codex
 * ================================================================ */

static void
test_codex_basic(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "r.jsonl",
		"{\"type\":\"session_meta\",\"payload\":{\"cwd\":\"/tmp\"}}\n"
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
		"\"role\":\"developer\",\"content\":"
		"[{\"type\":\"input_text\",\"text\":\"SCAFFOLDING\"}]}}\n"
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
		"\"role\":\"user\",\"content\":"
		"[{\"type\":\"input_text\",\"text\":\"find the bug\"}]}}\n"
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"reasoning\","
		"\"summary\":[{\"text\":\"SECRETREASONING\"}]}}\n"
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"function_call\","
		"\"name\":\"shell\",\"call_id\":\"c1\","
		"\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}\n"
		"{\"type\":\"response_item\",\"payload\":"
		"{\"type\":\"function_call_output\",\"call_id\":\"c1\","
		"\"output\":\"README.md\"}}\n"
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
		"\"role\":\"assistant\",\"content\":"
		"[{\"type\":\"output_text\",\"text\":\"Found it.\"}]}}\n");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("codex-cli", path, NULL, &error);
	g_assert_no_error(error);

	text = flatten(session);
	g_assert_nonnull(g_strstr_len(text, -1, "find the bug"));
	g_assert_nonnull(g_strstr_len(text, -1, "[Tool call: shell; id=c1;"));
	g_assert_nonnull(g_strstr_len(text, -1, "README.md"));
	g_assert_nonnull(g_strstr_len(text, -1, "Found it."));

	/* codex's developer items describe how codex is configured. Telling
	 * another provider it has those tools and rules would be a lie. */
	g_assert_null(g_strstr_len(text, -1, "SCAFFOLDING"));
	g_assert_null(g_strstr_len(text, -1, "SECRETREASONING"));
}

/*
 * codex is the one harness that hands the post-compaction history to the
 * marker itself.  The replacement array must seed the result and the
 * records after the marker must continue it, in that order.
 */
static void
test_codex_compaction(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "r.jsonl",
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
		"\"role\":\"user\",\"content\":"
		"[{\"type\":\"input_text\",\"text\":\"ANCIENT\"}]}}\n"
		"{\"type\":\"compacted\",\"payload\":{\"message\":\"\","
		"\"replacement_history\":[{\"type\":\"message\",\"role\":\"user\","
		"\"content\":[{\"type\":\"input_text\",\"text\":\"KEPT\"}]}]}}\n"
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
		"\"role\":\"assistant\",\"content\":"
		"[{\"type\":\"output_text\",\"text\":\"AFTER\"}]}}\n");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;
	GList            *messages;

	session = ai_native_session_read_file("codex-cli", path, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ai_native_session_get_compacted(session));

	text = flatten(session);
	g_assert_null(g_strstr_len(text, -1, "ANCIENT"));
	g_assert_nonnull(g_strstr_len(text, -1, "KEPT"));
	g_assert_nonnull(g_strstr_len(text, -1, "AFTER"));

	/* Order, not just presence: the replacement history is the prefix. */
	messages = ai_native_session_get_messages(session);
	g_assert_cmpint(g_list_length(messages), ==, 2);
	{
		g_autofree gchar *first = ai_message_get_text(
			AI_MESSAGE(g_list_first(messages)->data));

		g_assert_cmpstr(first, ==, "KEPT");
	}
}

/* ================================================================
 * grok-build
 * ================================================================ */

#define GROK_CHUNK(kind, text) \
	"{\"method\":\"session/update\",\"params\":{\"update\":{" \
	"\"sessionUpdate\":\"" kind "\",\"content\":{\"type\":\"text\"," \
	"\"text\":\"" text "\"}}}}\n"

/*
 * Grok streams an answer as many chunks.  Each is its own record, and
 * appending each as a separate message would turn one answer into nine
 * consecutive assistant turns --- a shape no provider ever produces.
 */
static void
test_grok_chunks(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "updates.jsonl",
		GROK_CHUNK("user_message_chunk", "what is 2+2?")
		GROK_CHUNK("agent_thought_chunk", "SECRETREASONING")
		GROK_CHUNK("agent_message_chunk", "The answer ")
		GROK_CHUNK("agent_message_chunk", "is four.")
		GROK_CHUNK("user_message_chunk", "thanks"));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("grok-build", path, NULL, &error);
	g_assert_no_error(error);

	/* user, assistant, user --- three, not five. */
	g_assert_cmpint(g_list_length(ai_native_session_get_messages(session)),
	                ==, 3);

	text = flatten(session);
	g_assert_nonnull(g_strstr_len(text, -1, "The answer"));
	g_assert_nonnull(g_strstr_len(text, -1, "is four."));
	g_assert_null(g_strstr_len(text, -1, "SECRETREASONING"));
}

static void
test_grok_compaction(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "updates.jsonl",
		GROK_CHUNK("user_message_chunk", "ANCIENT")
		"{\"method\":\"session/update\",\"params\":{\"update\":{"
		"\"sessionUpdate\":\"compaction_checkpoint\","
		"\"checkpoint_id\":\"abc\"}}}\n"
		"{\"method\":\"session/update\",\"params\":{\"update\":{"
		"\"sessionUpdate\":\"session_recap\","
		"\"summary\":\"We fixed the parser.\"}}}\n"
		GROK_CHUNK("user_message_chunk", "RECENT"));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("grok-build", path, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ai_native_session_get_compacted(session));

	text = flatten(session);
	g_assert_null(g_strstr_len(text, -1, "ANCIENT"));
	g_assert_nonnull(g_strstr_len(text, -1,
		"[Session recap: We fixed the parser.]"));
	g_assert_nonnull(g_strstr_len(text, -1, "RECENT"));
}

/* ================================================================
 * Malformed input
 * ================================================================ */

/*
 * Every one of these is a real transcript that another program could
 * write, and every one used to be a way to lose a session.  The
 * assertion that matters most is the one the harness makes for us: the
 * process is still running, with criticals fatal.
 */
static void
test_malformed(Fixture *fixture, gconstpointer data)
{
	static const gchar *documents[] = {
		/* Not JSON at all. */
		"this is not json\n",
		/* A root that is not an object. */
		"[1,2,3]\n\"a string\"\n42\nnull\ntrue\n",
		/* Right shape, wrong types throughout --- the silent-NULL case
		 * that json-glib's *_with_default() accessors do not catch. */
		"{\"type\":7,\"message\":{\"role\":true,\"content\":9}}\n"
		"{\"type\":\"user\",\"message\":[],\"isSidechain\":\"yes\"}\n"
		"{\"type\":\"user\",\"message\":{\"content\":[7,null,{}]}}\n"
		"{\"type\":\"user\",\"message\":{\"content\":"
		"[{\"type\":7},{\"type\":\"text\",\"text\":42}]}}\n"
		"{\"type\":\"assistant\",\"message\":{\"content\":"
		"[{\"type\":\"tool_use\",\"id\":[],\"name\":{},\"input\":null}]}}\n",
		/* JSON null members where an object is expected. */
		"{\"type\":null,\"message\":null,\"isCompactSummary\":null}\n",
		/* Empty and whitespace-only lines between good records. */
		"\n\n" CLAUDE_USER("survivor") "\n\n\n",
		/* A last line still being written --- the normal state of a
		 * transcript whose owner is running. */
		CLAUDE_USER("survivor") "{\"type\":\"assist",
		/* Deeply nested content that must not recurse without bound. */
		"{\"type\":\"user\",\"message\":{\"content\":"
		"[{\"type\":\"tool_use\",\"input\":"
		"[[[[[[[[[[1]]]]]]]]]]}]}}\n",
		/* Empty file. */
		"",
		/* No trailing newline. */
		"{\"type\":\"user\",\"message\":{\"role\":\"user\","
		"\"content\":[{\"type\":\"text\",\"text\":\"tail\"}]}}"
	};
	static const gchar *providers[] = {
		"claude-code", "claude-tmux", "codex", "codex-cli", "grok-build"
	};
	gsize i;
	gsize p;

	for (p = 0; p < G_N_ELEMENTS(providers); p++)
	{
		for (i = 0; i < G_N_ELEMENTS(documents); i++)
		{
			g_autofree gchar *path = NULL;
			g_autoptr(GError) error = NULL;
			g_autoptr(AiNativeSession) session = NULL;
			g_autofree gchar *text = NULL;

			path = write_file(fixture->cwd, "m.jsonl", documents[i]);

			/* Every one of these parses: a malformed record costs
			 * itself and nothing else. */
			session = ai_native_session_read_file(providers[p], path,
			                                      NULL, &error);
			g_assert_no_error(error);
			g_assert_nonnull(session);

			/* And the digest over whatever survived is still safe to
			 * build, which is where the result actually gets used. */
			text = ai_native_session_to_context_text(session, 4096);
			(void)text;

			g_unlink(path);
		}
	}
}

/* Invalid UTF-8 is the one hard failure, because everything downstream
 * assumes otherwise --- the digest goes straight into a prompt. */
static void
test_invalid_utf8(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fixture->cwd, "bad.jsonl",
	                                          NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	const gchar       bytes[] = "{\"type\":\"user\"}\n\xff\xfe\n";

	g_assert_true(g_file_set_contents(path, bytes, sizeof bytes - 1, NULL));

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_null(session);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

/* A NUL would truncate every strchr() walk without saying so, producing a
 * short import that reads as a short session. */
static void
test_embedded_nul(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fixture->cwd, "nul.jsonl",
	                                          NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	const gchar       bytes[] = "{\"type\":\"user\"}\n\0{\"type\":\"user\"}\n";

	g_assert_true(g_file_set_contents(path, bytes, sizeof bytes - 1, NULL));

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_null(session);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
}

static void
test_missing_file(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = g_build_filename(fixture->cwd, "nope.jsonl",
	                                          NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_null(session);
	g_assert_nonnull(error);
}

/*
 * The three SQLite-backed harnesses report unsupported rather than
 * half-read.  A partial import is worse than none: the gap is invisible
 * to the model that receives it.
 */
static void
test_unsupported_providers(Fixture *fixture, gconstpointer data)
{
	static const gchar *unsupported[] = {
		"opencode", "cursor", "antigravity", "claude", "openai", "gemini"
	};
	static const gchar *supported[] = {
		"claude-code", "claude-tmux", "codex", "codex-cli", "grok-build"
	};
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
	                                    CLAUDE_USER("hi"));
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(unsupported); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(AiNativeSession) session = NULL;

		g_assert_cmpint(
			ai_native_session_kind_for_provider(unsupported[i]), ==,
			AI_NATIVE_SESSION_UNSUPPORTED);

		session = ai_native_session_read_file(unsupported[i], path, NULL,
		                                      &error);
		g_assert_null(session);
		g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
	}

	for (i = 0; i < G_N_ELEMENTS(supported); i++)
	{
		g_assert_cmpint(ai_native_session_kind_for_provider(supported[i]),
		                ==, AI_NATIVE_SESSION_JSONL);
	}

	/* NULL is a question, not a crash. */
	g_assert_cmpint(ai_native_session_kind_for_provider(NULL), ==,
	                AI_NATIVE_SESSION_UNSUPPORTED);
}

/* ================================================================
 * The digest
 * ================================================================ */

static void
test_digest_trims_oldest(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		CLAUDE_USER("OLDEST message here")
		CLAUDE_ASSISTANT("second message here")
		CLAUDE_USER("third message here")
		CLAUDE_ASSISTANT("NEWEST message here"));
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *full = NULL;
	g_autofree gchar *trimmed = NULL;

	session = ai_native_session_read_file("claude-code", path, "sid",
	                                      &error);
	g_assert_no_error(error);

	full = ai_native_session_to_context_text(session, 0);
	g_assert_nonnull(full);
	g_assert_nonnull(g_strstr_len(full, -1, "OLDEST"));
	g_assert_nonnull(g_strstr_len(full, -1, "NEWEST"));
	/* The header names the provider so the receiving model knows the
	 * exchange is carried over rather than something it said. */
	g_assert_nonnull(g_strstr_len(full, -1, "claude-code"));

	/*
	 * Recency is what a continuing conversation needs.  A digest trimmed
	 * from the other end would keep the opening pleasantries and lose the
	 * thing being worked on.
	 */
	trimmed = ai_native_session_to_context_text(session, 60);
	g_assert_nonnull(trimmed);
	g_assert_nonnull(g_strstr_len(trimmed, -1, "NEWEST"));
	g_assert_null(g_strstr_len(trimmed, -1, "OLDEST"));
	g_assert_nonnull(g_strstr_len(trimmed, -1, "omitted"));
}

/* An empty history has no digest, rather than a header describing
 * nothing. */
static void
test_digest_empty(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		"{\"type\":\"mode\",\"mode\":\"default\"}\n");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_no_error(error);
	g_assert_null(ai_native_session_get_messages(session));
	g_assert_null(ai_native_session_to_context_text(session, 4096));
}

/* A compacted session says so, so the receiving model knows the history
 * it is being given has a floor rather than being the whole story. */
static void
test_digest_notes_compaction(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl",
		CLAUDE_USER("old")
		"{\"type\":\"user\",\"isCompactSummary\":true,"
		"\"message\":{\"role\":\"user\",\"content\":"
		"[{\"type\":\"text\",\"text\":\"summary\"}]}}\n");
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *digest = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_no_error(error);

	digest = ai_native_session_to_context_text(session, 0);
	g_assert_nonnull(g_strstr_len(digest, -1, "compacted"));
}

/* A huge tool result is summarised, not reproduced: pasting a 300 KiB
 * file read into a system prompt spends the whole context window on
 * something the new provider can simply read again. */
static void
test_large_tool_result(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *filler = g_strnfill(9000, 'x');
	g_autofree gchar *body = g_strdup_printf(
		"{\"type\":\"user\",\"message\":{\"role\":\"user\","
		"\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"c1\","
		"\"content\":\"%s\"}]}}\n", filler);
	g_autofree gchar *path = write_file(fixture->cwd, "s.jsonl", body);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *text = NULL;

	session = ai_native_session_read_file("claude-code", path, NULL, &error);
	g_assert_no_error(error);

	text = flatten(session);
	g_assert_cmpuint(strlen(text), <, 5000);
	g_assert_nonnull(g_strstr_len(text, -1, "[truncated]"));
}

/* ================================================================
 * Locating a client's transcript
 * ================================================================ */

/*
 * Sandboxed HOME and working directory, per CLAUDE.md.  Without both,
 * this reads the developer's real ~/.claude and passes or fails by whose
 * machine ran it.
 */
static void
test_locate_claude(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *encoded = NULL;
	g_autofree gchar *projects = NULL;
	g_autofree gchar *older = NULL;
	g_autofree gchar *newer = NULL;
	g_autofree gchar *text = NULL;
	GHashTable       *environment;
	gsize             i;

	/* claude replaces every separator, dot and underscore with a dash. */
	encoded = g_strdup(fixture->cwd);
	for (i = 0; encoded[i] != '\0'; i++)
	{
		if (encoded[i] == '/' || encoded[i] == '.' || encoded[i] == '_')
		{
			encoded[i] = '-';
		}
	}

	projects = g_build_filename(fixture->root, ".claude", "projects",
	                            encoded, NULL);
	older = write_file(projects, "older.jsonl", CLAUDE_USER("STALE"));
	newer = write_file(projects, "newer.jsonl", CLAUDE_USER("CURRENT"));

	/* mtime, not name order: none of these harnesses names a transcript
	 * so that it sorts chronologically. */
	{
		GStatBuf info;
		struct utimbuf times;

		g_assert_cmpint(g_stat(newer, &info), ==, 0);
		times.actime = info.st_atime;
		times.modtime = info.st_mtime - 500;
		g_assert_cmpint(g_utime(older, &times), ==, 0);
	}

	environment = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);
	g_hash_table_insert(environment, g_strdup("HOME"),
	                    g_strdup(fixture->root));
	ai_cli_client_set_environment(AI_CLI_CLIENT(client), environment);
	g_hash_table_unref(environment);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client),
	                                    fixture->cwd);

	/* With no session id pinned, the newest transcript wins --- the one
	 * `--continue` would have resumed. */
	session = ai_cli_client_read_native_session(AI_CLI_CLIENT(client),
	                                            &error);
	g_assert_no_error(error);
	g_assert_nonnull(session);
	g_assert_cmpstr(ai_native_session_get_session_id(session), ==, "newer");

	text = flatten(session);
	g_assert_nonnull(g_strstr_len(text, -1, "CURRENT"));
	g_assert_null(g_strstr_len(text, -1, "STALE"));

	/* An explicit session id always wins over the newest. */
	{
		g_autoptr(GError) pinned_error = NULL;
		g_autoptr(AiNativeSession) pinned = NULL;
		g_autofree gchar *pinned_text = NULL;

		ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "older");
		pinned = ai_cli_client_read_native_session(AI_CLI_CLIENT(client),
		                                           &pinned_error);
		g_assert_no_error(pinned_error);
		pinned_text = flatten(pinned);
		g_assert_nonnull(g_strstr_len(pinned_text, -1, "STALE"));
	}
}

/* No transcript yet is not a loud failure: a first turn that has not run
 * is the ordinary state of a fresh session. */
static void
test_locate_nothing(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	GHashTable *environment;

	environment = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);
	g_hash_table_insert(environment, g_strdup("HOME"),
	                    g_strdup(fixture->root));
	ai_cli_client_set_environment(AI_CLI_CLIENT(client), environment);
	g_hash_table_unref(environment);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client),
	                                    fixture->cwd);

	session = ai_cli_client_read_native_session(AI_CLI_CLIENT(client),
	                                            &error);
	g_assert_null(session);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND);
}

/* A provider with no readable store says so through the client entry
 * point too, not only through the kind lookup. */
static void
test_locate_unsupported(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiCursorClient) client = ai_cursor_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;

	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client),
	                                    fixture->cwd);

	session = ai_cli_client_read_native_session(AI_CLI_CLIENT(client),
	                                            &error);
	g_assert_null(session);
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
}

/* Grok buckets sessions by percent-encoded cwd, with a .cwd marker for
 * the cases where the encoding does not round-trip. */
static void
test_locate_grok(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *bucket = NULL;
	g_autofree gchar *marker = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *text = NULL;
	GHashTable       *environment;

	/* A bucket name that does not decode to the cwd, so only the .cwd
	 * marker can match it --- the case that exists because /home and
	 * /var/home name the same directory on this project's platform. */
	bucket = g_build_filename(fixture->root, ".grok", "sessions",
	                          "%2Fsomewhere%2Felse", NULL);
	directory = g_build_filename(bucket, "session-1", NULL);
	transcript = write_file(directory, "updates.jsonl",
	                        GROK_CHUNK("user_message_chunk", "FOUNDIT"));
	marker = write_file(bucket, ".cwd", fixture->cwd);

	environment = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);
	g_hash_table_insert(environment, g_strdup("HOME"),
	                    g_strdup(fixture->root));
	ai_cli_client_set_environment(AI_CLI_CLIENT(client), environment);
	g_hash_table_unref(environment);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client),
	                                    fixture->cwd);

	session = ai_cli_client_read_native_session(AI_CLI_CLIENT(client),
	                                            &error);
	g_assert_no_error(error);
	g_assert_nonnull(session);
	g_assert_cmpstr(ai_native_session_get_session_id(session), ==,
	                "session-1");

	text = flatten(session);
	g_assert_nonnull(g_strstr_len(text, -1, "FOUNDIT"));
}

/* codex files a rollout under sessions/YYYY/MM/DD, so the id alone does
 * not build a path and the tree has to be walked. */
static void
test_locate_codex(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiCodexCliClient) client = ai_codex_cli_client_new();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiNativeSession) session = NULL;
	g_autofree gchar *day = NULL;
	g_autofree gchar *transcript = NULL;
	g_autofree gchar *text = NULL;
	GHashTable       *environment;

	day = g_build_filename(fixture->root, ".codex", "sessions", "2026",
	                       "09", "08", NULL);
	transcript = write_file(day, "rollout-2026-09-08T10-00-00-abc123.jsonl",
		"{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
		"\"role\":\"user\",\"content\":"
		"[{\"type\":\"input_text\",\"text\":\"DEEPLYFILED\"}]}}\n");

	environment = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);
	g_hash_table_insert(environment, g_strdup("HOME"),
	                    g_strdup(fixture->root));
	ai_cli_client_set_environment(AI_CLI_CLIENT(client), environment);
	g_hash_table_unref(environment);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client),
	                                    fixture->cwd);

	session = ai_cli_client_read_native_session(AI_CLI_CLIENT(client),
	                                            &error);
	g_assert_no_error(error);
	g_assert_nonnull(session);
	g_assert_cmpstr(ai_native_session_get_session_id(session), ==,
	                "rollout-2026-09-08T10-00-00-abc123");

	text = flatten(session);
	g_assert_nonnull(g_strstr_len(text, -1, "DEEPLYFILED"));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

#define CASE(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	CASE("/ai-glib/native-session/claude/basic", test_claude_basic);
	CASE("/ai-glib/native-session/claude/tools", test_claude_tools);
	CASE("/ai-glib/native-session/claude/compaction",
	     test_claude_compaction);
	CASE("/ai-glib/native-session/claude/compaction-twice",
	     test_claude_compaction_twice);
	CASE("/ai-glib/native-session/claude/sidechain", test_claude_sidechain);

	CASE("/ai-glib/native-session/codex/basic", test_codex_basic);
	CASE("/ai-glib/native-session/codex/compaction", test_codex_compaction);

	CASE("/ai-glib/native-session/grok/chunks", test_grok_chunks);
	CASE("/ai-glib/native-session/grok/compaction", test_grok_compaction);

	CASE("/ai-glib/native-session/malformed", test_malformed);
	CASE("/ai-glib/native-session/invalid-utf8", test_invalid_utf8);
	CASE("/ai-glib/native-session/embedded-nul", test_embedded_nul);
	CASE("/ai-glib/native-session/missing-file", test_missing_file);
	CASE("/ai-glib/native-session/unsupported", test_unsupported_providers);

	CASE("/ai-glib/native-session/digest/trims-oldest",
	     test_digest_trims_oldest);
	CASE("/ai-glib/native-session/digest/empty", test_digest_empty);
	CASE("/ai-glib/native-session/digest/compaction",
	     test_digest_notes_compaction);
	CASE("/ai-glib/native-session/digest/large-tool-result",
	     test_large_tool_result);

	CASE("/ai-glib/native-session/locate/claude", test_locate_claude);
	CASE("/ai-glib/native-session/locate/nothing", test_locate_nothing);
	CASE("/ai-glib/native-session/locate/unsupported",
	     test_locate_unsupported);
	CASE("/ai-glib/native-session/locate/grok", test_locate_grok);
	CASE("/ai-glib/native-session/locate/codex", test_locate_codex);

#undef CASE

	return g_test_run();
}
