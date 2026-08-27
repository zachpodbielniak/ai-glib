/*
 * test-claude-code-client.c - Unit tests for AiClaudeCodeClient
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <locale.h>
#include <string.h>

#include "providers/ai-claude-code-client.h"
#include "providers/ai-claude-code-client-internal.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "core/ai-event.h"

/* ----------------------------------------------------------------
 * build_argv helpers (Ollama transport + regression locks)
 * ---------------------------------------------------------------- */

/* Return the index of @needle in NULL-terminated @argv, or -1. */
static gint
argv_index_of(gchar **argv, const gchar *needle)
{
	gint i;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			return i;
	return -1;
}

/* Count occurrences of @needle in NULL-terminated @argv. */
static gint
argv_count(gchar **argv, const gchar *needle)
{
	gint i, n = 0;
	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], needle) == 0)
			n++;
	return n;
}

/* Build argv for a single-user-message turn with the given model. */
static gchar **
build_argv_for_model(const gchar *model, const gchar *system,
                     gboolean streaming, gboolean skip_perms)
{
	AiClaudeCodeClient *client = ai_claude_code_client_new();
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	gchar **argv;

	if (model != NULL)
		ai_cli_client_set_model(AI_CLI_CLIENT(client), model);
	ai_claude_code_client_set_skip_permissions(client, skip_perms);

	argv = ai_claude_code_client_build_argv(
		AI_CLI_CLIENT(client), messages, system, 4096, streaming);

	g_list_free_full(messages, g_object_unref);
	g_object_unref(client);
	return argv;
}

/*
 * Ollama model: the argv is wrapped in `ollama launch claude --model
 * <suffix> --` and claude's own --model is omitted.
 */
static void
test_claude_code_build_argv_ollama(void)
{
	g_auto(GStrv) argv = build_argv_for_model("ollama/glm-5.2:cloud",
	                                          NULL, FALSE, FALSE);
	gint dd;

	g_assert_cmpstr(argv[0], ==, "ollama");
	g_assert_cmpstr(argv[1], ==, "launch");
	g_assert_cmpstr(argv[2], ==, "claude");
	g_assert_cmpstr(argv[3], ==, "--model");
	g_assert_cmpstr(argv[4], ==, "glm-5.2:cloud");
	g_assert_cmpstr(argv[5], ==, "--");
	g_assert_cmpstr(argv[6], ==, "--print");

	/* Exactly one --model (the ollama one); no claude --model after --. */
	g_assert_cmpint(argv_count(argv, "--model"), ==, 1);
	dd = argv_index_of(argv, "--");
	g_assert_cmpint(dd, ==, 5);
	g_assert_cmpint(argv_index_of(argv, "--print"), >, dd);
}

/*
 * Plain claude model: the historical behavior (regression lock) -- spawn
 * claude directly with its own --model, no ollama wrapper.
 */
