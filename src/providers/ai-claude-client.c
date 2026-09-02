/*
 * ai-claude-client.c - Anthropic Claude client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-claude-client.h"
#include "core/ai-json-util.h"
#include "core/ai-error.h"
#include "core/ai-http-error.h"
#include "core/ai-event.h"
#include "core/ai-event-source.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

#define CLAUDE_MESSAGES_ENDPOINT "/v1/messages"
#define CLAUDE_MODELS_ENDPOINT "/v1/models?limit=100"

/*
 * Private structure for AiClaudeClient.
 */
struct _AiClaudeClient
{
    AiClient parent_instance;

    gchar *api_version;
};

/*
 * Interface implementations forward declarations.
 */
static void ai_claude_client_provider_init(AiProviderInterface *iface);
static void ai_claude_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiClaudeClient, ai_claude_client, AI_TYPE_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_claude_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_claude_client_streamable_init))

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_API_VERSION,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_claude_client_finalize(GObject *object)
{
    AiClaudeClient *self = AI_CLAUDE_CLIENT(object);

    g_clear_pointer(&self->api_version, g_free);

    G_OBJECT_CLASS(ai_claude_client_parent_class)->finalize(object);
}

static void
ai_claude_client_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(object);

    switch (prop_id)
    {
        case PROP_API_VERSION:
            g_value_set_string(value, self->api_version);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_claude_client_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(object);

    switch (prop_id)
    {
        case PROP_API_VERSION:
            g_clear_pointer(&self->api_version, g_free);
            self->api_version = g_value_dup_string(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * Build the JSON request body for Claude's Messages API.
 */
static JsonNode *
ai_claude_client_build_request(
    AiClient    *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    GList       *tools
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(client);
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *model;
    GList *l;

    (void)self;

    model = ai_client_get_model(client);
    if (model == NULL)
    {
        model = AI_CLAUDE_DEFAULT_MODEL;
    }

    json_builder_begin_object(builder);

    /* Model */
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    /* Max tokens */
    json_builder_set_member_name(builder, "max_tokens");
    json_builder_add_int_value(builder, max_tokens > 0 ? max_tokens : 4096);

    /* System prompt */
    if (system_prompt != NULL && system_prompt[0] != '\0')
    {
        json_builder_set_member_name(builder, "system");
        json_builder_add_string_value(builder, system_prompt);
    }

    /* Messages */
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);

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
            g_autoptr(JsonNode) tool_node = ai_tool_to_json(tool, AI_PROVIDER_CLAUDE);

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
 * Parse Claude's response JSON into an AiResponse.
 */
static AiResponse *
ai_claude_client_parse_response(
    AiClient  *client,
    JsonNode  *json,
    GError   **error
){
    JsonObject *obj;
    const gchar *id;
    const gchar *model;
    const gchar *stop_reason_str;
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
     * answers `"error": "boom"` has still said it failed, and the
     * accessors below fall back to the generic wording rather than
     * criticalling their way to it.
     */
    if (ai_json_get_node(obj, "error") != NULL)
    {
        JsonObject *err_obj = ai_json_get_object(obj, "error");
        const gchar *err_msg = ai_json_get_string(err_obj, "message",
                                                  "Unknown error");
        const gchar *err_type = ai_json_get_string(err_obj, "type", "error");

        if (g_strcmp0(err_type, "authentication_error") == 0)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY, "%s", err_msg);
        }
        else if (g_strcmp0(err_type, "rate_limit_error") == 0)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED, "%s", err_msg);
        }
        else
        {
            g_set_error(error, AI_ERROR, AI_ERROR_SERVER_ERROR, "%s", err_msg);
        }

        return NULL;
    }

    id = ai_json_get_string(obj, "id", "");
    model = ai_json_get_string(obj, "model", "");
    stop_reason_str = ai_json_get_string(obj, "stop_reason", "");

    response = ai_response_new(id, model);
    ai_response_set_stop_reason(response, ai_stop_reason_from_string(stop_reason_str));

    /* Parse usage */
    {
        JsonObject *usage_obj = ai_json_get_object(obj, "usage");

        if (usage_obj != NULL)
        {
            gint input_tokens = (gint)ai_json_get_int(usage_obj, "input_tokens", 0);
            gint output_tokens = (gint)ai_json_get_int(usage_obj, "output_tokens", 0);
            g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

            ai_response_set_usage(response, usage);
        }
    }

    /* Parse content */
    {
        JsonArray *content_arr = ai_json_get_array(obj, "content");
        guint len = content_arr != NULL ? json_array_get_length(content_arr) : 0;
        guint i;

        for (i = 0; i < len; i++)
        {
            JsonObject *block_obj = ai_json_array_get_object(content_arr, i);
            const gchar *type;

            /* An element that is not an object describes no block. */
            if (block_obj == NULL)
            {
                continue;
            }

            type = ai_json_get_string(block_obj, "type", "text");

            if (g_strcmp0(type, "text") == 0)
            {
                const gchar *text = ai_json_get_string(block_obj, "text", "");
                g_autoptr(AiTextContent) content = ai_text_content_new(text);

                ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&content));
            }
            else if (g_strcmp0(type, "tool_use") == 0)
            {
                const gchar *tool_id = ai_json_get_string(block_obj, "id", "");
                const gchar *name = ai_json_get_string(block_obj, "name", "");
                JsonNode *input = ai_json_get_node(block_obj, "input");
                g_autoptr(AiToolUse) tool_use = ai_tool_use_new(tool_id, name, input);

                ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&tool_use));
            }
        }
    }

    return (AiResponse *)g_steal_pointer(&response);
}

