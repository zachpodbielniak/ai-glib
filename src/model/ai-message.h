/*
 * ai-message.h - Conversation message
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
#include "model/ai-content-block.h"

G_BEGIN_DECLS

#define AI_TYPE_MESSAGE (ai_message_get_type())

G_DECLARE_FINAL_TYPE(AiMessage, ai_message, AI, MESSAGE, GObject)

/**
 * ai_message_new:
 * @role: the message role
 *
 * Creates a new empty #AiMessage with the given role.
 *
 * Returns: (transfer full): a new #AiMessage
 */
AiMessage *
ai_message_new(AiRole role);

/**
 * ai_message_new_user:
 * @text: the message text
 *
 * Creates a new user message with text content.
 *
 * Returns: (transfer full): a new #AiMessage
 */
AiMessage *
ai_message_new_user(const gchar *text);

/**
 * ai_message_new_assistant:
 * @text: the message text
 *
 * Creates a new assistant message with text content.
 *
 * Returns: (transfer full): a new #AiMessage
 */
AiMessage *
ai_message_new_assistant(const gchar *text);

/**
 * ai_message_new_tool_result:
 * @tool_use_id: the tool use ID this result corresponds to
 * @content: the result content
 * @is_error: whether this is an error result
 *
 * Creates a new user message containing a tool result.
 *
 * Returns: (transfer full): a new #AiMessage
 */
AiMessage *
ai_message_new_tool_result(
    const gchar *tool_use_id,
    const gchar *content,
    gboolean     is_error
);

/**
 * ai_message_get_role:
 * @self: an #AiMessage
 *
 * Gets the message role.
 *
 * Returns: the #AiRole of this message
 */
AiRole
ai_message_get_role(AiMessage *self);

/**
 * ai_message_get_text:
 * @self: an #AiMessage
 *
 * Gets the concatenated text content of the message.
 * This combines all text content blocks into a single string.
 *
 * Returns: (transfer full) (nullable): the text content, free with g_free()
 */
gchar *
ai_message_get_text(AiMessage *self);

/**
 * ai_message_get_content_blocks:
 * @self: an #AiMessage
 *
 * Gets the list of content blocks in this message.
 *
 * Returns: (transfer none) (element-type AiContentBlock): the content blocks
 */
GList *
ai_message_get_content_blocks(AiMessage *self);

/**
 * ai_message_add_content_block:
 * @self: an #AiMessage
 * @block: (transfer full): the content block to add
 *
 * Adds a content block to the message.
 * The message takes ownership of the block.
 */
void
ai_message_add_content_block(
    AiMessage      *self,
    AiContentBlock *block
);

/**
 * ai_message_add_text:
 * @self: an #AiMessage
 * @text: the text to add
 *
 * Adds a text content block to the message.
 */
void
ai_message_add_text(
    AiMessage   *self,
    const gchar *text
);

/**
 * ai_message_to_json:
 * @self: an #AiMessage
 *
 * Serializes the message to JSON for API requests.
 *
 * Returns: (transfer full): a #JsonNode representing this message
 */
JsonNode *
ai_message_to_json(AiMessage *self);

/**
 * ai_message_new_from_json:
 * @json: a #JsonNode containing message data
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a new #AiMessage from JSON data.
 *
 * Returns: (transfer full) (nullable): a new #AiMessage, or %NULL on error
 */
AiMessage *
ai_message_new_from_json(
    JsonNode  *json,
    GError   **error
);

G_END_DECLS
