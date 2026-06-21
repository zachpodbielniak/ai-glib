/*
 * test-claude-launch.c - Unit tests for the Ollama-as-transport launcher
 *                        helpers (ai-claude-launch)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "providers/ai-claude-launch.h"

/* ================================================================
 * model_is_ollama / ollama_model
 * ================================================================ */

static void
test_is_ollama_null(void)
{
	g_assert_false(ai_claude_launch_model_is_ollama(NULL));
	g_assert_null(ai_claude_launch_ollama_model(NULL));
}

static void
test_is_ollama_empty(void)
{
	g_assert_false(ai_claude_launch_model_is_ollama(""));
	g_assert_null(ai_claude_launch_ollama_model(""));
}

static void
test_is_ollama_no_slash(void)
{
	/* "ollama" without a slash is NOT the transport. */
	g_assert_false(ai_claude_launch_model_is_ollama("ollama"));
	g_assert_null(ai_claude_launch_ollama_model("ollama"));
}

static void
test_is_ollama_empty_suffix(void)
{
	/* "ollama/" with nothing after the slash is NOT the transport. */
	g_assert_false(ai_claude_launch_model_is_ollama("ollama/"));
	g_assert_null(ai_claude_launch_ollama_model("ollama/"));
}

static void
test_is_ollama_minimal(void)
{
	g_assert_true(ai_claude_launch_model_is_ollama("ollama/x"));
	g_assert_cmpstr(ai_claude_launch_ollama_model("ollama/x"), ==, "x");
}

static void
test_is_ollama_typical_cloud(void)
{
	g_assert_true(ai_claude_launch_model_is_ollama("ollama/glm-5.2:cloud"));
	g_assert_cmpstr(ai_claude_launch_ollama_model("ollama/glm-5.2:cloud"),
	                ==, "glm-5.2:cloud");
}

static void
test_is_ollama_colon_suffix(void)
{
	g_assert_true(ai_claude_launch_model_is_ollama("ollama/foo:bar"));
	g_assert_cmpstr(ai_claude_launch_ollama_model("ollama/foo:bar"),
	                ==, "foo:bar");
}

static void
test_is_ollama_nested_slash(void)
{
	/* Only the first "ollama/" is consumed; the rest is verbatim. */
	g_assert_true(ai_claude_launch_model_is_ollama("ollama/foo/bar"));
	g_assert_cmpstr(ai_claude_launch_ollama_model("ollama/foo/bar"),
	                ==, "foo/bar");
}

static void
test_is_ollama_uppercase_prefix(void)
{
	/* Case-sensitive: "Ollama/" / "OLLAMA/" are not the transport. */
	g_assert_false(ai_claude_launch_model_is_ollama("Ollama/x"));
	g_assert_false(ai_claude_launch_model_is_ollama("OLLAMA/x"));
	g_assert_null(ai_claude_launch_ollama_model("Ollama/x"));
}

static void
test_is_ollama_leading_space(void)
{
	/* No trimming: the prefix must be at index 0. */
	g_assert_false(ai_claude_launch_model_is_ollama(" ollama/x"));
	g_assert_null(ai_claude_launch_ollama_model(" ollama/x"));
}

static void
test_is_ollama_trailing_space_suffix(void)
{
	/* Whitespace in the suffix is preserved verbatim. */
	g_assert_true(ai_claude_launch_model_is_ollama("ollama/x "));
	g_assert_cmpstr(ai_claude_launch_ollama_model("ollama/x "), ==, "x ");
}

static void
test_is_ollama_space_only_suffix(void)
{
	/* A non-empty (even all-whitespace) suffix counts. */
	g_assert_true(ai_claude_launch_model_is_ollama("ollama/ "));
	g_assert_cmpstr(ai_claude_launch_ollama_model("ollama/ "), ==, " ");
}

static void
test_is_ollama_midstring_not_prefix(void)
{
	/* Must be a true prefix, not a substring. */
	g_assert_false(ai_claude_launch_model_is_ollama("x-ollama/y"));
	g_assert_null(ai_claude_launch_ollama_model("x-ollama/y"));
}

static void
test_is_ollama_plain_claude_alias(void)
{
	g_assert_false(ai_claude_launch_model_is_ollama("sonnet"));
	g_assert_null(ai_claude_launch_ollama_model("sonnet"));
}

/* ================================================================
 * should_emit_claude_model
 * ================================================================ */

static void
test_should_emit_model(void)
{
	g_assert_true(ai_claude_launch_should_emit_claude_model(NULL));
	g_assert_true(ai_claude_launch_should_emit_claude_model("sonnet"));
	g_assert_true(ai_claude_launch_should_emit_claude_model("ollama"));
	g_assert_true(ai_claude_launch_should_emit_claude_model("ollama/"));
	g_assert_false(ai_claude_launch_should_emit_claude_model("ollama/x"));
}

