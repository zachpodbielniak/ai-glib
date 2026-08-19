/*
 * ai-agent-host.c - What the embedding application provides to agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-agent-host.h"

/* ── AiAgentHost ─────────────────────────────────────────────────── */

G_DEFINE_INTERFACE (AiAgentHost, ai_agent_host, G_TYPE_OBJECT)

static void
ai_agent_host_default_init (AiAgentHostInterface *iface)
{
    (void)iface;
}

/**
 * ai_agent_host_provision:
 * @self: an #AiAgentHost
 * @agent: the #AiAgent being started
 * @tool_allowlist: (array zero-terminated=1): what this agent may call
 * @out_endpoint: (out) (transfer full): where its tools live
 * @error: (out) (optional): return location for a #GError
 *
 * Grants @agent a scoped tool endpoint.
 *
 * Returns: %TRUE on success.
 */
gboolean
ai_agent_host_provision (AiAgentHost *self, AiAgent *agent,
                         const gchar * const *tool_allowlist,
                         AiAgentEndpoint **out_endpoint, GError **error)
{
    AiAgentHostInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_HOST(self), FALSE);

    iface = AI_AGENT_HOST_GET_IFACE(self);
    g_return_val_if_fail(iface->provision != NULL, FALSE);

    return iface->provision(self, agent, tool_allowlist, out_endpoint, error);
}

/**
 * ai_agent_host_revoke:
 * @self: an #AiAgentHost
 * @agent: the #AiAgent
 *
 * Withdraws whatever ai_agent_host_provision() granted.
 */
void
ai_agent_host_revoke (AiAgentHost *self, AiAgent *agent)
{
    AiAgentHostInterface *iface;

    g_return_if_fail(AI_IS_AGENT_HOST(self));

    iface = AI_AGENT_HOST_GET_IFACE(self);
    if (iface->revoke != NULL) iface->revoke(self, agent);
}

/**
 * ai_agent_host_list_tools:
 * @self: an #AiAgentHost
 * @tool_allowlist: (array zero-terminated=1): what the agent may call
 *
 * The in-process counterpart of provisioning: tools handed over directly
 * rather than reached through an endpoint.
 *
 * Returns: (transfer container) (element-type AiTool) (nullable): tools.
 */
GList *
ai_agent_host_list_tools (AiAgentHost *self,
                          const gchar * const *tool_allowlist)
{
    AiAgentHostInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_HOST(self), NULL);

    iface = AI_AGENT_HOST_GET_IFACE(self);
    if (iface->list_tools == NULL) return NULL;

    return iface->list_tools(self, tool_allowlist);
}

/**
 * ai_agent_host_request_approval_async:
 * @self: an #AiAgentHost
 * @agent: the #AiAgent asking
 * @tool: the tool it wants to call
 * @summary: what it intends to do
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the answer
 * @user_data: data for @callback
 *
 * Asks a human to confirm.  Lets ai-glib express "this needs permission"
 * without owning a user interface -- one host prompts in a minibuffer,
 * another posts to a chat room, and neither belongs in this library.
 */
void
ai_agent_host_request_approval_async (AiAgentHost *self, AiAgent *agent,
                                      const gchar *tool, const gchar *summary,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data)
{
    AiAgentHostInterface *iface;

    g_return_if_fail(AI_IS_AGENT_HOST(self));

    iface = AI_AGENT_HOST_GET_IFACE(self);
    g_return_if_fail(iface->request_approval_async != NULL);

    iface->request_approval_async(self, agent, tool, summary,
                                  cancellable, callback, user_data);
}

/**
 * ai_agent_host_request_approval_finish:
 * @self: an #AiAgentHost
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the call was approved.
 */
gboolean
ai_agent_host_request_approval_finish (AiAgentHost *self, GAsyncResult *result,
                                       GError **error)
{
    AiAgentHostInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_HOST(self), FALSE);

    iface = AI_AGENT_HOST_GET_IFACE(self);
    g_return_val_if_fail(iface->request_approval_finish != NULL, FALSE);

    return iface->request_approval_finish(self, result, error);
}
