/*
 * ai-image-generator.c - Image generation interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-image-generator.h"

G_DEFINE_INTERFACE(AiImageGenerator, ai_image_generator, G_TYPE_OBJECT)

/*
 * Signals are registered on the interface itself, the same way
 * AiStreamable registers its streaming signals, so any implementor gets
 * them without repeating the boilerplate.
 */
static void
ai_image_generator_default_init(AiImageGeneratorInterface *iface)
{
    (void)iface;

    /**
     * AiImageGenerator::image-progress:
     * @generator: the #AiImageGenerator
     * @completed: how many images have been produced so far
     * @total: how many were requested
     *
     * Emitted as each image of a multi-image request becomes available.
     *
     * Generating four images takes four times as long as generating one,
     * with nothing to show in between; this gives a front-end something
     * to report while it waits.
     */
    g_signal_new("image-progress",
                 AI_TYPE_IMAGE_GENERATOR,
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);

    /**
     * AiImageGenerator::partial-image:
     * @generator: the #AiImageGenerator
     * @preview: (type GLib.Bytes): the partially-rendered image
     * @index: which requested image this preview belongs to
     *
     * Emitted when a provider streams a partial preview of an image still
     * being generated.
     *
     * Only fires for models advertising
     * %AI_IMAGE_CAP_PARTIAL_STREAMING, and only when the request asked
     * for previews via ai_image_request_set_partial_images().
     */
    g_signal_new("partial-image",
                 AI_TYPE_IMAGE_GENERATOR,
                 G_SIGNAL_RUN_LAST,
                 0, NULL, NULL, NULL,
                 G_TYPE_NONE, 2, G_TYPE_BYTES, G_TYPE_UINT);
}

/* Bundle for the nested main loop driving the synchronous wrapper. */
typedef struct
{
    GMainLoop        *loop;
    AiImageResponse  *response;
    GError           *error;
} AiImageSyncData;

/*
 * Completion callback for ai_image_generator_generate_image().
 */
static void
ai_image_generator_on_sync_done(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    AiImageSyncData *data = user_data;

    data->response = ai_image_generator_generate_image_finish(
        AI_IMAGE_GENERATOR(source), result, &data->error);

    g_main_loop_quit(data->loop);
}

/**
 * ai_image_generator_generate_image_async:
 * @self: an #AiImageGenerator
 * @request: the image generation request parameters
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): callback to call when done
 * @user_data: user data for the callback
 *
 * Starts an asynchronous image generation request.
 * The @request parameter contains all the options for the generation,
 * including the prompt, model, size, quality, and style settings.
 */
void
ai_image_generator_generate_image_async(
    AiImageGenerator    *self,
    AiImageRequest      *request,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiImageGeneratorInterface *iface;

    g_return_if_fail(AI_IS_IMAGE_GENERATOR(self));
    g_return_if_fail(request != NULL);

    iface = AI_IMAGE_GENERATOR_GET_IFACE(self);
    g_return_if_fail(iface->generate_image_async != NULL);

    iface->generate_image_async(self, request, cancellable, callback, user_data);
}

/**
 * ai_image_generator_generate_image_finish:
 * @self: an #AiImageGenerator
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes an asynchronous image generation request.
 *
 * Returns: (transfer full) (nullable): the #AiImageResponse, or %NULL on error
 */
AiImageResponse *
ai_image_generator_generate_image_finish(
    AiImageGenerator  *self,
    GAsyncResult      *result,
    GError           **error
){
    AiImageGeneratorInterface *iface;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), NULL);

    iface = AI_IMAGE_GENERATOR_GET_IFACE(self);
    g_return_val_if_fail(iface->generate_image_finish != NULL, NULL);

    return iface->generate_image_finish(self, result, error);
}

