/*
 * test-openai-serialize.c - Unit tests for the OpenAI-format shared serializer
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <string.h>
#include <json-glib/json-glib.h>

#include "providers/ai-openai-shared.h"
#include "model/ai-message.h"
#include "model/ai-text-content.h"
#include "model/ai-image-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"
#include "core/ai-enums.h"

/* ================================================================
 * Helpers
 * ================================================================ */

static JsonArray *
serialize_with_flags(GList *messages, const gchar *system_prompt,
                     AiOpenAISerializeFlags flags)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) root = NULL;
    JsonObject *obj;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);

    ai_openai_shared_serialize_messages_array(builder, messages, system_prompt, flags);

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(root));

    obj = json_node_get_object(root);
    /* json_object_get_array_member returns a borrowed pointer; we extend its
     * lifetime by ref-ing the root JsonNode via dup. */
    return json_array_ref(json_object_get_array_member(obj, "messages"));
}

static JsonArray *
serialize(GList *messages, const gchar *system_prompt)
{
    return serialize_with_flags(messages, system_prompt, AI_OPENAI_SERIALIZE_DEFAULT);
}

/* ================================================================
 * Tests
 * ================================================================ */

static void
test_pure_text_user(void)
{
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
    GList *list = g_list_append(NULL, msg);
    g_autoptr(JsonArray) arr = serialize(list, NULL);
    JsonObject *wire;

    g_list_free(list);

    g_assert_cmpuint(json_array_get_length(arr), ==, 1);
    wire = json_array_get_object_element(arr, 0);
    g_assert_cmpstr(json_object_get_string_member(wire, "role"), ==, "user");
    g_assert_cmpstr(json_object_get_string_member(wire, "content"), ==, "Hello!");
    g_assert_false(json_object_has_member(wire, "tool_calls"));
}

static void
test_system_prompt_prepended(void)
{
    g_autoptr(AiMessage) msg = ai_message_new_user("Hi");
    GList *list = g_list_append(NULL, msg);
    g_autoptr(JsonArray) arr = serialize(list, "You are helpful.");
    JsonObject *sys, *user;

    g_list_free(list);

    g_assert_cmpuint(json_array_get_length(arr), ==, 2);
    sys = json_array_get_object_element(arr, 0);
    g_assert_cmpstr(json_object_get_string_member(sys, "role"), ==, "system");
    g_assert_cmpstr(json_object_get_string_member(sys, "content"), ==,
                    "You are helpful.");

    user = json_array_get_object_element(arr, 1);
    g_assert_cmpstr(json_object_get_string_member(user, "role"), ==, "user");
}

