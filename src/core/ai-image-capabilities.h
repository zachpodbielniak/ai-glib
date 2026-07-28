/*
 * ai-image-capabilities.h - What an image model can actually do
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
#include "core/ai-error.h"
#include "model/ai-image-request.h"

G_BEGIN_DECLS

/**
 * AiImageCapabilities:
 * @AI_IMAGE_CAP_NONE: Nothing beyond a bare prompt
 * @AI_IMAGE_CAP_REFERENCE_IMAGES: Accepts at least one reference image
 * @AI_IMAGE_CAP_MULTI_REFERENCE: Accepts more than one reference image
 * @AI_IMAGE_CAP_MASK: Accepts an edit mask
 * @AI_IMAGE_CAP_NEGATIVE_PROMPT: Accepts a negative prompt
 * @AI_IMAGE_CAP_SEED: Accepts a sampling seed
 * @AI_IMAGE_CAP_ASPECT_RATIO: Geometry is expressed as an aspect ratio
 * @AI_IMAGE_CAP_PIXEL_SIZE: Geometry is expressed as pixel dimensions
 * @AI_IMAGE_CAP_RESOLUTION_TIER: Accepts a resolution tier (1K/2K/4K)
 * @AI_IMAGE_CAP_TRANSPARENCY: Can render a transparent background
 * @AI_IMAGE_CAP_OUTPUT_FORMAT: Output encoding is selectable
 * @AI_IMAGE_CAP_QUALITY: Accepts a quality setting
 * @AI_IMAGE_CAP_STYLE: Accepts a style setting
 * @AI_IMAGE_CAP_MULTI_COUNT: Can return more than one image per request
 * @AI_IMAGE_CAP_VARIATION: Supports the variation operation
 * @AI_IMAGE_CAP_UPSCALE: Supports the upscale operation
 * @AI_IMAGE_CAP_PARTIAL_STREAMING: Can stream partial previews
 * @AI_IMAGE_CAP_SAFETY_CONTROL: Content filtering is tunable
 * @AI_IMAGE_CAP_WATERMARK_CONTROL: Watermarking is tunable
 * @AI_IMAGE_CAP_URL_RESPONSE: Can return a URL instead of inline bytes
 * @AI_IMAGE_CAP_SAMPLING: Accepts temperature / top-p / top-k
 * @AI_IMAGE_CAP_PROMPT_ENHANCEMENT: Can rewrite the prompt before use
 * @AI_IMAGE_CAP_LANGUAGE: Accepts a prompt language hint
 * @AI_IMAGE_CAP_INPUT_FIDELITY: Accepts an edit input-fidelity setting
 *
 * The set of request parameters a particular image model honours.
 *
 * Image APIs reject parameters their model does not understand rather than
 * ignoring them, so "which knobs exist" is a per-model fact that has to be
 * known before a request is serialised, not discovered from the resulting
 * error.  Each provider declares a static table of #AiImageModelInfo, and
 * ai_image_request_validate() gates the request against it.
 */
typedef enum /*< flags >*/
{
    AI_IMAGE_CAP_NONE               = 0,
    AI_IMAGE_CAP_REFERENCE_IMAGES   = 1 << 0,
    AI_IMAGE_CAP_MULTI_REFERENCE    = 1 << 1,
    AI_IMAGE_CAP_MASK               = 1 << 2,
    AI_IMAGE_CAP_NEGATIVE_PROMPT    = 1 << 3,
    AI_IMAGE_CAP_SEED               = 1 << 4,
    AI_IMAGE_CAP_ASPECT_RATIO       = 1 << 5,
    AI_IMAGE_CAP_PIXEL_SIZE         = 1 << 6,
    AI_IMAGE_CAP_RESOLUTION_TIER    = 1 << 7,
    AI_IMAGE_CAP_TRANSPARENCY       = 1 << 8,
    AI_IMAGE_CAP_OUTPUT_FORMAT      = 1 << 9,
    AI_IMAGE_CAP_QUALITY            = 1 << 10,
    AI_IMAGE_CAP_STYLE              = 1 << 11,
    AI_IMAGE_CAP_MULTI_COUNT        = 1 << 12,
    AI_IMAGE_CAP_VARIATION          = 1 << 13,
    AI_IMAGE_CAP_UPSCALE            = 1 << 14,
    AI_IMAGE_CAP_PARTIAL_STREAMING  = 1 << 15,
    AI_IMAGE_CAP_SAFETY_CONTROL     = 1 << 16,
    AI_IMAGE_CAP_WATERMARK_CONTROL  = 1 << 17,
    AI_IMAGE_CAP_URL_RESPONSE       = 1 << 18,
    AI_IMAGE_CAP_SAMPLING           = 1 << 19,
    AI_IMAGE_CAP_PROMPT_ENHANCEMENT = 1 << 20,
    AI_IMAGE_CAP_LANGUAGE           = 1 << 21,
    AI_IMAGE_CAP_INPUT_FIDELITY     = 1 << 22
} AiImageCapabilities;

