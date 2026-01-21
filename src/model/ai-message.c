/*
 * ai-message.c - Conversation message
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-message.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"
#include "core/ai-error.h"

/*
 * Private structure for AiMessage.
 */
struct _AiMessage
{
    GObject parent_instance;

    AiRole  role;
    GList  *content_blocks; /* List of AiContentBlock */
};

G_DEFINE_TYPE(AiMessage, ai_message, G_TYPE_OBJECT)

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_ROLE,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_message_finalize(GObject *object)
{
    AiMessage *self = AI_MESSAGE(object);

    g_list_free_full(self->content_blocks, g_object_unref);

    G_OBJECT_CLASS(ai_message_parent_class)->finalize(object);
}

static void
ai_message_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiMessage *self = AI_MESSAGE(object);

    switch (prop_id)
    {
        case PROP_ROLE:
            g_value_set_enum(value, self->role);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_message_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiMessage *self = AI_MESSAGE(object);

    switch (prop_id)
    {
        case PROP_ROLE:
            self->role = g_value_get_enum(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_message_class_init(AiMessageClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_message_finalize;
    object_class->get_property = ai_message_get_property;
    object_class->set_property = ai_message_set_property;

    /**
     * AiMessage:role:
     *
     * The message role.
     */
    properties[PROP_ROLE] =
        g_param_spec_enum("role",
                          "Role",
                          "The message role",
                          AI_TYPE_ROLE,
                          AI_ROLE_USER,
                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                          G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_message_init(AiMessage *self)
{
    self->role = AI_ROLE_USER;
    self->content_blocks = NULL;
}

/**
 * ai_message_new:
 * @role: the message role
 *
 * Creates a new empty #AiMessage with the given role.
 *
 * Returns: (transfer full): a new #AiMessage
 */
AiMessage *
ai_message_new(AiRole role)
{
    g_autoptr(AiMessage) self = g_object_new(AI_TYPE_MESSAGE,
                                              "role", role,
                                              NULL);

    return (AiMessage *)g_steal_pointer(&self);
}

/**
 * ai_message_new_user:
 * @text: the message text
 *
 * Creates a new user message with text content.
 *
 * Returns: (transfer full): a new #AiMessage
 */
AiMessage *
ai_message_new_user(const gchar *text)
{
    g_autoptr(AiMessage) self = ai_message_new(AI_ROLE_USER);
    g_autoptr(AiTextContent) content = ai_text_content_new(text);

    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&content));

    return (AiMessage *)g_steal_pointer(&self);
}

/**
 * ai_message_new_assistant:
 * @text: the message text
 *
 * Creates a new assistant message with text content.
 *
 * Returns: (transfer full): a new #AiMessage
 */
AiMessage *
ai_message_new_assistant(const gchar *text)
{
    g_autoptr(AiMessage) self = ai_message_new(AI_ROLE_ASSISTANT);
    g_autoptr(AiTextContent) content = ai_text_content_new(text);

    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&content));

    return (AiMessage *)g_steal_pointer(&self);
}

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
){
    g_autoptr(AiMessage) self = ai_message_new(AI_ROLE_USER);
    g_autoptr(AiToolResult) result = ai_tool_result_new(tool_use_id, content, is_error);

    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&result));

    return (AiMessage *)g_steal_pointer(&self);
}

/**
 * ai_message_get_role:
 * @self: an #AiMessage
 *
 * Gets the message role.
 *
 * Returns: the #AiRole of this message
 */
AiRole
ai_message_get_role(AiMessage *self)
{
    g_return_val_if_fail(AI_IS_MESSAGE(self), AI_ROLE_USER);

    return self->role;
}

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
ai_message_get_text(AiMessage *self)
{
    g_autoptr(GString) result = NULL;
    GList *l;

    g_return_val_if_fail(AI_IS_MESSAGE(self), NULL);

    result = g_string_new(NULL);

    for (l = self->content_blocks; l != NULL; l = l->next)
    {
        AiContentBlock *block = l->data;

        if (AI_IS_TEXT_CONTENT(block))
        {
            const gchar *text = ai_text_content_get_text(AI_TEXT_CONTENT(block));
            if (text != NULL)
            {
                if (result->len > 0)
                {
                    g_string_append_c(result, '\n');
                }
                g_string_append(result, text);
            }
        }
    }

    if (result->len == 0)
    {
        return NULL;
    }

    return g_string_free(g_steal_pointer(&result), FALSE);
}

/**
 * ai_message_get_content_blocks:
 * @self: an #AiMessage
 *
 * Gets the list of content blocks in this message.
 *
 * Returns: (transfer none) (element-type AiContentBlock): the content blocks
 */
