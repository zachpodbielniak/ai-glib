/*
 * ai-brigade.c - Orchestrates a set of agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-brigade.h"

struct _AiBrigade
{
    GObject parent_instance;

    GHashTable    *agents;        /* id -> AiAgent* (owns a ref) */

    AiAgentHost   *host;
    AiAgentWorker *worker;
    AiAgentStore  *store;
    AiPriceTable  *prices;
    AiBudget      *budget;

    guint          max_concurrent;
};

G_DEFINE_TYPE (AiBrigade, ai_brigade, G_TYPE_OBJECT)

enum
{
    SIGNAL_AGENT_STATE_CHANGED,
    SIGNAL_AGENT_FINISHED,
    SIGNAL_BUDGET_EXCEEDED,
    SIGNAL_IDLE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
ai_brigade_finalize (GObject *object)
{
    AiBrigade *self = AI_BRIGADE(object);

    g_clear_pointer(&self->agents, g_hash_table_destroy);
    g_clear_pointer(&self->budget, ai_budget_free);
    g_clear_pointer(&self->prices, ai_price_table_free);
    g_clear_object(&self->host);
    g_clear_object(&self->worker);
    g_clear_object(&self->store);

    G_OBJECT_CLASS(ai_brigade_parent_class)->finalize(object);
}

static void
ai_brigade_class_init (AiBrigadeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_brigade_finalize;

    /**
     * AiBrigade::agent-state-changed:
     * @self: the #AiBrigade
     * @agent_id: which agent
     * @new_state: its new #AiAgentState
     */
    signals[SIGNAL_AGENT_STATE_CHANGED] = g_signal_new(
        "agent-state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    /**
     * AiBrigade::agent-finished:
     * @self: the #AiBrigade
     * @agent_id: which agent
     * @state: the terminal #AiAgentState
     */
    signals[SIGNAL_AGENT_FINISHED] = g_signal_new(
        "agent-finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);

    /**
     * AiBrigade::budget-exceeded:
     * @self: the #AiBrigade
     * @reason: which ceiling was hit
     *
     * The brigade-wide budget, not an individual agent's.
     */
    signals[SIGNAL_BUDGET_EXCEEDED] = g_signal_new(
        "budget-exceeded", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * AiBrigade::idle:
     * @self: the #AiBrigade
     *
     * Emitted when the last live agent finishes.
     */
    signals[SIGNAL_IDLE] = g_signal_new(
        "idle", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
ai_brigade_init (AiBrigade *self)
{
    self->agents = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free, g_object_unref);
    self->budget = ai_budget_new();
    self->max_concurrent = 4;
}

AiBrigade *
ai_brigade_new (void)
{
    return g_object_new(AI_TYPE_BRIGADE, NULL);
}

/* ── Collaborators ───────────────────────────────────────────────── */

/* Written out rather than generated: a macro saves a dozen lines and
 * costs g-ir-scanner the ability to see the annotations, which it needs
 * for every one of these to appear correctly in a binding. */

/**
 * ai_brigade_set_host:
 * @self: an #AiBrigade
 * @host: (transfer none) (nullable): what the application provides
 */
void
ai_brigade_set_host (AiBrigade *self, AiAgentHost *host)
{
    g_return_if_fail(AI_IS_BRIGADE(self));
    g_return_if_fail(host == NULL || AI_IS_AGENT_HOST(host));
    g_clear_object(&self->host);
    if (host != NULL) self->host = g_object_ref(host);
}

/**
 * ai_brigade_get_host:
 * @self: an #AiBrigade
 *
 * Returns: (transfer none) (nullable): the host.
 */
AiAgentHost *
ai_brigade_get_host (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    return self->host;
}

/**
 * ai_brigade_set_worker:
 * @self: an #AiBrigade
 * @worker: (transfer none) (nullable): how agents execute
 */
void
ai_brigade_set_worker (AiBrigade *self, AiAgentWorker *worker)
{
    g_return_if_fail(AI_IS_BRIGADE(self));
    g_return_if_fail(worker == NULL || AI_IS_AGENT_WORKER(worker));
    g_clear_object(&self->worker);
    if (worker != NULL) self->worker = g_object_ref(worker);
}

/**
 * ai_brigade_get_worker:
 * @self: an #AiBrigade
 *
 * Returns: (transfer none) (nullable): the worker.
 */
AiAgentWorker *
ai_brigade_get_worker (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    return self->worker;
}

/**
 * ai_brigade_set_store:
 * @self: an #AiBrigade
 * @store: (transfer none) (nullable): where records persist
 */
void
ai_brigade_set_store (AiBrigade *self, AiAgentStore *store)
{
    g_return_if_fail(AI_IS_BRIGADE(self));
    g_return_if_fail(store == NULL || AI_IS_AGENT_STORE(store));
    g_clear_object(&self->store);
    if (store != NULL) self->store = g_object_ref(store);
}

/**
 * ai_brigade_get_store:
 * @self: an #AiBrigade
 *
 * Returns: (transfer none) (nullable): the store.
 */
AiAgentStore *
ai_brigade_get_store (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    return self->store;
}

/**
 * ai_brigade_set_price_table:
 * @self: an #AiBrigade
 * @prices: (transfer none) (nullable): the table
 */
void
ai_brigade_set_price_table (AiBrigade *self, AiPriceTable *prices)
{
    g_return_if_fail(AI_IS_BRIGADE(self));
    g_clear_pointer(&self->prices, ai_price_table_free);
    if (prices != NULL) self->prices = ai_price_table_copy(prices);
}

/**
 * ai_brigade_get_price_table:
 * @self: an #AiBrigade
 *
 * Returns: (transfer none) (nullable): the table.
 */
AiPriceTable *
ai_brigade_get_price_table (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    return self->prices;
}

void
ai_brigade_set_max_concurrent (AiBrigade *self, guint n)
{
    g_return_if_fail(AI_IS_BRIGADE(self));
    self->max_concurrent = n;
}

guint
ai_brigade_get_max_concurrent (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), 0);
    return self->max_concurrent;
}

/**
 * ai_brigade_get_budget:
 * @self: an #AiBrigade
 *
 * Returns: (transfer none): the brigade-wide budget.
 */
AiBudget *
ai_brigade_get_budget (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    return self->budget;
}

/* ── Membership ──────────────────────────────────────────────────── */

static void
on_agent_state_changed (AiAgent *agent, gint old_state, gint new_state,
                        gpointer user_data)
{
    AiBrigade *self = user_data;
    const gchar *reason = NULL;

    (void)old_state;

    g_signal_emit(self, signals[SIGNAL_AGENT_STATE_CHANGED], 0,
                  ai_agent_get_id(agent), new_state);

    if (ai_agent_state_is_terminal((AiAgentState)new_state))
    {
        g_signal_emit(self, signals[SIGNAL_AGENT_FINISHED], 0,
                      ai_agent_get_id(agent), new_state);
        if (ai_brigade_count_live(self) == 0)
            g_signal_emit(self, signals[SIGNAL_IDLE], 0);
    }

    if (ai_budget_exceeded(self->budget, &reason))
        g_signal_emit(self, signals[SIGNAL_BUDGET_EXCEEDED], 0, reason);
}

static void
on_agent_progress (AiAgent *agent, guint turns, guint64 in_tokens,
                   guint64 out_tokens, gpointer user_data)
{
    AiBrigade *self = user_data;
    const gchar *reason = NULL;

    (void)agent; (void)turns; (void)in_tokens; (void)out_tokens;

    /* The brigade budget is charged from each agent's own accounting
     * rather than tracked separately, so the two cannot disagree about
     * what was spent. */
    if (ai_budget_exceeded(self->budget, &reason))
        g_signal_emit(self, signals[SIGNAL_BUDGET_EXCEEDED], 0, reason);
}

gboolean
ai_brigade_add (AiBrigade *self, AiAgent *agent)
{
    const gchar *id;

    g_return_val_if_fail(AI_IS_BRIGADE(self), FALSE);
    g_return_val_if_fail(AI_IS_AGENT(agent), FALSE);

    id = ai_agent_get_id(agent);
    g_return_val_if_fail(id != NULL, FALSE);

    if (g_hash_table_contains(self->agents, id)) return FALSE;

    g_hash_table_insert(self->agents, g_strdup(id), g_object_ref(agent));

    g_signal_connect(agent, "state-changed",
                     G_CALLBACK(on_agent_state_changed), self);
    g_signal_connect(agent, "progress",
                     G_CALLBACK(on_agent_progress), self);
    return TRUE;
}

/**
 * ai_brigade_get:
 * @self: an #AiBrigade
 * @id: the agent id
 *
 * Returns: (transfer none) (nullable): the agent, or %NULL.
 */
AiAgent *
ai_brigade_get (AiBrigade *self, const gchar *id)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    if (id == NULL) return NULL;
    return g_hash_table_lookup(self->agents, id);
}

gboolean
ai_brigade_remove (AiBrigade *self, const gchar *id)
{
    AiAgent *agent;

    g_return_val_if_fail(AI_IS_BRIGADE(self), FALSE);
    if (id == NULL) return FALSE;

    agent = g_hash_table_lookup(self->agents, id);
    if (agent == NULL) return FALSE;

    g_signal_handlers_disconnect_by_data(agent, self);
    return g_hash_table_remove(self->agents, id);
}

/**
 * ai_brigade_list:
 * @self: an #AiBrigade
 *
 * Returns: (transfer container) (element-type AiAgent): every agent.
 */
GList *
ai_brigade_list (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    return g_hash_table_get_values(self->agents);
}

guint
ai_brigade_count_live (AiBrigade *self)
{
    GHashTableIter iter;
    gpointer k, v;
    guint n = 0;

    g_return_val_if_fail(AI_IS_BRIGADE(self), 0);

    g_hash_table_iter_init(&iter, self->agents);
    while (g_hash_table_iter_next(&iter, &k, &v))
        if (ai_agent_state_is_live(ai_agent_get_state(AI_AGENT(v)))) n++;

    return n;
}

gboolean
ai_brigade_can_start (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), FALSE);

