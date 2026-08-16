/*
 * test-cli-events.c - Per-provider NDJSON -> AiEvent translation
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Recorded lines in, asserted event sequence out. No network and no
 * subprocess: the shared read loop that feeds these parsers is covered in
 * test-cli-stream-reader.c, so what is left here is purely the translation
 * from each CLI's wire format.
 *
 * The malformed-input cases matter more than they look. GTest makes
 * criticals fatal, and json-glib's *_member_with_default() emit one when a
 * member is present but of another type -- so a CLI that changed a field
 * from a number to a string could abort a run rather than be ignored. Every
 * parser here reads through type-checked helpers, and these tests are what
 * hold that line.
 */

#include <glib.h>

#include "providers/ai-claude-code-client.h"
#include "providers/ai-grok-build-client.h"
#include "providers/ai-opencode-client.h"
#include "core/ai-cli-client.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-response.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	AiCliClient *client;
	AiResponse  *response;
	GPtrArray   *events;
	GError      *error;
	gboolean     ok;
} Parsed;

static Parsed *
parse_lines(AiCliClient *client, const gchar * const *lines)
{
	Parsed *p = g_new0(Parsed, 1);
	gsize i;

	p->client = client;
	p->response = ai_response_new("", "test-model");
	p->events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	p->ok = TRUE;

	for (i = 0; lines[i] != NULL; i++)
	{
		AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

		g_assert_nonnull(klass->parse_stream_events);

		if (!klass->parse_stream_events(client, lines[i], p->response,
		                                p->events, &p->error))
		{
			p->ok = FALSE;
			break;
		}
	}

	return p;
}

static Parsed *
parse_line(AiCliClient *client, const gchar *line)
{
	const gchar *lines[] = { line, NULL };
	return parse_lines(client, lines);
}

static void
parsed_free(Parsed *p)
{
	g_ptr_array_unref(p->events);
	g_clear_object(&p->response);
	g_clear_error(&p->error);
	g_free(p);
}

static guint
count_kind(Parsed *p, AiEventKind kind)
{
	guint i, n = 0;

	for (i = 0; i < p->events->len; i++)
		if (ai_event_get_kind(g_ptr_array_index(p->events, i)) == kind)
			n++;

	return n;
}

/* The first event of a kind, or NULL. */
static AiEvent *
first_of(Parsed *p, AiEventKind kind)
{
	guint i;

	for (i = 0; i < p->events->len; i++)
	{
		AiEvent *e = g_ptr_array_index(p->events, i);
		if (ai_event_get_kind(e) == kind)
			return e;
	}

	return NULL;
}

/* All TEXT_DELTA text concatenated -- what the answer would read as. */
static gchar *
all_text(Parsed *p)
{
	GString *acc = g_string_new(NULL);
	guint i;

	for (i = 0; i < p->events->len; i++)
	{
		AiEvent *e = g_ptr_array_index(p->events, i);

		if (ai_event_get_kind(e) == AI_EVENT_TEXT_DELTA &&
		    ai_event_get_text(e) != NULL)
			g_string_append(acc, ai_event_get_text(e));
	}

	return g_string_free(acc, FALSE);
}

/* ----------------------------------------------------------------
 * Malformed input -- run against every provider
 * ---------------------------------------------------------------- */

typedef AiCliClient *(*ClientCtor)(void);

static AiCliClient *make_claude_code(void)
{ return AI_CLI_CLIENT(ai_claude_code_client_new()); }

static AiCliClient *make_grok_build(void)
{ return AI_CLI_CLIENT(ai_grok_build_client_new()); }

static AiCliClient *make_opencode(void)
{ return AI_CLI_CLIENT(ai_opencode_client_new()); }

static const ClientCtor ALL_CTORS[] = {
	make_claude_code, make_grok_build, make_opencode
};

/*
 * Lines that must be tolerated in silence by every parser. A CLI is free to
 * print progress chatter, a partial write, or a field of an unexpected type;
 * none of that is the caller's problem and none of it may abort the run.
 */
