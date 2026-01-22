/*
 * test-opencode-client.c - Unit tests for AiOpenCodeClient
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-opencode-client.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "core/ai-config.h"

/*
 * Test that a new client can be created.
 */
static void
test_opencode_client_new(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;

	client = ai_opencode_client_new();
	g_assert_nonnull(client);
	g_assert_true(AI_IS_OPENCODE_CLIENT(client));
	g_assert_true(AI_IS_CLI_CLIENT(client));
}

/*
 * Test that the default model is correct.
 */
static void
test_opencode_client_default_model(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	const gchar *model;

	client = ai_opencode_client_new();
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_DEFAULT_MODEL);
}

/*
 * Test that the client implements the AiProvider interface.
 */
static void
test_opencode_client_provider_interface(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	AiProviderType type;
	const gchar *name;
	const gchar *model;

	client = ai_opencode_client_new();

	g_assert_true(AI_IS_PROVIDER(client));

	type = ai_provider_get_provider_type(AI_PROVIDER(client));
	g_assert_cmpint(type, ==, AI_PROVIDER_OPENCODE);

	name = ai_provider_get_name(AI_PROVIDER(client));
	g_assert_cmpstr(name, ==, "OpenCode");

	model = ai_provider_get_default_model(AI_PROVIDER(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_DEFAULT_MODEL);
}

/*
 * Test that the client implements the AiStreamable interface.
 */
static void
test_opencode_client_streamable_interface(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;

	client = ai_opencode_client_new();

	g_assert_true(AI_IS_STREAMABLE(client));
}

/*
 * Test model setting.
 */
static void
test_opencode_client_model(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	const gchar *model;

	client = ai_opencode_client_new();

	/* Default model */
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_DEFAULT_MODEL);

	/* Custom model - Anthropic */
	ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_CLAUDE_OPUS_4);
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_MODEL_CLAUDE_OPUS_4);

	/* Custom model - OpenAI */
	ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_GPT_4O);
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_MODEL_GPT_4O);

	/* Custom model - Google */
	ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_GEMINI_2_FLASH);
	model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_OPENCODE_MODEL_GEMINI_2_FLASH);
}

/*
 * Test executable path setting.
 */
static void
test_opencode_client_executable_path(void)
{
	g_autoptr(AiOpenCodeClient) client = NULL;
	const gchar *path;

	client = ai_opencode_client_new();

	/* Default path (NULL means search PATH) */
	path = ai_cli_client_get_executable_path(AI_CLI_CLIENT(client));
	g_assert_null(path);

	/* Set custom path */
	ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), "/usr/local/bin/opencode");
	path = ai_cli_client_get_executable_path(AI_CLI_CLIENT(client));
	g_assert_cmpstr(path, ==, "/usr/local/bin/opencode");
}

/*
 * Test GType registration.
 */
static void
test_opencode_client_gtype(void)
{
	GType type;

	type = ai_opencode_client_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiOpenCodeClient");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/opencode-client/new", test_opencode_client_new);
	g_test_add_func("/ai-glib/opencode-client/default-model", test_opencode_client_default_model);
	g_test_add_func("/ai-glib/opencode-client/provider-interface", test_opencode_client_provider_interface);
	g_test_add_func("/ai-glib/opencode-client/streamable-interface", test_opencode_client_streamable_interface);
	g_test_add_func("/ai-glib/opencode-client/model", test_opencode_client_model);
	g_test_add_func("/ai-glib/opencode-client/executable-path", test_opencode_client_executable_path);
	g_test_add_func("/ai-glib/opencode-client/gtype", test_opencode_client_gtype);

	return g_test_run();
}
