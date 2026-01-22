/*
 * ai-openai-client.c - OpenAI GPT client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-openai-client.h"
#include "core/ai-error.h"
#include "core/ai-image-generator.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-image-request.h"
#include "model/ai-generated-image.h"
#include "model/ai-image-response.h"

#define OPENAI_COMPLETIONS_ENDPOINT "/v1/chat/completions"
#define OPENAI_IMAGES_ENDPOINT "/v1/images/generations"

/*
 * Private structure for AiOpenAIClient.
 */
struct _AiOpenAIClient
{
    AiClient parent_instance;
};

/*
 * Interface implementations forward declarations.
 */
static void ai_openai_client_provider_init(AiProviderInterface *iface);
static void ai_openai_client_streamable_init(AiStreamableInterface *iface);
static void ai_openai_client_image_generator_init(AiImageGeneratorInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiOpenAIClient, ai_openai_client, AI_TYPE_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_openai_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_openai_client_streamable_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_IMAGE_GENERATOR,
                                              ai_openai_client_image_generator_init))

/*
 * Build the JSON request body for OpenAI's Chat Completions API.
 */
static JsonNode *
ai_openai_client_build_request(
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
        model = AI_OPENAI_DEFAULT_MODEL;
    }

    json_builder_begin_object(builder);

    /* Model */
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    /* Max tokens */
    if (max_tokens > 0)
    {
        json_builder_set_member_name(builder, "max_tokens");
        json_builder_add_int_value(builder, max_tokens);
    }

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
        g_autoptr(JsonNode) msg_node = ai_message_to_json(msg);

        json_builder_add_value(builder, g_steal_pointer(&msg_node));
    }

    json_builder_end_array(builder);

    /* Tools */
    if (tools != NULL)
    {
        json_builder_set_member_name(builder, "tools");
        json_builder_begin_array(builder);

        for (l = tools; l != NULL; l = l->next)
        {
            AiTool *tool = l->data;
            g_autoptr(JsonNode) tool_node = ai_tool_to_json(tool, AI_PROVIDER_OPENAI);

            json_builder_add_value(builder, g_steal_pointer(&tool_node));
        }

        json_builder_end_array(builder);
    }

    /* Temperature */
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
 * Parse OpenAI's response JSON into an AiResponse.
 */
static AiResponse *
ai_openai_client_parse_response(
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

    /* Check for error response */
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

    /* Parse usage */
    if (json_object_has_member(obj, "usage"))
    {
        JsonObject *usage_obj = json_object_get_object_member(obj, "usage");
        gint prompt_tokens = json_object_get_int_member_with_default(usage_obj, "prompt_tokens", 0);
        gint completion_tokens = json_object_get_int_member_with_default(usage_obj, "completion_tokens", 0);
        g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, completion_tokens);

        ai_response_set_usage(response, usage);
    }

    /* Parse choices */
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

                /* Content */
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

                /* Tool calls */
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

/*
 * Get the OpenAI Chat Completions endpoint URL.
 */
static gchar *
ai_openai_client_get_endpoint_url(AiClient *client)
{
    AiConfig *config = ai_client_get_config(client);
    const gchar *base_url = ai_config_get_base_url(config, AI_PROVIDER_OPENAI);

    return g_strconcat(base_url, OPENAI_COMPLETIONS_ENDPOINT, NULL);
}

/*
 * Add OpenAI-specific authentication headers.
 */
static void
ai_openai_client_add_auth_headers(
    AiClient    *client,
    SoupMessage *msg
){
    AiConfig *config = ai_client_get_config(client);
    const gchar *api_key = ai_config_get_api_key(config, AI_PROVIDER_OPENAI);
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);
    g_autofree gchar *auth_header = NULL;

    if (api_key != NULL)
    {
        auth_header = g_strdup_printf("Bearer %s", api_key);
        soup_message_headers_append(headers, "Authorization", auth_header);
    }
}

static void
ai_openai_client_class_init(AiOpenAIClientClass *klass)
{
    AiClientClass *client_class = AI_CLIENT_CLASS(klass);

    /* Override virtual methods */
    client_class->build_request = ai_openai_client_build_request;
    client_class->parse_response = ai_openai_client_parse_response;
    client_class->get_endpoint_url = ai_openai_client_get_endpoint_url;
    client_class->add_auth_headers = ai_openai_client_add_auth_headers;
}

