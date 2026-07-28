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

    /* Prompt shaping */
    gchar                *negative_prompt;
    gchar                *system_instruction;

    /* Operation */
    AiImageOperation      operation;

    /* Geometry.  `aspect_ratio` is deliberately independent of `size`:
     * the OpenAI family thinks in pixel dimensions while the Gemini
     * family thinks in ratio-plus-tier, and a request may be aimed at
     * either. */
    gchar                *aspect_ratio;
    AiImageResolution     resolution;

    /* Appearance */
    gchar                *style_preset;
    AiImageBackground     background;

    /* Output encoding */
    AiImageFormat         output_format;
    gint                  output_compression;   /* -1 => unset */

    /* Sampling.  Negative values mean "unset" throughout, so an untouched
     * request inherits each provider's own defaults. */
    gint64                seed;                 /* -1 => unset */
    gdouble               guidance_scale;       /* <0 => unset */
    gint                  steps;                /* -1 => unset */
    gdouble               strength;             /* <0 => unset */
    gdouble               temperature;          /* <0 => unset */
    gdouble               top_p;                /* <0 => unset */
    gint                  top_k;                /* -1 => unset */

    /* Safety and policy */
    AiImageModeration       moderation;
    AiImagePersonGeneration person_generation;
    AiTriState              watermark;
    AiTriState              enhance_prompt;
    gchar                  *language;

    /* Editing */
    AiImageFidelity       input_fidelity;
    gint                  partial_images;       /* -1 => unset */

    /* Reference images, in caller order.  Ordering is significant: the
     * providers that accept several have no other way to correlate a
     * reference with the part of the prompt describing it. */
    GList                *reference_images;     /* AiImage*, owned */
    AiImage              *mask;

    /* Verbatim passthrough, spliced into the request body */
    GHashTable           *extras;               /* gchar* -> GVariant* */
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

    self->operation = AI_IMAGE_OPERATION_GENERATE;
    self->resolution = AI_IMAGE_RESOLUTION_AUTO;
    self->background = AI_IMAGE_BACKGROUND_AUTO;
    self->output_format = AI_IMAGE_FORMAT_AUTO;
    self->moderation = AI_IMAGE_MODERATION_AUTO;
    self->person_generation = AI_IMAGE_PERSON_GENERATION_DEFAULT;
    self->watermark = AI_TRI_UNSET;
    self->enhance_prompt = AI_TRI_UNSET;
    self->input_fidelity = AI_IMAGE_FIDELITY_AUTO;

    /* Every numeric knob starts "unset" so that a request nobody has
     * touched serialises to nothing at all, leaving each provider on its
     * own defaults rather than on ai-glib's guess at them. */
    self->output_compression = -1;
    self->seed = -1;
    self->guidance_scale = -1.0;
    self->steps = -1;
    self->strength = -1.0;
    self->temperature = -1.0;
    self->top_p = -1.0;
    self->top_k = -1;
    self->partial_images = -1;

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

    copy->negative_prompt = g_strdup(self->negative_prompt);
    copy->system_instruction = g_strdup(self->system_instruction);
    copy->operation = self->operation;
    copy->aspect_ratio = g_strdup(self->aspect_ratio);
    copy->resolution = self->resolution;
    copy->style_preset = g_strdup(self->style_preset);
    copy->background = self->background;
    copy->output_format = self->output_format;
    copy->output_compression = self->output_compression;
    copy->seed = self->seed;
    copy->guidance_scale = self->guidance_scale;
    copy->steps = self->steps;
    copy->strength = self->strength;
    copy->temperature = self->temperature;
    copy->top_p = self->top_p;
    copy->top_k = self->top_k;
    copy->moderation = self->moderation;
    copy->person_generation = self->person_generation;
    copy->watermark = self->watermark;
    copy->enhance_prompt = self->enhance_prompt;
    copy->language = g_strdup(self->language);
    copy->input_fidelity = self->input_fidelity;
    copy->partial_images = self->partial_images;

    /* Deep-copy the reference list.  ai_image_copy() shares the underlying
     * GBytes, so this duplicates the bookkeeping, not the payloads. */
    if (self->reference_images != NULL)
    {
        GList *iter;

        for (iter = self->reference_images; iter != NULL; iter = iter->next)
        {
            copy->reference_images = g_list_append(
                copy->reference_images, ai_image_copy((AiImage *)iter->data));
        }
    }

    copy->mask = ai_image_copy(self->mask);

    if (self->extras != NULL)
    {
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        copy->extras = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);

        g_hash_table_iter_init(&iter, self->extras);
        while (g_hash_table_iter_next(&iter, &key, &value))
        {
            g_hash_table_insert(copy->extras, g_strdup((const gchar *)key),
                                g_variant_ref((GVariant *)value));
        }
    }

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

    g_clear_pointer(&self->negative_prompt, g_free);
    g_clear_pointer(&self->system_instruction, g_free);
    g_clear_pointer(&self->aspect_ratio, g_free);
    g_clear_pointer(&self->style_preset, g_free);
    g_clear_pointer(&self->language, g_free);

    g_list_free_full(self->reference_images, (GDestroyNotify)ai_image_free);
    self->reference_images = NULL;

    g_clear_pointer(&self->mask, ai_image_free);
    g_clear_pointer(&self->extras, g_hash_table_unref);

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

