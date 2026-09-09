/*
 * ai-mcp-host.c - Opt-in MCP access to a live conversation.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "ai-mcp-host.h"
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#define MCP_HOST_MAX_STRING (65536)
#define MCP_HOST_MAX_TODOS (256)
#define MCP_HOST_MAX_AGENTS (128)
#define MCP_HOST_MAX_RESULT (1024 * 1024)

struct _AiMcpHost
{
	GObject parent_instance;
	AiConversation *conversation;
	gchar *name;
	GHashTable *allowed;
	McpUnixSocketServer *socket_server;
	McpServer *stdio_server;
	GMainLoop *loop;
	gchar *directory;
	gchar *socket_path;
	gchar *config_path;
	gchar *base_prompt;
	gchar *last_error;
	guint64 operation;
	gboolean operation_pending;
	gboolean stopping;
	gboolean server_failed;
};

G_DEFINE_TYPE(AiMcpHost, ai_mcp_host, G_TYPE_OBJECT)

/* Deliberately finite: --mcp-all-tools grants this host surface, not a shell. */
static const gchar * const catalog[] = {
	"conversation_status", "conversation_send", "conversation_cancel",
	"conversation_clear", "conversation_transcript", "todo_read", "todo_write",
	"agent_spawn", "agent_status", "agent_result", "agent_cancel", NULL
};

const gchar * const *
ai_mcp_host_catalog(void)
{
	return catalog;
}

/* Return protocol errors as tool results, keeping the session usable. */
static McpToolResult *
text_result(const gchar *text, gboolean failed)
{
	McpToolResult *result = mcp_tool_result_new(failed);
	if (text != NULL && strlen(text) > MCP_HOST_MAX_RESULT)
	{
		const gchar *end = text + MCP_HOST_MAX_RESULT;
		g_autofree gchar *bounded = NULL;
		g_autofree gchar *marked = NULL;
		while (end > text && ((*end & 0xc0) == 0x80)) end--;
		bounded = g_strndup(text, end - text);
		marked = g_strconcat(bounded, "\n[Host MCP result truncated at 1 MiB]", NULL);
		mcp_tool_result_add_text(result, marked);
	}
	else
		mcp_tool_result_add_text(result, text != NULL ? text : "");
	return result;
}

static McpToolResult *
object_result(JsonObject *object)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	g_autofree gchar *text = NULL;
	json_node_set_object(node, object);
	text = json_to_string(node, FALSE);
	if (strlen(text) > MCP_HOST_MAX_RESULT)
		return text_result("State exceeds the 1 MiB MCP response limit; request a smaller transcript page or reduce the TODO list", TRUE);
	return text_result(text, FALSE);
}

/* Tool schemas are also checked here: mcp-glib does not enforce them. */
static gboolean
validate_node(JsonNode *node, JsonObject *schema, guint depth, GError **error)
{
	const gchar *type;
	JsonNode *enum_node;
	gboolean valid = FALSE;

	if (depth > 8 || node == NULL || schema == NULL)
		goto invalid;
	type = json_object_get_string_member(schema, "type");
	if (g_str_equal(type, "string"))
		valid = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING &&
		        strlen(json_node_get_string(node)) <= MCP_HOST_MAX_STRING;
	else if (g_str_equal(type, "integer"))
		valid = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64;
	else if (g_str_equal(type, "boolean"))
		valid = JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN;
	else if (g_str_equal(type, "object") && JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObject *object = json_node_get_object(node);
		JsonObject *properties = json_object_get_object_member(schema, "properties");
		g_autoptr(GList) members = json_object_get_members(object);
		GList *iter;
		JsonArray *required = json_object_has_member(schema, "required") ?
			json_object_get_array_member(schema, "required") : NULL;
		guint i;

		for (iter = members; iter != NULL; iter = iter->next)
		{
			const gchar *key = (const gchar *)iter->data;
			if (!json_object_has_member(properties, key) ||
			    !validate_node(json_object_get_member(object, key),
			                   json_object_get_object_member(properties, key), depth + 1, error))
				goto invalid;
		}
		for (i = 0; required != NULL && i < json_array_get_length(required); i++)
			if (!json_object_has_member(object, json_array_get_string_element(required, i)))
				goto invalid;
		valid = TRUE;
	}
	else if (g_str_equal(type, "array") && JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		guint i;
		if (json_array_get_length(array) > MCP_HOST_MAX_TODOS)
			goto invalid;
		for (i = 0; i < json_array_get_length(array); i++)
			if (!validate_node(json_array_get_element(array, i),
			                   json_object_get_object_member(schema, "items"), depth + 1, error))
				goto invalid;
		valid = TRUE;
	}
	if (!valid)
		goto invalid;
	enum_node = json_object_get_member(schema, "enum");
	if (enum_node != NULL)
	{
		JsonArray *values = json_node_get_array(enum_node);
		guint i;
		for (i = 0; i < json_array_get_length(values); i++)
			if (json_node_equal(node, json_array_get_element(values, i)))
				return TRUE;
		goto invalid;
	}
	return TRUE;
invalid:
	if (error != NULL && *error == NULL)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Arguments do not match the tool schema (unknown field, missing field, invalid type/value, or size limit)");
	return FALSE;
}

