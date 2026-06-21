/*
 * test-claude-code-client.c - Unit tests for AiClaudeCodeClient
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-claude-code-client.h"
#include "providers/ai-claude-code-client-internal.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"
#include "model/ai-message.h"

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
	si = argv_index_of(argv, "--system-prompt");
	g_assert_cmpint(si, >, dd);
	g_assert_cmpstr(argv[si + 1], ==, "SYS");
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
	g_test_add_func("/ai-glib/claude-code-client/gtype", test_claude_code_client_gtype);

	g_test_add_func("/ai-glib/claude-code-client/build-argv-ollama",
	                test_claude_code_build_argv_ollama);
	g_test_add_func("/ai-glib/claude-code-client/build-argv-plain",
	                test_claude_code_build_argv_plain);
	g_test_add_func("/ai-glib/claude-code-client/build-argv-ollama-streaming",
	                test_claude_code_build_argv_ollama_streaming);
	g_test_add_func("/ai-glib/claude-code-client/build-argv-ollama-skip-system",
	                test_claude_code_build_argv_ollama_skip_and_system);

	return g_test_run();
}