static void
test_assistant_with_two_tool_uses(void)
{
    g_autoptr(AiMessage) msg = ai_message_new(AI_ROLE_ASSISTANT);
    g_autoptr(AiTextContent) txt = ai_text_content_new("I'll use tools.");
    g_autoptr(AiToolUse) t1 = ai_tool_use_new_from_json_string(
        "call_1", "get_weather", "{\"city\":\"Miami\"}");
    g_autoptr(AiToolUse) t2 = ai_tool_use_new_from_json_string(
        "call_2", "calc", "{\"a\":13,\"b\":7}");
    GList *list;
    g_autoptr(JsonArray) arr = NULL;
    JsonObject *wire;
    JsonArray *calls;
    JsonObject *call1, *call2, *fn1, *fn2;
    const gchar *args1, *args2;
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *parsed;

    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&txt));
    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&t1));
    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&t2));

    list = g_list_append(NULL, msg);
    arr = serialize(list, NULL);
    g_list_free(list);

    g_assert_cmpuint(json_array_get_length(arr), ==, 1);
    wire = json_array_get_object_element(arr, 0);
    g_assert_cmpstr(json_object_get_string_member(wire, "role"), ==, "assistant");
    g_assert_cmpstr(json_object_get_string_member(wire, "content"), ==,
                    "I'll use tools.");

    g_assert_true(json_object_has_member(wire, "tool_calls"));
    calls = json_object_get_array_member(wire, "tool_calls");
    g_assert_cmpuint(json_array_get_length(calls), ==, 2);

    call1 = json_array_get_object_element(calls, 0);
    g_assert_cmpstr(json_object_get_string_member(call1, "id"), ==, "call_1");
    g_assert_cmpstr(json_object_get_string_member(call1, "type"), ==, "function");
    fn1 = json_object_get_object_member(call1, "function");
    g_assert_cmpstr(json_object_get_string_member(fn1, "name"), ==, "get_weather");
    args1 = json_object_get_string_member(fn1, "arguments");
    g_assert_nonnull(args1);
    /* Crucial: arguments must be a STRING, not a nested object. */
    g_assert_true(json_parser_load_from_data(parser, args1, -1, NULL));
    parsed = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpstr(json_object_get_string_member(parsed, "city"), ==, "Miami");

    call2 = json_array_get_object_element(calls, 1);
    fn2 = json_object_get_object_member(call2, "function");
    g_assert_cmpstr(json_object_get_string_member(fn2, "name"), ==, "calc");
    args2 = json_object_get_string_member(fn2, "arguments");
    g_assert_true(json_parser_load_from_data(parser, args2, -1, NULL));
    parsed = json_node_get_object(json_parser_get_root(parser));
    g_assert_cmpint(json_object_get_int_member(parsed, "a"), ==, 13);
}

static void
test_assistant_tool_call_only_null_content(void)
{
    /* Assistant with no text, only tool_use, must emit content: null. */
    g_autoptr(AiMessage) msg = ai_message_new(AI_ROLE_ASSISTANT);
    g_autoptr(AiToolUse) tu = ai_tool_use_new_from_json_string(
        "call_x", "foo", "{}");
    GList *list;
    g_autoptr(JsonArray) arr = NULL;
    JsonObject *wire;
    JsonNode *content_node;

    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&tu));
    list = g_list_append(NULL, msg);
    arr = serialize(list, NULL);
    g_list_free(list);

    wire = json_array_get_object_element(arr, 0);
    content_node = json_object_get_member(wire, "content");
    g_assert_nonnull(content_node);
    g_assert_true(JSON_NODE_HOLDS_NULL(content_node));
    g_assert_true(json_object_has_member(wire, "tool_calls"));
}

static void
test_user_with_two_tool_results(void)
{
    /* One user AiMessage carrying two tool_result blocks must become TWO
     * separate role:"tool" wire messages with correct ids. */
    g_autoptr(AiMessage) msg = ai_message_new(AI_ROLE_USER);
    g_autoptr(AiToolResult) r1 = ai_tool_result_new("call_1", "sunny, 72F", FALSE);
    g_autoptr(AiToolResult) r2 = ai_tool_result_new("call_2", "91", FALSE);
    GList *list;
    g_autoptr(JsonArray) arr = NULL;
    JsonObject *w1, *w2;

    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&r1));
    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&r2));

    list = g_list_append(NULL, msg);
    arr = serialize(list, NULL);
    g_list_free(list);

    g_assert_cmpuint(json_array_get_length(arr), ==, 2);

    w1 = json_array_get_object_element(arr, 0);
    g_assert_cmpstr(json_object_get_string_member(w1, "role"), ==, "tool");
    g_assert_cmpstr(json_object_get_string_member(w1, "tool_call_id"), ==,
                    "call_1");
    g_assert_cmpstr(json_object_get_string_member(w1, "content"), ==,
                    "sunny, 72F");
    /* No nested content arrays. */
    g_assert_false(json_object_has_member(w1, "type"));

    w2 = json_array_get_object_element(arr, 1);
    g_assert_cmpstr(json_object_get_string_member(w2, "role"), ==, "tool");
    g_assert_cmpstr(json_object_get_string_member(w2, "tool_call_id"), ==,
                    "call_2");
}

