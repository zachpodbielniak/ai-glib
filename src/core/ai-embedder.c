/*
 * ai-embedder.c - Text embedding interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-embedder.h"
#include "core/ai-error.h"

G_DEFINE_INTERFACE(AiEmbedder, ai_embedder, G_TYPE_OBJECT)

static void
ai_embedder_default_init(AiEmbedderInterface *iface)
{
    (void)iface;
}

/**
 * ai_embedder_embed_async:
 * @self: an #AiEmbedder
 * @texts: (array zero-terminated=1): the passages to embed
 * @model: (nullable): the model, or %NULL for the provider's default
 * @cancellable: (nullable): a #GCancellable
 * @callback: called on completion
 * @user_data: user data for @callback
 *
 * Starts embedding one or more passages.
 *
 * A provider that does not implement the interface reports the refusal
 * through @callback rather than returning silently, so a caller has one
 * place to handle every outcome.
 */
void
ai_embedder_embed_async (
    AiEmbedder          *self,
    const gchar *const  *texts,
    const gchar         *model,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiEmbedderInterface *iface;

    g_return_if_fail(AI_IS_EMBEDDER(self));
    g_return_if_fail(NULL != texts);

    iface = AI_EMBEDDER_GET_IFACE(self);

    if (NULL == iface->embed_async)
    {
        g_task_report_new_error(self, callback, user_data,
                                ai_embedder_embed_async,
                                AI_ERROR, AI_ERROR_NOT_SUPPORTED,
                                "%s does not produce embeddings",
                                G_OBJECT_TYPE_NAME(self));
        return;
    }

    iface->embed_async(self, texts, model, cancellable, callback, user_data);
}

/**
 * ai_embedder_embed_finish:
 * @self: an #AiEmbedder
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Finishes an ai_embedder_embed_async() call.
 *
 * Returns: (transfer full) (nullable): the vectors, or %NULL on error
 */
AiEmbedding *
ai_embedder_embed_finish (
    AiEmbedder    *self,
    GAsyncResult  *result,
    GError       **error
){
    AiEmbedderInterface *iface;

    g_return_val_if_fail(AI_IS_EMBEDDER(self), NULL);
    g_return_val_if_fail(G_IS_ASYNC_RESULT(result), NULL);

    /*
     * A task reported by embed_async() above never reached the provider,
     * so its own finish would not recognise it. Answering it here is what
     * lets a caller use one finish for both paths.
     */
    if (g_async_result_is_tagged(result, ai_embedder_embed_async))
        return g_task_propagate_pointer(G_TASK(result), error);

    iface = AI_EMBEDDER_GET_IFACE(self);

    if (NULL == iface->embed_finish)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
                    "%s does not produce embeddings",
                    G_OBJECT_TYPE_NAME(self));
        return NULL;
    }

    return iface->embed_finish(self, result, error);
}

typedef struct
{
    GMainLoop   *loop;
    AiEmbedding *embedding;
    GError      *error;
} AiEmbedderSyncData;

static void
ai_embedder_on_sync_done (
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    AiEmbedderSyncData *data = user_data;

    data->embedding = ai_embedder_embed_finish(AI_EMBEDDER(source), result,
                                               &data->error);
    g_main_loop_quit(data->loop);
}

/**
 * ai_embedder_embed:
 * @self: an #AiEmbedder
 * @texts: (array zero-terminated=1): the passages to embed
 * @model: (nullable): the model, or %NULL for the provider's default
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Embeds one or more passages, blocking until they are done.
 *
 * Returns: (transfer full) (nullable): the vectors, or %NULL on error
 */
