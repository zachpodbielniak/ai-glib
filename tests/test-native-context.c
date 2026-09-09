/*
 * test-native-context.c - Carrying a CLI's own history across a switch
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * tests/test-native-session.c covers reading a transcript.  This covers
 * what AiConversation does with one: the harvest on a provider switch,
 * the de-duplication against portable history, the bounds, and the ways
 * a harvest is allowed to fail without taking the switch with it.
 *
 * HOME and the working directory are sandboxed throughout, per CLAUDE.md.
 * Without that these read the developer's real ~/.claude and pass or fail
 * by whose machine ran them.
 */

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "ai-glib.h"

typedef struct
{
	gchar *root;
	gchar *cwd;
	gchar *projects;    /* the sandboxed ~/.claude/projects/<encoded> */
} Fixture;

/* claude replaces every separator, dot and underscore with a dash. */
static gchar *
encode_directory(const gchar *directory)
{
	gchar *encoded = g_strdup(directory);
	gsize  i;

	for (i = 0; encoded[i] != '\0'; i++)
	{
		if (encoded[i] == '/' || encoded[i] == '.' || encoded[i] == '_')
		{
			encoded[i] = '-';
		}
	}

	return encoded;
}

static void
fixture_set_up(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *encoded = NULL;

	fixture->root = g_dir_make_tmp("ai-carry-XXXXXX", NULL);
	g_assert_nonnull(fixture->root);

	fixture->cwd = g_build_filename(fixture->root, "project", NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->cwd, 0700), ==, 0);

	encoded = encode_directory(fixture->cwd);
	fixture->projects = g_build_filename(fixture->root, ".claude",
	                                     "projects", encoded, NULL);
	g_assert_cmpint(g_mkdir_with_parents(fixture->projects, 0700), ==, 0);
}

static void
fixture_tear_down(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *command = g_strdup_printf("rm -rf %s", fixture->root);

	g_spawn_command_line_sync(command, NULL, NULL, NULL, NULL);
	g_clear_pointer(&fixture->root, g_free);
	g_clear_pointer(&fixture->cwd, g_free);
	g_clear_pointer(&fixture->projects, g_free);
}

#define CLAUDE_USER(text) \
	"{\"type\":\"user\",\"message\":{\"role\":\"user\"," \
	"\"content\":[{\"type\":\"text\",\"text\":\"" text "\"}]}}\n"

#define CLAUDE_ASSISTANT(text) \
	"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\"," \
	"\"content\":[{\"type\":\"text\",\"text\":\"" text "\"}]}}\n"

static void
write_transcript(Fixture *fixture, const gchar *name, const gchar *text)
{
	g_autofree gchar *path = g_build_filename(fixture->projects, name, NULL);

	g_assert_true(g_file_set_contents(path, text, -1, NULL));
}

/* A claude-code client pointed entirely at the sandbox. */
static AiClaudeCodeClient *
sandboxed_client(Fixture *fixture)
{
	AiClaudeCodeClient *client = ai_claude_code_client_new();
	GHashTable         *environment;

	environment = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                    g_free);
	g_hash_table_insert(environment, g_strdup("HOME"),
	                    g_strdup(fixture->root));
	ai_cli_client_set_environment(AI_CLI_CLIENT(client), environment);
	g_hash_table_unref(environment);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client),
	                                    fixture->cwd);

	return client;
}

/*
 * The whole point.  ai-glib never saw the tool call, so a switch that
 * carried only its own AiMessage history would hand the next provider a
 * conversation with a hole in the middle of it.
 */