/*
 * ----------------------------------------------------------------------
 * Prompt shaping
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_negative_prompt:
 * @self: an #AiImageRequest
 *
 * Gets the negative prompt.
 *
 * Returns: (transfer none) (nullable): the negative prompt, or %NULL
 */
const gchar *
ai_image_request_get_negative_prompt(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->negative_prompt;
}

/**
 * ai_image_request_set_negative_prompt:
 * @self: an #AiImageRequest
 * @negative_prompt: (nullable): what to steer away from, or %NULL to clear
 *
 * Sets a description of what should *not* appear in the image.
 *
 * Only some backends expose this as a first-class parameter (Imagen does;
 * the OpenAI and Gemini chat-shaped endpoints do not).  Requires
 * %AI_IMAGE_CAP_NEGATIVE_PROMPT.
 */
void
ai_image_request_set_negative_prompt(
    AiImageRequest *self,
    const gchar    *negative_prompt
){
    g_return_if_fail(self != NULL);

    g_free(self->negative_prompt);
    self->negative_prompt = g_strdup(negative_prompt);
}

/**
 * ai_image_request_get_system_instruction:
 * @self: an #AiImageRequest
 *
 * Gets the system instruction.
 *
 * Returns: (transfer none) (nullable): the system instruction, or %NULL
 */
const gchar *
ai_image_request_get_system_instruction(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->system_instruction;
}

/**
 * ai_image_request_set_system_instruction:
 * @self: an #AiImageRequest
 * @system_instruction: (nullable): the instruction, or %NULL to clear
 *
 * Sets a system-level instruction for the generation.
 *
 * This only exists on providers whose image generation is a chat call
 * underneath -- Gemini's Nano Banana models route through
 * `:generateContent` and therefore accept a `systemInstruction`.  Purely
 * generative endpoints have nowhere to put it.
 */
void
ai_image_request_set_system_instruction(
    AiImageRequest *self,
    const gchar    *system_instruction
){
    g_return_if_fail(self != NULL);

    g_free(self->system_instruction);
    self->system_instruction = g_strdup(system_instruction);
}

/*
 * ----------------------------------------------------------------------
 * Operation
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_operation:
 * @self: an #AiImageRequest
 *
 * Gets the requested operation.
 *
 * Returns: the #AiImageOperation
 */
AiImageOperation
ai_image_request_get_operation(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_OPERATION_GENERATE);

    return self->operation;
}

/**
 * ai_image_request_set_operation:
 * @self: an #AiImageRequest
 * @operation: the #AiImageOperation to perform
 *
 * Sets what the provider is being asked to do.
 *
 * Providers dispatch on this to pick an endpoint, so it is not merely
 * advisory: %AI_IMAGE_OPERATION_EDIT sends an OpenAI request to
 * `/v1/images/edits` rather than `/v1/images/generations`.
 */
void
ai_image_request_set_operation(
    AiImageRequest   *self,
    AiImageOperation  operation
){
    g_return_if_fail(self != NULL);

    self->operation = operation;
}

