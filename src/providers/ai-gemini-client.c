/*
 * ai-gemini-client.c - Google Gemini client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "providers/ai-gemini-client.h"
#include "providers/ai-image-shared.h"
#include "core/ai-error.h"
#include "core/ai-image-generator.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"
#include "model/ai-image-request.h"
#include "model/ai-generated-image.h"
#include "model/ai-image-response.h"

/*
 * Private structure for AiGeminiClient.
 */
struct _AiGeminiClient
{
    AiClient parent_instance;
};

static void ai_gemini_client_provider_init(AiProviderInterface *iface);
static void ai_gemini_client_streamable_init(AiStreamableInterface *iface);
static void ai_gemini_client_image_generator_init(AiImageGeneratorInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiGeminiClient, ai_gemini_client, AI_TYPE_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_gemini_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_gemini_client_streamable_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_IMAGE_GENERATOR,
                                              ai_gemini_client_image_generator_init))

/*
 * Resolve a tool name for a tool_result whose AiToolResult was not given a
 * tool-name. Falls back to scanning earlier assistant messages for an
 * AiToolUse with a matching id. Returns "" if nothing matches (Gemini
 * requires the field, so we ship something).
 */
static const gchar *
resolve_tool_name(
    GList        *messages,
    GList        *up_to,        /* stop before this list node */
    const gchar  *tool_use_id
){
    GList *m;

    if (tool_use_id == NULL)
    {
        return "";
    }

    for (m = messages; m != NULL && m != up_to; m = m->next)
    {
        AiMessage *msg = m->data;
        GList *b;

        if (ai_message_get_role(msg) != AI_ROLE_ASSISTANT)
        {
            continue;
        }

        for (b = ai_message_get_content_blocks(msg); b != NULL; b = b->next)
        {
            AiContentBlock *block = b->data;

            if (AI_IS_TOOL_USE(block))
            {
                AiToolUse *tu = AI_TOOL_USE(block);
                const gchar *id = ai_tool_use_get_id(tu);

                if (g_strcmp0(id, tool_use_id) == 0)
                {
                    return ai_tool_use_get_name(tu);
                }
            }
        }
    }

    return "";
}

/*
 * Build Gemini API request.
 * Gemini uses { contents: [...], systemInstruction: {...}, tools: [...],
 *              generationConfig: {...} }
 *
 * Per-message expansion:
 *   role==assistant -> {"role":"model","parts":[{text|functionCall}, ...]}
 *   role==user      -> {"role":"user","parts":[{text|functionResponse}, ...]}
 */
static JsonNode *
ai_gemini_client_build_request(
    AiClient    *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    GList       *tools
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    GList *l;

    json_builder_begin_object(builder);

    /* Contents (messages) */
    json_builder_set_member_name(builder, "contents");
    json_builder_begin_array(builder);

    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        AiRole role = ai_message_get_role(msg);
        GList *blocks = ai_message_get_content_blocks(msg);
        GList *b;

        /* System messages are surfaced via top-level systemInstruction; skip
         * them in contents. */
        if (role == AI_ROLE_SYSTEM)
        {
            continue;
        }

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "role");
        if (role == AI_ROLE_ASSISTANT)
        {
            json_builder_add_string_value(builder, "model");
        }
        else
        {
            json_builder_add_string_value(builder, "user");
        }

        json_builder_set_member_name(builder, "parts");
        json_builder_begin_array(builder);

        for (b = blocks; b != NULL; b = b->next)
        {
            AiContentBlock *block = b->data;

            if (AI_IS_TEXT_CONTENT(block))
            {
                const gchar *text = ai_text_content_get_text(AI_TEXT_CONTENT(block));

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "text");
                json_builder_add_string_value(builder, text != NULL ? text : "");
                json_builder_end_object(builder);
            }
            else if (AI_IS_TOOL_USE(block) && role == AI_ROLE_ASSISTANT)
            {
                AiToolUse *tu = AI_TOOL_USE(block);
                const gchar *name = ai_tool_use_get_name(tu);
                JsonNode *input = ai_tool_use_get_input(tu);

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "functionCall");
                json_builder_begin_object(builder);

                json_builder_set_member_name(builder, "name");
                json_builder_add_string_value(builder, name != NULL ? name : "");

                json_builder_set_member_name(builder, "args");
                if (input != NULL && JSON_NODE_HOLDS_OBJECT(input))
                {
                    json_builder_add_value(builder, json_node_copy(input));
                }
                else
                {
                    json_builder_begin_object(builder);
                    json_builder_end_object(builder);
                }

                json_builder_end_object(builder); /* functionCall */
                json_builder_end_object(builder); /* part */
            }
            else if (AI_IS_TOOL_RESULT(block) && role == AI_ROLE_USER)
            {
                AiToolResult *tr = AI_TOOL_RESULT(block);
                const gchar *name = ai_tool_result_get_tool_name(tr);
                const gchar *content = ai_tool_result_get_content(tr);
                gboolean is_error = ai_tool_result_get_is_error(tr);

                if (name == NULL || name[0] == '\0')
                {
                    name = resolve_tool_name(messages, l,
                                             ai_tool_result_get_tool_use_id(tr));
                }

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "functionResponse");
                json_builder_begin_object(builder);

                json_builder_set_member_name(builder, "name");
                json_builder_add_string_value(builder, name);

                json_builder_set_member_name(builder, "response");
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, is_error ? "error" : "output");
                json_builder_add_string_value(builder, content != NULL ? content : "");
                json_builder_end_object(builder);

                json_builder_end_object(builder); /* functionResponse */
                json_builder_end_object(builder); /* part */
            }
        }

        /* Gemini requires at least one part per content. */
        if (blocks == NULL)
        {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "text");
            json_builder_add_string_value(builder, "");
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder); /* parts */
        json_builder_end_object(builder); /* content */
    }

    json_builder_end_array(builder); /* contents */

    /* System instruction */
    if (system_prompt != NULL && system_prompt[0] != '\0')
    {
        json_builder_set_member_name(builder, "systemInstruction");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "parts");
        json_builder_begin_array(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "text");
        json_builder_add_string_value(builder, system_prompt);
        json_builder_end_object(builder);
        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }

    /* Tools — Gemini wraps the function declaration list in a single
     * tools[0].functionDeclarations array. */
    if (tools != NULL)
    {
        json_builder_set_member_name(builder, "tools");
        json_builder_begin_array(builder);
        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "functionDeclarations");
        json_builder_begin_array(builder);

        for (l = tools; l != NULL; l = l->next)
        {
            AiTool *tool = l->data;
            g_autoptr(JsonNode) tool_node = ai_tool_to_json(tool, AI_PROVIDER_GEMINI);

            json_builder_add_value(builder, g_steal_pointer(&tool_node));
        }

        json_builder_end_array(builder); /* functionDeclarations */
        json_builder_end_object(builder); /* tools[0] */
        json_builder_end_array(builder); /* tools */
    }

    /* Generation config */
    json_builder_set_member_name(builder, "generationConfig");
    json_builder_begin_object(builder);

    if (max_tokens > 0)
    {
        json_builder_set_member_name(builder, "maxOutputTokens");
        json_builder_add_int_value(builder, max_tokens);
    }

    {
        gdouble temp = ai_client_get_temperature(client);
        if (temp != 1.0)
        {
            json_builder_set_member_name(builder, "temperature");
            json_builder_add_double_value(builder, temp);
        }
    }

    json_builder_end_object(builder);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * Parse Gemini response.
 */