    if (ai_budget_exceeded(self->budget, NULL)) return FALSE;
    if (self->max_concurrent == 0) return TRUE;
    return ai_brigade_count_live(self) < self->max_concurrent;
}

guint
ai_brigade_cancel_all (AiBrigade *self)
{
    g_autoptr(GList) agents = NULL;
    GList *l;
    guint n = 0;

    g_return_val_if_fail(AI_IS_BRIGADE(self), 0);

    /* Snapshot before cancelling: ::state-changed handlers can remove
     * agents, and mutating the table mid-iteration is undefined. */
    agents = g_hash_table_get_values(self->agents);
    for (l = agents; l != NULL; l = l->next)
    {
        AiAgent *a = AI_AGENT(l->data);
        if (ai_agent_state_is_live(ai_agent_get_state(a)))
        {
            ai_agent_cancel(a);
            n++;
        }
    }
    return n;
}

guint
ai_brigade_sweep (AiBrigade *self)
{
    g_autoptr(GList) agents = NULL;
    GList *l;
    guint n = 0;

    g_return_val_if_fail(AI_IS_BRIGADE(self), 0);
    if (self->worker == NULL) return 0;
    if (!ai_agent_worker_can_poll(self->worker)) return 0;

    agents = g_hash_table_get_values(self->agents);
    for (l = agents; l != NULL; l = l->next)
    {
        AiAgent *a = AI_AGENT(l->data);
        AiAgentState observed;

        if (!ai_agent_state_is_live(ai_agent_get_state(a))) continue;
        if (!ai_agent_worker_poll_state(self->worker, a, &observed, NULL))
            continue;
        if (observed != ai_agent_get_state(a))
        {
            ai_agent_set_state(a, observed);
            n++;
        }
    }
    return n;
}

guint
ai_brigade_interrupt_live (AiBrigade *self)
{
    g_autoptr(GList) agents = NULL;
    GList *l;
    guint n = 0;

    g_return_val_if_fail(AI_IS_BRIGADE(self), 0);

    agents = g_hash_table_get_values(self->agents);
    for (l = agents; l != NULL; l = l->next)
    {
        AiAgent *a = AI_AGENT(l->data);
        if (ai_agent_state_is_live(ai_agent_get_state(a)))
        {
            ai_agent_set_state(a, AI_AGENT_STATE_INTERRUPTED);
            n++;
        }
    }
    return n;
}
