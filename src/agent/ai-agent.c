/*
 * ai-agent.c - One agent run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-agent.h"

struct _AiAgent
{
    GObject parent_instance;

    gchar          *id;
    AiProvider     *provider;
    AiToolExecutor *executor;
    AiBudget       *budget;
    GCancellable   *cancellable;

    gchar          *system_prompt;
    gchar          *model;
    gchar          *description;
    gint            max_tokens;

    AiAgentState    state;
    gchar          *result;
    GError         *error;

    /* Wall clock for the run.  Frozen on the way into a terminal state:
     * a finished agent should keep reporting what it took, not go on
     * counting for as long as the record is kept. */
    gint64          started_us;
    gint64          finished_us;
};

G_DEFINE_TYPE (AiAgent, ai_agent, G_TYPE_OBJECT)

enum
{
    SIGNAL_STATE_CHANGED,
    SIGNAL_PROGRESS,
    SIGNAL_TOOL_CALLED,
    SIGNAL_FINISHED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
ai_agent_finalize (GObject *object)
{
    AiAgent *self = AI_AGENT(object);

    g_clear_pointer(&self->id, g_free);
    g_clear_pointer(&self->system_prompt, g_free);
    g_clear_pointer(&self->model, g_free);
    g_clear_pointer(&self->description, g_free);
    g_clear_pointer(&self->result, g_free);
    g_clear_pointer(&self->budget, ai_budget_free);
    g_clear_error(&self->error);
    g_clear_object(&self->provider);
    g_clear_object(&self->executor);
    g_clear_object(&self->cancellable);

    G_OBJECT_CLASS(ai_agent_parent_class)->finalize(object);
}

static void
ai_agent_class_init (AiAgentClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_agent_finalize;

    /**
     * AiAgent::state-changed:
     * @self: the #AiAgent
     * @old_state: the previous #AiAgentState
     * @new_state: the current one
     */
    signals[SIGNAL_STATE_CHANGED] = g_signal_new(
        "state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);

    /**
     * AiAgent::progress:
     * @self: the #AiAgent
     * @turns: turns completed
     * @in_tokens: cumulative input tokens
     * @out_tokens: cumulative output tokens
     */
    signals[SIGNAL_PROGRESS] = g_signal_new(
        "progress", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 3,
        G_TYPE_UINT, G_TYPE_UINT64, G_TYPE_UINT64);

    /**
     * AiAgent::tool-called:
     * @self: the #AiAgent
     * @tool_name: the tool
     */
    signals[SIGNAL_TOOL_CALLED] = g_signal_new(
        "tool-called", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * AiAgent::finished:
     * @self: the #AiAgent
     * @state: the terminal #AiAgentState
     *
     * Emitted once, when the run reaches a terminal state.  Carries the
     * state rather than a success flag because "cancelled",
     * "over-budget" and "failed" call for different responses from a
     * caller, and collapsing them would throw that away.
     */
    signals[SIGNAL_FINISHED] = g_signal_new(
        "finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
}

static void
ai_agent_init (AiAgent *self)
{
    self->executor    = ai_tool_executor_new();
    self->budget      = ai_budget_new();
    self->cancellable = g_cancellable_new();
    self->state       = AI_AGENT_STATE_IDLE;
    self->max_tokens  = 4096;
}

AiAgent *
ai_agent_new (const gchar *id, AiProvider *provider)
{
    g_autoptr(AiAgent) self = g_object_new(AI_TYPE_AGENT, NULL);

    self->id = g_strdup(id);
    if (provider != NULL) self->provider = g_object_ref(provider);

    return (AiAgent *)g_steal_pointer(&self);
}

const gchar *
ai_agent_get_id (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->id;
}

/**
 * ai_agent_get_provider:
 * @self: an #AiAgent
 *
 * Returns: (transfer none) (nullable): the provider turns are sent to.
 */
AiProvider *
ai_agent_get_provider (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->provider;
}

/**
 * ai_agent_get_executor:
 * @self: an #AiAgent
 *
 * Each agent owns its own executor, which is what makes an allowlist
 * structural rather than a check.
 *
 * Returns: (transfer none): the executor.
 */
AiToolExecutor *
ai_agent_get_executor (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->executor;
}

void
ai_agent_set_provider (AiAgent *self, AiProvider *provider)
{
    g_return_if_fail(AI_IS_AGENT(self));
    g_return_if_fail(provider == NULL || AI_IS_PROVIDER(provider));

    if (self->provider == provider) return;

    g_clear_object(&self->provider);
    if (provider != NULL) self->provider = g_object_ref(provider);
}

void
ai_agent_set_executor (AiAgent *self, AiToolExecutor *executor)
{
    g_return_if_fail(AI_IS_AGENT(self));
    g_return_if_fail(AI_IS_TOOL_EXECUTOR(executor));

    if (self->executor == executor) return;

    g_clear_object(&self->executor);
    self->executor = g_object_ref(executor);
}

void
ai_agent_set_description (AiAgent *self, const gchar *description)
{
    g_return_if_fail(AI_IS_AGENT(self));
    g_free(self->description);
    self->description = g_strdup(description);
}

/**
 * ai_agent_get_description:
 * @self: an #AiAgent
 *
 * Returns: (transfer none) (nullable): what this agent was asked to do.
 */
const gchar *
ai_agent_get_description (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->description;
}

gint64
ai_agent_get_elapsed_ms (AiAgent *self)
{
    gint64 end;

    g_return_val_if_fail(AI_IS_AGENT(self), 0);

    if (self->started_us == 0) return 0;

    end = self->finished_us != 0 ? self->finished_us : g_get_monotonic_time();
    return (end - self->started_us) / 1000;
}

AiAgentState
ai_agent_get_state (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), AI_AGENT_STATE_IDLE);
    return self->state;
}

/**
 * ai_agent_get_budget:
 * @self: an #AiAgent
 *
 * Returns: (transfer none): this agent's own budget.
 */
AiBudget *
ai_agent_get_budget (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->budget;
}

/**
 * ai_agent_get_cancellable:
 * @self: an #AiAgent
 *
 * Returns: (transfer none): the cancellable for this run.
 */
GCancellable *
ai_agent_get_cancellable (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->cancellable;
}

void
ai_agent_set_system_prompt (AiAgent *self, const gchar *prompt)
{
    g_return_if_fail(AI_IS_AGENT(self));
    g_free(self->system_prompt);
    self->system_prompt = g_strdup(prompt);
}

const gchar *
ai_agent_get_system_prompt (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->system_prompt;
}

void
ai_agent_set_model (AiAgent *self, const gchar *model)
{
    g_return_if_fail(AI_IS_AGENT(self));
    g_free(self->model);
    self->model = g_strdup(model);
}

const gchar *
ai_agent_get_model (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->model;
}

void
ai_agent_set_max_tokens (AiAgent *self, gint max_tokens)
{
    g_return_if_fail(AI_IS_AGENT(self));
    self->max_tokens = max_tokens;
}

gint
ai_agent_get_max_tokens (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), 0);
    return self->max_tokens;
}

