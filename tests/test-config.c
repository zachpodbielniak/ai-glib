/*
 * test-config.c - Unit tests for AiConfig
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

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

/*
 * Helper: write a string to a temporary file and return the path.
 * Caller must g_free() the returned path.
 */
static gchar *
write_temp_yaml(const gchar *content)
{
	gchar *path = NULL;
	GError *error = NULL;
	gint fd;

	fd = g_file_open_tmp("ai-config-test-XXXXXX.yaml", &path, &error);
	g_assert_no_error(error);
	g_assert_cmpint(fd, >=, 0);

	g_assert_true(g_file_set_contents(path, content, -1, &error));
	g_assert_no_error(error);
	close(fd);

	return path;
}

static void
test_config_load_from_file(void)
{
	g_autoptr(AiConfig) config = NULL;
	g_autofree gchar *path = NULL;
	GError *error = NULL;
	const gchar *yaml_content =
		"default_provider: ollama\n"
		"default_model: qwen2.5:7b\n"
		"timeout: 60\n"
		"max_retries: 5\n"
		"providers:\n"
		"  claude:\n"
		"    api_key: sk-ant-test-123\n"
		"  openai:\n"
		"    api_key: sk-openai-test\n"
		"    base_url: https://custom.openai.com\n"
		"  ollama:\n"
		"    base_url: http://remote-ollama:11434\n";

	/* Clear env vars so they don't interfere */
	g_unsetenv("ANTHROPIC_API_KEY");
	g_unsetenv("CLAUDE_API_KEY");
	g_unsetenv("OPENAI_API_KEY");
	g_unsetenv("OPENAI_BASE_URL");
	g_unsetenv("OLLAMA_HOST");
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");

	path = write_temp_yaml(yaml_content);
	config = g_object_new(AI_TYPE_CONFIG, NULL);

	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_no_error(error);

	/* Verify default provider and model */
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_OLLAMA);
	g_assert_cmpstr(ai_config_get_default_model(config),
	                ==, "qwen2.5:7b");

	/* Verify timeout and max_retries */
	g_assert_cmpuint(ai_config_get_timeout(config), ==, 60);
	g_assert_cmpuint(ai_config_get_max_retries(config), ==, 5);

	/* Verify provider API keys */
	g_assert_cmpstr(ai_config_get_api_key(config, AI_PROVIDER_CLAUDE),
	                ==, "sk-ant-test-123");
	g_assert_cmpstr(ai_config_get_api_key(config, AI_PROVIDER_OPENAI),
	                ==, "sk-openai-test");

	/* Verify provider base URLs */
	g_assert_cmpstr(ai_config_get_base_url(config, AI_PROVIDER_OPENAI),
	                ==, "https://custom.openai.com");
	g_assert_cmpstr(ai_config_get_base_url(config, AI_PROVIDER_OLLAMA),
	                ==, "http://remote-ollama:11434");

	g_unlink(path);
}

static void
test_config_file_priority(void)
{
	g_autoptr(AiConfig) config = NULL;
	g_autofree gchar *path1 = NULL;
	g_autofree gchar *path2 = NULL;
	GError *error = NULL;

	/* First file sets provider to ollama and model to llama3 */
	const gchar *yaml1 =
		"default_provider: ollama\n"
		"default_model: llama3\n"
		"timeout: 30\n";

	/* Second file overrides model and timeout, leaves provider */
	const gchar *yaml2 =
		"default_model: qwen2.5:7b\n"
		"timeout: 120\n";

	path1 = write_temp_yaml(yaml1);
	path2 = write_temp_yaml(yaml2);

	config = g_object_new(AI_TYPE_CONFIG, NULL);

	/* Load first (lower priority) */
	g_assert_true(ai_config_load_from_file(config, path1, &error));
	g_assert_no_error(error);

	/* Load second (higher priority, overrides) */
	g_assert_true(ai_config_load_from_file(config, path2, &error));
	g_assert_no_error(error);

	/* Provider stays from first file (not overridden by second) */
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_OLLAMA);

	/* Model and timeout come from second file */
	g_assert_cmpstr(ai_config_get_default_model(config),
	                ==, "qwen2.5:7b");
	g_assert_cmpuint(ai_config_get_timeout(config), ==, 120);

	g_unlink(path1);
	g_unlink(path2);
}

static void
test_config_default_provider_model(void)
{
	g_autoptr(AiConfig) config = NULL;

	/* Clear env vars so they don't interfere */
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");

	config = g_object_new(AI_TYPE_CONFIG, NULL);

	/* Defaults before any config is loaded */
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_CLAUDE);
	g_assert_null(ai_config_get_default_model(config));

	/* Programmatic set */
	ai_config_set_default_provider(config, AI_PROVIDER_OLLAMA);
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_OLLAMA);

	ai_config_set_default_model(config, "gpt-4");
	g_assert_cmpstr(ai_config_get_default_model(config), ==, "gpt-4");

	/* Clear model */
	ai_config_set_default_model(config, NULL);
	g_assert_null(ai_config_get_default_model(config));
}

