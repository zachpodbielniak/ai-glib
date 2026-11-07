/*
 * ai-mcp-config.c - One MCP server list, in every CLI's dialect
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <glib/gstdio.h>

#include "core/ai-mcp-config.h"
#include "core/ai-error.h"
#include "core/ai-tool-endpoint-consumer.h"

/* ------------------------------------------------------------------
 * Loading
 * ------------------------------------------------------------------ */

JsonObject *
ai_mcp_config_load_servers(
	const gchar * const  *paths,
	GError              **error
){
	g_autoptr(JsonObject) merged = json_object_new();
	guint                 i;

	for (i = 0; paths != NULL && paths[i] != NULL; i++)
	{
		g_autoptr(JsonParser) parser = NULL;
		g_autoptr(GList)      names  = NULL;
		JsonNode             *root;
		JsonNode             *section;
		JsonObject           *servers;
		GList                *l;

		if (!g_file_test(paths[i], G_FILE_TEST_EXISTS))
			continue;

		parser = json_parser_new();
		if (!json_parser_load_from_file(parser, paths[i], error))
		{
			g_prefix_error(error, "%s: ", paths[i]);
			return NULL;
		}

		root = json_parser_get_root(parser);
		if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root) || json_node_get_object(root) == NULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "%s: an MCP configuration must be a JSON object", paths[i]);
			return NULL;
		}

		section = json_object_get_member(json_node_get_object(root), "mcpServers");
		if (section == NULL)
			continue;
		if (!JSON_NODE_HOLDS_OBJECT(section) || json_node_get_object(section) == NULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "%s: mcpServers must be a JSON object", paths[i]);
			return NULL;
		}

		servers = json_node_get_object(section);
		names = json_object_get_members(servers);
		for (l = names; l != NULL; l = l->next)
			json_object_set_member(merged, l->data,
			                       json_node_copy(json_object_get_member(servers, l->data)));
	}

	return (JsonObject *)g_steal_pointer(&merged);
}

/* ------------------------------------------------------------------
 * Reading one server
 * ------------------------------------------------------------------ */

typedef struct
{
	const gchar *command;  /* borrowed, nullable */
	JsonArray   *args;     /* borrowed, nullable */
	JsonObject  *env;      /* borrowed, nullable */
	const gchar *url;      /* borrowed, nullable */
	const gchar *type;     /* borrowed, nullable: "http", "sse", "stdio" */
} Server;

static const gchar *
string_member(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node = json_object_get_member(object, name);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;
	return json_node_get_string(node);
}

/*
 * One entry of the map, read defensively: a server that is neither a
 * command nor a URL is reported rather than rendered as an empty table
 * some CLI would then refuse to start with.
 */
static gboolean
read_server(
	JsonNode   *node,
	Server     *out,
	GPtrArray  *skipped,
	const gchar *name
){
	JsonObject *object;
	JsonNode   *member;

	memset(out, 0, sizeof *out);
	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node) || json_node_get_object(node) == NULL)
	{
		if (skipped != NULL)
			g_ptr_array_add(skipped, g_strdup_printf("%s: not a JSON object", name));
		return FALSE;
	}

	object = json_node_get_object(node);
	out->command = string_member(object, "command");
	out->url = string_member(object, "url");
	out->type = string_member(object, "type");

	member = json_object_get_member(object, "args");
	if (member != NULL && JSON_NODE_HOLDS_ARRAY(member))
		out->args = json_node_get_array(member);
	member = json_object_get_member(object, "env");
	if (member != NULL && JSON_NODE_HOLDS_OBJECT(member) &&
	    json_object_get_size(json_node_get_object(member)) > 0)
		out->env = json_node_get_object(member);

	if ((out->command == NULL || *out->command == '\0') && (out->url == NULL || *out->url == '\0'))
	{
		if (skipped != NULL)
			g_ptr_array_add(skipped, g_strdup_printf("%s: names neither a command nor a url", name));
		return FALSE;
	}
	return TRUE;
}

