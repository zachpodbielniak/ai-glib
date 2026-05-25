/*
 * test-gemini-serialize.c - Unit tests for Gemini request serialization
 *
 * Verifies that the Gemini provider builds requests using Google's
 * { contents: [...], tools: [{functionDeclarations}], ... } wire shape
 * including functionCall / functionResponse parts.
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <json-glib/json-glib.h>

#include "ai-glib.h"
#include "providers/ai-gemini-client.h"
#include "core/ai-client.h"
#include "model/ai-message.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"
#include "model/ai-tool.h"
#include "core/ai-enums.h"

/* Reach into the AiClient vtable for the build_request hook so we can test
 * serialization without HTTP. */
typedef JsonNode * (*BuildRequestFn)(
    AiClient    *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    GList       *tools
);

static JsonNode *
build_request(AiGeminiClient *client, GList *messages, const gchar *system_prompt,
              GList *tools)
{
    AiClientClass *klass = AI_CLIENT_GET_CLASS(client);
    BuildRequestFn fn = (BuildRequestFn)klass->build_request;

    g_assert_nonnull(fn);
    return fn(AI_CLIENT(client), messages, system_prompt, 4096, tools);
}

static AiGeminiClient *
make_client(void)
{
    /* Use a dummy API key — we never hit the network here. */
    g_autoptr(AiConfig) cfg = ai_config_new();
    ai_config_set_api_key(cfg, AI_PROVIDER_GEMINI, "test-key");
    return ai_gemini_client_new_with_config(cfg);
}

/* ================================================================
 * Tests
 * ================================================================ */

static void
test_simple_user_text(void)
{
    g_autoptr(AiGeminiClient) client = make_client();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
    GList *list = g_list_append(NULL, msg);
    g_autoptr(JsonNode) req = build_request(client, list, NULL, NULL);
    JsonObject *root, *content, *part;
    JsonArray *contents, *parts;

    g_list_free(list);

    g_assert_true(JSON_NODE_HOLDS_OBJECT(req));
    root = json_node_get_object(req);
    contents = json_object_get_array_member(root, "contents");
    g_assert_cmpuint(json_array_get_length(contents), ==, 1);

    content = json_array_get_object_element(contents, 0);
    g_assert_cmpstr(json_object_get_string_member(content, "role"), ==, "user");
    parts = json_object_get_array_member(content, "parts");
    g_assert_cmpuint(json_array_get_length(parts), ==, 1);
    part = json_array_get_object_element(parts, 0);
    g_assert_cmpstr(json_object_get_string_member(part, "text"), ==, "Hello!");

    g_assert_false(json_object_has_member(root, "tools"));
}

static void
test_assistant_with_function_call(void)
{
    g_autoptr(AiGeminiClient) client = make_client();
    g_autoptr(AiMessage) msg = ai_message_new(AI_ROLE_ASSISTANT);
    g_autoptr(AiTextContent) txt = ai_text_content_new("Looking up.");
    g_autoptr(AiToolUse) tu = ai_tool_use_new_from_json_string(
        "call_x", "get_weather", "{\"city\":\"Miami\"}");
    GList *list;
    g_autoptr(JsonNode) req = NULL;
    JsonObject *root, *content, *part0, *part1, *fc, *args;
    JsonArray *contents, *parts;

    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&txt));
    ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&tu));

    list = g_list_append(NULL, msg);
    req = build_request(client, list, NULL, NULL);
    g_list_free(list);

    root = json_node_get_object(req);
    contents = json_object_get_array_member(root, "contents");
    content = json_array_get_object_element(contents, 0);
    g_assert_cmpstr(json_object_get_string_member(content, "role"), ==, "model");

    parts = json_object_get_array_member(content, "parts");
    g_assert_cmpuint(json_array_get_length(parts), ==, 2);

    part0 = json_array_get_object_element(parts, 0);
    g_assert_cmpstr(json_object_get_string_member(part0, "text"), ==,
                    "Looking up.");

    part1 = json_array_get_object_element(parts, 1);
    g_assert_true(json_object_has_member(part1, "functionCall"));
    fc = json_object_get_object_member(part1, "functionCall");
    g_assert_cmpstr(json_object_get_string_member(fc, "name"), ==, "get_weather");
    args = json_object_get_object_member(fc, "args");
    g_assert_cmpstr(json_object_get_string_member(args, "city"), ==, "Miami");
}

static void
test_tool_result_with_name(void)
{
    g_autoptr(AiGeminiClient) client = make_client();
    g_autoptr(AiMessage) msg = ai_message_new_tool_result_with_name(
        "call_x", "get_weather", "sunny, 82F", FALSE);
    GList *list = g_list_append(NULL, msg);
    g_autoptr(JsonNode) req = NULL;
    JsonObject *root, *content, *part, *fr, *response;
    JsonArray *contents, *parts;

    req = build_request(client, list, NULL, NULL);
    g_list_free(list);

    root = json_node_get_object(req);
    contents = json_object_get_array_member(root, "contents");
    content = json_array_get_object_element(contents, 0);
    g_assert_cmpstr(json_object_get_string_member(content, "role"), ==, "user");

    parts = json_object_get_array_member(content, "parts");
    part = json_array_get_object_element(parts, 0);
    g_assert_true(json_object_has_member(part, "functionResponse"));

    fr = json_object_get_object_member(part, "functionResponse");
    g_assert_cmpstr(json_object_get_string_member(fr, "name"), ==, "get_weather");
    response = json_object_get_object_member(fr, "response");
    g_assert_cmpstr(json_object_get_string_member(response, "output"), ==,
                    "sunny, 82F");
    g_assert_false(json_object_has_member(response, "error"));
}

