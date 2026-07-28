/*
 * ai-image-capabilities.c - What an image model can actually do
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "core/ai-image-capabilities.h"

/*
 * Private structure for the AiImageModelInfo boxed type.
 *
 * `sizes` and `aspect_ratios` are NULL-terminated string vectors so a
 * provider can point them straight at a static const table without
 * allocating; ai_image_model_info_set_*() copies, so the boxed type always
 * owns what it frees.
 */
struct _AiImageModelInfo
{
    gchar               *id;
    gchar               *display_name;
    AiProviderType       provider_type;
    AiImageCapabilities  capabilities;
    guint                max_count;
    guint                max_reference_images;
    gchar              **sizes;
    gchar              **aspect_ratios;
    gchar               *notes;
};

G_DEFINE_BOXED_TYPE(AiImageModelInfo, ai_image_model_info,
                    ai_image_model_info_copy, ai_image_model_info_free)

/*
 * GType registration for AiImageCapabilities.
 *
 * Registered as flags rather than an enum so bindings present it as a set.
 */
GType
ai_image_capabilities_get_type(void)
{
    static GType capabilities_type = 0;

    if (g_once_init_enter(&capabilities_type))
    {
        static const GFlagsValue values[] = {
            { AI_IMAGE_CAP_NONE, "AI_IMAGE_CAP_NONE", "none" },
            { AI_IMAGE_CAP_REFERENCE_IMAGES, "AI_IMAGE_CAP_REFERENCE_IMAGES", "reference-images" },
            { AI_IMAGE_CAP_MULTI_REFERENCE, "AI_IMAGE_CAP_MULTI_REFERENCE", "multi-reference" },
            { AI_IMAGE_CAP_MASK, "AI_IMAGE_CAP_MASK", "mask" },
            { AI_IMAGE_CAP_NEGATIVE_PROMPT, "AI_IMAGE_CAP_NEGATIVE_PROMPT", "negative-prompt" },
            { AI_IMAGE_CAP_SEED, "AI_IMAGE_CAP_SEED", "seed" },
            { AI_IMAGE_CAP_ASPECT_RATIO, "AI_IMAGE_CAP_ASPECT_RATIO", "aspect-ratio" },
            { AI_IMAGE_CAP_PIXEL_SIZE, "AI_IMAGE_CAP_PIXEL_SIZE", "pixel-size" },
            { AI_IMAGE_CAP_RESOLUTION_TIER, "AI_IMAGE_CAP_RESOLUTION_TIER", "resolution-tier" },
            { AI_IMAGE_CAP_TRANSPARENCY, "AI_IMAGE_CAP_TRANSPARENCY", "transparency" },
            { AI_IMAGE_CAP_OUTPUT_FORMAT, "AI_IMAGE_CAP_OUTPUT_FORMAT", "output-format" },
            { AI_IMAGE_CAP_QUALITY, "AI_IMAGE_CAP_QUALITY", "quality" },
            { AI_IMAGE_CAP_STYLE, "AI_IMAGE_CAP_STYLE", "style" },
            { AI_IMAGE_CAP_MULTI_COUNT, "AI_IMAGE_CAP_MULTI_COUNT", "multi-count" },
            { AI_IMAGE_CAP_VARIATION, "AI_IMAGE_CAP_VARIATION", "variation" },
            { AI_IMAGE_CAP_UPSCALE, "AI_IMAGE_CAP_UPSCALE", "upscale" },
            { AI_IMAGE_CAP_PARTIAL_STREAMING, "AI_IMAGE_CAP_PARTIAL_STREAMING", "partial-streaming" },
            { AI_IMAGE_CAP_SAFETY_CONTROL, "AI_IMAGE_CAP_SAFETY_CONTROL", "safety-control" },
            { AI_IMAGE_CAP_WATERMARK_CONTROL, "AI_IMAGE_CAP_WATERMARK_CONTROL", "watermark-control" },
            { AI_IMAGE_CAP_URL_RESPONSE, "AI_IMAGE_CAP_URL_RESPONSE", "url-response" },
            { AI_IMAGE_CAP_SAMPLING, "AI_IMAGE_CAP_SAMPLING", "sampling" },
            { AI_IMAGE_CAP_PROMPT_ENHANCEMENT, "AI_IMAGE_CAP_PROMPT_ENHANCEMENT", "prompt-enhancement" },
            { AI_IMAGE_CAP_LANGUAGE, "AI_IMAGE_CAP_LANGUAGE", "language" },
            { AI_IMAGE_CAP_INPUT_FIDELITY, "AI_IMAGE_CAP_INPUT_FIDELITY", "input-fidelity" },
            { 0, NULL, NULL }
        };

        GType type = g_flags_register_static("AiImageCapabilities", values);
        g_once_init_leave(&capabilities_type, type);
    }

    return capabilities_type;
}

