/*
 * ai-brigade.h - Orchestrates a set of agents
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

#include "agent/ai-agent.h"
#include "agent/ai-agent-host.h"
#include "agent/ai-agent-worker.h"
#include "agent/ai-agent-store.h"
#include "agent/ai-price-table.h"

G_BEGIN_DECLS

#define AI_TYPE_BRIGADE (ai_brigade_get_type())

G_DECLARE_FINAL_TYPE (AiBrigade, ai_brigade, AI, BRIGADE, GObject)

AiBrigade *ai_brigade_new (void);

void ai_brigade_set_host        (AiBrigade *self, AiAgentHost *host);
void ai_brigade_set_worker      (AiBrigade *self, AiAgentWorker *worker);
void ai_brigade_set_store       (AiBrigade *self, AiAgentStore *store);
void ai_brigade_set_price_table (AiBrigade *self, AiPriceTable *prices);

AiAgentHost   *ai_brigade_get_host        (AiBrigade *self);
AiAgentWorker *ai_brigade_get_worker      (AiBrigade *self);
AiAgentStore  *ai_brigade_get_store       (AiBrigade *self);
AiPriceTable  *ai_brigade_get_price_table (AiBrigade *self);

/**
 * ai_brigade_set_max_concurrent:
 * @self: an #AiBrigade
 * @n: how many agents may be live at once, or 0 for no limit
 *
 * Agents beyond the limit stay %AI_AGENT_STATE_QUEUED until a slot frees.
 */
void  ai_brigade_set_max_concurrent (AiBrigade *self, guint n);
guint ai_brigade_get_max_concurrent (AiBrigade *self);

AiBudget *ai_brigade_get_budget (AiBrigade *self);

/**
 * ai_brigade_add:
 * @self: an #AiBrigade
 * @agent: (transfer none): the agent to track
 *
 * Returns: %FALSE if an agent with that id is already present.
 */
gboolean ai_brigade_add (AiBrigade *self, AiAgent *agent);

AiAgent  *ai_brigade_get    (AiBrigade *self, const gchar *id);
gboolean  ai_brigade_remove (AiBrigade *self, const gchar *id);

gboolean  ai_brigade_start          (AiBrigade *self, AiAgent *agent,
                                     const gchar *prompt, GError **error);
gchar    *ai_brigade_take_finished  (AiBrigade *self);
gchar    *ai_brigade_reap           (AiBrigade *self, const gchar *id,
                                     GError **error);
gchar    *ai_brigade_generate_id    (AiBrigade *self, const gchar *prefix);

GList *ai_brigade_list (AiBrigade *self);

guint ai_brigade_count_live (AiBrigade *self);

/**
 * ai_brigade_can_start:
 * @self: an #AiBrigade
 *
 * Returns: %TRUE when a slot is free and the shared budget has room.
 */
gboolean ai_brigade_can_start (AiBrigade *self);

/**
 * ai_brigade_cancel_all:
 * @self: an #AiBrigade
 *
 * Cancels every live agent.  Returns how many were stopped.
 */
guint ai_brigade_cancel_all (AiBrigade *self);

/**
 * ai_brigade_sweep:
 * @self: an #AiBrigade
 *
 * Polls every agent whose worker cannot push its state.
 *
 * A detached worker is reparented to init, so no SIGCHLD reaches this
 * process and nothing announces that it finished; sweeping is the only
 * way such a run is ever noticed to have ended.  Harmless when every
 * worker pushes -- it simply finds nothing.
 *
 * Returns: how many agents changed state.
 */
guint ai_brigade_sweep (AiBrigade *self);

/**
 * ai_brigade_interrupt_live:
 * @self: an #AiBrigade
 *
 * Marks every live agent %AI_AGENT_STATE_INTERRUPTED.
 *
 * For use at startup after restoring from a store: an agent that was
 * running when the process stopped did not fail and did not finish, and
 * saying so is what stops anything resuming work whose outcome nobody
 * observed.
 *
 * Returns: how many were marked.
 */
guint ai_brigade_interrupt_live (AiBrigade *self);

G_END_DECLS
