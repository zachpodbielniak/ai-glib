/*
 * ai-image-content.h - Image content block (vision input)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "model/ai-content-block.h"
#include "model/ai-image.h"

G_BEGIN_DECLS

#define AI_TYPE_IMAGE_CONTENT (ai_image_content_get_type())

G_DECLARE_FINAL_TYPE(AiImageContent, ai_image_content, AI, IMAGE_CONTENT, AiContentBlock)

/**
 * AiImageContent:
 *
 * An image inside a message, for vision-capable models.
 *
 * Distinct from #AiImage, which is a bare payload: this is the content
 * block that puts one into a conversation, so a user message can carry a
 * screenshot alongside the sentence that asks about it.
 *
 * The bytes are sent inline, base64 encoded, rather than by URL. A provider
 * fetching a URL would need the image to be reachable from the internet,
 * which for a screenshot of somebody's private dashboard is exactly the
 * wrong requirement.
 */

/**
 * ai_image_content_new:
 * @image: the image payload
 *
 * Creates an image content block wrapping @image.
 *
 * Returns: (transfer full): a new #AiImageContent
 */
AiImageContent *
ai_image_content_new(AiImage *image);

/**
 * ai_image_content_new_from_bytes:
 * @bytes: the raw image data
 * @mime_type: (nullable): the MIME type, defaulting to image/png
 *
 * Returns: (transfer full): a new #AiImageContent
 */
AiImageContent *
ai_image_content_new_from_bytes(
    GBytes      *bytes,
    const gchar *mime_type
);

/**
 * ai_image_content_get_image:
 * @self: an #AiImageContent
 *
 * Returns: (transfer none) (nullable): the image payload
 */
AiImage *
ai_image_content_get_image(AiImageContent *self);

/**
 * ai_image_content_to_data_url:
 * @self: an #AiImageContent
 *
 * The image as a `data:` URL, which is the shape OpenAI-compatible
 * providers accept in an `image_url` content part.
 *
 * Returns: (transfer full) (nullable): the data URL
 */
gchar *
ai_image_content_to_data_url(AiImageContent *self);

G_END_DECLS
