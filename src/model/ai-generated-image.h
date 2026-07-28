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
#include <gio/gio.h>

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

AiGeneratedImage *
ai_generated_image_new_from_url(const gchar *url);

AiGeneratedImage *
ai_generated_image_new_from_base64(
    const gchar *base64_data,
    const gchar *mime_type
);

AiGeneratedImage *
ai_generated_image_copy(const AiGeneratedImage *self);

void
ai_generated_image_free(AiGeneratedImage *self);

gboolean
ai_generated_image_is_url(const AiGeneratedImage *self);

gboolean
ai_generated_image_is_base64(const AiGeneratedImage *self);

const gchar *
ai_generated_image_get_url(const AiGeneratedImage *self);

const gchar *
ai_generated_image_get_base64(const AiGeneratedImage *self);

const gchar *
ai_generated_image_get_mime_type(const AiGeneratedImage *self);

void
ai_generated_image_set_mime_type(
    AiGeneratedImage *self,
    const gchar      *mime_type
);

const gchar *
ai_generated_image_get_revised_prompt(const AiGeneratedImage *self);

void
ai_generated_image_set_revised_prompt(
    AiGeneratedImage *self,
    const gchar      *revised_prompt
);

GBytes *
ai_generated_image_get_bytes(
    AiGeneratedImage *self,
    GError          **error
);

void
ai_generated_image_load_bytes_async(
    AiGeneratedImage    *self,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

GBytes *
ai_generated_image_load_bytes_finish(
    AiGeneratedImage  *self,
    GAsyncResult      *result,
    GError           **error
);

gboolean
ai_generated_image_save_to_file(
    AiGeneratedImage *self,
    const gchar      *path,
    GError          **error
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiGeneratedImage, ai_generated_image_free)

G_END_DECLS
