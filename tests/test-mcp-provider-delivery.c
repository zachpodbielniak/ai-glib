/*
 * test-mcp-provider-delivery.c - Scoped CLI MCP configuration delivery
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <glib.h>
#include <glib/gstdio.h>
#include "ai-glib.h"

/* Write only a test-owned temporary file and return its absolute name. */
static gchar *
write_fragment(const gchar *contents)
{
	g_autoptr(GError) error = NULL;
	gchar *path = NULL;
	gint fd = g_file_open_tmp("ai-mcp-provider-XXXXXX", &path, &error);

	g_assert_no_error(error);
	g_assert_cmpint(fd, >=, 0);
	g_assert_true(g_close(fd, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_set_contents(path, contents, -1, &error));
	g_assert_no_error(error);
	return path;
}

/* All overrides share exec scope: a later exec -c replaces parent values. */
static void
assert_codex_overrides(AiCliClient *client, gboolean expected)
{
	g_auto(GStrv) argv = AI_CLI_CLIENT_GET_CLASS(client)->build_argv(client, NULL, NULL, 0, TRUE);
	gboolean saw_exec = FALSE;
	gboolean saw_resume = FALSE;
	gboolean saw_effort = FALSE;
	guint overrides = 0;
	guint i;

	g_assert_nonnull(argv);
	for (i = 0; argv[i] != NULL; i++)
	{
		if (g_str_equal(argv[i], "exec")) saw_exec = TRUE;
		if (g_str_equal(argv[i], "resume")) saw_resume = TRUE;
		if (g_str_equal(argv[i], "-c"))
		{
			g_assert_nonnull(argv[i + 1]);
			g_assert_true(saw_exec);
			g_assert_false(saw_resume);
			if (g_str_has_prefix(argv[i + 1], "model_reasoning_effort=")) saw_effort = TRUE;
			if (!g_str_has_prefix(argv[i + 1], "mcp_servers.")) continue;
			if (overrides == 0)
				g_assert_cmpstr(argv[i + 1], ==, "mcp_servers.ai_host.command=\"/path with spaces/ai-tui\"");
			else if (overrides == 1)
				g_assert_cmpstr(argv[i + 1], ==, "mcp_servers.ai_host.args=[\"--mcp-connect\",\"/tmp/quote\\\"socket\"]");
			else if (overrides == 2)
				g_assert_cmpstr(argv[i + 1], ==, "mcp_servers.ai_host.required=true");
			else
				g_assert_cmpstr(argv[i + 1], ==, "mcp_servers.ai_host.default_tools_approval_mode=\"approve\"");
			overrides++;
		}
	}
	g_assert_cmpuint(overrides, ==, expected ? 4 : 0);
	g_assert_cmpint(saw_effort, ==, *ai_cli_client_get_effort_level(client) != '\0');
}

/* Snapshotting the file and replacing/clearing the grant must be deterministic. */
static void
test_codex_scoped_delivery(void)
{
	g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
	AiCliClient *client = AI_CLI_CLIENT(codex);
	AiToolEndpointConsumer *consumer = AI_TOOL_ENDPOINT_CONSUMER(codex);
	g_autofree gchar *path = write_fragment(
		"# host tools\n\nmcp_servers.ai_host.command = \"/path with spaces/ai-tui\"\n"
		"mcp_servers.ai_host.args=[\"--mcp-connect\",\"/tmp/quote\\\"socket\"]\n"
		"mcp_servers.ai_host.required=true\n"
		"mcp_servers.ai_host.default_tools_approval_mode=\"approve\"\n");
	g_autoptr(AiAgentEndpoint) endpoint = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_CODEX, path);
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_tool_endpoint_consumer_supports_kind(consumer, AI_ENDPOINT_KIND_MCP_CONFIG_CODEX));
	g_assert_true(ai_tool_endpoint_consumer_apply(consumer, endpoint, &error));
	g_assert_no_error(error);
	g_assert_cmpint(g_unlink(path), ==, 0);
	assert_codex_overrides(client, TRUE);
	ai_cli_client_set_session_persistence(client, TRUE);
	ai_cli_client_set_session_id(client, "session-id");
	assert_codex_overrides(client, TRUE);
	{
		g_auto(GStrv) argv = AI_CLI_CLIENT_GET_CLASS(client)->build_argv(client, NULL,
			"Use ai_host.todo_write for the live TODO panel", 0, TRUE);
		g_autofree gchar *prompt = AI_CLI_CLIENT_GET_CLASS(client)->build_stdin(client, NULL);
		/* Resumed sessions must receive guidance for the current scoped grant. */
		g_assert_nonnull(argv);
		g_assert_nonnull(strstr(prompt, "Use ai_host.todo_write"));
	}
	g_assert_true(ai_tool_endpoint_consumer_clear(consumer, &error));
	g_assert_no_error(error);
	assert_codex_overrides(client, FALSE);
	g_assert_null(ai_tool_endpoint_consumer_get_endpoint(consumer));
}

