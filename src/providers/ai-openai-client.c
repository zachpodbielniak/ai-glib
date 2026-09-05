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
#include "core/ai-json-util.h"
#include "providers/ai-openai-shared.h"
#include "providers/ai-image-shared.h"
#include "core/ai-error.h"
#include "core/ai-http-error.h"
#include "core/ai-event.h"
#include "core/ai-event-source.h"
#include "core/ai-image-generator.h"
#include "core/ai-embedder.h"
#include "providers/ai-embedding-shared.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-image-request.h"
#include "model/ai-generated-image.h"
#include "model/ai-image-response.h"

#define OPENAI_COMPLETIONS_ENDPOINT "/v1/chat/completions"
#define OPENAI_IMAGES_ENDPOINT "/v1/images/generations"
#define OPENAI_IMAGES_EDITS_ENDPOINT "/v1/images/edits"
#define OPENAI_IMAGES_VARIATIONS_ENDPOINT "/v1/images/variations"
#define OPENAI_MODELS_ENDPOINT "/v1/models"
#define OPENAI_EMBEDDINGS_ENDPOINT "/v1/embeddings"

/*
 * Interface implementations forward declarations.
 */
static void ai_openai_client_provider_init(AiProviderInterface *iface);
static void ai_openai_client_embedder_init(AiEmbedderInterface *iface);
static void ai_openai_client_streamable_init(AiStreamableInterface *iface);
static void ai_openai_client_image_generator_init(AiImageGeneratorInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiOpenAIClient, ai_openai_client, AI_TYPE_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_openai_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_openai_client_streamable_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_IMAGE_GENERATOR,
                                              ai_openai_client_image_generator_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_EMBEDDER,
                                              ai_openai_client_embedder_init))

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

    /* Messages — per-provider serialization (handles tool_calls/tool messages
     * correctly; the generic ai_message_to_json emits Anthropic content
     * blocks which OpenAI rejects for tool flows). */
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);
    ai_openai_shared_serialize_messages_array(builder, messages, system_prompt, AI_OPENAI_SERIALIZE_DEFAULT);
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

    /*
     * NULL is reachable: json_parser_get_root() answers it for an empty
     * document, which a 200 with no body produces.  JSON_NODE_HOLDS_OBJECT()
     * would dereference it -- a critical, and fatal under
     * G_DEBUG=fatal-warnings -- so the NULL check comes first.
     */
    if (json == NULL || !JSON_NODE_HOLDS_OBJECT(json))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Expected JSON object in response");
        return NULL;
    }

    obj = json_node_get_object(json);

    /*
     * Check for an error response.  Presence, not shape: a server that
     * answers `"error": "boom"` has still said it failed, and reading the
     * message off a NULL object gives the generic wording rather than a
     * pair of criticals.
     */
    if (ai_json_get_node(obj, "error") != NULL)
    {
        JsonObject *err_obj = ai_json_get_object(obj, "error");
        const gchar *err_msg = ai_json_get_string(err_obj, "message",
                                                  "Unknown error");

        g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", err_msg);
        return NULL;
    }

    id = ai_json_get_string(obj, "id", "");
    model = ai_json_get_string(obj, "model", "");

    response = ai_response_new(id, model);

    /* Parse usage */
    {
        JsonObject *usage_obj = ai_json_get_object(obj, "usage");

        if (usage_obj != NULL)
        {
            gint prompt_tokens = (gint)ai_json_get_int(usage_obj, "prompt_tokens", 0);
            gint completion_tokens = (gint)ai_json_get_int(usage_obj, "completion_tokens", 0);
            g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, completion_tokens);

            ai_response_set_usage(response, usage);
        }
    }

    /* Parse choices */
    {
        JsonArray  *choices = ai_json_get_array(obj, "choices");
        JsonObject *choice = ai_json_array_get_object(choices, 0);

        if (choice != NULL)
        {
            const gchar *finish_reason =
                ai_json_get_string(choice, "finish_reason", "");
            JsonObject *message = ai_json_get_object(choice, "message");

            ai_response_set_stop_reason(response, ai_stop_reason_from_string(finish_reason));

            if (message != NULL)
            {
                /* Content */
                {
                    JsonNode *content_node = ai_json_get_node(message, "content");

                    if (content_node != NULL && JSON_NODE_HOLDS_VALUE(content_node))
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
                {
                    JsonArray *tool_calls = ai_json_get_array(message, "tool_calls");
                    guint len = tool_calls != NULL ? json_array_get_length(tool_calls) : 0;
                    guint i;

                    for (i = 0; i < len; i++)
                    {
                        JsonObject *tc = ai_json_array_get_object(tool_calls, i);
                        const gchar *tc_id = ai_json_get_string(tc, "id", "");
                        JsonObject *func = ai_json_get_object(tc, "function");
                        const gchar *name;
                        const gchar *args_str;
                        g_autoptr(AiToolUse) tool_use = NULL;

                        if (func != NULL)
                        {
                            name = ai_json_get_string(func, "name", "");
                            args_str = ai_json_get_string(func, "arguments", "{}");

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
ai_openai_client_build_api_url(AiOpenAIClient *self, const gchar *path)
{
	AiConfig *config = ai_client_get_config(AI_CLIENT(self));
	return g_strconcat(ai_config_get_base_url(config, AI_PROVIDER_OPENAI), path, NULL);
}

static gchar *
openai_api_url(AiOpenAIClient *self, const gchar *path)
{
	return AI_OPENAI_CLIENT_GET_CLASS(self)->build_api_url(self, path);
}

static gchar *
ai_openai_client_get_endpoint_url(AiClient *client)
{
	return openai_api_url(AI_OPENAI_CLIENT(client), OPENAI_COMPLETIONS_ENDPOINT);
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

    klass->build_api_url = ai_openai_client_build_api_url;

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
    /*
     * g_task_return_*() does NOT consume the reference the async function
     * took from g_task_new(); it owns that until the operation is finished
     * with.  The early-error paths unref directly, so only the completion
     * paths -- the ones that hand the task to `data` -- reach here, and
     * every one of them leaked a GTask and everything it referenced: the
     * task holds the source object, so a leaked task leaked the client, its
     * config, and their strings.
     *
     * Safe against the retry hand-off, which sets data->task to NULL before
     * freeing precisely so the task survives into the second attempt.
     */
    g_clear_object(&data->task);
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

        {
            GError *http_error = NULL;

            ai_http_error_set_from_bytes(&http_error, NULL, status,
                                         response_bytes);
            g_task_return_error(data->task, g_steal_pointer(&http_error));
        }

        openai_chat_async_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);

    /*
     * A 200 with an empty body: g_bytes_get_data() answers NULL for
     * zero-length bytes, and json_parser_load_from_data() asserts on a NULL
     * pointer -- a critical, and fatal under G_DEBUG=fatal-warnings.  An
     * empty document is a normal parse failure, so hand it one and let the
     * existing error path report it.
     */
    if (response_data == NULL)
    {
        response_data = "";
        response_len = 0;
    }
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

    msg = url != NULL ? soup_message_new("POST", url) : NULL;
    if (msg == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Invalid API base URL or token");
        g_object_unref(task);
        return;
    }
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");

    klass->add_auth_headers(AI_CLIENT(self), msg);

    {
        gsize body_len = strlen(request_body);
        g_autoptr(GBytes) body_bytes =
            g_bytes_new_take(g_steal_pointer(&request_body), body_len);

        soup_message_set_request_body_from_bytes(msg, "application/json",
                                                 body_bytes);
    }

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

typedef struct
{
    AiOpenAIClient *client;
    GTask          *task;
    SoupMessage    *msg;
} OpenAIListModelsData;

static void
openai_list_models_data_free(OpenAIListModelsData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->task);
    g_clear_object(&data->msg);
    g_slice_free(OpenAIListModelsData, data);
}

static void
on_openai_list_models_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OpenAIListModelsData *data = user_data;
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
        openai_list_models_data_free(data);
        return;
    }

    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(data->msg)))
    {
        guint status = soup_message_get_status(data->msg);

        {
            GError *http_error = NULL;

            ai_http_error_set_from_bytes(&http_error, NULL, status,
                                         response_bytes);
            g_task_return_error(data->task, g_steal_pointer(&http_error));
        }

        openai_list_models_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);

    /*
     * A 200 with an empty body: g_bytes_get_data() answers NULL for
     * zero-length bytes, and json_parser_load_from_data() asserts on a NULL
     * pointer -- a critical, and fatal under G_DEBUG=fatal-warnings.  An
     * empty document is a normal parse failure, so hand it one and let the
     * existing error path report it.
     */
    if (response_data == NULL)
    {
        response_data = "";
        response_len = 0;
    }
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        openai_list_models_data_free(data);
        return;
    }

    /* {"object": "list", "data": [{"id": "gpt-...", ...}, ...]} */
    root = ai_json_root_object(parser);
    arr = ai_json_get_array(root, "data");

    if (arr == NULL)
    {
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                "Malformed model list response");
        openai_list_models_data_free(data);
        return;
    }

    n = json_array_get_length(arr);
    for (i = 0; i < n; i++)
    {
        JsonObject *entry = ai_json_array_get_object(arr, i);

        if (ai_json_get_string(entry, "id", NULL) != NULL)
        {
            models = g_list_append(models,
                g_strdup(ai_json_get_string(entry, "id", "")));
        }
    }

    g_task_return_pointer(data->task, models, NULL);
    openai_list_models_data_free(data);
}