static AiResponse *
ai_gemini_client_parse_response(
    AiClient  *client,
    JsonNode  *json,
    GError   **error
){
    JsonObject *obj;
    g_autoptr(AiResponse) response = NULL;

    (void)client;

    if (!JSON_NODE_HOLDS_OBJECT(json))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Expected JSON object in response");
        return NULL;
    }

    obj = json_node_get_object(json);

    /* Check for error */
    if (json_object_has_member(obj, "error"))
    {
        JsonObject *err_obj = json_object_get_object_member(obj, "error");
        const gchar *err_msg = json_object_get_string_member_with_default(
            err_obj, "message", "Unknown error");

        g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", err_msg);
        return NULL;
    }

    response = ai_response_new("", "");

    /* Parse candidates */
    if (json_object_has_member(obj, "candidates"))
    {
        JsonArray *candidates = json_object_get_array_member(obj, "candidates");

        if (json_array_get_length(candidates) > 0)
        {
            JsonObject *candidate = json_array_get_object_element(candidates, 0);
            const gchar *finish_reason = json_object_get_string_member_with_default(
                candidate, "finishReason", "");

            if (g_strcmp0(finish_reason, "STOP") == 0)
            {
                ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
            }
            else if (g_strcmp0(finish_reason, "MAX_TOKENS") == 0)
            {
                ai_response_set_stop_reason(response, AI_STOP_REASON_MAX_TOKENS);
            }

            /* Parse content */
            if (json_object_has_member(candidate, "content"))
            {
                JsonObject *content = json_object_get_object_member(candidate, "content");
                gboolean tool_use_present = FALSE;

                if (json_object_has_member(content, "parts"))
                {
                    JsonArray *parts = json_object_get_array_member(content, "parts");
                    guint len = json_array_get_length(parts);
                    guint i;

                    for (i = 0; i < len; i++)
                    {
                        JsonObject *part = json_array_get_object_element(parts, i);

                        if (json_object_has_member(part, "text"))
                        {
                            const gchar *text = json_object_get_string_member(part, "text");
                            g_autoptr(AiTextContent) text_content = ai_text_content_new(text);

                            ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&text_content));
                        }
                        else if (json_object_has_member(part, "functionCall"))
                        {
                            JsonObject *fc = json_object_get_object_member(part, "functionCall");
                            const gchar *name = json_object_get_string_member_with_default(fc, "name", "");
                            const gchar *provided_id = json_object_get_string_member_with_default(fc, "id", NULL);
                            JsonNode *args = json_object_has_member(fc, "args")
                                ? json_object_get_member(fc, "args") : NULL;
                            g_autofree gchar *synthetic_id = NULL;
                            const gchar *id;
                            g_autoptr(AiToolUse) tool_use = NULL;

                            if (provided_id != NULL && provided_id[0] != '\0')
                            {
                                id = provided_id;
                            }
                            else
                            {
                                synthetic_id = g_uuid_string_random();
                                id = synthetic_id;
                            }

                            tool_use = ai_tool_use_new(id, name, args);
                            ai_response_add_content_block(response,
                                (AiContentBlock *)g_steal_pointer(&tool_use));
                            tool_use_present = TRUE;
                        }
                    }
                }

                if (tool_use_present)
                {
                    ai_response_set_stop_reason(response, AI_STOP_REASON_TOOL_USE);
                }
            }
        }
    }

    /* Parse usage */
    if (json_object_has_member(obj, "usageMetadata"))
    {
        JsonObject *usage_obj = json_object_get_object_member(obj, "usageMetadata");
        gint prompt_tokens = json_object_get_int_member_with_default(usage_obj, "promptTokenCount", 0);
        gint output_tokens = json_object_get_int_member_with_default(usage_obj, "candidatesTokenCount", 0);
        g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, output_tokens);

        ai_response_set_usage(response, usage);
    }

    return (AiResponse *)g_steal_pointer(&response);
}

static gchar *
ai_gemini_client_get_endpoint_url(AiClient *client)
{
    AiConfig *config = ai_client_get_config(client);
    const gchar *base_url = ai_config_get_base_url(config, AI_PROVIDER_GEMINI);
    const gchar *model = ai_client_get_model(client);
    const gchar *api_key = ai_config_get_api_key(config, AI_PROVIDER_GEMINI);

    if (model == NULL)
    {
        model = AI_GEMINI_DEFAULT_MODEL;
    }

    return g_strdup_printf("%s/v1beta/models/%s:generateContent?key=%s",
                           base_url, model, api_key != NULL ? api_key : "");
}

static void
ai_gemini_client_add_auth_headers(
    AiClient    *client,
    SoupMessage *msg
){
    /* Gemini uses API key in URL, not headers */
    (void)client;
    (void)msg;
}

static void
ai_gemini_client_class_init(AiGeminiClientClass *klass)
{
    AiClientClass *client_class = AI_CLIENT_CLASS(klass);

    client_class->build_request = ai_gemini_client_build_request;
    client_class->parse_response = ai_gemini_client_parse_response;
    client_class->get_endpoint_url = ai_gemini_client_get_endpoint_url;
    client_class->add_auth_headers = ai_gemini_client_add_auth_headers;
}

static void
ai_gemini_client_init(AiGeminiClient *self)
{
    (void)self;
    ai_client_set_model(AI_CLIENT(self), AI_GEMINI_DEFAULT_MODEL);
}

/*
 * AiProvider interface
 */

static AiProviderType
ai_gemini_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_GEMINI;
}

static const gchar *
ai_gemini_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "Gemini";
}

static const gchar *
ai_gemini_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_GEMINI_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiGeminiClient *client;
    GTask          *task;
    SoupMessage    *msg;
} GeminiChatAsyncData;

static void
gemini_chat_async_data_free(GeminiChatAsyncData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->msg);
    g_slice_free(GeminiChatAsyncData, data);
}

