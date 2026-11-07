/*
 * ai-mcp-config.h - One MCP server list, in every CLI's dialect
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A project names its MCP servers once, in Claude Code's `.mcp.json`
 * schema ({"mcpServers": {NAME: {command, args, env} | {type, url}}}).
 * Claude Code reads that file from its working directory by itself; no
 * other CLI does.  These functions translate the same list into the
 * dialect each CLI takes as a tool endpoint -- codex `-c` assignments, a
 * grok TOML fragment, an opencode config -- so a host can hand every CLI
 * the servers its project declares.
 *
 * What a dialect cannot carry is left out and named, never approximated:
 * a server whose environment holds a secret is not moved onto a command
 * line, where every user on the machine could read it.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "core/ai-cli-client.h"
#include "core/ai-tool-endpoint.h"

G_BEGIN_DECLS

/**
 * ai_mcp_config_load_servers:
 * @paths: (array zero-terminated=1): Claude-schema MCP config files, in
 *   order; a later file's server replaces an earlier one of the same name
 * @error: return location for a #GError
 *
 * Reads and merges the `mcpServers` maps of @paths.  A file that does not
 * exist is skipped, because a project without one is ordinary; a file that
 * exists and is not a JSON object with an object `mcpServers` is an error.
 *
 * Returns: (transfer full) (nullable): the merged server map (possibly
 *   empty), or %NULL with @error set
 */
JsonObject *
ai_mcp_config_load_servers(
	const gchar * const  *paths,
	GError              **error
);

/**
 * ai_mcp_config_translate:
 * @servers: a server map in Claude's schema
 * @kind: the endpoint kind to produce: %AI_ENDPOINT_KIND_MCP_CONFIG,
 *   %AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE, %AI_ENDPOINT_KIND_MCP_CONFIG_GROK
 *   or %AI_ENDPOINT_KIND_MCP_CONFIG_CODEX
 * @skipped: (nullable) (element-type utf8): receives one "NAME: reason"
 *   string for every server the dialect cannot carry
 * @error: return location for a #GError
 *
 * Renders @servers in the dialect @kind names.  The codex dialect is `-c`
 * assignments on a command line, so it carries `command` and `args` and
 * nothing else: a server with an environment or a URL is skipped there.
 *
 * Returns: (transfer full) (nullable): the configuration text, or %NULL
 *   with @error set for an unknown @kind
 */
gchar *
ai_mcp_config_translate(
	JsonObject   *servers,
	const gchar  *kind,
	GPtrArray    *skipped,
	GError      **error
);

/**
 * ai_mcp_config_endpoint_for:
 * @client: the CLI the servers are for
 * @servers: a server map in Claude's schema
 * @dir: a private directory to write the translated file into
 * @skipped: (nullable) (element-type utf8): receives "NAME: reason" for
 *   every server left out
 * @error: return location for a #GError
 *
 * Picks the MCP endpoint kind @client accepts, translates @servers into it
 * and writes the result to a new 0600 file in @dir.  Apply the endpoint
 * with ai_tool_endpoint_consumer_apply(); delete the file named by its
 * `value` once the endpoint is revoked.
 *
 * Returns: (transfer full) (nullable): the endpoint; %NULL without @error
 *   when @client takes no MCP endpoint or nothing in @servers survives
 *   translation; %NULL with @error set when writing fails
 */
AiAgentEndpoint *
ai_mcp_config_endpoint_for(
	AiCliClient  *client,
	JsonObject   *servers,
	const gchar  *dir,
	GPtrArray    *skipped,
	GError      **error
);

G_END_DECLS