static void
ai_openai_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOpenAIClient *self = AI_OPENAI_CLIENT(provider);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    OpenAIListModelsData *data;
    GTask *task;

    task = g_task_new(provider, cancellable, callback, user_data);

    url = openai_api_url(self, OPENAI_MODELS_ENDPOINT);
    msg = url != NULL ? soup_message_new("GET", url) : NULL;
    if (msg == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Invalid API base URL or token");
        g_object_unref(task);
        return;
    }
    klass->add_auth_headers(AI_CLIENT(self), msg);

    data = g_slice_new0(OpenAIListModelsData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_openai_list_models_response,
        data);
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
    g_clear_object(&data->task);
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

        {
            AiUsage *final_usage = ai_response_get_usage(data->response);
        
            if (final_usage != NULL)
            {
                g_autoptr(AiEvent) event = ai_event_new_usage(final_usage, -1);
                ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
            }
        }
        
        {
            g_autoptr(AiEvent) event = ai_event_new(AI_EVENT_STREAM_END);
            ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
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

    /*
     * A bare `null` payload parses fine and yields a NULL root, which
     * JSON_NODE_HOLDS_OBJECT() would dereference -- a critical, and fatal
     * under G_DEBUG=fatal-warnings. Server output is untrusted, so the NULL
     * check comes first.
     */
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
    {
        return;
    }

    obj = json_node_get_object(root);

    /* First chunk - extract id and model, emit stream-start */
    if (!data->stream_started)
    {
        const gchar *id = ai_json_get_string(obj, "id", "");
        const gchar *model = ai_json_get_string(obj, "model", "");

        data->response = ai_response_new(id, model);
        data->current_text = g_string_new("");
        data->stream_started = TRUE;

        g_signal_emit_by_name(data->client, "stream-start");
        {
            g_autoptr(AiEvent) event = ai_event_new(AI_EVENT_STREAM_START);
            ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
        }
    }

    /* Parse choices */
    {
        JsonArray  *choices = ai_json_get_array(obj, "choices");
        JsonObject *choice = ai_json_array_get_object(choices, 0);

        if (choice != NULL)
        {
            /* Check finish_reason.  A chunk mid-stream sends JSON null
             * here, which the accessor reports as absent. */
            const gchar *finish_reason =
                ai_json_get_string(choice, "finish_reason", NULL);
            JsonObject *delta = ai_json_get_object(choice, "delta");

            if (finish_reason != NULL)
            {
                ai_response_set_stop_reason(data->response,
                    ai_stop_reason_from_string(finish_reason));
            }

            /* Parse delta */
            if (delta != NULL)
            {
                /* Content delta */
                {
                    const gchar *content =
                        ai_json_get_string(delta, "content", NULL);

                    if (content != NULL)
                    {
                        g_string_append(data->current_text, content);
                        g_signal_emit_by_name(data->client, "delta", content);
                        {
                            g_autoptr(AiEvent) event = ai_event_new_text_delta(content);
                            ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
                        }
                    }
                }

                /* Tool calls delta */
                {
                    JsonArray *tool_calls = ai_json_get_array(delta, "tool_calls");
                    guint len = tool_calls != NULL ? json_array_get_length(tool_calls) : 0;
                    guint i;

                    if (len > 0 && data->tool_calls == NULL)
                    {
                        data->tool_calls = g_hash_table_new_full(
                            g_str_hash, g_str_equal,
                            g_free, (GDestroyNotify)openai_tool_call_free);
                    }

                    for (i = 0; i < len; i++)
                    {
                        JsonObject *tc = ai_json_array_get_object(tool_calls, i);
                        gint index = (gint)ai_json_get_int(tc, "index", 0);
                        g_autofree gchar *index_key = g_strdup_printf("%d", index);
                        const gchar *tc_id = ai_json_get_string(tc, "id", NULL);
                        JsonObject *func = ai_json_get_object(tc, "function");
                        OpenAIToolCall *existing;

                        if (tc == NULL)
                        {
                            continue;
                        }

                        existing = g_hash_table_lookup(data->tool_calls, index_key);
                        if (existing == NULL)
                        {
                            existing = g_slice_new0(OpenAIToolCall);
                            existing->arguments = g_string_new("");

                            /* Get id if present */
                            if (tc_id != NULL)
                            {
                                g_free(index_key);
                                index_key = g_strdup(tc_id);
                            }

                            g_hash_table_insert(data->tool_calls, g_strdup(index_key), existing);
                        }

                        /* Parse function */
                        if (func != NULL)
                        {
                            const gchar *name = ai_json_get_string(func, "name", NULL);
                            const gchar *args = ai_json_get_string(func, "arguments", NULL);

                            if (name != NULL)
                            {
                                g_free(existing->name);
                                existing->name = g_strdup(name);
                            }

                            if (args != NULL)
                            {
                                g_string_append(existing->arguments, args);
                            }
                        }
                    }
                }
            }
        }
    }

    /* Parse usage if present (usually in final chunk) */
    {
        JsonObject *usage_obj = ai_json_get_object(obj, "usage");

        if (usage_obj != NULL)
        {
            gint prompt_tokens = (gint)ai_json_get_int(usage_obj, "prompt_tokens", 0);
            gint completion_tokens = (gint)ai_json_get_int(usage_obj, "completion_tokens", 0);
            g_autoptr(AiUsage) usage = ai_usage_new(prompt_tokens, completion_tokens);

            ai_response_set_usage(data->response, usage);
        }
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
        /* A cancelled read must still complete the task.  ai-glib calls
         * neither g_task_set_return_on_cancel nor
         * g_task_set_check_cancellable anywhere, so a handler that
         * returns without completing leaves the GTask pending for the
         * life of the process: the caller's callback never runs, an
         * AiConversation never clears :busy, and every later turn is
         * refused by a conversation that looks permanently in flight.
         *
         * Returning the G_IO_ERROR_CANCELLED is what the caller expects
         * -- g_task_return_error completes even on a cancelled
         * cancellable, and ai_*_finish reports the cancellation. */
        g_task_return_error(data->task, g_steal_pointer(&error));
        openai_stream_data_free(data);
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

    /* SSE permits data: with or without one following space. */
    if (g_str_has_prefix(line, "data:"))
    {
        const gchar *payload = line + 5;
        g_autoptr(JsonParser) parser = json_parser_new();
        JsonObject *object;

        if (*payload == ' ')
            payload++;
        if (json_parser_load_from_data(parser, payload, -1, NULL))
        {
            object = ai_json_root_object(parser);
            if (ai_json_get_node(object, "error") != NULL)
            {
                ai_http_error_set(&error, NULL, 500, payload, strlen(payload));
                g_task_return_error(data->task, g_steal_pointer(&error));
                openai_stream_data_free(data);
                return;
            }
        }
        openai_process_stream_chunk(data, payload);
        if (strcmp(payload, "[DONE]") == 0)
        {
            if (data->response != NULL)
                g_task_return_pointer(data->task, g_object_ref(data->response), g_object_unref);
            else
                g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                        "Stream ended without valid response");
            openai_stream_data_free(data);
            return;
        }
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

        {
            GError *http_error = NULL;

            ai_http_error_set_from_stream(&http_error, NULL, status,
                                          data->input_stream, NULL);
            g_task_return_error(data->task, g_steal_pointer(&http_error));
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
    g_autoptr(JsonNode) request = NULL;
    JsonObject *object;
    JsonObject *options;

    request = AI_CLIENT_GET_CLASS(client)->build_request(
        client, messages, system_prompt, max_tokens, tools);
    if (request == NULL)
        return NULL;
    object = json_node_get_object(request);
    json_object_set_boolean_member(object, "stream", TRUE);
    options = json_object_new();
    json_object_set_boolean_member(options, "include_usage", TRUE);
    json_object_set_object_member(object, "stream_options", options);
    return g_steal_pointer(&request);
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

    msg = url != NULL ? soup_message_new("POST", url) : NULL;
    if (msg == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Invalid API base URL or token");
        g_object_unref(task);
        return;
    }
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Content-Type", "application/json");
    soup_message_headers_append(soup_message_get_request_headers(msg),
                                "Accept", "text/event-stream");

    klass->add_auth_headers(AI_CLIENT(self), msg);

    {
        g_autoptr(GBytes) body_bytes =
            g_bytes_new_take(g_steal_pointer(&request_body), request_len);

        soup_message_set_request_body_from_bytes(msg, "application/json",
                                                 body_bytes);
    }

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
 * OpenAI serves two image families that share an endpoint but not a
 * parameter set: DALL-E accepts response_format and style, while GPT Image
 * rejects both, uses a different quality vocabulary, and adds transparency,
 * output-format and streaming controls.  Rather than branching on the model
 * id throughout, each family is described by an AiImageModelInfo and the
 * shared builders emit only what the chosen model declares.
 *
 * Endpoints: POST /v1/images/generations, /v1/images/edits, /v1/images/variations
 */

typedef struct
{
    AiOpenAIClient *client;
    GTask          *task;
    gchar          *model;
} OpenAIImageGenData;

static void
openai_image_gen_data_free(OpenAIImageGenData *data)
{
    g_clear_object(&data->task);
    g_clear_object(&data->client);
    g_clear_pointer(&data->model, g_free);
    g_slice_free(OpenAIImageGenData, data);
}

/*
 * The GPT Image family: transparency, output encoding, editing with masks
 * and multiple references, streaming previews.  Notably absent is
 * URL_RESPONSE -- these models always return base64 and reject a
 * response_format member outright -- and STYLE, which they ignore.
 */
static const gchar * const openai_gpt_image_sizes[] = {
    "1024x1024", "1536x1024", "1024x1536", "auto", NULL
};
static const gchar * const openai_gpt_image_qualities[] = {
    "auto", "low", "medium", "high", NULL
};

/* DALL-E 3: one image at a time, its own quality words, plus style. */
static const gchar * const openai_dalle3_sizes[] = {
    "1024x1024", "1792x1024", "1024x1792", NULL
};
static const gchar * const openai_dalle3_qualities[] = {
    "standard", "hd", NULL
};

/* DALL-E 2: square sizes, batch generation, single-image edits + variations. */
static const gchar * const openai_dalle2_sizes[] = {
    "256x256", "512x512", "1024x1024", NULL
};

static AiImageModelInfo *
ai_openai_image_model_new_gpt_image(const gchar *id, const gchar *name)
{
    AiImageModelInfo *info;

    info = ai_image_model_info_new(
        id, name, AI_PROVIDER_OPENAI,
        AI_IMAGE_CAP_REFERENCE_IMAGES | AI_IMAGE_CAP_MULTI_REFERENCE |
        AI_IMAGE_CAP_MASK | AI_IMAGE_CAP_PIXEL_SIZE |
        AI_IMAGE_CAP_TRANSPARENCY | AI_IMAGE_CAP_OUTPUT_FORMAT |
        AI_IMAGE_CAP_QUALITY | AI_IMAGE_CAP_MULTI_COUNT |
        AI_IMAGE_CAP_PARTIAL_STREAMING | AI_IMAGE_CAP_SAFETY_CONTROL |
        AI_IMAGE_CAP_INPUT_FIDELITY);

    ai_image_model_info_set_sizes(info, openai_gpt_image_sizes);
    ai_image_model_info_set_qualities(info, openai_gpt_image_qualities);
    ai_image_model_info_set_max_count(info, 10);
    ai_image_model_info_set_max_reference_images(info, 16);
    ai_image_model_info_set_notes(
        info, "Always returns base64; rejects response_format and style");

    return info;
}

static GList *
ai_openai_client_list_image_models(AiImageGenerator *generator)
{
    GList *models = NULL;
    AiImageModelInfo *info;

    (void)generator;

    models = g_list_append(models, ai_openai_image_model_new_gpt_image(
        AI_OPENAI_IMAGE_MODEL_GPT_IMAGE_2, "GPT Image 2"));
    models = g_list_append(models, ai_openai_image_model_new_gpt_image(
        AI_OPENAI_IMAGE_MODEL_GPT_IMAGE_1_5, "GPT Image 1.5"));
    models = g_list_append(models, ai_openai_image_model_new_gpt_image(
        AI_OPENAI_IMAGE_MODEL_GPT_IMAGE_1, "GPT Image 1"));

    info = ai_image_model_info_new(
        AI_OPENAI_IMAGE_MODEL_DALL_E_3, "DALL-E 3", AI_PROVIDER_OPENAI,
        AI_IMAGE_CAP_PIXEL_SIZE | AI_IMAGE_CAP_QUALITY |
        AI_IMAGE_CAP_STYLE | AI_IMAGE_CAP_URL_RESPONSE |
        AI_IMAGE_CAP_PROMPT_ENHANCEMENT);
    ai_image_model_info_set_sizes(info, openai_dalle3_sizes);
    ai_image_model_info_set_qualities(info, openai_dalle3_qualities);
    ai_image_model_info_set_max_count(info, 1);
    ai_image_model_info_set_notes(info, "One image per request; revises prompts");
    models = g_list_append(models, info);

    info = ai_image_model_info_new(
        AI_OPENAI_IMAGE_MODEL_DALL_E_2, "DALL-E 2", AI_PROVIDER_OPENAI,
        AI_IMAGE_CAP_PIXEL_SIZE | AI_IMAGE_CAP_URL_RESPONSE |
        AI_IMAGE_CAP_MULTI_COUNT | AI_IMAGE_CAP_REFERENCE_IMAGES |
        AI_IMAGE_CAP_MASK | AI_IMAGE_CAP_VARIATION);
    ai_image_model_info_set_sizes(info, openai_dalle2_sizes);
    ai_image_model_info_set_max_count(info, 10);
    ai_image_model_info_set_max_reference_images(info, 1);
    ai_image_model_info_set_notes(info, "Edits take one image plus an optional mask");
    models = g_list_append(models, info);

    return models;
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
    const gchar *response_data;
    gsize response_len;
    AiImageResponse *response;

    (void)source;

    /* Status mapping, retries and body-excerpt error messages all happen
     * inside the shared sender. */
    response_bytes = ai_image_shared_send_finish(result, &error);

    if (response_bytes == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        openai_image_gen_data_free(data);
        return;
    }

    response_data = g_bytes_get_data(response_bytes, &response_len);

    /*
     * A 200 with an empty body: g_bytes_get_data() answers NULL for
     * zero-length bytes, and json_parser_load_from_data() asserts on a NULL
     * pointer -- a critical, and fatal under G_DEBUG=fatal-warnings.  An
     * empty document is a normal parse failure, so hand it one and let the
     * existing error path report it.
     */
    if (response_data == NULL)
    {
        response_data = "";
        response_len = 0;
    }
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, response_data, response_len, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        openai_image_gen_data_free(data);
        return;
    }

    response = ai_image_shared_parse_openai_response(
        json_parser_get_root(parser), data->model, &error);

    if (response == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
    }
    else
    {
        guint count = ai_image_response_get_image_count(response);

        /* A non-streaming request has no intermediate progress to report,
         * so this is a single terminal event rather than a series. */
        g_signal_emit_by_name(data->client, "image-progress", count, count);

        g_task_return_pointer(data->task, response,
                              (GDestroyNotify)ai_image_response_free);
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
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) body_bytes = NULL;
    const AiImageModelInfo *info;
    AiImageOperation operation;
    AiConfig *config;
    const gchar *endpoint;
    const gchar *model;
    OpenAIImageGenData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    model = ai_image_request_get_model(request);
    if (model == NULL)
    {
        model = ai_image_generator_get_default_model(generator);
    }

    if (model == NULL || model[0] == '\0')
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "An image model is required");
        g_object_unref(task);
        return;
    }
    info = ai_image_generator_get_model_info(generator, model);

    /* Reconcile the request with what this model accepts before building
     * anything.  Lenient by default: unsupported parameters are dropped
     * rather than sent and rejected. */
    if (!ai_image_request_validate(request, info, AI_IMAGE_VALIDATE_NONE,
                                   &error))
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    config = ai_client_get_config(AI_CLIENT(self));
    operation = ai_image_request_get_operation(request);

    /* Editing and variations are multipart form posts to their own
     * endpoints; plain generation is JSON. */
    if (operation == AI_IMAGE_OPERATION_EDIT ||
        operation == AI_IMAGE_OPERATION_VARIATION ||
        ai_image_request_get_reference_image_count(request) > 0)
    {
        g_autoptr(SoupMultipart) multipart = NULL;

        endpoint = operation == AI_IMAGE_OPERATION_VARIATION
                   ? OPENAI_IMAGES_VARIATIONS_ENDPOINT
                   : OPENAI_IMAGES_EDITS_ENDPOINT;

        multipart = ai_image_shared_build_openai_multipart(request, model,
                                                           info, &error);
        if (multipart == NULL)
        {
            g_task_return_error(task, g_steal_pointer(&error));
            g_object_unref(task);
            return;
        }

        url = openai_api_url(self, endpoint);
        msg = url != NULL ? soup_message_new("POST", url) : NULL;

        if (msg == NULL)
        {
            g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                    "Invalid API base URL or token");
            g_object_unref(task);
            return;
        }

        /*
         * Serialise the multipart ourselves rather than using
         * soup_message_new_from_multipart(): the retrying sender needs the
         * body as a GBytes it can replay onto a fresh message, and there
         * is no way to read a body back off a SoupMessage.
         */
        soup_multipart_to_message(multipart,
                                  soup_message_get_request_headers(msg),
                                  &body_bytes);

        {
            g_autofree gchar *content_type = g_strdup(soup_message_headers_get_one(
                soup_message_get_request_headers(msg), "Content-Type"));

            soup_message_set_request_body_from_bytes(msg, content_type,
                                                     body_bytes);
        }
    }
    else
    {
        g_autoptr(JsonNode) request_json = NULL;
        g_autofree gchar *request_body = NULL;
        gsize request_len = 0;

        request_json = ai_image_shared_build_openai_json(request, model, info);
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

        url = openai_api_url(self, OPENAI_IMAGES_ENDPOINT);
        msg = url != NULL ? soup_message_new("POST", url) : NULL;

        if (msg == NULL)
        {
            g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                    "Invalid API base URL or token");
            g_object_unref(task);
            return;
        }

        body_bytes = g_bytes_new_take(g_steal_pointer(&request_body),
                                      request_len);
        soup_message_set_request_body_from_bytes(msg, "application/json",
                                                 body_bytes);
    }

    klass->add_auth_headers(AI_CLIENT(self), msg);

    data = g_slice_new0(OpenAIImageGenData);
    data->client = g_object_ref(self);
    data->task = task;
    data->model = g_strdup(model);

    ai_image_shared_send_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        body_bytes,
        ai_config_get_max_retries(config),
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
    iface->get_default_model = ai_openai_client_get_image_default_model;
    iface->list_image_models = ai_openai_client_list_image_models;
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