static void
ai_openai_client_init(AiOpenAIClient *self)
{
    (void)self;

    /* Set default model */
    ai_client_set_model(AI_CLIENT(self), AI_OPENAI_DEFAULT_MODEL);
}

/*
 * AiProvider interface implementation
 */

static AiProviderType
ai_openai_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_OPENAI;
}

static const gchar *
ai_openai_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "OpenAI";
}

static const gchar *
ai_openai_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_OPENAI_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiOpenAIClient *client;
    GTask          *task;
    SoupMessage    *msg;
} OpenAIChatAsyncData;

static void
openai_chat_async_data_free(OpenAIChatAsyncData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->msg);
    g_slice_free(OpenAIChatAsyncData, data);
}

static void
on_openai_chat_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OpenAIChatAsyncData *data = user_data;
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
        openai_chat_async_data_free(data);
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

        openai_chat_async_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        openai_chat_async_data_free(data);
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

    openai_chat_async_data_free(data);
}

static void
ai_openai_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOpenAIClient *self = AI_OPENAI_CLIENT(provider);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    OpenAIChatAsyncData *data;
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

    data = g_slice_new0(OpenAIChatAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_openai_chat_response,
        data);
}

static AiResponse *
ai_openai_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_openai_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    task = g_task_new(provider, NULL, callback, user_data);

    models = g_list_append(models, g_strdup("gpt-4o"));
    models = g_list_append(models, g_strdup("gpt-4o-mini"));
    models = g_list_append(models, g_strdup("gpt-4-turbo"));
    models = g_list_append(models, g_strdup("gpt-3.5-turbo"));

    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
}

static GList *
ai_openai_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_openai_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_openai_client_get_provider_type;
    iface->get_name = ai_openai_client_get_name;
    iface->get_default_model = ai_openai_client_get_default_model;
    iface->chat_async = ai_openai_client_chat_async;
    iface->chat_finish = ai_openai_client_chat_finish;
    iface->list_models_async = ai_openai_client_list_models_async;
    iface->list_models_finish = ai_openai_client_list_models_finish;
}

/*
 * AiStreamable interface implementation
 *
 * OpenAI uses SSE with:
 *   data: {"id":...,"choices":[{"delta":{"content":"..."}}]}\n\n
 *   data: [DONE]\n\n
 */

typedef struct
{
    AiOpenAIClient   *client;
    GTask            *task;
    SoupMessage      *msg;
    GInputStream     *input_stream;
    GDataInputStream *data_stream;
    GCancellable     *cancellable;

    /* Response being built */
    AiResponse       *response;
    GString          *current_text;

    /* Tool call accumulation */
    GHashTable       *tool_calls;  /* id -> {name, arguments} */

    /* SSE parsing state */
    gchar            *current_event_data;

    /* State tracking */
    gboolean          stream_started;
} OpenAIStreamData;

typedef struct
{
    gchar   *name;
    GString *arguments;
} OpenAIToolCall;

static void
openai_tool_call_free(OpenAIToolCall *tc)
{
    g_free(tc->name);
    if (tc->arguments != NULL)
    {
        g_string_free(tc->arguments, TRUE);
    }
    g_slice_free(OpenAIToolCall, tc);
}

static void
openai_stream_data_free(OpenAIStreamData *data)
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
    if (data->tool_calls != NULL)
    {
        g_hash_table_destroy(data->tool_calls);
    }
    g_clear_pointer(&data->current_event_data, g_free);

    g_slice_free(OpenAIStreamData, data);
}