/*
 * ----------------------------------------------------------------------
 * Geometry
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_aspect_ratio:
 * @self: an #AiImageRequest
 *
 * Gets the aspect ratio.
 *
 * Returns: (transfer none) (nullable): the aspect ratio, or %NULL
 */
const gchar *
ai_image_request_get_aspect_ratio(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->aspect_ratio;
}

/**
 * ai_image_request_set_aspect_ratio:
 * @self: an #AiImageRequest
 * @aspect_ratio: (nullable): a ratio such as `"16:9"`, or %NULL to clear
 *
 * Sets the aspect ratio of the generated image.
 *
 * This is independent of ai_image_request_set_size(): the OpenAI family
 * expresses geometry as explicit pixel dimensions, while the Gemini family
 * expresses it as a ratio plus a resolution tier.  Setting one does not
 * disturb the other, so a single request can be aimed at either without
 * the caller knowing which is in play.  Requires
 * %AI_IMAGE_CAP_ASPECT_RATIO.
 */
void
ai_image_request_set_aspect_ratio(
    AiImageRequest *self,
    const gchar    *aspect_ratio
){
    g_return_if_fail(self != NULL);

    g_free(self->aspect_ratio);
    self->aspect_ratio = g_strdup(aspect_ratio);
}

/**
 * ai_image_request_get_resolution:
 * @self: an #AiImageRequest
 *
 * Gets the resolution tier.
 *
 * Returns: the #AiImageResolution
 */
AiImageResolution
ai_image_request_get_resolution(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_RESOLUTION_AUTO);

    return self->resolution;
}

/**
 * ai_image_request_set_resolution:
 * @self: an #AiImageRequest
 * @resolution: the #AiImageResolution tier
 *
 * Sets the resolution tier, orthogonally to the aspect ratio.
 *
 * Requires %AI_IMAGE_CAP_RESOLUTION_TIER; among the supported models only
 * Nano Banana Pro and Imagen honour tiers above 1K.
 */
void
ai_image_request_set_resolution(
    AiImageRequest    *self,
    AiImageResolution  resolution
){
    g_return_if_fail(self != NULL);

    self->resolution = resolution;
}

/*
 * ----------------------------------------------------------------------
 * Appearance
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_style_preset:
 * @self: an #AiImageRequest
 *
 * Gets the free-form style preset.
 *
 * Returns: (transfer none) (nullable): the style preset, or %NULL
 */
const gchar *
ai_image_request_get_style_preset(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->style_preset;
}

/**
 * ai_image_request_set_style_preset:
 * @self: an #AiImageRequest
 * @style_preset: (nullable): a provider-defined preset name, or %NULL
 *
 * Sets a named style preset.
 *
 * #AiImageStyle covers the two values DALL-E 3 understands; this is the
 * open-ended counterpart for backends with their own preset vocabularies.
 */
void
ai_image_request_set_style_preset(
    AiImageRequest *self,
    const gchar    *style_preset
){
    g_return_if_fail(self != NULL);

    g_free(self->style_preset);
    self->style_preset = g_strdup(style_preset);
}

/**
 * ai_image_request_get_background:
 * @self: an #AiImageRequest
 *
 * Gets the background treatment.
 *
 * Returns: the #AiImageBackground
 */
AiImageBackground
ai_image_request_get_background(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_BACKGROUND_AUTO);

    return self->background;
}

/**
 * ai_image_request_set_background:
 * @self: an #AiImageRequest
 * @background: the #AiImageBackground treatment
 *
 * Sets whether the image should be rendered with a transparent background.
 *
 * Transparency needs an output format with an alpha channel, so pairing
 * %AI_IMAGE_BACKGROUND_TRANSPARENT with %AI_IMAGE_FORMAT_JPEG is
 * contradictory; ai_image_request_validate() reports it.  Requires
 * %AI_IMAGE_CAP_TRANSPARENCY.
 */
void
ai_image_request_set_background(
    AiImageRequest    *self,
    AiImageBackground  background
){
    g_return_if_fail(self != NULL);

    self->background = background;
}

