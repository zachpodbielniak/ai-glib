/*
 * ai-image-request.c - Image generation request parameters
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-image-request.h"

/*
 * Private structure for AiImageRequest boxed type.
 * Stores all the parameters for an image generation request.
 */
struct _AiImageRequest
{
    gchar                *prompt;
    gchar                *model;
    AiImageSize           size;
    gchar                *custom_size;
    AiImageQuality        quality;
    AiImageStyle          style;
    gint                  count;
    AiImageResponseFormat response_format;
    gchar                *user;
};

/*
 * ai_image_request_get_type:
 *
 * Registers the AiImageRequest boxed type with the GLib type system.
 * Uses copy and free functions for memory management.
 */
G_DEFINE_BOXED_TYPE(AiImageRequest, ai_image_request, ai_image_request_copy, ai_image_request_free)

/**
 * ai_image_request_new:
 * @prompt: the text prompt describing the image to generate
 *
 * Creates a new #AiImageRequest with the specified prompt.
 * Default values:
 * - model: NULL (provider default)
 * - size: AI_IMAGE_SIZE_AUTO
 * - quality: AI_IMAGE_QUALITY_AUTO
 * - style: AI_IMAGE_STYLE_AUTO
 * - count: 1
 * - response_format: AI_IMAGE_RESPONSE_URL
 *
 * Returns: (transfer full): a new #AiImageRequest
 */
AiImageRequest *
ai_image_request_new(const gchar *prompt)
{
    AiImageRequest *self;

    g_return_val_if_fail(prompt != NULL, NULL);

    self = g_slice_new0(AiImageRequest);
    self->prompt = g_strdup(prompt);
    self->model = NULL;
    self->size = AI_IMAGE_SIZE_AUTO;
    self->custom_size = NULL;
    self->quality = AI_IMAGE_QUALITY_AUTO;
    self->style = AI_IMAGE_STYLE_AUTO;
    self->count = 1;
    self->response_format = AI_IMAGE_RESPONSE_URL;
    self->user = NULL;

    return self;
}

/**
 * ai_image_request_copy:
 * @self: an #AiImageRequest
 *
 * Creates a deep copy of an #AiImageRequest instance.
 *
 * Returns: (transfer full): a copy of @self
 */
AiImageRequest *
ai_image_request_copy(const AiImageRequest *self)
{
    AiImageRequest *copy;

    if (self == NULL)
    {
        return NULL;
    }

    copy = g_slice_new0(AiImageRequest);
    copy->prompt = g_strdup(self->prompt);
    copy->model = g_strdup(self->model);
    copy->size = self->size;
    copy->custom_size = g_strdup(self->custom_size);
    copy->quality = self->quality;
    copy->style = self->style;
    copy->count = self->count;
    copy->response_format = self->response_format;
    copy->user = g_strdup(self->user);

    return copy;
}

/**
 * ai_image_request_free:
 * @self: (nullable): an #AiImageRequest
 *
 * Frees an #AiImageRequest instance and all its allocated memory.
 * If @self is %NULL, this function does nothing.
 */
void
ai_image_request_free(AiImageRequest *self)
{
    if (self == NULL)
    {
        return;
    }

    g_clear_pointer(&self->prompt, g_free);
    g_clear_pointer(&self->model, g_free);
    g_clear_pointer(&self->custom_size, g_free);
    g_clear_pointer(&self->user, g_free);

    g_slice_free(AiImageRequest, self);
}

/**
 * ai_image_request_get_prompt:
 * @self: an #AiImageRequest
 *
 * Gets the text prompt for the image generation.
 *
 * Returns: (transfer none): the prompt string
 */
const gchar *
ai_image_request_get_prompt(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->prompt;
}

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
){
    g_return_if_fail(self != NULL);
    g_return_if_fail(prompt != NULL);

    g_free(self->prompt);
    self->prompt = g_strdup(prompt);
}

/**
 * ai_image_request_get_model:
 * @self: an #AiImageRequest
 *
 * Gets the model to use for image generation.
 *
 * Returns: (transfer none) (nullable): the model string, or %NULL for default
 */
const gchar *
ai_image_request_get_model(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->model;
}

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
){
    g_return_if_fail(self != NULL);

    g_free(self->model);
    self->model = g_strdup(model);
}

/**
 * ai_image_request_get_size:
 * @self: an #AiImageRequest
 *
 * Gets the image size setting.
 *
 * Returns: the #AiImageSize
 */
AiImageSize
ai_image_request_get_size(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_SIZE_AUTO);

    return self->size;
}

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
){
    g_return_if_fail(self != NULL);

    self->size = size;
}

/**
 * ai_image_request_get_custom_size:
 * @self: an #AiImageRequest
 *
 * Gets the custom size string (used when size is %AI_IMAGE_SIZE_CUSTOM).
 *
 * Returns: (transfer none) (nullable): the custom size string
 */
const gchar *
ai_image_request_get_custom_size(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->custom_size;
}

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
){
    g_return_if_fail(self != NULL);

    g_free(self->custom_size);
    self->custom_size = g_strdup(custom_size);
    if (custom_size != NULL)
    {
        self->size = AI_IMAGE_SIZE_CUSTOM;
    }
}

/**
 * ai_image_request_get_quality:
 * @self: an #AiImageRequest
 *
 * Gets the image quality setting.
 *
 * Returns: the #AiImageQuality
 */
AiImageQuality
ai_image_request_get_quality(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_QUALITY_AUTO);

    return self->quality;
}

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
){
    g_return_if_fail(self != NULL);

    self->quality = quality;
}

/**
 * ai_image_request_get_style:
 * @self: an #AiImageRequest
 *
 * Gets the image style setting.
 *
 * Returns: the #AiImageStyle
 */
AiImageStyle
ai_image_request_get_style(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_STYLE_AUTO);

    return self->style;
}

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
){
    g_return_if_fail(self != NULL);

    self->style = style;
}

/**
 * ai_image_request_get_count:
 * @self: an #AiImageRequest
 *
 * Gets the number of images to generate.
 *
 * Returns: the count (default 1)
 */
gint
ai_image_request_get_count(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, 1);

    return self->count;
}

/**
 * ai_image_request_set_count:
 * @self: an #AiImageRequest
 * @count: the number of images to generate (1-10)
 *
 * Sets the number of images to generate. The count is clamped to 1-10.
 */
void
ai_image_request_set_count(
    AiImageRequest *self,
    gint            count
){
    g_return_if_fail(self != NULL);

    self->count = CLAMP(count, 1, 10);
}

/**
 * ai_image_request_get_response_format:
 * @self: an #AiImageRequest
 *
 * Gets the response format setting.
 *
 * Returns: the #AiImageResponseFormat
 */
AiImageResponseFormat
ai_image_request_get_response_format(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_RESPONSE_URL);

    return self->response_format;
}

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
){
    g_return_if_fail(self != NULL);

    self->response_format = format;
}

/**
 * ai_image_request_get_user:
 * @self: an #AiImageRequest
 *
 * Gets the user identifier for abuse tracking.
 *
 * Returns: (transfer none) (nullable): the user identifier
 */
const gchar *
ai_image_request_get_user(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->user;
}

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
){
    g_return_if_fail(self != NULL);

    g_free(self->user);
    self->user = g_strdup(user);
}