static const gchar * const JUNK_LINES[] = {
	"",
	"   ",
	"\t",
	"not json at all",
	"[1, 2, 3]",                                  /* array, not object */
	"\"just a string\"",
	"42",
	"null",
	"{}",                                         /* no type */
	"{\"type\": \"nonesuch\"}",                   /* unknown type */
	"{\"type\": 7}",                              /* type is not a string */
	"{\"type\": \"result\", \"usage\": \"seven\"}",       /* usage not object */
	"{\"type\": \"result\", \"session_id\": 12345}",      /* id not a string */
	"{\"type\": \"result\", \"total_cost_usd\": \"free\"}",
	"{\"type\": \"assistant\", \"message\": \"flat\"}",   /* message not object */
	"{\"type\": \"assistant\", \"message\": {\"content\": \"str\"}}",
	"{\"type\": \"assistant\", \"message\": {\"content\": [null, 3, \"x\"]}}",
	"{\"type\": \"stream_event\", \"event\": 5}",
	"{\"type\": \"stream_event\", \"event\": {\"delta\": []}}",
	"{\"type\": \"text\", \"part\": []}",
	"{\"type\": \"tool_use\", \"part\": {\"state\": 9}}",
	"{\"type\": \"step_finish\", \"part\": {\"tokens\": \"lots\"}}",
	"{\"type\": \"user\", \"message\": {\"content\": [{\"type\": \"tool_result\"}]}}",
	"{\"type\": \"system\", \"subtype\": []}",
	"{\"truncated\": ",                           /* incomplete JSON */
	NULL
};

static void
test_junk_is_tolerated(void)
{
	gsize c;

	for (c = 0; c < G_N_ELEMENTS(ALL_CTORS); c++)
	{
		g_autoptr(GObject) client = G_OBJECT(ALL_CTORS[c]());
		gsize i;

		for (i = 0; JUNK_LINES[i] != NULL; i++)
		{
			Parsed *p = parse_line(AI_CLI_CLIENT(client), JUNK_LINES[i]);

			/*
			 * Tolerated means: no error, no crash, and no critical --
			 * the last of which GTest turns into a failure for us.
			 */
			if (!p->ok)
				g_error("%s rejected junk line %" G_GSIZE_FORMAT ": %s",
				        G_OBJECT_TYPE_NAME(client), i,
				        p->error != NULL ? p->error->message : "(no error)");

			parsed_free(p);
		}
	}
}

static void
test_junk_before_real_content(void)
{
	/* A parser that gave up on the first odd line would lose the answer. */
	gsize c;

	for (c = 0; c < G_N_ELEMENTS(ALL_CTORS); c++)
	{
		g_autoptr(GObject) client = G_OBJECT(ALL_CTORS[c]());
		const gchar *lines[] = {
			"garbage", "{}", "{\"type\": \"nope\"}", NULL
		};
		Parsed *p = parse_lines(AI_CLI_CLIENT(client), lines);

		g_assert_true(p->ok);
		g_assert_cmpuint(p->events->len, ==, 0);

		parsed_free(p);
	}
}

/* ----------------------------------------------------------------
 * claude-code
 * ---------------------------------------------------------------- */

static void
test_cc_content_array(void)
{
	/*
	 * The shape the CLI actually emits. Reading message.text instead once
	 * found nothing on every event, so a streamed reply arrived empty while
	 * the run reported success.
	 */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"assistant\",\"message\":{\"type\":\"message\",\"content\":["
		"{\"type\":\"text\",\"text\":\"Hello \"},"
		"{\"type\":\"text\",\"text\":\"world\"}]}}");
	g_autofree gchar *text = all_text(p);

	g_assert_true(p->ok);
	g_assert_cmpstr(text, ==, "Hello world");
	g_assert_cmpuint(count_kind(p, AI_EVENT_TEXT_DELTA), ==, 2);

	parsed_free(p);
}

static void
test_cc_flat_text_shape(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"assistant\",\"message\":{\"type\":\"text\",\"text\":\"flat\"}}");
	g_autofree gchar *text = all_text(p);

	g_assert_true(p->ok);
	g_assert_cmpstr(text, ==, "flat");

	parsed_free(p);
}

