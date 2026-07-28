/*
 * ai-image.h - Binary image payload (input side)
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

G_BEGIN_DECLS

#define AI_TYPE_IMAGE (ai_image_get_type())

/**
 * AiImage:
 *
 * A boxed type holding raw image bytes plus their metadata.
 *
 * #AiImage is the *input* counterpart to #AiGeneratedImage: it is what you
 * hand to an image request as a reference (for multi-image conditioning) or
 * as the subject of an edit, variation or upscale operation.
 *
 * Beyond the bytes and MIME type it carries three optional hints:
 *
 * - the pixel dimensions, when the caller happens to know them,
 * - a source URI, purely as provenance for diagnostics, and
 * - a #AiImage:role label such as `"style"` or `"subject"`.
 *
 * The role is what makes multi-reference conditioning legible to a model:
 * providers that accept several reference images (Gemini's Nano Banana Pro
 * takes up to fourteen) have no other way to tell which image is meant to
 * supply composition and which is meant to supply palette.
 */
typedef struct _AiImage AiImage;

/**
 * ai_image_get_type:
 *
 * Gets the #GType for #AiImage.
 *
 * Returns: the #GType for #AiImage
 */
GType
ai_image_get_type(void);

AiImage *
ai_image_new_from_bytes(
    GBytes      *bytes,
    const gchar *mime_type
);

AiImage *
ai_image_new_from_data(
    gconstpointer  data,
    gsize          length,
    const gchar   *mime_type
);

AiImage *
ai_image_new_from_file(
    const gchar  *path,
    GError      **error
);

AiImage *
ai_image_new_from_base64(
    const gchar *base64_data,
    const gchar *mime_type
);

AiImage *
ai_image_copy(const AiImage *self);

void
ai_image_free(AiImage *self);

GBytes *
ai_image_get_bytes(const AiImage *self);

gsize
ai_image_get_size(const AiImage *self);

const gchar *
ai_image_get_mime_type(const AiImage *self);

void
ai_image_set_mime_type(
    AiImage     *self,
    const gchar *mime_type
);

gchar *
ai_image_dup_base64(AiImage *self);

gint
ai_image_get_width(const AiImage *self);

gint
ai_image_get_height(const AiImage *self);

void
ai_image_set_dimensions(
    AiImage *self,
    gint     width,
    gint     height
);

const gchar *
ai_image_get_uri(const AiImage *self);

void
ai_image_set_uri(
    AiImage     *self,
    const gchar *uri
);

const gchar *
ai_image_get_role(const AiImage *self);

void
ai_image_set_role(
    AiImage     *self,
    const gchar *role
);

const gchar *
ai_image_get_filename(const AiImage *self);

void
ai_image_set_filename(
    AiImage     *self,
    const gchar *filename
);

gboolean
ai_image_save_to_file(
    AiImage      *self,
    const gchar  *path,
    GError      **error
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiImage, ai_image_free)

G_END_DECLS