AiEmbedding *
ai_embedder_embed (
    AiEmbedder          *self,
    const gchar *const  *texts,
    const gchar         *model,
    GCancellable        *cancellable,
    GError             **error
){
    AiEmbedderSyncData data = { NULL, NULL, NULL };
    GMainContext *context;

    g_return_val_if_fail(AI_IS_EMBEDDER(self), NULL);
    g_return_val_if_fail(NULL != texts, NULL);
    g_return_val_if_fail((NULL == error) || (NULL == *error), NULL);

    if (NULL == texts[0])
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                            "There is nothing to embed");
        return NULL;
    }

    /*
     * A private context, for the reason the image generator uses one: the
     * default context belongs to whatever the application is already
     * running, and a synchronous call that waited on it would be at the
     * mercy of that. The cost is stated in the header -- a server attached
     * to the default context is not dispatched while this runs.
     */
    context = g_main_context_new();
    g_main_context_push_thread_default(context);

    data.loop = g_main_loop_new(context, FALSE);

    ai_embedder_embed_async(self, texts, model, cancellable,
                            ai_embedder_on_sync_done, &data);

    g_main_loop_run(data.loop);

    g_main_loop_unref(data.loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);

    if (NULL != data.error)
    {
        g_propagate_error(error, data.error);
        return NULL;
    }

    return data.embedding;
}

/**
 * ai_embedder_embed_one:
 * @self: an #AiEmbedder
 * @text: the passage
 * @model: (nullable): the model, or %NULL for the provider's default
 * @cancellable: (nullable): a #GCancellable
 * @error: return location for a #GError
 *
 * Embeds a single passage.
 *
 * Returns: (transfer full) (nullable): a result holding one vector
 */
AiEmbedding *
ai_embedder_embed_one (
    AiEmbedder    *self,
    const gchar   *text,
    const gchar   *model,
    GCancellable  *cancellable,
    GError       **error
){
    const gchar *one[2];

    g_return_val_if_fail(AI_IS_EMBEDDER(self), NULL);

    one[0] = (NULL != text) ? text : "";
    one[1] = NULL;

    return ai_embedder_embed(self, one, model, cancellable, error);
}

/**
 * ai_embedder_get_default_embedding_model:
 * @self: an #AiEmbedder
 *
 * The model used when a caller names none.
 *
 * Returns: (transfer none) (nullable): the model id, or %NULL
 */
const gchar *
ai_embedder_get_default_embedding_model (AiEmbedder *self)
{
    AiEmbedderInterface *iface;

    g_return_val_if_fail(AI_IS_EMBEDDER(self), NULL);

    iface = AI_EMBEDDER_GET_IFACE(self);

    if (NULL == iface->get_default_embedding_model)
        return NULL;

    return iface->get_default_embedding_model(self);
}

/**
 * ai_embedder_list_embedding_models:
 * @self: an #AiEmbedder
 *
 * Lists the embedding models this provider serves.
 *
 * Returns: (transfer container) (element-type AiEmbeddingModelInfo) (nullable):
 *   the models, or %NULL if the provider does not publish a table. The infos
 *   themselves are static and must not be freed.
 */
GList *
ai_embedder_list_embedding_models (AiEmbedder *self)
{
    AiEmbedderInterface *iface;

    g_return_val_if_fail(AI_IS_EMBEDDER(self), NULL);

    iface = AI_EMBEDDER_GET_IFACE(self);

    if (NULL == iface->list_embedding_models)
        return NULL;

    return iface->list_embedding_models(self);
}

/**
 * ai_embedder_get_model_info:
 * @self: an #AiEmbedder
 * @model: (nullable): the model, or %NULL for the default
 *
 * Looks a model up in this provider's table.
 *
 * Returns: (transfer none) (nullable): what is known about @model, or %NULL
 *   when this provider does not serve it
 */
const AiEmbeddingModelInfo *
ai_embedder_get_model_info (
    AiEmbedder  *self,
    const gchar *model
){
    g_autoptr(GList) models = NULL;
    const gchar *wanted;
    GList *item;

    g_return_val_if_fail(AI_IS_EMBEDDER(self), NULL);

    wanted = (NULL != model)
        ? model
        : ai_embedder_get_default_embedding_model(self);

    if (NULL == wanted)
        return NULL;

    models = ai_embedder_list_embedding_models(self);

    for (item = models; NULL != item; item = item->next)
    {
        const AiEmbeddingModelInfo *info = item->data;

        if ((NULL != info) && (0 == g_strcmp0(info->id, wanted)))
            return info;
    }

    return NULL;
}
