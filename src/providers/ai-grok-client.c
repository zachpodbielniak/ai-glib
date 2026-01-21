/*
 * ai-grok-client.c - xAI Grok client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Grok uses an OpenAI-compatible API, so this implementation shares
 * much of the request/response handling with the OpenAI client.
 */

#include "config.h"

#include "providers/ai-grok-client.h"
#include "core/ai-error.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

#define GROK_COMPLETIONS_ENDPOINT "/v1/chat/completions"

/*
 * Private structure for AiGrokClient.
 */
struct _AiGrokClient
{
    AiClient parent_instance;
};

/*
 * Interface implementations forward declarations.
 */
static void ai_grok_client_provider_init(AiProviderInterface *iface);
static void ai_grok_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiGrokClient, ai_grok_client, AI_TYPE_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_grok_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_grok_client_streamable_init))

/*
 * Build request uses OpenAI format.
 */
static JsonNode *
ai_grok_client_build_request(
    AiClient    *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    GList       *tools
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *model;
    GList *l;

    model = ai_client_get_model(client);
    if (model == NULL)
    {
        model = AI_GROK_DEFAULT_MODEL;
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    if (max_tokens > 0)
    {
        json_builder_set_member_name(builder, "max_tokens");
        json_builder_add_int_value(builder, max_tokens);
    }

    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);

    if (system_prompt != NULL && system_prompt[0] != '\0')
    {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "role");
        json_builder_add_string_value(builder, "system");
        json_builder_set_member_name(builder, "content");
        json_builder_add_string_value(builder, system_prompt);
        json_builder_end_object(builder);
    }

    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        g_autoptr(JsonNode) msg_node = ai_message_to_json(msg);

        json_builder_add_value(builder, g_steal_pointer(&msg_node));
    }

    json_builder_end_array(builder);

    if (tools != NULL)
    {
        json_builder_set_member_name(builder, "tools");
        json_builder_begin_array(builder);

        for (l = tools; l != NULL; l = l->next)
        {
            AiTool *tool = l->data;
            g_autoptr(JsonNode) tool_node = ai_tool_to_json(tool, AI_PROVIDER_GROK);

            json_builder_add_value(builder, g_steal_pointer(&tool_node));
        }

        json_builder_end_array(builder);
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

    return json_builder_get_root(builder);
}

/*
 * Parse response uses OpenAI format.
 */
static AiResponse *
ai_grok_client_parse_response(
    AiClient  *client,
    JsonNode  *json,
    GError   **error
){
    JsonObject *obj;
    const gchar *id;
    const gchar *model;
    g_autoptr(AiResponse) response = NULL;

    (void)client;

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

    id = json_object_get_string_member_with_default(obj, "id", "");
    model = json_object_get_string_member_with_default(obj, "model", "");

    response = ai_response_new(id, model);

    if (json_object_has_member(obj, "usage"))
    {
        JsonObject *usage_obj = json_object_get_object_member(obj, "usage");
        gint prompt_tokens = json_object_get_int_member_with_default(usage_obj, "prompt_tokens", 0);
        gint completion_tokens = json_object_get_int_member_with_default(usage_obj, "completion_tokens", 0);
        g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, completion_tokens);

        ai_response_set_usage(response, usage);
    }

    if (json_object_has_member(obj, "choices"))
    {
        JsonArray *choices = json_object_get_array_member(obj, "choices");

        if (json_array_get_length(choices) > 0)
        {
            JsonObject *choice = json_array_get_object_element(choices, 0);
            const gchar *finish_reason = json_object_get_string_member_with_default(
                choice, "finish_reason", "");
            JsonObject *message;

            ai_response_set_stop_reason(response, ai_stop_reason_from_string(finish_reason));

            if (json_object_has_member(choice, "message"))
            {
                message = json_object_get_object_member(choice, "message");

                if (json_object_has_member(message, "content"))
                {
                    JsonNode *content_node = json_object_get_member(message, "content");

                    if (JSON_NODE_HOLDS_VALUE(content_node))
                    {
                        const gchar *text = json_node_get_string(content_node);
                        if (text != NULL)
                        {
                            g_autoptr(AiTextContent) content = ai_text_content_new(text);
                            ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&content));
                        }
                    }
                }

                if (json_object_has_member(message, "tool_calls"))
                {
                    JsonArray *tool_calls = json_object_get_array_member(message, "tool_calls");
                    guint len = json_array_get_length(tool_calls);
                    guint i;

                    for (i = 0; i < len; i++)
                    {
                        JsonObject *tc = json_array_get_object_element(tool_calls, i);
                        const gchar *tc_id = json_object_get_string_member_with_default(tc, "id", "");
                        JsonObject *func;
                        const gchar *name;
                        const gchar *args_str;
                        g_autoptr(AiToolUse) tool_use = NULL;

                        if (json_object_has_member(tc, "function"))
                        {
                            func = json_object_get_object_member(tc, "function");
                            name = json_object_get_string_member_with_default(func, "name", "");
                            args_str = json_object_get_string_member_with_default(func, "arguments", "{}");

                            tool_use = ai_tool_use_new_from_json_string(tc_id, name, args_str);
                            ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&tool_use));
                        }
                    }
                }
            }
        }
    }

    return (AiResponse *)g_steal_pointer(&response);
}