static void
test_cc_text_thinking_and_tool_together(void)
{
	/*
	 * All three used to be discarded except text, under a comment saying
	 * tool_use was "handled by the caller" -- no caller existed.
	 */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"assistant\",\"message\":{\"content\":["
		"{\"type\":\"thinking\",\"thinking\":\"let me look\"},"
		"{\"type\":\"text\",\"text\":\"Checking.\"},"
		"{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"Bash\","
		"\"input\":{\"command\":\"ls -la\"}}]}}");
	AiEvent *tool;
	AiEvent *thinking;

	g_assert_true(p->ok);
	g_assert_cmpuint(p->events->len, ==, 3);

	/* Order follows the content array. */
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(p->events, 0)), ==,
	                AI_EVENT_THINKING_DELTA);
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(p->events, 1)), ==,
	                AI_EVENT_TEXT_DELTA);
	g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(p->events, 2)), ==,
	                AI_EVENT_TOOL_STARTED);

	thinking = first_of(p, AI_EVENT_THINKING_DELTA);
	g_assert_cmpstr(ai_event_get_text(thinking), ==, "let me look");

	tool = first_of(p, AI_EVENT_TOOL_STARTED);
	g_assert_cmpstr(ai_event_get_tool_use_id(tool), ==, "toolu_01");
	g_assert_cmpstr(ai_tool_use_get_name(ai_event_get_tool_use(tool)), ==, "Bash");

	/* The arguments come through, which is what a tool summary needs. */
	{
		const gchar *cmd = ai_tool_use_get_input_string(
			ai_event_get_tool_use(tool), "command");
		g_assert_cmpstr(cmd, ==, "ls -la");
	}

	parsed_free(p);
}

static void
test_cc_thinking_is_not_the_answer(void)
{
	/* Reported, but never part of the assembled text. */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"assistant\",\"message\":{\"content\":["
		"{\"type\":\"thinking\",\"thinking\":\"secret reasoning\"},"
		"{\"type\":\"text\",\"text\":\"the answer\"}]}}");
	g_autofree gchar *text = all_text(p);

	g_assert_cmpstr(text, ==, "the answer");
	g_assert_cmpuint(count_kind(p, AI_EVENT_THINKING_DELTA), ==, 1);

	parsed_free(p);
}

static void
test_cc_tool_result_from_user_line(void)
{
	/*
	 * The CLI reports results on a "user" line, because that is how the
	 * transcript models a tool answering the model.
	 */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"user\",\"message\":{\"content\":["
		"{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_01\","
		"\"content\":\"a.c\\nb.c\"}]}}");
	AiEvent *e = first_of(p, AI_EVENT_TOOL_FINISHED);

	g_assert_true(p->ok);
	g_assert_nonnull(e);
	g_assert_cmpstr(ai_event_get_tool_use_id(e), ==, "toolu_01");
	g_assert_cmpstr(ai_tool_result_get_content(ai_event_get_tool_result(e)),
	                ==, "a.c\nb.c");
	g_assert_false(ai_tool_result_get_is_error(ai_event_get_tool_result(e)));

	parsed_free(p);
}

static void
test_cc_tool_result_block_array_content(void)
{
	/* content is either a plain string or an array of blocks. */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"user\",\"message\":{\"content\":["
		"{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_02\",\"content\":["
		"{\"type\":\"text\",\"text\":\"part one \"},"
		"{\"type\":\"text\",\"text\":\"part two\"}]}]}}");
	AiEvent *e = first_of(p, AI_EVENT_TOOL_FINISHED);

	g_assert_nonnull(e);
	g_assert_cmpstr(ai_tool_result_get_content(ai_event_get_tool_result(e)),
	                ==, "part one part two");

	parsed_free(p);
}

static void
test_cc_tool_result_error_flag(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"user\",\"message\":{\"content\":["
		"{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_03\","
		"\"content\":\"No such file\",\"is_error\":true}]}}");
	AiEvent *e = first_of(p, AI_EVENT_TOOL_FINISHED);

	g_assert_nonnull(e);
	g_assert_true(ai_tool_result_get_is_error(ai_event_get_tool_result(e)));

	parsed_free(p);
}