static void
test_tool_result_error_uses_error_key(void)
{
    g_autoptr(AiGeminiClient) client = make_client();
    g_autoptr(AiMessage) msg = ai_message_new_tool_result_with_name(
        "call_x", "do_thing", "permission denied", TRUE);
    GList *list = g_list_append(NULL, msg);
    g_autoptr(JsonNode) req = NULL;
    JsonObject *root, *content, *part, *fr, *response;
    JsonArray *contents, *parts;

    req = build_request(client, list, NULL, NULL);
    g_list_free(list);

    root = json_node_get_object(req);
    contents = json_object_get_array_member(root, "contents");
    content = json_array_get_object_element(contents, 0);
    parts = json_object_get_array_member(content, "parts");
    part = json_array_get_object_element(parts, 0);
    fr = json_object_get_object_member(part, "functionResponse");
    response = json_object_get_object_member(fr, "response");

    g_assert_cmpstr(json_object_get_string_member(response, "error"), ==,
                    "permission denied");
    g_assert_false(json_object_has_member(response, "output"));
}

static void
test_tools_wrapped_in_function_declarations(void)
{
    g_autoptr(AiGeminiClient) client = make_client();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hi");
    g_autoptr(AiTool) tool = ai_tool_new("get_weather", "Get current weather");
    GList *tools = g_list_append(NULL, tool);
    GList *list = g_list_append(NULL, msg);
    g_autoptr(JsonNode) req = NULL;
    JsonObject *root, *tools_entry, *fd;
    JsonArray *tools_arr, *fd_arr;

    ai_tool_add_parameter(tool, "city", "string", "City name", TRUE);

    req = build_request(client, list, NULL, tools);
    g_list_free(list);
    g_list_free(tools);

    root = json_node_get_object(req);
    g_assert_true(json_object_has_member(root, "tools"));
    tools_arr = json_object_get_array_member(root, "tools");
    g_assert_cmpuint(json_array_get_length(tools_arr), ==, 1);

    tools_entry = json_array_get_object_element(tools_arr, 0);
    g_assert_true(json_object_has_member(tools_entry, "functionDeclarations"));

    fd_arr = json_object_get_array_member(tools_entry, "functionDeclarations");
    g_assert_cmpuint(json_array_get_length(fd_arr), ==, 1);
    fd = json_array_get_object_element(fd_arr, 0);
    g_assert_cmpstr(json_object_get_string_member(fd, "name"), ==, "get_weather");
}

static void
test_tool_result_name_fallback_from_prior_tool_use(void)
{
    /* Caller forgot to pass tool_name to the result message — the serializer
     * must walk earlier assistant messages to recover it. */
    g_autoptr(AiGeminiClient) client = make_client();
    g_autoptr(AiMessage) asst = ai_message_new(AI_ROLE_ASSISTANT);
    g_autoptr(AiToolUse) tu = ai_tool_use_new_from_json_string(
        "call_x", "get_weather", "{}");
    g_autoptr(AiMessage) result = ai_message_new_tool_result(
        "call_x", "sunny", FALSE);
    GList *list = NULL;
    g_autoptr(JsonNode) req = NULL;
    JsonObject *root, *content, *part, *fr;
    JsonArray *contents, *parts;

    ai_message_add_content_block(asst, (AiContentBlock *)g_steal_pointer(&tu));
    list = g_list_append(list, asst);
    list = g_list_append(list, result);

    req = build_request(client, list, NULL, NULL);
    g_list_free(list);

    root = json_node_get_object(req);
    contents = json_object_get_array_member(root, "contents");
    g_assert_cmpuint(json_array_get_length(contents), ==, 2);

    content = json_array_get_object_element(contents, 1);
    parts = json_object_get_array_member(content, "parts");
    part = json_array_get_object_element(parts, 0);
    fr = json_object_get_object_member(part, "functionResponse");
    g_assert_cmpstr(json_object_get_string_member(fr, "name"), ==, "get_weather");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/gemini-serialize/simple-user-text", test_simple_user_text);
    g_test_add_func("/gemini-serialize/assistant-function-call",
                    test_assistant_with_function_call);
    g_test_add_func("/gemini-serialize/tool-result-with-name",
                    test_tool_result_with_name);
    g_test_add_func("/gemini-serialize/tool-result-error",
                    test_tool_result_error_uses_error_key);
    g_test_add_func("/gemini-serialize/tools-wrapped",
                    test_tools_wrapped_in_function_declarations);
    g_test_add_func("/gemini-serialize/tool-name-fallback",
                    test_tool_result_name_fallback_from_prior_tool_use);

    return g_test_run();
}
