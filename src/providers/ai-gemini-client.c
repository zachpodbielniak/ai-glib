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
#include "core/ai-error.h"
#include "core/ai-image-generator.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
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
 * Build Gemini API request.
 * Gemini uses a different format: { contents: [...], generationConfig: {...} }
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

    (void)tools; /* TODO: Implement tool support for Gemini */

    json_builder_begin_object(builder);

    /* Contents (messages) */
    json_builder_set_member_name(builder, "contents");
    json_builder_begin_array(builder);

    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        AiRole role = ai_message_get_role(msg);
        g_autofree gchar *text = ai_message_get_text(msg);

        json_builder_begin_object(builder);

        /* Gemini uses "user" and "model" for roles */
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
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "text");
        json_builder_add_string_value(builder, text != NULL ? text : "");
        json_builder_end_object(builder);
        json_builder_end_array(builder);

        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);

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

static void
ai_gemini_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    task = g_task_new(provider, NULL, callback, user_data);

    models = g_list_append(models, g_strdup("gemini-2.0-flash"));
    models = g_list_append(models, g_strdup("gemini-1.5-pro"));
    models = g_list_append(models, g_strdup("gemini-1.5-flash"));

    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
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
 * Gemini supports two image generation APIs:
 *
 * 1. Nano Banana (Native Gemini Image) - uses generateContent endpoint
 *    Models: gemini-2.5-flash-image, gemini-3-pro-image-preview
 *    Endpoint: POST /v1beta/models/{model}:generateContent
 *    Format: { contents: [...], generationConfig: { responseModalities: ["IMAGE"], imageConfig: {...} } }
 *
 * 2. Imagen (Legacy) - uses predict endpoint
 *    Models: imagen-4.0-generate-001, imagen-3.0-generate-001
 *    Endpoint: POST /v1beta/models/{model}:predict
 *    Format: { instances: [...], parameters: {...} }
 */

typedef struct
{
    AiGeminiClient *client;
    GTask          *task;
    SoupMessage    *msg;
    gboolean        is_nano_banana;
} GeminiImageGenData;

static void
gemini_image_gen_data_free(GeminiImageGenData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->msg);
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
 * Nano Banana supports more aspect ratios than Imagen.
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
 * Build the JSON request for Gemini Imagen API (legacy).
 */
static JsonNode *
ai_gemini_client_build_imagen_request(
    AiGeminiClient *self,
    AiImageRequest *request
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *aspect_ratio;
    gint count;

    (void)self;

    json_builder_begin_object(builder);

    /* Instances array - contains the prompt */
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

    /* Sample count (number of images) */
    count = ai_image_request_get_count(request);
    if (count > 0)
    {
        json_builder_set_member_name(builder, "sampleCount");
        json_builder_add_int_value(builder, count);
    }

    /* Aspect ratio */
    {
        AiImageSize size = ai_image_request_get_size(request);
        if (size == AI_IMAGE_SIZE_CUSTOM)
        {
            const gchar *custom = ai_image_request_get_custom_size(request);
            /* For custom size, try to parse as aspect ratio */
            aspect_ratio = custom != NULL ? custom : "1:1";
        }
        else
        {
            aspect_ratio = ai_gemini_size_to_aspect_ratio(size);
        }
        json_builder_set_member_name(builder, "aspectRatio");
        json_builder_add_string_value(builder, aspect_ratio);
    }

    /* Output options - always request base64 for easier handling */
    json_builder_set_member_name(builder, "outputOptions");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "mimeType");
    json_builder_add_string_value(builder, "image/png");
    json_builder_end_object(builder);

    json_builder_end_object(builder); /* end parameters */

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * Build the JSON request for Nano Banana (native Gemini image generation).
 * Uses the generateContent API.
 *
 * Basic format:
 * {
 *   "contents": [{ "parts": [{ "text": "prompt" }] }]
 * }
 *
 * With config (optional):
 * {
 *   "contents": [{ "parts": [{ "text": "prompt" }] }],
 *   "generationConfig": {
 *     "responseModalities": ["TEXT", "IMAGE"],
 *     "imageConfig": { "aspectRatio": "16:9" }
 *   }
 * }
 */