/* ================================================================
 * executable_name (env-sensitive; save & restore)
 * ================================================================ */

static void
test_executable_name_claude_default(void)
{
	g_autofree gchar *old_claude = g_strdup(g_getenv("CLAUDE_CODE_PATH"));
	g_autofree gchar *name = NULL;

	g_unsetenv("CLAUDE_CODE_PATH");
	name = ai_claude_launch_executable_name("sonnet");
	g_assert_cmpstr(name, ==, "claude");

	if (old_claude != NULL)
		g_setenv("CLAUDE_CODE_PATH", old_claude, TRUE);
}

static void
test_executable_name_claude_env(void)
{
	g_autofree gchar *old_claude = g_strdup(g_getenv("CLAUDE_CODE_PATH"));
	g_autofree gchar *name = NULL;

	g_setenv("CLAUDE_CODE_PATH", "/opt/claude", TRUE);
	name = ai_claude_launch_executable_name("sonnet");
	g_assert_cmpstr(name, ==, "/opt/claude");

	if (old_claude != NULL)
		g_setenv("CLAUDE_CODE_PATH", old_claude, TRUE);
	else
		g_unsetenv("CLAUDE_CODE_PATH");
}

static void
test_executable_name_ollama_default(void)
{
	g_autofree gchar *old_ollama = g_strdup(g_getenv("OLLAMA_PATH"));
	g_autofree gchar *name = NULL;

	g_unsetenv("OLLAMA_PATH");
	name = ai_claude_launch_executable_name("ollama/glm-5.2:cloud");
	g_assert_cmpstr(name, ==, "ollama");

	if (old_ollama != NULL)
		g_setenv("OLLAMA_PATH", old_ollama, TRUE);
}

static void
test_executable_name_ollama_env(void)
{
	g_autofree gchar *old_ollama = g_strdup(g_getenv("OLLAMA_PATH"));
	g_autofree gchar *name = NULL;

	g_setenv("OLLAMA_PATH", "/usr/local/bin/ollama", TRUE);
	name = ai_claude_launch_executable_name("ollama/x");
	g_assert_cmpstr(name, ==, "/usr/local/bin/ollama");

	if (old_ollama != NULL)
		g_setenv("OLLAMA_PATH", old_ollama, TRUE);
	else
		g_unsetenv("OLLAMA_PATH");
}

static void
test_executable_name_ollama_ignores_claude_env(void)
{
	g_autofree gchar *old_ollama = g_strdup(g_getenv("OLLAMA_PATH"));
	g_autofree gchar *old_claude = g_strdup(g_getenv("CLAUDE_CODE_PATH"));
	g_autofree gchar *name = NULL;

	g_unsetenv("OLLAMA_PATH");
	g_setenv("CLAUDE_CODE_PATH", "/opt/claude", TRUE);
	/* Ollama mode must NOT pick up the claude path. */
	name = ai_claude_launch_executable_name("ollama/x");
	g_assert_cmpstr(name, ==, "ollama");

	if (old_ollama != NULL)
		g_setenv("OLLAMA_PATH", old_ollama, TRUE);
	if (old_claude != NULL)
		g_setenv("CLAUDE_CODE_PATH", old_claude, TRUE);
	else
		g_unsetenv("CLAUDE_CODE_PATH");
}

static void
test_executable_name_null_model(void)
{
	g_autofree gchar *old_claude = g_strdup(g_getenv("CLAUDE_CODE_PATH"));
	g_autofree gchar *name = NULL;

	g_unsetenv("CLAUDE_CODE_PATH");
	name = ai_claude_launch_executable_name(NULL);
	g_assert_cmpstr(name, ==, "claude");

	if (old_claude != NULL)
		g_setenv("CLAUDE_CODE_PATH", old_claude, TRUE);
}

/* ================================================================
 * emit_tokens
 * ================================================================ */

/* Assert @argv (a GPtrArray of strings, NOT NULL-terminated here) equals
 * the given NULL-terminated expectation. */
static void
assert_argv_equals(GPtrArray *argv, const gchar * const *expected)
{
	guint i;

	for (i = 0; expected[i] != NULL; i++)
	{
		g_assert_cmpuint(i, <, argv->len);
		g_assert_cmpstr(g_ptr_array_index(argv, i), ==, expected[i]);
	}
	g_assert_cmpuint(argv->len, ==, i);
}

static void
test_emit_tokens_claude_mode(void)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	const gchar *expected[] = { "claude", "--print", "--output-format",
	                            "json", NULL };

	ai_claude_launch_emit_tokens(argv, "sonnet", "claude");
	/* Append a representative claude tail. */
	g_ptr_array_add(argv, g_strdup("--print"));
	g_ptr_array_add(argv, g_strdup("--output-format"));
	g_ptr_array_add(argv, g_strdup("json"));

	assert_argv_equals(argv, expected);
}