/* A name every dialect can spell without quoting: codex's dotted -c path cannot. */
static gboolean
name_is_bare(
	const gchar *name
){
	const gchar *p;

	if (name == NULL || *name == '\0')
		return FALSE;
	for (p = name; *p != '\0'; p++)
		if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
			return FALSE;
	return TRUE;
}

/* A JSON string literal, which is also a valid TOML basic string. */
static gchar *
quoted(
	const gchar *value
){
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_VALUE);

	json_node_set_string(node, value);
	return json_to_string(node, FALSE);
}

static gchar *
string_array_literal(
	JsonArray *args
){
	GString *out = g_string_new("[");
	guint    n   = args != NULL ? json_array_get_length(args) : 0;
	guint    i;

	for (i = 0; i < n; i++)
	{
		JsonNode         *e = json_array_get_element(args, i);
		g_autofree gchar *q = NULL;

		if (!JSON_NODE_HOLDS_VALUE(e) || json_node_get_value_type(e) != G_TYPE_STRING)
			continue;
		q = quoted(json_node_get_string(e));
		g_string_append_printf(out, "%s%s", out->len > 1 ? ", " : "", q);
	}
	g_string_append_c(out, ']');
	return g_string_free(out, FALSE);
}

/* Sorted names, so the same map always renders the same bytes. */
static GList *
sorted_names(
	JsonObject *object
){
	return g_list_sort(json_object_get_members(object), (GCompareFunc)g_strcmp0);
}

/* ------------------------------------------------------------------
 * Dialects
 * ------------------------------------------------------------------ */

static gchar *
translate_claude(
	JsonObject *servers
){
	g_autoptr(JsonNode)   node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) root = json_object_new();

	json_object_set_object_member(root, "mcpServers", json_object_ref(servers));
	json_node_set_object(node, root);
	return json_to_string(node, TRUE);
}

static gchar *
translate_opencode(
	JsonObject *servers,
	GPtrArray  *skipped
){
	g_autoptr(JsonNode)   node = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) root = json_object_new();
	JsonObject           *mcp  = json_object_new();
	g_autoptr(GList)      names = sorted_names(servers);
	GList                *l;

	for (l = names; l != NULL; l = l->next)
	{
		const gchar *name  = l->data;
		JsonObject  *entry;
		Server       srv;

		if (!read_server(json_object_get_member(servers, name), &srv, skipped, name))
			continue;

		entry = json_object_new();
		if (srv.command != NULL && *srv.command != '\0')
		{
			JsonArray *command = json_array_new();
			guint      i;

			json_object_set_string_member(entry, "type", "local");
			json_array_add_string_element(command, srv.command);
			for (i = 0; srv.args != NULL && i < json_array_get_length(srv.args); i++)
			{
				JsonNode *e = json_array_get_element(srv.args, i);

				if (JSON_NODE_HOLDS_VALUE(e) && json_node_get_value_type(e) == G_TYPE_STRING)
					json_array_add_string_element(command, json_node_get_string(e));
			}
			json_object_set_array_member(entry, "command", command);
			if (srv.env != NULL)
				json_object_set_object_member(entry, "environment", json_object_ref(srv.env));
		}
		else
		{
			json_object_set_string_member(entry, "type", "remote");
			json_object_set_string_member(entry, "url", srv.url);
		}
		json_object_set_boolean_member(entry, "enabled", TRUE);
		json_object_set_object_member(mcp, name, entry);
	}

	json_object_set_object_member(root, "mcp", mcp);
	json_node_set_object(node, root);
	return json_to_string(node, TRUE);
}