static void
on_gemini_chat_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GeminiChatAsyncData *data = user_data;
    g_autoptr(GBytes) response_bytes = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = NULL;
    SoupMessage *msg = data->msg;
    const gchar *response_data;
    gsize response_len;
    JsonNode *response_json;
    AiClientClass *klass;
    AiResponse *response;

    (void)source;

    response_bytes = soup_session_send_and_read_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)), result, &error);

    if (response_bytes == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_chat_async_data_free(data);
        return;
    }

    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(msg)))
    {
        guint status = soup_message_get_status(msg);

        if (status == 401 || status == 403)
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                                    "Authentication failed (HTTP %u)", status);
        }
        else if (status == 429)
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_RATE_LIMITED,
                                    "Rate limited (HTTP %u)", status);
        }
        else
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_NETWORK_ERROR,
                                    "Request failed (HTTP %u)", status);
        }

        gemini_chat_async_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_chat_async_data_free(data);
        return;
    }

    response_json = json_parser_get_root(parser);
    klass = AI_CLIENT_GET_CLASS(data->client);

    response = klass->parse_response(AI_CLIENT(data->client), response_json, &error);
    if (response == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
    }
    else
    {
        g_task_return_pointer(data->task, response, g_object_unref);
    }

    gemini_chat_async_data_free(data);
}

static void
ai_gemini_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiGeminiClient *self = AI_GEMINI_CLIENT(provider);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    GeminiChatAsyncData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    request_json = klass->build_request(AI_CLIENT(self), messages, system_prompt,
                                        max_tokens, tools);
    if (request_json == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build request");
        g_object_unref(task);
        return;
    }

    {
        g_autoptr(JsonGenerator) gen = json_generator_new();
        json_generator_set_root(gen, request_json);
        request_body = json_generator_to_data(gen, NULL);
    }

    url = klass->get_endpoint_url(AI_CLIENT(self));

    msg = soup_message_new("POST", url);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");

    klass->add_auth_headers(AI_CLIENT(self), msg);

    soup_message_set_request_body_from_bytes(msg, "application/json",
        g_bytes_new_take(g_steal_pointer(&request_body), strlen(request_body)));

    data = g_slice_new0(GeminiChatAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_gemini_chat_response,
        data);
}

static AiResponse *
ai_gemini_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

typedef struct
{
    AiGeminiClient *client;
    GTask          *task;
    SoupMessage    *msg;
} GeminiListModelsData;

static void
gemini_list_models_data_free(GeminiListModelsData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->task);
    g_clear_object(&data->msg);
    g_slice_free(GeminiListModelsData, data);
}

static void
on_gemini_list_models_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GeminiListModelsData *data = user_data;
    g_autoptr(GBytes) response_bytes = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = NULL;
    const gchar *response_data;
    gsize response_len;
    JsonObject *root;
    JsonArray *arr;
    GList *models = NULL;
    guint i, n;

    (void)source;

    response_bytes = soup_session_send_and_read_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)), result, &error);

    if (response_bytes == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_list_models_data_free(data);
        return;
    }

    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(data->msg)))
    {
        guint status = soup_message_get_status(data->msg);

        if (status == 401 || status == 403)
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                                    "Authentication failed (HTTP %u)", status);
        }
        else
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_NETWORK_ERROR,
                                    "Model listing failed (HTTP %u)", status);
        }

        gemini_list_models_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_list_models_data_free(data);
        return;
    }

    /* {"models": [{"name": "models/gemini-...",
     *              "supportedGenerationMethods": [...]}, ...]} ---
     * keep only chat-capable models, strip the "models/" prefix. */
    root = json_node_get_object(json_parser_get_root(parser));
    arr = (root != NULL && json_object_has_member(root, "models"))
        ? json_object_get_array_member(root, "models")
        : NULL;

    if (arr == NULL)
    {
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                "Malformed model list response");
        gemini_list_models_data_free(data);
        return;
    }

    n = json_array_get_length(arr);
    for (i = 0; i < n; i++)
    {
        JsonObject *entry = json_array_get_object_element(arr, i);
        const gchar *name;
        gboolean chat_capable = FALSE;

        if (entry == NULL || !json_object_has_member(entry, "name"))
        {
            continue;
        }

        if (json_object_has_member(entry, "supportedGenerationMethods"))
        {
            JsonArray *methods =
                json_object_get_array_member(entry, "supportedGenerationMethods");
            guint j, m = json_array_get_length(methods);

            for (j = 0; j < m; j++)
            {
                if (g_strcmp0(json_array_get_string_element(methods, j),
                              "generateContent") == 0)
                {
                    chat_capable = TRUE;
                    break;
                }
            }
        }

        if (!chat_capable)
        {
            continue;
        }

        name = json_object_get_string_member(entry, "name");
        if (g_str_has_prefix(name, "models/"))
        {
            name += strlen("models/");
        }
        models = g_list_append(models, g_strdup(name));
    }

    g_task_return_pointer(data->task, models, NULL);
    gemini_list_models_data_free(data);
}

static void
ai_gemini_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiGeminiClient *self = AI_GEMINI_CLIENT(provider);
    AiConfig *config = ai_client_get_config(AI_CLIENT(self));
    const gchar *api_key = ai_config_get_api_key(config, AI_PROVIDER_GEMINI);
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    GeminiListModelsData *data;
    GTask *task;

    task = g_task_new(provider, cancellable, callback, user_data);

    url = g_strdup_printf("%s/v1beta/models?pageSize=200&key=%s",
                          ai_config_get_base_url(config, AI_PROVIDER_GEMINI),
                          api_key != NULL ? api_key : "");
    msg = soup_message_new("GET", url);

    data = g_slice_new0(GeminiListModelsData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_gemini_list_models_response,
        data);
}

static GList *
ai_gemini_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_gemini_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_gemini_client_get_provider_type;
    iface->get_name = ai_gemini_client_get_name;
    iface->get_default_model = ai_gemini_client_get_default_model;
    iface->chat_async = ai_gemini_client_chat_async;
    iface->chat_finish = ai_gemini_client_chat_finish;
    iface->list_models_async = ai_gemini_client_list_models_async;
    iface->list_models_finish = ai_gemini_client_list_models_finish;
}

/*
 * AiStreamable interface implementation
 *
 * Gemini uses SSE with streamGenerateContent endpoint.
 * Each chunk is a JSON object with candidates[0].content.parts[0].text
 */

typedef struct
{
    AiGeminiClient   *client;
    GTask            *task;
    SoupMessage      *msg;
    GInputStream     *input_stream;
    GDataInputStream *data_stream;
    GCancellable     *cancellable;

    AiResponse       *response;
    GString          *current_text;

    gboolean          stream_started;
} GeminiStreamData;

static void
gemini_stream_data_free(GeminiStreamData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->msg);
    g_clear_object(&data->input_stream);
    g_clear_object(&data->data_stream);
    g_clear_object(&data->cancellable);
    g_clear_object(&data->response);

    if (data->current_text != NULL)
    {
        g_string_free(data->current_text, TRUE);
    }

    g_slice_free(GeminiStreamData, data);
}