static void
test_mixed_transcript(void)
{
    /* system + user + assistant(text+tool_use) + user(tool_result) + assistant(text)
     * should ordered exactly: system, user, assistant, tool, assistant. */
    g_autoptr(AiMessage) user1 = ai_message_new_user("What's the weather?");
    g_autoptr(AiMessage) asst1 = ai_message_new(AI_ROLE_ASSISTANT);
    g_autoptr(AiTextContent) asst1_txt = ai_text_content_new("Looking up.");
    g_autoptr(AiToolUse) asst1_tu = ai_tool_use_new_from_json_string(
        "call_w", "get_weather", "{\"city\":\"Miami\"}");
    g_autoptr(AiMessage) user2 = ai_message_new_tool_result_with_name(
        "call_w", "get_weather", "sunny, 82F", FALSE);
    g_autoptr(AiMessage) asst2 = ai_message_new_assistant("It's sunny.");
    GList *list = NULL;
    g_autoptr(JsonArray) arr = NULL;

    ai_message_add_content_block(asst1, (AiContentBlock *)g_steal_pointer(&asst1_txt));
    ai_message_add_content_block(asst1, (AiContentBlock *)g_steal_pointer(&asst1_tu));

    list = g_list_append(list, user1);
    list = g_list_append(list, asst1);
    list = g_list_append(list, user2);
    list = g_list_append(list, asst2);

    arr = serialize(list, "You help users.");
    g_list_free(list);

    g_assert_cmpuint(json_array_get_length(arr), ==, 5);
    g_assert_cmpstr(json_object_get_string_member(
        json_array_get_object_element(arr, 0), "role"), ==, "system");
    g_assert_cmpstr(json_object_get_string_member(
        json_array_get_object_element(arr, 1), "role"), ==, "user");
    g_assert_cmpstr(json_object_get_string_member(
        json_array_get_object_element(arr, 2), "role"), ==, "assistant");
    g_assert_cmpstr(json_object_get_string_member(
        json_array_get_object_element(arr, 3), "role"), ==, "tool");
    g_assert_cmpstr(json_object_get_string_member(
        json_array_get_object_element(arr, 4), "role"), ==, "assistant");
}

static void
test_args_as_object_for_ollama(void)
{
    /* Ollama's /api/chat requires arguments as a JSON OBJECT, not a string. */
    g_autoptr(AiMessage) msg = ai_message_new(AI_ROLE_ASSISTANT);
    g_autoptr(AiToolUse) tu = ai_tool_use_new_from_json_string(
        "call_1", "calc", "{\"a\":13,\"b\":7}");
    GList *list;
    g_autoptr(JsonArray) arr = NULL;
    JsonObject *wire, *call, *fn;
    JsonNode *args_node;
    JsonObject *args;

    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&tu));
    list = g_list_append(NULL, msg);

    arr = serialize_with_flags(list, NULL, AI_OPENAI_SERIALIZE_ARGS_AS_OBJECT);
    g_list_free(list);

    wire = json_array_get_object_element(arr, 0);
    call = json_array_get_object_element(
        json_object_get_array_member(wire, "tool_calls"), 0);
    fn = json_object_get_object_member(call, "function");

    args_node = json_object_get_member(fn, "arguments");
    g_assert_nonnull(args_node);
    /* The whole point: arguments must be an OBJECT, not a string. */
    g_assert_true(JSON_NODE_HOLDS_OBJECT(args_node));

    args = json_node_get_object(args_node);
    g_assert_cmpint(json_object_get_int_member(args, "a"), ==, 13);
    g_assert_cmpint(json_object_get_int_member(args, "b"), ==, 7);
}

/* ================================================================
 * Vision
 * ================================================================ */

/*
 * A user message carrying a screenshot must serialize as a content ARRAY of
 * parts -- text plus image_url with a data: URL -- not as a plain string.
 * Serialized as a string, the image silently disappears and the model
 * answers about a screenshot it was never shown.
 */
