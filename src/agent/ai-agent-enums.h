/*
 * ai-agent-enums.h - Agent lifecycle states
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

G_BEGIN_DECLS

/**
 * AiAgentState:
 * @AI_AGENT_STATE_IDLE: constructed, never started
 * @AI_AGENT_STATE_QUEUED: waiting for a concurrency slot
 * @AI_AGENT_STATE_STARTING: worker is being spawned
 * @AI_AGENT_STATE_RUNNING: turns are in flight
 * @AI_AGENT_STATE_WAITING_INPUT: blocked on a human answer
 * @AI_AGENT_STATE_BLOCKED: blocked on another agent
 * @AI_AGENT_STATE_DONE: finished successfully
 * @AI_AGENT_STATE_FAILED: finished with an error
 * @AI_AGENT_STATE_CANCELLED: stopped on request
 * @AI_AGENT_STATE_OVER_BUDGET: stopped by a budget ceiling
 * @AI_AGENT_STATE_INTERRUPTED: was running when the host stopped
 *
 * The lifecycle of one agent run.
 *
 * Deliberately a superset of what a detached-process worker can report.
 * A worker that only knows "running", "finished" and "the process is
 * gone" maps onto %AI_AGENT_STATE_RUNNING, %AI_AGENT_STATE_DONE and
 * %AI_AGENT_STATE_FAILED and simply never produces the others, so an
 * existing job registry can be reimplemented as an #AiAgentWorker
 * without inventing states it cannot observe.
 *
 * %AI_AGENT_STATE_INTERRUPTED is distinct from
 * %AI_AGENT_STATE_FAILED on purpose: an agent that was running when the
 * process stopped did not fail, it was abandoned, and nobody watched it
 * stop.  Conflating the two invites automatic resumption of work whose
 * outcome nobody knows.
 */
typedef enum
{
    AI_AGENT_STATE_IDLE = 0,
    AI_AGENT_STATE_QUEUED,
    AI_AGENT_STATE_STARTING,
    AI_AGENT_STATE_RUNNING,
    AI_AGENT_STATE_WAITING_INPUT,
    AI_AGENT_STATE_BLOCKED,
    AI_AGENT_STATE_DONE,
    AI_AGENT_STATE_FAILED,
    AI_AGENT_STATE_CANCELLED,
    AI_AGENT_STATE_OVER_BUDGET,
    AI_AGENT_STATE_INTERRUPTED
} AiAgentState;

/**
 * ai_agent_state_to_string:
 * @state: an #AiAgentState
 *
 * Returns: (transfer none): a stable lowercase name for @state.
 */
const gchar *
ai_agent_state_to_string (AiAgentState state);

/**
 * ai_agent_state_from_string:
 * @name: a state name
 *
 * Returns: the matching #AiAgentState, or %AI_AGENT_STATE_IDLE.
 */
AiAgentState
ai_agent_state_from_string (const gchar *name);

/**
 * ai_agent_state_is_terminal:
 * @state: an #AiAgentState
 *
 * Returns: %TRUE when no further transition will happen on its own.
 */
gboolean
ai_agent_state_is_terminal (AiAgentState state);

/**
 * ai_agent_state_is_live:
 * @state: an #AiAgentState
 *
 * Returns: %TRUE when the agent occupies a concurrency slot.
 */
gboolean
ai_agent_state_is_live (AiAgentState state);

G_END_DECLS