/*
 * Get the Claude Messages API endpoint URL.
 */
static gchar *
ai_claude_client_get_endpoint_url(AiClient *client)
{
    AiConfig *config = ai_client_get_config(client);
    const gchar *base_url = ai_config_get_base_url(config, AI_PROVIDER_CLAUDE);

    return g_strconcat(base_url, CLAUDE_MESSAGES_ENDPOINT, NULL);
}

/*
 * Add Claude-specific authentication headers.
 */
static void
ai_claude_client_add_auth_headers(
    AiClient    *client,
    SoupMessage *msg
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(client);
    AiConfig *config = ai_client_get_config(client);
    const gchar *api_key = ai_config_get_api_key(config, AI_PROVIDER_CLAUDE);
    SoupMessageHeaders *headers = soup_message_get_request_headers(msg);

    if (api_key != NULL)
    {
        soup_message_headers_append(headers, "x-api-key", api_key);
    }

    soup_message_headers_append(headers, "anthropic-version",
        self->api_version != NULL ? self->api_version : AI_CLAUDE_API_VERSION);
}

static void
ai_claude_client_class_init(AiClaudeClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiClientClass *client_class = AI_CLIENT_CLASS(klass);

    object_class->finalize = ai_claude_client_finalize;
    object_class->get_property = ai_claude_client_get_property;
    object_class->set_property = ai_claude_client_set_property;

    /* Override virtual methods */
    client_class->build_request = ai_claude_client_build_request;
    client_class->parse_response = ai_claude_client_parse_response;
    client_class->get_endpoint_url = ai_claude_client_get_endpoint_url;
    client_class->add_auth_headers = ai_claude_client_add_auth_headers;

    /**
     * AiClaudeClient:api-version:
     *
     * The Anthropic API version to use.
     */
    properties[PROP_API_VERSION] =
        g_param_spec_string("api-version",
                            "API Version",
                            "The Anthropic API version to use",
                            AI_CLAUDE_API_VERSION,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_claude_client_init(AiClaudeClient *self)
{
    self->api_version = g_strdup(AI_CLAUDE_API_VERSION);

    /* Set default model */
    ai_client_set_model(AI_CLIENT(self), AI_CLAUDE_DEFAULT_MODEL);
}

/*
 * AiProvider interface implementation
 */

static AiProviderType
ai_claude_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_CLAUDE;
}

static const gchar *
ai_claude_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "Claude";
}

static const gchar *
ai_claude_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_CLAUDE_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiClaudeClient *client;
    GTask          *task;
    SoupMessage    *msg;
} ChatAsyncData;

static void
chat_async_data_free(ChatAsyncData *data)
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
    g_slice_free(ChatAsyncData, data);
}