/* This callback holds the host until the provider has acknowledged cancellation. */
static void
send_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
	AiMcpHost *self = AI_MCP_HOST(user_data);
	g_autoptr(GError) error = NULL;
	ai_conversation_send_finish(AI_CONVERSATION(source), result, &error);
	self->operation_pending = FALSE;
	g_free(self->last_error);
	self->last_error = error != NULL ? g_strdup(error->message) : NULL;
	g_object_unref(self);
}

/* Build the canonical schema once per registration/call, without model data. */
static AiTool *
create_tool(AiMcpHost *self, const gchar *name)
{
	AiTool *tool;
	GList *iter;
	if (g_str_equal(name, "todo_write"))
	{
		tool = ai_tool_new(name, "Replace the host TODOS panel atomically. Use this instead of the CLI harness todo/plan tool.");
		ai_tool_add_array_parameter(tool, "todos", "Complete task list; at most 256 items",
			"{\"type\":\"object\",\"properties\":{\"content\":{\"type\":\"string\"},"
			"\"active_form\":{\"type\":\"string\"},\"status\":{\"type\":\"string\","
			"\"enum\":[\"pending\",\"in_progress\",\"completed\"]}},"
			"\"required\":[\"content\",\"status\"],\"additionalProperties\":false}", TRUE);
		return tool;
	}
	if (g_str_has_prefix(name, "agent_"))
	{
		for (iter = ai_tool_executor_get_tools(ai_conversation_get_executor(self->conversation));
		     iter != NULL; iter = iter->next)
			if (g_str_equal(ai_tool_get_name(AI_TOOL(iter->data)), name))
				return g_object_ref(iter->data);
		return NULL;
	}
	tool = ai_tool_new(name,
		g_str_equal(name, "conversation_status") ? "Read provider, activity, turn status, operation ID and last error." :
		g_str_equal(name, "conversation_send") ? "Start a user turn in the host. Returns immediately; poll conversation_status. External controllers only; do not send to yourself." :
		g_str_equal(name, "conversation_cancel") ? "Cancel the active host turn; returns immediately." :
		g_str_equal(name, "conversation_clear") ? "Clear host conversation and TODOS when idle; does not discard live agents." :
		g_str_equal(name, "conversation_transcript") ? "Read a bounded page of conversation messages." :
		"Read the host TODOS panel.");
	if (g_str_equal(name, "conversation_send"))
		ai_tool_add_parameter(tool, "prompt", "string", "User prompt, 1..65536 bytes, sent verbatim", TRUE);
	if (g_str_equal(name, "conversation_transcript"))
	{
		ai_tool_add_parameter(tool, "offset", "integer", "First message index (default 0)", FALSE);
		ai_tool_add_parameter(tool, "limit", "integer", "Maximum messages, 1..100 (default 20)", FALSE);
	}
	return tool;
}