/*
 * GType registration for AiImageValidateFlags.
 */
GType
ai_image_validate_flags_get_type(void)
{
    static GType validate_flags_type = 0;

    if (g_once_init_enter(&validate_flags_type))
    {
        static const GFlagsValue values[] = {
            { AI_IMAGE_VALIDATE_NONE, "AI_IMAGE_VALIDATE_NONE", "none" },
            { AI_IMAGE_VALIDATE_STRICT, "AI_IMAGE_VALIDATE_STRICT", "strict" },
            { 0, NULL, NULL }
        };

        GType type = g_flags_register_static("AiImageValidateFlags", values);
        g_once_init_leave(&validate_flags_type, type);
    }

    return validate_flags_type;
}

/**
 * ai_image_model_info_new:
 * @id: the model identifier as the provider's API spells it
 * @display_name: (nullable): a human-readable name, or %NULL to reuse @id
 * @provider_type: which provider serves this model
 * @capabilities: the parameters this model honours
 *
 * Creates a new #AiImageModelInfo.
 *
 * Limits default to permissive (one image, no references); a provider
 * tightens them with ai_image_model_info_set_max_count() and
 * ai_image_model_info_set_max_reference_images().
 *
 * Returns: (transfer full): a new #AiImageModelInfo
 */
AiImageModelInfo *
ai_image_model_info_new(
    const gchar         *id,
    const gchar         *display_name,
    AiProviderType       provider_type,
    AiImageCapabilities  capabilities
){
    AiImageModelInfo *self;

    g_return_val_if_fail(id != NULL, NULL);

    self = g_slice_new0(AiImageModelInfo);
    self->id = g_strdup(id);
    self->display_name = g_strdup(display_name != NULL ? display_name : id);
    self->provider_type = provider_type;
    self->capabilities = capabilities;
    self->max_count = 1;
    self->max_reference_images = 0;

    return self;
}

/**
 * ai_image_model_info_copy:
 * @self: (nullable): an #AiImageModelInfo
 *
 * Creates a deep copy of @self.
 *
 * Returns: (transfer full) (nullable): a copy, or %NULL if @self is %NULL
 */
AiImageModelInfo *
ai_image_model_info_copy(const AiImageModelInfo *self)
{
    AiImageModelInfo *copy;

    if (self == NULL)
    {
        return NULL;
    }

    copy = g_slice_new0(AiImageModelInfo);
    copy->id = g_strdup(self->id);
    copy->display_name = g_strdup(self->display_name);
    copy->provider_type = self->provider_type;
    copy->capabilities = self->capabilities;
    copy->max_count = self->max_count;
    copy->max_reference_images = self->max_reference_images;
    copy->sizes = g_strdupv(self->sizes);
    copy->aspect_ratios = g_strdupv(self->aspect_ratios);
    copy->notes = g_strdup(self->notes);

    return copy;
}

/**
 * ai_image_model_info_free:
 * @self: (nullable): an #AiImageModelInfo
 *
 * Frees @self.  Does nothing if @self is %NULL.
 */
void
ai_image_model_info_free(AiImageModelInfo *self)
{
    if (self == NULL)
    {
        return;
    }

    g_clear_pointer(&self->id, g_free);
    g_clear_pointer(&self->display_name, g_free);
    g_clear_pointer(&self->sizes, g_strfreev);
    g_clear_pointer(&self->aspect_ratios, g_strfreev);
    g_clear_pointer(&self->notes, g_free);

    g_slice_free(AiImageModelInfo, self);
}

/**
 * ai_image_model_info_get_id:
 * @self: an #AiImageModelInfo
 *
 * Gets the model identifier.
 *
 * Returns: (transfer none): the model id
 */
