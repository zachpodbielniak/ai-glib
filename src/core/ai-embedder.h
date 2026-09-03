/*
 * ai-embedder.h - Text embedding interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Embedding is a capability rather than a client, for the same reason image
 * generation is: it is not chat, it has no messages, no tools and no
 * streaming, and the providers that do it are not the same set that do the
 * rest. So it hangs off an interface a client implements, and a caller that
 * wants vectors asks for AI_TYPE_EMBEDDER rather than for a named provider.
 *
 * The model table each implementation returns is the registration. A model
 * absent from it is not offered, which is what keeps `dimensions` a fact
 * rather than a guess -- and dimensions matter here more than they look,
 * because a store that mixes two widths cannot compare its own rows.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "model/ai-embedding.h"

G_BEGIN_DECLS

/*
 * AiEmbeddingModelInfo:
 * @id: the model identifier sent on the wire
 * @dimensions: vector length, or 0 when the provider lets the caller choose
 * @max_input_chars: rough ceiling on one input, or 0 when unknown
 * @supports_batch: whether several inputs may share one request
 * @notes: (nullable): anything a caller should know before choosing it
 *
 * What one embedding model can do.
 *
 * @max_input_chars is characters rather than tokens because the tokeniser
 * belongs to the model and callers do not have it. It is deliberately
 * approximate: its job is to stop a caller sending a whole book, not to
 * predict the provider's exact limit.
 */
typedef struct
{
    const gchar *id;
    guint        dimensions;
    guint        max_input_chars;
    gboolean     supports_batch;
    const gchar *notes;
} AiEmbeddingModelInfo;

#define AI_TYPE_EMBEDDER (ai_embedder_get_type())

G_DECLARE_INTERFACE(AiEmbedder, ai_embedder, AI, EMBEDDER, GObject)

/*
 * AiEmbedderInterface:
 * @parent_iface: the parent interface
 * @embed_async: starts an asynchronous embedding
 * @embed_finish: finishes an asynchronous embedding
 * @get_default_embedding_model: the model used when the caller names none
 * @list_embedding_models: the models this provider serves
 * @_reserved: reserved for future expansion
 *
 * Interface for providers that turn text into vectors.
 *
 * Only @embed_async and @embed_finish are required. Implementing
 * @list_embedding_models is what makes a provider participate in dimension
 * checking and in any front-end that offers a choice of model.
 */
struct _AiEmbedderInterface
{
    GTypeInterface parent_iface;

    void          (*embed_async)                 (AiEmbedder           *self,
                                                  const gchar *const   *texts,
                                                  const gchar          *model,
                                                  GCancellable         *cancellable,
                                                  GAsyncReadyCallback   callback,
                                                  gpointer              user_data);
    AiEmbedding * (*embed_finish)                (AiEmbedder           *self,
                                                  GAsyncResult         *result,
                                                  GError              **error);
    const gchar * (*get_default_embedding_model) (AiEmbedder           *self);
    GList *       (*list_embedding_models)       (AiEmbedder           *self);

    /* Reserved for future expansion */
    gpointer _reserved[7];
};

void
ai_embedder_embed_async (
    AiEmbedder          *self,
    const gchar *const  *texts,
    const gchar         *model,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

AiEmbedding *
ai_embedder_embed_finish (
    AiEmbedder    *self,
    GAsyncResult  *result,
    GError       **error
);

AiEmbedding *
ai_embedder_embed (
    AiEmbedder          *self,
    const gchar *const  *texts,
    const gchar         *model,
    GCancellable        *cancellable,
    GError             **error
);

AiEmbedding *
ai_embedder_embed_one (
    AiEmbedder    *self,
    const gchar   *text,
    const gchar   *model,
    GCancellable  *cancellable,
    GError       **error
);

const gchar *
ai_embedder_get_default_embedding_model (AiEmbedder *self);

GList *
ai_embedder_list_embedding_models (AiEmbedder *self);

const AiEmbeddingModelInfo *
ai_embedder_get_model_info (
    AiEmbedder  *self,
    const gchar *model
);

G_END_DECLS
