/*
 * ai-agent-worker.h - How an agent actually runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "agent/ai-agent-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_AGENT_WORKER (ai_agent_worker_get_type())

G_DECLARE_INTERFACE (AiAgentWorker, ai_agent_worker, AI, AGENT_WORKER, GObject)

typedef struct _AiAgent AiAgent;

/**
 * AiAgentWorkerInterface:
 * @parent_iface: the parent interface
 * @start_async: begins a run
 * @start_finish: completes ai_agent_worker_start_async()
 * @poll_state: reports state for workers that cannot push it
 * @read_output: returns whatever the run has produced so far
 * @cancel: stops a run
 * @_reserved: reserved for future expansion
 *
 * How an agent executes: in this process, as a subprocess, as a detached
 * job, on another machine.
 *
 * @poll_state exists for workers that have no way to push.  A detached,
 * double-forked process is reparented to init, so no SIGCHLD ever
 * reaches the spawning program and there is no completion callback to
 * hook -- such a worker reports through @poll_state and the brigade
 * sweeps it.  An in-process worker leaves @poll_state %NULL and simply
 * emits #AiAgent::state-changed.
 *
 * That asymmetry is the whole reason this is an interface rather than an
 * enum: it lets a file-sentinel job registry become a worker
 * implementation instead of a special case threaded through the
 * orchestrator.
 */
struct _AiAgentWorkerInterface
{
    GTypeInterface parent_iface;

    void     (*start_async)  (AiAgentWorker       *self,
                              AiAgent             *agent,
                              const gchar         *prompt,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);
    gboolean (*start_finish) (AiAgentWorker       *self,
                              GAsyncResult        *result,
                              GError             **error);

    gboolean (*poll_state)   (AiAgentWorker       *self,
                              AiAgent             *agent,
                              AiAgentState        *out_state,
                              GError             **error);
    gchar *  (*read_output)  (AiAgentWorker       *self,
                              AiAgent             *agent,
                              GError             **error);
    gboolean (*cancel)       (AiAgentWorker       *self,
                              AiAgent             *agent,
                              GError             **error);

    /*< private >*/
    gpointer _reserved[8];
};

void     ai_agent_worker_start_async  (AiAgentWorker *self, AiAgent *agent,
                                       const gchar *prompt,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback,
                                       gpointer user_data);
gboolean ai_agent_worker_start_finish (AiAgentWorker *self,
                                       GAsyncResult *result, GError **error);
gboolean ai_agent_worker_poll_state   (AiAgentWorker *self, AiAgent *agent,
                                       AiAgentState *out_state, GError **error);
gchar   *ai_agent_worker_read_output  (AiAgentWorker *self, AiAgent *agent,
                                       GError **error);
gboolean ai_agent_worker_cancel       (AiAgentWorker *self, AiAgent *agent,
                                       GError **error);

gboolean ai_agent_worker_can_poll (AiAgentWorker *self);

G_END_DECLS
