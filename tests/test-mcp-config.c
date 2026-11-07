/*
 * test-mcp-config.c - One MCP server list, in every CLI's dialect
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The translations must be accepted by the very clients they are for:
 * the codex test applies its output through the codex client's own
 * validator, and the grok test through its overlay, so a dialect that
 * renders and is then refused fails here rather than in an agent.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include "ai-glib.h"

static const gchar *SERVERS_JSON =
	"{\"mcpServers\":{"
	"\"clawtilla\":{\"command\":\"/usr/bin/clawtilla-mcp-server\","
	"\"args\":[\"--agent\",\"a b\",\"--token-file\",\"/s/t\"]},"
	"\"secret-api\":{\"command\":\"api-mcp\",\"env\":{\"API_TOKEN\":\"s3cret\"}},"
	"\"remote\":{\"type\":\"http\",\"url\":\"https://mcp.example/\"},"
	"\"has.dot\":{\"command\":\"x\"},"
	"\"broken\":{\"args\":[\"nothing to run\"]}"
	"}}";

static JsonObject *
servers_fixture(void)
{
	g_autoptr(JsonNode) root = json_from_string(SERVERS_JSON, NULL);

	return json_object_ref(json_object_get_object_member(json_node_get_object(root), "mcpServers"));
}

static gboolean
skipped_names(GPtrArray *skipped, const gchar *name)
{
	guint i;

	for (i = 0; i < skipped->len; i++)
		if (g_str_has_prefix(g_ptr_array_index(skipped, i), name))
			return TRUE;
	return FALSE;
}

/* codex carries command and args only, and never puts a secret in argv. */
static void
test_codex_dialect(void)
{
	g_autoptr(JsonObject) servers = servers_fixture();
	g_autoptr(GPtrArray) skipped = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = ai_mcp_config_translate(servers, AI_ENDPOINT_KIND_MCP_CONFIG_CODEX, skipped, &error);

	g_assert_no_error(error);
	g_assert_nonnull(strstr(text, "mcp_servers.clawtilla.command=\"/usr/bin/clawtilla-mcp-server\"\n"));
	g_assert_nonnull(strstr(text, "mcp_servers.clawtilla.args=[\"--agent\", \"a b\", \"--token-file\", \"/s/t\"]\n"));
	g_assert_nonnull(strstr(text, "mcp_servers.clawtilla.default_tools_approval_mode=\"approve\""));
	g_assert_null(strstr(text, "s3cret"));
	g_assert_null(strstr(text, "mcp.example"));
	g_assert_true(skipped_names(skipped, "secret-api:"));
	g_assert_true(skipped_names(skipped, "remote:"));
	g_assert_true(skipped_names(skipped, "has.dot:"));
	g_assert_true(skipped_names(skipped, "broken:"));
}

/* grok's TOML takes all of it: env tables and URLs. */
static void
test_grok_dialect(void)
{
	g_autoptr(JsonObject) servers = servers_fixture();
	g_autoptr(GPtrArray) skipped = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *text = ai_mcp_config_translate(servers, AI_ENDPOINT_KIND_MCP_CONFIG_GROK, skipped, NULL);

	g_assert_nonnull(strstr(text, "[mcp_servers.clawtilla]\ncommand = \"/usr/bin/clawtilla-mcp-server\"\n"));
	g_assert_nonnull(strstr(text, "[mcp_servers.secret-api.env]\n\"API_TOKEN\" = \"s3cret\"\n"));
	g_assert_nonnull(strstr(text, "[mcp_servers.remote]\nurl = \"https://mcp.example/\"\n"));
	g_assert_nonnull(strstr(text, "[mcp_servers.\"has.dot\"]"));
	g_assert_true(skipped_names(skipped, "broken:"));
	g_assert_cmpuint(skipped->len, ==, 1);
}

static void
test_opencode_dialect(void)
{
	g_autoptr(JsonObject) servers = servers_fixture();
	g_autofree gchar *text = ai_mcp_config_translate(servers, AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE, NULL, NULL);
	g_autoptr(JsonNode) root = json_from_string(text, NULL);
	JsonObject *mcp = json_object_get_object_member(json_node_get_object(root), "mcp");
	JsonObject *claw = json_object_get_object_member(mcp, "clawtilla");
	JsonObject *remote = json_object_get_object_member(mcp, "remote");
	JsonObject *secret = json_object_get_object_member(mcp, "secret-api");

	g_assert_cmpstr(json_object_get_string_member(claw, "type"), ==, "local");
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(claw, "command")), ==, 5);
	g_assert_cmpstr(json_object_get_string_member(remote, "type"), ==, "remote");
	g_assert_cmpstr(json_object_get_string_member(
		json_object_get_object_member(secret, "environment"), "API_TOKEN"), ==, "s3cret");
	g_assert_false(json_object_has_member(mcp, "broken"));
}

static void
test_unknown_dialect(void)
{
	g_autoptr(JsonObject) servers = servers_fixture();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = ai_mcp_config_translate(servers, "mcp-config-nobody", NULL, &error);

	g_assert_null(text);
	g_assert_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED);
}