/**
 * ai_image_generator_generate_image:
 * @self: an #AiImageGenerator
 * @request: the image generation request parameters
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Generates an image synchronously, blocking until it arrives.
 *
 * Implemented generically on top of the asynchronous pair by running a
 * nested #GMainLoop on a private #GMainContext, so every provider gets it
 * without implementing anything.  The private context is what makes this
 * safe to call from inside an application that is already running a main
 * loop: the nested loop dispatches only this request's own sources and
 * cannot re-enter the caller's.
 *
 * Image generation takes tens of seconds, so this will block for a long
 * time.  Prefer ai_image_generator_generate_image_async() anywhere
 * responsiveness matters; this exists for scripts, command-line tools and
 * synchronous embedding APIs that have no other option.
 *
 * Returns: (transfer full) (nullable): the #AiImageResponse, or %NULL on
 *   error
 */
AiImageResponse *
ai_image_generator_generate_image(
    AiImageGenerator  *self,
    AiImageRequest    *request,
    GCancellable      *cancellable,
    GError           **error
){
    AiImageSyncData data = { NULL, NULL, NULL };
    GMainContext *context;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), NULL);
    g_return_val_if_fail(request != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    context = g_main_context_new();
    g_main_context_push_thread_default(context);

    data.loop = g_main_loop_new(context, FALSE);

    ai_image_generator_generate_image_async(
        self, request, cancellable, ai_image_generator_on_sync_done, &data);

    g_main_loop_run(data.loop);

    g_main_loop_unref(data.loop);
    g_main_context_pop_thread_default(context);
    g_main_context_unref(context);

    if (data.error != NULL)
    {
        g_propagate_error(error, data.error);
        return NULL;
    }

    return data.response;
}

/**
 * ai_image_generator_get_supported_sizes:
 * @self: an #AiImageGenerator
 *
 * Gets the list of supported image sizes for this provider.
 *
 * When a provider does not implement this directly, the sizes are derived
 * from its default model's #AiImageModelInfo.
 *
 * Returns: (transfer full) (element-type utf8): list of size strings
 */
GList *
ai_image_generator_get_supported_sizes(AiImageGenerator *self)
{
    AiImageGeneratorInterface *iface;
    const AiImageModelInfo *info;
    const gchar * const *sizes;
    GList *out = NULL;
    guint i;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), NULL);

    iface = AI_IMAGE_GENERATOR_GET_IFACE(self);
    if (iface->get_supported_sizes != NULL)
    {
        return iface->get_supported_sizes(self);
    }

    /* Fall back to the model table.  A model expressing geometry as
     * aspect ratios has no pixel sizes, so report the ratios instead --
     * they are what a caller would set for that model anyway. */
    info = ai_image_generator_get_model_info(
        self, ai_image_generator_get_default_model(self));
    if (info == NULL)
    {
        return NULL;
    }

    sizes = ai_image_model_info_get_sizes(info);
    if (sizes == NULL || sizes[0] == NULL)
    {
        sizes = ai_image_model_info_get_aspect_ratios(info);
    }

    if (sizes == NULL)
    {
        return NULL;
    }

    for (i = 0; sizes[i] != NULL; i++)
    {
        out = g_list_append(out, g_strdup(sizes[i]));
    }

    return out;
}

/**
 * ai_image_generator_list_image_models:
 * @self: an #AiImageGenerator
 *
 * Lists the image models this provider serves, with their capabilities.
 *
 * Returns: (transfer full) (element-type AiImageModelInfo) (nullable):
 *   the models, or %NULL if the provider does not publish a model table
 */
GList *
ai_image_generator_list_image_models(AiImageGenerator *self)
{
    AiImageGeneratorInterface *iface;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), NULL);

    iface = AI_IMAGE_GENERATOR_GET_IFACE(self);
    if (iface->list_image_models == NULL)
    {
        return NULL;
    }

    return iface->list_image_models(self);
}

/**
 * ai_image_generator_get_model_info:
 * @self: an #AiImageGenerator
 * @model: (nullable): a model id, or %NULL for the provider default
 *
 * Looks up the capability descriptor for @model.
 *
 * Returning %NULL for a model the provider has no entry for is
 * deliberate, and callers must treat it as "unknown" rather than
 * "unsupported": a model released after this build should still work, so
 * ai_image_request_validate() passes an unknown model through untouched
 * instead of second-guessing it.
 *
 * Returns: (transfer none) (nullable): the model info, or %NULL if
 *   unknown
 */