static void
on_chat_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    ChatAsyncData *data = user_data;
    g_autoptr(GBytes) response_bytes = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = NULL;
    SoupMessage *msg = data->msg;
    const gchar *response_data;
    gsize response_len;
    JsonNode *response_json;
    AiClientClass *klass;
    AiResponse *response;

    (void)source;  /* source is SoupSession, we use data->msg instead */

    response_bytes = soup_session_send_and_read_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)), result, &error);

    if (response_bytes == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        chat_async_data_free(data);
        return;
    }

    /* Check HTTP status */
    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(msg)))
    {
        guint status = soup_message_get_status(msg);

        {
            GError *http_error = NULL;

            ai_http_error_set_from_bytes(&http_error, NULL, status,
                                         response_bytes);
            g_task_return_error(data->task, g_steal_pointer(&http_error));
        }

        chat_async_data_free(data);
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
        chat_async_data_free(data);
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

    chat_async_data_free(data);
}

static void
ai_claude_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(provider);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    ChatAsyncData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    /* Build request */
    request_json = klass->build_request(AI_CLIENT(self), messages, system_prompt,
                                        max_tokens, tools);
    if (request_json == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build request");
        g_object_unref(task);
        return;
    }

    /* Serialize to JSON string */
    {
        g_autoptr(JsonGenerator) gen = json_generator_new();
        json_generator_set_root(gen, request_json);
        request_body = json_generator_to_data(gen, NULL);
    }

    /* Get endpoint URL */
    url = klass->get_endpoint_url(AI_CLIENT(self));

    /* Create HTTP request */
    msg = soup_message_new("POST", url);
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

    /* Set up callback data */
    data = g_slice_new0(ChatAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    /* Send request */
    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_chat_response,
        data);
}

static AiResponse *
ai_claude_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

typedef struct
{
    AiClaudeClient *client;
    GTask          *task;
    SoupMessage    *msg;
} ClaudeListModelsData;

static void
claude_list_models_data_free(ClaudeListModelsData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->task);
    g_clear_object(&data->msg);
    g_slice_free(ClaudeListModelsData, data);
}

static void
on_claude_list_models_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    ClaudeListModelsData *data = user_data;
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
        claude_list_models_data_free(data);
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

        claude_list_models_data_free(data);
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
        claude_list_models_data_free(data);
        return;
    }

    /* {"data": [{"type": "model", "id": "claude-...", ...}, ...]} */
    root = ai_json_root_object(parser);
    arr = ai_json_get_array(root, "data");

    if (arr == NULL)
    {
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                "Malformed model list response");
        claude_list_models_data_free(data);
        return;
    }

    n = json_array_get_length(arr);
    for (i = 0; i < n; i++)
    {
        JsonObject  *entry = ai_json_array_get_object(arr, i);
        const gchar *id = ai_json_get_string(entry, "id", NULL);

        if (id != NULL)
        {
            models = g_list_append(models, g_strdup(id));
        }
    }

    g_task_return_pointer(data->task, models, NULL);
    claude_list_models_data_free(data);
}

static void
ai_claude_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(provider);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    AiConfig *config = ai_client_get_config(AI_CLIENT(self));
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    ClaudeListModelsData *data;
    GTask *task;

    task = g_task_new(provider, cancellable, callback, user_data);

    url = g_strconcat(ai_config_get_base_url(config, AI_PROVIDER_CLAUDE),
                      CLAUDE_MODELS_ENDPOINT, NULL);
    msg = soup_message_new("GET", url);
    klass->add_auth_headers(AI_CLIENT(self), msg);

    data = g_slice_new0(ClaudeListModelsData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);

    soup_session_send_and_read_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_claude_list_models_response,
        data);
}

static GList *
ai_claude_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_claude_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_claude_client_get_provider_type;
    iface->get_name = ai_claude_client_get_name;
    iface->get_default_model = ai_claude_client_get_default_model;
    iface->chat_async = ai_claude_client_chat_async;
    iface->chat_finish = ai_claude_client_chat_finish;
    iface->list_models_async = ai_claude_client_list_models_async;
    iface->list_models_finish = ai_claude_client_list_models_finish;
}

/*
 * AiStreamable interface implementation
 *
 * Claude uses Server-Sent Events (SSE) for streaming. Event types:
 * - message_start: Initial message structure with id, model, usage
 * - content_block_start: Start of a content block (text or tool_use)
 * - content_block_delta: Text delta or tool input delta
 * - content_block_stop: End of a content block
 * - message_delta: Final updates (stop_reason, usage)
 * - message_stop: End of message
 */