static JsonNode *
ai_gemini_client_build_nano_banana_request(
    AiGeminiClient *self,
    AiImageRequest *request
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    AiImageSize size;

    (void)self;

    json_builder_begin_object(builder);

    /* Contents array - contains the prompt */
    json_builder_set_member_name(builder, "contents");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "parts");
    json_builder_begin_array(builder);
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "text");
    json_builder_add_string_value(builder, ai_image_request_get_prompt(request));
    json_builder_end_object(builder);
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    json_builder_end_array(builder);

    /* Only add generationConfig if we have non-default settings */
    size = ai_image_request_get_size(request);
    if (size != AI_IMAGE_SIZE_AUTO && size != AI_IMAGE_SIZE_1024)
    {
        const gchar *aspect_ratio;

        json_builder_set_member_name(builder, "generationConfig");
        json_builder_begin_object(builder);

        /* Response modalities */
        json_builder_set_member_name(builder, "responseModalities");
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "TEXT");
        json_builder_add_string_value(builder, "IMAGE");
        json_builder_end_array(builder);

        /* Image configuration */
        json_builder_set_member_name(builder, "imageConfig");
        json_builder_begin_object(builder);

        /* Aspect ratio */
        if (size == AI_IMAGE_SIZE_CUSTOM)
        {
            const gchar *custom = ai_image_request_get_custom_size(request);
            aspect_ratio = custom != NULL ? custom : "1:1";
        }
        else
        {
            aspect_ratio = ai_gemini_size_to_aspect_ratio(size);
        }
        json_builder_set_member_name(builder, "aspectRatio");
        json_builder_add_string_value(builder, aspect_ratio);

        json_builder_end_object(builder); /* end imageConfig */

        json_builder_end_object(builder); /* end generationConfig */
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * Parse Gemini Imagen response (legacy predict API).
 */
static AiImageResponse *
ai_gemini_client_parse_imagen_response(
    JsonNode  *json,
    GError   **error
){
    JsonObject *obj;
    g_autoptr(AiImageResponse) response = NULL;
    JsonArray *predictions;
    guint i;
    guint len;
    gint64 now;

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

    now = g_get_real_time() / G_USEC_PER_SEC;
    response = ai_image_response_new(NULL, now);

    /* Parse predictions array */
    if (json_object_has_member(obj, "predictions"))
    {
        predictions = json_object_get_array_member(obj, "predictions");
        len = json_array_get_length(predictions);

        for (i = 0; i < len; i++)
        {
            JsonObject *pred = json_array_get_object_element(predictions, i);
            AiGeneratedImage *image = NULL;

            if (json_object_has_member(pred, "bytesBase64Encoded"))
            {
                const gchar *b64 = json_object_get_string_member(pred, "bytesBase64Encoded");
                const gchar *mime = json_object_get_string_member_with_default(
                    pred, "mimeType", "image/png");
                image = ai_generated_image_new_from_base64(b64, mime);
            }

            if (image != NULL)
            {
                ai_image_response_add_image(response, image);
            }
        }
    }

    return (AiImageResponse *)g_steal_pointer(&response);
}

/*
 * Parse Nano Banana response (generateContent API).
 * Response format: { candidates: [{ content: { parts: [{ inlineData: { data, mimeType } }] } }] }
 */