static void
test_claude_code_build_argv_plain(void)
{
	g_auto(GStrv) argv = build_argv_for_model("sonnet", NULL, FALSE, FALSE);
	gint mi;

	g_assert_cmpstr(argv[0], ==, "claude");
	g_assert_cmpstr(argv[1], ==, "--print");
	/* No ollama wrapper. */
	g_assert_cmpint(argv_index_of(argv, "launch"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--"), ==, -1);
	/* claude --model sonnet present. */
	mi = argv_index_of(argv, "--model");
	g_assert_cmpint(mi, >=, 0);
	g_assert_cmpstr(argv[mi + 1], ==, "sonnet");
}

/* Ollama streaming: stream-json + --verbose ride after the --. */
static void
test_claude_code_build_argv_ollama_streaming(void)
{
	g_auto(GStrv) argv = build_argv_for_model("ollama/x", NULL, TRUE, FALSE);
	gint dd = argv_index_of(argv, "--");

	g_assert_cmpint(dd, ==, 5);
	g_assert_cmpint(argv_index_of(argv, "stream-json"), >, dd);
	g_assert_cmpint(argv_index_of(argv, "--verbose"), >, dd);
	g_assert_cmpint(argv_count(argv, "--model"), ==, 1);
}

/*
 * Ollama + skip-permissions + system prompt: all flags ride after the --,
 * still no claude --model.
 */
static void
test_claude_code_build_argv_ollama_skip_and_system(void)
{
	g_auto(GStrv) argv = build_argv_for_model("ollama/foo:bar", "SYS",
	                                          FALSE, TRUE);
	gint dd = argv_index_of(argv, "--");
	gint si;

	g_assert_cmpint(dd, ==, 5);
	g_assert_cmpint(argv_index_of(argv, "--dangerously-skip-permissions"),
	                >, dd);
	/*
	 * The prompt rides after the -- as a file, not as its own argv
	 * word -- see build-argv/no-word-reaches-the-arg-limit for why.
	 * Its contents are not read back here: build_argv_for_model()
	 * drops the client before returning, and the client takes its
	 * spill files with it.
	 */
	si = argv_index_of(argv, "--system-prompt-file");
	g_assert_cmpint(si, >, dd);
	g_assert_nonnull(argv[si + 1]);
	g_assert_cmpint(argv_index_of(argv, "--system-prompt"), ==, -1);
	g_assert_cmpint(argv_count(argv, "--model"), ==, 1);
}

/*
 * Test that a new client can be created.
 */
static void
test_claude_code_client_new(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;

	client = ai_claude_code_client_new();
	g_assert_nonnull(client);
	g_assert_true(AI_IS_CLAUDE_CODE_CLIENT(client));
	g_assert_true(AI_IS_CLI_CLIENT(client));
}

/*
 * Test that the default model is correct.
 */
static void
test_claude_code_client_default_model(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;
	const gchar *model;

	client = ai_claude_code_client_new();
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_CLAUDE_CODE_DEFAULT_MODEL);
}

/*
 * Test that the client implements the AiProvider interface.
 */
static void
test_claude_code_client_provider_interface(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;
	AiProviderType type;
	const gchar *name;
	const gchar *model;

	client = ai_claude_code_client_new();

	g_assert_true(AI_IS_PROVIDER(client));

	type = ai_provider_get_provider_type(AI_PROVIDER(client));
	g_assert_cmpint(type, ==, AI_PROVIDER_CLAUDE_CODE);

	name = ai_provider_get_name(AI_PROVIDER(client));
	g_assert_cmpstr(name, ==, "Claude Code");

	model = ai_provider_get_default_model(AI_PROVIDER(client));
	g_assert_cmpstr(model, ==, AI_CLAUDE_CODE_DEFAULT_MODEL);
}

/*
 * Test that the client implements the AiStreamable interface.
 */
static void
test_claude_code_client_streamable_interface(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;

	client = ai_claude_code_client_new();

	g_assert_true(AI_IS_STREAMABLE(client));
}

/*
 * Test model setting.
 */
static void
test_claude_code_client_model(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;
	const gchar *model;

	client = ai_claude_code_client_new();

	/* Default model */
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_CLAUDE_CODE_DEFAULT_MODEL);

	/* Custom model */
	ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_CLAUDE_CODE_MODEL_OPUS);
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_CLAUDE_CODE_MODEL_OPUS);
}

/*
 * Test session management.
 */
static void
test_claude_code_client_session_management(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;
	const gchar *session_id;
	gboolean persist;

	client = ai_claude_code_client_new();

	/* Default session state */
	session_id = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));
	g_assert_null(session_id);

	/* Default persistence is now TRUE */
	persist = ai_cli_client_get_session_persistence(AI_CLI_CLIENT(client));
	g_assert_true(persist);

	/* Set session ID */
	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "test-session-123");
	session_id = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));
	g_assert_cmpstr(session_id, ==, "test-session-123");

	/* Disable persistence */
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	persist = ai_cli_client_get_session_persistence(AI_CLI_CLIENT(client));
	g_assert_false(persist);

	/* Re-enable persistence */
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), TRUE);
	persist = ai_cli_client_get_session_persistence(AI_CLI_CLIENT(client));
	g_assert_true(persist);
}

/*
 * Test executable path setting.
 */
static void
test_claude_code_client_executable_path(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;
	const gchar *path;

	client = ai_claude_code_client_new();

	/* Default path (NULL means search PATH) */
	path = ai_cli_client_get_executable_path(AI_CLI_CLIENT(client));
	g_assert_null(path);

	/* Set custom path */
	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), "/usr/local/bin/claude");
	path = ai_cli_client_get_executable_path(AI_CLI_CLIENT(client));
	g_assert_cmpstr(path, ==, "/usr/local/bin/claude");
}

/*
 * Test total cost property.
 */
static void
test_claude_code_client_total_cost(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;
	gdouble cost;

	client = ai_claude_code_client_new();

	/* Default cost is 0 */
	cost = ai_claude_code_client_get_total_cost(client);
	g_assert_cmpfloat(cost, ==, 0.0);
}

/*
 * Test the inherited process-timeout-ms knob.  The default MUST be
 * non-zero: 0 means an unbounded g_subprocess_communicate wait, which
 * is the hang class that once froze a libreclaw session forever.
 */
