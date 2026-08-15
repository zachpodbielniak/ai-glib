/*
 * ai-openai-shared.c - OpenAI-compatible message/tool serialization helpers
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-openai-shared.h"

#include "core/ai-enums.h"
#include "model/ai-text-content.h"
#include "model/ai-image-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

/*
 * Encode a JsonNode as a JSON string. Returns "{}" for NULL / non-object
 * inputs. Caller frees with g_free().
 */
static gchar *
encode_json_node_as_string(JsonNode *node)
{
    g_autoptr(JsonGenerator) gen = NULL;
    g_autoptr(JsonNode) empty = NULL;

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
    {
        empty = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(empty, json_object_new());
        node = empty;
    }

    gen = json_generator_new();
    json_generator_set_root(gen, node);

    return json_generator_to_data(gen, NULL);
}

/*
 * Concatenate all AiTextContent blocks in @msg into a newly-allocated string.
 * Returns NULL if no text blocks (caller decides whether to emit JSON null
 * or omit the field).
 */
static gchar *
collect_text(AiMessage *msg)
{
    g_autoptr(GString) buf = NULL;
    GList *l;

    buf = g_string_new(NULL);

    for (l = ai_message_get_content_blocks(msg); l != NULL; l = l->next)
    {
        AiContentBlock *block = l->data;

        if (AI_IS_TEXT_CONTENT(block))
        {
            const gchar *text = ai_text_content_get_text(AI_TEXT_CONTENT(block));

            if (text == NULL)
            {
                continue;
            }

            if (buf->len > 0)
            {
                g_string_append_c(buf, '\n');
            }
            g_string_append(buf, text);
        }
    }

    if (buf->len == 0)
    {
        return NULL;
    }

    return g_string_free(g_steal_pointer(&buf), FALSE);
}

/*
 * Emit a single {"role":"system|user","content":"..."} wire message.
 */
static void
emit_simple_text_message(
    JsonBuilder *builder,
    const gchar *role,
    const gchar *content
){
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, role);

    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, content != NULL ? content : "");

    json_builder_end_object(builder);
}

/*
 * Emit an assistant wire message: content (text or null) + optional
 * tool_calls array.
 */
static void
emit_assistant_message(
    JsonBuilder            *builder,
    const gchar            *text,
    GList                  *tool_uses, /* of AiToolUse* */
    AiOpenAISerializeFlags  flags
){
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "assistant");

    json_builder_set_member_name(builder, "content");
    if (text != NULL && text[0] != '\0')
    {
        json_builder_add_string_value(builder, text);
    }
    else
    {
        json_builder_add_null_value(builder);
    }

    if (tool_uses != NULL)
    {
        GList *l;

        json_builder_set_member_name(builder, "tool_calls");
        json_builder_begin_array(builder);

        for (l = tool_uses; l != NULL; l = l->next)
        {
            AiToolUse *tu = AI_TOOL_USE(l->data);
            const gchar *id = ai_tool_use_get_id(tu);
            const gchar *name = ai_tool_use_get_name(tu);
            JsonNode *input = ai_tool_use_get_input(tu);

            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, id != NULL ? id : "");

            json_builder_set_member_name(builder, "type");
            json_builder_add_string_value(builder, "function");

            json_builder_set_member_name(builder, "function");
            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, name != NULL ? name : "");

            json_builder_set_member_name(builder, "arguments");
            if (flags & AI_OPENAI_SERIALIZE_ARGS_AS_OBJECT)
            {
                /* Ollama wants a real object. */
                if (input != NULL && JSON_NODE_HOLDS_OBJECT(input))
                {
                    json_builder_add_value(builder, json_node_copy(input));
                }
                else
                {
                    json_builder_begin_object(builder);
                    json_builder_end_object(builder);
                }
            }
            else
            {
                /* OpenAI / Grok want a JSON-encoded string. */
                g_autofree gchar *args_str = encode_json_node_as_string(input);
                json_builder_add_string_value(builder, args_str);
            }

            json_builder_end_object(builder);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);
}

/*
 * Emit a single {"role":"tool","tool_call_id":"...","content":"..."} message.
 */