static void
test_config_env_default_provider(void)
{
	g_autoptr(AiConfig) config = NULL;

	/* Clear all default env vars */
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");

	config = g_object_new(AI_TYPE_CONFIG, NULL);

	/* Without env var, falls back to built-in default */
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_CLAUDE);

	/* Set env var — should override built-in default */
	g_setenv("AI_GLIB_DEFAULT_PROVIDER", "ollama", TRUE);
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_OLLAMA);

	/* Test case-insensitive matching */
	g_setenv("AI_GLIB_DEFAULT_PROVIDER", "OpenAI", TRUE);
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_OPENAI);

	/* Programmatic set overrides env var */
	ai_config_set_default_provider(config, AI_PROVIDER_GEMINI);
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_GEMINI);

	/* Clean up */
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
}

static void
test_config_env_default_model(void)
{
	g_autoptr(AiConfig) config = NULL;

	/* Clear all default env vars */
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");

	config = g_object_new(AI_TYPE_CONFIG, NULL);

	/* Without env var, falls back to NULL */
	g_assert_null(ai_config_get_default_model(config));

	/* Set env var — should be returned */
	g_setenv("AI_GLIB_DEFAULT_MODEL", "qwen2.5:7b", TRUE);
	g_assert_cmpstr(ai_config_get_default_model(config),
	                ==, "qwen2.5:7b");

	/* Programmatic set overrides env var */
	ai_config_set_default_model(config, "gpt-4o");
	g_assert_cmpstr(ai_config_get_default_model(config),
	                ==, "gpt-4o");

	/* Clean up */
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
}

static void
test_config_env_overrides_file(void)
{
	g_autoptr(AiConfig) config = NULL;
	g_autofree gchar *path = NULL;
	GError *error = NULL;
	const gchar *yaml_content =
		"default_provider: gemini\n"
		"default_model: gemini-pro\n";

	/* Clear env vars first */
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");

	path = write_temp_yaml(yaml_content);
	config = g_object_new(AI_TYPE_CONFIG, NULL);

	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_no_error(error);

	/* File values should be returned when no env var set */
	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_GEMINI);
	g_assert_cmpstr(ai_config_get_default_model(config),
	                ==, "gemini-pro");

	/* Env vars override file values */
	g_setenv("AI_GLIB_DEFAULT_PROVIDER", "ollama", TRUE);
	g_setenv("AI_GLIB_DEFAULT_MODEL", "llama3", TRUE);

	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_OLLAMA);
	g_assert_cmpstr(ai_config_get_default_model(config),
	                ==, "llama3");

	/* Programmatic set overrides env vars */
	ai_config_set_default_provider(config, AI_PROVIDER_CLAUDE);
	ai_config_set_default_model(config, "claude-sonnet-4-20250514");

	g_assert_cmpint(ai_config_get_default_provider(config),
	                ==, AI_PROVIDER_CLAUDE);
	g_assert_cmpstr(ai_config_get_default_model(config),
	                ==, "claude-sonnet-4-20250514");

	/* Clean up */
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
	g_unlink(path);
}

static void
test_config_file_missing(void)
{
	g_autoptr(AiConfig) config = NULL;
	GError *error = NULL;

	config = g_object_new(AI_TYPE_CONFIG, NULL);

	/* Loading a non-existent file should return FALSE */
	g_assert_false(ai_config_load_from_file(
	    config, "/tmp/does-not-exist-ai-glib-test.yaml", &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
}

static void
test_config_file_invalid_yaml(void)
{
	g_autoptr(AiConfig) config = NULL;
	g_autofree gchar *path = NULL;
	GError *error = NULL;

	/* Write invalid YAML content */
	path = write_temp_yaml(":\n  bad: [unclosed\n  :\n");
	config = g_object_new(AI_TYPE_CONFIG, NULL);

	g_assert_false(ai_config_load_from_file(config, path, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);

	g_unlink(path);
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
	g_test_add_func("/ai-glib/config/load-from-file",
	                test_config_load_from_file);
	g_test_add_func("/ai-glib/config/file-priority",
	                test_config_file_priority);
	g_test_add_func("/ai-glib/config/default-provider-model",
	                test_config_default_provider_model);
	g_test_add_func("/ai-glib/config/env-default-provider",
	                test_config_env_default_provider);
	g_test_add_func("/ai-glib/config/env-default-model",
	                test_config_env_default_model);
	g_test_add_func("/ai-glib/config/env-overrides-file",
	                test_config_env_overrides_file);
	g_test_add_func("/ai-glib/config/file-missing",
	                test_config_file_missing);
	g_test_add_func("/ai-glib/config/file-invalid-yaml",
	                test_config_file_invalid_yaml);

	return g_test_run();
}