/* Invalid replacement removes the old grant and never emits non-MCP overrides. */
static void
test_codex_rejects_invalid(void)
{
	const gchar *invalid[] = {
		"", "# empty\n", "sandbox_mode=\"danger-full-access\"",
		"[mcp_servers.ai_host]\ncommand=\"ai\"", "mcp_servers.ai_host.command=123",
		"mcp_servers.ai_host.command=\"\"", "mcp_servers.ai_host.args=[1]",
		"mcp_servers.ai_host.args={}", "mcp_servers.ai_host.args=[]", "mcp_servers.ai_host.env=\"secret\"",
		"mcp_servers..command=\"ai\"", "mcp_servers.a b.command=\"ai\"",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.command=\"other\"",
		"mcp_servers.ai_host.command=\"ai\" # comment",
		"mcp_servers.ai_host.required=true",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.required=1",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.required=\"true\"",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.required=[]",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.required=null",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.required=true\nmcp_servers.ai_host.required=false",
		"mcp_servers.ai_host.default_tools_approval_mode=\"approve\"",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.default_tools_approval_mode=true",
		"mcp_servers.ai_host.command=\"ai\"\nmcp_servers.ai_host.default_tools_approval_mode=\"unknown\"", NULL
	};
	g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
	AiToolEndpointConsumer *consumer = AI_TOOL_ENDPOINT_CONSUMER(codex);
	guint i;

	for (i = 0; invalid[i] != NULL; i++)
	{
		g_autofree gchar *path = write_fragment(invalid[i]);
		g_autoptr(AiAgentEndpoint) endpoint = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_CODEX, path);
		g_autoptr(GError) error = NULL;
		g_autofree gchar *valid_path = write_fragment("mcp_servers.ai_host.command=\"ai\"");
		g_autoptr(AiAgentEndpoint) valid = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_CODEX, valid_path);

		g_assert_true(ai_tool_endpoint_consumer_apply(consumer, valid, &error));
		g_assert_no_error(error);
		g_assert_false(ai_tool_endpoint_consumer_apply(consumer, endpoint, &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_null(ai_tool_endpoint_consumer_get_endpoint(consumer));
		assert_codex_overrides(AI_CLI_CLIENT(codex), FALSE);
		g_assert_cmpint(g_unlink(path), ==, 0);
		g_assert_cmpint(g_unlink(valid_path), ==, 0);
	}
}