static void
test_cc_multiple_tool_results_one_line(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"user\",\"message\":{\"content\":["
		"{\"type\":\"tool_result\",\"tool_use_id\":\"a\",\"content\":\"1\"},"
		"{\"type\":\"tool_result\",\"tool_use_id\":\"b\",\"content\":\"2\"},"
		"{\"type\":\"tool_result\",\"tool_use_id\":\"c\",\"content\":\"3\"}]}}");

	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_FINISHED), ==, 3);

	parsed_free(p);
}

static void
test_cc_system_init_captures_session(void)
{
	/*
	 * The init line carries the session id well before the result line
	 * does, so a run interrupted mid-turn can still be resumed.
	 */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"sess-abc\"}");

	g_assert_true(p->ok);
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "sess-abc");
	g_assert_cmpuint(count_kind(p, AI_EVENT_STATUS), ==, 1);

	parsed_free(p);
}

static void
test_cc_session_persistence_off(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p;

	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);

	p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"result\",\"result\":\"x\",\"session_id\":\"sess-xyz\"}");

	g_assert_null(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)));

	parsed_free(p);
}

static void
test_cc_result_usage_and_cost(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"result\",\"result\":\"done\",\"session_id\":\"s1\","
		"\"usage\":{\"input_tokens\":120,\"output_tokens\":45},"
		"\"total_cost_usd\":0.0034}");
	AiEvent *e = first_of(p, AI_EVENT_USAGE);

	g_assert_nonnull(e);
	g_assert_cmpint(ai_usage_get_input_tokens(ai_event_get_usage(e)), ==, 120);
	g_assert_cmpint(ai_usage_get_output_tokens(ai_event_get_usage(e)), ==, 45);
	g_assert_cmpint(ai_event_get_cost_micros(e), ==, 3400);

	parsed_free(p);
}

static void
test_cc_result_without_cost_is_unpriced(void)
{
	/* -1, not 0: a missing cost is not a free turn. */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"result\",\"result\":\"done\","
		"\"usage\":{\"input_tokens\":1,\"output_tokens\":2}}");
	AiEvent *e = first_of(p, AI_EVENT_USAGE);

	g_assert_nonnull(e);
	g_assert_cmpint(ai_event_get_cost_micros(e), ==, -1);

	parsed_free(p);
}

static void
test_cc_partial_message_deltas(void)
{
	/* What --include-partial-messages buys: token-level text. */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	const gchar *lines[] = {
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"Hel\"}}}",
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"lo\"}}}",
		NULL
	};
	Parsed *p = parse_lines(AI_CLI_CLIENT(client), lines);
	g_autofree gchar *text = all_text(p);

	g_assert_cmpstr(text, ==, "Hello");

	parsed_free(p);
}

static void
test_cc_stream_event_tool_and_args(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	const gchar *lines[] = {
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\","
		"\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Edit\"}}}",
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"pa\"}}}",
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"th\\\":1}\"}}}",
		NULL
	};
	Parsed *p = parse_lines(AI_CLI_CLIENT(client), lines);
	AiEvent *started = first_of(p, AI_EVENT_TOOL_STARTED);
	GString *acc = g_string_new(NULL);
	guint i;

	g_assert_nonnull(started);
	g_assert_cmpstr(ai_event_get_tool_use_id(started), ==, "t1");
	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_INPUT_DELTA), ==, 2);

	/* The fragments only parse once concatenated -- that is the point. */
	for (i = 0; i < p->events->len; i++)
	{
		AiEvent *e = g_ptr_array_index(p->events, i);
		if (ai_event_get_kind(e) == AI_EVENT_TOOL_INPUT_DELTA)
			g_string_append(acc, ai_event_get_text(e));
	}
	g_assert_cmpstr(acc->str, ==, "{\"path\":1}");
	g_string_free(acc, TRUE);

	parsed_free(p);
}

static void
test_cc_thinking_delta_stream_event(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hmm\"}}}");
	AiEvent *e = first_of(p, AI_EVENT_THINKING_DELTA);

	g_assert_nonnull(e);
	g_assert_cmpstr(ai_event_get_text(e), ==, "hmm");

	parsed_free(p);
}