static void
gemini_process_stream_chunk(
    GeminiStreamData *data,
    const gchar      *json_str
){
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(GError) error = NULL;
    JsonNode *root;
    JsonObject *obj;

    if (json_str == NULL || json_str[0] == '\0')
    {
        return;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, -1, &error))
    {
        return;
    }

    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        return;
    }

    obj = json_node_get_object(root);

    if (!data->stream_started)
    {
        data->response = ai_response_new("", "");
        data->current_text = g_string_new("");
        data->stream_started = TRUE;

        g_signal_emit_by_name(data->client, "stream-start");
    }

    /* Parse candidates */
    if (json_object_has_member(obj, "candidates"))
    {
        JsonArray *candidates = json_object_get_array_member(obj, "candidates");

        if (json_array_get_length(candidates) > 0)
        {
            JsonObject *candidate = json_array_get_object_element(candidates, 0);
            const gchar *finish_reason = json_object_get_string_member_with_default(
                candidate, "finishReason", "");

            if (g_strcmp0(finish_reason, "STOP") == 0)
            {
                ai_response_set_stop_reason(data->response, AI_STOP_REASON_END_TURN);
            }
            else if (g_strcmp0(finish_reason, "MAX_TOKENS") == 0)
            {
                ai_response_set_stop_reason(data->response, AI_STOP_REASON_MAX_TOKENS);
            }

            if (json_object_has_member(candidate, "content"))
            {
                JsonObject *content = json_object_get_object_member(candidate, "content");

                if (json_object_has_member(content, "parts"))
                {
                    JsonArray *parts = json_object_get_array_member(content, "parts");
                    guint len = json_array_get_length(parts);
                    guint i;

                    for (i = 0; i < len; i++)
                    {
                        JsonObject *part = json_array_get_object_element(parts, i);

                        if (json_object_has_member(part, "text"))
                        {
                            const gchar *text = json_object_get_string_member(part, "text");
                            if (text != NULL)
                            {
                                g_string_append(data->current_text, text);
                                g_signal_emit_by_name(data->client, "delta", text);
                            }
                        }
                        else if (json_object_has_member(part, "functionCall"))
                        {
                            /* Gemini streams tool calls atomically per part —
                             * accumulate the AiToolUse directly into the
                             * response now. */
                            JsonObject *fc = json_object_get_object_member(part, "functionCall");
                            const gchar *name = json_object_get_string_member_with_default(fc, "name", "");
                            const gchar *provided_id = json_object_get_string_member_with_default(fc, "id", NULL);
                            JsonNode *args = json_object_has_member(fc, "args")
                                ? json_object_get_member(fc, "args") : NULL;
                            g_autofree gchar *synthetic_id = NULL;
                            const gchar *id;
                            g_autoptr(AiToolUse) tool_use = NULL;

                            if (provided_id != NULL && provided_id[0] != '\0')
                            {
                                id = provided_id;
                            }
                            else
                            {
                                synthetic_id = g_uuid_string_random();
                                id = synthetic_id;
                            }

                            tool_use = ai_tool_use_new(id, name, args);
                            ai_response_add_content_block(data->response,
                                (AiContentBlock *)g_steal_pointer(&tool_use));
                            ai_response_set_stop_reason(data->response,
                                AI_STOP_REASON_TOOL_USE);
                        }
                    }
                }
            }
        }
    }

    /* Parse usage */
    if (json_object_has_member(obj, "usageMetadata"))
    {
        JsonObject *usage_obj = json_object_get_object_member(obj, "usageMetadata");
        gint prompt_tokens = json_object_get_int_member_with_default(usage_obj, "promptTokenCount", 0);
        gint output_tokens = json_object_get_int_member_with_default(usage_obj, "candidatesTokenCount", 0);
        g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, output_tokens);

        ai_response_set_usage(data->response, usage);
    }
}

static void gemini_read_next_line(GeminiStreamData *data);

static void
on_gemini_line_read(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GeminiStreamData *data = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = NULL;
    gsize length;

    (void)source;

    line = g_data_input_stream_read_line_finish(data->data_stream, result, &length, &error);

    if (error != NULL)
    {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_task_return_error(data->task, g_steal_pointer(&error));
            gemini_stream_data_free(data);
        }
        return;
    }

    if (line == NULL)
    {
        /* EOF - finalize response */
        if (data->response != NULL)
        {
            if (data->current_text != NULL && data->current_text->len > 0)
            {
                g_autoptr(AiTextContent) content = ai_text_content_new(data->current_text->str);
                ai_response_add_content_block(data->response, (AiContentBlock *)g_steal_pointer(&content));
            }

            g_signal_emit_by_name(data->client, "stream-end", data->response);
            g_task_return_pointer(data->task, g_object_ref(data->response), g_object_unref);
        }
        else
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                    "Stream ended without valid response");
        }
        gemini_stream_data_free(data);
        return;
    }

    /* Gemini streaming returns SSE format: data: {json} */
    if (g_str_has_prefix(line, "data: "))
    {
        gemini_process_stream_chunk(data, line + 6);
    }

    gemini_read_next_line(data);
}

static void
gemini_read_next_line(GeminiStreamData *data)
{
    g_data_input_stream_read_line_async(
        data->data_stream,
        G_PRIORITY_DEFAULT,
        data->cancellable,
        on_gemini_line_read,
        data);
}

static void
on_gemini_stream_ready(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GeminiStreamData *data = user_data;
    g_autoptr(GError) error = NULL;

    data->input_stream = soup_session_send_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)),
        result,
        &error);

    if (data->input_stream == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_stream_data_free(data);
        return;
    }

    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(data->msg)))
    {
        guint status = soup_message_get_status(data->msg);

        if (status == 401 || status == 403)
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                                    "Authentication failed (HTTP %u)", status);
        }
        else if (status == 429)
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_RATE_LIMITED,
                                    "Rate limited (HTTP %u)", status);
        }
        else
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_NETWORK_ERROR,
                                    "Request failed (HTTP %u)", status);
        }
        gemini_stream_data_free(data);
        return;
    }

    data->data_stream = g_data_input_stream_new(data->input_stream);
    g_data_input_stream_set_newline_type(data->data_stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    gemini_read_next_line(data);
}

static gchar *
ai_gemini_client_get_stream_endpoint_url(AiClient *client)
{
    AiConfig *config = ai_client_get_config(client);
    const gchar *base_url = ai_config_get_base_url(config, AI_PROVIDER_GEMINI);
    const gchar *model = ai_client_get_model(client);
    const gchar *api_key = ai_config_get_api_key(config, AI_PROVIDER_GEMINI);

    if (model == NULL)
    {
        model = AI_GEMINI_DEFAULT_MODEL;
    }

    /* Use streamGenerateContent instead of generateContent */
    return g_strdup_printf("%s/v1beta/models/%s:streamGenerateContent?alt=sse&key=%s",
                           base_url, model, api_key != NULL ? api_key : "");
}

