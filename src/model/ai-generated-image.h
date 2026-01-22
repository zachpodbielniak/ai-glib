/*
 * ai-generated-image.h - Single generated image
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

#define AI_TYPE_GENERATED_IMAGE (ai_generated_image_get_type())

/**
 * AiGeneratedImage:
 *
 * A boxed type representing a single generated image.
 * Contains either a URL or base64-encoded data, along with
 * metadata like the MIME type and revised prompt.
 */
typedef struct _AiGeneratedImage AiGeneratedImage;

/**
 * ai_generated_image_get_type:
 *
 * Gets the #GType for #AiGeneratedImage.
 *
 * Returns: the #GType for #AiGeneratedImage
 */
GType
ai_generated_image_get_type(void);

/**
 * ai_generated_image_new_from_url:
 * @url: the URL of the generated image
 *
 * Creates a new #AiGeneratedImage from a URL.
 *
 * Returns: (transfer full): a new #AiGeneratedImage
 */
AiGeneratedImage *
ai_generated_image_new_from_url(const gchar *url);

/**
 * ai_generated_image_new_from_base64:
 * @base64_data: the base64-encoded image data
 * @mime_type: (nullable): the MIME type (e.g., "image/png"), or %NULL
 *
 * Creates a new #AiGeneratedImage from base64-encoded data.
 *
 * Returns: (transfer full): a new #AiGeneratedImage
 */
AiGeneratedImage *
ai_generated_image_new_from_base64(
    const gchar *base64_data,
    const gchar *mime_type
);

/**
 * ai_generated_image_copy:
 * @self: an #AiGeneratedImage
 *
 * Creates a copy of an #AiGeneratedImage.
 *
 * Returns: (transfer full): a copy of @self
 */
AiGeneratedImage *
ai_generated_image_copy(const AiGeneratedImage *self);

/**
 * ai_generated_image_free:
 * @self: (nullable): an #AiGeneratedImage
 *
 * Frees an #AiGeneratedImage instance.
 */
void
ai_generated_image_free(AiGeneratedImage *self);

/**
 * ai_generated_image_is_url:
 * @self: an #AiGeneratedImage
 *
 * Checks if this image contains a URL reference.
 *
 * Returns: %TRUE if the image is a URL, %FALSE if base64
 */
gboolean
ai_generated_image_is_url(const AiGeneratedImage *self);

/**
 * ai_generated_image_is_base64:
 * @self: an #AiGeneratedImage
 *
 * Checks if this image contains base64-encoded data.
 *
 * Returns: %TRUE if the image is base64, %FALSE if URL
 */
gboolean
ai_generated_image_is_base64(const AiGeneratedImage *self);

/**
 * ai_generated_image_get_url:
 * @self: an #AiGeneratedImage
 *
 * Gets the URL of the generated image.
 * Only valid if ai_generated_image_is_url() returns %TRUE.
 *
 * Returns: (transfer none) (nullable): the URL, or %NULL if base64
 */
const gchar *
ai_generated_image_get_url(const AiGeneratedImage *self);

/**
 * ai_generated_image_get_base64:
 * @self: an #AiGeneratedImage
 *
 * Gets the base64-encoded image data.
 * Only valid if ai_generated_image_is_base64() returns %TRUE.
 *
 * Returns: (transfer none) (nullable): the base64 data, or %NULL if URL
 */
const gchar *
ai_generated_image_get_base64(const AiGeneratedImage *self);

/**
 * ai_generated_image_get_mime_type:
 * @self: an #AiGeneratedImage
 *
 * Gets the MIME type of the image.
 *
 * Returns: (transfer none) (nullable): the MIME type, or %NULL if unknown
 */
const gchar *
ai_generated_image_get_mime_type(const AiGeneratedImage *self);

/**
 * ai_generated_image_set_mime_type:
 * @self: an #AiGeneratedImage
 * @mime_type: (nullable): the MIME type
 *
 * Sets the MIME type of the image.
 */
void
ai_generated_image_set_mime_type(
    AiGeneratedImage *self,
    const gchar      *mime_type
);

/**
 * ai_generated_image_get_revised_prompt:
 * @self: an #AiGeneratedImage
 *
 * Gets the revised prompt used to generate the image.
 * Some providers (like DALL-E 3) may modify the prompt for better results.
 *
 * Returns: (transfer none) (nullable): the revised prompt, or %NULL
 */
const gchar *
ai_generated_image_get_revised_prompt(const AiGeneratedImage *self);

/**
 * ai_generated_image_set_revised_prompt:
 * @self: an #AiGeneratedImage
 * @revised_prompt: (nullable): the revised prompt
 *
 * Sets the revised prompt used to generate the image.
 */
void
ai_generated_image_set_revised_prompt(
    AiGeneratedImage *self,
    const gchar      *revised_prompt
);

/**
 * ai_generated_image_get_bytes:
 * @self: an #AiGeneratedImage
 * @error: (out) (optional): return location for a #GError
 *
 * Gets the raw image data as a #GBytes.
 * For URL images, this will fetch the data.
 * For base64 images, this will decode the data.
 *
 * Returns: (transfer full) (nullable): the image data, or %NULL on error
 */
GBytes *
ai_generated_image_get_bytes(
    AiGeneratedImage *self,
    GError          **error
);

/**
 * ai_generated_image_save_to_file:
 * @self: an #AiGeneratedImage
 * @path: the file path to save to
 * @error: (out) (optional): return location for a #GError
 *
 * Saves the generated image to a file.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
ai_generated_image_save_to_file(
    AiGeneratedImage *self,
    const gchar      *path,
    GError          **error
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiGeneratedImage, ai_generated_image_free)

G_END_DECLS
