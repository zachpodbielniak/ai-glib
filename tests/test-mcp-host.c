/*
 * Host contract tests: real executor/panels, no model/network calls.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mcp/ai-mcp-host.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

/* A consumer that advertises a dialect but refuses application exercises the
 * failure after configuration generation, not just an unsupported-kind check. */
typedef struct { AiCliClient parent_instance; } RejectingCli;
typedef struct { AiCliClientClass parent_class; } RejectingCliClass;
GType rejecting_cli_get_type(void);
G_DEFINE_TYPE_WITH_CODE(RejectingCli, rejecting_cli, AI_TYPE_CLI_CLIENT,
	G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, NULL))

static gboolean
reject_endpoint(AiCliClient *client, const AiAgentEndpoint *endpoint, GError **error)
{
	(void)client;
	if (endpoint == NULL) return TRUE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Fixture endpoint refusal");
	return FALSE;
}

static void
rejecting_cli_class_init(RejectingCliClass *klass)
{
	static const gchar * const kinds[] = { AI_ENDPOINT_KIND_MCP_CONFIG_CODEX, NULL };
	AI_CLI_CLIENT_CLASS(klass)->endpoint_kinds = kinds;
	AI_CLI_CLIENT_CLASS(klass)->endpoint_applied = reject_endpoint;
}

static void
rejecting_cli_init(RejectingCli *self)
{
	(void)self;
}

/* Every fixture owns a real conversation so state assertions observe the same
 * executor and transcript used by ai-tui, not a second MCP-only state store. */
typedef struct
{
	AiConversation *conversation;
	AiMcpHost *host;
	McpServer *server;
} Fixture;

static void
setup(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiOllamaClient) provider = ai_ollama_client_new();
	g_autoptr(GError) error = NULL;
	const gchar * const *tools = (const gchar * const *)data;
	fixture->conversation = ai_conversation_new(G_OBJECT(provider));
	ai_conversation_enable_background_agents(fixture->conversation, 4);
	fixture->host = ai_mcp_host_new(fixture->conversation, "test-host", tools, tools == NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(fixture->host);
	fixture->server = ai_mcp_host_create_server(fixture->host);
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
	(void)data;
	g_clear_object(&fixture->server);
	ai_mcp_host_stop(fixture->host);
	g_clear_object(&fixture->host);
	g_clear_object(&fixture->conversation);
}

/* Direct invocation is the transport-independent counterpart of tools/call. */
static McpToolResult *
call(Fixture *fixture, const gchar *name, const gchar *json)
{
	g_autoptr(JsonNode) node = json_from_string(json, NULL);
	g_autoptr(GError) error = NULL;
	McpToolResult *result = mcp_server_invoke_tool(fixture->server, name, json_node_get_object(node), &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	return result;
}

static const gchar *
result_text(McpToolResult *result)
{
	return json_object_get_string_member(json_array_get_object_element(mcp_tool_result_get_content(result), 0), "text");
}

static void
test_shared_todos(Fixture *fixture, gconstpointer data)
{
	g_autoptr(McpToolResult) result = call(fixture, "todo_write",
		"{\"todos\":[{\"content\":\"Add MCP\",\"active_form\":\"Adding MCP\",\"status\":\"in_progress\"}]}");
	g_autofree gchar *transcript = NULL;
	g_autoptr(McpServer) other = ai_mcp_host_create_server(fixture->host);
	g_autoptr(McpToolResult) read = NULL;
	(void)data;
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_assert_cmpuint(ai_tool_executor_get_n_todos(ai_conversation_get_executor(fixture->conversation)), ==, 1);
	transcript = ai_transcript_to_text(ai_conversation_get_transcript(fixture->conversation), 80);
	g_assert_nonnull(strstr(transcript, "Adding MCP"));
	read = mcp_server_invoke_tool(other, "todo_read", NULL, NULL);
	g_assert_nonnull(strstr(result_text(read), "Add MCP"));
	g_assert_nonnull(strstr(result_text(read), "in_progress"));
}

static void
test_invalid_atomic(Fixture *fixture, gconstpointer data)
{
	static const gchar * const invalid[] = {
		"{}", "{\"todos\":null}", "{\"todos\":4}", "{\"todos\":[null]}",
		"{\"todos\":[{}]}", "{\"todos\":[{\"content\":4,\"status\":\"pending\"}]}",
		"{\"todos\":[{\"content\":\"lost\",\"status\":\"typo\"}]}",
		"{\"todos\":[{\"content\":\"lost\",\"status\":\"pending\",\"extra\":true}]}",
		"{\"todos\":[],\"extra\":true}",
		"{\"todos\":[{\"content\":\"valid\",\"status\":\"pending\"},{\"content\":false}]}"
	};
	g_autoptr(McpToolResult) initial = call(fixture, "todo_write", "{\"todos\":[{\"content\":\"Preserve\",\"status\":\"pending\"}]}");
	guint i;
	(void)data;
	g_assert_false(mcp_tool_result_get_is_error(initial));
	for (i = 0; i < G_N_ELEMENTS(invalid); i++)
	{
		g_autoptr(McpToolResult) result = call(fixture, "todo_write", invalid[i]);
		const AiTodo *todo = ai_tool_executor_get_todo(ai_conversation_get_executor(fixture->conversation), 0);
		g_assert_true(mcp_tool_result_get_is_error(result));
		g_assert_cmpstr(todo->content, ==, "Preserve");
	}
}

static void
test_bounds(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GString) large = g_string_new("{\"todos\":[");
	g_autofree gchar *text = g_strnfill(65537, 'a');
	g_autofree gchar *prompt = g_strdup_printf("{\"prompt\":\"%s\"}", text);
	g_autoptr(McpToolResult) result = NULL;
	guint i;
	(void)data;
	for (i = 0; i < 257; i++)
		g_string_append_printf(large, "%s{\"content\":\"x\",\"status\":\"pending\"}", i > 0 ? "," : "");
	g_string_append(large, "]}");
	result = call(fixture, "todo_write", large->str);
	g_assert_true(mcp_tool_result_get_is_error(result));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "conversation_send", prompt);
	g_assert_true(mcp_tool_result_get_is_error(result));
	g_assert_false(ai_conversation_get_busy(fixture->conversation));
}

