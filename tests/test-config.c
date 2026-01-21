/*
 * test-config.c - Unit tests for AiConfig
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "core/ai-config.h"
#include "core/ai-enums.h"

static void
test_config_new(void)
{
	g_autoptr(AiConfig) config = NULL;

	config = ai_config_new();
	g_assert_nonnull(config);
	g_assert_true(AI_IS_CONFIG(config));
}

static void
test_config_api_key(void)
{
	g_autoptr(AiConfig) config = NULL;
	const gchar *key;

	/* Clear any existing environment variables (including alternatives) */
	g_unsetenv("ANTHROPIC_API_KEY");
	g_unsetenv("CLAUDE_API_KEY");
	g_unsetenv("OPENAI_API_KEY");
	g_unsetenv("GEMINI_API_KEY");
	g_unsetenv("XAI_API_KEY");
	g_unsetenv("GROK_API_KEY");
	g_unsetenv("OLLAMA_API_KEY");
	g_unsetenv("OLLAMA_HOST");

	config = ai_config_new();

	/* Initially NULL (after clearing env) */
	key = ai_config_get_api_key(config, AI_PROVIDER_CLAUDE);
	g_assert_null(key);

	/* Set and get */
	ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "test-key-123");
	key = ai_config_get_api_key(config, AI_PROVIDER_CLAUDE);
	g_assert_cmpstr(key, ==, "test-key-123");

	/* Different providers are independent */
	key = ai_config_get_api_key(config, AI_PROVIDER_OPENAI);
	g_assert_null(key);

	ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "openai-key");
	key = ai_config_get_api_key(config, AI_PROVIDER_OPENAI);
	g_assert_cmpstr(key, ==, "openai-key");
}

static void
test_config_base_url(void)
{
	g_autoptr(AiConfig) config = NULL;
	const gchar *url;

	config = ai_config_new();

	/* Default URLs */
	url = ai_config_get_base_url(config, AI_PROVIDER_CLAUDE);
	g_assert_cmpstr(url, ==, "https://api.anthropic.com");

	url = ai_config_get_base_url(config, AI_PROVIDER_OPENAI);
	g_assert_cmpstr(url, ==, "https://api.openai.com");

	url = ai_config_get_base_url(config, AI_PROVIDER_OLLAMA);
	g_assert_cmpstr(url, ==, "http://localhost:11434");

	/* Custom URL */
	ai_config_set_base_url(config, AI_PROVIDER_OPENAI, "https://custom.api.com");
	url = ai_config_get_base_url(config, AI_PROVIDER_OPENAI);
	g_assert_cmpstr(url, ==, "https://custom.api.com");
}

static void
test_config_from_env(void)
{
	g_autoptr(AiConfig) config = NULL;
	const gchar *key;

	/* Clear any alternative env vars first */
	g_unsetenv("CLAUDE_API_KEY");

	/* Set environment variable */
	g_setenv("ANTHROPIC_API_KEY", "env-test-key", TRUE);

	/* ai_config_new() automatically reads from environment */
	config = ai_config_new();
	g_assert_nonnull(config);

	key = ai_config_get_api_key(config, AI_PROVIDER_CLAUDE);
	g_assert_cmpstr(key, ==, "env-test-key");

	/* Clean up */
	g_unsetenv("ANTHROPIC_API_KEY");
}

static void
test_config_gtype(void)
{
	GType type;

	type = ai_config_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiConfig");
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/config/new", test_config_new);
	g_test_add_func("/ai-glib/config/api-key", test_config_api_key);
	g_test_add_func("/ai-glib/config/base-url", test_config_base_url);
	g_test_add_func("/ai-glib/config/from-env", test_config_from_env);
	g_test_add_func("/ai-glib/config/gtype", test_config_gtype);

	return g_test_run();
}
