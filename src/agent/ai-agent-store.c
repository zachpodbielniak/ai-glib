/*
 * ai-agent-store.c - Persistence for agent runs
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-agent-store.h"

enum
{
    SIGNAL_RECORD_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_INTERFACE (AiAgentStore, ai_agent_store, G_TYPE_OBJECT)

static void
ai_agent_store_default_init (AiAgentStoreInterface *iface)
{
    /**
     * AiAgentStore::record-changed:
     * @self: the #AiAgentStore
     * @id: the record that changed
     *
     * Emitted when a record is changed by something other than this
     * process, for stores that implement watching.
     *
     * This is how a detached worker reports completion.  Such a process
     * is reparented to init, so nothing signals the spawner; noticing
     * that its record on disk changed is the only notification available.
     */
    signals[SIGNAL_RECORD_CHANGED] = g_signal_new(
        "record-changed",
        G_TYPE_FROM_INTERFACE(iface),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_STRING);
}

/**
 * ai_agent_store_save:
 * @self: an #AiAgentStore
 * @id: the agent id
 * @record: (transfer none): the record, a #GVariant dictionary
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE on success.
 */
gboolean
ai_agent_store_save (AiAgentStore *self, const gchar *id,
                     GVariant *record, GError **error)
{
    AiAgentStoreInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_STORE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    iface = AI_AGENT_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->save != NULL, FALSE);

    return iface->save(self, id, record, error);
}

/**
 * ai_agent_store_load:
 * @self: an #AiAgentStore
 * @id: the agent id
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the record, or %NULL.
 */
GVariant *
ai_agent_store_load (AiAgentStore *self, const gchar *id, GError **error)
{
    AiAgentStoreInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_STORE(self), NULL);

    iface = AI_AGENT_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->load != NULL, NULL);

    return iface->load(self, id, error);
}

/**
 * ai_agent_store_list:
 * @self: an #AiAgentStore
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the ids.
 */
gchar **
ai_agent_store_list (AiAgentStore *self, GError **error)
{
    AiAgentStoreInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_STORE(self), NULL);

    iface = AI_AGENT_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->list != NULL, NULL);

    return iface->list(self, error);
}

/**
 * ai_agent_store_remove:
 * @self: an #AiAgentStore
 * @id: the agent id
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: %TRUE if a record was removed.
 */
gboolean
ai_agent_store_remove (AiAgentStore *self, const gchar *id, GError **error)
{
    AiAgentStoreInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_STORE(self), FALSE);

    iface = AI_AGENT_STORE_GET_IFACE(self);
    g_return_val_if_fail(iface->remove != NULL, FALSE);

    return iface->remove(self, id, error);
}

/**
 * ai_agent_store_watch:
 * @self: an #AiAgentStore
 * @error: (out) (optional): return location for a #GError
 *
 * Begins emitting #AiAgentStore::record-changed.
 *
 * Returns: %FALSE when @self does not support watching, which is normal
 *   for a store this process is the only writer of.
 */
gboolean
ai_agent_store_watch (AiAgentStore *self, GError **error)
{
    AiAgentStoreInterface *iface;

    g_return_val_if_fail(AI_IS_AGENT_STORE(self), FALSE);

    iface = AI_AGENT_STORE_GET_IFACE(self);
    if (iface->watch == NULL) return FALSE;

    return iface->watch(self, error);
}

/**
 * ai_agent_store_unwatch:
 * @self: an #AiAgentStore
 *
 * Stops watching.
 */
void
ai_agent_store_unwatch (AiAgentStore *self)
{
    AiAgentStoreInterface *iface;

    g_return_if_fail(AI_IS_AGENT_STORE(self));

    iface = AI_AGENT_STORE_GET_IFACE(self);
    if (iface->unwatch != NULL) iface->unwatch(self);
}
