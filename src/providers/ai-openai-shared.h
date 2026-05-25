/*
 * ai-openai-shared.h - OpenAI-compatible message/tool serialization helpers
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API. Shared
 * between OpenAI, Grok, and Ollama provider implementations because all
 * three use the OpenAI Chat Completions message shape on the wire.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-openai-shared.h is an internal header"
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "model/ai-message.h"

G_BEGIN_DECLS

/**
 * AiOpenAISerializeFlags:
 * @AI_OPENAI_SERIALIZE_DEFAULT: OpenAI Chat Completions wire format.
 *     `tool_calls[].function.arguments` is emitted as a JSON-encoded string.
 *     This is what OpenAI itself and OpenAI-compatible Grok require.
 * @AI_OPENAI_SERIALIZE_ARGS_AS_OBJECT: `tool_calls[].function.arguments` is
 *     emitted as a JSON object (not a string). Required by Ollama's
 *     /api/chat endpoint, which rejects the OpenAI string form even though
 *     the rest of the message shape is OpenAI-compatible.
 */
typedef enum
{
    AI_OPENAI_SERIALIZE_DEFAULT         = 0,
    AI_OPENAI_SERIALIZE_ARGS_AS_OBJECT  = 1 << 0
} AiOpenAISerializeFlags;

/*
 * Serialize @messages (optionally prefixed by @system_prompt) into the
 * "messages" JSON array using OpenAI Chat Completions format.
 *
 * Caller must position @builder immediately AFTER
 *   json_builder_set_member_name(builder, "messages");
 *   json_builder_begin_array(builder);
 *
 * and must call json_builder_end_array(builder) afterward.
 *
 * Expansion per AiMessage:
 *
 *  - role==system / @system_prompt non-NULL:
 *      {"role":"system","content":"..."}
 *
 *  - role==user with only text blocks:
 *      {"role":"user","content":"..."}
 *
 *  - role==user containing AiToolResult blocks:
 *      one {"role":"tool","tool_call_id":"<id>","content":"..."} message PER
 *      tool result. Any text blocks in the same AiMessage are emitted as a
 *      separate {"role":"user","content":"..."} message BEFORE the tool
 *      messages, preserving order.
 *
 *  - role==assistant with text and/or AiToolUse blocks:
 *      one {"role":"assistant","content":<text or null>,
 *           "tool_calls":[{"id":"...","type":"function",
 *                          "function":{"name":"...",
 *                                      "arguments":<JSON>}}]} message
 *      "content" is JSON null when only tool_uses are present.
 *      "arguments" is a JSON-encoded string by default (OpenAI / Grok
 *      requirement); pass AI_OPENAI_SERIALIZE_ARGS_AS_OBJECT for Ollama.
 */
void
ai_openai_shared_serialize_messages_array(
    JsonBuilder            *builder,
    GList                  *messages,
    const gchar            *system_prompt,
    AiOpenAISerializeFlags  flags
);

G_END_DECLS
