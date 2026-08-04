/*
 * ai-agent-store.h - Persistence for agent runs
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

G_BEGIN_DECLS

#define AI_TYPE_AGENT_STORE (ai_agent_store_get_type())

G_DECLARE_INTERFACE (AiAgentStore, ai_agent_store, AI, AGENT_STORE, GObject)

typedef struct _AiAgent AiAgent;

/**
 * AiAgentStoreInterface:
 * @parent_iface: the parent interface
 * @save: writes an agent's record
 * @load: reads one back
 * @list: enumerates known ids
 * @remove: deletes a record
 * @watch: begins reporting externally-made changes
 * @unwatch: stops
 * @_reserved: reserved for future expansion
 *
 * Where agent records live between runs, so a restart does not lose
 * track of work that was in flight.
 *
 * @watch exists for stores whose records are changed by something other
 * than this process -- a directory of sentinel files written by detached
 * workers, say.  A store backed by memory or by a database this process
 * owns leaves it %NULL.
 *
 * Records are plain #GVariant dictionaries so the interface does not
 * dictate a schema; a store that already writes JSON job files can adapt
 * rather than migrate.
 */
struct _AiAgentStoreInterface
{
    GTypeInterface parent_iface;

    gboolean  (*save)    (AiAgentStore *self, const gchar *id,
                          GVariant *record, GError **error);
    GVariant *(*load)    (AiAgentStore *self, const gchar *id,
                          GError **error);
    gchar   **(*list)    (AiAgentStore *self, GError **error);
    gboolean  (*remove)  (AiAgentStore *self, const gchar *id,
                          GError **error);

    gboolean  (*watch)   (AiAgentStore *self, GError **error);
    void      (*unwatch) (AiAgentStore *self);

    /*< private >*/
    gpointer _reserved[8];
};

gboolean  ai_agent_store_save   (AiAgentStore *self, const gchar *id,
                                 GVariant *record, GError **error);
GVariant *ai_agent_store_load   (AiAgentStore *self, const gchar *id,
                                 GError **error);
gchar   **ai_agent_store_list   (AiAgentStore *self, GError **error);
gboolean  ai_agent_store_remove (AiAgentStore *self, const gchar *id,
                                 GError **error);
gboolean  ai_agent_store_watch  (AiAgentStore *self, GError **error);
void      ai_agent_store_unwatch(AiAgentStore *self);

G_END_DECLS