/* Validate the whole input before touching shared state; failed writes are atomic. */
static McpToolResult *
invoke_tool(McpServer *server, const gchar *name, JsonObject *arguments, gpointer user_data)
{
	g_autoptr(AiMcpHost) self = g_weak_ref_get((GWeakRef *)user_data);
	AiConversation *conversation;
	AiToolExecutor *executor;
	g_autoptr(AiTool) tool = NULL;
	g_autoptr(JsonNode) input = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(JsonObject) empty = json_object_new();
	g_autoptr(JsonObject) output = json_object_new();
	g_autoptr(GError) error = NULL;
	(void)server;

	if (self == NULL) return text_result("Host session has closed", TRUE);
	conversation = self->conversation;
	executor = ai_conversation_get_executor(conversation);
	if (self->stopping || !g_hash_table_contains(self->allowed, name))
		return text_result("Tool is not enabled", TRUE);
	tool = create_tool(self, name);
	if (tool == NULL)
		return text_result("Tool is unavailable in this conversation", TRUE);
	json_node_set_object(input, arguments != NULL ? arguments : empty);
	schema = ai_tool_get_parameters_json(tool);
	if (!validate_node(input, json_node_get_object(schema), 0, &error))
		return text_result(error->message, TRUE);
	arguments = json_node_get_object(input);

	if (g_str_equal(name, "conversation_status"))
	{
		GObject *provider = ai_conversation_get_provider(conversation);
		const gchar *activity = ai_conversation_get_activity(conversation);
		json_object_set_string_member(output, "provider", ai_provider_get_name(AI_PROVIDER(provider)));
		json_object_set_boolean_member(output, "busy", ai_conversation_get_busy(conversation) || self->operation_pending);
		json_object_set_string_member(output, "activity", activity != NULL ? activity : "");
		json_object_set_int_member(output, "operation_id", (gint64)self->operation);
		json_object_set_string_member(output, "operation_state", self->operation_pending ? "running" : self->last_error != NULL ? "failed" : self->operation > 0 ? "completed" : "idle");
		if (self->last_error != NULL) json_object_set_string_member(output, "error", self->last_error);
		json_object_set_int_member(output, "todos", ai_tool_executor_get_n_todos(executor));
		json_object_set_int_member(output, "agents_live", ai_conversation_get_brigade(conversation) != NULL ?
			ai_brigade_count_live(ai_conversation_get_brigade(conversation)) : 0);
	}
	else if (g_str_equal(name, "conversation_send"))
	{
		const gchar *prompt = json_object_get_string_member(arguments, "prompt");
		if (*prompt == '\0') return text_result("prompt must not be empty", TRUE);
		if (self->operation_pending || ai_conversation_get_busy(conversation))
			return text_result("Conversation is busy; wait or cancel before sending another turn", TRUE);
		self->operation_pending = TRUE;
		self->operation++;
		g_clear_pointer(&self->last_error, g_free);
		ai_conversation_send_async(conversation, prompt, NULL, send_finished, g_object_ref(self));
		json_object_set_int_member(output, "operation_id", (gint64)self->operation);
		json_object_set_string_member(output, "state", "accepted");
	}
	else if (g_str_equal(name, "conversation_cancel"))
	{
		ai_conversation_cancel(conversation);
		json_object_set_boolean_member(output, "cancel_requested", TRUE);
	}
	else if (g_str_equal(name, "conversation_clear"))
	{
		if (self->operation_pending || ai_conversation_get_busy(conversation))
			return text_result("Conversation is busy; cancel and wait before clearing", TRUE);
		ai_conversation_clear(conversation);
		json_object_set_boolean_member(output, "cleared", TRUE);
	}
	else if (g_str_equal(name, "conversation_transcript"))
	{
		gint64 offset = json_object_has_member(arguments, "offset") ? json_object_get_int_member(arguments, "offset") : 0;
		gint64 limit = json_object_has_member(arguments, "limit") ? json_object_get_int_member(arguments, "limit") : 20;
		GList *iter;
		gint64 index = 0;
		gsize bytes = 0;
		JsonArray *messages;
		if (offset < 0 || limit < 1 || limit > 100) return text_result("offset must be nonnegative; limit must be 1..100", TRUE);
		messages = json_array_new();
		json_object_set_array_member(output, "messages", messages);
		for (iter = ai_conversation_get_messages(conversation); iter != NULL; iter = iter->next, index++)
		{
			g_autofree gchar *text = NULL;
			JsonObject *message;
			if (index < offset) continue;
			if (index - offset >= limit || bytes >= MCP_HOST_MAX_STRING) break;
			text = ai_message_get_text(AI_MESSAGE(iter->data));
			message = json_object_new();
			json_object_set_int_member(message, "index", index);
			json_object_set_string_member(message, "role", ai_role_to_string(ai_message_get_role(AI_MESSAGE(iter->data))));
			if (text != NULL && strlen(text) > MCP_HOST_MAX_STRING)
			{
				gchar *end = text + MCP_HOST_MAX_STRING;
				while (end > text && ((*end & 0xc0) == 0x80)) end--;
				*end = '\0';
				json_object_set_boolean_member(message, "truncated", TRUE);
			}
			json_object_set_string_member(message, "text", text != NULL ? text : "");
			bytes += text != NULL ? strlen(text) : 0;
			json_array_add_object_element(messages, message);
		}
		json_object_set_int_member(output, "next_offset", index);
		json_object_set_boolean_member(output, "has_more", iter != NULL);
	}
	else if (g_str_equal(name, "todo_read"))
	{
		JsonArray *todos = json_array_new();
		guint i;
		json_object_set_array_member(output, "todos", todos);
		for (i = 0; i < ai_tool_executor_get_n_todos(executor); i++)
		{
			const AiTodo *todo = ai_tool_executor_get_todo(executor, i);
			JsonObject *item = json_object_new();
			json_object_set_string_member(item, "content", todo->content);
			json_object_set_string_member(item, "status", ai_todo_state_to_string(todo->state));
			if (todo->active_form != NULL) json_object_set_string_member(item, "active_form", todo->active_form);
			json_array_add_object_element(todos, item);
		}
	}
	else
	{
		g_autoptr(AiToolUse) use = NULL;
		g_autofree gchar *text = NULL;
		if (g_str_equal(name, "agent_spawn"))
		{
			g_autoptr(GList) agents = ai_brigade_list(ai_conversation_get_brigade(conversation));
			if (g_list_length(agents) >= MCP_HOST_MAX_AGENTS)
				return text_result("Agent limit reached (128 tracked); collect completed agents with agent_result before spawning more", TRUE);
		}
		if (g_str_equal(name, "agent_spawn") && !json_object_has_member(arguments, "provider"))
		{
			GObject *provider = ai_conversation_get_provider(conversation);
			const gchar *model = AI_IS_CLI_CLIENT(provider) ? ai_cli_client_get_model(AI_CLI_CLIENT(provider)) :
				AI_IS_CLIENT(provider) ? ai_client_get_model(AI_CLIENT(provider)) : NULL;
			json_object_set_string_member(arguments, "provider", ai_provider_type_to_string(ai_provider_get_provider_type(AI_PROVIDER(provider))));
			if (model != NULL && !json_object_has_member(arguments, "model")) json_object_set_string_member(arguments, "model", model);
		}
		use = ai_tool_use_new("mcp-host", name, input);
		text = ai_tool_executor_execute(executor, use, NULL, &error);
		return text_result(error != NULL ? error->message : text, error != NULL);
	}
	return object_result(output);
}

/* Weak bindings permit independently retained sessions without a host/server
 * reference cycle, and keep an invocation alive only for its actual duration. */
static void
host_binding_free(gpointer data)
{
	GWeakRef *binding = data;
	g_weak_ref_clear(binding);
	g_free(binding);
}