static void
ai_gemini_client_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiGeminiClient *self = AI_GEMINI_CLIENT(streamable);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    gsize request_len;
    GeminiStreamData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    request_json = klass->build_request(AI_CLIENT(self), messages, system_prompt,
                                        max_tokens, tools);

    if (request_json == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build request");
        g_object_unref(task);
        return;
    }

    {
        g_autoptr(JsonGenerator) gen = json_generator_new();
        json_generator_set_root(gen, request_json);
        request_body = json_generator_to_data(gen, &request_len);
    }

    /* Use streaming endpoint */
    url = ai_gemini_client_get_stream_endpoint_url(AI_CLIENT(self));

    msg = soup_message_new("POST", url);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Accept", "text/event-stream");

    soup_message_set_request_body_from_bytes(msg, "application/json",
        g_bytes_new_take(g_steal_pointer(&request_body), request_len));

    data = g_slice_new0(GeminiStreamData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);
    data->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;
    data->stream_started = FALSE;

    soup_session_send_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_gemini_stream_ready,
        data);
}

static AiResponse *
ai_gemini_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    (void)streamable;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_gemini_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_gemini_client_chat_stream_async;
    iface->chat_stream_finish = ai_gemini_client_chat_stream_finish;
}

/*
 * AiImageGenerator interface implementation
 *
 * Gemini serves image generation through two unrelated APIs:
 *
 * 1. Nano Banana (native Gemini image) -- a chat call underneath, posted to
 *    :generateContent with responseModalities asking for an image back.
 *    Because it is a chat call it is the only path that accepts reference
 *    images, as additional inline_data parts, which is what makes
 *    multi-image conditioning possible at all.
 *
 * 2. Imagen (legacy) -- a purely generative :predict endpoint with its own
 *    parameter vocabulary and no reference-image support.
 *
 * Note the wire-format asymmetry: requests spell inline data snake_case
 * (inline_data) while responses come back camelCase (inlineData).
 */

typedef struct
{
    AiGeminiClient *client;
    GTask          *task;
    gchar          *model;
    gboolean        is_nano_banana;
} GeminiImageGenData;

static void
gemini_image_gen_data_free(GeminiImageGenData *data)
{
    g_clear_object(&data->client);
    g_clear_pointer(&data->model, g_free);
    g_slice_free(GeminiImageGenData, data);
}

/*
 * Check if the model is a Nano Banana model (native Gemini image generation).
 * Nano Banana models use the generateContent API instead of predict.
 */
static gboolean
ai_gemini_is_nano_banana_model(const gchar *model)
{
    if (model == NULL)
    {
        return TRUE; /* Default to Nano Banana */
    }

    /* Nano Banana models have "image" in the name (gemini-*-image) */
    if (g_str_has_prefix(model, "gemini-") && g_str_has_suffix(model, "-image"))
    {
        return TRUE;
    }
    if (g_str_has_prefix(model, "gemini-") && strstr(model, "-image-") != NULL)
    {
        return TRUE;
    }
    if (g_str_has_prefix(model, "gemini-") && strstr(model, "-pro-image") != NULL)
    {
        return TRUE;
    }

    /* Imagen models use the predict API */
    if (g_str_has_prefix(model, "imagen-"))
    {
        return FALSE;
    }

    /* Default to Nano Banana for unknown gemini models */
    return TRUE;
}

/*
 * Convert AiImageSize to aspect ratio string.
 *
 * Only consulted when the caller has not set an aspect ratio directly;
 * the Gemini family thinks in ratios, so ai_image_request_set_aspect_ratio()
 * is the natural way to drive it.
 */
static const gchar *
ai_gemini_size_to_aspect_ratio(AiImageSize size)
{
    switch (size)
    {
        case AI_IMAGE_SIZE_256:
        case AI_IMAGE_SIZE_512:
        case AI_IMAGE_SIZE_1024:
            return "1:1";
        case AI_IMAGE_SIZE_1024_1792:
            return "9:16";  /* Portrait */
        case AI_IMAGE_SIZE_1792_1024:
            return "16:9";  /* Landscape */
        case AI_IMAGE_SIZE_AUTO:
        case AI_IMAGE_SIZE_CUSTOM:
        default:
            return "1:1";
    }
}

/*
 * Resolve the aspect ratio to send: the caller's explicit choice when set,
 * otherwise one derived from the pixel size, otherwise square.
 */
static const gchar *
ai_gemini_resolve_aspect_ratio(AiImageRequest *request)
{
    const gchar *aspect = ai_image_request_get_aspect_ratio(request);

    if (aspect != NULL)
    {
        return aspect;
    }

    if (ai_image_request_get_size(request) == AI_IMAGE_SIZE_CUSTOM)
    {
        const gchar *custom = ai_image_request_get_custom_size(request);

        if (custom != NULL)
        {
            return custom;
        }
    }

    return ai_gemini_size_to_aspect_ratio(ai_image_request_get_size(request));
}

/* Every ratio the Nano Banana family accepts. */
static const gchar * const gemini_nano_banana_ratios[] = {
    "1:1", "2:3", "3:2", "3:4", "4:3", "4:5", "5:4",
    "9:16", "16:9", "21:9", NULL
};

/* Imagen is more restrictive. */
static const gchar * const gemini_imagen_ratios[] = {
    "1:1", "3:4", "4:3", "9:16", "16:9", NULL
};

