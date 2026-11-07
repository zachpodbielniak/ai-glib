/*
 * test-cli-config-dir.c - Running a CLI under a separate account
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ai_cli_client_set_config_dir() points each CLI's own configuration
 * variable at an account directory.  These tests pin which variable each
 * CLI gets, that a CLI with no such variable refuses instead of silently
 * using the default login, that an endpoint built from the same variable
 * follows the account, and that claude-tmux's pane -- whose parent is the
 * shared tmux server, not this process -- receives the variables at all.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "ai-glib.h"
#include "core/ai-cli-client-private.h"
#include "providers/ai-claude-tmux-client-internal.h"

/* Each CLI's account variables, as the class declares them. */
static void
test_env_per_provider(void)
{
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiClaudeTmuxClient) tmux = ai_claude_tmux_client_new();
	g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(claude), "/acct/a", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(claude), "CLAUDE_CONFIG_DIR"), ==, "/acct/a");
	g_assert_cmpstr(ai_cli_client_get_config_dir(AI_CLI_CLIENT(claude)), ==, "/acct/a");

	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(tmux), "/acct/a", &error));
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(tmux), "CLAUDE_CONFIG_DIR"), ==, "/acct/a");

	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(codex), "/acct/c", &error));
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(codex), "CODEX_HOME"), ==, "/acct/c");

	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(grok), "/acct/g", &error));
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(grok), "GROK_HOME"), ==, "/acct/g");

	/* opencode has no single home: all three XDG roots move. */
	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(opencode), "/acct/o", &error));
	g_assert_no_error(error);
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "XDG_CONFIG_HOME"), ==, "/acct/o/config");
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "XDG_DATA_HOME"), ==, "/acct/o/data");
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "XDG_STATE_HOME"), ==, "/acct/o/state");
}

/* Clearing the account removes exactly what setting it added. */
static void
test_clear_restores_default(void)
{
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(GError) error = NULL;

	ai_cli_client_set_env(AI_CLI_CLIENT(opencode), "OPENCODE_CONFIG", "/caller/kept.json");
	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(opencode), "/acct/o", &error));
	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(opencode), NULL, &error));
	g_assert_no_error(error);
	g_assert_null(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "XDG_CONFIG_HOME"));
	g_assert_null(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "XDG_DATA_HOME"));
	g_assert_null(ai_cli_client_get_config_dir(AI_CLI_CLIENT(opencode)));
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "OPENCODE_CONFIG"), ==, "/caller/kept.json");
}

/*
 * cursor-agent and antigravity keep their login under $HOME.  Accepting a
 * directory and running the default account anyway would be a lie about
 * whose allowance is being spent.
 */