static void
openai_process_stream_chunk(
    OpenAIStreamData *data,
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

    /* Check for [DONE] marker */
    if (g_strcmp0(json_str, "[DONE]") == 0)
    {
        /* Finalize response */
        if (data->current_text != NULL && data->current_text->len > 0)
        {
            g_autoptr(AiTextContent) content = ai_text_content_new(data->current_text->str);
            ai_response_add_content_block(data->response, (AiContentBlock *)g_steal_pointer(&content));
        }

        /* Add accumulated tool calls */
        if (data->tool_calls != NULL)
        {
            GHashTableIter iter;
            gpointer key, value;

            g_hash_table_iter_init(&iter, data->tool_calls);
            while (g_hash_table_iter_next(&iter, &key, &value))
            {
                const gchar *tc_id = key;
                OpenAIToolCall *tc = value;
                g_autoptr(AiToolUse) tool_use = ai_tool_use_new_from_json_string(
                    tc_id, tc->name, tc->arguments->str);
                ai_response_add_content_block(data->response, (AiContentBlock *)g_steal_pointer(&tool_use));
            }
        }

        g_signal_emit_by_name(data->client, "stream-end", data->response);
        return;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, -1, &error))
    {
        g_debug("Failed to parse OpenAI SSE chunk: %s", error->message);
        return;
    }

    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        return;
    }

    obj = json_node_get_object(root);

    /* First chunk - extract id and model, emit stream-start */
    if (!data->stream_started)
    {
        const gchar *id = json_object_get_string_member_with_default(obj, "id", "");
        const gchar *model = json_object_get_string_member_with_default(obj, "model", "");

        data->response = ai_response_new(id, model);
        data->current_text = g_string_new("");
        data->stream_started = TRUE;

        g_signal_emit_by_name(data->client, "stream-start");
    }

    /* Parse choices */
    if (json_object_has_member(obj, "choices"))
    {
        JsonArray *choices = json_object_get_array_member(obj, "choices");

        if (json_array_get_length(choices) > 0)
        {
            JsonObject *choice = json_array_get_object_element(choices, 0);

            /* Check finish_reason */
            if (json_object_has_member(choice, "finish_reason"))
            {
                JsonNode *fr_node = json_object_get_member(choice, "finish_reason");
                if (!JSON_NODE_HOLDS_NULL(fr_node))
                {
                    const gchar *finish_reason = json_node_get_string(fr_node);
                    ai_response_set_stop_reason(data->response,
                        ai_stop_reason_from_string(finish_reason));
                }
            }

            /* Parse delta */
            if (json_object_has_member(choice, "delta"))
            {
                JsonObject *delta = json_object_get_object_member(choice, "delta");

                /* Content delta */
                if (json_object_has_member(delta, "content"))
                {
                    JsonNode *content_node = json_object_get_member(delta, "content");
                    if (JSON_NODE_HOLDS_VALUE(content_node))
                    {
                        const gchar *content = json_node_get_string(content_node);
                        if (content != NULL)
                        {
                            g_string_append(data->current_text, content);
                            g_signal_emit_by_name(data->client, "delta", content);
                        }
                    }
                }

                /* Tool calls delta */
                if (json_object_has_member(delta, "tool_calls"))
                {
                    JsonArray *tool_calls = json_object_get_array_member(delta, "tool_calls");
                    guint len = json_array_get_length(tool_calls);
                    guint i;

                    if (data->tool_calls == NULL)
                    {
                        data->tool_calls = g_hash_table_new_full(
                            g_str_hash, g_str_equal,
                            g_free, (GDestroyNotify)openai_tool_call_free);
                    }

                    for (i = 0; i < len; i++)
                    {
                        JsonObject *tc = json_array_get_object_element(tool_calls, i);
                        gint index = json_object_get_int_member_with_default(tc, "index", 0);
                        g_autofree gchar *index_key = g_strdup_printf("%d", index);
                        OpenAIToolCall *existing;

                        existing = g_hash_table_lookup(data->tool_calls, index_key);
                        if (existing == NULL)
                        {
                            existing = g_slice_new0(OpenAIToolCall);
                            existing->arguments = g_string_new("");

                            /* Get id if present */
                            if (json_object_has_member(tc, "id"))
                            {
                                g_free(index_key);
                                index_key = g_strdup(json_object_get_string_member(tc, "id"));
                            }

                            g_hash_table_insert(data->tool_calls, g_strdup(index_key), existing);
                        }

                        /* Parse function */
                        if (json_object_has_member(tc, "function"))
                        {
                            JsonObject *func = json_object_get_object_member(tc, "function");

                            if (json_object_has_member(func, "name"))
                            {
                                g_free(existing->name);
                                existing->name = g_strdup(json_object_get_string_member(func, "name"));
                            }

                            if (json_object_has_member(func, "arguments"))
                            {
                                const gchar *args = json_object_get_string_member(func, "arguments");
                                g_string_append(existing->arguments, args);
                            }
                        }
                    }
                }
            }
        }
    }

    /* Parse usage if present (usually in final chunk) */
    if (json_object_has_member(obj, "usage"))
    {
        JsonObject *usage_obj = json_object_get_object_member(obj, "usage");
        gint prompt_tokens = json_object_get_int_member_with_default(usage_obj, "prompt_tokens", 0);
        gint completion_tokens = json_object_get_int_member_with_default(usage_obj, "completion_tokens", 0);
        g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, completion_tokens);

        ai_response_set_usage(data->response, usage);
    }
}