static void
test_cc_tool_use_without_name_ignored(void)
{
	/* A call nobody can name is not worth reporting as one. */
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"assistant\",\"message\":{\"content\":["
		"{\"type\":\"tool_use\",\"id\":\"t9\"}]}}");

	g_assert_true(p->ok);
	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_STARTED), ==, 0);

	parsed_free(p);
}

/* ----------------------------------------------------------------
 * grok-build
 * ---------------------------------------------------------------- */

static void
test_gb_text_delta(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}}");
	g_autofree gchar *text = all_text(p);

	g_assert_cmpstr(text, ==, "hi");

	parsed_free(p);
}

static void
test_gb_thinking_delta(void)
{
	/* Explicitly dropped before, on the grounds that it is not the answer. */
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"reasoning\"}}}");
	g_autofree gchar *text = all_text(p);
	AiEvent *e = first_of(p, AI_EVENT_THINKING_DELTA);

	g_assert_nonnull(e);
	g_assert_cmpstr(ai_event_get_text(e), ==, "reasoning");

	/* Still excluded from the answer. */
	g_assert_cmpstr(text, ==, "");

	parsed_free(p);
}

static void
test_gb_tool_start_then_complete_args(void)
{
	/*
	 * content_block_start knows the name but not the arguments; the whole
	 * assistant message is the only place the assembled input appears. Both
	 * emit TOOL_STARTED for the same id on purpose.
	 */
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	const gchar *lines[] = {
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\","
		"\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"bash\"}}}",
		"{\"type\":\"assistant\",\"message\":{\"content\":["
		"{\"type\":\"text\",\"text\":\"ignored, deltas already had it\"},"
		"{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"bash\","
		"\"input\":{\"command\":\"make\"}}]}}",
		NULL
	};
	Parsed *p = parse_lines(AI_CLI_CLIENT(client), lines);
	g_autofree gchar *text = all_text(p);
	AiEvent *last = NULL;
	guint i;

	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_STARTED), ==, 2);

	/* The assistant line's text is still ignored, or it would double. */
	g_assert_cmpstr(text, ==, "");

	for (i = 0; i < p->events->len; i++)
	{
		AiEvent *e = g_ptr_array_index(p->events, i);
		if (ai_event_get_kind(e) == AI_EVENT_TOOL_STARTED)
			last = e;
	}

	g_assert_cmpstr(ai_event_get_tool_use_id(last), ==, "t1");
	{
		const gchar *cmd = ai_tool_use_get_input_string(
			ai_event_get_tool_use(last), "command");
		g_assert_cmpstr(cmd, ==, "make");
	}

	parsed_free(p);
}

static void
test_gb_error_line_fails(void)
{
	/*
	 * grok prints this on stdout and exits 0, so the status is no help --
	 * the parser has to be the one that notices.
	 */
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"error\",\"message\":\"unknown reasoning effort\"}");

	g_assert_false(p->ok);
	g_assert_nonnull(p->error);
	g_assert_true(strstr(p->error->message, "unknown reasoning effort") != NULL);

	parsed_free(p);
}

static void
test_gb_result_is_error(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"result\",\"is_error\":true,\"result\":\"it broke\"}");

	g_assert_false(p->ok);
	g_assert_nonnull(p->error);

	parsed_free(p);
}

static void
test_gb_camel_and_snake_session(void)
{
	/*
	 * --output-format json speaks camelCase while the NDJSON result line
	 * speaks snake_case, and both reach this parser.
	 */
	g_autoptr(AiGrokBuildClient) camel = ai_grok_build_client_new();
	g_autoptr(AiGrokBuildClient) snake = ai_grok_build_client_new();
	Parsed *pc = parse_line(AI_CLI_CLIENT(camel),
		"{\"type\":\"result\",\"sessionId\":\"camel-1\",\"text\":\"a\"}");
	Parsed *ps = parse_line(AI_CLI_CLIENT(snake),
		"{\"type\":\"result\",\"session_id\":\"snake-1\",\"result\":\"a\"}");

	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(camel)),
	                ==, "camel-1");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(snake)),
	                ==, "snake-1");

	parsed_free(pc);
	parsed_free(ps);
}