static gchar *
translate_grok(
	JsonObject *servers,
	GPtrArray  *skipped
){
	GString          *out   = g_string_new(NULL);
	g_autoptr(GList)  names = sorted_names(servers);
	GList            *l;

	for (l = names; l != NULL; l = l->next)
	{
		const gchar      *name  = l->data;
		g_autofree gchar *key   = NULL;
		Server            srv;

		if (!read_server(json_object_get_member(servers, name), &srv, skipped, name))
			continue;

		/* A bare key where TOML allows one, quoted otherwise. */
		key = name_is_bare(name) ? g_strdup(name) : quoted(name);
		g_string_append_printf(out, "%s[mcp_servers.%s]\n", out->len > 0 ? "\n" : "", key);

		if (srv.command != NULL && *srv.command != '\0')
		{
			g_autofree gchar *command = quoted(srv.command);
			g_autofree gchar *args    = string_array_literal(srv.args);

			g_string_append_printf(out, "command = %s\nargs = %s\nenabled = true\n", command, args);
			if (srv.env != NULL)
			{
				g_autoptr(GList) vars = sorted_names(srv.env);
				GList           *v;

				g_string_append_printf(out, "\n[mcp_servers.%s.env]\n", key);
				for (v = vars; v != NULL; v = v->next)
				{
					const gchar      *value = string_member(srv.env, v->data);
					g_autofree gchar *qk    = quoted(v->data);
					g_autofree gchar *qv    = NULL;

					if (value == NULL)
						continue;
					qv = quoted(value);
					g_string_append_printf(out, "%s = %s\n", qk, qv);
				}
			}
		}
		else
		{
			g_autofree gchar *url = quoted(srv.url);

			g_string_append_printf(out, "url = %s\nenabled = true\n", url);
		}
	}

	return g_string_free(out, FALSE);
}

static gchar *
translate_codex(
	JsonObject *servers,
	GPtrArray  *skipped
){
	GString          *out   = g_string_new(NULL);
	g_autoptr(GList)  names = sorted_names(servers);
	GList            *l;

	for (l = names; l != NULL; l = l->next)
	{
		const gchar      *name    = l->data;
		g_autofree gchar *command = NULL;
		g_autofree gchar *args    = NULL;
		Server            srv;

		if (!read_server(json_object_get_member(servers, name), &srv, skipped, name))
			continue;

		/*
		 * codex takes these as -c assignments on its command line.  An
		 * environment there is readable by every user through the process
		 * table, and the environments these files carry are resolved
		 * secrets: leaving the server out, by name, is the safe answer.
		 */
		if (srv.env != NULL)
		{
			if (skipped != NULL)
				g_ptr_array_add(skipped, g_strdup_printf(
					"%s: its environment would be on codex's command line, "
					"where every user can read it", name));
			continue;
		}
		if (srv.command == NULL || *srv.command == '\0')
		{
			if (skipped != NULL)
				g_ptr_array_add(skipped, g_strdup_printf(
					"%s: codex is given only command servers this way, not URLs", name));
			continue;
		}
		if (!name_is_bare(name))
		{
			if (skipped != NULL)
				g_ptr_array_add(skipped, g_strdup_printf(
					"%s: codex cannot address a server name with characters other "
					"than letters, digits, '_' and '-'", name));
			continue;
		}

		command = quoted(srv.command);
		args = string_array_literal(srv.args);
		g_string_append_printf(out, "mcp_servers.%s.command=%s\n", name, command);
		g_string_append_printf(out, "mcp_servers.%s.args=%s\n", name, args);
		/*
		 * codex exec runs with approvals off, which rejects every MCP call
		 * not pre-approved.  These are the servers the project itself
		 * declares, so their tools are approved -- scoped to the server,
		 * never a global policy.
		 */
		g_string_append_printf(out, "mcp_servers.%s.default_tools_approval_mode=\"approve\"\n", name);
	}

	return g_string_free(out, FALSE);
}