static void
test_carries_on_switch(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *carried;

	write_transcript(fixture, "s.jsonl",
		CLAUDE_USER("audit the parser and remember the word amber")
		"{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\","
		"\"content\":[{\"type\":\"tool_use\",\"id\":\"c1\","
		"\"name\":\"read_file\",\"input\":{\"path\":\"parser.c\"}}]}}\n"
		"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":"
		"[{\"type\":\"tool_result\",\"tool_use_id\":\"c1\","
		"\"content\":\"static int lex(void);\"}]}}\n"
		CLAUDE_ASSISTANT("The lexer is static."));

	conversation = ai_conversation_new(G_OBJECT(cli));
	g_assert_null(ai_conversation_get_carried_context(conversation));

	g_assert_true(ai_conversation_set_provider(conversation,
	                                           G_OBJECT(target), &error));
	g_assert_no_error(error);

	carried = ai_conversation_get_carried_context(conversation);
	g_assert_nonnull(carried);

	/* The prose, the tool call, and the tool's own output. */
	g_assert_nonnull(g_strstr_len(carried, -1, "amber"));
	g_assert_nonnull(g_strstr_len(carried, -1, "[Tool call: read_file"));
	g_assert_nonnull(g_strstr_len(carried, -1, "static int lex(void);"));
	g_assert_nonnull(g_strstr_len(carried, -1, "The lexer is static."));

	/* Named, so the receiving model knows this is carried over rather
	 * than something it said itself. */
	g_assert_nonnull(g_strstr_len(carried, -1, "claude-code"));
}

/*
 * The digest reaches the model.  Asserting only on the getter would pass
 * against an implementation that harvested perfectly and then never sent
 * it --- which is the failure that matters.
 */
static void
test_digest_reaches_the_provider(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	const gchar *seen;

	write_transcript(fixture, "s.jsonl", CLAUDE_USER("the word is amber"));

	conversation = ai_conversation_new(G_OBJECT(cli));
	ai_conversation_set_system_prompt(conversation, "Be terse.");
	ai_conversation_set_stream(conversation, FALSE);
	g_assert_true(ai_conversation_set_provider(conversation,
	                                           G_OBJECT(target), &error));
	g_assert_no_error(error);

	loop = g_main_loop_new(NULL, FALSE);
	ai_conversation_send_async(conversation, "what word?", NULL, NULL, NULL);

	/* The mock answers synchronously enough that one iteration settles
	 * it; spin until it is no longer busy rather than assuming. */
	while (ai_conversation_get_busy(conversation))
	{
		g_main_context_iteration(NULL, TRUE);
	}

	seen = ai_mock_provider_get_last_system_prompt(target);
	g_assert_nonnull(seen);
	g_assert_nonnull(g_strstr_len(seen, -1, "amber"));

	/*
	 * The host's own instruction comes last, so it keeps its force over a
	 * digest that may be tens of kilobytes.
	 */
	g_assert_nonnull(g_strstr_len(seen, -1, "Be terse."));
	g_assert_cmpint(g_strstr_len(seen, -1, "amber") - seen, <,
	                g_strstr_len(seen, -1, "Be terse.") - seen);

	/* And :system-prompt still reads back what the host set, not what
	 * ai-glib prepended to it. */
	g_assert_cmpstr(ai_conversation_get_system_prompt(conversation), ==,
	                "Be terse.");
}

/*
 * Sending an exchange as an AiMessage and again inside the digest reads
 * to the model as the user having asked the same thing twice.
 */
static void
test_excludes_portable_history(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *carried;
	const gchar *first;

	write_transcript(fixture, "s.jsonl",
		CLAUDE_USER("DUPLICATED question")
		CLAUDE_ASSISTANT("PRIVATEWORK the CLI did alone"));

	conversation = ai_conversation_new(G_OBJECT(cli));

	/*
	 * AiConversation is opaque, so the exclusion is asserted directly
	 * against the library entry point the conversation itself calls ---
	 * a turn cannot be recorded through the CLI provider without
	 * spawning it.
	 */
	{
		g_autoptr(AiNativeSession) session = NULL;
		g_autofree gchar *path = g_build_filename(fixture->projects,
		                                          "s.jsonl", NULL);
		g_autoptr(GError) read_error = NULL;
		g_autoptr(AiMessage) known =
			ai_message_new_user("DUPLICATED question");
		GList *exclude = g_list_append(NULL, known);
		g_autofree gchar *digest = NULL;

		session = ai_native_session_read_file("claude-code", path, NULL,
		                                      &read_error);
		g_assert_no_error(read_error);

		digest = ai_native_session_to_context_text_full(session, exclude, 0);
		g_assert_nonnull(digest);
		g_assert_null(g_strstr_len(digest, -1, "DUPLICATED"));
		g_assert_nonnull(g_strstr_len(digest, -1, "PRIVATEWORK"));

		g_list_free(exclude);
	}

	g_assert_true(ai_conversation_set_provider(conversation,
	                                           G_OBJECT(target), &error));
	g_assert_no_error(error);

	carried = ai_conversation_get_carried_context(conversation);
	g_assert_nonnull(carried);
	first = g_strstr_len(carried, -1, "PRIVATEWORK");
	g_assert_nonnull(first);
}