/* Publish the exact grant for every connection, so reconnects cannot widen it. */
static void
register_tools(AiMcpHost *self, McpServer *server)
{
	guint i;
	g_autoptr(GString) instructions = g_string_new("These tools control the ai host session. Enabled tools: ");
	for (i = 0; catalog[i] != NULL; i++)
	{
		g_autoptr(AiTool) definition = NULL;
		g_autoptr(McpTool) tool = NULL;
		g_autoptr(JsonNode) schema = NULL;
		GWeakRef *binding;
		if (!g_hash_table_contains(self->allowed, catalog[i])) continue;
		definition = create_tool(self, catalog[i]);
		if (definition == NULL) continue;
		tool = mcp_tool_new(catalog[i], ai_tool_get_description(definition));
		schema = ai_tool_get_parameters_json(definition);
		json_object_set_boolean_member(json_node_get_object(schema), "additionalProperties", FALSE);
		mcp_tool_set_input_schema(tool, schema);
		binding = g_new0(GWeakRef, 1);
		g_weak_ref_init(binding, self);
		mcp_server_add_tool(server, tool, invoke_tool, binding, host_binding_free);
		g_string_append_printf(instructions, "%s ", catalog[i]);
	}
	g_string_append(instructions, ". Use host todo_write for TODOS and host agent_* for AGENTS when enabled; these update the visible application. Do not substitute the CLI harness plan/TODO or subagent tools. Do not call conversation_send to send yourself a message; it is for external controllers.");
	mcp_server_set_instructions(server, instructions->str);
}

McpServer *
ai_mcp_host_create_server(AiMcpHost *self)
{
	McpServer *server = mcp_server_new(self->name, "1.0.0");
	register_tools(self, server);
	return server;
}

static void
session_created(McpUnixSocketServer *listener, McpServer *server, gpointer user_data)
{
	McpTransport *transport = mcp_server_get_transport(server);
	(void)listener;
	if (MCP_IS_STDIO_TRANSPORT(transport))
		mcp_stdio_transport_set_max_line_bytes(MCP_STDIO_TRANSPORT(transport), 1024 * 1024);
	register_tools(AI_MCP_HOST(user_data), server);
}

static void
stdio_disconnected(McpServer *server, gpointer user_data)
{
	AiMcpHost *self = AI_MCP_HOST(user_data);
	(void)server;
	if (self->loop != NULL) g_main_loop_quit(self->loop);
	g_signal_emit_by_name(self, "closed");
}

/* An initialized transport can fail without a server disconnect signal. */
static void
stdio_state_changed(McpTransport *transport, gint old_state, gint new_state, gpointer user_data)
{
	AiMcpHost *self = AI_MCP_HOST(user_data);
	(void)transport;
	(void)old_state;
	if (new_state == MCP_TRANSPORT_STATE_ERROR && !self->stopping)
	{
		self->server_failed = TRUE;
		stdio_disconnected(self->stdio_server, self);
	}
}

static void
server_started(GObject *source, GAsyncResult *result, gpointer user_data)
{
	AiMcpHost *self = AI_MCP_HOST(user_data);
	g_autoptr(GError) error = NULL;
	if (!mcp_server_start_finish(MCP_SERVER(source), result, &error))
	{
		g_printerr("MCP server: %s\n", error->message);
		self->server_failed = !self->stopping;
		g_free(self->last_error);
		self->last_error = g_strdup(error->message);
		stdio_disconnected(MCP_SERVER(source), self);
	}
	g_object_unref(self);
}

/* Existing socket paths are never replaced. Require a private, owned directory. */
gboolean
ai_mcp_host_start(AiMcpHost *self, const gchar *socket_path, gboolean stdio, GError **error)
{
	struct stat info;
	g_autofree gchar *parent = NULL;
	if ((stdio && self->stdio_server != NULL) || (!stdio && self->socket_server != NULL))
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "MCP server is already started");
		return FALSE;
	}
	self->stopping = FALSE;
	if (stdio)
	{
		g_autoptr(McpStdioTransport) transport = mcp_stdio_transport_new();
		mcp_stdio_transport_set_max_line_bytes(transport, 1024 * 1024);
		self->stdio_server = ai_mcp_host_create_server(self);
		mcp_server_set_transport(self->stdio_server, MCP_TRANSPORT(transport));
		g_signal_connect_object(transport, "state-changed", G_CALLBACK(stdio_state_changed), self, 0);
		g_signal_connect_object(self->stdio_server, "client-disconnected", G_CALLBACK(stdio_disconnected), self, 0);
		mcp_server_start_async(self->stdio_server, NULL, server_started, g_object_ref(self));
		return TRUE;
	}
	if (self->directory == NULL)
	{
		self->directory = g_dir_make_tmp("ai-mcp-XXXXXX", error);
		if (self->directory == NULL) return FALSE;
	}
	self->socket_path = socket_path != NULL ? g_strdup(socket_path) : g_build_filename(self->directory, "host.sock", NULL);
	parent = g_path_get_dirname(self->socket_path);
	if (!g_path_is_absolute(self->socket_path) || strlen(self->socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path) ||
	    g_lstat(parent, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 0077) != 0)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"MCP socket requires a short absolute path inside an owned mode-0700 directory");
		g_clear_pointer(&self->socket_path, g_free);
		return FALSE;
	}
	if (g_lstat(self->socket_path, &info) == 0 || errno != ENOENT)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "MCP socket path already exists or cannot be inspected; it will not be replaced");
		g_clear_pointer(&self->socket_path, g_free);
		return FALSE;
	}
	self->socket_server = mcp_unix_socket_server_new(self->name, "1.0.0", self->socket_path);
	g_signal_connect_object(self->socket_server, "session-created", G_CALLBACK(session_created), self, 0);
	if (!mcp_unix_socket_server_start(self->socket_server, error))
	{
		g_clear_object(&self->socket_server);
		g_clear_pointer(&self->socket_path, g_free);
		return FALSE;
	}
	return TRUE;
}