static void
test_opt_in(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(McpToolResult) result = NULL;
	GList *tools = mcp_server_list_tools(fixture->server);
	(void)data;
	g_assert_cmpuint(g_list_length(tools), ==, 1);
	g_list_free_full(tools, g_object_unref);
	result = mcp_server_invoke_tool(fixture->server, "todo_write", NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(ai_conversation_get_executor(fixture->conversation)), ==, 0);
	/* A filesystem tool must remain undiscoverable and uncallable without
	 * its explicit grant, even if the caller guesses its name. */
	g_clear_error(&error);
	result = mcp_server_invoke_tool(fixture->server, "write", NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
}

static void
test_empty_unknown(void)
{
	g_autoptr(AiOllamaClient) provider = ai_ollama_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(AiMcpHost) host = NULL;
	g_autoptr(McpServer) server = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *bad[] = { "conversation_status,todo_wirte", NULL };
	const gchar *agents[] = { "agent_spawn", NULL };
	host = ai_mcp_host_new(conversation, "empty", NULL, FALSE, &error);
	g_assert_no_error(error);
	server = ai_mcp_host_create_server(host);
	g_assert_null(mcp_server_list_tools(server));
	g_clear_object(&server);
	g_clear_object(&host);
	host = ai_mcp_host_new(conversation, "unknown", bad, FALSE, &error);
	g_assert_null(host);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	host = ai_mcp_host_new(conversation, "agents-disabled", agents, FALSE, &error);
	g_assert_null(host);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
}

static void
test_controls(Fixture *fixture, gconstpointer data)
{
	g_autoptr(McpToolResult) status = call(fixture, "conversation_status", "{}");
	g_autoptr(McpToolResult) clear = call(fixture, "conversation_clear", "{}");
	g_autoptr(McpToolResult) empty = call(fixture, "conversation_send", "{\"prompt\":\"\"}");
	g_autoptr(McpToolResult) wrong = call(fixture, "conversation_send", "{\"prompt\":false}");
	g_autoptr(McpToolResult) page = call(fixture, "conversation_transcript", "{\"limit\":101}");
	g_autoptr(McpToolResult) cancel = call(fixture, "conversation_cancel", "{}");
	(void)data;
	g_assert_false(mcp_tool_result_get_is_error(status));
	g_assert_nonnull(strstr(result_text(status), "\"busy\":false"));
	g_assert_false(mcp_tool_result_get_is_error(clear));
	g_assert_false(mcp_tool_result_get_is_error(cancel));
	g_assert_true(mcp_tool_result_get_is_error(empty));
	g_assert_true(mcp_tool_result_get_is_error(wrong));
	g_assert_true(mcp_tool_result_get_is_error(page));
}

static void
test_socket_paths(Fixture *fixture, gconstpointer data)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-mcp-path-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "existing", NULL);
	g_autofree gchar *contents = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	g_assert_true(g_file_set_contents(path, "preserve", -1, NULL));
	g_assert_false(ai_mcp_host_start(fixture->host, path, FALSE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
	g_clear_error(&error);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_cmpstr(contents, ==, "preserve");
	g_assert_false(ai_mcp_host_start(fixture->host, "/tmp/ai-mcp-public.sock", FALSE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);
	g_assert_true(ai_mcp_host_start(fixture->host, NULL, FALSE, &error));
	g_assert_no_error(error);
	g_clear_pointer(&contents, g_free);
	contents = g_strdup(ai_mcp_host_get_socket_path(fixture->host));
	g_assert_true(g_file_test(contents, G_FILE_TEST_EXISTS));
	ai_mcp_host_stop(fixture->host);
	g_assert_false(g_file_test(contents, G_FILE_TEST_EXISTS));
	g_unlink(path);
	g_rmdir(directory);
}

static void
test_rebind(Fixture *fixture, gconstpointer data)
{
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
	g_autoptr(AiCursorClient) cursor = ai_cursor_client_new();
	g_autoptr(AiOllamaClient) ollama = ai_ollama_client_new();
	g_autoptr(GObject) refusing = g_object_new(rejecting_cli_get_type(), NULL);
	g_autoptr(AiConversation) replacement = ai_conversation_new(G_OBJECT(claude));
	g_autoptr(GError) error = NULL;
	g_autoptr(McpToolResult) result = NULL;
	g_autofree gchar *config = NULL;
	g_autofree gchar *path = NULL;
	const AiAgentEndpoint *endpoint;
	(void)data;
	ai_conversation_set_system_prompt(replacement, "Custom instructions");
	ai_conversation_enable_background_agents(replacement, 4);
	g_assert_true(ai_mcp_host_bind(fixture->host, replacement, "/tmp/bin with spaces/ai-tui", TRUE, &error));
	g_assert_no_error(error);
	endpoint = ai_conversation_get_tool_endpoint(replacement);
	g_assert_cmpstr(endpoint->kind, ==, AI_ENDPOINT_KIND_MCP_CONFIG);
	g_assert_true(g_file_get_contents(endpoint->value, &config, NULL, NULL));
	g_assert_nonnull(strstr(config, "/tmp/bin with spaces/ai-tui"));
	path = g_strdup(endpoint->value);
	g_assert_nonnull(strstr(ai_conversation_get_system_prompt(replacement), "Custom instructions"));
	g_assert_nonnull(strstr(ai_conversation_get_system_prompt(replacement), "CLI harness"));
	/* A server made BEFORE rebinding must now write into the replacement. */
	result = call(fixture, "todo_write", "{\"todos\":[{\"content\":\"New session\",\"status\":\"pending\"}]}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_assert_cmpuint(ai_tool_executor_get_n_todos(ai_conversation_get_executor(replacement)), ==, 1);
	g_assert_cmpuint(ai_tool_executor_get_n_todos(ai_conversation_get_executor(fixture->conversation)), ==, 0);
	g_assert_false(ai_mcp_host_set_provider(fixture->host, G_OBJECT(cursor), "/tmp/ai", TRUE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
	g_clear_error(&error);
	g_assert_true(ai_conversation_get_provider(replacement) == G_OBJECT(claude));
	/* Refusal after writing a different dialect must restore its predecessor. */
	g_assert_false(ai_mcp_host_set_provider(fixture->host, refusing, "/tmp/ai", TRUE, &error));
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
	g_clear_error(&error);
	g_assert_true(ai_conversation_get_provider(replacement) == G_OBJECT(claude));
	g_assert_cmpstr(ai_conversation_get_tool_endpoint(replacement)->kind, ==, AI_ENDPOINT_KIND_MCP_CONFIG);
	{
		g_autofree gchar *restored = NULL;
		g_assert_true(g_file_get_contents(path, &restored, NULL, NULL));
		g_assert_cmpstr(restored, ==, config);
	}
	g_assert_true(ai_mcp_host_set_provider(fixture->host, G_OBJECT(codex), "/tmp/ai", TRUE, &error));
	g_assert_no_error(error);
	g_assert_true(ai_conversation_get_provider(replacement) == G_OBJECT(codex));
	g_assert_cmpstr(ai_conversation_get_tool_endpoint(replacement)->kind, ==, AI_ENDPOINT_KIND_MCP_CONFIG_CODEX);
	/* A model turn must wait for its host tools, including after switching
	 * providers. A missing bridge must fail startup instead of hiding tools. */
	g_clear_pointer(&config, g_free);
	g_assert_true(g_file_get_contents(ai_conversation_get_tool_endpoint(replacement)->value,
		&config, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(config, "mcp_servers.ai_host.required=true\n"));
	g_assert_nonnull(strstr(config, "mcp_servers.ai_host.default_tools_approval_mode=\"approve\"\n"));
	g_assert_true(ai_mcp_host_set_provider(fixture->host, G_OBJECT(ollama), "/tmp/ai", TRUE, &error));
	g_assert_no_error(error);
	g_assert_true(ai_conversation_get_provider(replacement) == G_OBJECT(ollama));
	g_assert_null(ai_conversation_get_tool_endpoint(replacement));
	g_assert_cmpstr(ai_conversation_get_system_prompt(replacement), ==, "Custom instructions");
	g_assert_true(ai_mcp_host_set_provider(fixture->host, G_OBJECT(claude), "/tmp/ai", TRUE, &error));
	g_assert_no_error(error);
	g_assert_nonnull(ai_conversation_get_tool_endpoint(replacement));
	g_assert_true(ai_mcp_host_bind(fixture->host, replacement, "/tmp/ai", FALSE, &error));
	g_assert_no_error(error);
	g_assert_null(ai_conversation_get_tool_endpoint(replacement));
	ai_mcp_host_stop(fixture->host);
	g_assert_null(ai_conversation_get_tool_endpoint(replacement));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
}

/* Explicit caller files remain effective during injection and untouched after it. */
static void
test_config_merge(void)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-mcp-config-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "caller.json", NULL);
	g_autoptr(AiClaudeCodeClient) claude = ai_claude_code_client_new();
	g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(claude));
	g_autoptr(AiMcpHost) host = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *current_path = NULL;
	const gchar *tools[] = { "todo_write", NULL };
	const gchar *original = "{\"mcpServers\":{\"existing\":{\"command\":\"preserved\"}}}";

	g_assert_true(g_file_set_contents(path, original, -1, NULL));
	g_object_set(claude, "mcp-config-path", path, NULL);
	host = ai_mcp_host_new(conversation, "merge", tools, FALSE, &error);
	g_assert_no_error(error);
	g_assert_true(ai_mcp_host_bind(host, conversation, "/tmp/ai", TRUE, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_get_contents(ai_conversation_get_tool_endpoint(conversation)->value, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "preserved"));
	g_assert_nonnull(strstr(contents, "ai_host"));
	/* Repeat binding to cover an existing injected file as the input. */
	g_assert_true(ai_mcp_host_bind(host, conversation, "/tmp/ai", TRUE, &error));
	g_assert_no_error(error);
	g_clear_pointer(&contents, g_free);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_cmpstr(contents, ==, original);
	g_assert_true(ai_mcp_host_bind(host, conversation, "/tmp/ai", FALSE, &error));
	g_object_get(claude, "mcp-config-path", &current_path, NULL);
	g_assert_cmpstr(current_path, ==, path);

	g_assert_true(g_file_set_contents(path, "{\"model\":\"keep-model\",\"mcp\":{\"existing\":{\"type\":\"local\",\"command\":[\"preserved\"]}}}", -1, NULL));
	ai_cli_client_set_env(AI_CLI_CLIENT(opencode), "OPENCODE_CONFIG", path);
	g_assert_true(ai_mcp_host_set_provider(host, G_OBJECT(opencode), "/tmp/ai", TRUE, &error));
	g_assert_no_error(error);
	g_clear_pointer(&contents, g_free);
	g_assert_true(g_file_get_contents(ai_conversation_get_tool_endpoint(conversation)->value, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "keep-model"));
	g_assert_nonnull(strstr(contents, "preserved"));
	g_assert_nonnull(strstr(contents, "ai_host"));
	ai_mcp_host_stop(host);
	g_assert_cmpstr(ai_cli_client_get_env(AI_CLI_CLIENT(opencode), "OPENCODE_CONFIG"), ==, path);
	g_unlink(path);
	g_rmdir(directory);
}

/* Sessions can remain referenced by asynchronous transport callbacks after stop. */
static void
test_server_outlives_host(void)
{
	g_autoptr(AiOllamaClient) provider = ai_ollama_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(AiMcpHost) host = NULL;
	g_autoptr(McpServer) server = NULL;
	g_autoptr(McpToolResult) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *tools[] = { "todo_read", NULL };

	host = ai_mcp_host_new(conversation, "lifetime", tools, FALSE, &error);
	g_assert_no_error(error);
	server = ai_mcp_host_create_server(host);
	g_clear_object(&host);
	result = mcp_server_invoke_tool(server, "todo_read", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_true(mcp_tool_result_get_is_error(result));
}

/* Exercise the actual executor through the MCP grant, including relative
 * paths, optional numeric parameters and atomic rejection of invalid edits. */
static void
test_filesystem(Fixture *fixture, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = g_dir_make_tmp("ai-mcp-files-XXXXXX", &error);
	g_autofree gchar *path = NULL;
	g_autofree gchar *contents = NULL;
	g_autoptr(McpToolResult) result = NULL;
	const gchar *names[] = { "read", "write", "edit", "multi_edit", "glob", "grep", "ls", NULL };
	GList *tools = mcp_server_list_tools(fixture->server);
	guint i;
	(void)data;

	g_assert_no_error(error);
	ai_conversation_set_working_directory(fixture->conversation, directory);
	path = g_build_filename(directory, "sample.txt", NULL);
	for (i = 0; names[i] != NULL; i++)
	{
		GList *iter;
		for (iter = tools; iter != NULL; iter = iter->next)
			if (g_str_equal(mcp_tool_get_name(iter->data), names[i])) break;
		g_assert_nonnull(iter);
	}
	g_list_free_full(tools, g_object_unref);
	result = call(fixture, "write", "{\"path\":\"sample.txt\",\"content\":\"hello world\"}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(contents, ==, "hello world");
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "read", "{\"path\":\"sample.txt\",\"offset\":6,\"limit\":5}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_assert_cmpstr(result_text(result), ==, "world");
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "edit", "{\"path\":\"sample.txt\",\"old_string\":\"world\",\"new_string\":\"MCP\"}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "multi_edit", "{\"path\":\"sample.txt\",\"edits\":[{\"old_string\":\"hello\",\"new_string\":\"goodbye\"},{\"old_string\":\"missing\",\"new_string\":\"oops\"}]}");
	g_assert_true(mcp_tool_result_get_is_error(result));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "read", "{\"path\":\"sample.txt\"}");
	g_assert_cmpstr(result_text(result), ==, "hello MCP");
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "read", "{\"path\":\"sample.txt\",\"offset\":\"invalid\"}");
	g_assert_true(mcp_tool_result_get_is_error(result));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "write", "{\"path\":\"sample.txt\",\"content\":false}");
	g_assert_true(mcp_tool_result_get_is_error(result));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "read", "{\"path\":\"sample.txt\"}");
	g_assert_cmpstr(result_text(result), ==, "hello MCP");
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "multi_edit", "{\"path\":\"sample.txt\",\"edits\":[{\"old_string\":\"hello\",\"new_string\":\"goodbye\"},{\"old_string\":\"MCP\",\"new_string\":\"tools\"}]}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "read", "{\"path\":\"sample.txt\"}");
	g_assert_cmpstr(result_text(result), ==, "goodbye tools");
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "glob", "{\"pattern\":\"*.txt\"}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_assert_nonnull(strstr(result_text(result), "sample.txt"));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "grep", "{\"pattern\":\"goodbye\",\"glob\":\"*.txt\"}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_assert_nonnull(strstr(result_text(result), "goodbye tools"));
	g_clear_pointer(&result, mcp_tool_result_unref);
	result = call(fixture, "ls", "{}");
	g_assert_false(mcp_tool_result_get_is_error(result));
	g_assert_nonnull(strstr(result_text(result), "sample.txt"));
	g_unlink(path);
	g_rmdir(directory);
}

/* Verify every enum provider through the same live host, preserving a server
 * created before switching. Unsupported injection must fail explicitly; its
 * external-only mode must still expose the exact filesystem grant. */
static void
test_filesystem_providers(void)
{
	g_autoptr(AiOllamaClient) initial = ai_ollama_client_new();
	g_autoptr(AiConfig) config = ai_config_new();
	GEnumClass *types = g_type_class_ref(AI_TYPE_PROVIDER_TYPE);
	Fixture fixture = { NULL, NULL, NULL };
	guint i;

	fixture.conversation = ai_conversation_new(G_OBJECT(initial));
	ai_conversation_enable_background_agents(fixture.conversation, 4);
	ai_conversation_set_system_prompt(fixture.conversation, "Preserve caller instructions");
	fixture.host = ai_mcp_host_new(fixture.conversation, "provider-matrix", NULL, TRUE, NULL);
	fixture.server = ai_mcp_host_create_server(fixture.host);
	for (i = 0; i < types->n_values; i++)
	{
		AiProviderType type = (AiProviderType)types->values[i].value;
		g_autoptr(GError) error = NULL;
		g_autoptr(GObject) provider = ai_provider_factory_new(type, config, &error);
		const gchar *kind = NULL;
		gboolean unsupported = type == AI_PROVIDER_CURSOR || type == AI_PROVIDER_ANTIGRAVITY;

		g_test_message("Filesystem provider: %s", ai_provider_type_to_string(type));
		g_assert_no_error(error);
		g_assert_nonnull(provider);
		switch (type)
		{
		case AI_PROVIDER_CLAUDE_CODE:
		case AI_PROVIDER_CLAUDE_TMUX: kind = AI_ENDPOINT_KIND_MCP_CONFIG; break;
		case AI_PROVIDER_CODEX_CLI: kind = AI_ENDPOINT_KIND_MCP_CONFIG_CODEX; break;
		case AI_PROVIDER_GROK_BUILD: kind = AI_ENDPOINT_KIND_MCP_CONFIG_GROK; break;
		case AI_PROVIDER_OPENCODE: kind = AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE; break;
		default: break;
		}
		if (unsupported)
		{
			GObject *previous = ai_conversation_get_provider(fixture.conversation);
			g_assert_false(ai_mcp_host_set_provider(fixture.host, provider, "/tmp/ai-tui", TRUE, &error));
			g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
			g_assert_true(ai_conversation_get_provider(fixture.conversation) == previous);
			g_clear_error(&error);
		}
		g_assert_true(ai_mcp_host_set_provider(fixture.host, provider, "/tmp/ai-tui", !unsupported, &error));
		g_assert_no_error(error);
		if (kind != NULL)
		{
			const AiAgentEndpoint *endpoint = ai_conversation_get_tool_endpoint(fixture.conversation);
			g_assert_nonnull(endpoint);
			g_assert_cmpstr(endpoint->kind, ==, kind);
			g_assert_nonnull(strstr(ai_conversation_get_system_prompt(fixture.conversation), "host read/write/edit/multi_edit"));
		}
		else
		{
			g_assert_null(ai_conversation_get_tool_endpoint(fixture.conversation));
			g_assert_cmpstr(ai_conversation_get_system_prompt(fixture.conversation), ==, "Preserve caller instructions");
			/* HTTP tools are a separate, explicit --local-tools choice. */
			if (AI_IS_CLIENT(provider))
			{
				ai_conversation_set_local_tools(fixture.conversation, TRUE);
				g_assert_true(ai_conversation_get_local_tools(fixture.conversation));
			}
		}
		test_filesystem(&fixture, NULL);
	}
	teardown(&fixture, NULL);
	g_type_class_unref(types);
}

int
main(int argc, char **argv)
{
	static const gchar * const status_only[] = { "conversation_status", NULL };
	g_test_init(&argc, &argv, NULL);
	g_test_add("/mcp/host/filesystem", Fixture, NULL, setup, test_filesystem, teardown);
	g_test_add_func("/mcp/host/filesystem-providers", test_filesystem_providers);
	g_test_add_func("/mcp/host/server-outlives-host", test_server_outlives_host);
	g_test_add("/mcp/host/shared-todos", Fixture, NULL, setup, test_shared_todos, teardown);
	g_test_add("/mcp/host/invalid-atomic", Fixture, NULL, setup, test_invalid_atomic, teardown);
	g_test_add("/mcp/host/bounds", Fixture, NULL, setup, test_bounds, teardown);
	g_test_add("/mcp/host/opt-in", Fixture, status_only, setup, test_opt_in, teardown);
	g_test_add_func("/mcp/host/empty-unknown", test_empty_unknown);
	g_test_add("/mcp/host/controls", Fixture, NULL, setup, test_controls, teardown);
	g_test_add("/mcp/host/socket-paths", Fixture, NULL, setup, test_socket_paths, teardown);
	g_test_add("/mcp/host/rebind", Fixture, NULL, setup, test_rebind, teardown);
	g_test_add_func("/mcp/host/config-merge", test_config_merge);
	return g_test_run();
}