static void
test_home_keyed_cli_refuses(void)
{
	g_autoptr(AiCursorClient) cursor = ai_cursor_client_new();
	g_autoptr(AiAntigravityClient) agy = ai_antigravity_client_new();
	g_autoptr(GError) error = NULL;

	g_assert_false(ai_cli_client_supports_config_dir(AI_CLI_CLIENT(cursor)));
	g_assert_false(ai_cli_client_set_config_dir(AI_CLI_CLIENT(cursor), "/acct/x", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
	g_assert_nonnull(strstr(error->message, "$HOME"));
	g_clear_error(&error);

	g_assert_false(ai_cli_client_set_config_dir(AI_CLI_CLIENT(agy), "/acct/x", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
	g_clear_error(&error);

	/* Clearing is always fine: it asks for the default account. */
	g_assert_true(ai_cli_client_set_config_dir(AI_CLI_CLIENT(cursor), NULL, &error));
	g_assert_no_error(error);
}

/*
 * grok's MCP overlay is built from GROK_HOME.  Moving the account while an
 * endpoint is in force must rebuild the overlay from the new account, not
 * leave the old one in place or overwrite the overlay with the bare
 * account directory.
 */
static void
test_grok_endpoint_follows_account(void)
{
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();
	AiCliClient *client = AI_CLI_CLIENT(grok);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *first = g_dir_make_tmp("ai-acct-first-XXXXXX", &error);
	g_autofree gchar *second = g_dir_make_tmp("ai-acct-second-XXXXXX", NULL);
	g_autofree gchar *fragment = NULL;
	g_autofree gchar *second_config = g_build_filename(second, "config.toml", NULL);
	g_autofree gchar *overlay_config = NULL;
	g_autofree gchar *contents = NULL;
	g_autoptr(AiAgentEndpoint) endpoint = NULL;
	const gchar *overlay;
	gint fd;

	g_assert_no_error(error);
	fd = g_file_open_tmp("ai-acct-frag-XXXXXX.toml", &fragment, &error);
	g_assert_no_error(error);
	close(fd);
	g_assert_true(g_file_set_contents(fragment, "[mcp_servers.ai_host]\ncommand=\"ai\"\n", -1, NULL));
	g_assert_true(g_file_set_contents(second_config, "model=\"second\"\n", -1, NULL));

	endpoint = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_GROK, fragment);
	g_assert_true(ai_cli_client_set_config_dir(client, first, &error));
	g_assert_true(ai_tool_endpoint_consumer_apply(AI_TOOL_ENDPOINT_CONSUMER(grok), endpoint, &error));
	g_assert_no_error(error);

	g_assert_true(ai_cli_client_set_config_dir(client, second, &error));
	g_assert_no_error(error);
	overlay = ai_cli_client_get_env(client, "GROK_HOME");
	g_assert_cmpstr(overlay, !=, second);
	g_assert_cmpstr(overlay, !=, first);
	overlay_config = g_build_filename(overlay, "config.toml", NULL);
	g_assert_true(g_file_get_contents(overlay_config, &contents, NULL, &error));
	g_assert_nonnull(strstr(contents, "model=\"second\""));
	g_assert_nonnull(strstr(contents, "[mcp_servers.ai_host]"));

	/* Revoking lands on the account, not on the first one. */
	g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(grok), &error));
	g_assert_cmpstr(ai_cli_client_get_env(client, "GROK_HOME"), ==, second);

	g_assert_cmpint(g_unlink(second_config), ==, 0);
	g_assert_cmpint(g_rmdir(second), ==, 0);
	g_assert_cmpint(g_rmdir(first), ==, 0);
	g_assert_cmpint(g_unlink(fragment), ==, 0);
}

/* The pane's claude is the tmux server's child: -e is the only road in. */
static void
test_tmux_session_env(void)
{
	g_autoptr(GPtrArray) argv = NULL;
	g_autoptr(GHashTable) caller = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(GHashTable) endpoint = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	guint i;
	gint new_session = -1;
	gint dash_dash = -1;
	gboolean saw_account = FALSE;
	gboolean saw_token = FALSE;

	g_hash_table_insert(caller, g_strdup("CLAUDE_CONFIG_DIR"), g_strdup("/acct/a"));
	g_hash_table_insert(endpoint, g_strdup("CALLBACK_TOKEN"), g_strdup("t"));

	argv = ai_claude_tmux_client_build_session_argv("tmux", "sock", "sess", "/w", "claude",
	                                                FALSE, "id", "/s.json", NULL, NULL,
	                                                TRUE, NULL, NULL);
	ai_claude_tmux_client_insert_session_env(argv, caller, endpoint);

	for (i = 0; i < argv->len && g_ptr_array_index(argv, i) != NULL; i++)
	{
		const gchar *arg = g_ptr_array_index(argv, i);

		if (g_strcmp0(arg, "new-session") == 0)
			new_session = (gint)i;
		if (g_strcmp0(arg, "--") == 0 && dash_dash < 0)
			dash_dash = (gint)i;
		if (g_strcmp0(arg, "CLAUDE_CONFIG_DIR=/acct/a") == 0)
		{
			saw_account = TRUE;
			g_assert_cmpstr(g_ptr_array_index(argv, i - 1), ==, "-e");
			g_assert_cmpint((gint)i, >, new_session);
			g_assert_cmpint(dash_dash, <, 0);   /* before the command */
		}
		if (g_strcmp0(arg, "CALLBACK_TOKEN=t") == 0)
			saw_token = TRUE;
	}

	g_assert_true(saw_account);
	g_assert_true(saw_token);
	/* Still NULL-terminated for g_subprocess_newv(). */
	g_assert_null(g_ptr_array_index(argv, argv->len - 1));
}

/* A host hook joins the client's readiness hooks instead of replacing them. */
static void
test_tmux_settings_merge(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *merged = ai_claude_tmux_client_merge_settings(
		"{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"done\"}]}]}}",
		"{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"mine\"}]}],"
		"\"PreToolUse\":[{\"matcher\":\"*\",\"hooks\":[{\"type\":\"command\",\"command\":\"gate\"}]}]},"
		"\"model\":\"x\"}", &error);
	g_autoptr(JsonNode) root = NULL;
	JsonObject *hooks;

	g_assert_no_error(error);
	root = json_from_string(merged, NULL);
	hooks = json_object_get_object_member(json_node_get_object(root), "hooks");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(hooks, "Stop")), ==, 2);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(hooks, "PreToolUse")), ==, 1);
	g_assert_cmpstr(json_object_get_string_member(json_node_get_object(root), "model"), ==, "x");

	g_free(ai_claude_tmux_client_merge_settings("{}", "[]", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/cli/config-dir/env-per-provider", test_env_per_provider);
	g_test_add_func("/cli/config-dir/clear", test_clear_restores_default);
	g_test_add_func("/cli/config-dir/home-keyed-refuses", test_home_keyed_cli_refuses);
	g_test_add_func("/cli/config-dir/grok-endpoint-follows", test_grok_endpoint_follows_account);
	g_test_add_func("/cli/config-dir/tmux-session-env", test_tmux_session_env);
	g_test_add_func("/cli/config-dir/tmux-settings-merge", test_tmux_settings_merge);

	return g_test_run();
}
