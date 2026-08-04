/*
 * ai-budget.c - Hard spend and effort ceilings for an agent run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-budget.h"

struct _AiBudget
{
    guint64 max_input_tokens;
    guint64 max_output_tokens;
    guint   max_turns;
    gint64  max_cost_micros;
    gint64  max_wall_ms;

    guint64 input_tokens;
    guint64 output_tokens;
    guint   turns;
    gint64  cost_micros;
    gint64  started_us;
};

G_DEFINE_BOXED_TYPE (AiBudget, ai_budget, ai_budget_copy, ai_budget_free)

AiBudget *
ai_budget_new (void)
{
    return g_new0(AiBudget, 1);
}

AiBudget *
ai_budget_copy (const AiBudget *self)
{
    AiBudget *copy;

    g_return_val_if_fail(self != NULL, NULL);
    copy = g_new0(AiBudget, 1);
    *copy = *self;
    return copy;
}

void
ai_budget_free (AiBudget *self)
{
    g_free(self);
}

#define SETTER(field, type)                                     \
    void ai_budget_set_##field (AiBudget *self, type v)         \
    { g_return_if_fail(self != NULL); self->field = v; }
#define GETTER(field, type)                                     \
    type ai_budget_get_##field (const AiBudget *self)           \
    { g_return_val_if_fail(self != NULL, 0); return self->field; }

SETTER(max_input_tokens,  guint64)
SETTER(max_output_tokens, guint64)
SETTER(max_turns,         guint)
SETTER(max_cost_micros,   gint64)
SETTER(max_wall_ms,       gint64)

GETTER(max_input_tokens,  guint64)
GETTER(max_output_tokens, guint64)
GETTER(max_turns,         guint)
GETTER(max_cost_micros,   gint64)
GETTER(max_wall_ms,       gint64)

GETTER(input_tokens,  guint64)
GETTER(output_tokens, guint64)
GETTER(turns,         guint)
GETTER(cost_micros,   gint64)

#undef SETTER
#undef GETTER

void
ai_budget_start (AiBudget *self)
{
    g_return_if_fail(self != NULL);
    self->started_us = g_get_monotonic_time();
}

void
ai_budget_add_usage (AiBudget *self, guint64 in_tokens,
                     guint64 out_tokens, gint64 cost_micros)
{
    g_return_if_fail(self != NULL);
    self->input_tokens  += in_tokens;
    self->output_tokens += out_tokens;
    self->cost_micros   += cost_micros;
}

void
ai_budget_add_turn (AiBudget *self)
{
    g_return_if_fail(self != NULL);
    self->turns++;
}

gint64
ai_budget_get_elapsed_ms (const AiBudget *self)
{
    g_return_val_if_fail(self != NULL, 0);
    if (self->started_us == 0) return 0;
    return (g_get_monotonic_time() - self->started_us) / 1000;
}

/* Zero means unlimited for that dimension, which is why every test is
 * guarded rather than compared against a sentinel. */
gboolean
ai_budget_exceeded (const AiBudget *self, const gchar **out_reason)
{
    g_return_val_if_fail(self != NULL, FALSE);

    if (self->max_input_tokens > 0
        && self->input_tokens >= self->max_input_tokens)
    { if (out_reason) *out_reason = "input token limit"; return TRUE; }

    if (self->max_output_tokens > 0
        && self->output_tokens >= self->max_output_tokens)
    { if (out_reason) *out_reason = "output token limit"; return TRUE; }

    if (self->max_cost_micros > 0
        && self->cost_micros >= self->max_cost_micros)
    { if (out_reason) *out_reason = "cost limit"; return TRUE; }

    if (self->max_turns > 0 && self->turns >= self->max_turns)
    { if (out_reason) *out_reason = "turn limit"; return TRUE; }

    if (self->max_wall_ms > 0
        && ai_budget_get_elapsed_ms(self) >= self->max_wall_ms)
    { if (out_reason) *out_reason = "wall clock limit"; return TRUE; }

    return FALSE;
}

gboolean
ai_budget_would_exceed (const AiBudget *self, const gchar **out_reason)
{
    return ai_budget_exceeded(self, out_reason);
}