const gchar *
ai_mcp_host_get_socket_path(AiMcpHost *self)
{
	return self->socket_path;
}

/* Revocation precedes deleting generated files, so no new child sees stale paths. */
void
ai_mcp_host_stop(AiMcpHost *self)
{
	gboolean was_started;
	AiBrigade *brigade;
	AiAgentWorker *worker;
	gint64 deadline;

	if (self->stopping) return;
	self->stopping = TRUE;
	was_started = self->socket_server != NULL || self->stdio_server != NULL || self->operation_pending;
	/* Stop accepting requests before dispatching any cancellation callbacks. */
	if (self->socket_server != NULL) mcp_unix_socket_server_stop(self->socket_server);
	if (self->stdio_server != NULL) mcp_server_stop(self->stdio_server);
	if (self->conversation != NULL)
	{
		if (was_started)
		{
			brigade = ai_conversation_get_brigade(self->conversation);
			worker = brigade != NULL ? ai_brigade_get_worker(brigade) : NULL;
			ai_conversation_cancel(self->conversation);
			if (brigade != NULL) ai_brigade_cancel_all(brigade);
			/* CANCELLED is a logical state, not proof that child processes have
			 * exited. Drain actual run completions, allowing the subprocess
			 * layer's two-second pipe cleanup grace while bounding shutdown. */
			deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
			while (self->operation_pending || ai_conversation_get_busy(self->conversation) ||
			       (AI_IS_LOCAL_WORKER(worker) && ai_local_worker_get_pending_count(AI_LOCAL_WORKER(worker)) > 0))
			{
				if (g_get_monotonic_time() >= deadline)
				{
					g_printerr("MCP shutdown: provider cancellation did not finish within three seconds\n");
					break;
				}
				g_main_context_iteration(NULL, FALSE);
				g_usleep(1000);
			}
		}
		if (ai_conversation_get_tool_endpoint(self->conversation) != NULL && self->config_path != NULL)
			ai_conversation_set_tool_endpoint(self->conversation, NULL, NULL);
	}
	g_clear_object(&self->socket_server);
	g_clear_object(&self->stdio_server);
	if (self->config_path != NULL) g_unlink(self->config_path);
	g_clear_pointer(&self->config_path, g_free);
	if (self->directory != NULL) g_rmdir(self->directory);
	g_clear_pointer(&self->directory, g_free);
	g_clear_pointer(&self->socket_path, g_free);
}

static void
ai_mcp_host_dispose(GObject *object)
{
	AiMcpHost *self = AI_MCP_HOST(object);
	ai_mcp_host_stop(self);
	g_clear_object(&self->conversation);
	G_OBJECT_CLASS(ai_mcp_host_parent_class)->dispose(object);
}

static void
ai_mcp_host_finalize(GObject *object)
{
	AiMcpHost *self = AI_MCP_HOST(object);
	g_free(self->name);
	g_free(self->base_prompt);
	g_free(self->last_error);
	g_hash_table_unref(self->allowed);
	G_OBJECT_CLASS(ai_mcp_host_parent_class)->finalize(object);
}