gchar *
ai_mcp_config_translate(
	JsonObject   *servers,
	const gchar  *kind,
	GPtrArray    *skipped,
	GError      **error
){
	g_return_val_if_fail(servers != NULL, NULL);
	g_return_val_if_fail(kind != NULL, NULL);

	if (g_strcmp0(kind, AI_ENDPOINT_KIND_MCP_CONFIG) == 0)
		return translate_claude(servers);
	if (g_strcmp0(kind, AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE) == 0)
		return translate_opencode(servers, skipped);
	if (g_strcmp0(kind, AI_ENDPOINT_KIND_MCP_CONFIG_GROK) == 0)
		return translate_grok(servers, skipped);
	if (g_strcmp0(kind, AI_ENDPOINT_KIND_MCP_CONFIG_CODEX) == 0)
		return translate_codex(servers, skipped);

	g_set_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
	            "no MCP configuration dialect called '%s'", kind);
	return NULL;
}

/* ------------------------------------------------------------------
 * Endpoint
 * ------------------------------------------------------------------ */

/* The dialects, in the order a client is asked about them. */
static const struct
{
	const gchar *kind;
	const gchar *suffix;
} DIALECTS[] = {
	{ AI_ENDPOINT_KIND_MCP_CONFIG_CODEX,    ".conf" },
	{ AI_ENDPOINT_KIND_MCP_CONFIG_GROK,     ".toml" },
	{ AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE, ".json" },
	{ AI_ENDPOINT_KIND_MCP_CONFIG,          ".json" },
};

AiAgentEndpoint *
ai_mcp_config_endpoint_for(
	AiCliClient  *client,
	JsonObject   *servers,
	const gchar  *dir,
	GPtrArray    *skipped,
	GError      **error
){
	AiToolEndpointConsumer *consumer;
	const gchar            *kind   = NULL;
	const gchar            *suffix = NULL;
	g_autofree gchar       *text   = NULL;
	g_autofree gchar       *name   = NULL;
	g_autofree gchar       *path   = NULL;
	g_autofree gchar       *uuid   = NULL;
	guint                   i;

	g_return_val_if_fail(AI_IS_CLI_CLIENT(client), NULL);
	g_return_val_if_fail(servers != NULL, NULL);
	g_return_val_if_fail(dir != NULL, NULL);

	consumer = AI_TOOL_ENDPOINT_CONSUMER(client);
	for (i = 0; i < G_N_ELEMENTS(DIALECTS); i++)
	{
		if (ai_tool_endpoint_consumer_supports_kind(consumer, DIALECTS[i].kind))
		{
			kind = DIALECTS[i].kind;
			suffix = DIALECTS[i].suffix;
			break;
		}
	}
	if (kind == NULL || json_object_get_size(servers) == 0)
		return NULL;

	{
		g_autoptr(GPtrArray) left_out = g_ptr_array_new_with_free_func(g_free);

		text = ai_mcp_config_translate(servers, kind, left_out, error);
		for (i = 0; skipped != NULL && i < left_out->len; i++)
			g_ptr_array_add(skipped, g_strdup(g_ptr_array_index(left_out, i)));
		if (text == NULL)
			return NULL;
		/* Nothing survived: an endpoint naming no server grants nothing. */
		if (left_out->len >= json_object_get_size(servers))
			return NULL;
	}

	if (g_mkdir_with_parents(dir, 0700) != 0)
	{
		g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
		            "cannot create %s: %s", dir, g_strerror(errno));
		return NULL;
	}

	/* A random name: the directory is shared by every session the host runs. */
	uuid = g_uuid_string_random();
	name = g_strconcat("mcp-", uuid, suffix, NULL);
	path = g_build_filename(dir, name, NULL);

	/* 0600 before any byte lands: an opencode environment holds secrets. */
	{
		gint fd = g_open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

		if (fd < 0)
		{
			g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
			            "cannot create %s: %s", path, g_strerror(errno));
			return NULL;
		}
		g_close(fd, NULL);
	}
	if (!g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error))
	{
		g_unlink(path);
		return NULL;
	}

	return ai_agent_endpoint_new(kind, path);
}