/* Later files win, missing files are skipped, a malformed one is an error. */
static void
test_load_merges(void)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("ai-mcp-load-XXXXXX", &error);
	g_autofree gchar *a = g_build_filename(dir, "a.json", NULL);
	g_autofree gchar *b = g_build_filename(dir, "b.json", NULL);
	g_autofree gchar *missing = g_build_filename(dir, "none.json", NULL);
	const gchar *paths[] = { a, missing, b, NULL };
	g_autoptr(JsonObject) merged = NULL;

	g_assert_true(g_file_set_contents(a, "{\"mcpServers\":{\"x\":{\"command\":\"one\"},\"y\":{\"command\":\"y\"}}}", -1, NULL));
	g_assert_true(g_file_set_contents(b, "{\"mcpServers\":{\"x\":{\"command\":\"two\"}}}", -1, NULL));
	merged = ai_mcp_config_load_servers(paths, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_object_get_size(merged), ==, 2);
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(merged, "x"), "command"), ==, "two");

	g_assert_true(g_file_set_contents(b, "{\"mcpServers\":[]}", -1, NULL));
	g_clear_pointer(&merged, json_object_unref);
	merged = ai_mcp_config_load_servers(paths, &error);
	g_assert_null(merged);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);

	g_unlink(a);
	g_unlink(b);
	g_rmdir(dir);
}

/* The written codex file is accepted by the codex client's own validator. */
static void
test_endpoint_for_codex_applies(void)
{
	g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
	g_autoptr(JsonObject) servers = servers_fixture();
	g_autoptr(GPtrArray) skipped = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("ai-mcp-ep-XXXXXX", &error);
	g_autoptr(AiAgentEndpoint) endpoint = NULL;
	GStatBuf st;

	endpoint = ai_mcp_config_endpoint_for(AI_CLI_CLIENT(codex), servers, dir, skipped, &error);
	g_assert_no_error(error);
	g_assert_nonnull(endpoint);
	g_assert_cmpstr(endpoint->kind, ==, AI_ENDPOINT_KIND_MCP_CONFIG_CODEX);
	g_assert_cmpint(g_stat(endpoint->value, &st), ==, 0);
	g_assert_cmpint(st.st_mode & 0777, ==, 0600);
	g_assert_cmpuint(skipped->len, ==, 4);

	g_assert_true(ai_tool_endpoint_consumer_apply(AI_TOOL_ENDPOINT_CONSUMER(codex), endpoint, &error));
	g_assert_no_error(error);
	g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(codex), &error));

	g_unlink(endpoint->value);
	g_rmdir(dir);
}

/* grok composes the fragment into its overlay. */
static void
test_endpoint_for_grok_applies(void)
{
	g_autoptr(AiGrokBuildClient) grok = ai_grok_build_client_new();
	g_autoptr(JsonObject) servers = servers_fixture();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *dir = g_dir_make_tmp("ai-mcp-ep-XXXXXX", &error);
	g_autofree gchar *home = g_dir_make_tmp("ai-mcp-home-XXXXXX", &error);
	g_autoptr(AiAgentEndpoint) endpoint = NULL;
	g_autofree gchar *config = NULL;
	g_autofree gchar *contents = NULL;

	ai_cli_client_set_env(AI_CLI_CLIENT(grok), "GROK_HOME", home);
	endpoint = ai_mcp_config_endpoint_for(AI_CLI_CLIENT(grok), servers, dir, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(endpoint->kind, ==, AI_ENDPOINT_KIND_MCP_CONFIG_GROK);
	g_assert_true(ai_tool_endpoint_consumer_apply(AI_TOOL_ENDPOINT_CONSUMER(grok), endpoint, &error));
	g_assert_no_error(error);

	config = g_build_filename(ai_cli_client_get_env(AI_CLI_CLIENT(grok), "GROK_HOME"), "config.toml", NULL);
	g_assert_true(g_file_get_contents(config, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "[mcp_servers.clawtilla]"));

	g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(grok), &error));
	g_unlink(endpoint->value);
	g_rmdir(dir);
	g_rmdir(home);
}

/* Nothing that survives means no endpoint at all, not an empty grant. */
static void
test_endpoint_nothing_survives(void)
{
	g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
	g_autoptr(JsonNode) root = json_from_string("{\"s\":{\"command\":\"x\",\"env\":{\"K\":\"v\"}}}", NULL);
	g_autoptr(GPtrArray) skipped = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiAgentEndpoint) endpoint = NULL;

	endpoint = ai_mcp_config_endpoint_for(AI_CLI_CLIENT(codex), json_node_get_object(root),
	                                      "/nonexistent-never-written", skipped, &error);
	g_assert_no_error(error);
	g_assert_null(endpoint);
	g_assert_cmpuint(skipped->len, ==, 1);
}

gint
main(gint argc, gchar **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/mcp-config/codex", test_codex_dialect);
	g_test_add_func("/mcp-config/grok", test_grok_dialect);
	g_test_add_func("/mcp-config/opencode", test_opencode_dialect);
	g_test_add_func("/mcp-config/unknown", test_unknown_dialect);
	g_test_add_func("/mcp-config/load-merges", test_load_merges);
	g_test_add_func("/mcp-config/endpoint-codex", test_endpoint_for_codex_applies);
	g_test_add_func("/mcp-config/endpoint-grok", test_endpoint_for_grok_applies);
	g_test_add_func("/mcp-config/endpoint-empty", test_endpoint_nothing_survives);

	return g_test_run();
}
