/*
 * ai-agent-worker.c - How an agent actually runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-agent-worker.h"

G_DEFINE_INTERFACE (AiAgentWorker, ai_agent_worker, G_TYPE_OBJECT)

static void
ai_agent_worker_default_init (AiAgentWorkerInterface *iface)
{
    (void)iface;
}

/**
 * ai_agent_worker_start_async:
 * @self: an #AiAgentWorker
 * @agent: the #AiAgent to run
 * @prompt: the initial prompt
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the run has been started
 * @user_data: data for @callback
 *
 * Begins a run.  Completion of this call means the agent was *started*,
 * not that it finished -- the run's outcome arrives through
 * #AiAgent::finished or, for a worker that cannot push, through
 * ai_agent_worker_poll_state().
 */
void
ai_agent_worker_start_async (AiAgentWorker *self, AiAgent *agent,
                             const gchar *prompt, GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data)
{
    AiAgentWorkerInterface *iface;

    g_return_if_fail(AI_IS_AGENT_WORKER(self));

    iface = AI_AGENT_WORKER_GET_IFACE(self);
    g_return_if_fail(iface->start_async != NULL);

    iface->start_async(self, agent, prompt, cancellable, callback, user_data);
}

/**
 * ai_agent_worker_start_finish:
 * @self: an #AiAgentWorker
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the run was started.
 */
gboolean
ai_agent_worker_start_finish (AiAgentWorker *self, GAsyncResult *result,
                              GError **error)
{
    AiAgentWorkerInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_WORKER(self), FALSE);

    iface = AI_AGENT_WORKER_GET_IFACE(self);
    g_return_val_if_fail(iface->start_finish != NULL, FALSE);

    return iface->start_finish(self, result, error);
}

/**
 * ai_agent_worker_poll_state:
 * @self: an #AiAgentWorker
 * @agent: the #AiAgent
 * @out_state: (out): the observed state
 * @error: (out) (optional): return location for a #GError
 *
 * Asks a worker that cannot push what state its agent is in.
 *
 * Returns: %FALSE when @self does not implement polling, which is the
 *   normal answer for an in-process worker.  Use
 *   ai_agent_worker_can_poll() to distinguish that from a real failure.
 */
gboolean
ai_agent_worker_poll_state (AiAgentWorker *self, AiAgent *agent,
                            AiAgentState *out_state, GError **error)
{
    AiAgentWorkerInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_WORKER(self), FALSE);

    iface = AI_AGENT_WORKER_GET_IFACE(self);
    if (iface->poll_state == NULL) return FALSE;

    return iface->poll_state(self, agent, out_state, error);
}

/**
 * ai_agent_worker_can_poll:
 * @self: an #AiAgentWorker
 *
 * Returns: %TRUE when @self reports state on demand rather than pushing.
 */
gboolean
ai_agent_worker_can_poll (AiAgentWorker *self)
{
    g_return_val_if_fail(AI_IS_AGENT_WORKER(self), FALSE);
    return AI_AGENT_WORKER_GET_IFACE(self)->poll_state != NULL;
}

/**
 * ai_agent_worker_read_output:
 * @self: an #AiAgentWorker
 * @agent: the #AiAgent
 * @error: (out) (optional): return location for a #GError
 *
 * Returns whatever the run has produced so far.  Valid mid-run: for a
 * worker writing to a file this is a tail, which is how a caller shows
 * progress for something that cannot stream.
 *
 * Returns: (transfer full) (nullable): the output, or %NULL.
 */
gchar *
ai_agent_worker_read_output (AiAgentWorker *self, AiAgent *agent,
                             GError **error)
{
    AiAgentWorkerInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_WORKER(self), NULL);

    iface = AI_AGENT_WORKER_GET_IFACE(self);
    if (iface->read_output == NULL) return NULL;

    return iface->read_output(self, agent, error);
}

/**
 * ai_agent_worker_cancel:
 * @self: an #AiAgentWorker
 * @agent: the #AiAgent
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if the run was stopped.
 */
gboolean
ai_agent_worker_cancel (AiAgentWorker *self, AiAgent *agent, GError **error)
{
    AiAgentWorkerInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_WORKER(self), FALSE);

    iface = AI_AGENT_WORKER_GET_IFACE(self);
    if (iface->cancel == NULL) return FALSE;

    return iface->cancel(self, agent, error);
}
