/*
 * ai-content-block.c - Base class for content blocks
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-content-block.h"

/*
 * Private data for AiContentBlock.
 */
typedef struct
{
    AiContentType content_type;
} AiContentBlockPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(AiContentBlock, ai_content_block, G_TYPE_OBJECT)

/*
 * Default implementation of get_content_type.
 * Returns AI_CONTENT_TYPE_TEXT as a fallback.
 */
static AiContentType
ai_content_block_real_get_content_type(AiContentBlock *self)
{
    AiContentBlockPrivate *priv;

    priv = ai_content_block_get_instance_private(self);
    return priv->content_type;
}

/*
 * Default implementation of to_json.
 * Returns an empty object node.
 */
static JsonNode *
ai_content_block_real_to_json(AiContentBlock *self)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static void
ai_content_block_class_init(AiContentBlockClass *klass)
{
    /* Set up default virtual method implementations */
    klass->get_content_type = ai_content_block_real_get_content_type;
    klass->to_json = ai_content_block_real_to_json;
}

static void
ai_content_block_init(AiContentBlock *self)
{
    AiContentBlockPrivate *priv;

    priv = ai_content_block_get_instance_private(self);
    priv->content_type = AI_CONTENT_TYPE_TEXT;
}

/**
 * ai_content_block_get_content_type:
 * @self: an #AiContentBlock
 *
 * Gets the content type of this block.
 * This calls the virtual method which subclasses can override.
 *
 * Returns: the #AiContentType of this block
 */
AiContentType
ai_content_block_get_content_type(AiContentBlock *self)
{
    AiContentBlockClass *klass;

    g_return_val_if_fail(AI_IS_CONTENT_BLOCK(self), AI_CONTENT_TYPE_TEXT);

    klass = AI_CONTENT_BLOCK_GET_CLASS(self);
    g_return_val_if_fail(klass->get_content_type != NULL, AI_CONTENT_TYPE_TEXT);

    return klass->get_content_type(self);
}

/**
 * ai_content_block_to_json:
 * @self: an #AiContentBlock
 *
 * Serializes this content block to JSON format suitable for API requests.
 * This calls the virtual method which subclasses must implement.
 *
 * Returns: (transfer full): a #JsonNode representing this block
 */
JsonNode *
ai_content_block_to_json(AiContentBlock *self)
{
    AiContentBlockClass *klass;

    g_return_val_if_fail(AI_IS_CONTENT_BLOCK(self), NULL);

    klass = AI_CONTENT_BLOCK_GET_CLASS(self);
    g_return_val_if_fail(klass->to_json != NULL, NULL);

    return klass->to_json(self);
}