static GList *
ai_gemini_client_list_image_models(AiImageGenerator *generator)
{
    GList *models = NULL;
    AiImageModelInfo *info;
    AiImageCapabilities nano_banana_caps;

    (void)generator;

    /*
     * Nano Banana runs as a chat completion, so it inherits the sampling
     * and system-instruction controls that a purely generative endpoint
     * has nowhere to put -- and, crucially, reference images.
     */
    nano_banana_caps =
        AI_IMAGE_CAP_REFERENCE_IMAGES | AI_IMAGE_CAP_ASPECT_RATIO |
        AI_IMAGE_CAP_MULTI_COUNT | AI_IMAGE_CAP_SEED |
        AI_IMAGE_CAP_SAMPLING | AI_IMAGE_CAP_SAFETY_CONTROL;

    info = ai_image_model_info_new(
        AI_GEMINI_IMAGE_MODEL_NANO_BANANA, "Nano Banana", AI_PROVIDER_GEMINI,
        nano_banana_caps);
    ai_image_model_info_set_aspect_ratios(info, gemini_nano_banana_ratios);
    ai_image_model_info_set_max_count(info, 4);
    ai_image_model_info_set_max_reference_images(info, 3);
    ai_image_model_info_set_notes(info, "Stable native image model, up to 1K");
    models = g_list_append(models, info);

    info = ai_image_model_info_new(
        AI_GEMINI_IMAGE_MODEL_NANO_BANANA_2, "Nano Banana 2",
        AI_PROVIDER_GEMINI,
        nano_banana_caps | AI_IMAGE_CAP_MULTI_REFERENCE);
    ai_image_model_info_set_aspect_ratios(info, gemini_nano_banana_ratios);
    ai_image_model_info_set_max_count(info, 4);
    ai_image_model_info_set_max_reference_images(info, 6);
    models = g_list_append(models, info);

    /*
     * Nano Banana Pro is the multi-reference workhorse: up to fourteen
     * conditioning images and output up to 4K.
     */
    info = ai_image_model_info_new(
        AI_GEMINI_IMAGE_MODEL_NANO_BANANA_PRO, "Nano Banana Pro",
        AI_PROVIDER_GEMINI,
        nano_banana_caps | AI_IMAGE_CAP_MULTI_REFERENCE |
        AI_IMAGE_CAP_RESOLUTION_TIER);
    ai_image_model_info_set_aspect_ratios(info, gemini_nano_banana_ratios);
    ai_image_model_info_set_max_count(info, 4);
    ai_image_model_info_set_max_reference_images(info, 14);
    ai_image_model_info_set_notes(
        info, "Multi-reference conditioning, up to 4K");
    models = g_list_append(models, info);

    info = ai_image_model_info_new(
        AI_GEMINI_IMAGE_MODEL_IMAGEN_4, "Imagen 4", AI_PROVIDER_GEMINI,
        AI_IMAGE_CAP_ASPECT_RATIO | AI_IMAGE_CAP_MULTI_COUNT |
        AI_IMAGE_CAP_NEGATIVE_PROMPT | AI_IMAGE_CAP_SEED |
        AI_IMAGE_CAP_SAFETY_CONTROL | AI_IMAGE_CAP_WATERMARK_CONTROL |
        AI_IMAGE_CAP_OUTPUT_FORMAT | AI_IMAGE_CAP_PROMPT_ENHANCEMENT |
        AI_IMAGE_CAP_LANGUAGE);
    ai_image_model_info_set_aspect_ratios(info, gemini_imagen_ratios);
    ai_image_model_info_set_max_count(info, 4);
    ai_image_model_info_set_notes(info, "No reference images");
    models = g_list_append(models, info);

    info = ai_image_model_info_new(
        AI_GEMINI_IMAGE_MODEL_IMAGEN_3, "Imagen 3", AI_PROVIDER_GEMINI,
        AI_IMAGE_CAP_ASPECT_RATIO | AI_IMAGE_CAP_MULTI_COUNT |
        AI_IMAGE_CAP_NEGATIVE_PROMPT | AI_IMAGE_CAP_SEED |
        AI_IMAGE_CAP_SAFETY_CONTROL | AI_IMAGE_CAP_WATERMARK_CONTROL |
        AI_IMAGE_CAP_OUTPUT_FORMAT);
    ai_image_model_info_set_aspect_ratios(info, gemini_imagen_ratios);
    ai_image_model_info_set_max_count(info, 4);
    ai_image_model_info_set_notes(info, "No reference images");
    models = g_list_append(models, info);

    return models;
}

/*
 * Map the requested moderation level onto Imagen's safetyFilterLevel.
 */
static const gchar *
ai_gemini_safety_filter_level(AiImageModeration moderation)
{
    switch (moderation)
    {
        case AI_IMAGE_MODERATION_LOW:
            return "block_only_high";
        case AI_IMAGE_MODERATION_NONE:
            return "block_none";
        case AI_IMAGE_MODERATION_AUTO:
        default:
            return NULL;
    }
}

/*
 * Build the JSON request for Gemini Imagen API (legacy).
 */
static JsonNode *
ai_gemini_client_build_imagen_request(
    AiGeminiClient *self,
    AiImageRequest *request
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *str;
    gint count;

    (void)self;

    json_builder_begin_object(builder);

    /* Instances array - the prompt, and the negative prompt beside it */
    json_builder_set_member_name(builder, "instances");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "prompt");
    json_builder_add_string_value(builder, ai_image_request_get_prompt(request));
    json_builder_end_object(builder);
    json_builder_end_array(builder);

    /* Parameters */
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);

    count = ai_image_request_get_count(request);
    if (count > 0)
    {
        json_builder_set_member_name(builder, "sampleCount");
        json_builder_add_int_value(builder, count);
    }

    json_builder_set_member_name(builder, "aspectRatio");
    json_builder_add_string_value(builder,
                                  ai_gemini_resolve_aspect_ratio(request));

    str = ai_image_request_get_negative_prompt(request);
    if (str != NULL)
    {
        json_builder_set_member_name(builder, "negativePrompt");
        json_builder_add_string_value(builder, str);
    }

    if (ai_image_request_get_seed(request) >= 0)
    {
        json_builder_set_member_name(builder, "seed");
        json_builder_add_int_value(builder,
                                   ai_image_request_get_seed(request));
    }

    str = ai_image_person_generation_to_string(
        ai_image_request_get_person_generation(request));
    if (str != NULL)
    {
        json_builder_set_member_name(builder, "personGeneration");
        json_builder_add_string_value(builder, str);
    }

    str = ai_gemini_safety_filter_level(
        ai_image_request_get_moderation(request));
    if (str != NULL)
    {
        json_builder_set_member_name(builder, "safetyFilterLevel");
        json_builder_add_string_value(builder, str);
    }

    if (ai_image_request_get_watermark(request) != AI_TRI_UNSET)
    {
        json_builder_set_member_name(builder, "addWatermark");
        json_builder_add_boolean_value(
            builder,
            ai_image_request_get_watermark(request) == AI_TRI_TRUE);
    }

    if (ai_image_request_get_enhance_prompt(request) != AI_TRI_UNSET)
    {
        json_builder_set_member_name(builder, "enhancePrompt");
        json_builder_add_boolean_value(
            builder,
            ai_image_request_get_enhance_prompt(request) == AI_TRI_TRUE);
    }

    str = ai_image_request_get_language(request);
    if (str != NULL)
    {
        json_builder_set_member_name(builder, "language");
        json_builder_add_string_value(builder, str);
    }

    /* Output options.  Base64 comes back either way; the MIME type here
     * chooses the encoding of the returned bytes. */
    json_builder_set_member_name(builder, "outputOptions");
    json_builder_begin_object(builder);

    str = ai_image_format_to_mime_type(
        ai_image_request_get_output_format(request));
    json_builder_set_member_name(builder, "mimeType");
    json_builder_add_string_value(builder, str != NULL ? str : "image/png");

    if (ai_image_request_get_output_compression(request) >= 0)
    {
        json_builder_set_member_name(builder, "compressionQuality");
        json_builder_add_int_value(
            builder, ai_image_request_get_output_compression(request));
    }

    json_builder_end_object(builder);

    ai_image_shared_apply_extras(builder, request);

    json_builder_end_object(builder); /* end parameters */

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * Build the JSON request for Nano Banana (native Gemini image generation).
 *
 * generationConfig is emitted unconditionally.  It carries
 * responseModalities, and without it the model answers a request for a
 * picture with a paragraph of text -- so making it conditional on a
 * non-default size, as this once did, silently broke every request that
 * left the size alone.
 */
