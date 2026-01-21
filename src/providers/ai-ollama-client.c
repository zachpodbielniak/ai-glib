/*
 * ai-ollama-client.c - Ollama client (local)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-ollama-client.h"
#include "core/ai-error.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

#define OLLAMA_CHAT_ENDPOINT "/api/chat"

/*
 * Private structure for AiOllamaClient.
 */
struct _AiOllamaClient
{
    AiClient parent_instance;
};

static void ai_ollama_client_provider_init(AiProviderInterface *iface);
static void ai_ollama_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiOllamaClient, ai_ollama_client, AI_TYPE_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_ollama_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_ollama_client_streamable_init))

/*
 * Build Ollama API request.
 */
static JsonNode *
ai_ollama_client_build_request(
    AiClient    *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    GList       *tools
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *model;
    GList *l;

    (void)tools; /* TODO: Implement tool support */

    model = ai_client_get_model(client);
    if (model == NULL)
    {
        model = AI_OLLAMA_DEFAULT_MODEL;
    }

    json_builder_begin_object(builder);

    /* Model */
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    /* Messages */
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);

    /* Add system message if provided */
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
        AiRole role = ai_message_get_role(msg);
        g_autofree gchar *text = ai_message_get_text(msg);

        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "role");
        json_builder_add_string_value(builder, ai_role_to_string(role));

        json_builder_set_member_name(builder, "content");
        json_builder_add_string_value(builder, text != NULL ? text : "");

        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);

    /* Disable streaming for sync requests */
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, FALSE);

    /* Options */
    json_builder_set_member_name(builder, "options");
    json_builder_begin_object(builder);

    if (max_tokens > 0)
    {
        json_builder_set_member_name(builder, "num_predict");
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
 * Parse Ollama response.
 */
static AiResponse *
ai_ollama_client_parse_response(
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
        const gchar *err_msg = json_object_get_string_member(obj, "error");

        g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", err_msg);
        return NULL;
    }

    response = ai_response_new("", ai_client_get_model(client));

    /* Parse done status */
    if (json_object_get_boolean_member_with_default(obj, "done", FALSE))
    {
        const gchar *done_reason = json_object_get_string_member_with_default(
            obj, "done_reason", "");

        if (g_strcmp0(done_reason, "length") == 0)
        {
            ai_response_set_stop_reason(response, AI_STOP_REASON_MAX_TOKENS);
        }
        else
        {
            ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
        }
    }

    /* Parse message */
    if (json_object_has_member(obj, "message"))
    {
        JsonObject *message = json_object_get_object_member(obj, "message");
        const gchar *content = json_object_get_string_member_with_default(
            message, "content", "");

        if (content != NULL && content[0] != '\0')
        {
            g_autoptr(AiTextContent) text_content = ai_text_content_new(content);
            ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&text_content));
        }
    }

    /* Parse usage */
    {
        gint prompt_tokens = json_object_get_int_member_with_default(obj, "prompt_eval_count", 0);
        gint output_tokens = json_object_get_int_member_with_default(obj, "eval_count", 0);

        if (prompt_tokens > 0 || output_tokens > 0)
        {
            g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, output_tokens);
            ai_response_set_usage(response, usage);
        }
    }

    return (AiResponse *)g_steal_pointer(&response);
}

static gchar *
ai_ollama_client_get_endpoint_url(AiClient *client)
{
    AiConfig *config = ai_client_get_config(client);
    const gchar *base_url = ai_config_get_base_url(config, AI_PROVIDER_OLLAMA);

    return g_strconcat(base_url, OLLAMA_CHAT_ENDPOINT, NULL);
}

static void
ai_ollama_client_add_auth_headers(
    AiClient    *client,
    SoupMessage *msg
){
    /* Ollama doesn't require authentication */
    (void)client;
    (void)msg;
}

static void
ai_ollama_client_class_init(AiOllamaClientClass *klass)
{
    AiClientClass *client_class = AI_CLIENT_CLASS(klass);

    client_class->build_request = ai_ollama_client_build_request;
    client_class->parse_response = ai_ollama_client_parse_response;
    client_class->get_endpoint_url = ai_ollama_client_get_endpoint_url;
    client_class->add_auth_headers = ai_ollama_client_add_auth_headers;
}