static void
test_gb_usage_event(void)
{
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"result\",\"result\":\"x\","
		"\"usage\":{\"input_tokens\":10,\"output_tokens\":20},"
		"\"total_cost_usd\":0.5}");
	AiEvent *e = first_of(p, AI_EVENT_USAGE);

	g_assert_nonnull(e);
	g_assert_cmpint(ai_usage_get_total_tokens(ai_event_get_usage(e)), ==, 30);
	g_assert_cmpint(ai_event_get_cost_micros(e), ==, 500000);

	parsed_free(p);
}

static void
test_gb_content_block_stop_without_start(void)
{
	/* Out-of-order or truncated streams must not crash the parser. */
	g_autoptr(AiGrokBuildClient) client = ai_grok_build_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_stop\","
		"\"index\":3}}");

	g_assert_true(p->ok);
	g_assert_cmpuint(p->events->len, ==, 0);

	parsed_free(p);
}

/* ----------------------------------------------------------------
 * opencode
 * ---------------------------------------------------------------- */

static void
test_oc_text(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"text\",\"part\":{\"text\":\"hello\"}}");
	g_autofree gchar *text = all_text(p);

	g_assert_cmpstr(text, ==, "hello");

	parsed_free(p);
}

static void
test_oc_tool_completed(void)
{
	/*
	 * A part that arrives already completed yields both a start and a
	 * finish, so a consumer's state machine looks the same here as for the
	 * providers that really do announce a call before answering it.
	 */
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_use\",\"part\":{\"id\":\"c1\",\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\",\"input\":{\"command\":\"ls\"},"
		"\"output\":\"a.c\"}}}");
	AiEvent *started = first_of(p, AI_EVENT_TOOL_STARTED);
	AiEvent *finished = first_of(p, AI_EVENT_TOOL_FINISHED);

	g_assert_nonnull(started);
	g_assert_nonnull(finished);
	g_assert_cmpstr(ai_event_get_tool_use_id(started), ==, "c1");
	g_assert_cmpstr(ai_tool_use_get_name(ai_event_get_tool_use(started)),
	                ==, "bash");
	g_assert_cmpstr(ai_tool_result_get_content(
		ai_event_get_tool_result(finished)), ==, "a.c");
	g_assert_false(ai_tool_result_get_is_error(
		ai_event_get_tool_result(finished)));

	{
		const gchar *cmd = ai_tool_use_get_input_string(
			ai_event_get_tool_use(started), "command");
		g_assert_cmpstr(cmd, ==, "ls");
	}

	parsed_free(p);
}

static void
test_oc_tool_status_progression(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	const gchar *lines[] = {
		"{\"type\":\"tool_use\",\"part\":{\"id\":\"c1\",\"tool\":\"bash\","
		"\"state\":{\"status\":\"pending\"}}}",
		"{\"type\":\"tool_use\",\"part\":{\"id\":\"c1\",\"tool\":\"bash\","
		"\"state\":{\"status\":\"running\"}}}",
		"{\"type\":\"tool_use\",\"part\":{\"id\":\"c1\",\"tool\":\"bash\","
		"\"state\":{\"status\":\"completed\",\"output\":\"done\"}}}",
		NULL
	};
	Parsed *p = parse_lines(AI_CLI_CLIENT(client), lines);

	/* Three starts (one per part), one finish (only the completed one). */
	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_STARTED), ==, 3);
	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_FINISHED), ==, 1);

	parsed_free(p);
}

static void
test_oc_tool_error(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_use\",\"part\":{\"id\":\"c2\",\"tool\":\"bash\","
		"\"state\":{\"status\":\"error\",\"error\":\"permission denied\"}}}");
	AiEvent *e = first_of(p, AI_EVENT_TOOL_FINISHED);

	g_assert_nonnull(e);
	g_assert_true(ai_tool_result_get_is_error(ai_event_get_tool_result(e)));
	g_assert_cmpstr(ai_tool_result_get_content(ai_event_get_tool_result(e)),
	                ==, "permission denied");

	parsed_free(p);
}

static void
test_oc_tool_without_state(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_use\",\"part\":{\"id\":\"c3\",\"tool\":\"grep\"}}");

	g_assert_true(p->ok);
	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_STARTED), ==, 1);
	g_assert_cmpuint(count_kind(p, AI_EVENT_TOOL_FINISHED), ==, 0);

	parsed_free(p);
}

