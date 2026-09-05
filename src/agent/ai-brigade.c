/*
 * ai-brigade.c - Orchestrates a set of agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-brigade.h"

#include "core/ai-error.h"

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
    gboolean       pumping;

    /* Agents admitted but not yet started, with the prompt each is
     * waiting to be given.  A queued agent has to keep its prompt
     * somewhere, and the agent itself is deliberately not the place: an
     * agent is one run, not a mailbox. */
    GQueue        *queue;          /* borrowed AiAgent*, in arrival order */
    GHashTable    *queued_prompts; /* id -> gchar* */

    /* Terminal agents whose outcome nothing has collected yet.  This is
     * what makes a notification possible at all: something has to
     * remember that an agent finished until whoever cares next looks. */
    GQueue        *unreported;     /* owned gchar* ids, oldest first */

    guint          next_id;
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

    if (self->queue != NULL)
    {
        g_queue_free(self->queue);
        self->queue = NULL;
    }

    if (self->unreported != NULL)
    {
        g_queue_free_full(self->unreported, g_free);
        self->unreported = NULL;
    }

    g_clear_pointer(&self->queued_prompts, g_hash_table_unref);
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
    self->queue = g_queue_new();
    self->queued_prompts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                 g_free, g_free);
    self->unreported = g_queue_new();
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

static void brigade_pump_queue (AiBrigade *self);

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
        /* Remember it finished until somebody collects the news.  A
         * conversation only looks between turns, so without this the
         * fact would be gone by the time anyone could act on it. */
        g_queue_push_tail(self->unreported, g_strdup(ai_agent_get_id(agent)));

        g_signal_emit(self, signals[SIGNAL_AGENT_FINISHED], 0,
                      ai_agent_get_id(agent), new_state);

        /* A slot just freed.  Started before ::idle is considered, so a
         * queued agent taking the slot means the brigade was never idle
         * -- which is the truth. */
        brigade_pump_queue(self);

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

    /* Agents can be retained by callers after the brigade is destroyed. */
    g_signal_connect_object(agent, "state-changed",
                            G_CALLBACK(on_agent_state_changed), self, 0);
    g_signal_connect_object(agent, "progress",
                            G_CALLBACK(on_agent_progress), self, 0);
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
    GList   *link;

    g_return_val_if_fail(AI_IS_BRIGADE(self), FALSE);
    if (id == NULL) return FALSE;

    agent = g_hash_table_lookup(self->agents, id);
    if (agent == NULL) return FALSE;

    g_signal_handlers_disconnect_by_data(agent, self);

    /* Every other structure keyed by this agent goes with it.  A queue
     * entry left behind would be dispatched against an agent nothing
     * tracks any more, and an unreported id would announce the finish of
     * something a caller can no longer look up. */
    g_queue_remove_all(self->queue, agent);
    g_hash_table_remove(self->queued_prompts, id);

    while ((link = g_queue_find_custom(self->unreported, id,
                                       (GCompareFunc)g_strcmp0)) != NULL)
    {
        g_free(link->data);
        g_queue_delete_link(self->unreported, link);
    }

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

/* ── Dispatch ────────────────────────────────────────────────────── */

/*
 * Reap the GTask the worker completes.
 *
 * There is nothing to report here: the worker has already moved the
 * agent to a terminal state, which is where an interested caller reads
 * the outcome from.  Not calling start_finish() would leak the task, and
 * a failure logged as anything above g_debug would abort a test run for
 * something that is ordinary operation.
 */
static void
on_worker_started (GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;

    (void)user_data;

    if (!ai_agent_worker_start_finish(AI_AGENT_WORKER(source), result, &error))
        g_debug("brigade: agent run ended: %s",
                error != NULL ? error->message : "no reason given");
}

static void
brigade_dispatch (AiBrigade *self, AiAgent *agent, const gchar *prompt)
{
    ai_agent_set_state(agent, AI_AGENT_STATE_STARTING);

    ai_agent_worker_start_async(self->worker, agent, prompt, NULL,
                                on_worker_started, self);
}

/*
 * Start whatever the concurrency limit now has room for.
 *
 * Guarded against re-entry because dispatching changes an agent's state,
 * which lands back in on_agent_state_changed(), which pumps.  An agent
 * that fails the instant it starts -- one with no provider, say -- makes
 * that a same-stack recursion rather than a theoretical one.
 */
static void
brigade_pump_queue (AiBrigade *self)
{
    if (self->pumping) return;
    self->pumping = TRUE;

    while (!g_queue_is_empty(self->queue) && ai_brigade_can_start(self))
    {
        AiAgent          *agent = g_queue_pop_head(self->queue);
        const gchar      *id    = ai_agent_get_id(agent);
        g_autofree gchar *prompt = NULL;

        prompt = g_strdup(g_hash_table_lookup(self->queued_prompts, id));
        g_hash_table_remove(self->queued_prompts, id);

        /*
         * A queue entry is an intention, not a commitment. An agent
         * cancelled while it waited has already left
         * %AI_AGENT_STATE_QUEUED, and starting it now would restart work
         * somebody explicitly stopped -- from their side, a kill that
         * did nothing but delay.
         */
        if (ai_agent_get_state(agent) != AI_AGENT_STATE_QUEUED)
            continue;

        brigade_dispatch(self, agent, prompt);
    }

    self->pumping = FALSE;
}

