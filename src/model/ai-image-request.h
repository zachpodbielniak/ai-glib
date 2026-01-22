/*
 * ai-image-request.h - Image generation request parameters
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

#include "core/ai-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_IMAGE_REQUEST (ai_image_request_get_type())

/**
 * AiImageRequest:
 *
 * A boxed type containing parameters for an image generation request.
 * Use this to specify the prompt, model, size, quality, style, and other
 * options when generating images.
 */
typedef struct _AiImageRequest AiImageRequest;

/**
 * ai_image_request_get_type:
 *
 * Gets the #GType for #AiImageRequest.
 *
 * Returns: the #GType for #AiImageRequest
 */
GType
ai_image_request_get_type(void);

/**
 * ai_image_request_new:
 * @prompt: the text prompt describing the image to generate
 *
 * Creates a new #AiImageRequest with the specified prompt.
 * Other parameters will be set to their default values.
 *
 * Returns: (transfer full): a new #AiImageRequest
 */
AiImageRequest *
ai_image_request_new(const gchar *prompt);

/**
 * ai_image_request_copy:
 * @self: an #AiImageRequest
 *
 * Creates a copy of an #AiImageRequest.
 *
 * Returns: (transfer full): a copy of @self
 */
AiImageRequest *
ai_image_request_copy(const AiImageRequest *self);

/**
 * ai_image_request_free:
 * @self: (nullable): an #AiImageRequest
 *
 * Frees an #AiImageRequest instance.
 */
void
ai_image_request_free(AiImageRequest *self);

/**
 * ai_image_request_get_prompt:
 * @self: an #AiImageRequest
 *
 * Gets the text prompt for the image generation.
 *
 * Returns: (transfer none): the prompt string
 */
const gchar *
ai_image_request_get_prompt(const AiImageRequest *self);

/**
 * ai_image_request_set_prompt:
 * @self: an #AiImageRequest
 * @prompt: the new prompt
 *
 * Sets the text prompt for the image generation.
 */
void
ai_image_request_set_prompt(
    AiImageRequest *self,
    const gchar    *prompt
);

/**
 * ai_image_request_get_model:
 * @self: an #AiImageRequest
 *
 * Gets the model to use for image generation.
 *
 * Returns: (transfer none) (nullable): the model string, or %NULL for default
 */
const gchar *
ai_image_request_get_model(const AiImageRequest *self);

/**
 * ai_image_request_set_model:
 * @self: an #AiImageRequest
 * @model: (nullable): the model to use, or %NULL for default
 *
 * Sets the model to use for image generation.
 */
void
ai_image_request_set_model(
    AiImageRequest *self,
    const gchar    *model
);

/**
 * ai_image_request_get_size:
 * @self: an #AiImageRequest
 *
 * Gets the image size setting.
 *
 * Returns: the #AiImageSize
 */
AiImageSize
ai_image_request_get_size(const AiImageRequest *self);

/**
 * ai_image_request_set_size:
 * @self: an #AiImageRequest
 * @size: the image size
 *
 * Sets the image size for generation.
 */
void
ai_image_request_set_size(
    AiImageRequest *self,
    AiImageSize     size
);

/**
 * ai_image_request_get_custom_size:
 * @self: an #AiImageRequest
 *
 * Gets the custom size string (used when size is %AI_IMAGE_SIZE_CUSTOM).
 *
 * Returns: (transfer none) (nullable): the custom size string
 */
const gchar *
ai_image_request_get_custom_size(const AiImageRequest *self);

/**
 * ai_image_request_set_custom_size:
 * @self: an #AiImageRequest
 * @custom_size: (nullable): the custom size string (e.g., "800x600")
 *
 * Sets a custom size string. This also sets size to %AI_IMAGE_SIZE_CUSTOM.
 */
void
ai_image_request_set_custom_size(
    AiImageRequest *self,
    const gchar    *custom_size
);

/**
 * ai_image_request_get_quality:
 * @self: an #AiImageRequest
 *
 * Gets the image quality setting.
 *
 * Returns: the #AiImageQuality
 */
AiImageQuality
ai_image_request_get_quality(const AiImageRequest *self);

/**
 * ai_image_request_set_quality:
 * @self: an #AiImageRequest
 * @quality: the image quality
 *
 * Sets the image quality for generation.
 */
void
ai_image_request_set_quality(
    AiImageRequest *self,
    AiImageQuality  quality
);

/**
 * ai_image_request_get_style:
 * @self: an #AiImageRequest
 *
 * Gets the image style setting.
 *
 * Returns: the #AiImageStyle
 */
AiImageStyle
ai_image_request_get_style(const AiImageRequest *self);

/**
 * ai_image_request_set_style:
 * @self: an #AiImageRequest
 * @style: the image style
 *
 * Sets the image style for generation.
 */
void
ai_image_request_set_style(
    AiImageRequest *self,
    AiImageStyle    style
);

/**
 * ai_image_request_get_count:
 * @self: an #AiImageRequest
 *
 * Gets the number of images to generate.
 *
 * Returns: the count (default 1)
 */
gint
ai_image_request_get_count(const AiImageRequest *self);

/**
 * ai_image_request_set_count:
 * @self: an #AiImageRequest
 * @count: the number of images to generate (1-10)
 *
 * Sets the number of images to generate.
 */
void
ai_image_request_set_count(
    AiImageRequest *self,
    gint            count
);

/**
 * ai_image_request_get_response_format:
 * @self: an #AiImageRequest
 *
 * Gets the response format setting.
 *
 * Returns: the #AiImageResponseFormat
 */
AiImageResponseFormat
ai_image_request_get_response_format(const AiImageRequest *self);

/**
 * ai_image_request_set_response_format:
 * @self: an #AiImageRequest
 * @format: the response format
 *
 * Sets the response format for generated images.
 */
void
ai_image_request_set_response_format(
    AiImageRequest        *self,
    AiImageResponseFormat  format
);

/**
 * ai_image_request_get_user:
 * @self: an #AiImageRequest
 *
 * Gets the user identifier for abuse tracking.
 *
 * Returns: (transfer none) (nullable): the user identifier
 */
const gchar *
ai_image_request_get_user(const AiImageRequest *self);

/**
 * ai_image_request_set_user:
 * @self: an #AiImageRequest
 * @user: (nullable): the user identifier
 *
 * Sets the user identifier for abuse tracking.
 */
void
ai_image_request_set_user(
    AiImageRequest *self,
    const gchar    *user
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiImageRequest, ai_image_request_free)

G_END_DECLS