/* Opting out is a property, and it must actually stop the read. */
static void
test_opt_out(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GError) error = NULL;

	write_transcript(fixture, "s.jsonl", CLAUDE_USER("amber"));

	conversation = ai_conversation_new(G_OBJECT(cli));
	g_assert_true(ai_conversation_get_import_native_context(conversation));

	ai_conversation_set_import_native_context(conversation, FALSE);
	g_assert_false(ai_conversation_get_import_native_context(conversation));

	g_assert_true(ai_conversation_set_provider(conversation,
	                                           G_OBJECT(target), &error));
	g_assert_no_error(error);
	g_assert_null(ai_conversation_get_carried_context(conversation));
}

/*
 * A failed harvest must never fail the switch.  A provider with no
 * readable store, and a first turn that has not run yet, are both
 * ordinary --- refusing to change provider because a file was missing
 * would be a worse answer to either.
 */
static void
test_failure_never_blocks_the_switch(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(GError) error = NULL;

	/* No transcript written at all. */
	{
		g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
		g_autoptr(AiConversation) conversation =
			ai_conversation_new(G_OBJECT(cli));

		g_assert_true(ai_conversation_set_provider(conversation,
		                                           G_OBJECT(target),
		                                           &error));
		g_assert_no_error(error);
		g_assert_null(ai_conversation_get_carried_context(conversation));
	}

	/* A provider whose store this library cannot read. */
	{
		g_autoptr(AiCursorClient) cursor = ai_cursor_client_new();
		g_autoptr(AiConversation) conversation =
			ai_conversation_new(G_OBJECT(cursor));

		ai_cli_client_set_working_directory(AI_CLI_CLIENT(cursor),
		                                    fixture->cwd);

		g_assert_true(ai_conversation_set_provider(conversation,
		                                           G_OBJECT(target),
		                                           &error));
		g_assert_no_error(error);
		g_assert_null(ai_conversation_get_carried_context(conversation));
	}

	/* A transcript that is entirely garbage. */
	{
		g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
		g_autoptr(AiConversation) conversation = NULL;

		write_transcript(fixture, "s.jsonl", "}{ not json at all\n\n\n");
		conversation = ai_conversation_new(G_OBJECT(cli));

		g_assert_true(ai_conversation_set_provider(conversation,
		                                           G_OBJECT(target),
		                                           &error));
		g_assert_no_error(error);
		g_assert_null(ai_conversation_get_carried_context(conversation));
	}

	/* An HTTP provider on the way out has no native store and must not
	 * be probed for one. */
	{
		g_autoptr(AiMockProvider) http = ai_mock_provider_new();
		g_autoptr(AiConversation) conversation =
			ai_conversation_new(G_OBJECT(http));

		g_assert_true(ai_conversation_set_provider(conversation,
		                                           G_OBJECT(target),
		                                           &error));
		g_assert_no_error(error);
		g_assert_null(ai_conversation_get_carried_context(conversation));
	}
}

/*
 * Four switches at 64 KiB apiece is a system prompt larger than most
 * context windows, and the failure would arrive as the provider
 * rejecting a turn rather than as anything pointing here.
 */