static void openai_read_next_line(OpenAIStreamData *data);

static void
on_openai_line_read(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OpenAIStreamData *data = user_data;
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
            openai_stream_data_free(data);
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
        openai_stream_data_free(data);
        return;
    }

    /* Parse SSE: data: {json} */
    if (g_str_has_prefix(line, "data: "))
    {
        openai_process_stream_chunk(data, line + 6);
    }

    openai_read_next_line(data);
}

static void
openai_read_next_line(OpenAIStreamData *data)
{
    g_data_input_stream_read_line_async(
        data->data_stream,
        G_PRIORITY_DEFAULT,
        data->cancellable,
        on_openai_line_read,
        data);
}

static void
on_openai_stream_ready(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OpenAIStreamData *data = user_data;
    g_autoptr(GError) error = NULL;

    data->input_stream = soup_session_send_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)),
        result,
        &error);

    if (data->input_stream == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        openai_stream_data_free(data);
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
        openai_stream_data_free(data);
        return;
    }

    data->data_stream = g_data_input_stream_new(data->input_stream);
    g_data_input_stream_set_newline_type(data->data_stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    openai_read_next_line(data);
}

static JsonNode *
ai_openai_client_build_stream_request(
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
        model = AI_OPENAI_DEFAULT_MODEL;
    }

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    /* Enable streaming */
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);

    /* Request usage in stream */
    json_builder_set_member_name(builder, "stream_options");
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "include_usage");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

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
            g_autoptr(JsonNode) tool_node = ai_tool_to_json(tool, AI_PROVIDER_OPENAI);
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

static void
ai_openai_client_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOpenAIClient *self = AI_OPENAI_CLIENT(streamable);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    gsize request_len;
    OpenAIStreamData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    request_json = ai_openai_client_build_stream_request(
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
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Accept", "text/event-stream");

    klass->add_auth_headers(AI_CLIENT(self), msg);

    soup_message_set_request_body_from_bytes(msg, "application/json",
        g_bytes_new_take(g_steal_pointer(&request_body), request_len));

    data = g_slice_new0(OpenAIStreamData);
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
        on_openai_stream_ready,
        data);
}

static AiResponse *
ai_openai_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    (void)streamable;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_openai_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_openai_client_chat_stream_async;
    iface->chat_stream_finish = ai_openai_client_chat_stream_finish;
}

/*
 * AiImageGenerator interface implementation
 *
 * OpenAI supports image generation via the DALL-E models and gpt-image-1.
 * Endpoint: POST /v1/images/generations
 */

typedef struct
{
    AiOpenAIClient *client;
    GTask          *task;
    SoupMessage    *msg;
} OpenAIImageGenData;

static void
openai_image_gen_data_free(OpenAIImageGenData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->msg);
    g_slice_free(OpenAIImageGenData, data);
}

/*
 * Build the JSON request for OpenAI image generation.
 */
