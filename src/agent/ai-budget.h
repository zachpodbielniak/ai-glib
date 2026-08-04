/*
 * ai-budget.h - Hard spend and effort ceilings for an agent run
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

#define AI_TYPE_BUDGET (ai_budget_get_type())

/**
 * AiBudget:
 *
 * Limits for one agent run, and what it has spent so far.
 *
 * These are ceilings, not guidance.  An agent that reaches one stops;
 * the alternative -- warning and continuing -- means the number is
 * decoration, and the entire reason to track spend is to be able to
 * bound it.
 *
 * Cost is carried in micro-dollars as a #gint64 rather than a #gdouble.
 * It is accumulated across thousands of turns, and floating point drift
 * in the one figure a limit is enforced against would be the worst
 * possible place for it.
 */
typedef struct _AiBudget AiBudget;

GType ai_budget_get_type (void) G_GNUC_CONST;

AiBudget *ai_budget_new  (void);
AiBudget *ai_budget_copy (const AiBudget *self);
void      ai_budget_free (AiBudget *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AiBudget, ai_budget_free)

/* Limits.  Zero means unlimited for that dimension. */
void ai_budget_set_max_input_tokens  (AiBudget *self, guint64 n);
void ai_budget_set_max_output_tokens (AiBudget *self, guint64 n);
void ai_budget_set_max_turns         (AiBudget *self, guint n);
void ai_budget_set_max_cost_micros   (AiBudget *self, gint64 micros);
void ai_budget_set_max_wall_ms       (AiBudget *self, gint64 ms);

guint64 ai_budget_get_max_input_tokens  (const AiBudget *self);
guint64 ai_budget_get_max_output_tokens (const AiBudget *self);
guint   ai_budget_get_max_turns         (const AiBudget *self);
gint64  ai_budget_get_max_cost_micros   (const AiBudget *self);
gint64  ai_budget_get_max_wall_ms       (const AiBudget *self);

/* Consumption. */
void ai_budget_add_usage (AiBudget *self, guint64 in_tokens,
                          guint64 out_tokens, gint64 cost_micros);
void ai_budget_add_turn  (AiBudget *self);
void ai_budget_start     (AiBudget *self);

guint64 ai_budget_get_input_tokens  (const AiBudget *self);
guint64 ai_budget_get_output_tokens (const AiBudget *self);
guint   ai_budget_get_turns         (const AiBudget *self);
gint64  ai_budget_get_cost_micros   (const AiBudget *self);
gint64  ai_budget_get_elapsed_ms    (const AiBudget *self);

/**
 * ai_budget_exceeded:
 * @self: an #AiBudget
 * @out_reason: (out) (optional) (transfer none): which limit was hit
 *
 * Returns: %TRUE when any ceiling has been reached.
 */
gboolean ai_budget_exceeded (const AiBudget *self, const gchar **out_reason);

/**
 * ai_budget_would_exceed:
 * @self: an #AiBudget
 * @out_reason: (out) (optional) (transfer none): which limit would be hit
 *
 * Like ai_budget_exceeded(), but also refuses a turn that would take the
 * run past its turn ceiling.
 *
 * Checked *before* a turn rather than after: discovering the limit
 * afterwards means having already paid for the request that broke it.
 *
 * Returns: %TRUE when the next turn must not be started.
 */
gboolean ai_budget_would_exceed (const AiBudget *self, const gchar **out_reason);

G_END_DECLS