static void
test_limit_bounds_the_total(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GString) transcript = g_string_new(NULL);
	const gchar *carried;
	gsize        i;

	{
		g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);

		conversation = ai_conversation_new(G_OBJECT(cli));

		/*
		 * The conversation pushes its own working directory onto a CLI
		 * target on every switch, so without this the second hop back
		 * clobbers the client's sandboxed directory with NULL and every
		 * later harvest silently finds nothing --- which would leave
		 * this asserting a bound against a single import.
		 */
		ai_conversation_set_working_directory(conversation, fixture->cwd);
		ai_conversation_set_native_context_limit(conversation, 2048);

		for (i = 0; i < 5; i++)
		{
			g_autoptr(GError) error = NULL;
			g_autofree gchar *filler = g_strnfill(1000, 'a' + (gchar)i);
			g_autofree gchar *record = NULL;

			/*
			 * The transcript grows between hops, as a real session
			 * does.  Re-reading an *unchanged* one is idempotent, so a
			 * fixed transcript would test the per-harvest cut and never
			 * the accumulation this bound exists for.
			 */
			record = g_strdup_printf(
				"{\"type\":\"user\",\"message\":{\"role\":\"user\","
				"\"content\":[{\"type\":\"text\",\"text\":\"%s\"}]}}\n",
				filler);
			g_string_append(transcript, record);
			write_transcript(fixture, "s.jsonl", transcript->str);

			g_assert_true(ai_conversation_set_provider(
				conversation, G_OBJECT(target), &error));
			g_assert_no_error(error);
			g_assert_true(ai_conversation_set_provider(
				conversation, G_OBJECT(cli), &error));
			g_assert_no_error(error);
		}
	}

	carried = ai_conversation_get_carried_context(conversation);
	g_assert_nonnull(carried);

	/* Five harvests of a growing transcript is well past 2048 unbounded;
	 * the accumulated total is still bounded, and still valid UTF-8,
	 * because it goes straight into a prompt and a trim that split a
	 * character would corrupt it. */
	g_assert_cmpuint(strlen(carried), <=, 2048 + 256);
	g_assert_true(g_utf8_validate(carried, -1, NULL));
	g_assert_nonnull(g_strstr_len(carried, -1, "trimmed"));

	/* Trimmed from the front: the newest hop's content survives. */
	g_assert_nonnull(g_strstr_len(carried, -1, "eee"));
}

/*
 * Toggling between two providers re-reads a transcript that has not
 * changed.  Appending it again on every hop would fill the prompt with
 * repetition of the same exchange.
 */
static void
test_repeat_harvest_is_idempotent(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autofree gchar *once = NULL;
	gsize i;

	write_transcript(fixture, "s.jsonl",
		CLAUDE_USER("remember the word amber"));

	conversation = ai_conversation_new(G_OBJECT(cli));
	ai_conversation_set_working_directory(conversation, fixture->cwd);
	ai_conversation_set_native_context_limit(conversation, 0);

	for (i = 0; i < 4; i++)
	{
		g_autoptr(GError) error = NULL;

		g_assert_true(ai_conversation_set_provider(conversation,
		                                           G_OBJECT(target),
		                                           &error));
		g_assert_no_error(error);

		if (once == NULL)
		{
			once = g_strdup(
				ai_conversation_get_carried_context(conversation));
			g_assert_nonnull(once);
		}

		g_assert_true(ai_conversation_set_provider(conversation,
		                                           G_OBJECT(cli), &error));
		g_assert_no_error(error);
	}

	/* Byte-identical to the first harvest: unchanged input, unchanged
	 * result, however many times the user toggled. */
	g_assert_cmpstr(ai_conversation_get_carried_context(conversation), ==,
	                once);
}

/* A limit of zero means no limit, and must not be read as "carry
 * nothing". */
static void
test_zero_limit_is_unbounded(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *filler = g_strnfill(200000, 'y');
	g_autofree gchar *body = g_strdup_printf(
		"{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":"
		"[{\"type\":\"text\",\"text\":\"%s\"}]}}\n", filler);

	write_transcript(fixture, "s.jsonl", body);

	conversation = ai_conversation_new(G_OBJECT(cli));
	ai_conversation_set_native_context_limit(conversation, 0);

	g_assert_true(ai_conversation_set_provider(conversation,
	                                           G_OBJECT(target), &error));
	g_assert_no_error(error);

	g_assert_cmpuint(
		strlen(ai_conversation_get_carried_context(conversation)), >,
		100000);
}