GList *
ai_message_get_content_blocks(AiMessage *self)
{
    g_return_val_if_fail(AI_IS_MESSAGE(self), NULL);

    return self->content_blocks;
}

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
){
    g_return_if_fail(AI_IS_MESSAGE(self));
    g_return_if_fail(AI_IS_CONTENT_BLOCK(block));

    self->content_blocks = g_list_append(self->content_blocks, block);
}

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
){
    g_autoptr(AiTextContent) content = NULL;

    g_return_if_fail(AI_IS_MESSAGE(self));

    content = ai_text_content_new(text);
    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&content));
}

/**
 * ai_message_to_json:
 * @self: an #AiMessage
 *
 * Serializes the message to JSON for API requests.
 * Format: { "role": "user", "content": [...] }
 *
 * Returns: (transfer full): a #JsonNode representing this message
 */
JsonNode *
ai_message_to_json(AiMessage *self)
{
    g_autoptr(JsonBuilder) builder = NULL;
    GList *l;

    g_return_val_if_fail(AI_IS_MESSAGE(self), NULL);

    builder = json_builder_new();

    json_builder_begin_object(builder);

    /* Role */
    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, ai_role_to_string(self->role));

    /* Content */
    json_builder_set_member_name(builder, "content");

    /* If single text block, use string shorthand; otherwise use array */
    if (g_list_length(self->content_blocks) == 1 &&
        AI_IS_TEXT_CONTENT(self->content_blocks->data))
    {
        const gchar *text = ai_text_content_get_text(AI_TEXT_CONTENT(self->content_blocks->data));
        json_builder_add_string_value(builder, text != NULL ? text : "");
    }
    else
    {
        json_builder_begin_array(builder);

        for (l = self->content_blocks; l != NULL; l = l->next)
        {
            AiContentBlock *block = l->data;
            g_autoptr(JsonNode) block_node = ai_content_block_to_json(block);

            json_builder_add_value(builder, g_steal_pointer(&block_node));
        }

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/**
 * ai_message_new_from_json:
 * @json: a #JsonNode containing message data
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a new #AiMessage from JSON data.
 * Expects format: { "role": "...", "content": "..." | [...] }
 *
 * Returns: (transfer full) (nullable): a new #AiMessage, or %NULL on error
 */
AiMessage *
ai_message_new_from_json(
    JsonNode  *json,
    GError   **error
){
    JsonObject *obj;
    const gchar *role_str;
    AiRole role;
    g_autoptr(AiMessage) self = NULL;

    g_return_val_if_fail(json != NULL, NULL);

    if (!JSON_NODE_HOLDS_OBJECT(json))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Expected JSON object for message");
        return NULL;
    }

    obj = json_node_get_object(json);

    /* Parse role */
    if (!json_object_has_member(obj, "role"))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Message missing 'role' field");
        return NULL;
    }

    role_str = json_object_get_string_member(obj, "role");
    role = ai_role_from_string(role_str);
    self = ai_message_new(role);

    /* Parse content */
    if (json_object_has_member(obj, "content"))
    {
        JsonNode *content_node = json_object_get_member(obj, "content");

        if (JSON_NODE_HOLDS_VALUE(content_node))
        {
            /* String shorthand */
            const gchar *text = json_node_get_string(content_node);
            ai_message_add_text(self, text);
        }
        else if (JSON_NODE_HOLDS_ARRAY(content_node))
        {
            /* Array of content blocks */
            JsonArray *arr = json_node_get_array(content_node);
            guint len = json_array_get_length(arr);
            guint i;

            for (i = 0; i < len; i++)
            {
                JsonNode *block_node = json_array_get_element(arr, i);
                JsonObject *block_obj;
                const gchar *type;

                if (!JSON_NODE_HOLDS_OBJECT(block_node))
                {
                    continue;
                }

                block_obj = json_node_get_object(block_node);
                type = json_object_get_string_member_with_default(block_obj, "type", "text");

                if (g_strcmp0(type, "text") == 0)
                {
                    const gchar *text = json_object_get_string_member_with_default(block_obj, "text", "");
                    ai_message_add_text(self, text);
                }
                else if (g_strcmp0(type, "tool_use") == 0)
                {
                    const gchar *id = json_object_get_string_member_with_default(block_obj, "id", "");
                    const gchar *name = json_object_get_string_member_with_default(block_obj, "name", "");
                    JsonNode *input = json_object_get_member(block_obj, "input");
                    g_autoptr(AiToolUse) tool_use = ai_tool_use_new(id, name, input);

                    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&tool_use));
                }
                else if (g_strcmp0(type, "tool_result") == 0)
                {
                    const gchar *tool_use_id = json_object_get_string_member_with_default(block_obj, "tool_use_id", "");
                    const gchar *content = json_object_get_string_member_with_default(block_obj, "content", "");
                    gboolean is_error = json_object_get_boolean_member_with_default(block_obj, "is_error", FALSE);
                    g_autoptr(AiToolResult) result = ai_tool_result_new(tool_use_id, content, is_error);

                    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&result));
                }
            }
        }
    }

    return (AiMessage *)g_steal_pointer(&self);
}