const gchar *
ai_image_model_info_get_id(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->id;
}

/**
 * ai_image_model_info_get_display_name:
 * @self: an #AiImageModelInfo
 *
 * Gets the human-readable model name.
 *
 * Returns: (transfer none): the display name
 */
const gchar *
ai_image_model_info_get_display_name(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->display_name;
}

/**
 * ai_image_model_info_get_provider_type:
 * @self: an #AiImageModelInfo
 *
 * Gets the provider that serves this model.
 *
 * Returns: the #AiProviderType
 */
AiProviderType
ai_image_model_info_get_provider_type(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, AI_PROVIDER_CLAUDE);

    return self->provider_type;
}

/**
 * ai_image_model_info_get_capabilities:
 * @self: an #AiImageModelInfo
 *
 * Gets the full capability set.
 *
 * Returns: the #AiImageCapabilities
 */
AiImageCapabilities
ai_image_model_info_get_capabilities(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, AI_IMAGE_CAP_NONE);

    return self->capabilities;
}

/**
 * ai_image_model_info_supports:
 * @self: an #AiImageModelInfo
 * @capability: the capability, or capabilities, to test for
 *
 * Tests whether the model supports @capability.
 *
 * When @capability names several bits, all of them must be present.
 *
 * Returns: %TRUE if every requested capability is supported
 */
gboolean
ai_image_model_info_supports(
    const AiImageModelInfo *self,
    AiImageCapabilities     capability
){
    g_return_val_if_fail(self != NULL, FALSE);

    return (self->capabilities & capability) == capability;
}

/**
 * ai_image_model_info_get_max_count:
 * @self: an #AiImageModelInfo
 *
 * Gets the greatest number of images obtainable from one request.
 *
 * Returns: the maximum count
 */
guint
ai_image_model_info_get_max_count(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, 1);

    return self->max_count;
}

/**
 * ai_image_model_info_set_max_count:
 * @self: an #AiImageModelInfo
 * @max_count: the maximum images per request
 *
 * Sets the greatest number of images obtainable from one request.
 */
void
ai_image_model_info_set_max_count(
    AiImageModelInfo *self,
    guint             max_count
){
    g_return_if_fail(self != NULL);

    self->max_count = MAX(max_count, 1);
}

/**
 * ai_image_model_info_get_max_reference_images:
 * @self: an #AiImageModelInfo
 *
 * Gets the greatest number of reference images accepted.
 *
 * Returns: the maximum reference count, or 0 when references are not
 *   supported at all
 */
guint
ai_image_model_info_get_max_reference_images(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->max_reference_images;
}

/**
 * ai_image_model_info_set_max_reference_images:
 * @self: an #AiImageModelInfo
 * @max_reference_images: the maximum reference images accepted
 *
 * Sets the greatest number of reference images accepted.
 */
void
ai_image_model_info_set_max_reference_images(
    AiImageModelInfo *self,
    guint             max_reference_images
){
    g_return_if_fail(self != NULL);

    self->max_reference_images = max_reference_images;
}

/**
 * ai_image_model_info_get_sizes:
 * @self: an #AiImageModelInfo
 *
 * Gets the pixel sizes this model accepts.
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): the
 *   supported sizes, or %NULL if the model does not take pixel sizes
 */
const gchar * const *
ai_image_model_info_get_sizes(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return (const gchar * const *)self->sizes;
}

/**
 * ai_image_model_info_set_sizes:
 * @self: an #AiImageModelInfo
 * @sizes: (array zero-terminated=1) (nullable): the supported sizes
 *
 * Sets the pixel sizes this model accepts.  The vector is copied.
 */
void
ai_image_model_info_set_sizes(
    AiImageModelInfo    *self,
    const gchar * const *sizes
){
    g_return_if_fail(self != NULL);

    g_clear_pointer(&self->sizes, g_strfreev);
    self->sizes = g_strdupv((gchar **)sizes);
}

/**
 * ai_image_model_info_get_aspect_ratios:
 * @self: an #AiImageModelInfo
 *
 * Gets the aspect ratios this model accepts.
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): the
 *   supported ratios, or %NULL if the model does not take aspect ratios
 */
const gchar * const *
ai_image_model_info_get_aspect_ratios(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return (const gchar * const *)self->aspect_ratios;
}