static void
ai_ollama_client_init(AiOllamaClient *self)
{
    (void)self;
    ai_client_set_model(AI_CLIENT(self), AI_OLLAMA_DEFAULT_MODEL);
}

/*
 * AiProvider interface
 */

static AiProviderType
ai_ollama_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_OLLAMA;
}

static const gchar *
ai_ollama_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "Ollama";
}

static const gchar *
ai_ollama_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_OLLAMA_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiOllamaClient *client;
    GTask          *task;
    SoupMessage    *msg;
} OllamaChatAsyncData;

static void
ollama_chat_async_data_free(OllamaChatAsyncData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->msg);
    g_slice_free(OllamaChatAsyncData, data);
}

static void
on_ollama_chat_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OllamaChatAsyncData *data = user_data;
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
        ollama_chat_async_data_free(data);
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

        ollama_chat_async_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        ollama_chat_async_data_free(data);
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

    ollama_chat_async_data_free(data);
}

static void
ai_ollama_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOllamaClient *self = AI_OLLAMA_CLIENT(provider);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    OllamaChatAsyncData *data;
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

    data = g_slice_new0(OllamaChatAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_ollama_chat_response,
        data);
}

static AiResponse *
ai_ollama_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_ollama_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    task = g_task_new(provider, NULL, callback, user_data);

    /* Common Ollama models */
    models = g_list_append(models, g_strdup("llama3.2"));
    models = g_list_append(models, g_strdup("llama3.1"));
    models = g_list_append(models, g_strdup("mistral"));
    models = g_list_append(models, g_strdup("codellama"));

    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
}

static GList *
ai_ollama_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_ollama_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_ollama_client_get_provider_type;
    iface->get_name = ai_ollama_client_get_name;
    iface->get_default_model = ai_ollama_client_get_default_model;
    iface->chat_async = ai_ollama_client_chat_async;
    iface->chat_finish = ai_ollama_client_chat_finish;
    iface->list_models_async = ai_ollama_client_list_models_async;
    iface->list_models_finish = ai_ollama_client_list_models_finish;
}

/*
 * AiStreamable interface implementation
 *
 * Ollama uses NDJSON (newline-delimited JSON) streaming.
 * Each line is a complete JSON object with message.content delta.
 * Last message has done: true
 */

typedef struct
{
    AiOllamaClient   *client;
    GTask            *task;
    SoupMessage      *msg;
    GInputStream     *input_stream;
    GDataInputStream *data_stream;
    GCancellable     *cancellable;

    AiResponse       *response;
    GString          *current_text;

    gboolean          stream_started;
} OllamaStreamData;

static void
ollama_stream_data_free(OllamaStreamData *data)
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

    g_slice_free(OllamaStreamData, data);
}

static void
ollama_process_stream_chunk(
    OllamaStreamData *data,
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
        const gchar *model = json_object_get_string_member_with_default(obj, "model", "");

        data->response = ai_response_new("", model);
        data->current_text = g_string_new("");
        data->stream_started = TRUE;

        g_signal_emit_by_name(data->client, "stream-start");
    }

    /* Parse message content delta */
    if (json_object_has_member(obj, "message"))
    {
        JsonObject *message = json_object_get_object_member(obj, "message");
        const gchar *content = json_object_get_string_member_with_default(
            message, "content", "");

        if (content != NULL && content[0] != '\0')
        {
            g_string_append(data->current_text, content);
            g_signal_emit_by_name(data->client, "delta", content);
        }
    }

    /* Check if done */
    if (json_object_get_boolean_member_with_default(obj, "done", FALSE))
    {
        const gchar *done_reason = json_object_get_string_member_with_default(
            obj, "done_reason", "");

        if (g_strcmp0(done_reason, "length") == 0)
        {
            ai_response_set_stop_reason(data->response, AI_STOP_REASON_MAX_TOKENS);
        }
        else
        {
            ai_response_set_stop_reason(data->response, AI_STOP_REASON_END_TURN);
        }

        /* Parse usage from final message */
        {
            gint prompt_tokens = json_object_get_int_member_with_default(obj, "prompt_eval_count", 0);
            gint output_tokens = json_object_get_int_member_with_default(obj, "eval_count", 0);

            if (prompt_tokens > 0 || output_tokens > 0)
            {
                g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, output_tokens);
                ai_response_set_usage(data->response, usage);
            }
        }

        /* Finalize response */
        if (data->current_text != NULL && data->current_text->len > 0)
        {
            g_autoptr(AiTextContent) text_content = ai_text_content_new(data->current_text->str);
            ai_response_add_content_block(data->response, (AiContentBlock *)g_steal_pointer(&text_content));
        }

        g_signal_emit_by_name(data->client, "stream-end", data->response);
    }
}