void
ai_agent_set_state (AiAgent *self, AiAgentState state)
{
    AiAgentState old;

    g_return_if_fail(AI_IS_AGENT(self));
    if (self->state == state) return;

    old = self->state;
    self->state = state;

    if (state == AI_AGENT_STATE_RUNNING && old != AI_AGENT_STATE_RUNNING)
    {
        ai_budget_start(self->budget);
        self->started_us = g_get_monotonic_time();
    }

    if (ai_agent_state_is_terminal(state) && self->finished_us == 0)
        self->finished_us = g_get_monotonic_time();

    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, (gint)old,
                  (gint)state);

    /* ::finished fires once, on entering a terminal state.  Emitting it
     * from here rather than from each call site is what guarantees the
     * "once" -- there is no path into a terminal state that does not go
     * through this function. */
    if (ai_agent_state_is_terminal(state) && !ai_agent_state_is_terminal(old))
        g_signal_emit(self, signals[SIGNAL_FINISHED], 0, (gint)state);
}

void
ai_agent_record_turn (AiAgent *self, guint64 in_tokens, guint64 out_tokens,
                      gint64 cost_micros)
{
    g_return_if_fail(AI_IS_AGENT(self));

    ai_budget_add_turn(self->budget);
    /* -1 means the model is unpriced.  Adding it would quietly subtract
     * from the total, which is a worse answer than "we do not know". */
    ai_budget_add_usage(self->budget, in_tokens, out_tokens,
                        cost_micros > 0 ? cost_micros : 0);

    g_signal_emit(self, signals[SIGNAL_PROGRESS], 0,
                  ai_budget_get_turns(self->budget),
                  ai_budget_get_input_tokens(self->budget),
                  ai_budget_get_output_tokens(self->budget));
}

void
ai_agent_cancel (AiAgent *self)
{
    g_return_if_fail(AI_IS_AGENT(self));

    g_cancellable_cancel(self->cancellable);
    if (!ai_agent_state_is_terminal(self->state))
        ai_agent_set_state(self, AI_AGENT_STATE_CANCELLED);
}

const gchar *
ai_agent_get_result (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->result;
}

void
ai_agent_set_result (AiAgent *self, const gchar *result)
{
    g_return_if_fail(AI_IS_AGENT(self));
    g_free(self->result);
    self->result = g_strdup(result);
}

const GError *
ai_agent_get_error (AiAgent *self)
{
    g_return_val_if_fail(AI_IS_AGENT(self), NULL);
    return self->error;
}

/**
 * ai_agent_take_error:
 * @self: an #AiAgent
 * @error: (transfer full) (nullable): the failure
 *
 * Records why the run failed, taking ownership of @error.
 */
void
ai_agent_take_error (AiAgent *self, GError *error)
{
    g_return_if_fail(AI_IS_AGENT(self));
    g_clear_error(&self->error);
    self->error = error;
}
