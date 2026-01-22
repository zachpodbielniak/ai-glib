/*
 * ai-image-generator.h - Image generation interface
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
#include <gio/gio.h>

#include "model/ai-image-request.h"
#include "model/ai-image-response.h"

G_BEGIN_DECLS

#define AI_TYPE_IMAGE_GENERATOR (ai_image_generator_get_type())

G_DECLARE_INTERFACE(AiImageGenerator, ai_image_generator, AI, IMAGE_GENERATOR, GObject)

/**
 * AiImageGeneratorInterface:
 * @parent_iface: the parent interface
 * @generate_image_async: starts an asynchronous image generation
 * @generate_image_finish: finishes an asynchronous image generation
 * @get_supported_sizes: gets the list of supported image sizes
 * @get_default_model: gets the default model for image generation
 * @_reserved: reserved for future expansion
 *
 * Interface for AI image generation providers.
 */
struct _AiImageGeneratorInterface
{
    GTypeInterface parent_iface;

    /* Virtual methods */
    void              (*generate_image_async)  (AiImageGenerator    *self,
                                                AiImageRequest      *request,
                                                GCancellable        *cancellable,
                                                GAsyncReadyCallback  callback,
                                                gpointer             user_data);
    AiImageResponse * (*generate_image_finish) (AiImageGenerator    *self,
                                                GAsyncResult        *result,
                                                GError             **error);
    GList *           (*get_supported_sizes)   (AiImageGenerator    *self);
    const gchar *     (*get_default_model)     (AiImageGenerator    *self);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

/**
 * ai_image_generator_generate_image_async:
 * @self: an #AiImageGenerator
 * @request: the image generation request parameters
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): callback to call when done
 * @user_data: user data for the callback
 *
 * Starts an asynchronous image generation request.
 */
void
ai_image_generator_generate_image_async(
    AiImageGenerator    *self,
    AiImageRequest      *request,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

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
);

/**
 * ai_image_generator_get_supported_sizes:
 * @self: an #AiImageGenerator
 *
 * Gets the list of supported image sizes for this provider.
 *
 * Returns: (transfer full) (element-type utf8): list of size strings
 */
GList *
ai_image_generator_get_supported_sizes(AiImageGenerator *self);

/**
 * ai_image_generator_get_default_model:
 * @self: an #AiImageGenerator
 *
 * Gets the default model for image generation.
 *
 * Returns: (transfer none): the default model name
 */
const gchar *
ai_image_generator_get_default_model(AiImageGenerator *self);

G_END_DECLS
