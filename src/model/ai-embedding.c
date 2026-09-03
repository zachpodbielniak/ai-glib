/*
 * ai-embedding.c - A set of vectors, and what produced them
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-embedding.h"
#include "core/ai-error.h"

#include <math.h>
#include <string.h>

struct _AiEmbedding
{
    gatomicrefcount  ref_count;

    gchar           *model;
    gsize            dimensions;

    /* One gfloat array per vector, each @dimensions long. */
    GPtrArray       *vectors;
};

G_DEFINE_BOXED_TYPE(AiEmbedding, ai_embedding,
                    ai_embedding_ref, ai_embedding_unref)

AiEmbedding *
ai_embedding_new (
    const gchar *model,
    gsize        dimensions
){
    AiEmbedding *self;

    self = g_new0(AiEmbedding, 1);
    g_atomic_ref_count_init(&self->ref_count);

    self->model = g_strdup(model);
    self->dimensions = dimensions;
    self->vectors = g_ptr_array_new_with_free_func(g_free);

    return self;
}

AiEmbedding *
ai_embedding_ref (AiEmbedding *self)
{
    g_return_val_if_fail(NULL != self, NULL);

    g_atomic_ref_count_inc(&self->ref_count);

    return self;
}

void
ai_embedding_unref (AiEmbedding *self)
{
    g_return_if_fail(NULL != self);

    if (!g_atomic_ref_count_dec(&self->ref_count))
        return;

    g_clear_pointer(&self->model, g_free);
    g_clear_pointer(&self->vectors, g_ptr_array_unref);
    g_free(self);
}

gboolean
ai_embedding_add_vector (
    AiEmbedding  *self,
    const gfloat *vector,
    gsize         dimensions,
    GError      **error
){
    gfloat *copy;

    g_return_val_if_fail(NULL != self, FALSE);
    g_return_val_if_fail(NULL != vector, FALSE);

    if (0 == dimensions)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                            "An embedding of no dimensions is not a vector");
        return FALSE;
    }

    /*
     * The first vector settles the width when the set was created without
     * one, which is what lets a caller build a result before it knows what
     * the provider will return.
     */
    if (0 == self->dimensions)
        self->dimensions = dimensions;

    if (dimensions != self->dimensions)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "This embedding is %" G_GSIZE_FORMAT " dimensions and "
                    "the vector offered is %" G_GSIZE_FORMAT,
                    self->dimensions, dimensions);
        return FALSE;
    }

    copy = g_new0(gfloat, dimensions);
    memcpy(copy, vector, dimensions * sizeof(gfloat));
    g_ptr_array_add(self->vectors, copy);

    return TRUE;
}

gsize
ai_embedding_get_n_vectors (AiEmbedding *self)
{
    g_return_val_if_fail(NULL != self, 0);

    return self->vectors->len;
}

gsize
ai_embedding_get_dimensions (AiEmbedding *self)
{
    g_return_val_if_fail(NULL != self, 0);

    return self->dimensions;
}

const gchar *
ai_embedding_get_model (AiEmbedding *self)
{
    g_return_val_if_fail(NULL != self, NULL);

    return self->model;
}

const gfloat *
ai_embedding_get_vector (
    AiEmbedding *self,
    gsize        index
){
    g_return_val_if_fail(NULL != self, NULL);

    if (index >= self->vectors->len)
        return NULL;

    return g_ptr_array_index(self->vectors, index);
}

void
ai_embedding_normalize (
    gfloat *vector,
    gsize   dimensions
){
    gdouble sum = 0.0;
    gdouble norm;
    gsize i;

    g_return_if_fail(NULL != vector);

    for (i = 0; i < dimensions; i++)
        sum += (gdouble)vector[i] * (gdouble)vector[i];

    if (sum <= 0.0)
        return;

    norm = sqrt(sum);

    for (i = 0; i < dimensions; i++)
        vector[i] = (gfloat)((gdouble)vector[i] / norm);
}

gdouble
ai_embedding_cosine (
    const gfloat *a,
    const gfloat *b,
    gsize         dimensions
){
    gdouble dot = 0.0;
    gdouble norm_a = 0.0;
    gdouble norm_b = 0.0;
    gsize i;

    g_return_val_if_fail(NULL != a, 0.0);
    g_return_val_if_fail(NULL != b, 0.0);

    for (i = 0; i < dimensions; i++)
    {
        dot += (gdouble)a[i] * (gdouble)b[i];
        norm_a += (gdouble)a[i] * (gdouble)a[i];
        norm_b += (gdouble)b[i] * (gdouble)b[i];
    }

    if ((norm_a <= 0.0) || (norm_b <= 0.0))
        return 0.0;

    return dot / (sqrt(norm_a) * sqrt(norm_b));
}
