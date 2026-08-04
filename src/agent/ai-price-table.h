/*
 * ai-price-table.h - Model pricing, in integer micro-dollars
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

#define AI_TYPE_PRICE_TABLE (ai_price_table_get_type())

/**
 * AiPriceTable:
 *
 * Maps a model name to its input and output price, and computes the cost
 * of a turn.
 *
 * Prices are quoted in US dollars per million tokens, because that is how
 * every provider publishes them; computed costs come back in
 * micro-dollars as a #gint64, because they are summed across thousands
 * of turns and floating point would drift in exactly the figure a budget
 * is enforced against.
 *
 * A model with no entry is reported as *unpriced*, never as costing
 * nothing.  The two are indistinguishable once summed, and treating an
 * unknown model as free silently understates spend -- which is worse
 * than admitting the number is unavailable.
 */
typedef struct _AiPriceTable AiPriceTable;

GType ai_price_table_get_type (void) G_GNUC_CONST;

AiPriceTable *ai_price_table_new  (void);
AiPriceTable *ai_price_table_copy (const AiPriceTable *self);
void          ai_price_table_free (AiPriceTable *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AiPriceTable, ai_price_table_free)

void ai_price_table_set (AiPriceTable *self, const gchar *model,
                         gdouble input_per_mtok, gdouble output_per_mtok);

/**
 * ai_price_table_lookup:
 * @self: an #AiPriceTable
 * @model: model name; matched case-insensitively
 * @out_input: (out) (optional): dollars per million input tokens
 * @out_output: (out) (optional): dollars per million output tokens
 *
 * Returns: %TRUE when @model is priced.
 */
gboolean ai_price_table_lookup (const AiPriceTable *self, const gchar *model,
                                gdouble *out_input, gdouble *out_output);

gboolean ai_price_table_is_priced (const AiPriceTable *self,
                                   const gchar *model);

/**
 * ai_price_table_cost_micros:
 * @self: an #AiPriceTable
 * @model: model name
 * @input_tokens: tokens sent
 * @output_tokens: tokens received
 *
 * Returns: the cost in micro-dollars, or -1 when @model is unpriced.
 *   The sentinel is deliberate: a caller that treats it as zero has
 *   chosen to, rather than been quietly given a wrong total.
 */
gint64 ai_price_table_cost_micros (const AiPriceTable *self,
                                   const gchar *model,
                                   guint64 input_tokens,
                                   guint64 output_tokens);

G_END_DECLS