static void
test_oc_tool_unnamed_falls_back(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_use\",\"part\":{\"state\":{\"status\":\"running\"}}}");
	AiEvent *e = first_of(p, AI_EVENT_TOOL_STARTED);

	g_assert_nonnull(e);
	g_assert_cmpstr(ai_tool_use_get_name(ai_event_get_tool_use(e)), ==, "tool");

	parsed_free(p);
}

static void
test_oc_call_id_spelling(void)
{
	/* opencode has spelled the id both ways across versions. */
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"tool_use\",\"part\":{\"callID\":\"legacy-1\",\"tool\":\"ls\","
		"\"state\":{\"status\":\"running\"}}}");
	AiEvent *e = first_of(p, AI_EVENT_TOOL_STARTED);

	g_assert_cmpstr(ai_event_get_tool_use_id(e), ==, "legacy-1");

	parsed_free(p);
}

static void
test_oc_step_finish_usage(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":7,\"output\":9}}}");
	AiEvent *e = first_of(p, AI_EVENT_USAGE);

	g_assert_nonnull(e);
	g_assert_cmpint(ai_usage_get_input_tokens(ai_event_get_usage(e)), ==, 7);

	/* opencode reports no cost at all. */
	g_assert_cmpint(ai_event_get_cost_micros(e), ==, -1);

	parsed_free(p);
}

static void
test_oc_session_id_camel(void)
{
	g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
	Parsed *p = parse_line(AI_CLI_CLIENT(client),
		"{\"type\":\"text\",\"sessionID\":\"ses_9\",\"part\":{\"text\":\"x\"}}");

	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(client)),
	                ==, "ses_9");

	parsed_free(p);
}

/* ----------------------------------------------------------------
 * Cross-provider invariants
 * ---------------------------------------------------------------- */

static void
test_every_provider_implements_the_vfunc(void)
{
	gsize c;

	for (c = 0; c < G_N_ELEMENTS(ALL_CTORS); c++)
	{
		g_autoptr(GObject) client = G_OBJECT(ALL_CTORS[c]());
		AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);

		g_assert_nonnull(klass->parse_stream_events);

		/*
		 * The old contract is kept as a projection of the new one, so both
		 * must be present and must agree -- see below.
		 */
		g_assert_nonnull(klass->parse_stream_line);
	}
}

