/*
 * ai-agent-isolation.c - Where an agent is allowed to make a mess
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-agent-isolation.h"

G_DEFINE_INTERFACE (AiAgentIsolation, ai_agent_isolation, G_TYPE_OBJECT)

static void
ai_agent_isolation_default_init (AiAgentIsolationInterface *iface)
{
    (void)iface;
}

/**
 * ai_agent_isolation_prepare:
 * @self: an #AiAgentIsolation
 * @agent: the #AiAgent about to run
 * @out_cwd: (out) (optional) (transfer full): where the agent should run
 * @out_env: (out) (optional) (transfer full) (element-type utf8 utf8):
 *   extra environment for the worker
 * @error: (out) (optional): return location for a #GError
 *
 * Creates the sandbox.
 *
 * Returns: %TRUE on success.  On failure the caller must still call
 *   ai_agent_isolation_teardown(), since a half-created sandbox is
 *   exactly what leaks.
 */
gboolean
ai_agent_isolation_prepare (AiAgentIsolation *self, AiAgent *agent,
                            gchar **out_cwd, GHashTable **out_env,
                            GError **error)
{
    AiAgentIsolationInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_ISOLATION(self), FALSE);

    iface = AI_AGENT_ISOLATION_GET_IFACE(self);
    g_return_val_if_fail(iface->prepare != NULL, FALSE);

    return iface->prepare(self, agent, out_cwd, out_env, error);
}

/**
 * ai_agent_isolation_teardown:
 * @self: an #AiAgentIsolation
 * @agent: the #AiAgent
 *
 * Destroys the sandbox.  Safe to call twice, and safe to call after a
 * failed prepare -- it runs from an unwind path, where the state is
 * least predictable.
 */
void
ai_agent_isolation_teardown (AiAgentIsolation *self, AiAgent *agent)
{
    AiAgentIsolationInterface *iface;

    g_return_if_fail(AI_IS_AGENT_ISOLATION(self));

    iface = AI_AGENT_ISOLATION_GET_IFACE(self);
    if (iface->teardown != NULL) iface->teardown(self, agent);
}

/**
 * ai_agent_isolation_describe:
 * @self: an #AiAgentIsolation
 *
 * Returns: (transfer full) (nullable): a short label for a UI.
 */
gchar *
ai_agent_isolation_describe (AiAgentIsolation *self)
{
    AiAgentIsolationInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_ISOLATION(self), NULL);

    iface = AI_AGENT_ISOLATION_GET_IFACE(self);
    if (iface->describe == NULL) return NULL;

    return iface->describe(self);
}