typedef struct
{
    AiClaudeClient  *client;
    GTask           *task;
    SoupMessage     *msg;
    GInputStream    *input_stream;
    GDataInputStream *data_stream;
    GCancellable    *cancellable;

    /* Response being built */
    AiResponse      *response;
    GString         *current_text;
    gchar           *current_tool_id;
    gchar           *current_tool_name;
    GString         *current_tool_input;

    /* SSE parsing state */
    gchar           *current_event_type;
    GString         *current_event_data;

    /* State tracking */
    gboolean         stream_started;
    gboolean         in_text_block;
    gboolean         in_tool_block;
} StreamAsyncData;

static void
stream_async_data_free(StreamAsyncData *data)
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
    if (data->current_tool_input != NULL)
    {
        g_string_free(data->current_tool_input, TRUE);
    }
    if (data->current_event_data != NULL)
    {
        g_string_free(data->current_event_data, TRUE);
    }
    g_clear_pointer(&data->current_tool_id, g_free);
    g_clear_pointer(&data->current_tool_name, g_free);
    g_clear_pointer(&data->current_event_type, g_free);

    g_slice_free(StreamAsyncData, data);
}

/*
 * The response under construction.
 *
 * Created on demand rather than only by message_start: every branch of
 * process_stream_event() writes into it, and a stream whose
 * message_start is missing -- or whose `message` member is not an object,
 * which is what a type check turns from a critical into a NULL -- would
 * otherwise reach ai_response_get_usage(NULL) at message_stop.
 */
static AiResponse *
stream_response(StreamAsyncData *data)
{
    if (data->response == NULL)
    {
        data->response = ai_response_new("", "");
    }

    return data->response;
}

/*
 * Process a single SSE event from the stream.
 */
