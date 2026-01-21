/*
 * ai-content-block.h - Base class for content blocks
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
#include <json-glib/json-glib.h>

#include "core/ai-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_CONTENT_BLOCK (ai_content_block_get_type())

G_DECLARE_DERIVABLE_TYPE(AiContentBlock, ai_content_block, AI, CONTENT_BLOCK, GObject)

/**
 * AiContentBlockClass:
 * @parent_class: the parent class
 * @get_content_type: virtual function to get the content type
 * @to_json: virtual function to serialize to JSON
 * @_reserved: reserved for future expansion
 *
 * Class structure for #AiContentBlock.
 */
struct _AiContentBlockClass
{
    GObjectClass parent_class;

    /* Virtual methods */
    AiContentType (*get_content_type)(AiContentBlock *self);
    JsonNode *    (*to_json)         (AiContentBlock *self);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

/**
 * ai_content_block_get_content_type:
 * @self: an #AiContentBlock
 *
 * Gets the content type of this block.
 *
 * Returns: the #AiContentType of this block
 */
AiContentType
ai_content_block_get_content_type(AiContentBlock *self);

/**
 * ai_content_block_to_json:
 * @self: an #AiContentBlock
 *
 * Serializes this content block to JSON.
 *
 * Returns: (transfer full): a #JsonNode representing this block
 */
JsonNode *
ai_content_block_to_json(AiContentBlock *self);

G_END_DECLS