static void
test_emit_tokens_ollama_mode(void)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	const gchar *expected[] = {
		"ollama", "launch", "claude", "--model", "glm-5.2:cloud", "--",
		"--print", "--output-format", "json", NULL
	};

	ai_claude_launch_emit_tokens(argv, "ollama/glm-5.2:cloud", "ollama");
	g_ptr_array_add(argv, g_strdup("--print"));
	g_ptr_array_add(argv, g_strdup("--output-format"));
	g_ptr_array_add(argv, g_strdup("json"));

	assert_argv_equals(argv, expected);
}

static void
test_emit_tokens_empty_tail_claude(void)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	const gchar *expected[] = { "claude", NULL };

	ai_claude_launch_emit_tokens(argv, NULL, "claude");
	assert_argv_equals(argv, expected);
}

static void
test_emit_tokens_empty_tail_ollama(void)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	const gchar *expected[] = {
		"ollama", "launch", "claude", "--model", "m", "--", NULL
	};

	ai_claude_launch_emit_tokens(argv, "ollama/m", "ollama");
	assert_argv_equals(argv, expected);
}

static void
test_emit_tokens_program_token_used(void)
{
	/* The program token is whatever the caller passes (e.g. a resolved
	 * path), not a hardcoded name. */
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	const gchar *expected[] = {
		"/usr/bin/ollama", "launch", "claude", "--model", "m", "--", NULL
	};

	ai_claude_launch_emit_tokens(argv, "ollama/m", "/usr/bin/ollama");
	assert_argv_equals(argv, expected);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/claude-launch/is-ollama/null", test_is_ollama_null);
	g_test_add_func("/claude-launch/is-ollama/empty", test_is_ollama_empty);
	g_test_add_func("/claude-launch/is-ollama/no-slash",
	                test_is_ollama_no_slash);
	g_test_add_func("/claude-launch/is-ollama/empty-suffix",
	                test_is_ollama_empty_suffix);
	g_test_add_func("/claude-launch/is-ollama/minimal",
	                test_is_ollama_minimal);
	g_test_add_func("/claude-launch/is-ollama/typical-cloud",
	                test_is_ollama_typical_cloud);
	g_test_add_func("/claude-launch/is-ollama/colon-suffix",
	                test_is_ollama_colon_suffix);
	g_test_add_func("/claude-launch/is-ollama/nested-slash",
	                test_is_ollama_nested_slash);
	g_test_add_func("/claude-launch/is-ollama/uppercase-prefix",
	                test_is_ollama_uppercase_prefix);
	g_test_add_func("/claude-launch/is-ollama/leading-space",
	                test_is_ollama_leading_space);
	g_test_add_func("/claude-launch/is-ollama/trailing-space-suffix",
	                test_is_ollama_trailing_space_suffix);
	g_test_add_func("/claude-launch/is-ollama/space-only-suffix",
	                test_is_ollama_space_only_suffix);
	g_test_add_func("/claude-launch/is-ollama/midstring-not-prefix",
	                test_is_ollama_midstring_not_prefix);
	g_test_add_func("/claude-launch/is-ollama/plain-claude-alias",
	                test_is_ollama_plain_claude_alias);

	g_test_add_func("/claude-launch/should-emit-model",
	                test_should_emit_model);

	g_test_add_func("/claude-launch/exe/claude-default",
	                test_executable_name_claude_default);
	g_test_add_func("/claude-launch/exe/claude-env",
	                test_executable_name_claude_env);
	g_test_add_func("/claude-launch/exe/ollama-default",
	                test_executable_name_ollama_default);
	g_test_add_func("/claude-launch/exe/ollama-env",
	                test_executable_name_ollama_env);
	g_test_add_func("/claude-launch/exe/ollama-ignores-claude-env",
	                test_executable_name_ollama_ignores_claude_env);
	g_test_add_func("/claude-launch/exe/null-model",
	                test_executable_name_null_model);

	g_test_add_func("/claude-launch/emit/claude-mode",
	                test_emit_tokens_claude_mode);
	g_test_add_func("/claude-launch/emit/ollama-mode",
	                test_emit_tokens_ollama_mode);
	g_test_add_func("/claude-launch/emit/empty-tail-claude",
	                test_emit_tokens_empty_tail_claude);
	g_test_add_func("/claude-launch/emit/empty-tail-ollama",
	                test_emit_tokens_empty_tail_ollama);
	g_test_add_func("/claude-launch/emit/program-token-used",
	                test_emit_tokens_program_token_used);

	return g_test_run();
}