/*
 * ----------------------------------------------------------------------
 * Output encoding
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_output_format:
 * @self: an #AiImageRequest
 *
 * Gets the requested output encoding.
 *
 * Returns: the #AiImageFormat
 */
AiImageFormat
ai_image_request_get_output_format(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_FORMAT_AUTO);

    return self->output_format;
}

/**
 * ai_image_request_set_output_format:
 * @self: an #AiImageRequest
 * @output_format: the #AiImageFormat to request
 *
 * Sets the encoding of the returned image.  Requires
 * %AI_IMAGE_CAP_OUTPUT_FORMAT.
 */
void
ai_image_request_set_output_format(
    AiImageRequest *self,
    AiImageFormat   output_format
){
    g_return_if_fail(self != NULL);

    self->output_format = output_format;
}

/**
 * ai_image_request_get_output_compression:
 * @self: an #AiImageRequest
 *
 * Gets the output compression level.
 *
 * Returns: the compression level 0-100, or -1 when unset
 */
gint
ai_image_request_get_output_compression(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1);

    return self->output_compression;
}

/**
 * ai_image_request_set_output_compression:
 * @self: an #AiImageRequest
 * @compression: compression level 0-100, or -1 to leave unset
 *
 * Sets the compression level for lossy output formats.
 *
 * Meaningless for %AI_IMAGE_FORMAT_PNG, and ignored for it.  Values
 * outside 0-100 (other than the -1 sentinel) are clamped.
 */
void
ai_image_request_set_output_compression(
    AiImageRequest *self,
    gint            compression
){
    g_return_if_fail(self != NULL);

    if (compression < 0)
    {
        self->output_compression = -1;
        return;
    }

    self->output_compression = MIN(compression, 100);
}

/*
 * ----------------------------------------------------------------------
 * Sampling
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_seed:
 * @self: an #AiImageRequest
 *
 * Gets the sampling seed.
 *
 * Returns: the seed, or -1 when unset
 */
gint64
ai_image_request_get_seed(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1);

    return self->seed;
}

/**
 * ai_image_request_set_seed:
 * @self: an #AiImageRequest
 * @seed: the seed, or -1 to leave unset
 *
 * Sets the sampling seed, for reproducible generations.
 *
 * Reproducibility is best-effort: providers only guarantee it while the
 * model and the rest of the parameters are held fixed.  Requires
 * %AI_IMAGE_CAP_SEED.
 */
void
ai_image_request_set_seed(
    AiImageRequest *self,
    gint64          seed
){
    g_return_if_fail(self != NULL);

    self->seed = seed < 0 ? -1 : seed;
}

/**
 * ai_image_request_get_guidance_scale:
 * @self: an #AiImageRequest
 *
 * Gets the guidance scale.
 *
 * Returns: the guidance scale, or a negative value when unset
 */
gdouble
ai_image_request_get_guidance_scale(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1.0);

    return self->guidance_scale;
}

/**
 * ai_image_request_set_guidance_scale:
 * @self: an #AiImageRequest
 * @guidance_scale: the guidance scale, or a negative value to leave unset
 *
 * Sets how strictly the sampler should follow the prompt.
 */
void
ai_image_request_set_guidance_scale(
    AiImageRequest *self,
    gdouble         guidance_scale
){
    g_return_if_fail(self != NULL);

    self->guidance_scale = guidance_scale < 0.0 ? -1.0 : guidance_scale;
}

/**
 * ai_image_request_get_steps:
 * @self: an #AiImageRequest
 *
 * Gets the sampling step count.
 *
 * Returns: the step count, or -1 when unset
 */
gint
ai_image_request_get_steps(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1);

    return self->steps;
}

/**
 * ai_image_request_set_steps:
 * @self: an #AiImageRequest
 * @steps: the step count, or -1 to leave unset
 *
 * Sets the number of sampling steps.
 */
void
ai_image_request_set_steps(
    AiImageRequest *self,
    gint            steps
){
    g_return_if_fail(self != NULL);

    self->steps = steps < 0 ? -1 : steps;
}

/**
 * ai_image_request_get_strength:
 * @self: an #AiImageRequest
 *
 * Gets the image-to-image strength.
 *
 * Returns: the strength, or a negative value when unset
 */
