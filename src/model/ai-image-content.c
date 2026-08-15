/*
 * ai-image-content.c - Image content block (vision input)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-image-content.h"

/*
 * Private structure for AiImageContent.
 */
struct _AiImageContent
{
    AiContentBlock parent_instance;

    AiImage *image;
};

G_DEFINE_TYPE(AiImageContent, ai_image_content, AI_TYPE_CONTENT_BLOCK)

static void
ai_image_content_finalize(GObject *object)
{
    AiImageContent *self = AI_IMAGE_CONTENT(object);

    g_clear_pointer(&self->image, ai_image_free);

    G_OBJECT_CLASS(ai_image_content_parent_class)->finalize(object);
}

static AiContentType
ai_image_content_get_content_type(AiContentBlock *block)
{
    (void)block;
    return AI_CONTENT_TYPE_IMAGE;
}

/*
 * Serialize to Anthropic's shape:
 *
 *   { "type": "image",
 *     "source": { "type": "base64", "media_type": "...", "data": "..." } }
 *
 * The generic ai_message_to_json() emits Anthropic content blocks --
 * ai-openai-shared.c converts for the OpenAI-compatible providers, which
 * spell the same thing as an image_url part carrying a data: URL.
 */
static JsonNode *
ai_image_content_to_json(AiContentBlock *block)
{
    AiImageContent *self = AI_IMAGE_CONTENT(block);
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autofree gchar *encoded = NULL;
    const gchar *mime_type = "image/png";

    if (self->image != NULL)
    {
        GBytes *bytes = ai_image_get_bytes(self->image);
        const gchar *declared = ai_image_get_mime_type(self->image);

        if (declared != NULL)
        {
            mime_type = declared;
        }

        if (bytes != NULL)
        {
            gsize length = 0;
            const guchar *data = g_bytes_get_data(bytes, &length);

            encoded = g_base64_encode(data, length);
        }
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "image");

    json_builder_set_member_name(builder, "source");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "base64");

    json_builder_set_member_name(builder, "media_type");
    json_builder_add_string_value(builder, mime_type);

    json_builder_set_member_name(builder, "data");
    json_builder_add_string_value(builder, encoded != NULL ? encoded : "");

    json_builder_end_object(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static void
ai_image_content_class_init(AiImageContentClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiContentBlockClass *content_class = AI_CONTENT_BLOCK_CLASS(klass);

    object_class->finalize = ai_image_content_finalize;

    content_class->get_content_type = ai_image_content_get_content_type;
    content_class->to_json = ai_image_content_to_json;
}

static void
ai_image_content_init(AiImageContent *self)
{
    self->image = NULL;
}

AiImageContent *
ai_image_content_new(AiImage *image)
{
    AiImageContent *self;

    g_return_val_if_fail(image != NULL, NULL);

    self = g_object_new(AI_TYPE_IMAGE_CONTENT, NULL);
    self->image = ai_image_copy(image);

    return self;
}

AiImageContent *
ai_image_content_new_from_bytes(
    GBytes      *bytes,
    const gchar *mime_type
){
    g_autoptr(AiImage) image = NULL;

    g_return_val_if_fail(bytes != NULL, NULL);

    image = ai_image_new_from_bytes(bytes,
                                    mime_type != NULL ? mime_type : "image/png");

    return ai_image_content_new(image);
}

AiImage *
ai_image_content_get_image(AiImageContent *self)
{
    g_return_val_if_fail(AI_IS_IMAGE_CONTENT(self), NULL);

    return self->image;
}

gchar *
ai_image_content_to_data_url(AiImageContent *self)
{
    GBytes *bytes;
    const gchar *mime_type;
    g_autofree gchar *encoded = NULL;
    gsize length = 0;
    const guchar *data;

    g_return_val_if_fail(AI_IS_IMAGE_CONTENT(self), NULL);

    if (self->image == NULL)
    {
        return NULL;
    }

    bytes = ai_image_get_bytes(self->image);

    if (bytes == NULL)
    {
        return NULL;
    }

    mime_type = ai_image_get_mime_type(self->image);
    data = g_bytes_get_data(bytes, &length);
    encoded = g_base64_encode(data, length);

    return g_strdup_printf("data:%s;base64,%s",
                           mime_type != NULL ? mime_type : "image/png",
                           encoded);
}
