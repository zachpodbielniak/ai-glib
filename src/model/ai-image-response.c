/*
 * ai-image-response.c - Image generation response container
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-image-response.h"

/*
 * Private structure for AiImageResponse boxed type.
 * Stores the response metadata and list of generated images.
 */
struct _AiImageResponse
{
    gchar  *id;
    gint64  created;
    gchar  *model;
    GList  *images;  /* GList<AiGeneratedImage*> */
};

/*
 * ai_image_response_get_type:
 *
 * Registers the AiImageResponse boxed type with the GLib type system.
 * Uses copy and free functions for memory management.
 */
G_DEFINE_BOXED_TYPE(AiImageResponse, ai_image_response, ai_image_response_copy, ai_image_response_free)

/**
 * ai_image_response_new:
 * @id: (nullable): the response ID
 * @created: the creation timestamp (Unix epoch)
 *
 * Creates a new empty #AiImageResponse.
 *
 * Returns: (transfer full): a new #AiImageResponse
 */
AiImageResponse *
ai_image_response_new(
    const gchar *id,
    gint64       created
){
    AiImageResponse *self;

    self = g_slice_new0(AiImageResponse);
    self->id = g_strdup(id);
    self->created = created;
    self->model = NULL;
    self->images = NULL;

    return self;
}

/**
 * ai_image_response_copy:
 * @self: an #AiImageResponse
 *
 * Creates a deep copy of an #AiImageResponse instance.
 *
 * Returns: (transfer full): a copy of @self
 */
AiImageResponse *
ai_image_response_copy(const AiImageResponse *self)
{
    AiImageResponse *copy;
    GList *l;

    if (self == NULL)
    {
        return NULL;
    }

    copy = g_slice_new0(AiImageResponse);
    copy->id = g_strdup(self->id);
    copy->created = self->created;
    copy->model = g_strdup(self->model);
    copy->images = NULL;

    /* Deep copy the images list */
    for (l = self->images; l != NULL; l = l->next)
    {
        AiGeneratedImage *img = l->data;
        copy->images = g_list_append(copy->images, ai_generated_image_copy(img));
    }

    return copy;
}

/**
 * ai_image_response_free:
 * @self: (nullable): an #AiImageResponse
 *
 * Frees an #AiImageResponse instance and all its allocated memory.
 * If @self is %NULL, this function does nothing.
 */
void
ai_image_response_free(AiImageResponse *self)
{
    if (self == NULL)
    {
        return;
    }

    g_clear_pointer(&self->id, g_free);
    g_clear_pointer(&self->model, g_free);

    /* Free all images in the list */
    g_list_free_full(self->images, (GDestroyNotify)ai_generated_image_free);

    g_slice_free(AiImageResponse, self);
}

/**
 * ai_image_response_get_id:
 * @self: an #AiImageResponse
 *
 * Gets the response ID.
 *
 * Returns: (transfer none) (nullable): the response ID
 */
const gchar *
ai_image_response_get_id(const AiImageResponse *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->id;
}

/**
 * ai_image_response_get_created:
 * @self: an #AiImageResponse
 *
 * Gets the creation timestamp.
 *
 * Returns: the creation timestamp as Unix epoch
 */
gint64
ai_image_response_get_created(const AiImageResponse *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->created;
}

/**
 * ai_image_response_get_images:
 * @self: an #AiImageResponse
 *
 * Gets the list of generated images.
 *
 * Returns: (transfer none) (element-type AiGeneratedImage): the images list
 */
GList *
ai_image_response_get_images(const AiImageResponse *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->images;
}

/**
 * ai_image_response_get_image_count:
 * @self: an #AiImageResponse
 *
 * Gets the number of images in the response.
 *
 * Returns: the number of images
 */
guint
ai_image_response_get_image_count(const AiImageResponse *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return g_list_length(self->images);
}

/**
 * ai_image_response_get_image:
 * @self: an #AiImageResponse
 * @index: the image index (0-based)
 *
 * Gets a specific image by index.
 *
 * Returns: (transfer none) (nullable): the image at @index, or %NULL
 */
AiGeneratedImage *
ai_image_response_get_image(
    const AiImageResponse *self,
    guint                  index
){
    g_return_val_if_fail(self != NULL, NULL);

    return g_list_nth_data(self->images, index);
}

/**
 * ai_image_response_add_image:
 * @self: an #AiImageResponse
 * @image: (transfer full): the image to add
 *
 * Adds an image to the response. Takes ownership of @image.
 */
void
ai_image_response_add_image(
    AiImageResponse  *self,
    AiGeneratedImage *image
){
    g_return_if_fail(self != NULL);
    g_return_if_fail(image != NULL);

    self->images = g_list_append(self->images, image);
}

/**
 * ai_image_response_get_model:
 * @self: an #AiImageResponse
 *
 * Gets the model used for generation.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_image_response_get_model(const AiImageResponse *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->model;
}

/**
 * ai_image_response_set_model:
 * @self: an #AiImageResponse
 * @model: (nullable): the model name
 *
 * Sets the model used for generation.
 */
void
ai_image_response_set_model(
    AiImageResponse *self,
    const gchar     *model
){
    g_return_if_fail(self != NULL);

    g_free(self->model);
    self->model = g_strdup(model);
}
