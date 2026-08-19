/*
 * ai-tool-endpoint.c - Where an agent's tools live
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Moved verbatim out of agent/ai-agent-host.c so that core/ can apply an
 * endpoint without depending on the agent layer.  See the header.
 */

#include "core/ai-tool-endpoint.h"

G_DEFINE_BOXED_TYPE (AiAgentEndpoint, ai_agent_endpoint,
                     ai_agent_endpoint_copy, ai_agent_endpoint_free)

/**
 * ai_agent_endpoint_new:
 * @kind: one of the AI_ENDPOINT_KIND_* spellings
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