GType ai_image_capabilities_get_type(void);
#define AI_TYPE_IMAGE_CAPABILITIES (ai_image_capabilities_get_type())

/**
 * AiImageValidateFlags:
 * @AI_IMAGE_VALIDATE_NONE: Drop unsupported parameters (default)
 * @AI_IMAGE_VALIDATE_STRICT: Fail on any unsupported parameter
 *
 * How ai_image_request_validate() should treat a parameter the target
 * model does not support.
 *
 * The lenient default suits interactive callers, which would rather get a
 * slightly different image than an error; %AI_IMAGE_VALIDATE_STRICT suits
 * scripted callers that need to know their request was not honoured as
 * written.
 */
typedef enum /*< flags >*/
{
    AI_IMAGE_VALIDATE_NONE   = 0,
    AI_IMAGE_VALIDATE_STRICT = 1 << 0
} AiImageValidateFlags;

GType ai_image_validate_flags_get_type(void);
#define AI_TYPE_IMAGE_VALIDATE_FLAGS (ai_image_validate_flags_get_type())

#define AI_TYPE_IMAGE_MODEL_INFO (ai_image_model_info_get_type())

/**
 * AiImageModelInfo:
 *
 * A description of one image model: its identifier, what it can do, and
 * the limits it enforces.
 *
 * Providers declare these as a static table, which is the whole of what
 * "registering" an image model amounts to -- adding a model, or a whole
 * provider, is a table entry rather than a new code path.  The same table
 * drives ai_image_request_validate(), the `--list-image-models` CLI
 * output, and any front-end that wants to offer only the options a chosen
 * model actually accepts.
 */
typedef struct _AiImageModelInfo AiImageModelInfo;

/**
 * ai_image_model_info_get_type:
 *
 * Gets the #GType for #AiImageModelInfo.
 *
 * Returns: the #GType for #AiImageModelInfo
 */
GType
ai_image_model_info_get_type(void);

AiImageModelInfo *
ai_image_model_info_new(
    const gchar         *id,
    const gchar         *display_name,
    AiProviderType       provider_type,
    AiImageCapabilities  capabilities
);

AiImageModelInfo *
ai_image_model_info_copy(const AiImageModelInfo *self);

void
ai_image_model_info_free(AiImageModelInfo *self);

const gchar *
ai_image_model_info_get_id(const AiImageModelInfo *self);

const gchar *
ai_image_model_info_get_display_name(const AiImageModelInfo *self);

AiProviderType
ai_image_model_info_get_provider_type(const AiImageModelInfo *self);

AiImageCapabilities
ai_image_model_info_get_capabilities(const AiImageModelInfo *self);

gboolean
ai_image_model_info_supports(
    const AiImageModelInfo *self,
    AiImageCapabilities     capability
);

guint
ai_image_model_info_get_max_count(const AiImageModelInfo *self);

void
ai_image_model_info_set_max_count(
    AiImageModelInfo *self,
    guint             max_count
);

guint
ai_image_model_info_get_max_reference_images(const AiImageModelInfo *self);

void
ai_image_model_info_set_max_reference_images(
    AiImageModelInfo *self,
    guint             max_reference_images
);

const gchar * const *
ai_image_model_info_get_sizes(const AiImageModelInfo *self);

void
ai_image_model_info_set_sizes(
    AiImageModelInfo    *self,
    const gchar * const *sizes
);

const gchar * const *
ai_image_model_info_get_aspect_ratios(const AiImageModelInfo *self);

void
ai_image_model_info_set_aspect_ratios(
    AiImageModelInfo    *self,
    const gchar * const *aspect_ratios
);

const gchar * const *
ai_image_model_info_get_qualities(const AiImageModelInfo *self);

void
ai_image_model_info_set_qualities(
    AiImageModelInfo    *self,
    const gchar * const *qualities
);

const gchar *
ai_image_model_info_map_quality(
    const AiImageModelInfo *self,
    AiImageQuality          quality
);

const gchar *
ai_image_model_info_get_notes(const AiImageModelInfo *self);

void
ai_image_model_info_set_notes(
    AiImageModelInfo *self,
    const gchar      *notes
);

gchar *
ai_image_capabilities_to_string(AiImageCapabilities capabilities);

gboolean
ai_image_request_validate(
    AiImageRequest          *request,
    const AiImageModelInfo  *info,
    AiImageValidateFlags     flags,
    GError                 **error
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiImageModelInfo, ai_image_model_info_free)

G_END_DECLS