static void
test_user_with_image(void)
{
    g_autoptr(AiMessage) msg = ai_message_new_user("What is in this ad?");
    g_autoptr(GBytes) bytes = g_bytes_new_static("\x89PNGfake", 9);
    g_autoptr(AiImageContent) image = ai_image_content_new_from_bytes(bytes, "image/png");
    g_autoptr(JsonArray) messages = NULL;
    GList *list = NULL;
    JsonObject *user;
    JsonArray *content;
    JsonObject *part;
    const gchar *url;

    ai_message_add_content_block(msg, AI_CONTENT_BLOCK(g_object_ref(image)));

    list = g_list_append(list, msg);
    messages = serialize_with_flags(list, NULL, AI_OPENAI_SERIALIZE_DEFAULT);
    g_list_free(list);

    g_assert_cmpint((gint)json_array_get_length(messages), ==, 1);

    user = json_array_get_object_element(messages, 0);
    g_assert_cmpstr(json_object_get_string_member(user, "role"), ==, "user");

    /* The content is an array, and the text survives beside the image. */
    content = json_object_get_array_member(user, "content");
    g_assert_cmpint((gint)json_array_get_length(content), ==, 2);

    part = json_array_get_object_element(content, 0);
    g_assert_cmpstr(json_object_get_string_member(part, "type"), ==, "text");
    g_assert_cmpstr(json_object_get_string_member(part, "text"), ==,
                    "What is in this ad?");

    part = json_array_get_object_element(content, 1);
    g_assert_cmpstr(json_object_get_string_member(part, "type"), ==, "image_url");

    url = json_object_get_string_member(
        json_object_get_object_member(part, "image_url"), "url");
    g_assert_true(g_str_has_prefix(url, "data:image/png;base64,"));
}

/*
 * The generic (Anthropic) path spells the same image differently: an image
 * block with a base64 source. Both providers must be reachable from one
 * message, since which one is in use is a configuration value.
 */
static void
test_image_block_anthropic_shape(void)
{
    g_autoptr(GBytes) bytes = g_bytes_new_static("\x89PNGfake", 9);
    g_autoptr(AiImageContent) image = ai_image_content_new_from_bytes(bytes, "image/webp");
    g_autoptr(JsonNode) node = ai_content_block_to_json(AI_CONTENT_BLOCK(image));
    JsonObject *obj;
    JsonObject *source;

    g_assert_true(JSON_NODE_HOLDS_OBJECT(node));
    obj = json_node_get_object(node);

    g_assert_cmpstr(json_object_get_string_member(obj, "type"), ==, "image");

    source = json_object_get_object_member(obj, "source");
    g_assert_cmpstr(json_object_get_string_member(source, "type"), ==, "base64");
    g_assert_cmpstr(json_object_get_string_member(source, "media_type"), ==,
                    "image/webp");
    g_assert_cmpuint(strlen(json_object_get_string_member(source, "data")), >, 0);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/openai-serialize/pure-text-user", test_pure_text_user);
    g_test_add_func("/openai-serialize/system-prompt", test_system_prompt_prepended);
    g_test_add_func("/openai-serialize/assistant-two-tool-uses",
                    test_assistant_with_two_tool_uses);
    g_test_add_func("/openai-serialize/assistant-tool-call-only-null-content",
                    test_assistant_tool_call_only_null_content);
    g_test_add_func("/openai-serialize/user-two-tool-results",
                    test_user_with_two_tool_results);
    g_test_add_func("/openai-serialize/mixed-transcript",
                    test_mixed_transcript);
    g_test_add_func("/openai-serialize/user-with-image", test_user_with_image);
    g_test_add_func("/openai-serialize/image-block-anthropic-shape",
                    test_image_block_anthropic_shape);
    g_test_add_func("/openai-serialize/args-as-object-ollama",
                    test_args_as_object_for_ollama);

    return g_test_run();
}