gdouble
ai_image_request_get_strength(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1.0);

    return self->strength;
}

/**
 * ai_image_request_set_strength:
 * @self: an #AiImageRequest
 * @strength: 0.0-1.0, or a negative value to leave unset
 *
 * Sets how far an edit may depart from its reference image.
 *
 * Higher values grant the model more licence.  Only meaningful when
 * reference images are present.
 */
void
ai_image_request_set_strength(
    AiImageRequest *self,
    gdouble         strength
){
    g_return_if_fail(self != NULL);

    self->strength = strength < 0.0 ? -1.0 : MIN(strength, 1.0);
}

/**
 * ai_image_request_get_temperature:
 * @self: an #AiImageRequest
 *
 * Gets the sampling temperature.
 *
 * Returns: the temperature, or a negative value when unset
 */
gdouble
ai_image_request_get_temperature(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1.0);

    return self->temperature;
}

/**
 * ai_image_request_set_temperature:
 * @self: an #AiImageRequest
 * @temperature: the temperature, or a negative value to leave unset
 *
 * Sets the sampling temperature.
 *
 * Only meaningful on providers whose image generation is a chat call
 * underneath.  Requires %AI_IMAGE_CAP_SAMPLING.
 */
void
ai_image_request_set_temperature(
    AiImageRequest *self,
    gdouble         temperature
){
    g_return_if_fail(self != NULL);

    self->temperature = temperature < 0.0 ? -1.0 : temperature;
}

/**
 * ai_image_request_get_top_p:
 * @self: an #AiImageRequest
 *
 * Gets the nucleus-sampling threshold.
 *
 * Returns: the top-p value, or a negative value when unset
 */
gdouble
ai_image_request_get_top_p(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1.0);

    return self->top_p;
}

/**
 * ai_image_request_set_top_p:
 * @self: an #AiImageRequest
 * @top_p: the top-p value, or a negative value to leave unset
 *
 * Sets the nucleus-sampling threshold.  Requires %AI_IMAGE_CAP_SAMPLING.
 */
void
ai_image_request_set_top_p(
    AiImageRequest *self,
    gdouble         top_p
){
    g_return_if_fail(self != NULL);

    self->top_p = top_p < 0.0 ? -1.0 : top_p;
}

/**
 * ai_image_request_get_top_k:
 * @self: an #AiImageRequest
 *
 * Gets the top-k sampling cutoff.
 *
 * Returns: the top-k value, or -1 when unset
 */
gint
ai_image_request_get_top_k(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1);

    return self->top_k;
}

/**
 * ai_image_request_set_top_k:
 * @self: an #AiImageRequest
 * @top_k: the top-k value, or -1 to leave unset
 *
 * Sets the top-k sampling cutoff.  Requires %AI_IMAGE_CAP_SAMPLING.
 */
void
ai_image_request_set_top_k(
    AiImageRequest *self,
    gint            top_k
){
    g_return_if_fail(self != NULL);

    self->top_k = top_k < 0 ? -1 : top_k;
}

/*
 * ----------------------------------------------------------------------
 * Safety and policy
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_moderation:
 * @self: an #AiImageRequest
 *
 * Gets the requested moderation level.
 *
 * Returns: the #AiImageModeration
 */
AiImageModeration
ai_image_request_get_moderation(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_MODERATION_AUTO);

    return self->moderation;
}

/**
 * ai_image_request_set_moderation:
 * @self: an #AiImageRequest
 * @moderation: the #AiImageModeration level
 *
 * Requests a content-filtering level.
 *
 * This is a request, not a guarantee -- every provider enforces a floor
 * below which it will not go, and a provider with no such control ignores
 * this entirely.  Requires %AI_IMAGE_CAP_SAFETY_CONTROL.
 */
void
ai_image_request_set_moderation(
    AiImageRequest    *self,
    AiImageModeration  moderation
){
    g_return_if_fail(self != NULL);

    self->moderation = moderation;
}

/**
 * ai_image_request_get_person_generation:
 * @self: an #AiImageRequest
 *
 * Gets the person-generation policy.
 *
 * Returns: the #AiImagePersonGeneration
 */
