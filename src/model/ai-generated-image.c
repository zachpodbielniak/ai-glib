/*
 * ai-generated-image.c - Single generated image
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <gio/gio.h>

#include "model/ai-generated-image.h"
#include "core/ai-error.h"

/*
 * Private structure for AiGeneratedImage boxed type.
 * Stores either a URL or base64 data, plus metadata.
 */
struct _AiGeneratedImage
{
    gchar    *url;
    gchar    *base64_data;
    gchar    *mime_type;
    gchar    *revised_prompt;
    gboolean  is_url;
};

/*
 * ai_generated_image_get_type:
 *
 * Registers the AiGeneratedImage boxed type with the GLib type system.
 * Uses copy and free functions for memory management.
 */
G_DEFINE_BOXED_TYPE(AiGeneratedImage, ai_generated_image, ai_generated_image_copy, ai_generated_image_free)

/**
 * ai_generated_image_new_from_url:
 * @url: the URL of the generated image
 *
 * Creates a new #AiGeneratedImage from a URL.
 *
 * Returns: (transfer full): a new #AiGeneratedImage
 */
AiGeneratedImage *
ai_generated_image_new_from_url(const gchar *url)
{
    AiGeneratedImage *self;

    g_return_val_if_fail(url != NULL, NULL);

    self = g_slice_new0(AiGeneratedImage);
    self->url = g_strdup(url);
    self->base64_data = NULL;
    self->mime_type = NULL;
    self->revised_prompt = NULL;
    self->is_url = TRUE;

    return self;
}

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
){
    AiGeneratedImage *self;

    g_return_val_if_fail(base64_data != NULL, NULL);

    self = g_slice_new0(AiGeneratedImage);
    self->url = NULL;
    self->base64_data = g_strdup(base64_data);
    self->mime_type = g_strdup(mime_type);
    self->revised_prompt = NULL;
    self->is_url = FALSE;

    return self;
}

/**
 * ai_generated_image_copy:
 * @self: an #AiGeneratedImage
 *
 * Creates a deep copy of an #AiGeneratedImage instance.
 *
 * Returns: (transfer full): a copy of @self
 */
AiGeneratedImage *
ai_generated_image_copy(const AiGeneratedImage *self)
{
    AiGeneratedImage *copy;

    if (self == NULL)
    {
        return NULL;
    }

    copy = g_slice_new0(AiGeneratedImage);
    copy->url = g_strdup(self->url);
    copy->base64_data = g_strdup(self->base64_data);
    copy->mime_type = g_strdup(self->mime_type);
    copy->revised_prompt = g_strdup(self->revised_prompt);
    copy->is_url = self->is_url;

    return copy;
}

/**
 * ai_generated_image_free:
 * @self: (nullable): an #AiGeneratedImage
 *
 * Frees an #AiGeneratedImage instance and all its allocated memory.
 * If @self is %NULL, this function does nothing.
 */
void
ai_generated_image_free(AiGeneratedImage *self)
{
    if (self == NULL)
    {
        return;
    }

    g_clear_pointer(&self->url, g_free);
    g_clear_pointer(&self->base64_data, g_free);
    g_clear_pointer(&self->mime_type, g_free);
    g_clear_pointer(&self->revised_prompt, g_free);

    g_slice_free(AiGeneratedImage, self);
}

/**
 * ai_generated_image_is_url:
 * @self: an #AiGeneratedImage
 *
 * Checks if this image contains a URL reference.
 *
 * Returns: %TRUE if the image is a URL, %FALSE if base64
 */
gboolean
ai_generated_image_is_url(const AiGeneratedImage *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->is_url;
}

/**
 * ai_generated_image_is_base64:
 * @self: an #AiGeneratedImage
 *
 * Checks if this image contains base64-encoded data.
 *
 * Returns: %TRUE if the image is base64, %FALSE if URL
 */
gboolean
ai_generated_image_is_base64(const AiGeneratedImage *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return !self->is_url;
}

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
ai_generated_image_get_url(const AiGeneratedImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->url;
}

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
ai_generated_image_get_base64(const AiGeneratedImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->base64_data;
}

/**
 * ai_generated_image_get_mime_type:
 * @self: an #AiGeneratedImage
 *
 * Gets the MIME type of the image.
 *
 * Returns: (transfer none) (nullable): the MIME type, or %NULL if unknown
 */
const gchar *
ai_generated_image_get_mime_type(const AiGeneratedImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->mime_type;
}

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
){
    g_return_if_fail(self != NULL);

    g_free(self->mime_type);
    self->mime_type = g_strdup(mime_type);
}

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
ai_generated_image_get_revised_prompt(const AiGeneratedImage *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->revised_prompt;
}

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
){
    g_return_if_fail(self != NULL);

    g_free(self->revised_prompt);
    self->revised_prompt = g_strdup(revised_prompt);
}

/**
 * ai_generated_image_get_bytes:
 * @self: an #AiGeneratedImage
 * @error: (out) (optional): return location for a #GError
 *
 * Gets the raw image data as a #GBytes.
 * For base64 images, this decodes the data.
 * For URL images, this returns NULL (use async fetch instead).
 *
 * Returns: (transfer full) (nullable): the image data, or %NULL on error
 */
GBytes *
ai_generated_image_get_bytes(
    AiGeneratedImage *self,
    GError          **error
){
    g_return_val_if_fail(self != NULL, NULL);

    if (self->is_url)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "Cannot synchronously get bytes for URL image. Use async API.");
        return NULL;
    }

    if (self->base64_data == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "No base64 data available");
        return NULL;
    }

    {
        gsize out_len;
        guchar *decoded;

        decoded = g_base64_decode(self->base64_data, &out_len);
        if (decoded == NULL)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                        "Failed to decode base64 data");
            return NULL;
        }

        return g_bytes_new_take(decoded, out_len);
    }
}

/**
 * ai_generated_image_save_to_file:
 * @self: an #AiGeneratedImage
 * @path: the file path to save to
 * @error: (out) (optional): return location for a #GError
 *
 * Saves the generated image to a file.
 * Only works for base64 images; for URL images, fetch first.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean
ai_generated_image_save_to_file(
    AiGeneratedImage *self,
    const gchar      *path,
    GError          **error
){
    g_autoptr(GBytes) bytes = NULL;
    g_autoptr(GFile) file = NULL;
    g_autoptr(GFileOutputStream) stream = NULL;
    const guchar *data;
    gsize len;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    bytes = ai_generated_image_get_bytes(self, error);
    if (bytes == NULL)
    {
        return FALSE;
    }

    file = g_file_new_for_path(path);
    stream = g_file_replace(file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, error);
    if (stream == NULL)
    {
        return FALSE;
    }

    data = g_bytes_get_data(bytes, &len);
    return g_output_stream_write_all(G_OUTPUT_STREAM(stream), data, len,
                                     NULL, NULL, error);
}