/**
 * ai_image_model_info_set_aspect_ratios:
 * @self: an #AiImageModelInfo
 * @aspect_ratios: (array zero-terminated=1) (nullable): the supported ratios
 *
 * Sets the aspect ratios this model accepts.  The vector is copied.
 */
void
ai_image_model_info_set_aspect_ratios(
    AiImageModelInfo    *self,
    const gchar * const *aspect_ratios
){
    g_return_if_fail(self != NULL);

    g_clear_pointer(&self->aspect_ratios, g_strfreev);
    self->aspect_ratios = g_strdupv((gchar **)aspect_ratios);
}

/**
 * ai_image_model_info_get_notes:
 * @self: an #AiImageModelInfo
 *
 * Gets the free-form notes about this model.
 *
 * Returns: (transfer none) (nullable): the notes, or %NULL
 */
const gchar *
ai_image_model_info_get_notes(const AiImageModelInfo *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->notes;
}

/**
 * ai_image_model_info_set_notes:
 * @self: an #AiImageModelInfo
 * @notes: (nullable): a short remark, or %NULL to clear
 *
 * Sets free-form notes, surfaced by `ai --list-image-models`.
 */
void
ai_image_model_info_set_notes(
    AiImageModelInfo *self,
    const gchar      *notes
){
    g_return_if_fail(self != NULL);

    g_free(self->notes);
    self->notes = g_strdup(notes);
}

/**
 * ai_image_capabilities_to_string:
 * @capabilities: an #AiImageCapabilities set
 *
 * Renders @capabilities as a comma-separated list of nicknames.
 *
 * Returns: (transfer full): the rendered list, or the empty string
 */
gchar *
ai_image_capabilities_to_string(AiImageCapabilities capabilities)
{
    g_autoptr(GString) out = NULL;
    GFlagsClass *klass;
    guint i;

    out = g_string_new(NULL);
    klass = g_type_class_ref(AI_TYPE_IMAGE_CAPABILITIES);

    for (i = 0; i < klass->n_values; i++)
    {
        const GFlagsValue *value = &klass->values[i];

        if (value->value == 0)
        {
            continue;
        }

        if ((capabilities & value->value) == (guint)value->value)
        {
            if (out->len > 0)
            {
                g_string_append_c(out, ',');
            }
            g_string_append(out, value->value_nick);
        }
    }

    g_type_class_unref(klass);

    return g_strdup(out->str);
}

/*
 * Report one unsupported parameter.
 *
 * Returns TRUE when the caller should go on to drop the parameter
 * (lenient mode), and FALSE when validation has failed outright (strict
 * mode), in which case *error has been set and the caller must return.
 */
static gboolean
ai_image_validate_reject(
    AiImageValidateFlags   flags,
    const gchar           *model_id,
    const gchar           *parameter,
    const gchar           *reason,
    GError               **error
){
    if ((flags & AI_IMAGE_VALIDATE_STRICT) != 0)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "Model '%s' does not support %s: %s",
                    model_id, parameter, reason);
        return FALSE;
    }

    g_debug("ai-image: dropping unsupported parameter '%s' for model '%s' (%s)",
            parameter, model_id, reason);

    return TRUE;
}

/*
 * Test whether a value appears in a NULL-terminated vector.  An absent or
 * empty vector means "no opinion", so everything is allowed.
 */