static void
test_line_and_events_agree(void)
{
	/*
	 * parse_stream_line is implemented over parse_stream_events, so the
	 * text it reports must be exactly the concatenated text deltas. If the
	 * two ever became separate implementations this is what would catch it.
	 */
	struct { ClientCtor ctor; const gchar *line; const gchar *expect; } cases[] = {
		{ make_claude_code,
		  "{\"type\":\"assistant\",\"message\":{\"content\":["
		  "{\"type\":\"thinking\",\"thinking\":\"no\"},"
		  "{\"type\":\"text\",\"text\":\"yes\"}]}}", "yes" },
		{ make_grok_build,
		  "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		  "\"delta\":{\"type\":\"text_delta\",\"text\":\"grok\"}}}", "grok" },
		{ make_opencode,
		  "{\"type\":\"text\",\"part\":{\"text\":\"oc\"}}", "oc" },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(GObject) client = G_OBJECT(cases[i].ctor());
		AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
		g_autoptr(AiResponse) response = ai_response_new("", "m");
		g_autofree gchar *delta = NULL;
		g_autoptr(GError) error = NULL;
		Parsed *p;
		g_autofree gchar *text = NULL;

		g_assert_true(klass->parse_stream_line(AI_CLI_CLIENT(client),
		                                       cases[i].line, response,
		                                       &delta, &error));
		g_assert_no_error(error);
		g_assert_cmpstr(delta, ==, cases[i].expect);

		p = parse_line(AI_CLI_CLIENT(client), cases[i].line);
		text = all_text(p);
		g_assert_cmpstr(text, ==, cases[i].expect);
		parsed_free(p);
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/cli-events/junk-tolerated", test_junk_is_tolerated);
	g_test_add_func("/ai-glib/cli-events/junk-before-content",
	                test_junk_before_real_content);

	g_test_add_func("/ai-glib/cli-events/cc/content-array", test_cc_content_array);
	g_test_add_func("/ai-glib/cli-events/cc/flat-text", test_cc_flat_text_shape);
	g_test_add_func("/ai-glib/cli-events/cc/text-thinking-tool",
	                test_cc_text_thinking_and_tool_together);
	g_test_add_func("/ai-glib/cli-events/cc/thinking-excluded",
	                test_cc_thinking_is_not_the_answer);
	g_test_add_func("/ai-glib/cli-events/cc/tool-result", test_cc_tool_result_from_user_line);
	g_test_add_func("/ai-glib/cli-events/cc/tool-result-array",
	                test_cc_tool_result_block_array_content);
	g_test_add_func("/ai-glib/cli-events/cc/tool-result-error",
	                test_cc_tool_result_error_flag);
	g_test_add_func("/ai-glib/cli-events/cc/tool-results-many",
	                test_cc_multiple_tool_results_one_line);
	g_test_add_func("/ai-glib/cli-events/cc/system-init",
	                test_cc_system_init_captures_session);
	g_test_add_func("/ai-glib/cli-events/cc/session-persistence-off",
	                test_cc_session_persistence_off);
	g_test_add_func("/ai-glib/cli-events/cc/usage-and-cost",
	                test_cc_result_usage_and_cost);
	g_test_add_func("/ai-glib/cli-events/cc/unpriced",
	                test_cc_result_without_cost_is_unpriced);
	g_test_add_func("/ai-glib/cli-events/cc/partial-messages",
	                test_cc_partial_message_deltas);
	g_test_add_func("/ai-glib/cli-events/cc/stream-tool-args",
	                test_cc_stream_event_tool_and_args);
	g_test_add_func("/ai-glib/cli-events/cc/thinking-delta",
	                test_cc_thinking_delta_stream_event);
	g_test_add_func("/ai-glib/cli-events/cc/tool-without-name",
	                test_cc_tool_use_without_name_ignored);

	g_test_add_func("/ai-glib/cli-events/gb/text", test_gb_text_delta);
	g_test_add_func("/ai-glib/cli-events/gb/thinking", test_gb_thinking_delta);
	g_test_add_func("/ai-glib/cli-events/gb/tool-args",
	                test_gb_tool_start_then_complete_args);
	g_test_add_func("/ai-glib/cli-events/gb/error-line", test_gb_error_line_fails);
	g_test_add_func("/ai-glib/cli-events/gb/result-is-error", test_gb_result_is_error);
	g_test_add_func("/ai-glib/cli-events/gb/camel-and-snake",
	                test_gb_camel_and_snake_session);
	g_test_add_func("/ai-glib/cli-events/gb/usage", test_gb_usage_event);
	g_test_add_func("/ai-glib/cli-events/gb/stop-without-start",
	                test_gb_content_block_stop_without_start);

	g_test_add_func("/ai-glib/cli-events/oc/text", test_oc_text);
	g_test_add_func("/ai-glib/cli-events/oc/tool-completed", test_oc_tool_completed);
	g_test_add_func("/ai-glib/cli-events/oc/tool-progression",
	                test_oc_tool_status_progression);
	g_test_add_func("/ai-glib/cli-events/oc/tool-error", test_oc_tool_error);
	g_test_add_func("/ai-glib/cli-events/oc/tool-no-state", test_oc_tool_without_state);
	g_test_add_func("/ai-glib/cli-events/oc/tool-unnamed",
	                test_oc_tool_unnamed_falls_back);
	g_test_add_func("/ai-glib/cli-events/oc/call-id-spelling", test_oc_call_id_spelling);
	g_test_add_func("/ai-glib/cli-events/oc/step-finish", test_oc_step_finish_usage);
	g_test_add_func("/ai-glib/cli-events/oc/session-id", test_oc_session_id_camel);

	g_test_add_func("/ai-glib/cli-events/all/vfunc-present",
	                test_every_provider_implements_the_vfunc);
	g_test_add_func("/ai-glib/cli-events/all/line-events-agree",
	                test_line_and_events_agree);

	return g_test_run();
}