static void
test_claude_code_client_process_timeout(void)
{
	g_autoptr(AiClaudeCodeClient) client = NULL;
	gint v;

	client = ai_claude_code_client_new();

	g_assert_cmpint(
		ai_cli_client_get_process_timeout_ms(AI_CLI_CLIENT(client)),
		==, 1800000);

	g_object_set(client, "process-timeout-ms", 60000, NULL);
	g_object_get(client, "process-timeout-ms", &v, NULL);
	g_assert_cmpint(v, ==, 60000);

	ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(client), 0);
	g_assert_cmpint(
		ai_cli_client_get_process_timeout_ms(AI_CLI_CLIENT(client)),
		==, 0);
}

/*
 * Test GType registration.
 */
static void
test_claude_code_client_gtype(void)
{
	GType type;

	type = ai_claude_code_client_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiClaudeCodeClient");
}

/*
 * The CLI emits an Anthropic message whose "type" is "message" and whose
 * text lives in a "content" array of blocks.  The parser used to read
 * message->text, which no event has, so every streamed reply arrived
 * empty while the run itself reported success.
 */
static void
test_claude_code_parse_stream_content_blocks(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(AiResponse) resp = ai_response_new("test", "test-model");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *line =
		"{\"type\":\"assistant\",\"message\":{\"type\":\"message\","
		"\"role\":\"assistant\",\"content\":["
		"{\"type\":\"thinking\",\"thinking\":\"deliberating\"},"
		"{\"type\":\"text\",\"text\":\"Hello\"},"
		"{\"type\":\"text\",\"text\":\", world\"}]}}";

	g_assert_nonnull(klass->parse_stream_line);
	g_assert_true(klass->parse_stream_line(AI_CLI_CLIENT(client), line,
	                                       resp, &delta, &error));
	g_assert_no_error(error);
	g_assert_nonnull(delta);
	/* Text blocks concatenated; the thinking block is not the answer. */
	g_assert_cmpstr(delta, ==, "Hello, world");
}

/* The older flat shape must keep working. */
static void
test_claude_code_parse_stream_flat_text(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(AiResponse) resp = ai_response_new("test", "test-model");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *line =
		"{\"type\":\"assistant\",\"message\":"
		"{\"type\":\"text\",\"text\":\"flat\"}}";

	g_assert_true(klass->parse_stream_line(AI_CLI_CLIENT(client), line,
	                                       resp, &delta, &error));
	g_assert_cmpstr(delta, ==, "flat");
}

/* A message carrying no text block yields no delta rather than "". */
static void
test_claude_code_parse_stream_no_text_block(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(client);
	g_autoptr(AiResponse) resp = ai_response_new("test", "test-model");
	g_autofree gchar *delta = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *line =
		"{\"type\":\"assistant\",\"message\":{\"type\":\"message\","
		"\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\"}]}}";

	g_assert_true(klass->parse_stream_line(AI_CLI_CLIENT(client), line,
	                                       resp, &delta, &error));
	g_assert_null(delta);
}


/* ----------------------------------------------------------------
 * build_argv: the rest of what `claude --print` accepts
 * ---------------------------------------------------------------- */

static const gchar *
argv_value_after(gchar **argv, const gchar *needle)
{
	gint i = argv_index_of(argv, needle);

	if (i < 0 || argv[i + 1] == NULL)
		return NULL;

	return argv[i + 1];
}

/* Build argv for a one-message turn against @client. */
static gchar **
build_argv_for(AiClaudeCodeClient *client, gboolean streaming)
{
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	gchar **argv;

	argv = ai_claude_code_client_build_argv(AI_CLI_CLIENT(client), messages,
	                                        NULL, 4096, streaming);

	g_list_free_full(messages, g_object_unref);
	return argv;
}

static void
test_cc_argv_agent_and_agents(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "agent", "reviewer",
	             "agents-json", "{\"r\":{\"description\":\"d\"}}",
	             NULL);
	argv = build_argv_for(client, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--agent"), ==, "reviewer");
	g_assert_cmpstr(argv_value_after(argv, "--agents"),
	                ==, "{\"r\":{\"description\":\"d\"}}");
}

/*
 * --append-system-prompt adds to the default prompt, where
 * AiCliClient:system-prompt replaces it. They are separate flags and must
 * both be able to appear.
 */
static void
test_cc_argv_append_system_prompt(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	g_auto(GStrv) argv = NULL;

	g_autofree gchar *system_read = NULL;
	g_autofree gchar *append_read = NULL;

	g_object_set(client, "append-system-prompt", "Be terse.", NULL);
	argv = ai_claude_code_client_build_argv(AI_CLI_CLIENT(client), messages,
	                                        "You are helpful.", 4096, FALSE);

	/*
	 * Both go in files rather than in argv, and they are still two
	 * separate flags -- which is the thing this test exists to hold.
	 */
	g_assert_true(g_file_get_contents(
		argv_value_after(argv, "--system-prompt-file"),
		&system_read, NULL, NULL));
	g_assert_cmpstr(system_read, ==, "You are helpful.");

	g_assert_true(g_file_get_contents(
		argv_value_after(argv, "--append-system-prompt-file"),
		&append_read, NULL, NULL));
	g_assert_cmpstr(append_read, ==, "Be terse.");

	g_list_free_full(messages, g_object_unref);
}

