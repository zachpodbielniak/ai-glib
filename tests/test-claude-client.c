/*
 * test-claude-client.c - Unit tests for AiClaudeClient
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-claude-client.h"
#include "core/ai-provider.h"
#include "core/ai-config.h"

static void
test_claude_client_new(void)
{
	g_autoptr(AiClaudeClient) client = NULL;

	client = ai_claude_client_new();
	g_assert_nonnull(client);
	g_assert_true(AI_IS_CLAUDE_CLIENT(client));
	g_assert_true(AI_IS_CLIENT(client));
}

static void
test_claude_client_with_key(void)
{
	g_autoptr(AiClaudeClient) client = NULL;
	AiConfig *config;
	const gchar *key;

	client = ai_claude_client_new_with_key("test-api-key");
	g_assert_nonnull(client);

	config = ai_client_get_config(AI_CLIENT(client));
	key = ai_config_get_api_key(config, AI_PROVIDER_CLAUDE);
	g_assert_cmpstr(key, ==, "test-api-key");
}

static void
test_claude_client_provider_interface(void)
{
	g_autoptr(AiClaudeClient) client = NULL;
	AiProviderType type;
	const gchar *name;
	const gchar *model;

	client = ai_claude_client_new();

	g_assert_true(AI_IS_PROVIDER(client));

	type = ai_provider_get_provider_type(AI_PROVIDER(client));
	g_assert_cmpint(type, ==, AI_PROVIDER_CLAUDE);

	name = ai_provider_get_name(AI_PROVIDER(client));
	g_assert_cmpstr(name, ==, "Claude");

	model = ai_provider_get_default_model(AI_PROVIDER(client));
	g_assert_cmpstr(model, ==, AI_CLAUDE_DEFAULT_MODEL);
}

static void
test_claude_client_api_version(void)
{
	g_autoptr(AiClaudeClient) client = NULL;
	const gchar *version;

	client = ai_claude_client_new();

	/* Default version */
	version = ai_claude_client_get_api_version(client);
	g_assert_cmpstr(version, ==, AI_CLAUDE_API_VERSION);

	/* Custom version */
	ai_claude_client_set_api_version(client, "2024-01-01");
	version = ai_claude_client_get_api_version(client);
	g_assert_cmpstr(version, ==, "2024-01-01");
}

static void
test_claude_client_model(void)
{
	g_autoptr(AiClaudeClient) client = NULL;
	const gchar *model;

	client = ai_claude_client_new();

	/* Default model */
	model = ai_client_get_model(AI_CLIENT(client));
	g_assert_cmpstr(model, ==, AI_CLAUDE_DEFAULT_MODEL);

	/* Custom model */
	ai_client_set_model(AI_CLIENT(client), "claude-opus-4-20250514");
	model = ai_client_get_model(AI_CLIENT(client));
	g_assert_cmpstr(model, ==, "claude-opus-4-20250514");
}

static void
test_claude_client_gtype(void)
{
	GType type;

	type = ai_claude_client_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiClaudeClient");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/claude-client/new", test_claude_client_new);
	g_test_add_func("/ai-glib/claude-client/with-key", test_claude_client_with_key);
	g_test_add_func("/ai-glib/claude-client/provider-interface", test_claude_client_provider_interface);
	g_test_add_func("/ai-glib/claude-client/api-version", test_claude_client_api_version);
	g_test_add_func("/ai-glib/claude-client/model", test_claude_client_model);
	g_test_add_func("/ai-glib/claude-client/gtype", test_claude_client_gtype);

	return g_test_run();
}