/*
 * Embeddings
 *
 * POST /v1/embeddings, which is the shape every OpenAI-compatible server
 * also serves -- so pointing base_url at vLLM, LM Studio, llama.cpp or
 * Together reaches this same code with a different model name.
 *
 * text-embedding-3-* accept a "dimensions" parameter that truncates the
 * vector. It is deliberately not exposed: a store cannot compare vectors of
 * two widths, so the width has to be a property of the model as configured
 * rather than of an individual call, and the model table is where that
 * belongs.
 */

static const AiEmbeddingModelInfo openai_embedding_models[] = {
    {
        AI_OPENAI_EMBEDDING_MODEL_3_SMALL,
        1536,
        32000,
        TRUE,
        "1536 dimensions. The usual choice; cheaper than 3-large and "
        "close behind it."
    },
    {
        AI_OPENAI_EMBEDDING_MODEL_3_LARGE,
        3072,
        32000,
        TRUE,
        "3072 dimensions, and twice the storage per passage."
    },
    {
        "text-embedding-ada-002",
        1536,
        32000,
        TRUE,
        "Superseded by text-embedding-3-small, which is better and cheaper."
    }
};

typedef struct
{
    GTask *task;
    gchar *model;
    gsize  expected;
} OpenAIEmbedData;

static void
openai_embed_data_free(OpenAIEmbedData *data)
{
    g_clear_pointer(&data->model, g_free);
    g_slice_free(OpenAIEmbedData, data);
}