static void
ai_mcp_host_class_init(AiMcpHostClass *klass)
{
	G_OBJECT_CLASS(klass)->dispose = ai_mcp_host_dispose;
	G_OBJECT_CLASS(klass)->finalize = ai_mcp_host_finalize;
	g_signal_new("closed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ai_mcp_host_init(AiMcpHost *self)
{
	self->allowed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

AiMcpHost *
ai_mcp_host_new(AiConversation *conversation, const gchar *name,
                const gchar * const *tools, gboolean all_tools, GError **error)
{
	g_autoptr(AiMcpHost) self = g_object_new(AI_TYPE_MCP_HOST, NULL);
	guint i;
	self->conversation = g_object_ref(conversation);
	self->name = g_strdup(name);
	for (i = 0; tools != NULL && tools[i] != NULL; i++)
	{
		g_auto(GStrv) names = g_strsplit(tools[i], ",", -1);
		guint j;
		for (j = 0; names[j] != NULL; j++)
		{
			guint k;
			for (k = 0; catalog[k] != NULL; k++)
				if (g_str_equal(names[j], catalog[k])) break;
			if (catalog[k] == NULL)
			{
				g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Unknown MCP tool '%s'; use --mcp-list-tools", names[j]);
				return NULL;
			}
			g_hash_table_add(self->allowed, g_strdup(names[j]));
		}
	}
	if (all_tools)
		for (i = 0; catalog[i] != NULL; i++) g_hash_table_add(self->allowed, g_strdup(catalog[i]));
	for (i = 0; catalog[i] != NULL; i++)
	{
		g_autoptr(AiTool) tool = NULL;
		if (!g_hash_table_contains(self->allowed, catalog[i])) continue;
		tool = create_tool(self, catalog[i]);
		if (tool == NULL)
		{
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "MCP tool '%s' requires enabled background agents", catalog[i]);
			return NULL;
		}
	}
	return g_steal_pointer(&self);
}

static gboolean
quit_loop(gpointer user_data)
{
	g_main_loop_quit((GMainLoop *)user_data);
	return G_SOURCE_CONTINUE;
}

gint
ai_mcp_host_run(AiMcpHost *self)
{
	guint sigint_id;
	guint sigterm_id;
	self->loop = g_main_loop_new(NULL, FALSE);
	sigint_id = g_unix_signal_add(SIGINT, quit_loop, self->loop);
	sigterm_id = g_unix_signal_add(SIGTERM, quit_loop, self->loop);
	g_main_loop_run(self->loop);
	g_source_remove(sigint_id);
	g_source_remove(sigterm_id);
	g_clear_pointer(&self->loop, g_main_loop_unref);
	ai_mcp_host_stop(self);
	return self->server_failed ? 1 : 0;
}

/* JSON string escaping is also valid TOML basic-string escaping for these
 * filesystem paths; json-glib escapes control characters and backslashes. */
static gchar *
quote_string(const gchar *value)
{
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_VALUE);
	json_node_set_string(node, value);
	return json_to_string(node, FALSE);
}

/* Mint the dialect negotiated by the provider. Never edit workspace/global files. */
static gchar *
endpoint_config(const gchar *kind, const gchar *executable, const gchar *path)
{
	g_autofree gchar *command = quote_string(executable);
	g_autofree gchar *socket = quote_string(path);
	if (g_str_equal(kind, AI_ENDPOINT_KIND_MCP_CONFIG_GROK))
		return g_strdup_printf("[mcp_servers.ai_host]\ncommand = %s\nargs = [\"--mcp-connect\", %s]\n", command, socket);
	if (g_str_equal(kind, "mcp-config-codex"))
		/* The foreground turn depends on these tools. Wait for initialization
		 * instead of allowing Codex's optional-server startup grace to omit them.
		 * The user explicitly granted this host's allowlist. Codex exec runs
		 * without approval prompts, so approve only this server's tools. */
		return g_strdup_printf("mcp_servers.ai_host.command=%s\nmcp_servers.ai_host.args=[\"--mcp-connect\",%s]\nmcp_servers.ai_host.required=true\nmcp_servers.ai_host.default_tools_approval_mode=\"approve\"\n", command, socket);
	if (g_str_equal(kind, AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE))
		return g_strdup_printf("{\"mcp\":{\"ai_host\":{\"type\":\"local\",\"command\":[%s,\"--mcp-connect\",%s],\"enabled\":true}}}", command, socket);
	return g_strdup_printf("{\"mcpServers\":{\"ai_host\":{\"command\":%s,\"args\":[\"--mcp-connect\",%s]}}}", command, socket);
}

/* Retain explicitly configured MCP servers and OpenCode settings during the
 * grant, not just after revocation. Other provider dialects merge natively. */
static gchar *
merge_caller_config(GObject *provider, const gchar *kind, const gchar *generated, GError **error)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *buffer = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileInputStream) input = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) extra = NULL;
	JsonObject *root;
	JsonObject *servers;
	JsonNode *node;
	const gchar *section;
	gsize size = 0;
	struct stat info;

	if (g_str_equal(kind, AI_ENDPOINT_KIND_MCP_CONFIG))
	{
		g_object_get(provider, "mcp-config-path", &path, NULL);
		section = "mcpServers";
	}
	else if (g_str_equal(kind, AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE))
	{
		const gchar *configured = ai_cli_client_get_env(AI_CLI_CLIENT(provider), "OPENCODE_CONFIG");
		path = g_strdup(configured != NULL ? configured : g_getenv("OPENCODE_CONFIG"));
		section = "mcp";
	}
	else
		return g_strdup(generated);
	if (path == NULL || *path == '\0') return g_strdup(generated);
	if (g_stat(path, &info) != 0 || !S_ISREG(info.st_mode))
	{
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Explicit MCP configuration must be a readable regular JSON file: %s", path);
		return NULL;
	}
	file = g_file_new_for_path(path);
	input = g_file_read(file, NULL, error);
	if (input == NULL) return NULL;
	buffer = g_malloc(MCP_HOST_MAX_RESULT + 1);
	if (!g_input_stream_read_all(G_INPUT_STREAM(input), buffer, MCP_HOST_MAX_RESULT + 1, &size, NULL, error)) return NULL;
	if (size > MCP_HOST_MAX_RESULT)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Explicit MCP configuration exceeds 1 MiB");
		return NULL;
	}
	if (!json_parser_load_from_data(parser, buffer, (gssize)size, error)) return NULL;
	node = json_parser_get_root(parser);
	if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
	root = json_node_get_object(node);
	node = json_object_get_member(root, section);
	if (node != NULL && !JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
	if (node == NULL)
	{
		servers = json_object_new();
		json_object_set_object_member(root, section, servers);
	}
	else servers = json_node_get_object(node);
	extra = json_from_string(generated, NULL);
	json_object_set_member(servers, "ai_host", json_node_copy(json_object_get_member(
		json_object_get_object_member(json_node_get_object(extra), section), "ai_host")));
	return json_to_string(json_parser_get_root(parser), FALSE);
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Explicit MCP configuration and its server map must be JSON objects");
	return NULL;
}

