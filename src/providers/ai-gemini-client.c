/*
 * ai-gemini-client.c - Google Gemini client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-gemini-client.h"
#include "core/ai-error.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

/*
 * Private structure for AiGeminiClient.
 */
struct _AiGeminiClient
{
    AiClient parent_instance;
};

static void ai_gemini_client_provider_init(AiProviderInterface *iface);
static void ai_gemini_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiGeminiClient, ai_gemini_client, AI_TYPE_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_gemini_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_gemini_client_streamable_init))

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
    ai_gemini_client_chat_async(AI_PROVIDER(streamable), messages, system_prompt,
                                max_tokens, tools, cancellable, callback, user_data);
}

static AiResponse *
ai_gemini_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    return ai_gemini_client_chat_finish(AI_PROVIDER(streamable), result, error);
}

static void
ai_gemini_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_gemini_client_chat_stream_async;
    iface->chat_stream_finish = ai_gemini_client_chat_stream_finish;
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