static void
on_openai_embed_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    OpenAIEmbedData *data = user_data;
    g_autoptr(GTask) task = data->task;
    g_autoptr(GBytes) body = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = NULL;
    AiEmbedding *embedding;
    const gchar *text;
    gsize length = 0;

    (void)source;

    body = ai_image_shared_send_finish(result, &error);

    if (NULL == body)
    {
        g_task_return_error(task, g_steal_pointer(&error));
        openai_embed_data_free(data);
        return;
    }

    text = g_bytes_get_data(body, &length);
    if (text == NULL)
        text = "";
    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, text, (gssize)length, &error))
    {
        g_task_return_error(task, g_steal_pointer(&error));
        openai_embed_data_free(data);
        return;
    }

    embedding = ai_embedding_shared_parse(AI_EMBEDDING_WIRE_OPENAI,
                                          json_parser_get_root(parser),
                                          data->model, data->expected,
                                          &error);

    if (NULL == embedding)
        g_task_return_error(task, g_steal_pointer(&error));
    else
        g_task_return_pointer(task, embedding,
                              (GDestroyNotify)ai_embedding_unref);

    openai_embed_data_free(data);
}

static void
ai_openai_client_embed_async(
    AiEmbedder          *embedder,
    const gchar *const  *texts,
    const gchar         *model,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOpenAIClient *self = AI_OPENAI_CLIENT(embedder);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(SoupMessage) msg = NULL;
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(GBytes) body_bytes = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    OpenAIEmbedData *data;
    AiConfig *config;
    gsize request_len = 0;
    gsize count = 0;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    if (NULL == model)
        model = ai_embedder_get_default_embedding_model(embedder);

    if (model == NULL || model[0] == '\0')
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "An embedding model is required");
        g_object_unref(task);
        return;
    }

    while (NULL != texts[count])
        count++;

    if (0 == count)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "There is nothing to embed");
        g_object_unref(task);
        return;
    }

    config = ai_client_get_config(AI_CLIENT(self));

    request_json = ai_embedding_shared_build_request(texts, model);

    {
        g_autoptr(JsonGenerator) gen = json_generator_new();

        json_generator_set_root(gen, request_json);
        request_body = json_generator_to_data(gen, &request_len);
    }

    url = openai_api_url(self, OPENAI_EMBEDDINGS_ENDPOINT);
    msg = url != NULL ? soup_message_new("POST", url) : NULL;

    if (NULL == msg)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Invalid API base URL or token");
        g_object_unref(task);
        return;
    }

    body_bytes = g_bytes_new_take(g_steal_pointer(&request_body), request_len);
    soup_message_set_request_body_from_bytes(msg, "application/json",
                                             body_bytes);

    klass->add_auth_headers(AI_CLIENT(self), msg);

    data = g_slice_new0(OpenAIEmbedData);
    data->task = task;
    data->model = g_strdup(model);
    data->expected = count;

    ai_image_shared_send_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        body_bytes,
        ai_config_get_max_retries(config),
        cancellable,
        on_openai_embed_response,
        data);
}

static AiEmbedding *
ai_openai_client_embed_finish(
    AiEmbedder    *embedder,
    GAsyncResult  *result,
    GError       **error
){
    (void)embedder;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static const gchar *
ai_openai_client_get_default_embedding_model(AiEmbedder *embedder)
{
    (void)embedder;
    return AI_OPENAI_EMBEDDING_MODEL_3_SMALL;
}

static GList *
ai_openai_client_list_embedding_models(AiEmbedder *embedder)
{
    GList *out = NULL;
    gsize i;

    (void)embedder;

    for (i = G_N_ELEMENTS(openai_embedding_models); i > 0; i--)
        out = g_list_prepend(out,
            (gpointer)&openai_embedding_models[i - 1]);

    return out;
}

static void
ai_openai_client_embedder_init(AiEmbedderInterface *iface)
{
    iface->embed_async = ai_openai_client_embed_async;
    iface->embed_finish = ai_openai_client_embed_finish;
    iface->get_default_embedding_model =
        ai_openai_client_get_default_embedding_model;
    iface->list_embedding_models = ai_openai_client_list_embedding_models;
}
