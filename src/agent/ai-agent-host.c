/*
 * ai-agent-host.c - What the embedding application provides to agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-agent-host.h"

/* ── AiAgentEndpoint ─────────────────────────────────────────────── */

G_DEFINE_BOXED_TYPE (AiAgentEndpoint, ai_agent_endpoint,
                     ai_agent_endpoint_copy, ai_agent_endpoint_free)

/**
 * ai_agent_endpoint_new:
 * @kind: "mcp-config", "http-url" or "stdio"
 * @value: the path, URL or command line
 *
 * Returns: (transfer full): a new #AiAgentEndpoint.
 */
AiAgentEndpoint *
ai_agent_endpoint_new (const gchar *kind, const gchar *value)
{
    AiAgentEndpoint *self = g_new0(AiAgentEndpoint, 1);

    self->kind  = g_strdup(kind);
    self->value = g_strdup(value);
    self->env   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        g_free, g_free);
    return self;
}

/**
 * ai_agent_endpoint_copy:
 * @self: an #AiAgentEndpoint
 *
 * Returns: (transfer full): a deep copy.
 */
AiAgentEndpoint *
ai_agent_endpoint_copy (const AiAgentEndpoint *self)
{
    AiAgentEndpoint *copy;
    GHashTableIter iter;
    gpointer k, v;

    g_return_val_if_fail(self != NULL, NULL);

    copy = ai_agent_endpoint_new(self->kind, self->value);
    copy->ttl_seconds = self->ttl_seconds;
    if (self->env != NULL)
    {
        g_hash_table_iter_init(&iter, self->env);
        while (g_hash_table_iter_next(&iter, &k, &v))
            g_hash_table_insert(copy->env, g_strdup(k), g_strdup(v));
    }
    return copy;
}

/**
 * ai_agent_endpoint_free:
 * @self: (transfer full): an #AiAgentEndpoint
 */
void
ai_agent_endpoint_free (AiAgentEndpoint *self)
{
    if (self == NULL) return;
    g_free(self->kind);
    g_free(self->value);
    if (self->env != NULL) g_hash_table_destroy(self->env);
    g_free(self);
}

/**
 * ai_agent_endpoint_set_env:
 * @self: an #AiAgentEndpoint
 * @key: variable name
 * @value: variable value
 *
 * Sets an environment variable the worker must pass to the agent.
 *
 * Credentials belong here rather than on a command line: argv is
 * readable by any process on the machine through /proc, and a token that
 * grants tool access is not something to publish that way.
 */
void
ai_agent_endpoint_set_env (AiAgentEndpoint *self, const gchar *key,
                           const gchar *value)
{
    g_return_if_fail(self != NULL);
    g_return_if_fail(key != NULL);
    g_hash_table_replace(self->env, g_strdup(key), g_strdup(value));
}

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
