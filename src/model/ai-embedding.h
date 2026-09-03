/*
 * ai-embedding.h - A set of vectors, and what produced them
 *
 * Copyright (C) 2025
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

#define AI_TYPE_EMBEDDING (ai_embedding_get_type())

typedef struct _AiEmbedding AiEmbedding;

GType
ai_embedding_get_type(void) G_GNUC_CONST;

/*
 * ai_embedding_new:
 * @model: the model that produced these vectors
 * @dimensions: the length every vector shares
 *
 * Creates an empty result.
 *
 * The model is carried with the vectors rather than left to the caller
 * because vectors from two models cannot be compared: the numbers are in
 * different spaces, and the cosine between them is not a weak match but
 * noise that reads exactly like one. Anything storing a vector needs to
 * store what made it, and the only way to guarantee that is to hand the two
 * over together.
 *
 * Returns: (transfer full): a new #AiEmbedding
 */
AiEmbedding *
ai_embedding_new (
    const gchar *model,
    gsize        dimensions
);

AiEmbedding *
ai_embedding_ref (AiEmbedding *self);

void
ai_embedding_unref (AiEmbedding *self);

/*
 * ai_embedding_add_vector:
 * @self: an #AiEmbedding
 * @vector: (array length=dimensions): the values
 * @dimensions: how many
 * @error: return location for a GError
 *
 * Appends one vector, copying it.
 *
 * Refuses a vector whose length differs from the set's. A ragged result
 * would not fail here; it would fail much later, as a comparison between
 * two passages that happened to arrive in different batches.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_embedding_add_vector (
    AiEmbedding  *self,
    const gfloat *vector,
    gsize         dimensions,
    GError      **error
);

/*
 * ai_embedding_get_n_vectors:
 * @self: an #AiEmbedding
 *
 * Returns: how many vectors it holds
 */
gsize
ai_embedding_get_n_vectors (AiEmbedding *self);

/*
 * ai_embedding_get_dimensions:
 * @self: an #AiEmbedding
 *
 * Returns: the length every vector shares
 */
gsize
ai_embedding_get_dimensions (AiEmbedding *self);

/*
 * ai_embedding_get_model:
 * @self: an #AiEmbedding
 *
 * Returns: (transfer none): the model that produced the vectors
 */
const gchar *
ai_embedding_get_model (AiEmbedding *self);

/*
 * ai_embedding_get_vector:
 * @self: an #AiEmbedding
 * @index: which one
 *
 * Borrows one vector. It is owned by @self and lives as long as it does.
 *
 * Returns: (transfer none) (nullable) (array): the values, or %NULL when
 *   @index is out of range
 */
const gfloat *
ai_embedding_get_vector (
    AiEmbedding *self,
    gsize        index
);

/*
 * ai_embedding_normalize:
 * @vector: (inout) (array length=dimensions): the values
 * @dimensions: how many
 *
 * Scales a vector to unit length, in place.
 *
 * A vector of all zeroes is left alone rather than producing NaNs. That is
 * what an empty passage embeds to, and a NaN propagates silently through
 * every comparison it subsequently touches.
 */
void
ai_embedding_normalize (
    gfloat *vector,
    gsize   dimensions
);

/*
 * ai_embedding_cosine:
 * @a: (array length=dimensions): one vector
 * @b: (array length=dimensions): the other
 * @dimensions: their shared length
 *
 * Cosine similarity, in [-1, 1].
 *
 * Divides by the norms rather than assuming unit vectors, so a vector that
 * came from somewhere else is scored correctly rather than plausibly.
 * Returns 0.0 when either side is all zeroes instead of dividing by it.
 *
 * Returns: the similarity
 */
gdouble
ai_embedding_cosine (
    const gfloat *a,
    const gfloat *b,
    gsize         dimensions
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiEmbedding, ai_embedding_unref)

G_END_DECLS