/* Bound the file read and reject missing paths and embedded NUL bytes. */
static void
test_codex_file_errors(void)
{
	g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
	g_autofree gchar *large = g_strnfill(65537, 'x');
	g_autofree gchar *path = write_fragment(large);
	g_autoptr(AiAgentEndpoint) endpoint = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_CODEX, path);
	g_autoptr(GError) error = NULL;
	const gchar binary[] = "mcp_servers.ai_host.command=\"ai\"\0hidden";
	AiToolEndpointConsumer *consumer = AI_TOOL_ENDPOINT_CONSUMER(codex);

	g_assert_false(ai_tool_endpoint_consumer_apply(consumer, endpoint, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	g_assert_true(g_file_set_contents(path, binary, sizeof binary - 1, &error));
	g_assert_no_error(error);
	g_assert_false(ai_tool_endpoint_consumer_apply(consumer, endpoint, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	g_assert_cmpint(g_unlink(path), ==, 0);
	g_assert_false(ai_tool_endpoint_consumer_apply(consumer, endpoint, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
	g_assert_null(ai_tool_endpoint_consumer_get_endpoint(consumer));
}

/* Grok must copy the explicit caller home, then restore it and remove the overlay. */
static void
test_grok_restore(void)
{
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *home = g_dir_make_tmp("ai-mcp-grok-XXXXXX", &error);
	g_autofree gchar *config = NULL;
	g_autofree gchar *fragment = write_fragment("[mcp_servers.ai_host]\ncommand=\"ai\"\n");
	g_autofree gchar *overlay = NULL;
	g_autofree gchar *overlay_config = NULL;
	g_autofree gchar *contents = NULL;
	g_autoptr(AiAgentEndpoint) endpoint = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_GROK, fragment);
	AiCliClient *client = AI_CLI_CLIENT(grok);
	AiToolEndpointConsumer *consumer = AI_TOOL_ENDPOINT_CONSUMER(grok);

	g_assert_no_error(error);
	config = g_build_filename(home, "config.toml", NULL);
	g_assert_true(g_file_set_contents(config, "model=\"saved\"\n", -1, &error));
	ai_cli_client_set_env(client, "GROK_HOME", home);
	g_assert_true(ai_tool_endpoint_consumer_apply(consumer, endpoint, &error));
	g_assert_no_error(error);
	overlay = g_strdup(ai_cli_client_get_env(client, "GROK_HOME"));
	g_assert_cmpstr(overlay, !=, home);
	overlay_config = g_build_filename(overlay, "config.toml", NULL);
	g_assert_true(g_file_get_contents(overlay_config, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(contents, "model=\"saved\""));
	g_assert_nonnull(strstr(contents, "[mcp_servers.ai_host]"));
	g_assert_true(ai_tool_endpoint_consumer_clear(consumer, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(ai_cli_client_get_env(client, "GROK_HOME"), ==, home);
	g_assert_false(g_file_test(overlay, G_FILE_TEST_EXISTS));
	g_assert_cmpint(g_unlink(config), ==, 0);
	g_assert_cmpint(g_rmdir(home), ==, 0);
	g_assert_cmpint(g_unlink(fragment), ==, 0);
}

/* Endpoint revocation must restore pre-existing caller configuration. */
static void
test_restore_caller_config(void)
{
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiClaudeTmuxClient) tmux = ai_claude_tmux_client_new();
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(AiAgentEndpoint) claude_ep = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/host.json");
	g_autoptr(AiAgentEndpoint) open_ep = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE, "/tmp/host.json");
	g_autoptr(GError) error = NULL;
	guint i;

	ai_claude_code_client_set_mcp_config_path(claude, "/caller/claude.json");
	ai_claude_tmux_client_set_mcp_config_path(tmux, "/caller/tmux.json");
	ai_cli_client_set_env(AI_CLI_CLIENT(opencode), "OPENCODE_CONFIG", "/caller/open.json");
	for (i = 0; i < 2; i++)
	{
		g_assert_true(ai_tool_endpoint_consumer_apply(AI_TOOL_ENDPOINT_CONSUMER(claude), claude_ep, &error));
		g_assert_true(ai_tool_endpoint_consumer_apply(AI_TOOL_ENDPOINT_CONSUMER(tmux), claude_ep, &error));
		g_assert_true(ai_tool_endpoint_consumer_apply(AI_TOOL_ENDPOINT_CONSUMER(opencode), open_ep, &error));
		g_assert_no_error(error);
	}
	g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(claude), &error));
	g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(tmux), &error));
	g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(opencode), &error));
	g_assert_no_error(error);
	g_assert_cmpstr(ai_claude_code_client_get_mcp_config_path(claude), ==, "/caller/claude.json");
	g_assert_cmpstr(ai_claude_tmux_client_get_mcp_config_path(tmux), ==, "/caller/tmux.json");
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "OPENCODE_CONFIG"), ==, "/caller/open.json");
	/* A second clear must not strip the restored settings. */
	g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(opencode), &error));
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "OPENCODE_CONFIG"), ==, "/caller/open.json");
}

/* No unverified CLI capability may silently accept and discard a host grant. */
static void
test_unsupported_providers(void)
{
	g_autoptr(AiCursorClient) cursor = ai_cursor_client_new();
	g_autoptr(AiAntigravityClient) agy = ai_antigravity_client_new();
	AiToolEndpointConsumer *consumers[] = { AI_TOOL_ENDPOINT_CONSUMER(cursor), AI_TOOL_ENDPOINT_CONSUMER(agy) };
	g_autoptr(AiAgentEndpoint) ep = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/host.json");
	guint i;

	for (i = 0; i < G_N_ELEMENTS(consumers); i++)
	{
		g_autoptr(GError) error = NULL;
		g_assert_false(ai_tool_endpoint_consumer_apply(consumers[i], ep, &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "does not take"));
		g_assert_null(ai_tool_endpoint_consumer_get_endpoint(consumers[i]));
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/mcp/provider/codex-scoped-delivery", test_codex_scoped_delivery);
	g_test_add_func("/mcp/provider/codex-invalid", test_codex_rejects_invalid);
	g_test_add_func("/mcp/provider/codex-file-errors", test_codex_file_errors);
	g_test_add_func("/mcp/provider/restore-caller-config", test_restore_caller_config);
	g_test_add_func("/mcp/provider/grok-restore", test_grok_restore);
	g_test_add_func("/mcp/provider/unsupported", test_unsupported_providers);
	return g_test_run();
}
