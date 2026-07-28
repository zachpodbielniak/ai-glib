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
#include "model/ai-image.h"

G_BEGIN_DECLS

#define AI_TYPE_IMAGE_REQUEST (ai_image_request_get_type())

/**
 * AiImageRequest:
 *
 * A boxed type containing parameters for an image generation request.
 *
 * #AiImageRequest is deliberately a *superset* of what any single provider
 * accepts: it carries every parameter the supported image APIs expose,
 * from the prompt itself through reference images, seeds, transparency,
 * safety policy and output encoding.  No provider honours all of it.
 *
 * Each provider projects the request through the #AiImageModelInfo of the
 * model being used, and ai_image_request_validate() decides what happens
 * to anything that model does not support: by default it is dropped with a
 * debug message, and under %AI_IMAGE_VALIDATE_STRICT it raises
 * %AI_ERROR_INVALID_REQUEST instead.
 *
 * That rule is what keeps the type both complete and safe.  Image APIs
 * reject unknown parameters outright rather than ignoring them, so a
 * request object that always serialised every field would fail against
 * most models; funnelling everything through one capability check means a
 * caller can set whatever it likes and still get a request the target
 * model accepts.
 *
 * Parameters that need to distinguish "off" from "unspecified" use
 * #AiTriState rather than #gboolean, and numeric parameters use a negative
 * sentinel, so an untouched request serialises to the provider's own
 * defaults rather than to ai-glib's.
 *
 * Anything not modelled here can still be sent via
 * ai_image_request_set_extra(), which is spliced verbatim into the request
 * body -- so a newly-shipped API parameter is usable without waiting for
 * an ai-glib release.
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

AiImageRequest *
ai_image_request_new(const gchar *prompt);

AiImageRequest *
ai_image_request_copy(const AiImageRequest *self);

void
ai_image_request_free(AiImageRequest *self);

const gchar *
ai_image_request_get_prompt(const AiImageRequest *self);

void
ai_image_request_set_prompt(
    AiImageRequest *self,
    const gchar    *prompt
);

const gchar *
ai_image_request_get_model(const AiImageRequest *self);

void
ai_image_request_set_model(
    AiImageRequest *self,
    const gchar    *model
);

AiImageSize
ai_image_request_get_size(const AiImageRequest *self);

void
ai_image_request_set_size(
    AiImageRequest *self,
    AiImageSize     size
);

const gchar *
ai_image_request_get_custom_size(const AiImageRequest *self);

void
ai_image_request_set_custom_size(
    AiImageRequest *self,
    const gchar    *custom_size
);

AiImageQuality
ai_image_request_get_quality(const AiImageRequest *self);

void
ai_image_request_set_quality(
    AiImageRequest *self,
    AiImageQuality  quality
);

AiImageStyle
ai_image_request_get_style(const AiImageRequest *self);

void
ai_image_request_set_style(
    AiImageRequest *self,
    AiImageStyle    style
);

gint
ai_image_request_get_count(const AiImageRequest *self);

void
ai_image_request_set_count(
    AiImageRequest *self,
    gint            count
);

AiImageResponseFormat
ai_image_request_get_response_format(const AiImageRequest *self);

void
ai_image_request_set_response_format(
    AiImageRequest        *self,
    AiImageResponseFormat  format
);

const gchar *
ai_image_request_get_user(const AiImageRequest *self);

void
ai_image_request_set_user(
    AiImageRequest *self,
    const gchar    *user
);

/* Prompt shaping */

const gchar *
ai_image_request_get_negative_prompt(const AiImageRequest *self);

void
ai_image_request_set_negative_prompt(
    AiImageRequest *self,
    const gchar    *negative_prompt
);

const gchar *
ai_image_request_get_system_instruction(const AiImageRequest *self);

void
ai_image_request_set_system_instruction(
    AiImageRequest *self,
    const gchar    *system_instruction
);

/* Operation */

AiImageOperation
ai_image_request_get_operation(const AiImageRequest *self);

void
ai_image_request_set_operation(
    AiImageRequest   *self,
    AiImageOperation  operation
);

/* Geometry */

const gchar *
ai_image_request_get_aspect_ratio(const AiImageRequest *self);

void
ai_image_request_set_aspect_ratio(
    AiImageRequest *self,
    const gchar    *aspect_ratio
);

AiImageResolution
ai_image_request_get_resolution(const AiImageRequest *self);

void
ai_image_request_set_resolution(
    AiImageRequest    *self,
    AiImageResolution  resolution
);

/* Appearance */

const gchar *
ai_image_request_get_style_preset(const AiImageRequest *self);

