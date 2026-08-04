/*
 * ai-price-table.c - Model pricing, in integer micro-dollars
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-price-table.h"

typedef struct
{
    gdouble input_per_mtok;
    gdouble output_per_mtok;
} PriceEntry;

struct _AiPriceTable
{
    GHashTable *entries;   /* lowercased model -> PriceEntry* */
};

G_DEFINE_BOXED_TYPE (AiPriceTable, ai_price_table,
                     ai_price_table_copy, ai_price_table_free)

AiPriceTable *
ai_price_table_new (void)
{
    AiPriceTable *self = g_new0(AiPriceTable, 1);

    self->entries = g_hash_table_new_full(g_str_hash, g_str_equal,
                                          g_free, g_free);
    return self;
}

AiPriceTable *
ai_price_table_copy (const AiPriceTable *self)
{
    AiPriceTable *copy;
    GHashTableIter iter;
    gpointer k, v;

    g_return_val_if_fail(self != NULL, NULL);
    copy = ai_price_table_new();
    g_hash_table_iter_init(&iter, self->entries);
    while (g_hash_table_iter_next(&iter, &k, &v))
    {
        PriceEntry *e = g_new0(PriceEntry, 1);
        *e = *(PriceEntry *)v;
        g_hash_table_insert(copy->entries, g_strdup((const gchar *)k), e);
    }
    return copy;
}

void
ai_price_table_free (AiPriceTable *self)
{
    if (self == NULL) return;
    g_hash_table_destroy(self->entries);
    g_free(self);
}

void
ai_price_table_set (AiPriceTable *self, const gchar *model,
                    gdouble input_per_mtok, gdouble output_per_mtok)
{
    PriceEntry *e;

    g_return_if_fail(self != NULL);
    g_return_if_fail(model != NULL);

    e = g_new0(PriceEntry, 1);
    e->input_per_mtok  = input_per_mtok;
    e->output_per_mtok = output_per_mtok;
    /* Lowercased: providers are inconsistent about case in model ids and
     * a lookup that misses because of it reports the model as unpriced,
     * which looks like a missing table entry rather than a typo. */
    g_hash_table_replace(self->entries, g_ascii_strdown(model, -1), e);
}

gboolean
ai_price_table_lookup (const AiPriceTable *self, const gchar *model,
                       gdouble *out_input, gdouble *out_output)
{
    g_autofree gchar *key = NULL;
    PriceEntry *e;

    g_return_val_if_fail(self != NULL, FALSE);
    if (model == NULL) return FALSE;

    key = g_ascii_strdown(model, -1);
    e = g_hash_table_lookup(self->entries, key);
    if (e == NULL) return FALSE;

    if (out_input)  *out_input  = e->input_per_mtok;
    if (out_output) *out_output = e->output_per_mtok;
    return TRUE;
}

gboolean
ai_price_table_is_priced (const AiPriceTable *self, const gchar *model)
{
    return ai_price_table_lookup(self, model, NULL, NULL);
}

gint64
ai_price_table_cost_micros (const AiPriceTable *self, const gchar *model,
                            guint64 input_tokens, guint64 output_tokens)
{
    gdouble in_rate = 0.0, out_rate = 0.0;
    gdouble dollars;

    if (!ai_price_table_lookup(self, model, &in_rate, &out_rate))
        return -1;

    dollars = ((gdouble)input_tokens  / 1000000.0) * in_rate
            + ((gdouble)output_tokens / 1000000.0) * out_rate;

    /* Rounded once, at the boundary between the floating-point world of
     * published rates and the integer world everything downstream sums
     * in. */
    return (gint64)(dollars * 1000000.0 + 0.5);
}