AiImagePersonGeneration
ai_image_request_get_person_generation(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_PERSON_GENERATION_DEFAULT);

    return self->person_generation;
}

/**
 * ai_image_request_set_person_generation:
 * @self: an #AiImageRequest
 * @person_generation: the #AiImagePersonGeneration policy
 *
 * Sets the policy for depicting people.  Requires
 * %AI_IMAGE_CAP_SAFETY_CONTROL.
 */
void
ai_image_request_set_person_generation(
    AiImageRequest          *self,
    AiImagePersonGeneration  person_generation
){
    g_return_if_fail(self != NULL);

    self->person_generation = person_generation;
}

/**
 * ai_image_request_get_watermark:
 * @self: an #AiImageRequest
 *
 * Gets the watermark preference.
 *
 * Returns: the #AiTriState watermark preference
 */
AiTriState
ai_image_request_get_watermark(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_TRI_UNSET);

    return self->watermark;
}

/**
 * ai_image_request_set_watermark:
 * @self: an #AiImageRequest
 * @watermark: whether to embed a provenance watermark
 *
 * Sets whether the provider should embed a watermark.
 *
 * Some providers watermark unconditionally regardless of this setting.
 * Requires %AI_IMAGE_CAP_WATERMARK_CONTROL.
 */
void
ai_image_request_set_watermark(
    AiImageRequest *self,
    AiTriState      watermark
){
    g_return_if_fail(self != NULL);

    self->watermark = watermark;
}

/**
 * ai_image_request_get_enhance_prompt:
 * @self: an #AiImageRequest
 *
 * Gets the prompt-enhancement preference.
 *
 * Returns: the #AiTriState preference
 */
AiTriState
ai_image_request_get_enhance_prompt(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_TRI_UNSET);

    return self->enhance_prompt;
}

/**
 * ai_image_request_set_enhance_prompt:
 * @self: an #AiImageRequest
 * @enhance_prompt: whether to let the provider rewrite the prompt
 *
 * Sets whether the provider may expand the prompt before generating.
 *
 * When enabled, the rewritten prompt is usually reported back via
 * ai_generated_image_get_revised_prompt().
 */
void
ai_image_request_set_enhance_prompt(
    AiImageRequest *self,
    AiTriState      enhance_prompt
){
    g_return_if_fail(self != NULL);

    self->enhance_prompt = enhance_prompt;
}

/**
 * ai_image_request_get_language:
 * @self: an #AiImageRequest
 *
 * Gets the prompt language hint.
 *
 * Returns: (transfer none) (nullable): the language code, or %NULL
 */
const gchar *
ai_image_request_get_language(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->language;
}

/**
 * ai_image_request_set_language:
 * @self: an #AiImageRequest
 * @language: (nullable): a language code such as `"en"`, or %NULL to clear
 *
 * Hints at the language the prompt is written in.
 */
void
ai_image_request_set_language(
    AiImageRequest *self,
    const gchar    *language
){
    g_return_if_fail(self != NULL);

    g_free(self->language);
    self->language = g_strdup(language);
}

/*
 * ----------------------------------------------------------------------
 * Editing
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_get_input_fidelity:
 * @self: an #AiImageRequest
 *
 * Gets the input-fidelity setting.
 *
 * Returns: the #AiImageFidelity
 */
AiImageFidelity
ai_image_request_get_input_fidelity(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_FIDELITY_AUTO);

    return self->input_fidelity;
}

/**
 * ai_image_request_set_input_fidelity:
 * @self: an #AiImageRequest
 * @input_fidelity: the #AiImageFidelity to request
 *
 * Sets how closely an edit should preserve its input image.
 *
 * %AI_IMAGE_FIDELITY_HIGH is what you want when editing a face or a logo
 * that must survive the round trip recognisably.
 */
void
ai_image_request_set_input_fidelity(
    AiImageRequest  *self,
    AiImageFidelity  input_fidelity
){
    g_return_if_fail(self != NULL);

    self->input_fidelity = input_fidelity;
}

/**
 * ai_image_request_get_partial_images:
 * @self: an #AiImageRequest
 *
 * Gets the number of partial previews requested.
 *
 * Returns: the count, or -1 when unset
 */