/**
 * ai_brigade_start:
 * @self: an #AiBrigade
 * @agent: (transfer none): the agent to run
 * @prompt: what to ask it
 * @error: (out) (optional): return location for a #GError
 *
 * Admits @agent and starts it, or queues it behind the concurrency
 * limit.
 *
 * Returns as soon as the agent has been admitted --- it does not wait for
 * the run.  That is what "background" means here: the outcome arrives
 * later through #AiBrigade::agent-finished, or is found by asking, and
 * the caller carries on in the meantime.
 *
 * A queued agent is %AI_AGENT_STATE_QUEUED and is dispatched, in arrival
 * order, when a slot frees.  Nothing is dropped for want of a slot.
 *
 * Returns: %FALSE if there is no worker, or the id collides with a
 *   different agent already present.
 */
gboolean
ai_brigade_start (AiBrigade *self, AiAgent *agent, const gchar *prompt,
                  GError **error)
{
    const gchar *id;
    AiAgent     *existing;

    g_return_val_if_fail(AI_IS_BRIGADE(self), FALSE);
    g_return_val_if_fail(AI_IS_AGENT(agent), FALSE);

    if (self->worker == NULL)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                            "this brigade has no worker, so it cannot run "
                            "anything");
        return FALSE;
    }

    id = ai_agent_get_id(agent);
    existing = id != NULL ? g_hash_table_lookup(self->agents, id) : NULL;

    if (existing != NULL && existing != agent)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "an agent named '%s' is already registered", id);
        return FALSE;
    }

    if (existing == NULL && !ai_brigade_add(self, agent))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "could not register agent '%s'", id != NULL ? id : "(null)");
        return FALSE;
    }

    if (ai_brigade_can_start(self))
    {
        brigade_dispatch(self, agent, prompt);
        return TRUE;
    }

    ai_agent_set_state(agent, AI_AGENT_STATE_QUEUED);
    g_hash_table_insert(self->queued_prompts, g_strdup(id), g_strdup(prompt));
    g_queue_push_tail(self->queue, agent);

    return TRUE;
}

/**
 * ai_brigade_take_finished:
 * @self: an #AiBrigade
 *
 * Collects the id of one agent that finished since this was last called.
 *
 * The brigade remembers every terminal transition until somebody asks,
 * because the thing that most wants to know --- a conversation, between
 * turns --- is not looking when it happens.  Ids come back oldest first
 * and each is reported exactly once, so a caller draining this in a loop
 * sees every finish in order and none twice.
 *
 * The agent itself stays registered; this reports the news, not the
 * result. Use ai_brigade_reap() to collect that and drop the record.
 *
 * Returns: (transfer full) (nullable): an agent id, or %NULL when there
 *   is nothing new.
 */
gchar *
ai_brigade_take_finished (AiBrigade *self)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);
    return g_queue_pop_head(self->unreported);
}

/**
 * ai_brigade_reap:
 * @self: an #AiBrigade
 * @id: the agent id
 * @error: (out) (optional): return location for a #GError
 *
 * Collects a finished agent's output and forgets the agent.
 *
 * Refuses an agent that is still live rather than waiting: a caller that
 * blocks here has stopped being a caller and become the run.  Check
 * ai_agent_get_state() first, or wait for #AiBrigade::agent-finished.
 *
 * A run that failed still reaps --- its error message is the output ---
 * because "what happened to it" is exactly what the caller asked for.
 *
 * Returns: (transfer full) (nullable): the final text, or %NULL on error.
 */
gchar *
ai_brigade_reap (AiBrigade *self, const gchar *id, GError **error)
{
    AiAgent      *agent;
    const gchar  *result;
    gchar        *out;

    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);

    agent = ai_brigade_get(self, id);

    if (agent == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "no agent named '%s'", id != NULL ? id : "(null)");
        return NULL;
    }

    if (ai_agent_state_is_live(ai_agent_get_state(agent)))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "agent '%s' is still %s", id,
                    ai_agent_state_to_string(ai_agent_get_state(agent)));
        return NULL;
    }

    result = ai_agent_get_result(agent);

    if (result != NULL)
    {
        out = g_strdup(result);
    }
    else if (ai_agent_get_error(agent) != NULL)
    {
        out = g_strdup_printf("agent '%s' %s: %s", id,
                              ai_agent_state_to_string(ai_agent_get_state(agent)),
                              ai_agent_get_error(agent)->message);
    }
    else
    {
        out = g_strdup_printf("agent '%s' %s and produced no output", id,
                              ai_agent_state_to_string(ai_agent_get_state(agent)));
    }

    ai_brigade_remove(self, id);

    return out;
}

/**
 * ai_brigade_generate_id:
 * @self: an #AiBrigade
 * @prefix: (nullable): a stem for the name, or %NULL for "agent"
 *
 * Mints an id nothing in this brigade is using.
 *
 * Ids are short and readable (`agent-1`, `reviewer-2`) because they are
 * typed back at a tool and read in a status listing; a UUID would be
 * correct and useless.
 *
 * Returns: (transfer full): a fresh id.
 */
gchar *
ai_brigade_generate_id (AiBrigade *self, const gchar *prefix)
{
    g_return_val_if_fail(AI_IS_BRIGADE(self), NULL);

    if (prefix == NULL || prefix[0] == '\0') prefix = "agent";

    for (;;)
    {
        g_autofree gchar *candidate = NULL;

        self->next_id++;
        candidate = g_strdup_printf("%s-%u", prefix, self->next_id);

        if (!g_hash_table_contains(self->agents, candidate))
            return g_steal_pointer(&candidate);
    }
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