static void
process_stream_event(
    StreamAsyncData *data,
    const gchar     *event_type,
    const gchar     *event_data
){
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    g_autoptr(GError) error = NULL;

    if (event_data == NULL || event_data[0] == '\0')
    {
        return;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, event_data, -1, &error))
    {
        g_debug("Failed to parse SSE event: %s", error->message);
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

    if (g_strcmp0(event_type, "message_start") == 0)
    {
        /*
         * Extract message info and create the response object.  Not
         * conditional on `message` being there: this event is the start
         * of the response whatever it carries, and leaving it NULL is
         * what made a malformed one abort five branches later.
         */
        {
            JsonObject *msg_obj = ai_json_get_object(obj, "message");
            const gchar *id = ai_json_get_string(msg_obj, "id", "");
            const gchar *model = ai_json_get_string(msg_obj, "model", "");
            JsonObject *usage_obj = ai_json_get_object(msg_obj, "usage");

            g_clear_object(&data->response);
            data->response = ai_response_new(id, model);

            /* Parse initial usage if present */
            if (usage_obj != NULL)
            {
                gint input_tokens = (gint)ai_json_get_int(usage_obj, "input_tokens", 0);
                gint output_tokens = (gint)ai_json_get_int(usage_obj, "output_tokens", 0);
                g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

                ai_response_set_usage(data->response, usage);
            }
        }

        /* Emit stream-start signal */
        if (!data->stream_started)
        {
            g_autoptr(AiEvent) event = ai_event_new(AI_EVENT_STREAM_START);

            data->stream_started = TRUE;
            g_signal_emit_by_name(data->client, "stream-start");
            ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
        }
    }
    else if (g_strcmp0(event_type, "content_block_start") == 0)
    {
        /* Start of a new content block */
        {
            JsonObject *block_obj = ai_json_get_object(obj, "content_block");
            const gchar *type = ai_json_get_string(block_obj, "type", "");

            if (g_strcmp0(type, "text") == 0)
            {
                data->in_text_block = TRUE;
                data->current_text = g_string_new("");
            }
            else if (g_strcmp0(type, "tool_use") == 0)
            {
                g_autoptr(AiToolUse) starting = NULL;
                g_autoptr(AiEvent) event = NULL;

                data->in_tool_block = TRUE;
                data->current_tool_id =
                    g_strdup(ai_json_get_string(block_obj, "id", ""));
                data->current_tool_name =
                    g_strdup(ai_json_get_string(block_obj, "name", ""));
                data->current_tool_input = g_string_new("");

                /*
                 * Announce the call now, with an empty input: the arguments
                 * arrive as input_json_delta fragments and a frontend should
                 * not have to wait for the last one to say what is running.
                 * A second TOOL_STARTED follows at content_block_stop with
                 * the assembled input; consumers key on the id and update.
                 */
                starting = ai_tool_use_new(data->current_tool_id,
                                           data->current_tool_name, NULL);
                event = ai_event_new_tool_started(starting);
                ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
            }
        }
    }
    else if (g_strcmp0(event_type, "content_block_delta") == 0)
    {
        /* Delta update to a content block */
        {
            JsonObject *delta_obj = ai_json_get_object(obj, "delta");
            const gchar *type = ai_json_get_string(delta_obj, "type", "");

            if (g_strcmp0(type, "text_delta") == 0)
            {
                const gchar *text = ai_json_get_string(delta_obj, "text", "");

                if (data->current_text != NULL)
                {
                    g_string_append(data->current_text, text);
                }

                /* Emit delta signal */
                g_signal_emit_by_name(data->client, "delta", text);

                {
                    g_autoptr(AiEvent) event = ai_event_new_text_delta(text);
                    ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
                }
            }
            else if (g_strcmp0(type, "thinking_delta") == 0)
            {
                /*
                 * Extended thinking was previously not decoded at all, so a
                 * reasoning model appeared to stall between tool calls. It
                 * is reported under its own kind: not part of the answer,
                 * but worth showing.
                 */
                const gchar *thinking =
                    ai_json_get_string(delta_obj, "thinking", "");

                if (thinking[0] != '\0')
                {
                    g_autoptr(AiEvent) event =
                        ai_event_new_thinking_delta(thinking);
                    ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
                }
            }
            else if (g_strcmp0(type, "input_json_delta") == 0)
            {
                const gchar *partial =
                    ai_json_get_string(delta_obj, "partial_json", "");

                if (data->current_tool_input != NULL)
                {
                    g_string_append(data->current_tool_input, partial);
                }

                if (partial[0] != '\0')
                {
                    g_autoptr(AiEvent) event = ai_event_new_tool_input_delta(
                        data->current_tool_id, partial);
                    ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
                }
            }
        }
    }
    else if (g_strcmp0(event_type, "content_block_stop") == 0)
    {
        /* End of a content block - add it to the response */
        if (data->in_text_block && data->current_text != NULL)
        {
            g_autoptr(AiTextContent) content = ai_text_content_new(data->current_text->str);
            ai_response_add_content_block(stream_response(data), (AiContentBlock *)g_steal_pointer(&content));

            g_string_free(data->current_text, TRUE);
            data->current_text = NULL;
            data->in_text_block = FALSE;
        }
        else if (data->in_tool_block && data->current_tool_input != NULL)
        {
            g_autoptr(AiToolUse) tool_use = ai_tool_use_new_from_json_string(
                data->current_tool_id,
                data->current_tool_name,
                data->current_tool_input->str);
            /*
             * Emit before stealing, which is what the note left here for a
             * long time said would be needed and never did.
             */
            {
                g_autoptr(AiEvent) event = ai_event_new_tool_started(tool_use);

                g_signal_emit_by_name(data->client, "tool-use", tool_use);
                ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
            }

            ai_response_add_content_block(stream_response(data), (AiContentBlock *)g_steal_pointer(&tool_use));

            g_string_free(data->current_tool_input, TRUE);
            data->current_tool_input = NULL;
            g_clear_pointer(&data->current_tool_id, g_free);
            g_clear_pointer(&data->current_tool_name, g_free);
            data->in_tool_block = FALSE;
        }
    }
    else if (g_strcmp0(event_type, "message_delta") == 0)
    {
        /* Final message updates */
        {
            JsonObject *delta_obj = ai_json_get_object(obj, "delta");
            const gchar *stop_reason =
                ai_json_get_string(delta_obj, "stop_reason", "");

            if (stop_reason[0] != '\0')
            {
                ai_response_set_stop_reason(stream_response(data),
                                            ai_stop_reason_from_string(stop_reason));
            }
        }

        /* Update usage with final values */
        {
            JsonObject *usage_obj = ai_json_get_object(obj, "usage");

            if (usage_obj != NULL)
            {
                gint output_tokens = (gint)ai_json_get_int(usage_obj, "output_tokens", 0);

                /* Get existing usage to preserve input tokens */
                AiUsage *old_usage = ai_response_get_usage(stream_response(data));
                gint input_tokens = 0;

                if (old_usage != NULL)
                {
                    input_tokens = ai_usage_get_input_tokens(old_usage);
                }

                {
                    g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);
                    ai_response_set_usage(data->response, usage);
                }
            }
        }
    }
    else if (g_strcmp0(event_type, "message_stop") == 0)
    {
        /* End of message - emit stream-end signal */
        {
            AiUsage *usage = ai_response_get_usage(stream_response(data));

            if (usage != NULL)
            {
                g_autoptr(AiEvent) event = ai_event_new_usage(usage, -1);
                ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
            }
        }

        {
            g_autoptr(AiEvent) event = ai_event_new(AI_EVENT_STREAM_END);
            ai_event_source_emit(AI_EVENT_SOURCE(data->client), event);
        }

        g_signal_emit_by_name(data->client, "stream-end", data->response);
    }
}

