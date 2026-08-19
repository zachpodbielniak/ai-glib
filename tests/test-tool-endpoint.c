/*
 * test-tool-endpoint.c - AiAgentEndpoint and AiToolEndpointConsumer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The contract under test is the one that lets a host grant tools to a
 * CLI provider without knowing which CLI it is: the host writes a
 * dialect, the provider decides delivery, and the flat `kind` string is
 * the whole negotiation between them.
 *
 * The per-provider delivery assertions matter more than they look.  Each
 * of the four wrappers carries its own mechanism -- a flag, an
 * environment variable, a directory -- and nothing but a test says that
 * a grant actually arrives rather than being accepted and dropped.  An
 * agent that believes it has tools and has none is the exact failure
 * this whole path exists to prevent.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "ai-glib.h"
#include "core/ai-error.h"
#include "providers/ai-grok-build-client-internal.h"

/* ------------------------------------------------------------------ */
/* The boxed type                                                      */
/* ------------------------------------------------------------------ */

static void
test_endpoint_round_trips(void)
{
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/x.json");
	g_autoptr(AiAgentEndpoint) copy = NULL;

	ai_agent_endpoint_set_env(ep, "TOKEN", "secret");
	ep->ttl_seconds = 60;

	copy = ai_agent_endpoint_copy(ep);

	g_assert_cmpstr(copy->kind, ==, AI_ENDPOINT_KIND_MCP_CONFIG);
	g_assert_cmpstr(copy->value, ==, "/tmp/x.json");
	g_assert_cmpint(copy->ttl_seconds, ==, 60);
	g_assert_cmpstr(g_hash_table_lookup(copy->env, "TOKEN"), ==, "secret");

	/* A deep copy: mutating the original must not reach the copy, or a
	 * revoked credential would still be live somewhere. */
	ai_agent_endpoint_set_env(ep, "TOKEN", "changed");
	g_assert_cmpstr(g_hash_table_lookup(copy->env, "TOKEN"), ==, "secret");
}

static void
test_endpoint_is_a_boxed_type(void)
{
	/* Bindings and g_object_get() both need this to be registered. */
	g_assert_true(G_TYPE_IS_BOXED(AI_TYPE_AGENT_ENDPOINT));
}

/* ------------------------------------------------------------------ */
/* Kind negotiation                                                    */
/* ------------------------------------------------------------------ */

static void
test_every_cli_takes_the_env_kind(void)
{
	/*
	 * The base applies AiAgentEndpoint.env at spawn for every subclass,
	 * so refusing the env kind anywhere would be a lie about what the
	 * client does.
	 */
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();

	g_assert_true(ai_tool_endpoint_consumer_supports_kind(
		AI_TOOL_ENDPOINT_CONSUMER(claude), AI_ENDPOINT_KIND_ENV));
	g_assert_true(ai_tool_endpoint_consumer_supports_kind(
		AI_TOOL_ENDPOINT_CONSUMER(opencode), AI_ENDPOINT_KIND_ENV));
	g_assert_true(ai_tool_endpoint_consumer_supports_kind(
		AI_TOOL_ENDPOINT_CONSUMER(grok), AI_ENDPOINT_KIND_ENV));
}

static void
test_kinds_are_not_interchangeable(void)
{
	/*
	 * The dialects genuinely differ -- claude nests under mcpServers,
	 * opencode under mcp with a different server shape, grok is TOML --
	 * so a client must refuse a kind it cannot read rather than accept
	 * it and hand the CLI a file it will ignore.
	 */
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();

	g_assert_true(ai_tool_endpoint_consumer_supports_kind(
		AI_TOOL_ENDPOINT_CONSUMER(claude), AI_ENDPOINT_KIND_MCP_CONFIG));
	g_assert_false(ai_tool_endpoint_consumer_supports_kind(
		AI_TOOL_ENDPOINT_CONSUMER(claude),
		AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE));

	g_assert_true(ai_tool_endpoint_consumer_supports_kind(
		AI_TOOL_ENDPOINT_CONSUMER(opencode),
		AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE));
	g_assert_false(ai_tool_endpoint_consumer_supports_kind(
		AI_TOOL_ENDPOINT_CONSUMER(opencode), AI_ENDPOINT_KIND_MCP_CONFIG));
}

