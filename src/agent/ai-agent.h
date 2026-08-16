/*
 * ai-agent.h - One agent run
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

#include "core/ai-provider.h"
#include "convenience/ai-tool-executor.h"
#include "agent/ai-agent-enums.h"
#include "agent/ai-budget.h"

G_BEGIN_DECLS

#define AI_TYPE_AGENT (ai_agent_get_type())

G_DECLARE_FINAL_TYPE (AiAgent, ai_agent, AI, AGENT, GObject)

/**
 * ai_agent_new:
 * @id: a stable identifier, unique within its brigade
 * @provider: (transfer none) (nullable): where turns are sent, or %NULL to
 *   set one later with ai_agent_set_provider()
 *
 * Creates an agent.
 *
 * Each agent owns its own #AiToolExecutor.  That is not tidiness: the
 * executor holds the tool list and per-run state, so agents sharing one
 * would see each other's tools and race on it.  Owning one apiece is
 * also what makes an allowlist structural -- an agent is handed an
 * executor built from what it may call, so calling anything else is not
 * refused, it is unrepresentable.
 *
 * Returns: (transfer full): a new #AiAgent.
 */
AiAgent *ai_agent_new (const gchar *id, AiProvider *provider);

const gchar     *ai_agent_get_id       (AiAgent *self);
AiProvider      *ai_agent_get_provider (AiAgent *self);
AiToolExecutor  *ai_agent_get_executor (AiAgent *self);
AiAgentState     ai_agent_get_state    (AiAgent *self);
AiBudget        *ai_agent_get_budget   (AiAgent *self);
GCancellable    *ai_agent_get_cancellable (AiAgent *self);

/**
 * ai_agent_set_provider:
 * @self: an #AiAgent
 * @provider: (transfer none) (nullable): where turns are sent
 *
 * Replaces the provider.
 *
 * This is what lets one agent run on a model the conversation that
 * spawned it is not using: a turn held with Grok over HTTP can delegate
 * to a Claude Code subprocess, because nothing about an agent is tied to
 * the provider of whoever asked for it.
 */
void ai_agent_set_provider (AiAgent *self, AiProvider *provider);

/**
 * ai_agent_set_executor:
 * @self: an #AiAgent
 * @executor: (transfer none): the executor this agent runs inside
 *
 * Replaces the executor built by ai_agent_new().
 *
 * The allowlist is applied by handing an agent an executor built from
 * what it may call, so a caller that has assembled one -- from an agent
 * file's `tools:` list, say -- installs it here rather than trying to
 * subtract from the default afterwards.
 */
void ai_agent_set_executor (AiAgent *self, AiToolExecutor *executor);

/**
 * ai_agent_set_description:
 * @self: an #AiAgent
 * @description: (nullable): a short phrase naming the work
 *
 * What this agent was asked to do, in a few words.
 *
 * Exists because an id is not an answer to "what is that one doing?".
 * A status listing of `a1 running 0:42` tells nobody anything; `a1
 * running 0:42  audit the search backends` does.
 */
void         ai_agent_set_description (AiAgent *self, const gchar *description);
const gchar *ai_agent_get_description (AiAgent *self);

/**
 * ai_agent_get_elapsed_ms:
 * @self: an #AiAgent
 *
 * How long the run has been going, or how long it took.
 *
 * Measured from the move to %AI_AGENT_STATE_RUNNING and frozen on
 * entering a terminal state, so a finished agent keeps reporting what it
 * cost rather than counting up forever after nobody is watching.
 *
 * Returns: milliseconds, or 0 if the agent never started.
 */
gint64 ai_agent_get_elapsed_ms (AiAgent *self);

void         ai_agent_set_system_prompt (AiAgent *self, const gchar *prompt);
const gchar *ai_agent_get_system_prompt (AiAgent *self);
void         ai_agent_set_model         (AiAgent *self, const gchar *model);
const gchar *ai_agent_get_model         (AiAgent *self);
void         ai_agent_set_max_tokens    (AiAgent *self, gint max_tokens);
gint         ai_agent_get_max_tokens    (AiAgent *self);

/**
 * ai_agent_set_state:
 * @self: an #AiAgent
 * @state: the new state
 *
 * Moves @self to @state and emits #AiAgent::state-changed.  A move to a
 * terminal state also emits #AiAgent::finished.
 */
void ai_agent_set_state (AiAgent *self, AiAgentState state);

/**
 * ai_agent_record_turn:
 * @self: an #AiAgent
 * @in_tokens: tokens sent
 * @out_tokens: tokens received
 * @cost_micros: cost in micro-dollars, or -1 when the model is unpriced
 *
 * Records one turn against the budget and emits #AiAgent::progress.
 */
void ai_agent_record_turn (AiAgent *self, guint64 in_tokens,
                           guint64 out_tokens, gint64 cost_micros);

/**
 * ai_agent_cancel:
 * @self: an #AiAgent
 *
 * Requests that the run stop.  Always allowed: a caller who wants to
 * stop an agent must never be refused.
 */
void ai_agent_cancel (AiAgent *self);

/**
 * ai_agent_get_result:
 * @self: an #AiAgent
 *
 * Returns: (transfer none) (nullable): the final text, once finished.
 */
const gchar *ai_agent_get_result (AiAgent *self);

void ai_agent_set_result (AiAgent *self, const gchar *result);

/**
 * ai_agent_get_error:
 * @self: an #AiAgent
 *
 * Returns: (transfer none) (nullable): why the run failed, if it did.
 */
const GError *ai_agent_get_error (AiAgent *self);

void ai_agent_take_error (AiAgent *self, GError *error);

G_END_DECLS
