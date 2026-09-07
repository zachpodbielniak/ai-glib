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
#include "core/ai-error.h"
#include "core/ai-client.h"
#include "convenience/ai-simple.h"
#include "convenience/ai-provider-factory.h"
#include <yaml-glib.h>

static gchar *sandbox;

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

/* Saving must preserve arbitrary settings, replace permissive modes and clear overlays. */
static void
test_config_save_defaults(void)
{
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(AiConfig) loaded = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_build_filename(sandbox, "ai-glib", NULL);
	g_autofree gchar *path = g_build_filename(directory, "config.yaml", NULL);
	g_autofree gchar *inherited = write_temp_yaml("default_model: inherited\n");
	g_autofree gchar *saved = NULL;
	YamlMapping *mapping;
	GStatBuf st;

	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
	g_assert_true(ai_config_save_defaults(config, NULL, AI_PROVIDER_OLLAMA, "first", &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_stat(directory, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0700);
	g_assert_true(g_file_set_contents(path,
		"timeout: 77\nproviders:\n  openai:\n    api_key: secret\n"
		"extra: {items: [one, two], enabled: true}\n"
		"default_provider: ollama\ndefault_model: first\n", -1, &error));
	g_assert_cmpint(g_chmod(path, 0644), ==, 0);
	g_assert_true(ai_config_save_defaults(config, NULL, AI_PROVIDER_OPENAI, "second: #model", &error));
	g_assert_no_error(error);
	g_assert_cmpint(ai_config_get_default_provider(config), ==, AI_PROVIDER_OPENAI);
	g_assert_cmpstr(ai_config_get_default_model(config), ==, "second: #model");
	g_assert_true(ai_config_save_defaults(config, NULL, AI_PROVIDER_GEMINI, NULL, &error));
	g_assert_no_error(error);
	g_assert_null(ai_config_get_default_model(config));
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
	g_assert_true(yaml_parser_load_from_file(parser, path, &error));
	mapping = yaml_node_get_mapping(yaml_parser_get_root(parser));
	g_assert_cmpuint(yaml_mapping_get_size(mapping), ==, 5);
	g_assert_cmpint(yaml_mapping_get_int_member(mapping, "timeout"), ==, 77);
	g_assert_true(g_file_get_contents(path, &saved, NULL, &error));
	g_assert_nonnull(strstr(saved, "default_model: \"\""));
	g_assert_cmpstr(yaml_mapping_get_string_member(mapping, "default_provider"), ==, "gemini");
	mapping = yaml_mapping_get_mapping_member(mapping, "extra");
	g_assert_true(yaml_mapping_get_boolean_member(mapping, "enabled"));
	g_assert_cmpuint(yaml_sequence_get_length(yaml_mapping_get_sequence_member(mapping, "items")), ==, 2);
	g_assert_true(ai_config_load_from_file(loaded, inherited, &error));
	g_assert_cmpstr(ai_config_get_default_model(loaded), ==, "inherited");
	g_assert_true(ai_config_load_from_file(loaded, path, &error));
	g_assert_no_error(error);
	g_assert_null(ai_config_get_default_model(loaded));
	g_assert_cmpstr(ai_config_get_api_key(loaded, AI_PROVIDER_OPENAI), ==, "secret");
	g_unlink(inherited);
	g_unlink(path);
	g_rmdir(directory);
}

/* Failed saves never replace the original bytes or the in-memory defaults. */
static void
test_config_save_rejected(void)
{
	const gchar *invalid[] = {
		"broken: [\n", "- sequence\n", "scalar\n", "", "{}\n---\n{}\n",
		"apps: []\n", "apps: {ai: null}\n", "apps: {ai-tui: []}\n",
		"apps: {ai: {default_provider: typo}}\n",
		"apps: {ai: {default_model: []}}\n",
		"apps: {ai: {}, ai: {}}\n",
		"default_provider: typo\n", "default_provider: []\n",
		"default_provider: {}\n", "default_provider: null\n",
		"default_model: []\n", "default_model: {}\n",
		"default_model: \"bad\\0model\"\n",
		"\"default_provider\\0suffix\": ollama\n",
		"extra: \"bad\\0value\"\n",
		"extra: &loop {self: *loop}\n",
		"extra: &loop [*loop]\n",
		"extra: &outer {child: {parent: *outer}}\n",
		"&root {self: *root}\n"
	};
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autofree gchar *directory = g_build_filename(sandbox, "ai-glib", NULL);
	g_autofree gchar *path = g_build_filename(directory, "config.yaml", NULL);
	guint i;

	ai_config_set_default_provider(config, AI_PROVIDER_OLLAMA);
	ai_config_set_default_model(config, "unchanged");
	g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
	for (i = 0; i < G_N_ELEMENTS(invalid); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *after = NULL;

		g_assert_true(g_file_set_contents(path, invalid[i], -1, &error));
		g_assert_false(ai_config_load_from_file(config, path, &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
		g_clear_error(&error);
		g_assert_false(ai_config_save_defaults(config, NULL, AI_PROVIDER_OPENAI, "new", &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_false(ai_config_save_defaults(config, "ai", AI_PROVIDER_OPENAI, "new", &error));
		g_assert_nonnull(error);
		g_clear_error(&error);
		g_assert_true(g_file_get_contents(path, &after, NULL, &error));
		g_assert_cmpstr(after, ==, invalid[i]);
		g_assert_cmpint(ai_config_get_default_provider(config), ==, AI_PROVIDER_OLLAMA);
		g_assert_cmpstr(ai_config_get_default_model(config), ==, "unchanged");
	}
	g_unlink(path);
	g_rmdir(directory);
}

/* A table keeps precedence and cross-provider model isolation visible together. */
static void
test_config_resolve_defaults(void)
{
	const gchar *scopes[] = {NULL, "ai", "ai-tui"};
	const struct {
		const gchar *provider;
		const gchar *model;
		const gchar *legacy;
		AiProviderType expected_provider;
		const gchar *expected_model;
	} cases[] = {
		{NULL, NULL, NULL, AI_PROVIDER_OLLAMA, "configured"},
		{"", "default", "", AI_PROVIDER_OLLAMA, "configured"},
		{NULL, NULL, "openai", AI_PROVIDER_OPENAI, NULL},
		{"default", NULL, "openai", AI_PROVIDER_OLLAMA, "configured"},
		{"gemini", NULL, "openai", AI_PROVIDER_GEMINI, NULL},
		{"openai", "default", NULL, AI_PROVIDER_OPENAI, NULL},
		{"ollama", NULL, "openai", AI_PROVIDER_OLLAMA, "configured"},
		{"openai", "explicit", NULL, AI_PROVIDER_OPENAI, "explicit"},
		{"ANTHROPIC", NULL, NULL, AI_PROVIDER_CLAUDE, NULL},
		{NULL, "explicit", "openai", AI_PROVIDER_OPENAI, "explicit"}
	};
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *model = NULL;
	g_autofree gchar *path = write_temp_yaml(
		"default_provider: ollama\ndefault_model: configured\n"
		"apps:\n  ai: {default_provider: ollama, default_model: configured}\n"
		"  ai-tui: {default_provider: ollama, default_model: configured}\n");
	AiProviderType provider;
	guint i;
	guint j;

	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
	g_assert_true(ai_config_load_from_file(config, path, &error));
	for (j = 0; j < G_N_ELEMENTS(scopes); j++)
	{
		for (i = 0; i < G_N_ELEMENTS(cases); i++)
		{
			if (cases[i].legacy != NULL)
				g_setenv("AI_PROVIDER", cases[i].legacy, TRUE);
			else
				g_unsetenv("AI_PROVIDER");
			g_assert_true(ai_provider_factory_resolve_defaults(config, scopes[j], cases[i].provider,
				cases[i].model, &provider, &model, &error));
			g_assert_no_error(error);
			g_assert_cmpint(provider, ==, cases[i].expected_provider);
			g_assert_cmpstr(model, ==, cases[i].expected_model);
			g_clear_pointer(&model, g_free);
		}
	}
	g_setenv("AI_PROVIDER", "typo", TRUE);
	g_assert_false(ai_provider_factory_resolve_defaults(config, NULL, NULL, NULL, &provider, &model, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
	g_assert_true(ai_provider_factory_resolve_defaults(config, NULL, "default", NULL, &provider, &model, &error));
	g_clear_pointer(&model, g_free);
	g_unsetenv("AI_PROVIDER");
	g_assert_false(ai_provider_factory_resolve_defaults(config, NULL, "typo", NULL, &provider, &model, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
	g_setenv("AI_GLIB_DEFAULT_PROVIDER", "typo", TRUE);
	g_assert_false(ai_provider_factory_resolve_defaults(config, NULL, "default", NULL, &provider, &model, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_assert_cmpint(provider, ==, (AiProviderType)-1);
	g_assert_null(model);
	g_clear_error(&error);
	ai_config_set_default_provider(config, AI_PROVIDER_OLLAMA);
	ai_config_set_default_model(config, "default");
	g_assert_true(ai_provider_factory_resolve_defaults(config, NULL, NULL, NULL, &provider, &model, &error));
	g_assert_null(model);
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_assert_true(g_file_set_contents(path, "default_provider: typo\n", -1, &error));
	g_assert_false(ai_config_load_from_file(config, path, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_unlink(path);
}

/* App saves preserve library settings, siblings, unknown fields and aliased maps. */
static void
test_config_app_defaults(void)
{
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(AiConfig) loaded = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(YamlParser) parser = yaml_parser_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_build_filename(sandbox, "ai-glib", NULL);
	g_autofree gchar *path = g_build_filename(directory, "config.yaml", NULL);
	g_autofree gchar *model = NULL;
	YamlMapping *root;
	YamlMapping *apps;
	AiProviderType provider;
	GStatBuf st;

	g_unsetenv("AI_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
	g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path,
		"default_provider: ollama\ndefault_model: library\ntimeout: 77\n"
		"apps: &apps\n  ai: &ai\n    default_provider: openai\n"
		"    default_model: app-model\n    extra: retained\n"
		"  ai-tui: *ai\n  other: {custom: kept}\n"
		"copy: *apps\n", -1, &error));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_cmpint(ai_config_get_app_provider(config, "ai"), ==, AI_PROVIDER_OPENAI);
	g_assert_cmpstr(ai_config_get_app_model(config, "ai-tui"), ==, "app-model");
	g_assert_true(ai_config_save_defaults(config, "ai", AI_PROVIDER_GEMINI, "new-model", &error));
	g_assert_cmpint(ai_config_get_default_provider(config), ==, AI_PROVIDER_OLLAMA);
	g_assert_cmpstr(ai_config_get_default_model(config), ==, "library");
	g_assert_cmpstr(ai_config_get_app_model(config, "ai-tui"), ==, "app-model");
	g_assert_true(yaml_parser_load_from_file(parser, path, &error));
	root = yaml_node_get_mapping(yaml_parser_get_root(parser));
	g_assert_cmpstr(yaml_mapping_get_string_member(root, "default_provider"), ==, "ollama");
	g_assert_cmpstr(yaml_mapping_get_string_member(root, "default_model"), ==, "library");
	g_assert_cmpint(yaml_mapping_get_int_member(root, "timeout"), ==, 77);
	apps = yaml_mapping_get_mapping_member(root, "apps");
	g_assert_cmpstr(yaml_mapping_get_string_member(yaml_mapping_get_mapping_member(apps, "ai"), "extra"), ==, "retained");
	g_assert_cmpstr(yaml_mapping_get_string_member(yaml_mapping_get_mapping_member(apps, "other"), "custom"), ==, "kept");
	apps = yaml_mapping_get_mapping_member(root, "copy");
	g_assert_cmpstr(yaml_mapping_get_string_member(yaml_mapping_get_mapping_member(apps, "ai"), "default_model"), ==, "app-model");
	g_assert_true(ai_config_load_from_file(loaded, path, &error));
	g_assert_cmpstr(ai_config_get_app_model(loaded, "ai"), ==, "new-model");
	g_assert_cmpstr(ai_config_get_app_model(loaded, "ai-tui"), ==, "app-model");

	/* Library env overrides, even invalid ones, cannot affect app resolution. */
	g_setenv("AI_GLIB_DEFAULT_PROVIDER", "typo", TRUE);
	g_setenv("AI_GLIB_DEFAULT_MODEL", "env-model", TRUE);
	g_setenv("AI_PROVIDER", "openai", TRUE);
	g_assert_true(ai_provider_factory_resolve_defaults(loaded, "ai", NULL, NULL, &provider, &model, &error));
	g_assert_cmpint(provider, ==, AI_PROVIDER_OPENAI);
	g_assert_null(model);
	g_assert_true(ai_provider_factory_resolve_defaults(loaded, "ai", "default", "default", &provider, &model, &error));
	g_assert_cmpint(provider, ==, AI_PROVIDER_GEMINI);
	g_assert_cmpstr(model, ==, "new-model");
	g_clear_pointer(&model, g_free);
	g_assert_true(ai_provider_factory_resolve_defaults(loaded, "ai-tui", NULL, NULL, &provider, &model, &error));
	g_assert_cmpstr(model, ==, "app-model");
	g_clear_pointer(&model, g_free);
	g_assert_true(ai_config_save_defaults(config, "ai-tui", AI_PROVIDER_CURSOR, NULL, &error));
	g_assert_true(ai_config_load_from_file(loaded, path, &error));
	g_assert_null(ai_config_get_app_model(loaded, "ai-tui"));
	g_assert_cmpstr(ai_config_get_app_model(loaded, "ai"), ==, "new-model");
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
	g_unsetenv("AI_PROVIDER");
	g_assert_cmpint(ai_config_get_default_provider(loaded), ==, AI_PROVIDER_OLLAMA);
	g_assert_cmpstr(ai_config_get_default_model(loaded), ==, "library");
	/* The convenience interface must consume the library pair, not either app. */
	{
		g_autoptr(AiSimple) simple = ai_simple_new_with_config(loaded);
		AiProvider *simple_provider = ai_simple_get_provider(simple);

		g_assert_cmpint(ai_provider_get_provider_type(simple_provider), ==, AI_PROVIDER_OLLAMA);
		g_assert_cmpstr(ai_client_get_model(AI_CLIENT(simple_provider)), ==, "library");
	}
	g_assert_true(ai_config_save_defaults(config, NULL, AI_PROVIDER_GROK, "library-new", &error));
	g_assert_true(ai_config_load_from_file(loaded, path, &error));
	g_assert_cmpstr(ai_config_get_app_model(loaded, "ai"), ==, "new-model");
	g_assert_cmpint(ai_config_get_app_provider(loaded, "ai-tui"), ==, AI_PROVIDER_CURSOR);
	g_assert_no_error(error);
	g_unlink(path);

	/* A brand-new app save must not introduce top-level library defaults. */
	g_assert_true(ai_config_save_defaults(config, "ai", AI_PROVIDER_OPENAI, "fresh", &error));
	g_clear_object(&parser);
	parser = yaml_parser_new();
	g_assert_true(yaml_parser_load_from_file(parser, path, &error));
	root = yaml_node_get_mapping(yaml_parser_get_root(parser));
	g_assert_cmpuint(yaml_mapping_get_size(root), ==, 1);
	g_assert_true(yaml_mapping_has_member(root, "apps"));
	g_assert_false(yaml_mapping_has_member(root, "default_provider"));
	g_assert_false(yaml_mapping_has_member(root, "default_model"));
	g_assert_no_error(error);
	g_unlink(path);
	g_rmdir(directory);
}

/* Missing app fields use native defaults, never the library's saved or env values. */
static void
test_config_app_missing(void)
{
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *model = NULL;
	g_autofree gchar *path = write_temp_yaml(
		"default_provider: ollama\ndefault_model: library\n"
		"apps: {ai: {default_provider: gemini}}\n");
	AiProviderType provider;

	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_setenv("AI_GLIB_DEFAULT_PROVIDER", "openai", TRUE);
	g_setenv("AI_GLIB_DEFAULT_MODEL", "env-model", TRUE);
	g_unsetenv("AI_PROVIDER");
	g_assert_true(ai_provider_factory_resolve_defaults(config, "ai", NULL, NULL, &provider, &model, &error));
	g_assert_cmpint(provider, ==, AI_PROVIDER_GEMINI);
	g_assert_null(model);
	g_assert_true(ai_provider_factory_resolve_defaults(config, "ai-tui", NULL, NULL, &provider, &model, &error));
	g_assert_cmpint(provider, ==, AI_PROVIDER_CLAUDE);
	g_assert_null(model);
	g_assert_true(ai_provider_factory_resolve_defaults(config, NULL, NULL, NULL, &provider, &model, &error));
	g_assert_cmpint(provider, ==, AI_PROVIDER_OPENAI);
	g_assert_cmpstr(model, ==, "env-model");
	g_clear_pointer(&model, g_free);
	g_assert_true(g_file_set_contents(path, "apps: {ai: {default_model: quoted}}\n", -1, &error));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_cmpint(ai_config_get_app_provider(config, "ai"), ==, AI_PROVIDER_GEMINI);
	g_assert_cmpstr(ai_config_get_app_model(config, "ai"), ==, "quoted");
	g_assert_true(g_file_set_contents(path, "apps: {ai: {default_model: null}}\n", -1, &error));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_null(ai_config_get_app_model(config, "ai"));
	g_assert_true(g_file_set_contents(path, "apps: {ai: {default_model: 'null'}}\n", -1, &error));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_cmpstr(ai_config_get_app_model(config, "ai"), ==, "null");
	g_assert_no_error(error);
	g_unsetenv("AI_GLIB_DEFAULT_PROVIDER");
	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
	g_unlink(path);
}

/* All scopes share null handling, including clearing an inherited model. */
static void
test_config_null_models(void)
{
	const struct {
		const gchar *yaml;
		const gchar *expected;
	} cases[] = {
		{"null", NULL}, {"~", NULL}, {"NULL", NULL}, {"", NULL},
		{"''", NULL}, {"!!null 'null'", NULL},
		{"'null'", "null"}, {"\"null\"", "null"}, {"'~'", "~"}
	};
	g_autofree gchar *path = write_temp_yaml("{}\n");
	guint i;

	g_unsetenv("AI_GLIB_DEFAULT_MODEL");
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *yaml = g_strdup_printf(
			"default_model: %s\napps:\n  ai:\n    default_model: %s\n"
			"  ai-tui:\n    default_model: %s\n",
			cases[i].yaml, cases[i].yaml, cases[i].yaml);

		g_assert_true(g_file_set_contents(path,
			"default_model: inherited\napps:\n"
			"  ai: {default_model: inherited}\n  ai-tui: {default_model: inherited}\n",
			-1, &error));
		g_assert_true(ai_config_load_from_file(config, path, &error));
		g_assert_true(g_file_set_contents(path, yaml, -1, &error));
		g_assert_true(ai_config_load_from_file(config, path, &error));
		g_assert_no_error(error);
		g_assert_cmpstr(ai_config_get_default_model(config), ==, cases[i].expected);
		g_assert_cmpstr(ai_config_get_app_model(config, "ai"), ==, cases[i].expected);
		g_assert_cmpstr(ai_config_get_app_model(config, "ai-tui"), ==, cases[i].expected);
	}
	g_unlink(path);
}

/* Invalid public input must fail before libyaml sees it, without touching disk. */
static void
test_config_invalid_utf8_model(void)
{
	const gchar *scopes[] = {NULL, "ai", "ai-tui"};
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autofree gchar *directory = g_build_filename(sandbox, "ai-glib", NULL);
	g_autofree gchar *path = g_build_filename(directory, "config.yaml", NULL);
	const gchar *original = "default_provider: ollama\ndefault_model: unchanged\n";
	guint i;

	g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, original, -1, NULL));
	g_assert_true(ai_config_load_from_file(config, path, NULL));
	for (i = 0; i < G_N_ELEMENTS(scopes); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *after = NULL;

		g_assert_false(ai_config_save_defaults(config, scopes[i], AI_PROVIDER_OPENAI, "bad\xff", &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
		g_assert_true(g_file_get_contents(path, &after, NULL, NULL));
		g_assert_cmpstr(after, ==, original);
		g_assert_cmpint(ai_config_get_default_provider(config), ==, AI_PROVIDER_OLLAMA);
		g_assert_cmpstr(ai_config_get_default_model(config), ==, "unchanged");
		g_assert_cmpint(ai_config_get_app_provider(config, "ai"), ==, AI_PROVIDER_CLAUDE);
		g_assert_null(ai_config_get_app_model(config, "ai"));
		g_assert_cmpint(ai_config_get_app_provider(config, "ai-tui"), ==, AI_PROVIDER_CLAUDE);
		g_assert_null(ai_config_get_app_model(config, "ai-tui"));
	}
	g_unlink(path);
	g_rmdir(directory);
}

/* Reject cycles before yaml-glib conversion without changing any loaded state. */
static void
test_config_yaml_edges(void)
{
	g_autoptr(AiConfig) config = g_object_new(AI_TYPE_CONFIG, NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = write_temp_yaml("default_model: retained\n");
	g_autoptr(GString) deep = g_string_new("extra: ");
	const gchar *cycles[] = {
		"default_model: new\nextra: &loop {self: *loop}\n",
		"default_model: new\nsequence: &sequence [*sequence]\n",
		"default_model: new\nextra: &outer {child: {parent: *outer}}\n",
		"&root {default_model: new, self: *root}\n"
	};
	const gchar raw_nul[] = "default_model: new\n\0default_provider: openai\n";
	guint i;

	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(ai_config_get_default_model(config), ==, "retained");
	for (i = 0; i < G_N_ELEMENTS(cycles); i++)
	{
		g_assert_true(g_file_set_contents(path, cycles[i], -1, &error));
		g_assert_false(ai_config_load_from_file(config, path, &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
		g_clear_error(&error);
		g_assert_cmpstr(ai_config_get_default_model(config), ==, "retained");
	}
	/* Repeated aliases are a DAG, not a cycle. */
	g_assert_true(g_file_set_contents(path,
		"a: &a [one, two]\nb: &b [*a, *a]\nc: [*b, *b]\n", -1, &error));
	g_assert_true(ai_config_load_from_file(config, path, &error));
	g_assert_no_error(error);
	for (i = 0; i < 130; i++)
		g_string_append_c(deep, '[');
	g_string_append(deep, "value");
	for (i = 0; i < 130; i++)
		g_string_append_c(deep, ']');
	g_assert_true(g_file_set_contents(path, deep->str, deep->len, &error));
	g_assert_false(ai_config_load_from_file(config, path, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
	g_assert_cmpstr(ai_config_get_default_model(config), ==, "retained");
	g_assert_true(g_file_set_contents(path, raw_nul, sizeof(raw_nul) - 1, &error));
	g_assert_false(ai_config_load_from_file(config, path, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_assert_cmpstr(ai_config_get_default_model(config), ==, "retained");
	g_unlink(path);
}

int
main(
	int   argc,
	char *argv[]
){
	gint result;

	/* Set XDG before GLib caches it; never read or write the developer's config. */
	sandbox = g_dir_make_tmp("ai-config-sandbox-XXXXXX", NULL);
	g_assert_nonnull(sandbox);
	g_setenv("HOME", sandbox, TRUE);
	g_setenv("XDG_CONFIG_HOME", sandbox, TRUE);
	g_setenv("GIO_USE_VFS", "local", TRUE);
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

	g_test_add_func("/ai-glib/config/save-defaults", test_config_save_defaults);
	g_test_add_func("/ai-glib/config/save-rejected", test_config_save_rejected);
	g_test_add_func("/ai-glib/config/resolve-defaults", test_config_resolve_defaults);
	g_test_add_func("/ai-glib/config/app-defaults", test_config_app_defaults);
	g_test_add_func("/ai-glib/config/app-missing", test_config_app_missing);
	g_test_add_func("/ai-glib/config/null-models", test_config_null_models);
	g_test_add_func("/ai-glib/config/invalid-utf8-model", test_config_invalid_utf8_model);
	g_test_add_func("/ai-glib/config/yaml-edges", test_config_yaml_edges);
	result = g_test_run();
	g_rmdir(sandbox);
	g_free(sandbox);
	return result;
}