static void
test_an_unsupported_kind_is_an_error(void)
{
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/x.json");
	g_autoptr(GError) error = NULL;

	g_assert_false(ai_tool_endpoint_consumer_apply(
		AI_TOOL_ENDPOINT_CONSUMER(opencode), ep, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);

	/* And nothing was stored: a refused grant must leave no trace. */
	g_assert_null(ai_tool_endpoint_consumer_get_endpoint(
		AI_TOOL_ENDPOINT_CONSUMER(opencode)));
}

/* ------------------------------------------------------------------ */
/* Per-provider delivery                                               */
/* ------------------------------------------------------------------ */

static void
test_claude_code_delivers_via_mcp_config_path(void)
{
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/x.json");
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_tool_endpoint_consumer_apply(
		AI_TOOL_ENDPOINT_CONSUMER(claude), ep, &error));
	g_assert_no_error(error);

	/* The interface is a second door onto the existing property, not a
	 * parallel path that could disagree with it. */
	g_assert_cmpstr(ai_claude_code_client_get_mcp_config_path(claude), ==,
	                "/tmp/x.json");

	g_assert_true(ai_tool_endpoint_consumer_clear(
		AI_TOOL_ENDPOINT_CONSUMER(claude), &error));
	g_assert_no_error(error);
	g_assert_null(ai_claude_code_client_get_mcp_config_path(claude));
}

static void
test_opencode_delivers_via_the_environment(void)
{
	/*
	 * opencode 1.18.18 has no --mcp-config; OPENCODE_CONFIG is the only
	 * per-invocation lever, so the grant has to arrive as an environment
	 * variable or not at all.
	 */
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE,
		                      "/tmp/oc.json");
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_tool_endpoint_consumer_apply(
		AI_TOOL_ENDPOINT_CONSUMER(opencode), ep, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode),
	                                      "OPENCODE_CONFIG"), ==,
	                "/tmp/oc.json");

	g_assert_true(ai_tool_endpoint_consumer_clear(
		AI_TOOL_ENDPOINT_CONSUMER(opencode), &error));
	g_assert_null(ai_cli_client_get_env(AI_CLI_CLIENT(opencode),
	                                    "OPENCODE_CONFIG"));
}

static void
test_grok_delivers_via_a_home_overlay(void)
{
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *fragment = NULL;
	g_autofree gchar *dir = NULL;
	const gchar *home;

	dir = g_dir_make_tmp("ai-glib-frag-XXXXXX", &error);
	g_assert_no_error(error);
	fragment = g_build_filename(dir, "frag.toml", NULL);
	g_assert_true(g_file_set_contents(
		fragment, "[mcp_servers.probe]\ncommand = \"true\"\n", -1, &error));

	{
		g_autoptr(AiAgentEndpoint) ep =
			ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG_GROK,
			                      fragment);

		g_assert_true(ai_tool_endpoint_consumer_apply(
			AI_TOOL_ENDPOINT_CONSUMER(grok), ep, &error));
		g_assert_no_error(error);
	}

	home = ai_cli_client_get_env(AI_CLI_CLIENT(grok), "GROK_HOME");
	g_assert_nonnull(home);
	g_assert_true(g_file_test(home, G_FILE_TEST_IS_DIR));

	/* The fragment reached the overlay's config, which is the only thing
	 * that makes the server visible to grok. */
	{
		g_autofree gchar *config = g_build_filename(home, "config.toml",
		                                            NULL);
		g_autofree gchar *body = NULL;

		g_assert_true(g_file_get_contents(config, &body, NULL, &error));
		g_assert_nonnull(strstr(body, "[mcp_servers.probe]"));
	}

	{
		g_autofree gchar *kept = g_strdup(home);

		g_assert_true(ai_tool_endpoint_consumer_clear(
			AI_TOOL_ENDPOINT_CONSUMER(grok), &error));
		g_assert_no_error(error);

		/* Revoke removes the overlay: a directory per run that nothing
		 * cleans up is a leak with a credential inside it. */
		g_assert_false(g_file_test(kept, G_FILE_TEST_EXISTS));
		g_assert_null(ai_cli_client_get_env(AI_CLI_CLIENT(grok),
		                                    "GROK_HOME"));
	}

	g_unlink(fragment);
	g_rmdir(dir);
}

static void
test_grok_argv_stays_on_the_top_level_command(void)
{
	/*
	 * A regression guard with a reason.  `grok agent --plugin-dir` reads
	 * like the obvious way to inject MCP servers, and it is documented
	 * as exactly that -- but the `agent` subcommand has no
	 * --prompt-file, no --output-format, no --resume and no
	 * --permission-mode: it is the ACP transport, a different protocol.
	 * Moving argv there would discard the argv builder, session handling
	 * and the entire streaming parser in exchange for one flag.
	 *
	 * Anyone reading only the --plugin-dir help text will be tempted.
	 * This fails when they try.
	 */
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();
	g_autoptr(AiMessage) msg = ai_message_new_user("hi");
	GList *messages = g_list_append(NULL, msg);
	g_auto(GStrv) argv = NULL;
	gsize i;

	argv = ai_grok_build_client_build_argv(AI_CLI_CLIENT(grok), messages,
	                                       NULL, 256, FALSE);
	g_list_free(messages);

	g_assert_nonnull(argv);

	for (i = 0; argv[i] != NULL; i++)
	{
		g_assert_cmpstr(argv[i], !=, "agent");
		g_assert_cmpstr(argv[i], !=, "--plugin-dir");
	}
}

/* ------------------------------------------------------------------ */
/* Environment                                                         */
/* ------------------------------------------------------------------ */