static JsonNode *
ai_openai_client_build_image_request(
    AiOpenAIClient *self,
    AiImageRequest *request
){
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *model;
    const gchar *size_str;
    const gchar *quality_str;
    const gchar *style_str;
    const gchar *format_str;

    (void)self;

    json_builder_begin_object(builder);

    /* Prompt (required) */
    json_builder_set_member_name(builder, "prompt");
    json_builder_add_string_value(builder, ai_image_request_get_prompt(request));

    /* Model */
    model = ai_image_request_get_model(request);
    if (model == NULL)
    {
        model = AI_OPENAI_IMAGE_DEFAULT_MODEL;
    }
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    /* Size */
    {
        AiImageSize size = ai_image_request_get_size(request);
        if (size == AI_IMAGE_SIZE_CUSTOM)
        {
            size_str = ai_image_request_get_custom_size(request);
        }
        else
        {
            size_str = ai_image_size_to_string(size);
        }

        if (size_str != NULL)
        {
            json_builder_set_member_name(builder, "size");
            json_builder_add_string_value(builder, size_str);
        }
    }

    /* Quality */
    quality_str = ai_image_quality_to_string(ai_image_request_get_quality(request));
    if (quality_str != NULL)
    {
        json_builder_set_member_name(builder, "quality");
        json_builder_add_string_value(builder, quality_str);
    }

    /* Style */
    style_str = ai_image_style_to_string(ai_image_request_get_style(request));
    if (style_str != NULL)
    {
        json_builder_set_member_name(builder, "style");
        json_builder_add_string_value(builder, style_str);
    }

    /* Number of images */
    {
        gint count = ai_image_request_get_count(request);
        if (count > 1)
        {
            json_builder_set_member_name(builder, "n");
            json_builder_add_int_value(builder, count);
        }
    }

    /* Response format */
    format_str = ai_image_response_format_to_string(ai_image_request_get_response_format(request));
    json_builder_set_member_name(builder, "response_format");
    json_builder_add_string_value(builder, format_str);

    /* User (optional) */
    {
        const gchar *user = ai_image_request_get_user(request);
        if (user != NULL)
        {
            json_builder_set_member_name(builder, "user");
            json_builder_add_string_value(builder, user);
        }
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * Parse OpenAI image generation response.
 */
static AiImageResponse *
ai_openai_client_parse_image_response(
    JsonNode  *json,
    GError   **error
){
    JsonObject *obj;
    gint64 created;
    g_autoptr(AiImageResponse) response = NULL;
    JsonArray *data_array;
    guint i;
    guint len;

    if (!JSON_NODE_HOLDS_OBJECT(json))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Expected JSON object in response");
        return NULL;
    }

    obj = json_node_get_object(json);

    /* Check for error response */
    if (json_object_has_member(obj, "error"))
    {
        JsonObject *err_obj = json_object_get_object_member(obj, "error");
        const gchar *err_msg = json_object_get_string_member_with_default(
            err_obj, "message", "Unknown error");

        g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", err_msg);
        return NULL;
    }

    created = json_object_get_int_member_with_default(obj, "created", 0);
    response = ai_image_response_new(NULL, created);

    /* Parse data array */
    if (json_object_has_member(obj, "data"))
    {
        data_array = json_object_get_array_member(obj, "data");
        len = json_array_get_length(data_array);

        for (i = 0; i < len; i++)
        {
            JsonObject *img_obj = json_array_get_object_element(data_array, i);
            AiGeneratedImage *image = NULL;

            if (json_object_has_member(img_obj, "url"))
            {
                const gchar *url = json_object_get_string_member(img_obj, "url");
                image = ai_generated_image_new_from_url(url);
            }
            else if (json_object_has_member(img_obj, "b64_json"))
            {
                const gchar *b64 = json_object_get_string_member(img_obj, "b64_json");
                image = ai_generated_image_new_from_base64(b64, "image/png");
            }

            if (image != NULL)
            {
                /* Set revised prompt if present */
                if (json_object_has_member(img_obj, "revised_prompt"))
                {
                    const gchar *revised = json_object_get_string_member(img_obj, "revised_prompt");
                    ai_generated_image_set_revised_prompt(image, revised);
                }

                ai_image_response_add_image(response, image);
            }
        }
    }

    return (AiImageResponse *)g_steal_pointer(&response);
}

static void
on_openai_image_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OpenAIImageGenData *data = user_data;
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
        openai_image_gen_data_free(data);
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
            /* Try to parse error message from response body */
            response_data = g_bytes_get_data(response_bytes, &response_len);
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_SERVER_ERROR,
                                    "Request failed (HTTP %u): %.*s", status,
                                    (int)MIN(response_len, 200), response_data);
        }

        openai_image_gen_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        openai_image_gen_data_free(data);
        return;
    }

    response_json = json_parser_get_root(parser);
    response = ai_openai_client_parse_image_response(response_json, &error);

    if (response == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
    }
    else
    {
        g_task_return_pointer(data->task, response, (GDestroyNotify)ai_image_response_free);
    }

    openai_image_gen_data_free(data);
}