/* ----------------------------------------------------------------
 * The kernel's per-argument limit
 * ---------------------------------------------------------------- */

/*
 * MAX_ARG_STRLEN: the kernel caps a *single* argv word at 32 pages.
 *
 * It is not ARG_MAX (the total, megabytes here) and no headroom in the
 * total helps: execve refuses with E2BIG on the one long word. Linux
 * has hard-coded 32 * PAGE_SIZE since 2.6.23 and exports it to
 * userspace nowhere, so it is written out here rather than included.
 */
#define AI_TEST_MAX_ARG_STRLEN (32 * 4096)

/*
 * Comfortably past it, and past it by more than a rounding error, so a
 * fixture that stopped being oversized would be obvious rather than
 * marginal.
 */
#define AI_TEST_HUGE_PROMPT_BYTES (200 * 1024)

static gchar *
huge_prompt(gchar fill)
{
	GString *s = g_string_sized_new(AI_TEST_HUGE_PROMPT_BYTES + 1);

	/*
	 * Not a single repeated byte: the point is to read it back and
	 * know it is the prompt that was asked for, and a run of one
	 * character compares equal to a differently-truncated run of the
	 * same character.
	 */
	g_string_append_printf(s, "%c-BEGIN ", fill);
	while (s->len < AI_TEST_HUGE_PROMPT_BYTES)
		g_string_append_c(s, fill);
	g_string_append_printf(s, " END-%c", fill);

	return g_string_free(s, FALSE);
}

/*
 * No argv word may reach the kernel's per-argument limit -- and this
 * fixture has to reach it, or the test is vacuous.
 *
 * The bug: build_argv() emitted the system prompt as `--system-prompt
 * <text>`, one argv word. An agent's system prompt is assembled by
 * concatenating its identity files, so it grows with the product's own
 * output, and past 131,072 bytes g_subprocess fails E2BIG before claude
 * runs. The agent replies "Failed to execute child process (Argument
 * list too long)" and nothing else, for ever.
 *
 * The user prompt was moved to stdin for exactly this reason and the
 * comment saying so sits nine lines below where this one was: the rule
 * had been applied to the call site somebody noticed rather than to the
 * function.
 *
 * The first assertion is the one that matters. A fixture with a short
 * prompt passes the loop below in a build with the bug still in it, so
 * asserting that the prompts are genuinely oversized is what makes the
 * rest evidence.
 */