static void read_next_line(StreamAsyncData *data);

static void
on_line_read(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    StreamAsyncData *data = user_data;
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
        stream_async_data_free(data);
        return;
    }

    if (line == NULL)
    {
        /* EOF - stream is complete */
        if (data->response != NULL)
        {
            g_task_return_pointer(data->task, g_object_ref(data->response), g_object_unref);
        }
        else
        {
            g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                    "Stream ended without a valid response");
        }

        stream_async_data_free(data);
        return;
    }

    /* Parse SSE format:
     * event: <event-type>
     * data: <json-data>
     * <blank line>
     */
    if (g_str_has_prefix(line, "event: "))
    {
        g_clear_pointer(&data->current_event_type, g_free);
        data->current_event_type = g_strdup(line + 7);
    }
    else if (g_str_has_prefix(line, "data: "))
    {
        if (data->current_event_data == NULL)
        {
            data->current_event_data = g_string_new(line + 6);
        }
        else
        {
            g_string_append(data->current_event_data, line + 6);
        }
    }
    else if (length == 0 || line[0] == '\0')
    {
        /* Empty line marks end of event */
        if (data->current_event_type != NULL && data->current_event_data != NULL)
        {
            process_stream_event(data, data->current_event_type, data->current_event_data->str);
        }

        g_clear_pointer(&data->current_event_type, g_free);
        if (data->current_event_data != NULL)
        {
            g_string_free(data->current_event_data, TRUE);
            data->current_event_data = NULL;
        }
    }

    /* Read next line */
    read_next_line(data);
}

static void
read_next_line(StreamAsyncData *data)
{
    g_data_input_stream_read_line_async(
        data->data_stream,
        G_PRIORITY_DEFAULT,
        data->cancellable,
        on_line_read,
        data);
}

static void
on_stream_ready(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    StreamAsyncData *data = user_data;
    g_autoptr(GError) error = NULL;

    data->input_stream = soup_session_send_finish(
        ai_client_get_soup_session(AI_CLIENT(data->client)),
        result,
        &error);

    if (data->input_stream == NULL)
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        stream_async_data_free(data);
        return;
    }

    /* Check HTTP status */
    if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(data->msg)))
    {
        guint status = soup_message_get_status(data->msg);

        {
            GError *http_error = NULL;

            ai_http_error_set_from_stream(&http_error, NULL, status,
                                          data->input_stream, NULL);
            g_task_return_error(data->task, g_steal_pointer(&http_error));
        }

        stream_async_data_free(data);
        return;
    }

    /* Wrap in a data input stream for line-by-line reading */
    data->data_stream = g_data_input_stream_new(data->input_stream);
    g_data_input_stream_set_newline_type(data->data_stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    /* Start reading lines */
    read_next_line(data);
}

/*
 * Build a streaming request (adds "stream": true to the request).
 */
