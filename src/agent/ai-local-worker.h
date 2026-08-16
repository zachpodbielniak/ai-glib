/*
 * ai-local-worker.h - Running an agent in this process
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

#include "agent/ai-agent-worker.h"

G_BEGIN_DECLS

#define AI_TYPE_LOCAL_WORKER (ai_local_worker_get_type())

G_DECLARE_FINAL_TYPE (AiLocalWorker, ai_local_worker, AI, LOCAL_WORKER, GObject)

/**
 * ai_local_worker_new:
 *
 * Creates the #AiAgentWorker that runs agents in this process.
 *
 * Each run is one ai_tool_executor_run_async() against the agent's own
 * executor and its own provider, on the thread-default #GMainContext.
 * Nothing blocks: an agent started here is a set of callbacks the main
 * loop will get round to, which is what makes "in the background" mean
 * anything in a single-threaded GLib program.
 *
 * That the agent brings its own provider is the whole point. A
 * conversation held with one model can start an agent on another --- an
 * HTTP turn delegating to a `claude-code` subprocess, say --- because
 * this worker never consults whoever asked for the run.
 *
 * State is pushed, not polled: the worker moves the agent through
 * %AI_AGENT_STATE_RUNNING to a terminal state and #AiAgent::state-changed
 * announces it, so @poll_state is %NULL and ai_brigade_sweep() has
 * nothing to do. A worker that spawns detached processes would be the
 * other shape.
 *
 * Returns: (transfer full): a new #AiLocalWorker
 */
AiLocalWorker *ai_local_worker_new (void);

G_END_DECLS