/* Clearing is the host's escape hatch when the imported history is
 * unwanted or too expensive to send on every turn. */
static void
test_clear(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	g_autoptr(GError) error = NULL;

	write_transcript(fixture, "s.jsonl", CLAUDE_USER("amber"));

	conversation = ai_conversation_new(G_OBJECT(cli));
	g_assert_true(ai_conversation_set_provider(conversation,
	                                           G_OBJECT(target), &error));
	g_assert_no_error(error);
	g_assert_nonnull(ai_conversation_get_carried_context(conversation));

	ai_conversation_clear_carried_context(conversation);
	g_assert_null(ai_conversation_get_carried_context(conversation));

	/* Idempotent: clearing twice is not a critical. */
	ai_conversation_clear_carried_context(conversation);
	g_assert_null(ai_conversation_get_carried_context(conversation));
}

/* A refused switch must not leave a digest behind for a provider the
 * conversation never moved to. */
static void
test_refused_switch_carries_nothing(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiMockProvider) target = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = NULL;
	AiAgentEndpoint endpoint;
	g_autoptr(GError) error = NULL;

	write_transcript(fixture, "s.jsonl", CLAUDE_USER("amber"));
	conversation = ai_conversation_new(G_OBJECT(cli));

	memset(&endpoint, 0, sizeof endpoint);
	endpoint.kind = (gchar *)AI_ENDPOINT_KIND_MCP_CONFIG;
	endpoint.value = (gchar *)"{}";

	if (!ai_conversation_set_tool_endpoint(conversation, &endpoint, &error))
	{
		/* The CLI declined the endpoint; nothing to test here. */
		g_test_skip("provider does not accept the test endpoint");
		return;
	}

	/* The mock is not an endpoint consumer, so the switch is refused. */
	g_clear_error(&error);
	g_assert_false(ai_conversation_set_provider(conversation,
	                                            G_OBJECT(target), &error));
	g_assert_nonnull(error);
	g_assert_null(ai_conversation_get_carried_context(conversation));
}

/* The properties are the binding surface: GObject syntax must reach the
 * same state the C accessors do. */
static void
test_properties(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) cli = sandboxed_client(fixture);
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(cli));
	gboolean          import = TRUE;
	guint             limit = 0;
	g_autofree gchar *carried = NULL;

	g_object_set(conversation, "import-native-context", FALSE,
	             "native-context-limit", 4096u, NULL);
	g_object_get(conversation, "import-native-context", &import,
	             "native-context-limit", &limit,
	             "carried-context", &carried, NULL);

	g_assert_false(import);
	g_assert_cmpuint(limit, ==, 4096);
	g_assert_null(carried);
	g_assert_false(ai_conversation_get_import_native_context(conversation));
	g_assert_cmpuint(
		ai_conversation_get_native_context_limit(conversation), ==, 4096);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

#define CASE(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	CASE("/ai-glib/native-context/carries-on-switch", test_carries_on_switch);
	CASE("/ai-glib/native-context/reaches-provider",
	     test_digest_reaches_the_provider);
	CASE("/ai-glib/native-context/excludes-portable",
	     test_excludes_portable_history);
	CASE("/ai-glib/native-context/opt-out", test_opt_out);
	CASE("/ai-glib/native-context/failure-never-blocks",
	     test_failure_never_blocks_the_switch);
	CASE("/ai-glib/native-context/limit-bounds-total",
	     test_limit_bounds_the_total);
	CASE("/ai-glib/native-context/repeat-harvest",
	     test_repeat_harvest_is_idempotent);
	CASE("/ai-glib/native-context/zero-limit", test_zero_limit_is_unbounded);
	CASE("/ai-glib/native-context/clear", test_clear);
	CASE("/ai-glib/native-context/refused-switch",
	     test_refused_switch_carries_nothing);
	CASE("/ai-glib/native-context/properties", test_properties);

#undef CASE

	return g_test_run();
}