gint
ai_image_request_get_partial_images(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, -1);

    return self->partial_images;
}

/**
 * ai_image_request_set_partial_images:
 * @self: an #AiImageRequest
 * @partial_images: how many previews to stream, or -1 to leave unset
 *
 * Asks the provider to stream partial previews while generating.
 *
 * Each preview arrives as an #AiImageGenerator::partial-image signal.
 * Requires %AI_IMAGE_CAP_PARTIAL_STREAMING.
 */
void
ai_image_request_set_partial_images(
    AiImageRequest *self,
    gint            partial_images
){
    g_return_if_fail(self != NULL);

    self->partial_images = partial_images < 0 ? -1 : partial_images;
}

/*
 * ----------------------------------------------------------------------
 * Reference images
 * ----------------------------------------------------------------------
 */

/**
 * ai_image_request_add_reference_image:
 * @self: an #AiImageRequest
 * @image: (transfer none): the reference to append
 *
 * Appends a reference image.
 *
 * The request takes a copy, so the caller keeps ownership of @image.
 * Order is preserved and significant: providers accepting several
 * references have no positional convention of their own, so ai-glib sends
 * them in the order added and folds any #AiImage:role labels into the
 * prompt text.
 *
 * Requires %AI_IMAGE_CAP_REFERENCE_IMAGES, and more than one additionally
 * requires %AI_IMAGE_CAP_MULTI_REFERENCE.
 */
void
ai_image_request_add_reference_image(
    AiImageRequest *self,
    AiImage        *image
){
    g_return_if_fail(self != NULL);
    g_return_if_fail(image != NULL);

    self->reference_images = g_list_append(self->reference_images,
                                           ai_image_copy(image));
}

/**
 * ai_image_request_add_reference_file:
 * @self: an #AiImageRequest
 * @path: path to an image file
 * @role: (nullable): a role label such as `"style"`, or %NULL
 * @error: (out) (optional): return location for a #GError
 *
 * Reads @path and appends it as a reference image.
 *
 * Convenience wrapper over ai_image_new_from_file() plus
 * ai_image_request_add_reference_image().
 *
 * Returns: %TRUE on success
 */
gboolean
ai_image_request_add_reference_file(
    AiImageRequest  *self,
    const gchar     *path,
    const gchar     *role,
    GError         **error
){
    AiImage *image;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    image = ai_image_new_from_file(path, error);
    if (image == NULL)
    {
        return FALSE;
    }

    if (role != NULL)
    {
        ai_image_set_role(image, role);
    }

    /* Hand the freshly-built image straight to the list rather than going
     * through add_reference_image(), which would copy it needlessly. */
    self->reference_images = g_list_append(self->reference_images, image);

    return TRUE;
}

/**
 * ai_image_request_get_reference_images:
 * @self: an #AiImageRequest
 *
 * Gets the reference images, in the order they were added.
 *
 * Returns: (transfer none) (element-type AiImage) (nullable): the
 *   references, or %NULL if there are none
 */
GList *
ai_image_request_get_reference_images(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->reference_images;
}

/**
 * ai_image_request_get_reference_image_count:
 * @self: an #AiImageRequest
 *
 * Gets the number of reference images.
 *
 * Returns: the reference count
 */
guint
ai_image_request_get_reference_image_count(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return g_list_length(self->reference_images);
}

/**
 * ai_image_request_clear_reference_images:
 * @self: an #AiImageRequest
 *
 * Removes every reference image.
 */
void
ai_image_request_clear_reference_images(AiImageRequest *self)
{
    g_return_if_fail(self != NULL);

    g_list_free_full(self->reference_images, (GDestroyNotify)ai_image_free);
    self->reference_images = NULL;
}

/**
 * ai_image_request_get_mask:
 * @self: an #AiImageRequest
 *
 * Gets the edit mask.
 *
 * Returns: (transfer none) (nullable): the mask, or %NULL
 */
AiImage *
ai_image_request_get_mask(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->mask;
}

/**
 * ai_image_request_set_mask:
 * @self: an #AiImageRequest
 * @mask: (transfer none) (nullable): the mask, or %NULL to clear
 *
 * Sets the mask marking which region of a reference image to replace.
 *
 * The transparent areas of the mask are the areas that get regenerated.
 * The request takes a copy.  Requires %AI_IMAGE_CAP_MASK.
 */