static void ollama_read_next_line(OllamaStreamData *data);

static void
on_ollama_line_read(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OllamaStreamData *data = user_data;
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
            ollama_stream_data_free(data);
        }
        return;
    }

    if (line == NULL)
    {
        /* EOF */
        if (data->response != NULL)
        {
            g_task_return_pointer(data->task, g_object_ref(data->response), g_object_unref);
        }
        else
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                    "Stream ended without valid response");
        }
        ollama_stream_data_free(data);
        return;
    }

    /* Ollama uses NDJSON - each line is a complete JSON object */
    if (length > 0)
    {
        ollama_process_stream_chunk(data, line);
    }

    ollama_read_next_line(data);
}

static void
ollama_read_next_line(OllamaStreamData *data)
{
    g_data_input_stream_read_line_async(
        data->data_stream,
        G_PRIORITY_DEFAULT,
        data->cancellable,
        on_ollama_line_read,
        data);
}

static void
on_ollama_stream_ready(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OllamaStreamData *data = user_data;
    g_autoptr(GError) error = NULL;

    data->input_stream = soup_session_send_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)),
        result,
        &error);

    if (data->input_stream == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        ollama_stream_data_free(data);
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
        ollama_stream_data_free(data);
        return;
    }

    data->data_stream = g_data_input_stream_new(data->input_stream);
    g_data_input_stream_set_newline_type(data->data_stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    ollama_read_next_line(data);
}

static JsonNode *
ai_ollama_client_build_stream_request(
    AiClient    *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    GList       *tools
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *model;
    GList *l;

    (void)tools; /* TODO: Implement tool support */

    model = ai_client_get_model(client);
    if (model == NULL)
    {
        model = AI_OLLAMA_DEFAULT_MODEL;
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

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
        AiRole role = ai_message_get_role(msg);
        g_autofree gchar *text = ai_message_get_text(msg);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "role");
        json_builder_add_string_value(builder, ai_role_to_string(role));
        json_builder_set_member_name(builder, "content");
        json_builder_add_string_value(builder, text != NULL ? text : "");
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);

    /* Enable streaming */
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);

    /* Options */
    json_builder_set_member_name(builder, "options");
    json_builder_begin_object(builder);

    if (max_tokens > 0)
    {
        json_builder_set_member_name(builder, "num_predict");
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

static void
ai_ollama_client_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOllamaClient *self = AI_OLLAMA_CLIENT(streamable);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    gsize request_len;
    OllamaStreamData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    request_json = ai_ollama_client_build_stream_request(
        AI_CLIENT(self), messages, system_prompt, max_tokens, tools);

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

    url = klass->get_endpoint_url(AI_CLIENT(self));

    msg = soup_message_new("POST", url);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");

    soup_message_set_request_body_from_bytes(msg, "application/json",
        g_bytes_new_take(g_steal_pointer(&request_body), request_len));

    data = g_slice_new0(OllamaStreamData);
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
        on_ollama_stream_ready,
        data);
}

static AiResponse *
ai_ollama_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    (void)streamable;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_ollama_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_ollama_client_chat_stream_async;
    iface->chat_stream_finish = ai_ollama_client_chat_stream_finish;
}

/*
 * Public API
 */

AiOllamaClient *
ai_ollama_client_new(void)
{
    g_autoptr(AiOllamaClient) self = g_object_new(AI_TYPE_OLLAMA_CLIENT, NULL);

    return (AiOllamaClient *)g_steal_pointer(&self);
}

AiOllamaClient *
ai_ollama_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiOllamaClient) self = g_object_new(AI_TYPE_OLLAMA_CLIENT,
                                                   "config", config,
                                                   NULL);

    return (AiOllamaClient *)g_steal_pointer(&self);
}

AiOllamaClient *
ai_ollama_client_new_with_host(const gchar *host)
{
    g_autoptr(AiConfig) config = ai_config_new();

    ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, host);

    return ai_ollama_client_new_with_config(config);
}
