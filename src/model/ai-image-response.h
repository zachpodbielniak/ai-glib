/*
 * ai-image-response.h - Image generation response container
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

#include "model/ai-generated-image.h"

G_BEGIN_DECLS

#define AI_TYPE_IMAGE_RESPONSE (ai_image_response_get_type())

/**
 * AiImageResponse:
 *
 * A boxed type containing the response from an image generation request.
 * Contains the response ID, creation timestamp, and a list of generated images.
 */
typedef struct _AiImageResponse AiImageResponse;

/**
 * ai_image_response_get_type:
 *
 * Gets the #GType for #AiImageResponse.
 *
 * Returns: the #GType for #AiImageResponse
 */
GType
ai_image_response_get_type(void);

/**
 * ai_image_response_new:
 * @id: (nullable): the response ID
 * @created: the creation timestamp (Unix epoch)
 *
 * Creates a new empty #AiImageResponse.
 *
 * Returns: (transfer full): a new #AiImageResponse
 */
AiImageResponse *
ai_image_response_new(
    const gchar *id,
    gint64       created
);

/**
 * ai_image_response_copy:
 * @self: an #AiImageResponse
 *
 * Creates a copy of an #AiImageResponse.
 *
 * Returns: (transfer full): a copy of @self
 */
AiImageResponse *
ai_image_response_copy(const AiImageResponse *self);

/**
 * ai_image_response_free:
 * @self: (nullable): an #AiImageResponse
 *
 * Frees an #AiImageResponse instance.
 */
void
ai_image_response_free(AiImageResponse *self);

/**
 * ai_image_response_get_id:
 * @self: an #AiImageResponse
 *
 * Gets the response ID.
 *
 * Returns: (transfer none) (nullable): the response ID
 */
const gchar *
ai_image_response_get_id(const AiImageResponse *self);

/**
 * ai_image_response_get_created:
 * @self: an #AiImageResponse
 *
 * Gets the creation timestamp.
 *
 * Returns: the creation timestamp as Unix epoch
 */
gint64
ai_image_response_get_created(const AiImageResponse *self);

/**
 * ai_image_response_get_images:
 * @self: an #AiImageResponse
 *
 * Gets the list of generated images.
 *
 * Returns: (transfer none) (element-type AiGeneratedImage): the images list
 */
GList *
ai_image_response_get_images(const AiImageResponse *self);

/**
 * ai_image_response_get_image_count:
 * @self: an #AiImageResponse
 *
 * Gets the number of images in the response.
 *
 * Returns: the number of images
 */
guint
ai_image_response_get_image_count(const AiImageResponse *self);

/**
 * ai_image_response_get_image:
 * @self: an #AiImageResponse
 * @index: the image index (0-based)
 *
 * Gets a specific image by index.
 *
 * Returns: (transfer none) (nullable): the image at @index, or %NULL
 */
AiGeneratedImage *
ai_image_response_get_image(
    const AiImageResponse *self,
    guint                  index
);

/**
 * ai_image_response_add_image:
 * @self: an #AiImageResponse
 * @image: (transfer full): the image to add
 *
 * Adds an image to the response. Takes ownership of @image.
 */
void
ai_image_response_add_image(
    AiImageResponse  *self,
    AiGeneratedImage *image
);

/**
 * ai_image_response_get_model:
 * @self: an #AiImageResponse
 *
 * Gets the model used for generation.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_image_response_get_model(const AiImageResponse *self);

/**
 * ai_image_response_set_model:
 * @self: an #AiImageResponse
 * @model: (nullable): the model name
 *
 * Sets the model used for generation.
 */
void
ai_image_response_set_model(
    AiImageResponse *self,
    const gchar     *model
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiImageResponse, ai_image_response_free)

G_END_DECLS