void
ai_image_request_set_mask(
    AiImageRequest *self,
    AiImage        *mask
){
    g_return_if_fail(self != NULL);

    g_clear_pointer(&self->mask, ai_image_free);
    self->mask = ai_image_copy(mask);
}

/*
 * ----------------------------------------------------------------------
 * Provider-specific passthrough
 * ----------------------------------------------------------------------
 */

/*
 * Lazily create the extras table.  Most requests never use it, and an
 * always-allocated GHashTable on a type that gets copied per retry is a
 * waste worth avoiding.
 */
static GHashTable *
ai_image_request_ensure_extras(AiImageRequest *self)
{
    if (self->extras == NULL)
    {
        self->extras = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    }

    return self->extras;
}

/**
 * ai_image_request_set_extra:
 * @self: an #AiImageRequest
 * @key: the request-body member name
 * @value: (transfer none) (nullable): the value, or %NULL to remove @key
 *
 * Sets a provider-specific parameter, spliced verbatim into the request
 * body at the top level.
 *
 * This is the escape hatch for anything ai-glib does not model.  Image
 * APIs gain parameters faster than a binding can track them, and without
 * this a caller would have to wait for a release to use one.  Nothing is
 * validated: an unknown key reaches the provider exactly as written, and
 * whatever the provider says about it comes back as the request error.
 *
 * Extras are applied last and therefore override any modelled parameter
 * that serialises to the same member.
 */
void
ai_image_request_set_extra(
    AiImageRequest *self,
    const gchar    *key,
    GVariant       *value
){
    g_return_if_fail(self != NULL);
    g_return_if_fail(key != NULL);

    if (value == NULL)
    {
        if (self->extras != NULL)
        {
            g_hash_table_remove(self->extras, key);
        }
        return;
    }

    /* Sink here so callers can pass a freshly-constructed floating variant
     * (g_variant_new_string(), etc.) without an explicit ref dance. */
    g_hash_table_insert(ai_image_request_ensure_extras(self),
                        g_strdup(key),
                        g_variant_ref_sink(value));
}

/**
 * ai_image_request_set_extra_string:
 * @self: an #AiImageRequest
 * @key: the request-body member name
 * @value: (nullable): the string value, or %NULL to remove @key
 *
 * Convenience wrapper over ai_image_request_set_extra() for string values,
 * which is what command-line and scripting front-ends nearly always have.
 */
void
ai_image_request_set_extra_string(
    AiImageRequest *self,
    const gchar    *key,
    const gchar    *value
){
    g_return_if_fail(self != NULL);
    g_return_if_fail(key != NULL);

    if (value == NULL)
    {
        ai_image_request_set_extra(self, key, NULL);
        return;
    }

    ai_image_request_set_extra(self, key, g_variant_new_string(value));
}

/**
 * ai_image_request_get_extra:
 * @self: an #AiImageRequest
 * @key: the request-body member name
 *
 * Gets a previously-set provider-specific parameter.
 *
 * Returns: (transfer none) (nullable): the value, or %NULL if unset
 */
GVariant *
ai_image_request_get_extra(
    const AiImageRequest *self,
    const gchar          *key
){
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(key != NULL, NULL);

    if (self->extras == NULL)
    {
        return NULL;
    }

    return g_hash_table_lookup(self->extras, key);
}

/**
 * ai_image_request_get_extras:
 * @self: an #AiImageRequest
 *
 * Gets every provider-specific parameter.
 *
 * Returns: (transfer none) (element-type utf8 GLib.Variant) (nullable):
 *   the extras table, or %NULL if none have been set
 */
GHashTable *
ai_image_request_get_extras(const AiImageRequest *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->extras;
}

/**
 * ai_image_request_clear_extras:
 * @self: an #AiImageRequest
 *
 * Removes every provider-specific parameter.
 */
void
ai_image_request_clear_extras(AiImageRequest *self)
{
    g_return_if_fail(self != NULL);

    g_clear_pointer(&self->extras, g_hash_table_unref);
}