static void
emit_tool_result_message(
    JsonBuilder  *builder,
    AiToolResult *result
){
    const gchar *tool_use_id = ai_tool_result_get_tool_use_id(result);
    const gchar *content = ai_tool_result_get_content(result);

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "tool");

    json_builder_set_member_name(builder, "tool_call_id");
    json_builder_add_string_value(builder, tool_use_id != NULL ? tool_use_id : "");

    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, content != NULL ? content : "");

    json_builder_end_object(builder);
}

/*
 * Emit a user message whose content is an array of parts: the text, then
 * one image_url part per image. This is how OpenAI-compatible providers --
 * OpenAI, Grok, and anything else speaking Chat Completions -- accept
 * vision input, as against Anthropic's image/source/base64 block.
 *
 * The images travel as data: URLs rather than links, because the caller's
 * screenshot is not on the public internet and should not have to be.
 */
static void
emit_multimodal_user_message(
    JsonBuilder *builder,
    const gchar *text,
    GList       *images
){
    GList *i;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "user");

    json_builder_set_member_name(builder, "content");
    json_builder_begin_array(builder);

    if (text != NULL && text[0] != '\0')
    {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "text");
        json_builder_set_member_name(builder, "text");
        json_builder_add_string_value(builder, text);
        json_builder_end_object(builder);
    }

    for (i = images; i != NULL; i = i->next)
    {
        g_autofree gchar *url = ai_image_content_to_data_url(AI_IMAGE_CONTENT(i->data));

        if (url == NULL)
        {
            continue;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, "image_url");
        json_builder_set_member_name(builder, "image_url");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "url");
        json_builder_add_string_value(builder, url);
        json_builder_end_object(builder);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);
}

void
ai_openai_shared_serialize_messages_array(
    JsonBuilder            *builder,
    GList                  *messages,
    const gchar            *system_prompt,
    AiOpenAISerializeFlags  flags
){
    GList *l;

    g_return_if_fail(builder != NULL);

    if (system_prompt != NULL && system_prompt[0] != '\0')
    {
        emit_simple_text_message(builder, "system", system_prompt);
    }

    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        AiRole role = ai_message_get_role(msg);
        GList *blocks = ai_message_get_content_blocks(msg);
        GList *b;
        g_autoptr(GList) tool_uses = NULL;
        g_autoptr(GList) tool_results = NULL;
        g_autoptr(GList) images = NULL;
        gboolean has_text = FALSE;
        g_autofree gchar *text = NULL;

        for (b = blocks; b != NULL; b = b->next)
        {
            AiContentBlock *block = b->data;

            if (AI_IS_TEXT_CONTENT(block))
            {
                has_text = TRUE;
            }
            else if (AI_IS_TOOL_USE(block))
            {
                tool_uses = g_list_append(tool_uses, block);
            }
            else if (AI_IS_IMAGE_CONTENT(block))
            {
                images = g_list_append(images, block);
            }
            else if (AI_IS_TOOL_RESULT(block))
            {
                tool_results = g_list_append(tool_results, block);
            }
        }

        if (has_text)
        {
            text = collect_text(msg);
        }

        if (role == AI_ROLE_ASSISTANT)
        {
            /* Single assistant wire message carrying text and any tool_calls. */
            emit_assistant_message(builder, text, tool_uses, flags);
        }
        else if (role == AI_ROLE_SYSTEM)
        {
            /* In-conversation system message (rare; usually folded into the
             * top-level system_prompt). Emit as a system wire message. */
            emit_simple_text_message(builder, "system", text != NULL ? text : "");
        }
        else
        {
            /* user role */
            if (images != NULL)
            {
                emit_multimodal_user_message(builder, text, images);
            }
            else if (text != NULL)
            {
                emit_simple_text_message(builder, "user", text);
            }

            for (b = tool_results; b != NULL; b = b->next)
            {
                emit_tool_result_message(builder, AI_TOOL_RESULT(b->data));
            }

            /* User messages with only tool_uses are nonsensical for OpenAI;
             * skip them silently. */
            if (text == NULL && tool_results == NULL && tool_uses == NULL &&
                images == NULL)
            {
                /* Truly empty user message — emit empty content so the
                 * transcript stays well-formed. */
                emit_simple_text_message(builder, "user", "");
            }
        }
    }
}