const AiImageModelInfo *
ai_image_generator_get_model_info(
    AiImageGenerator *self,
    const gchar      *model
){
    g_autolist(AiImageModelInfo) models = NULL;
    GHashTable *cache;
    static GMutex cache_lock;
    const AiImageModelInfo *found = NULL;
    GList *iter;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), NULL);

    if (model == NULL)
    {
        model = ai_image_generator_get_default_model(self);
        if (model == NULL)
        {
            return NULL;
        }
    }

    models = ai_image_generator_list_image_models(self);
    if (models == NULL)
    {
        return NULL;
    }

    for (iter = models; iter != NULL; iter = iter->next)
    {
        AiImageModelInfo *info = iter->data;

        if (g_strcmp0(ai_image_model_info_get_id(info), model) == 0)
        {
            found = info;
            break;
        }
    }

    if (found == NULL)
    {
        return NULL;
    }

    /*
     * The list is transfer-full but this returns transfer-none, so the
     * descriptor has to outlive the list.  Model tables are small, fixed,
     * and looked up repeatedly, so keep one copy per model id in a
     * per-instance cache rather than making every caller manage a
     * lifetime for what is really static data. Deployment-specific tables
     * can describe identical model IDs differently, so never share this
     * cache between generators. Returned data lives as long as self.
     */
    g_mutex_lock(&cache_lock);

    cache = g_object_get_data(G_OBJECT(self), "ai-image-model-cache");
    if (cache == NULL)
    {
        cache = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)ai_image_model_info_free);
        g_object_set_data_full(G_OBJECT(self), "ai-image-model-cache", cache,
                               (GDestroyNotify)g_hash_table_unref);
    }

    if (!g_hash_table_contains(cache, model))
    {
        g_hash_table_insert(cache, g_strdup(model),
                            ai_image_model_info_copy(found));
    }

    found = g_hash_table_lookup(cache, model);

    g_mutex_unlock(&cache_lock);

    return found;
}

/**
 * ai_image_generator_get_capabilities:
 * @self: an #AiImageGenerator
 * @model: (nullable): a model id, or %NULL for the provider default
 *
 * Gets the capability set of @model.
 *
 * Returns: the #AiImageCapabilities, or %AI_IMAGE_CAP_NONE if the model
 *   is unknown
 */
AiImageCapabilities
ai_image_generator_get_capabilities(
    AiImageGenerator *self,
    const gchar      *model
){
    const AiImageModelInfo *info;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), AI_IMAGE_CAP_NONE);

    info = ai_image_generator_get_model_info(self, model);
    if (info == NULL)
    {
        return AI_IMAGE_CAP_NONE;
    }

    return ai_image_model_info_get_capabilities(info);
}

/**
 * ai_image_generator_supports:
 * @self: an #AiImageGenerator
 * @model: (nullable): a model id, or %NULL for the provider default
 * @capability: the capability, or capabilities, to test for
 *
 * Tests whether @model supports @capability.
 *
 * Returns: %TRUE if every requested capability is supported
 */
gboolean
ai_image_generator_supports(
    AiImageGenerator    *self,
    const gchar         *model,
    AiImageCapabilities  capability
){
    AiImageCapabilities capabilities;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), FALSE);

    capabilities = ai_image_generator_get_capabilities(self, model);

    return (capabilities & capability) == capability;
}

/**
 * ai_image_generator_get_default_model:
 * @self: an #AiImageGenerator
 *
 * Gets the default model for image generation.
 *
 * Returns: (transfer none) (nullable): the default model name, or %NULL
 *   if the caller must choose a model
 */
const gchar *
ai_image_generator_get_default_model(AiImageGenerator *self)
{
    AiImageGeneratorInterface *iface;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), NULL);

    iface = AI_IMAGE_GENERATOR_GET_IFACE(self);
    if (iface->get_default_model == NULL)
    {
        return NULL;
    }

    return iface->get_default_model(self);
}
