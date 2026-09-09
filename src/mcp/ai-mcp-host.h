/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <ai-glib.h>
#include <mcp.h>

/* Private application bridge: public ai-glib consumers need no MCP headers. */
#define AI_TYPE_MCP_HOST (ai_mcp_host_get_type())
G_DECLARE_FINAL_TYPE(AiMcpHost, ai_mcp_host, AI, MCP_HOST, GObject)

AiMcpHost *ai_mcp_host_new(AiConversation *conversation, const gchar *name,
                          const gchar * const *tools, gboolean all_tools,
                          GError **error);
const gchar * const *ai_mcp_host_catalog(void);
McpServer *ai_mcp_host_create_server(AiMcpHost *self);
gboolean ai_mcp_host_start(AiMcpHost *self, const gchar *socket_path,
                           gboolean stdio, GError **error);
void ai_mcp_host_stop(AiMcpHost *self);
const gchar *ai_mcp_host_get_socket_path(AiMcpHost *self);
gboolean ai_mcp_host_bind(AiMcpHost *self, AiConversation *conversation,
                          const gchar *executable, gboolean inject, GError **error);
gint ai_mcp_host_run(AiMcpHost *self);
gint ai_mcp_connect(const gchar *path, GError **error);
gboolean ai_mcp_host_set_provider(AiMcpHost *self, GObject *provider,
                                  const gchar *executable, gboolean inject, GError **error);