static gboolean
ai_image_value_allowed(
    const gchar * const *allowed,
    const gchar         *value
){
    guint i;

    if (allowed == NULL || allowed[0] == NULL || value == NULL)
    {
        return TRUE;
    }

    for (i = 0; allowed[i] != NULL; i++)
    {
        if (g_strcmp0(allowed[i], value) == 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * ai_image_request_validate:
 * @request: the #AiImageRequest to check, modified in place under
 *   %AI_IMAGE_VALIDATE_NONE
 * @info: (nullable): the target model, or %NULL to skip validation
 * @flags: how to treat unsupported parameters
 * @error: (out) (optional): return location for a #GError
 *
 * Reconciles @request with what @info says the target model accepts.
 *
 * This is the single choke point every provider funnels its requests
 * through before serialising, and it is what lets #AiImageRequest be a
 * superset of all the supported APIs without generating requests those
 * APIs reject.  Image endpoints refuse unknown parameters outright rather
 * than ignoring them, so sending a model something it has never heard of
 * costs a round trip and returns an error instead of an image.
 *
 * Under the default %AI_IMAGE_VALIDATE_NONE each unsupported parameter is
 * reset to its unset value and logged at debug level, leaving a request
 * the model will accept.  Under %AI_IMAGE_VALIDATE_STRICT the first one
 * raises %AI_ERROR_INVALID_REQUEST instead, for callers that need to know
 * their request was not honoured as written.
 *
 * Limits are enforced as well as capabilities: counts above the model's
 * maximum are clamped, and excess reference images are rejected rather
 * than silently truncated, since dropping a reference changes the result
 * in a way the caller cannot see.
 *
 * Passing %NULL for @info skips validation entirely, which is what happens
 * for a model the provider has no entry for -- an unknown model is passed
 * through untouched rather than being second-guessed, so a model released
 * after this build still works.
 *
 * Returns: %TRUE if the request is usable
 */
gboolean
ai_image_request_validate(
    AiImageRequest          *request,
    const AiImageModelInfo  *info,
    AiImageValidateFlags     flags,
    GError                 **error
){
    const gchar *model_id;
    guint reference_count;

    g_return_val_if_fail(request != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (info == NULL)
    {
        return TRUE;
    }

    model_id = info->id;
    reference_count = ai_image_request_get_reference_image_count(request);

    /* Reference images.  Too many is an error even in lenient mode:
     * quietly dropping one would change the image in a way the caller has
     * no way to notice. */
    if (reference_count > 0)
    {
        if (!ai_image_model_info_supports(info, AI_IMAGE_CAP_REFERENCE_IMAGES))
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                        "Model '%s' does not accept reference images",
                        model_id);
            return FALSE;
        }

        if (reference_count > 1 &&
            !ai_image_model_info_supports(info, AI_IMAGE_CAP_MULTI_REFERENCE))
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                        "Model '%s' accepts only one reference image, %u given",
                        model_id, reference_count);
            return FALSE;
        }

        if (info->max_reference_images > 0 &&
            reference_count > info->max_reference_images)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                        "Model '%s' accepts at most %u reference images, %u given",
                        model_id, info->max_reference_images, reference_count);
            return FALSE;
        }
    }

    /* A mask without a reference image has nothing to mask. */
    if (ai_image_request_get_mask(request) != NULL)
    {
        if (!ai_image_model_info_supports(info, AI_IMAGE_CAP_MASK))
        {
            if (!ai_image_validate_reject(flags, model_id, "an edit mask",
                                          "masks are not supported", error))
            {
                return FALSE;
            }
            ai_image_request_set_mask(request, NULL);
        }
        else if (reference_count == 0)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                        "A mask requires a reference image to apply it to");
            return FALSE;
        }
    }

    /* Operations.  An unsupported operation is always an error: silently
     * generating from scratch when an edit was asked for would produce a
     * plausible-looking but entirely wrong result. */
    switch (ai_image_request_get_operation(request))
    {
        case AI_IMAGE_OPERATION_VARIATION:
            if (!ai_image_model_info_supports(info, AI_IMAGE_CAP_VARIATION))
            {
                g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                            "Model '%s' does not support variations", model_id);
                return FALSE;
            }
            break;

        case AI_IMAGE_OPERATION_UPSCALE:
            if (!ai_image_model_info_supports(info, AI_IMAGE_CAP_UPSCALE))
            {
                g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                            "Model '%s' does not support upscaling", model_id);
                return FALSE;
            }
            break;

        case AI_IMAGE_OPERATION_EDIT:
            if (!ai_image_model_info_supports(info, AI_IMAGE_CAP_REFERENCE_IMAGES))
            {
                g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                            "Model '%s' does not support editing", model_id);
                return FALSE;
            }
            break;

        case AI_IMAGE_OPERATION_GENERATE:
        default:
            break;
    }

    /* Count.  Clamping is safe here -- the caller gets fewer images than
     * asked for, which is visible in the response. */
    if (ai_image_request_get_count(request) > 1)
    {
        if (!ai_image_model_info_supports(info, AI_IMAGE_CAP_MULTI_COUNT))
        {
            if (!ai_image_validate_reject(flags, model_id, "multiple images",
                                          "only one image per request", error))
            {
                return FALSE;
            }
            ai_image_request_set_count(request, 1);
        }
        else if ((guint)ai_image_request_get_count(request) > info->max_count)
        {
            if (!ai_image_validate_reject(flags, model_id, "the requested count",
                                          "above the per-request maximum", error))
            {
                return FALSE;
            }
            ai_image_request_set_count(request, (gint)info->max_count);
        }
    }

    /* Geometry */
    if (ai_image_request_get_aspect_ratio(request) != NULL)
    {
        if (!ai_image_model_info_supports(info, AI_IMAGE_CAP_ASPECT_RATIO))
        {
            if (!ai_image_validate_reject(flags, model_id, "an aspect ratio",
                                          "geometry is set by pixel size", error))
            {
                return FALSE;
            }
            ai_image_request_set_aspect_ratio(request, NULL);
        }
        else if (!ai_image_value_allowed(
                     ai_image_model_info_get_aspect_ratios(info),
                     ai_image_request_get_aspect_ratio(request)))
        {
            if (!ai_image_validate_reject(flags, model_id,
                                          "that aspect ratio",
                                          "not in the supported set", error))
            {
                return FALSE;
            }
            ai_image_request_set_aspect_ratio(request, NULL);
        }
    }

    if (ai_image_request_get_size(request) != AI_IMAGE_SIZE_AUTO &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_PIXEL_SIZE))
    {
        if (!ai_image_validate_reject(flags, model_id, "a pixel size",
                                      "geometry is set by aspect ratio", error))
        {
            return FALSE;
        }
        ai_image_request_set_size(request, AI_IMAGE_SIZE_AUTO);
    }

    if (ai_image_request_get_resolution(request) != AI_IMAGE_RESOLUTION_AUTO &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_RESOLUTION_TIER))
    {
        if (!ai_image_validate_reject(flags, model_id, "a resolution tier",
                                      "resolution is not selectable", error))
        {
            return FALSE;
        }
        ai_image_request_set_resolution(request, AI_IMAGE_RESOLUTION_AUTO);
    }

    /* Appearance */
    if (ai_image_request_get_quality(request) != AI_IMAGE_QUALITY_AUTO &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_QUALITY))
    {
        if (!ai_image_validate_reject(flags, model_id, "a quality setting",
                                      "quality is not selectable", error))
        {
            return FALSE;
        }
        ai_image_request_set_quality(request, AI_IMAGE_QUALITY_AUTO);
    }

    if ((ai_image_request_get_style(request) != AI_IMAGE_STYLE_AUTO ||
         ai_image_request_get_style_preset(request) != NULL) &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_STYLE))
    {
        if (!ai_image_validate_reject(flags, model_id, "a style setting",
                                      "style is not selectable", error))
        {
            return FALSE;
        }
        ai_image_request_set_style(request, AI_IMAGE_STYLE_AUTO);
        ai_image_request_set_style_preset(request, NULL);
    }

    if (ai_image_request_get_background(request) != AI_IMAGE_BACKGROUND_AUTO &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_TRANSPARENCY))
    {
        if (!ai_image_validate_reject(flags, model_id, "background control",
                                      "transparency is not supported", error))
        {
            return FALSE;
        }
        ai_image_request_set_background(request, AI_IMAGE_BACKGROUND_AUTO);
    }

    /* Output encoding.  Transparency needs an alpha channel, so asking for
     * a transparent JPEG is self-contradictory however capable the model
     * is; catch it here rather than returning a silently opaque image. */
    if (ai_image_request_get_background(request) == AI_IMAGE_BACKGROUND_TRANSPARENT &&
        ai_image_request_get_output_format(request) == AI_IMAGE_FORMAT_JPEG)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "A transparent background requires an output format with "
                    "an alpha channel; JPEG has none");
        return FALSE;
    }

    if (ai_image_request_get_output_format(request) != AI_IMAGE_FORMAT_AUTO &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_OUTPUT_FORMAT))
    {
        if (!ai_image_validate_reject(flags, model_id, "an output format",
                                      "the encoding is fixed", error))
        {
            return FALSE;
        }
        ai_image_request_set_output_format(request, AI_IMAGE_FORMAT_AUTO);
        ai_image_request_set_output_compression(request, -1);
    }

    /* Sampling */
    if (ai_image_request_get_seed(request) >= 0 &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_SEED))
    {
        if (!ai_image_validate_reject(flags, model_id, "a seed",
                                      "generation is not seedable", error))
        {
            return FALSE;
        }
        ai_image_request_set_seed(request, -1);
    }

    if ((ai_image_request_get_temperature(request) >= 0.0 ||
         ai_image_request_get_top_p(request) >= 0.0 ||
         ai_image_request_get_top_k(request) >= 0) &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_SAMPLING))
    {
        if (!ai_image_validate_reject(flags, model_id, "sampling parameters",
                                      "sampling is not exposed", error))
        {
            return FALSE;
        }
        ai_image_request_set_temperature(request, -1.0);
        ai_image_request_set_top_p(request, -1.0);
        ai_image_request_set_top_k(request, -1);
    }

    /* Prompt shaping */
    if (ai_image_request_get_negative_prompt(request) != NULL &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_NEGATIVE_PROMPT))
    {
        if (!ai_image_validate_reject(flags, model_id, "a negative prompt",
                                      "negative prompts are not supported",
                                      error))
        {
            return FALSE;
        }
        ai_image_request_set_negative_prompt(request, NULL);
    }

    if (ai_image_request_get_enhance_prompt(request) != AI_TRI_UNSET &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_PROMPT_ENHANCEMENT))
    {
        if (!ai_image_validate_reject(flags, model_id, "prompt enhancement",
                                      "the prompt is used verbatim", error))
        {
            return FALSE;
        }
        ai_image_request_set_enhance_prompt(request, AI_TRI_UNSET);
    }

    if (ai_image_request_get_language(request) != NULL &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_LANGUAGE))
    {
        if (!ai_image_validate_reject(flags, model_id, "a language hint",
                                      "the language is auto-detected", error))
        {
            return FALSE;
        }
        ai_image_request_set_language(request, NULL);
    }

    /* Safety and policy */
    if ((ai_image_request_get_moderation(request) != AI_IMAGE_MODERATION_AUTO ||
         ai_image_request_get_person_generation(request) !=
             AI_IMAGE_PERSON_GENERATION_DEFAULT) &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_SAFETY_CONTROL))
    {
        if (!ai_image_validate_reject(flags, model_id, "safety controls",
                                      "filtering is not tunable", error))
        {
            return FALSE;
        }
        ai_image_request_set_moderation(request, AI_IMAGE_MODERATION_AUTO);
        ai_image_request_set_person_generation(
            request, AI_IMAGE_PERSON_GENERATION_DEFAULT);
    }

    if (ai_image_request_get_watermark(request) != AI_TRI_UNSET &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_WATERMARK_CONTROL))
    {
        if (!ai_image_validate_reject(flags, model_id, "watermark control",
                                      "watermarking is not tunable", error))
        {
            return FALSE;
        }
        ai_image_request_set_watermark(request, AI_TRI_UNSET);
    }

    /* Editing extras */
    if (ai_image_request_get_input_fidelity(request) != AI_IMAGE_FIDELITY_AUTO &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_INPUT_FIDELITY))
    {
        if (!ai_image_validate_reject(flags, model_id, "input fidelity",
                                      "fidelity is not selectable", error))
        {
            return FALSE;
        }
        ai_image_request_set_input_fidelity(request, AI_IMAGE_FIDELITY_AUTO);
    }

    if (ai_image_request_get_partial_images(request) >= 0 &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_PARTIAL_STREAMING))
    {
        if (!ai_image_validate_reject(flags, model_id, "partial previews",
                                      "streaming previews are not supported",
                                      error))
        {
            return FALSE;
        }
        ai_image_request_set_partial_images(request, -1);
    }

    /* Response format.  A model that only returns inline bytes cannot
     * honour a request for a URL; fall back rather than fail, since the
     * caller gets the image either way. */
    if (ai_image_request_get_response_format(request) == AI_IMAGE_RESPONSE_URL &&
        !ai_image_model_info_supports(info, AI_IMAGE_CAP_URL_RESPONSE))
    {
        ai_image_request_set_response_format(request, AI_IMAGE_RESPONSE_BASE64);
    }

    return TRUE;
}
