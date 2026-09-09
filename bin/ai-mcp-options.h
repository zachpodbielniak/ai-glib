/* Shared MCP options for ai and ai-tui. SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include "mcp/ai-mcp-host.h"

static gboolean opt_mcp_server;
static gboolean opt_mcp_all_tools;
static gboolean opt_mcp_list_tools;
static gboolean opt_mcp_no_inject;
static gchar *opt_mcp_socket;
static gchar *opt_mcp_connect;
static gchar **opt_mcp_tools;
static gchar *mcp_executable;

static const GOptionEntry mcp_option_entries[] = {
	{ "mcp-server", 0, 0, G_OPTION_ARG_NONE, &opt_mcp_server, "Serve MCP over stdio (headless; no tools enabled by default)", NULL },
	{ "mcp-socket", 0, 0, G_OPTION_ARG_FILENAME, &opt_mcp_socket, "Serve MCP on a private Unix socket (with --mcp-server: socket instead of stdio)", "PATH" },
	{ "mcp-tools", 0, 0, G_OPTION_ARG_STRING_ARRAY, &opt_mcp_tools, "Enable named MCP tools; comma-separated, repeatable", "NAMES" },
	{ "mcp-all-tools", 0, 0, G_OPTION_ARG_NONE, &opt_mcp_all_tools, "Enable every host MCP tool, including turn control and background agents", NULL },
	{ "mcp-list-tools", 0, 0, G_OPTION_ARG_NONE, &opt_mcp_list_tools, "List available host MCP tool names and exit", NULL },
	{ "mcp-no-inject", 0, 0, G_OPTION_ARG_NONE, &opt_mcp_no_inject, "Keep MCP for external controllers; do not inject into CLI providers", NULL },
	{ "mcp-connect", 0, 0, G_OPTION_ARG_FILENAME, &opt_mcp_connect, "Bridge stdio to an existing host MCP Unix socket", "PATH" },
	{ NULL, 0, 0, 0, NULL, NULL, NULL }
};

/* A process-scoped cleanup must stop before dropping the last application
 * reference; in-flight provider callbacks can retain their own host reference. */
static void
mcp_cleanup(AiMcpHost **host)
{
	if (*host != NULL)
	{
		ai_mcp_host_stop(*host);
		g_clear_object(host);
	}
}

static gboolean
mcp_requested(void)
{
	return opt_mcp_server || opt_mcp_socket != NULL || opt_mcp_tools != NULL || opt_mcp_all_tools;
}

/* Listing/bridging runs before provider lookup or reading stdin as a prompt. */
static gint
mcp_early(gint argc, const gchar *argv0, gboolean incompatible, GError **error)
{
	guint i;
	if ((opt_mcp_connect != NULL && (mcp_requested() || opt_mcp_list_tools || opt_mcp_no_inject)) ||
	    (opt_mcp_list_tools && (mcp_requested() || opt_mcp_no_inject)) ||
	    ((opt_mcp_server || opt_mcp_connect != NULL || opt_mcp_list_tools) && argc > 1) ||
	    ((mcp_requested() || opt_mcp_connect != NULL) && incompatible) ||
	    (opt_mcp_no_inject && !mcp_requested()))
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			"Conflicting MCP modes: servers/bridges take no prompt; MCP cannot be combined with native launch or other output modes");
		return 2;
	}
	if (opt_mcp_list_tools)
	{
		const gchar * const *names = ai_mcp_host_catalog();
		for (i = 0; names[i] != NULL; i++) g_print("%s\n", names[i]);
		return 0;
	}
	if (opt_mcp_connect != NULL) return ai_mcp_connect(opt_mcp_connect, error);
	if (mcp_requested())
	{
		g_autofree gchar *found = strchr(argv0, G_DIR_SEPARATOR) != NULL ?
			g_canonicalize_filename(argv0, NULL) : g_find_program_in_path(argv0);
		if (found == NULL)
		{
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Cannot resolve MCP bridge executable");
			return 2;
		}
		mcp_executable = g_steal_pointer(&found);
	}
	return -1;
}

/* A server-only process uses exactly the same model and panels as the TUI. */
static gint
mcp_headless(GObject *provider, const gchar *name, const gchar *system_prompt,
             gint max_tokens, gboolean stream, gboolean no_agents)
{
	g_autoptr(AiConversation) conversation = ai_conversation_new(provider);
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiCommandSet) commands = NULL;
	AiMcpHost *host __attribute__((cleanup(mcp_cleanup))) = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *cwd = g_get_current_dir();

	ai_conversation_set_system_prompt(conversation, system_prompt);
	ai_conversation_set_max_tokens(conversation, max_tokens);
	ai_conversation_set_stream(conversation, stream);
	ai_resource_registry_set_working_directory(registry, cwd);
	ai_resource_registry_scan(registry);
	commands = ai_command_set_new(registry);
	ai_conversation_set_command_set(conversation, commands);
	ai_conversation_set_working_directory(conversation, cwd);
	if (!no_agents) ai_conversation_enable_background_agents(conversation, 4);
	host = ai_mcp_host_new(conversation, name, (const gchar * const *)opt_mcp_tools,
	                       opt_mcp_all_tools, &error);
	if (host == NULL) goto failed;
	/* Explicit socket mode stays headless but uses no stdio transport. */
	if (opt_mcp_socket != NULL && !ai_mcp_host_start(host, opt_mcp_socket, FALSE, &error)) goto failed;
	if (!ai_mcp_host_bind(host, conversation, mcp_executable, !opt_mcp_no_inject, &error)) goto failed;
	if (opt_mcp_socket == NULL && !ai_mcp_host_start(host, NULL, TRUE, &error)) goto failed;
	return ai_mcp_host_run(host);
failed:
	g_printerr("%s: %s\n", name, error->message);
	return 2;
}