static gchar *
ai_grok_client_get_endpoint_url(AiClient *client)
{
    AiConfig *config = ai_client_get_config(client);
    const gchar *base_url = ai_config_get_base_url(config, AI_PROVIDER_GROK);

    return g_strconcat(base_url, GROK_COMPLETIONS_ENDPOINT, NULL);
}

static void
ai_grok_client_add_auth_headers(
    AiClient    *client,
    SoupMessage *msg
){
    AiConfig *config = ai_client_get_config(client);
    const gchar *api_key = ai_config_get_api_key(config, AI_PROVIDER_GROK);
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    g_autofree gchar *auth_header = NULL;

    if (api_key != NULL)
    {
        auth_header = g_strdup_printf("Bearer %s", api_key);
        soup_message_headers_append(headers, "Authorization", auth_header);
    }
}

static void
ai_grok_client_class_init(AiGrokClientClass *klass)
{
    AiClientClass *client_class = AI_CLIENT_CLASS(klass);

    client_class->build_request = ai_grok_client_build_request;
    client_class->parse_response = ai_grok_client_parse_response;
    client_class->get_endpoint_url = ai_grok_client_get_endpoint_url;
    client_class->add_auth_headers = ai_grok_client_add_auth_headers;
}

static void
ai_grok_client_init(AiGrokClient *self)
{
    (void)self;
    ai_client_set_model(AI_CLIENT(self), AI_GROK_DEFAULT_MODEL);
}

/*
 * AiProvider interface
 */

static AiProviderType
ai_grok_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_GROK;
}

static const gchar *
ai_grok_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "Grok";
}

static const gchar *
ai_grok_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_GROK_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiGrokClient *client;
    GTask        *task;
    SoupMessage  *msg;
} GrokChatAsyncData;

static void
grok_chat_async_data_free(GrokChatAsyncData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->msg);
    g_slice_free(GrokChatAsyncData, data);
}

static void
on_grok_chat_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GrokChatAsyncData *data = user_data;
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
        grok_chat_async_data_free(data);
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

        grok_chat_async_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        grok_chat_async_data_free(data);
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

    grok_chat_async_data_free(data);
}

static void
ai_grok_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiGrokClient *self = AI_GROK_CLIENT(provider);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    GrokChatAsyncData *data;
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

    data = g_slice_new0(GrokChatAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_grok_chat_response,
        data);
}

static AiResponse *
ai_grok_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_grok_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    task = g_task_new(provider, NULL, callback, user_data);

    models = g_list_append(models, g_strdup("grok-2"));
    models = g_list_append(models, g_strdup("grok-2-mini"));

    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
}

static GList *
ai_grok_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_grok_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_grok_client_get_provider_type;
    iface->get_name = ai_grok_client_get_name;
    iface->get_default_model = ai_grok_client_get_default_model;
    iface->chat_async = ai_grok_client_chat_async;
    iface->chat_finish = ai_grok_client_chat_finish;
    iface->list_models_async = ai_grok_client_list_models_async;
    iface->list_models_finish = ai_grok_client_list_models_finish;
}

static void
ai_grok_client_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    ai_grok_client_chat_async(AI_PROVIDER(streamable), messages, system_prompt,
                              max_tokens, tools, cancellable, callback, user_data);
}

static AiResponse *
ai_grok_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    return ai_grok_client_chat_finish(AI_PROVIDER(streamable), result, error);
}

static void
ai_grok_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_grok_client_chat_stream_async;
    iface->chat_stream_finish = ai_grok_client_chat_stream_finish;
}

/*
 * Public API
 */

AiGrokClient *
ai_grok_client_new(void)
{
    g_autoptr(AiGrokClient) self = g_object_new(AI_TYPE_GROK_CLIENT, NULL);

    return (AiGrokClient *)g_steal_pointer(&self);
}

AiGrokClient *
ai_grok_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiGrokClient) self = g_object_new(AI_TYPE_GROK_CLIENT,
                                                 "config", config,
                                                 NULL);

    return (AiGrokClient *)g_steal_pointer(&self);
}

AiGrokClient *
ai_grok_client_new_with_key(const gchar *api_key)
{
    g_autoptr(AiConfig) config = ai_config_new();

    ai_config_set_api_key(config, AI_PROVIDER_GROK, api_key);

    return ai_grok_client_new_with_config(config);
}
