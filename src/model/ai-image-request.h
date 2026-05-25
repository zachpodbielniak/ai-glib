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

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiImageRequest, ai_image_request_free)

G_END_DECLS