static void
ai_openai_client_generate_image_async(
    AiImageGenerator    *generator,
    AiImageRequest      *request,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOpenAIClient *self = AI_OPENAI_CLIENT(generator);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    gsize request_len;
    AiConfig *config;
    const gchar *base_url;
    OpenAIImageGenData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    request_json = ai_openai_client_build_image_request(self, request);
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
    base_url = ai_config_get_base_url(config, AI_PROVIDER_OPENAI);
    url = g_strconcat(base_url, OPENAI_IMAGES_ENDPOINT, NULL);

    msg = soup_message_new("POST", url);
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");

    klass->add_auth_headers(AI_CLIENT(self), msg);

    soup_message_set_request_body_from_bytes(msg, "application/json",
        g_bytes_new_take(g_steal_pointer(&request_body), request_len));

    data = g_slice_new0(OpenAIImageGenData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_openai_image_response,
        data);
}

static AiImageResponse *
ai_openai_client_generate_image_finish(
    AiImageGenerator  *generator,
    GAsyncResult      *result,
    GError           **error
){
    (void)generator;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static GList *
ai_openai_client_get_supported_sizes(AiImageGenerator *generator)
{
    GList *sizes = NULL;

    (void)generator;

    sizes = g_list_append(sizes, g_strdup("256x256"));
    sizes = g_list_append(sizes, g_strdup("512x512"));
    sizes = g_list_append(sizes, g_strdup("1024x1024"));
    sizes = g_list_append(sizes, g_strdup("1024x1792"));
    sizes = g_list_append(sizes, g_strdup("1792x1024"));

    return sizes;
}

static const gchar *
ai_openai_client_get_image_default_model(AiImageGenerator *generator)
{
    (void)generator;
    return AI_OPENAI_IMAGE_DEFAULT_MODEL;
}

static void
ai_openai_client_image_generator_init(AiImageGeneratorInterface *iface)
{
    iface->generate_image_async = ai_openai_client_generate_image_async;
    iface->generate_image_finish = ai_openai_client_generate_image_finish;
    iface->get_supported_sizes = ai_openai_client_get_supported_sizes;
    iface->get_default_model = ai_openai_client_get_image_default_model;
}

/*
 * Public API
 */

/**
 * ai_openai_client_new:
 *
 * Creates a new #AiOpenAIClient using the default configuration.
 * The API key will be read from the OPENAI_API_KEY environment variable.
 *
 * Returns: (transfer full): a new #AiOpenAIClient
 */
AiOpenAIClient *
ai_openai_client_new(void)
{
    g_autoptr(AiOpenAIClient) self = g_object_new(AI_TYPE_OPENAI_CLIENT, NULL);

    return (AiOpenAIClient *)g_steal_pointer(&self);
}

/**
 * ai_openai_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiOpenAIClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiOpenAIClient
 */
AiOpenAIClient *
ai_openai_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiOpenAIClient) self = g_object_new(AI_TYPE_OPENAI_CLIENT,
                                                   "config", config,
                                                   NULL);

    return (AiOpenAIClient *)g_steal_pointer(&self);
}

/**
 * ai_openai_client_new_with_key:
 * @api_key: the OpenAI API key
 *
 * Creates a new #AiOpenAIClient with the specified API key.
 *
 * Returns: (transfer full): a new #AiOpenAIClient
 */
AiOpenAIClient *
ai_openai_client_new_with_key(const gchar *api_key)
{
    g_autoptr(AiConfig) config = ai_config_new();

    ai_config_set_api_key(config, AI_PROVIDER_OPENAI, api_key);

    return ai_openai_client_new_with_config(config);
}