static void
test_endpoint_env_does_not_clobber_caller_env(void)
{
	/*
	 * Two tables on purpose: revoking an endpoint must not strip a
	 * variable the caller set, and a scoped credential must win over a
	 * general setting of the same name while it is in force.
	 */
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_ENV, NULL);
	g_autoptr(GError) error = NULL;

	ai_cli_client_set_env(AI_CLI_CLIENT(opencode), "KEEP", "mine");
	ai_agent_endpoint_set_env(ep, "GRANT", "scoped");

	g_assert_true(ai_tool_endpoint_consumer_apply(
		AI_TOOL_ENDPOINT_CONSUMER(opencode), ep, &error));
	g_assert_no_error(error);

	/* The endpoint's own variables are not in the caller's table ... */
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "KEEP"),
	                ==, "mine");
	g_assert_null(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "GRANT"));

	/* ... and revoking leaves the caller's alone. */
	g_assert_true(ai_tool_endpoint_consumer_clear(
		AI_TOOL_ENDPOINT_CONSUMER(opencode), &error));
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "KEEP"),
	                ==, "mine");
}

static void
test_notify_fires_on_apply(void)
{
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/x.json");
	g_autoptr(GError) error = NULL;
	guint notified = 0;

	/* notify::tool-endpoint is the change signal.  A bespoke
	 * ::endpoint-applied would carry nothing this does not. */
	g_signal_connect_swapped(claude, "notify::tool-endpoint",
	                         G_CALLBACK(g_atomic_int_inc), &notified);

	g_assert_true(ai_tool_endpoint_consumer_apply(
		AI_TOOL_ENDPOINT_CONSUMER(claude), ep, &error));
	g_assert_cmpuint(notified, ==, 1);

	g_assert_true(ai_tool_endpoint_consumer_clear(
		AI_TOOL_ENDPOINT_CONSUMER(claude), &error));
	g_assert_cmpuint(notified, ==, 2);
}

/* ------------------------------------------------------------------ */
/* Through a conversation                                              */
/* ------------------------------------------------------------------ */

static void
test_conversation_refuses_an_http_provider(void)
{
	/*
	 * An HTTP provider hosts no tools to point anywhere, so this has to
	 * fail loudly.  Succeeding quietly is how a caller ends up believing
	 * it granted tools that never existed -- the same class of bug as a
	 * knob that does nothing.
	 */
	g_autoptr(AiMockProvider) mock = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(mock));
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/x.json");
	g_autoptr(GError) error = NULL;

	g_assert_false(ai_conversation_set_tool_endpoint(conversation, ep,
	                                                 &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

static void
test_conversation_hands_a_cli_provider_its_endpoint(void)
{
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiConversation) conversation =
		ai_conversation_new(G_OBJECT(claude));
	g_autoptr(AiAgentEndpoint) ep =
		ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, "/tmp/x.json");
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_conversation_set_tool_endpoint(conversation, ep,
	                                                &error));
	g_assert_no_error(error);
	g_assert_cmpstr(ai_claude_code_client_get_mcp_config_path(claude), ==,
	                "/tmp/x.json");
	g_assert_nonnull(ai_conversation_get_tool_endpoint(conversation));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/tool-endpoint/round-trip",
	                test_endpoint_round_trips);
	g_test_add_func("/ai-glib/tool-endpoint/boxed",
	                test_endpoint_is_a_boxed_type);
	g_test_add_func("/ai-glib/tool-endpoint/env-kind-everywhere",
	                test_every_cli_takes_the_env_kind);
	g_test_add_func("/ai-glib/tool-endpoint/kinds-differ",
	                test_kinds_are_not_interchangeable);
	g_test_add_func("/ai-glib/tool-endpoint/unsupported-kind",
	                test_an_unsupported_kind_is_an_error);
	g_test_add_func("/ai-glib/tool-endpoint/claude-code",
	                test_claude_code_delivers_via_mcp_config_path);
	g_test_add_func("/ai-glib/tool-endpoint/opencode",
	                test_opencode_delivers_via_the_environment);
	g_test_add_func("/ai-glib/tool-endpoint/grok-overlay",
	                test_grok_delivers_via_a_home_overlay);
	g_test_add_func("/ai-glib/tool-endpoint/grok-argv-guard",
	                test_grok_argv_stays_on_the_top_level_command);
	g_test_add_func("/ai-glib/tool-endpoint/env-tables-separate",
	                test_endpoint_env_does_not_clobber_caller_env);
	g_test_add_func("/ai-glib/tool-endpoint/notify",
	                test_notify_fires_on_apply);
	g_test_add_func("/ai-glib/tool-endpoint/conversation-refuses-http",
	                test_conversation_refuses_an_http_provider);
	g_test_add_func("/ai-glib/tool-endpoint/conversation-cli",
	                test_conversation_hands_a_cli_provider_its_endpoint);

	return g_test_run();
}
