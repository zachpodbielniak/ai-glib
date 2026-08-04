/*
 * ai-agent-enums.c - Agent lifecycle states
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-agent-enums.h"

#include <string.h>

static const gchar *const state_names[] = {
    "idle", "queued", "starting", "running", "waiting-input",
    "blocked", "done", "failed", "cancelled", "over-budget",
    "interrupted", NULL
};

const gchar *
ai_agent_state_to_string (AiAgentState state)
{
    if (state < 0 || state > AI_AGENT_STATE_INTERRUPTED)
        return "idle";
    return state_names[state];
}

AiAgentState
ai_agent_state_from_string (const gchar *name)
{
    gint i;

    if (name == NULL) return AI_AGENT_STATE_IDLE;
    for (i = 0; state_names[i] != NULL; i++)
        if (strcmp(name, state_names[i]) == 0)
            return (AiAgentState)i;
    return AI_AGENT_STATE_IDLE;
}

gboolean
ai_agent_state_is_terminal (AiAgentState state)
{
    switch (state)
    {
        case AI_AGENT_STATE_DONE:
        case AI_AGENT_STATE_FAILED:
        case AI_AGENT_STATE_CANCELLED:
        case AI_AGENT_STATE_OVER_BUDGET:
            return TRUE;
        default:
            return FALSE;
    }
}

gboolean
ai_agent_state_is_live (AiAgentState state)
{
    switch (state)
    {
        case AI_AGENT_STATE_STARTING:
        case AI_AGENT_STATE_RUNNING:
        case AI_AGENT_STATE_WAITING_INPUT:
        case AI_AGENT_STATE_BLOCKED:
            return TRUE;
        default:
            return FALSE;
    }
}
