/*
 * ai-agent-isolation.h - Where an agent is allowed to make a mess
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

#define AI_TYPE_AGENT_ISOLATION (ai_agent_isolation_get_type())

G_DECLARE_INTERFACE (AiAgentIsolation, ai_agent_isolation,
                     AI, AGENT_ISOLATION, GObject)

typedef struct _AiAgent AiAgent;

/**
 * AiAgentIsolationInterface:
 * @parent_iface: the parent interface
 * @prepare: creates the sandbox and reports where the agent should run
 * @teardown: destroys it
 * @describe: a short human-readable label
 * @_reserved: reserved for future expansion
 *
 * A sandbox backend: nothing, a scratch checkout, a container.
 *
 * @teardown must tolerate being called twice, and being called on a
 * @prepare that failed halfway.  It runs from an unwind path, which is
 * exactly when the state is least predictable, and an isolation backend
 * that only cleans up after a tidy success is one that leaks after every
 * interesting failure.
 */
struct _AiAgentIsolationInterface
{
    GTypeInterface parent_iface;

    gboolean (*prepare)  (AiAgentIsolation  *self,
                          AiAgent           *agent,
                          gchar            **out_cwd,
                          GHashTable       **out_env,
                          GError           **error);
    void     (*teardown) (AiAgentIsolation  *self,
                          AiAgent           *agent);
    gchar *  (*describe) (AiAgentIsolation  *self);

    /*< private >*/
    gpointer _reserved[8];
};

gboolean ai_agent_isolation_prepare  (AiAgentIsolation *self, AiAgent *agent,
                                      gchar **out_cwd, GHashTable **out_env,
                                      GError **error);
void     ai_agent_isolation_teardown (AiAgentIsolation *self, AiAgent *agent);
gchar   *ai_agent_isolation_describe (AiAgentIsolation *self);

G_END_DECLS
