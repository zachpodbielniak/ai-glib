/*
 * test-claude-code-client.c - Unit tests for AiClaudeCodeClient
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-claude-code-client.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"

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

	persist = ai_cli_client_get_session_persistence(AI_CLI_CLIENT(client));
	g_assert_false(persist);

	/* Set session ID */
	ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "test-session-123");
	session_id = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));
	g_assert_cmpstr(session_id, ==, "test-session-123");

	/* Set persistence */
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

	return g_test_run();
}