static AiImageResponse *
ai_gemini_client_parse_nano_banana_response(
    JsonNode  *json,
    GError   **error
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

    /* Check for error */
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

    /* Parse candidates array */
    if (json_object_has_member(obj, "candidates"))
    {
        JsonArray *candidates = json_object_get_array_member(obj, "candidates");
        guint num_candidates = json_array_get_length(candidates);
        guint c;

        for (c = 0; c < num_candidates; c++)
        {
            JsonObject *candidate = json_array_get_object_element(candidates, c);

            if (json_object_has_member(candidate, "content"))
            {
                JsonObject *content = json_object_get_object_member(candidate, "content");

                if (json_object_has_member(content, "parts"))
                {
                    JsonArray *parts = json_object_get_array_member(content, "parts");
                    guint num_parts = json_array_get_length(parts);
                    guint p;

                    for (p = 0; p < num_parts; p++)
                    {
                        JsonObject *part = json_array_get_object_element(parts, p);

                        /* Check for inline_data (image) */
                        if (json_object_has_member(part, "inlineData"))
                        {
                            JsonObject *inline_data = json_object_get_object_member(part, "inlineData");
                            const gchar *b64 = json_object_get_string_member_with_default(
                                inline_data, "data", NULL);
                            const gchar *mime = json_object_get_string_member_with_default(
                                inline_data, "mimeType", "image/png");

                            if (b64 != NULL)
                            {
                                AiGeneratedImage *image = ai_generated_image_new_from_base64(b64, mime);
                                ai_image_response_add_image(response, image);
                            }
                        }
                    }
                }
            }
        }
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
    SoupMessage *msg = data->msg;
    const gchar *response_data;
    gsize response_len;
    JsonNode *response_json;
    AiImageResponse *response;

    (void)source;

    response_bytes = soup_session_send_and_read_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)), result, &error);

    if (response_bytes == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        gemini_image_gen_data_free(data);
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
            response_data = g_bytes_get_data(response_bytes, &response_len);
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_SERVER_ERROR,
                                    "Request failed (HTTP %u): %.*s", status,
                                    (int)MIN(response_len, 200), response_data);
        }

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

    response_json = json_parser_get_root(parser);

    /* Use appropriate parser based on model type */
    if (data->is_nano_banana)
    {
        response = ai_gemini_client_parse_nano_banana_response(response_json, &error);
    }
    else
    {
        response = ai_gemini_client_parse_imagen_response(response_json, &error);
    }

    if (response == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
    }
    else
    {
        g_task_return_pointer(data->task, response, (GDestroyNotify)ai_image_response_free);
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
    gsize request_len;
    AiConfig *config;
    const gchar *base_url;
    const gchar *api_key;
    const gchar *model;
    gboolean is_nano_banana;
    GeminiImageGenData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    /* Get the image model and determine API type */
    model = ai_image_request_get_model(request);
    if (model == NULL)
    {
        model = AI_GEMINI_IMAGE_DEFAULT_MODEL;
    }
    is_nano_banana = ai_gemini_is_nano_banana_model(model);

    /* Build the appropriate request based on model type */
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

    /* Build the appropriate endpoint URL based on model type */
    if (is_nano_banana)
    {
        /* Nano Banana uses generateContent endpoint */
        url = g_strdup_printf("%s/v1beta/models/%s:generateContent?key=%s",
                              base_url, model, api_key != NULL ? api_key : "");
    }
    else
    {
        /* Imagen uses predict endpoint */
        url = g_strdup_printf("%s/v1beta/models/%s:predict?key=%s",
                              base_url, model, api_key != NULL ? api_key : "");
    }

    msg = soup_message_new("POST", url);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");

    soup_message_set_request_body_from_bytes(msg, "application/json",
        g_bytes_new_take(g_steal_pointer(&request_body), request_len));

    data = g_slice_new0(GeminiImageGenData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);
    data->is_nano_banana = is_nano_banana;

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
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

static GList *
ai_gemini_client_get_supported_sizes(AiImageGenerator *generator)
{
    GList *sizes = NULL;

    (void)generator;

    /*
     * Gemini uses aspect ratios instead of pixel dimensions.
     * Nano Banana supports all of these aspect ratios.
     */
    sizes = g_list_append(sizes, g_strdup("1:1"));
    sizes = g_list_append(sizes, g_strdup("2:3"));
    sizes = g_list_append(sizes, g_strdup("3:2"));
    sizes = g_list_append(sizes, g_strdup("3:4"));
    sizes = g_list_append(sizes, g_strdup("4:3"));
    sizes = g_list_append(sizes, g_strdup("4:5"));
    sizes = g_list_append(sizes, g_strdup("5:4"));
    sizes = g_list_append(sizes, g_strdup("9:16"));
    sizes = g_list_append(sizes, g_strdup("16:9"));
    sizes = g_list_append(sizes, g_strdup("21:9"));

    return sizes;
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
    iface->get_supported_sizes = ai_gemini_client_get_supported_sizes;
    iface->get_default_model = ai_gemini_client_get_image_default_model;
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