static JsonNode *
ai_claude_client_build_stream_request(
    AiClient    *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    GList       *tools
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(client);
    g_autoptr(JsonBuilder) builder = json_builder_new();
    const gchar *model;
    GList *l;

    (void)self;

    model = ai_client_get_model(client);
    if (model == NULL)
    {
        model = AI_CLAUDE_DEFAULT_MODEL;
    }

    json_builder_begin_object(builder);

    /* Model */
    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    /* Max tokens */
    json_builder_set_member_name(builder, "max_tokens");
    json_builder_add_int_value(builder, max_tokens > 0 ? max_tokens : 4096);

    /* Enable streaming */
    json_builder_set_member_name(builder, "stream");
    json_builder_add_boolean_value(builder, TRUE);

    /* System prompt */
    if (system_prompt != NULL && system_prompt[0] != '\0')
    {
        json_builder_set_member_name(builder, "system");
        json_builder_add_string_value(builder, system_prompt);
    }

    /* Messages */
    json_builder_set_member_name(builder, "messages");
    json_builder_begin_array(builder);

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
            g_autoptr(JsonNode) tool_node = ai_tool_to_json(tool, AI_PROVIDER_CLAUDE);

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

static void
ai_claude_client_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiClaudeClient *self = AI_CLAUDE_CLIENT(streamable);
    AiClientClass *klass = AI_CLIENT_GET_CLASS(self);
    g_autoptr(JsonNode) request_json = NULL;
    g_autoptr(SoupMessage) msg = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *request_body = NULL;
    gsize request_len;
    StreamAsyncData *data;
    GTask *task;

    task = g_task_new(self, cancellable, callback, user_data);

    /* Build streaming request */
    request_json = ai_claude_client_build_stream_request(
        AI_CLIENT(self), messages, system_prompt, max_tokens, tools);

    if (request_json == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build request");
        g_object_unref(task);
        return;
    }

    /* Serialize to JSON string */
    {
        g_autoptr(JsonGenerator) gen = json_generator_new();
        json_generator_set_root(gen, request_json);
        request_body = json_generator_to_data(gen, &request_len);
    }

    /* Get endpoint URL */
    url = klass->get_endpoint_url(AI_CLIENT(self));

    /* Create HTTP request */
    msg = soup_message_new("POST", url);
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

    /* Set up callback data */
    data = g_slice_new0(StreamAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->msg = g_object_ref(msg);
    data->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;
    data->stream_started = FALSE;
    data->in_text_block = FALSE;
    data->in_tool_block = FALSE;

    /* Send request - use send_async to get the input stream */
    soup_session_send_async(
        ai_client_get_soup_session(AI_CLIENT(self)),
        msg,
        G_PRIORITY_DEFAULT,
        cancellable,
        on_stream_ready,
        data);
}

static AiResponse *
ai_claude_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    (void)streamable;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_claude_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_claude_client_chat_stream_async;
    iface->chat_stream_finish = ai_claude_client_chat_stream_finish;
}

/*
 * Public API
 */

/**
 * ai_claude_client_new:
 *
 * Creates a new #AiClaudeClient using the default configuration.
 * The API key will be read from the ANTHROPIC_API_KEY environment variable.
 *
 * Returns: (transfer full): a new #AiClaudeClient
 */
AiClaudeClient *
ai_claude_client_new(void)
{
    g_autoptr(AiClaudeClient) self = g_object_new(AI_TYPE_CLAUDE_CLIENT, NULL);

    return (AiClaudeClient *)g_steal_pointer(&self);
}

/**
 * ai_claude_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiClaudeClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiClaudeClient
 */
AiClaudeClient *
ai_claude_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiClaudeClient) self = g_object_new(AI_TYPE_CLAUDE_CLIENT,
                                                   "config", config,
                                                   NULL);

    return (AiClaudeClient *)g_steal_pointer(&self);
}

/**
 * ai_claude_client_new_with_key:
 * @api_key: the Anthropic API key
 *
 * Creates a new #AiClaudeClient with the specified API key.
 *
 * Returns: (transfer full): a new #AiClaudeClient
 */
AiClaudeClient *
ai_claude_client_new_with_key(const gchar *api_key)
{
    g_autoptr(AiConfig) config = ai_config_new();

    ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, api_key);

    return ai_claude_client_new_with_config(config);
}

/**
 * ai_claude_client_get_api_version:
 * @self: an #AiClaudeClient
 *
 * Gets the Anthropic API version being used.
 *
 * Returns: (transfer none): the API version string
 */
const gchar *
ai_claude_client_get_api_version(AiClaudeClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_CLIENT(self), NULL);

    return self->api_version;
}

/**
 * ai_claude_client_set_api_version:
 * @self: an #AiClaudeClient
 * @version: the API version string
 *
 * Sets the Anthropic API version to use.
 */
void
ai_claude_client_set_api_version(
    AiClaudeClient *self,
    const gchar    *version
){
    g_return_if_fail(AI_IS_CLAUDE_CLIENT(self));

    g_clear_pointer(&self->api_version, g_free);
    self->api_version = g_strdup(version);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_API_VERSION]);
}