static void
test_cc_argv_no_word_reaches_the_arg_limit(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_autofree gchar *system = huge_prompt('S');
	g_autofree gchar *append = huge_prompt('A');
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	g_auto(GStrv) argv = NULL;
	const gchar *system_path;
	const gchar *append_path;
	g_autofree gchar *system_read = NULL;
	g_autofree gchar *append_read = NULL;
	GStatBuf st;
	gint i;

	/* The fixture reaches the limit. Without this the loop proves nothing. */
	g_assert_cmpuint(strlen(system), >=, AI_TEST_MAX_ARG_STRLEN);
	g_assert_cmpuint(strlen(append), >=, AI_TEST_MAX_ARG_STRLEN);

	g_object_set(client, "append-system-prompt", append, NULL);
	argv = ai_claude_code_client_build_argv(AI_CLI_CLIENT(client), messages,
	                                        system, 4096, FALSE);
	g_assert_nonnull(argv);

	for (i = 0; argv[i] != NULL; i++) {
		if (strlen(argv[i]) >= AI_TEST_MAX_ARG_STRLEN)
			g_error("argv[%d] is %" G_GSIZE_FORMAT " bytes, at or "
			        "over the kernel's %d-byte limit for one "
			        "argument", i, strlen(argv[i]),
			        AI_TEST_MAX_ARG_STRLEN);
	}

	/*
	 * And the prompt still reaches claude. Staying under the limit by
	 * dropping the prompt would pass the loop above and start every
	 * agent with no identity at all, which is worse than not starting.
	 */
	g_assert_cmpint(argv_index_of(argv, "--system-prompt"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--append-system-prompt"), ==, -1);

	system_path = argv_value_after(argv, "--system-prompt-file");
	append_path = argv_value_after(argv, "--append-system-prompt-file");
	g_assert_nonnull(system_path);
	g_assert_nonnull(append_path);

	g_assert_true(g_file_get_contents(system_path, &system_read, NULL, NULL));
	g_assert_cmpstr(system_read, ==, system);
	g_assert_true(g_file_get_contents(append_path, &append_read, NULL, NULL));
	g_assert_cmpstr(append_read, ==, append);

	/*
	 * Readable by its owner and nobody else. It holds the agent's whole
	 * identity, and it sits in a world-readable directory.
	 */
	g_assert_cmpint(g_stat(system_path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpint(g_stat(append_path, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);

	g_list_free_full(messages, g_object_unref);
}

/*
 * The spill files do not outlive the client that made them.
 *
 * They hold the agent's identity and they live in the temporary
 * directory, so a daemon running a fleet for weeks must not leave one
 * behind per agent per restart.
 */
static void
test_cc_argv_prompt_files_go_with_the_client(void)
{
	AiClaudeCodeClient *client = ai_claude_code_client_new();
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *system_path = NULL;
	g_autofree gchar *append_path = NULL;

	g_object_set(client, "append-system-prompt", "Be terse.", NULL);
	argv = ai_claude_code_client_build_argv(AI_CLI_CLIENT(client), messages,
	                                        "You are helpful.", 4096, FALSE);

	system_path = g_strdup(argv_value_after(argv, "--system-prompt-file"));
	append_path = g_strdup(argv_value_after(argv,
	                                        "--append-system-prompt-file"));
	g_assert_nonnull(system_path);
	g_assert_nonnull(append_path);

	/* Present while the client is: the positive half of the assertion. */
	g_assert_true(g_file_test(system_path, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(append_path, G_FILE_TEST_EXISTS));

	g_object_unref(client);

	g_assert_false(g_file_test(system_path, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(append_path, G_FILE_TEST_EXISTS));

	g_list_free_full(messages, g_object_unref);
}

/*
 * A second turn does not leave the first turn's file behind.
 *
 * One client serves an agent for its whole life, so "cleaned up at
 * finalize" alone would accumulate one file per turn on a process that
 * never exits -- which is every daemon this library is used from.
 */
static void
test_cc_argv_prompt_files_do_not_accumulate(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	g_auto(GStrv) first = NULL;
	g_auto(GStrv) second = NULL;
	g_autofree gchar *first_path = NULL;

	first = ai_claude_code_client_build_argv(AI_CLI_CLIENT(client), messages,
	                                         "One.", 4096, FALSE);
	first_path = g_strdup(argv_value_after(first, "--system-prompt-file"));
	g_assert_nonnull(first_path);
	g_assert_true(g_file_test(first_path, G_FILE_TEST_EXISTS));

	second = ai_claude_code_client_build_argv(AI_CLI_CLIENT(client), messages,
	                                          "Two.", 4096, FALSE);
	g_assert_nonnull(argv_value_after(second, "--system-prompt-file"));

	g_assert_false(g_file_test(first_path, G_FILE_TEST_EXISTS));

	g_list_free_full(messages, g_object_unref);
}

static void
test_cc_argv_fallback_and_schema(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "fallback-model", "haiku,sonnet",
	             "json-schema", "{\"type\":\"object\"}",
	             NULL);
	argv = build_argv_for(client, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--fallback-model"),
	                ==, "haiku,sonnet");
	g_assert_cmpstr(argv_value_after(argv, "--json-schema"),
	                ==, "{\"type\":\"object\"}");
}

/*
 * The budget must be spelled with a dot regardless of locale: claude
 * parses it as a number, and a locale that formats 1.25 as "1,25" would
 * produce a flag it rejects.
 */
static void
test_cc_argv_max_budget_locale_independent(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;
	const gchar *value;

	/* Best-effort: if the locale is unavailable the C locale still
	 * exercises the formatting path. */
	setlocale(LC_NUMERIC, "de_DE.UTF-8");

	g_object_set(client, "max-budget-usd", 1.25, NULL);
	argv = build_argv_for(client, FALSE);

	value = argv_value_after(argv, "--max-budget-usd");
	g_assert_nonnull(value);
	g_assert_nonnull(strchr(value, '.'));
	g_assert_null(strchr(value, ','));

	setlocale(LC_NUMERIC, "C");
}

/* Zero means "no ceiling", not "--max-budget-usd 0". */
static void
test_cc_argv_max_budget_zero(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = build_argv_for(client, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--max-budget-usd"), ==, -1);
}

static void
test_cc_argv_settings_and_sources(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "settings", "/etc/claude/settings.json",
	             "setting-sources", "user,project",
	             "strict-mcp-config", TRUE,
	             NULL);
	argv = build_argv_for(client, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--settings"),
	                ==, "/etc/claude/settings.json");
	g_assert_cmpstr(argv_value_after(argv, "--setting-sources"),
	                ==, "user,project");
	g_assert_cmpint(argv_index_of(argv, "--strict-mcp-config"), >, 0);
}

/* --tools and --betas take each item as its own argv word. */
static void
test_cc_argv_tools_and_betas(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;
	gint ti;

	g_object_set(client,
	             "tools", "Bash,Edit,Read",
	             "betas", "beta-one,beta-two",
	             NULL);
	argv = build_argv_for(client, FALSE);

	ti = argv_index_of(argv, "--tools");
	g_assert_cmpint(ti, >, 0);
	g_assert_cmpstr(argv[ti + 1], ==, "Bash");
	g_assert_cmpstr(argv[ti + 2], ==, "Edit");
	g_assert_cmpstr(argv[ti + 3], ==, "Read");
	g_assert_cmpint(argv_count(argv, "--tools"), ==, 1);

	g_assert_cmpint(argv_index_of(argv, "beta-one"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "beta-two"), >, 0);
}

/* Plugins accumulate: one flag per path rather than one list. */
static void
test_cc_argv_plugins(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "plugin-dirs", "/opt/a, /opt/b",
	             "plugin-urls", "https://example.invalid/p.zip",
	             NULL);
	argv = build_argv_for(client, FALSE);

	g_assert_cmpint(argv_count(argv, "--plugin-dir"), ==, 2);
	g_assert_cmpint(argv_index_of(argv, "/opt/a"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "/opt/b"), >, 0);
	g_assert_cmpint(argv_count(argv, "--plugin-url"), ==, 1);
}

static void
test_cc_argv_isolation_switches(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "bare", TRUE,
	             "safe-mode", TRUE,
	             "disable-slash-commands", TRUE,
	             "exclude-dynamic-system-prompt-sections", TRUE,
	             "autocompact", "auto",
	             NULL);
	argv = build_argv_for(client, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--bare"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--safe-mode"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--disable-slash-commands"), >, 0);
	g_assert_cmpint(argv_index_of(argv,
	                "--exclude-dynamic-system-prompt-sections"), >, 0);
	g_assert_cmpstr(argv_value_after(argv, "--autocompact"), ==, "auto");
}

/* --debug takes an optional filter; a filter alone is enough to enable it. */
static void
test_cc_argv_debug(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "debug", TRUE, NULL);
	argv = build_argv_for(client, FALSE);
	g_assert_cmpint(argv_index_of(argv, "--debug"), >, 0);
	/* No filter, so --debug must not swallow the next flag's value. */
	g_assert_cmpint(argv_index_of(argv, "--model"), >, 0);
	g_strfreev(g_steal_pointer(&argv));

	g_object_set(client, "debug-filter", "api,hooks",
	                     "debug-file", "/tmp/claude.log", NULL);
	argv = build_argv_for(client, FALSE);
	g_assert_cmpstr(argv_value_after(argv, "--debug"), ==, "api,hooks");
	g_assert_cmpstr(argv_value_after(argv, "--debug-file"),
	                ==, "/tmp/claude.log");
}

/*
 * These three are only valid with stream-json, and claude rejects them
 * elsewhere, so they are gated on the output format rather than left to
 * the caller.
 */
static void
test_cc_argv_stream_only_flags(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client,
	             "include-partial-messages", TRUE,
	             "include-hook-events", TRUE,
	             "forward-subagent-text", TRUE,
	             NULL);

	argv = build_argv_for(client, FALSE);
	g_assert_cmpint(argv_index_of(argv, "--include-partial-messages"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--include-hook-events"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--forward-subagent-text"), ==, -1);
	g_strfreev(g_steal_pointer(&argv));

	argv = build_argv_for(client, TRUE);
	g_assert_cmpint(argv_index_of(argv, "--include-partial-messages"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--include-hook-events"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--forward-subagent-text"), >, 0);
}

/* --fork-session is only accepted alongside --resume. */
static void
test_cc_argv_fork_session(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "fork-session", TRUE, NULL);

	argv = build_argv_for(client, FALSE);
	g_assert_cmpint(argv_index_of(argv, "--fork-session"), ==, -1);
	g_strfreev(g_steal_pointer(&argv));

	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-1");
	argv = build_argv_for(client, FALSE);
	g_assert_cmpint(argv_index_of(argv, "--resume"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--fork-session"), >, 0);
}

/*
 * --continue picks up the most recent conversation in the directory when
 * there is no id to resume. The system prompt is not re-sent: the
 * conversation already carries it, same as on a --resume.
 */
static void
test_cc_argv_continue(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	AiMessage *msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	argv = ai_claude_code_client_build_argv(AI_CLI_CLIENT(client), messages,
	                                        "SYS", 4096, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--continue"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--system-prompt"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--resume"), ==, -1);

	g_list_free_full(messages, g_object_unref);
}

/* An explicit session id names a different conversation, and wins. */
static void
test_cc_argv_continue_yields_to_session_id(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "sess-3");
	argv = build_argv_for(client, FALSE);

	g_assert_cmpstr(argv_value_after(argv, "--resume"), ==, "sess-3");
	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
}

/* claude accepts --fork-session with --continue as well as with --resume. */
static void
test_cc_argv_continue_with_fork(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, "fork-session", TRUE, NULL);
	argv = build_argv_for(client, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--continue"), >, 0);
	g_assert_cmpint(argv_index_of(argv, "--fork-session"), >, 0);
}

/* Persistence off asks for a fresh conversation every time. */
static void
test_cc_argv_continue_needs_persistence(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = NULL;

	g_object_set(client, "continue-session", TRUE, NULL);
	ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), FALSE);
	argv = build_argv_for(client, FALSE);

	g_assert_cmpint(argv_index_of(argv, "--continue"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--no-session-persistence"), >, 0);
}

/* An unconfigured client must still build the command it always did. */
static void
test_cc_argv_defaults_unchanged(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_auto(GStrv) argv = build_argv_for(client, FALSE);

	g_assert_cmpstr(argv[0], ==, "claude");
	g_assert_cmpstr(argv[1], ==, "--print");
	g_assert_cmpstr(argv_value_after(argv, "--output-format"), ==, "json");
	g_assert_cmpstr(argv_value_after(argv, "--model"), ==, "sonnet");
	/* Nothing new leaks into the default command line. */
	g_assert_cmpint(argv_index_of(argv, "--agent"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--bare"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--tools"), ==, -1);
	g_assert_cmpint(argv_index_of(argv, "--debug"), ==, -1);
}

static void
test_cc_property_round_trip(void)
{
	g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
	g_autofree gchar *agent = NULL;
	g_autofree gchar *tools = NULL;
	gdouble budget = 0.0;
	gboolean bare = FALSE;

	g_object_set(client,
	             "agent", "reviewer",
	             "tools", "Bash",
	             "max-budget-usd", 2.5,
	             "bare", TRUE,
	             NULL);

	g_object_get(client,
	             "agent", &agent,
	             "tools", &tools,
	             "max-budget-usd", &budget,
	             "bare", &bare,
	             NULL);

	g_assert_cmpstr(agent, ==, "reviewer");
	g_assert_cmpstr(tools, ==, "Bash");
	g_assert_cmpfloat(budget, ==, 2.5);
	g_assert_true(bare);
}


/* ----------------------------------------------------------------
 * result-event cost
 * ---------------------------------------------------------------- */

/*
 * Feed one NDJSON line through the client's own stream parser.
 *
 * Through the vfunc rather than a copy of the parsing, so this asserts
 * on what the CLI pipeline actually runs.
 */
static void
parse_one_line(AiClaudeCodeClient *client, const gchar *line,
               AiResponse *response)
{
	AiCliClientClass *cli_class = AI_CLI_CLIENT_GET_CLASS(client);
	GPtrArray *events = g_ptr_array_new_with_free_func(
		(GDestroyNotify)ai_event_unref);
	GError *error = NULL;

	g_assert_nonnull(cli_class->parse_stream_events);
	cli_class->parse_stream_events(AI_CLI_CLIENT(client), line, response,
	                               events, &error);
	g_assert_no_error(error);
	g_ptr_array_unref(events);
}

/*
 * The CLI states what the turn cost; that figure reaches the response.
 *
 * It cannot be recomputed from the token counts beside it. Claude Code
 * bills cache reads, which appear in neither input_tokens nor
 * output_tokens -- the numbers below are a real shape, where 8 input
 * tokens accompany a bill of two cents. Anything pricing 8 and 1555
 * through a rate card lands nowhere near it.
 */
static void
test_result_cost_is_the_cli_figure(void)
{
	AiClaudeCodeClient *client = ai_claude_code_client_new();
	AiResponse *response = ai_response_new("id", "sonnet");
	const gchar *line =
		"{\"type\":\"result\",\"result\":\"done\","
		"\"session_id\":\"s1\",\"total_cost_usd\":0.0204,"
		"\"usage\":{\"input_tokens\":8,\"output_tokens\":1555,"
		"\"cache_read_input_tokens\":41233}}";

	parse_one_line(client, line, response);

	g_assert_cmpint(ai_response_get_cost_micros(response), ==, 20400);

	g_object_unref(response);
	g_object_unref(client);
}

/*
 * A turn the provider did not price reads as unknown, not as free.
 *
 * Zero would be indistinguishable from a turn that genuinely cost
 * nothing, and a caller summing costs would quietly report a fleet
 * spending nothing at all.
 */
static void
test_result_without_cost_is_unknown(void)
{
	AiClaudeCodeClient *client = ai_claude_code_client_new();
	AiResponse *response = ai_response_new("id", "sonnet");
	const gchar *line =
		"{\"type\":\"result\",\"result\":\"done\","
		"\"usage\":{\"input_tokens\":8,\"output_tokens\":12}}";

	parse_one_line(client, line, response);

	g_assert_cmpint(ai_response_get_cost_micros(response), ==, -1);

	g_object_unref(response);
	g_object_unref(client);
}


int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/claude-code-client/new", test_claude_code_client_new);
	g_test_add_func("/ai-glib/claude-code-client/default-model", test_claude_code_client_default_model);
	g_test_add_func("/ai-glib/claude-code-client/provider-interface", test_claude_code_client_provider_interface);
	g_test_add_func("/ai-glib/claude-code-client/streamable-interface", test_claude_code_client_streamable_interface);
	g_test_add_func("/ai-glib/claude-code-client/model", test_claude_code_client_model);
	g_test_add_func("/ai-glib/claude-code-client/session-management", test_claude_code_client_session_management);
	g_test_add_func("/ai-glib/claude-code-client/executable-path", test_claude_code_client_executable_path);
	g_test_add_func("/ai-glib/claude-code-client/total-cost", test_claude_code_client_total_cost);
	g_test_add_func("/ai-glib/claude-code-client/process-timeout", test_claude_code_client_process_timeout);
	g_test_add_func("/ai-glib/claude-code-client/gtype", test_claude_code_client_gtype);

	g_test_add_func("/ai-glib/claude-code-client/build-argv-ollama",
	                test_claude_code_build_argv_ollama);
	g_test_add_func("/ai-glib/claude-code-client/build-argv-plain",
	                test_claude_code_build_argv_plain);
	g_test_add_func("/ai-glib/claude-code-client/build-argv-ollama-streaming",
	                test_claude_code_build_argv_ollama_streaming);
	g_test_add_func("/ai-glib/claude-code-client/build-argv-ollama-skip-system",
	                test_claude_code_build_argv_ollama_skip_and_system);

	g_test_add_func("/ai-glib/claude-code-client/build-argv/agent-and-agents",
	                test_cc_argv_agent_and_agents);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/append-system-prompt",
	                test_cc_argv_append_system_prompt);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/no-word-reaches-the-arg-limit",
	                test_cc_argv_no_word_reaches_the_arg_limit);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/prompt-files-go-with-the-client",
	                test_cc_argv_prompt_files_go_with_the_client);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/prompt-files-do-not-accumulate",
	                test_cc_argv_prompt_files_do_not_accumulate);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/fallback-and-schema",
	                test_cc_argv_fallback_and_schema);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/max-budget-locale",
	                test_cc_argv_max_budget_locale_independent);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/max-budget-zero",
	                test_cc_argv_max_budget_zero);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/settings",
	                test_cc_argv_settings_and_sources);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/tools-and-betas",
	                test_cc_argv_tools_and_betas);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/plugins",
	                test_cc_argv_plugins);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/isolation-switches",
	                test_cc_argv_isolation_switches);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/debug",
	                test_cc_argv_debug);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/stream-only-flags",
	                test_cc_argv_stream_only_flags);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/fork-session",
	                test_cc_argv_fork_session);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/continue",
	                test_cc_argv_continue);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/continue-yields-to-id",
	                test_cc_argv_continue_yields_to_session_id);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/continue-with-fork",
	                test_cc_argv_continue_with_fork);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/continue-needs-persistence",
	                test_cc_argv_continue_needs_persistence);
	g_test_add_func("/ai-glib/claude-code-client/build-argv/defaults-unchanged",
	                test_cc_argv_defaults_unchanged);
	g_test_add_func("/ai-glib/claude-code-client/property-round-trip",
	                test_cc_property_round_trip);

	g_test_add_func("/ai-glib/claude-code-client/parse-stream-content-blocks",
	                test_claude_code_parse_stream_content_blocks);
	g_test_add_func("/ai-glib/claude-code-client/parse-stream-flat-text",
	                test_claude_code_parse_stream_flat_text);
	g_test_add_func("/ai-glib/claude-code-client/parse-stream-no-text-block",
	                test_claude_code_parse_stream_no_text_block);
	g_test_add_func("/ai-glib/claude-code-client/result-cost-is-the-cli-figure",
	                test_result_cost_is_the_cli_figure);
	g_test_add_func("/ai-glib/claude-code-client/result-without-cost-is-unknown",
	                test_result_without_cost_is_unknown);

	return g_test_run();
}