void
ai_image_request_set_style_preset(
    AiImageRequest *self,
    const gchar    *style_preset
);

AiImageBackground
ai_image_request_get_background(const AiImageRequest *self);

void
ai_image_request_set_background(
    AiImageRequest    *self,
    AiImageBackground  background
);

/* Output encoding */

AiImageFormat
ai_image_request_get_output_format(const AiImageRequest *self);

void
ai_image_request_set_output_format(
    AiImageRequest *self,
    AiImageFormat   output_format
);

gint
ai_image_request_get_output_compression(const AiImageRequest *self);

void
ai_image_request_set_output_compression(
    AiImageRequest *self,
    gint            compression
);

/* Sampling */

gint64
ai_image_request_get_seed(const AiImageRequest *self);

void
ai_image_request_set_seed(
    AiImageRequest *self,
    gint64          seed
);

gdouble
ai_image_request_get_guidance_scale(const AiImageRequest *self);

void
ai_image_request_set_guidance_scale(
    AiImageRequest *self,
    gdouble         guidance_scale
);

gint
ai_image_request_get_steps(const AiImageRequest *self);

void
ai_image_request_set_steps(
    AiImageRequest *self,
    gint            steps
);

gdouble
ai_image_request_get_strength(const AiImageRequest *self);

void
ai_image_request_set_strength(
    AiImageRequest *self,
    gdouble         strength
);

gdouble
ai_image_request_get_temperature(const AiImageRequest *self);

void
ai_image_request_set_temperature(
    AiImageRequest *self,
    gdouble         temperature
);

gdouble
ai_image_request_get_top_p(const AiImageRequest *self);

void
ai_image_request_set_top_p(
    AiImageRequest *self,
    gdouble         top_p
);

gint
ai_image_request_get_top_k(const AiImageRequest *self);

void
ai_image_request_set_top_k(
    AiImageRequest *self,
    gint            top_k
);

/* Safety and policy */

AiImageModeration
ai_image_request_get_moderation(const AiImageRequest *self);

void
ai_image_request_set_moderation(
    AiImageRequest    *self,
    AiImageModeration  moderation
);

AiImagePersonGeneration
ai_image_request_get_person_generation(const AiImageRequest *self);

void
ai_image_request_set_person_generation(
    AiImageRequest          *self,
    AiImagePersonGeneration  person_generation
);

AiTriState
ai_image_request_get_watermark(const AiImageRequest *self);

void
ai_image_request_set_watermark(
    AiImageRequest *self,
    AiTriState      watermark
);

AiTriState
ai_image_request_get_enhance_prompt(const AiImageRequest *self);

void
ai_image_request_set_enhance_prompt(
    AiImageRequest *self,
    AiTriState      enhance_prompt
);

const gchar *
ai_image_request_get_language(const AiImageRequest *self);

void
ai_image_request_set_language(
    AiImageRequest *self,
    const gchar    *language
);

/* Editing */

AiImageFidelity
ai_image_request_get_input_fidelity(const AiImageRequest *self);

void
ai_image_request_set_input_fidelity(
    AiImageRequest  *self,
    AiImageFidelity  input_fidelity
);

gint
ai_image_request_get_partial_images(const AiImageRequest *self);

void
ai_image_request_set_partial_images(
    AiImageRequest *self,
    gint            partial_images
);

/* Reference images */

void
ai_image_request_add_reference_image(
    AiImageRequest *self,
    AiImage        *image
);

gboolean
ai_image_request_add_reference_file(
    AiImageRequest  *self,
    const gchar     *path,
    const gchar     *role,
    GError         **error
);

GList *
ai_image_request_get_reference_images(const AiImageRequest *self);

guint
ai_image_request_get_reference_image_count(const AiImageRequest *self);

void
ai_image_request_clear_reference_images(AiImageRequest *self);

AiImage *
ai_image_request_get_mask(const AiImageRequest *self);

void
ai_image_request_set_mask(
    AiImageRequest *self,
    AiImage        *mask
);

/* Provider-specific passthrough */

void
ai_image_request_set_extra(
    AiImageRequest *self,
    const gchar    *key,
    GVariant       *value
);

void
ai_image_request_set_extra_string(
    AiImageRequest *self,
    const gchar    *key,
    const gchar    *value
);

GVariant *
ai_image_request_get_extra(
    const AiImageRequest *self,
    const gchar          *key
);

GHashTable *
ai_image_request_get_extras(const AiImageRequest *self);

void
ai_image_request_clear_extras(AiImageRequest *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiImageRequest, ai_image_request_free)

G_END_DECLS