/* Rebinding keeps socket sessions pointed at the current model, including /new.
 * The endpoint consumer handles delivery and restoration of caller settings. */
gboolean
ai_mcp_host_bind(AiMcpHost *self, AiConversation *conversation,
                 const gchar *executable, gboolean inject, GError **error)
{
	GObject *provider = ai_conversation_get_provider(conversation);
	g_autofree gchar *config = NULL;
	g_autofree gchar *generated = NULL;
	g_autofree gchar *previous_config = NULL;
	g_autofree gchar *instructions = NULL;
	g_autoptr(AiAgentEndpoint) endpoint = NULL;
	g_autoptr(AiAgentEndpoint) previous_endpoint = NULL;
	g_autoptr(McpServer) description = NULL;
	const gchar *kind = NULL;
	guint i;
	static const gchar * const kinds[] = { AI_ENDPOINT_KIND_MCP_CONFIG,
		AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE, AI_ENDPOINT_KIND_MCP_CONFIG_GROK,
		"mcp-config-codex", NULL };

	if (self->operation_pending || ai_conversation_get_busy(conversation))
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Cannot change MCP binding during a turn");
		return FALSE;
	}
	if (ai_conversation_get_brigade(conversation) == NULL)
	{
		for (i = 0; catalog[i] != NULL; i++)
			if (g_str_has_prefix(catalog[i], "agent_") && g_hash_table_contains(self->allowed, catalog[i]))
			{
				g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Cannot rebind enabled MCP agent tools without a brigade");
				return FALSE;
			}
	}
	if (inject && AI_IS_CLI_CLIENT(provider) && g_hash_table_size(self->allowed) != 0)
	{
		for (i = 0; kinds[i] != NULL; i++)
			if (ai_tool_endpoint_consumer_supports_kind(AI_TOOL_ENDPOINT_CONSUMER(provider), kinds[i]))
			{
				kind = kinds[i];
				break;
			}
		if (kind == NULL)
		{
			g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				"%s has no reliable session-scoped MCP injection; use --mcp-no-inject for external control only",
				ai_provider_get_name(AI_PROVIDER(provider)));
			return FALSE;
		}
		if (self->socket_server == NULL && !ai_mcp_host_start(self, NULL, FALSE, error))
			return FALSE;
		if (self->config_path == NULL)
			self->config_path = g_build_filename(self->directory, "provider-config", NULL);
		g_file_get_contents(self->config_path, &previous_config, NULL, NULL);
		generated = endpoint_config(kind, executable, self->socket_path);
		config = merge_caller_config(provider, kind, generated, error);
		if (config == NULL) return FALSE;
		if (!g_file_set_contents_full(self->config_path, config, -1,
		                             G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
			return FALSE;
		endpoint = ai_agent_endpoint_new(kind, self->config_path);
	}
	if (self->conversation != conversation)
	{
		if (ai_conversation_get_tool_endpoint(self->conversation) != NULL)
		{
			previous_endpoint = ai_agent_endpoint_copy(ai_conversation_get_tool_endpoint(self->conversation));
			ai_conversation_set_tool_endpoint(self->conversation, NULL, NULL);
		}
	}
	if (endpoint != NULL && !ai_conversation_set_tool_endpoint(conversation, endpoint, error))
	{
		/* A refused new dialect must leave the previous session usable. */
		if (previous_config != NULL) g_file_set_contents(self->config_path, previous_config, -1, NULL);
		if (previous_endpoint != NULL) ai_conversation_set_tool_endpoint(self->conversation, previous_endpoint, NULL);
		return FALSE;
	}
	if (endpoint == NULL && ai_conversation_get_tool_endpoint(conversation) != NULL)
		ai_conversation_set_tool_endpoint(conversation, NULL, NULL);
	g_set_object(&self->conversation, conversation);
	/* Keep one copy of host instructions across repeated /new and provider changes. */
	if (self->base_prompt == NULL)
		self->base_prompt = g_strdup(ai_conversation_get_system_prompt(conversation) != NULL ?
			ai_conversation_get_system_prompt(conversation) : "");
	if (endpoint != NULL)
	{
		description = ai_mcp_host_create_server(self);
		instructions = g_strdup_printf("%s\n\n%s", self->base_prompt, mcp_server_get_instructions(description));
		ai_conversation_set_system_prompt(conversation, instructions);
	}
	else
		ai_conversation_set_system_prompt(conversation, self->base_prompt);
	return TRUE;
}

/* Full-duplex stdio bridge. Cancellation closes both directions when either
 * peer leaves; bytes are streamed with GIO's fixed-size splice buffers. */
typedef struct
{
	GMainLoop *loop;
	GCancellable *cancellable;
	guint pending;
	GError *error;
} Proxy;

static void
proxy_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	Proxy *proxy = (Proxy *)user_data;
	g_autoptr(GError) error = NULL;
	g_output_stream_splice_finish(G_OUTPUT_STREAM(source), result, &error);
	if (error != NULL && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && proxy->error == NULL)
		proxy->error = g_steal_pointer(&error);
	g_cancellable_cancel(proxy->cancellable);
	if (--proxy->pending == 0) g_main_loop_quit(proxy->loop);
}