static JsonNode *
ai_gemini_client_build_nano_banana_request(
    AiGeminiClient *self,
    AiImageRequest *request
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *str;

    (void)self;

    json_builder_begin_object(builder);

    /* Contents: the prompt, plus one inline_data part per reference */
    json_builder_set_member_name(builder, "contents");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "role");
    json_builder_add_string_value(builder, "user");

    json_builder_set_member_name(builder, "parts");
    ai_image_shared_build_gemini_parts(builder, request);

    json_builder_end_object(builder);
    json_builder_end_array(builder);

    str = ai_image_request_get_system_instruction(request);
    if (str != NULL)
    {
        json_builder_set_member_name(builder, "systemInstruction");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "parts");
        json_builder_begin_array(builder);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "text");
        json_builder_add_string_value(builder, str);
        json_builder_end_object(builder);
        json_builder_end_array(builder);
        json_builder_end_object(builder);
    }

    json_builder_set_member_name(builder, "generationConfig");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "responseModalities");
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "TEXT");
    json_builder_add_string_value(builder, "IMAGE");
    json_builder_end_array(builder);

    if (ai_image_request_get_count(request) > 1)
    {
        json_builder_set_member_name(builder, "candidateCount");
        json_builder_add_int_value(builder,
                                   ai_image_request_get_count(request));
    }

    if (ai_image_request_get_seed(request) >= 0)
    {
        json_builder_set_member_name(builder, "seed");
        json_builder_add_int_value(builder,
                                   ai_image_request_get_seed(request));
    }

    if (ai_image_request_get_temperature(request) >= 0.0)
    {
        json_builder_set_member_name(builder, "temperature");
        json_builder_add_double_value(
            builder, ai_image_request_get_temperature(request));
    }

    if (ai_image_request_get_top_p(request) >= 0.0)
    {
        json_builder_set_member_name(builder, "topP");
        json_builder_add_double_value(builder,
                                      ai_image_request_get_top_p(request));
    }

    if (ai_image_request_get_top_k(request) >= 0)
    {
        json_builder_set_member_name(builder, "topK");
        json_builder_add_int_value(builder,
                                   ai_image_request_get_top_k(request));
    }

    /* Image configuration */
    json_builder_set_member_name(builder, "imageConfig");
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "aspectRatio");
    json_builder_add_string_value(builder,
                                  ai_gemini_resolve_aspect_ratio(request));

    str = ai_image_resolution_to_string(
        ai_image_request_get_resolution(request));
    if (str != NULL)
    {
        json_builder_set_member_name(builder, "imageSize");
        json_builder_add_string_value(builder, str);
    }

    json_builder_end_object(builder); /* end imageConfig */

    json_builder_end_object(builder); /* end generationConfig */

    ai_image_shared_apply_extras(builder, request);

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * Parse a Gemini image response.
 *
 * Three mutually incompatible envelopes are in circulation and any of them
 * can turn up depending on the model and API version:
 *
 *   predictions[].bytesBase64Encoded       (Imagen :predict)
 *   generatedImages[].bytesBase64Encoded   (older image endpoints)
 *   candidates[].content.parts[].inlineData (Nano Banana :generateContent)
 *
 * Rather than trusting the caller's model classification to pick one, try
 * each in turn -- the cost is three absent-member checks and it makes the
 * parser robust to a model being served by a different envelope than
 * expected.
 */
static AiImageResponse *
ai_gemini_client_parse_image_response(
    JsonNode     *json,
    const gchar  *model,
    GError      **error
){
    JsonObject *obj;
    g_autoptr(AiImageResponse) response = NULL;
    gint64 now;

    if (!JSON_NODE_HOLDS_OBJECT(json))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Expected JSON object in response");
        return NULL;
    }

    obj = json_node_get_object(json);

    if (json_object_has_member(obj, "error"))
    {
        JsonObject *err_obj = json_object_get_object_member(obj, "error");
        const gchar *err_msg = json_object_get_string_member_with_default(
            err_obj, "message", "Unknown error");

        g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", err_msg);
        return NULL;
    }

    now = g_get_real_time() / G_USEC_PER_SEC;
    response = ai_image_response_new(NULL, now);
    if (model != NULL)
    {
        ai_image_response_set_model(response, model);
    }

    /* Envelope 1 and 2: a flat array of base64 payloads. */
    {
        const gchar *keys[] = { "predictions", "generatedImages", NULL };
        guint k;

        for (k = 0; keys[k] != NULL; k++)
        {
            JsonArray *array;
            guint len;
            guint i;

            if (!json_object_has_member(obj, keys[k]))
            {
                continue;
            }

            array = json_object_get_array_member(obj, keys[k]);
            len = json_array_get_length(array);

            for (i = 0; i < len; i++)
            {
                JsonObject *item = json_array_get_object_element(array, i);
                const gchar *b64;
                const gchar *mime;

                if (item == NULL)
                {
                    continue;
                }

                b64 = json_object_get_string_member_with_default(
                    item, "bytesBase64Encoded", NULL);
                if (b64 == NULL)
                {
                    continue;
                }

                mime = json_object_get_string_member_with_default(
                    item, "mimeType", "image/png");

                ai_image_response_add_image(
                    response, ai_generated_image_new_from_base64(b64, mime));
            }
        }
    }

    /* Envelope 3: chat candidates carrying inline image parts. */
    if (json_object_has_member(obj, "candidates"))
    {
        JsonArray *candidates = json_object_get_array_member(obj, "candidates");
        guint num_candidates = json_array_get_length(candidates);
        guint c;

        for (c = 0; c < num_candidates; c++)
        {
            JsonObject *candidate = json_array_get_object_element(candidates, c);
            JsonObject *content;
            JsonArray *parts;
            guint num_parts;
            guint p;

            if (candidate == NULL ||
                !json_object_has_member(candidate, "content"))
            {
                continue;
            }

            content = json_object_get_object_member(candidate, "content");
            if (!json_object_has_member(content, "parts"))
            {
                continue;
            }

            parts = json_object_get_array_member(content, "parts");
            num_parts = json_array_get_length(parts);

            for (p = 0; p < num_parts; p++)
            {
                JsonObject *part = json_array_get_object_element(parts, p);
                JsonObject *inline_data;
                const gchar *b64;
                const gchar *mime;

                /* Requests use inline_data, responses inlineData; accept
                 * both so neither spelling can surprise us. */
                if (part == NULL)
                {
                    continue;
                }
                else if (json_object_has_member(part, "inlineData"))
                {
                    inline_data = json_object_get_object_member(part,
                                                                "inlineData");
                }
                else if (json_object_has_member(part, "inline_data"))
                {
                    inline_data = json_object_get_object_member(part,
                                                                "inline_data");
                }
                else
                {
                    continue;
                }

                b64 = json_object_get_string_member_with_default(
                    inline_data, "data", NULL);
                if (b64 == NULL)
                {
                    continue;
                }

                mime = json_object_get_string_member_with_default(
                    inline_data, "mimeType", NULL);
                if (mime == NULL)
                {
                    mime = json_object_get_string_member_with_default(
                        inline_data, "mime_type", "image/png");
                }

                ai_image_response_add_image(
                    response, ai_generated_image_new_from_base64(b64, mime));
            }
        }
    }

    if (ai_image_response_get_image_count(response) == 0)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Gemini returned no image data; the model may have "
                    "answered with text, or the prompt may have been refused");
        return NULL;
    }

    return (AiImageResponse *)g_steal_pointer(&response);
}

