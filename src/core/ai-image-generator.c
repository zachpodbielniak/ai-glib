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

static void
ai_image_generator_default_init(AiImageGeneratorInterface *iface)
{
    (void)iface;
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
 * ai_image_generator_get_supported_sizes:
 * @self: an #AiImageGenerator
 *
 * Gets the list of supported image sizes for this provider.
 *
 * Returns: (transfer full) (element-type utf8): list of size strings
 */
GList *
ai_image_generator_get_supported_sizes(AiImageGenerator *self)
{
    AiImageGeneratorInterface *iface;

    g_return_val_if_fail(AI_IS_IMAGE_GENERATOR(self), NULL);

    iface = AI_IMAGE_GENERATOR_GET_IFACE(self);
    if (iface->get_supported_sizes == NULL)
    {
        return NULL;
    }

    return iface->get_supported_sizes(self);
}

/**
 * ai_image_generator_get_default_model:
 * @self: an #AiImageGenerator
 *
 * Gets the default model for image generation.
 *
 * Returns: (transfer none): the default model name
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