gint
ai_mcp_connect(const gchar *path, GError **error)
{
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GSocketConnection) connection = NULL;
	g_autoptr(GInputStream) input = g_unix_input_stream_new(STDIN_FILENO, FALSE);
	g_autoptr(GOutputStream) output = g_unix_output_stream_new(STDOUT_FILENO, FALSE);
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	Proxy proxy = { loop, cancellable, 2, NULL };
	gint output_flags;
	if (!g_path_is_absolute(path))
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "--mcp-connect requires an absolute socket path");
		return 2;
	}
	signal(SIGPIPE, SIG_IGN);
	address = g_unix_socket_address_new(path);
	g_socket_client_set_timeout(client, 10);
	connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, error);
	if (connection == NULL) return 1;
	output_flags = fcntl(STDOUT_FILENO, F_GETFL);
	if (output_flags < 0 || fcntl(STDOUT_FILENO, F_SETFL, output_flags | O_NONBLOCK) < 0)
	{
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			"Cannot make MCP bridge output nonblocking: %s", g_strerror(errno));
		return 1;
	}
	/* A connect deadline must not become an idle session deadline. */
	g_socket_set_timeout(g_socket_connection_get_socket(connection), 0);
	g_output_stream_splice_async(g_io_stream_get_output_stream(G_IO_STREAM(connection)), input,
		G_OUTPUT_STREAM_SPLICE_NONE, G_PRIORITY_DEFAULT, cancellable, proxy_done, &proxy);
	g_output_stream_splice_async(output, g_io_stream_get_input_stream(G_IO_STREAM(connection)),
		G_OUTPUT_STREAM_SPLICE_NONE, G_PRIORITY_DEFAULT, cancellable, proxy_done, &proxy);
	g_main_loop_run(loop);
	/* This function can also be called by an in-process test driver. */
	fcntl(STDOUT_FILENO, F_SETFL, output_flags);
	if (proxy.error != NULL)
	{
		g_propagate_error(error, proxy.error);
		return 1;
	}
	return 0;
}

/* Negotiate using a temporary conversation before changing the live provider.
 * Applying its already-validated endpoint to the real conversation is then
 * independent of the outgoing provider's dialect. */
gboolean
ai_mcp_host_set_provider(AiMcpHost *self, GObject *provider,
                         const gchar *executable, gboolean inject, GError **error)
{
	g_autoptr(AiConversation) previous = g_object_ref(self->conversation);
	g_autoptr(AiConversation) candidate = NULL;
	g_autoptr(GObject) old_provider = g_object_ref(ai_conversation_get_provider(previous));
	g_autoptr(AiAgentEndpoint) old_endpoint = NULL;
	g_autoptr(AiAgentEndpoint) new_endpoint = NULL;
	g_autofree gchar *new_prompt = NULL;
	g_autofree gchar *old_config = NULL;
	g_autofree gchar *old_prompt = g_strdup(ai_conversation_get_system_prompt(previous));
	gboolean success;

	if (ai_conversation_get_busy(previous) || self->operation_pending)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Cannot switch provider during a turn");
		return FALSE;
	}
	candidate = ai_conversation_new(provider);
	ai_conversation_set_system_prompt(candidate, ai_conversation_get_system_prompt(previous));
	/* Borrow the brigade so the negotiated tool catalog stays identical. */
	ai_conversation_set_brigade(candidate, ai_conversation_get_brigade(previous));
	if (ai_conversation_get_tool_endpoint(previous) != NULL)
		old_endpoint = ai_agent_endpoint_copy(ai_conversation_get_tool_endpoint(previous));
	if (self->config_path != NULL) g_file_get_contents(self->config_path, &old_config, NULL, NULL);
	if (!ai_mcp_host_bind(self, candidate, executable, inject, error))
	{
		g_set_object(&self->conversation, previous);
		return FALSE;
	}
	if (ai_conversation_get_tool_endpoint(candidate) != NULL)
		new_endpoint = ai_agent_endpoint_copy(ai_conversation_get_tool_endpoint(candidate));
	new_prompt = g_strdup(ai_conversation_get_system_prompt(candidate));
	g_set_object(&self->conversation, previous);
	/* bind cleared the old endpoint before replacing its conversation. */
	success = ai_conversation_set_provider(previous, provider, error);
	if (success)
	{
		if (new_endpoint != NULL)
			success = ai_conversation_set_tool_endpoint(previous, new_endpoint, error);
		if (success) ai_conversation_set_system_prompt(previous, new_prompt);
	}
	if (!success)
	{
		if (AI_IS_TOOL_ENDPOINT_CONSUMER(provider))
			ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(provider), NULL);
		if (ai_conversation_get_tool_endpoint(previous) != NULL)
			ai_conversation_set_tool_endpoint(previous, NULL, NULL);
		ai_conversation_set_provider(previous, old_provider, NULL);
		if (old_config != NULL) g_file_set_contents(self->config_path, old_config, -1, NULL);
		if (old_endpoint != NULL) ai_conversation_set_tool_endpoint(previous, old_endpoint, NULL);
		ai_conversation_set_system_prompt(previous, old_prompt);
	}
	return success;
}