static void
on_gemini_image_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GeminiImageGenData *data = user_data;
    g_autoptr(GBytes) response_bytes = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = NULL;
    const gchar *response_data;
    gsize response_len;
    AiImageResponse *response;

    (void)source;

    response_bytes = ai_image_shared_send_finish(result, &error);

    if (response_bytes == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_image_gen_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_image_gen_data_free(data);
        return;
    }

    response = ai_gemini_client_parse_image_response(
        json_parser_get_root(parser), data->model, &error);

    if (response == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
    }
    else
    {
        guint count = ai_image_response_get_image_count(response);

        g_signal_emit_by_name(data->client, "image-progress", count, count);

        g_task_return_pointer(data->task, response,
                              (GDestroyNotify)ai_image_response_free);
    }

    gemini_image_gen_data_free(data);
}

static void
ai_gemini_client_generate_image_async(
    AiImageGenerator    *generator,
    AiImageRequest      *request,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiGeminiClient *self = AI_GEMINI_CLIENT(generator);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    g_autoptr(GBytes) body_bytes = NULL;
    g_autoptr(GError) error = NULL;
    const AiImageModelInfo *info;
    gsize request_len = 0;
    AiConfig *config;
    const gchar *base_url;
    const gchar *api_key;
    const gchar *model;
    gboolean is_nano_banana;
    GeminiImageGenData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    model = ai_image_request_get_model(request);
    if (model == NULL)
    {
        model = AI_GEMINI_IMAGE_DEFAULT_MODEL;
    }
    is_nano_banana = ai_gemini_is_nano_banana_model(model);

    info = ai_image_generator_get_model_info(generator, model);

    if (!ai_image_request_validate(request, info, AI_IMAGE_VALIDATE_NONE,
                                   &error))
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    if (is_nano_banana)
    {
        request_json = ai_gemini_client_build_nano_banana_request(self, request);
    }
    else
    {
        request_json = ai_gemini_client_build_imagen_request(self, request);
    }

    if (request_json == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build image request");
        g_object_unref(task);
        return;
    }

    {
        g_autoptr(JsonGenerator) gen = json_generator_new();
        json_generator_set_root(gen, request_json);
        request_body = json_generator_to_data(gen, &request_len);
    }

    config = ai_client_get_config(AI_CLIENT(self));
    base_url = ai_config_get_base_url(config, AI_PROVIDER_GEMINI);
    api_key = ai_config_get_api_key(config, AI_PROVIDER_GEMINI);

    url = g_strdup_printf("%s/v1beta/models/%s:%s", base_url, model,
                          is_nano_banana ? "generateContent" : "predict");

    msg = soup_message_new("POST", url);
    if (msg == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Invalid Gemini base URL: %s", base_url);
        g_object_unref(task);
        return;
    }

    /*
     * The key goes in a header, not the query string.  A URL-embedded
     * credential leaks into proxy logs, browser history and crash reports;
     * x-goog-api-key is what the API expects and what the reference
     * scripts use.
     */
    if (api_key != NULL)
    {
        soup_message_headers_append(soup_message_get_request_headers(msg),
                                    "x-goog-api-key", api_key);
    }

    body_bytes = g_bytes_new_take(g_steal_pointer(&request_body), request_len);
    soup_message_set_request_body_from_bytes(msg, "application/json", body_bytes);

    data = g_slice_new0(GeminiImageGenData);
    data->client = g_object_ref(self);
    data->task = task;
    data->model = g_strdup(model);
    data->is_nano_banana = is_nano_banana;

    ai_image_shared_send_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        body_bytes,
        ai_config_get_max_retries(config),
        cancellable,
        on_gemini_image_response,
        data);
}

static AiImageResponse *
ai_gemini_client_generate_image_finish(
    AiImageGenerator  *generator,
    GAsyncResult      *result,
    GError           **error
){
    (void)generator;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static const gchar *
ai_gemini_client_get_image_default_model(AiImageGenerator *generator)
{
    (void)generator;
    return AI_GEMINI_IMAGE_DEFAULT_MODEL;
}

static void
ai_gemini_client_image_generator_init(AiImageGeneratorInterface *iface)
{
    iface->generate_image_async = ai_gemini_client_generate_image_async;
    iface->generate_image_finish = ai_gemini_client_generate_image_finish;
    iface->get_default_model = ai_gemini_client_get_image_default_model;
    iface->list_image_models = ai_gemini_client_list_image_models;
}

/*
 * Public API
 */

AiGeminiClient *
ai_gemini_client_new(void)
{
    g_autoptr(AiGeminiClient) self = g_object_new(AI_TYPE_GEMINI_CLIENT, NULL);

    return (AiGeminiClient *)g_steal_pointer(&self);
}

AiGeminiClient *
ai_gemini_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiGeminiClient) self = g_object_new(AI_TYPE_GEMINI_CLIENT,
                                                   "config", config,
                                                   NULL);

    return (AiGeminiClient *)g_steal_pointer(&self);
}

AiGeminiClient *
ai_gemini_client_new_with_key(const gchar *api_key)
{
    g_autoptr(AiConfig) config = ai_config_new();

    ai_config_set_api_key(config, AI_PROVIDER_GEMINI, api_key);

    return ai_gemini_client_new_with_config(config);
}
