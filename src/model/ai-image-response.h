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

AiImageResponse *
ai_image_response_new(
    const gchar *id,
    gint64       created
);

AiImageResponse *
ai_image_response_copy(const AiImageResponse *self);

void
ai_image_response_free(AiImageResponse *self);

const gchar *
ai_image_response_get_id(const AiImageResponse *self);

gint64
ai_image_response_get_created(const AiImageResponse *self);

GList *
ai_image_response_get_images(const AiImageResponse *self);

guint
ai_image_response_get_image_count(const AiImageResponse *self);

AiGeneratedImage *
ai_image_response_get_image(
    const AiImageResponse *self,
    guint                  index
);

void
ai_image_response_add_image(
    AiImageResponse  *self,
    AiGeneratedImage *image
);

const gchar *
ai_image_response_get_model(const AiImageResponse *self);

void
ai_image_response_set_model(
    AiImageResponse *self,
    const gchar     *model
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiImageResponse, ai_image_response_free)

G_END_DECLS
