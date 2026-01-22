/*
 * ai-claude-code-client.c - Claude Code CLI client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-claude-code-client.h"
#include "core/ai-error.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

/*
 * Private structure for AiClaudeCodeClient.
 */
struct _AiClaudeCodeClient
{
    AiCliClient parent_instance;

    gdouble total_cost;
};

/*
 * Interface implementations forward declarations.
 */
static void ai_claude_code_client_provider_init(AiProviderInterface *iface);
static void ai_claude_code_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiClaudeCodeClient, ai_claude_code_client, AI_TYPE_CLI_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_claude_code_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_claude_code_client_streamable_init))

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_TOTAL_COST,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_claude_code_client_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(object);

    switch (prop_id)
    {
        case PROP_TOTAL_COST:
            g_value_set_double(value, self->total_cost);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * Get the executable path for the claude CLI.
 * Checks CLAUDE_CODE_PATH environment variable first, then falls back to "claude".
 */
static gchar *
ai_claude_code_client_get_executable_path(AiCliClient *client)
{
    const gchar *env_path;

    (void)client;

    /* Check environment variable override */
    env_path = g_getenv("CLAUDE_CODE_PATH");
    if (env_path != NULL && env_path[0] != '\0')
    {
        return g_strdup(env_path);
    }

    /* Fall back to searching PATH */
    return g_strdup("claude");
}

/*
 * Build command line arguments for the claude CLI.
 *
 * Non-streaming: claude --print --output-format json --model <model> --system-prompt "..." "prompt"
 * Streaming: claude --print --output-format stream-json --verbose --model <model> "prompt"
 */
static gchar **
ai_claude_code_client_build_argv(
    AiCliClient *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    gboolean     streaming
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(client);
    GPtrArray *args;
    const gchar *model;
    const gchar *session_id;
    gboolean persist;
    GString *prompt;
    GList *l;

    (void)self;
    (void)max_tokens;  /* Claude Code CLI doesn't have a max tokens flag */

    args = g_ptr_array_new();

    /* Executable (will be replaced with resolved path) */
    g_ptr_array_add(args, g_strdup("claude"));

    /* Print mode (required for non-interactive use) */
    g_ptr_array_add(args, g_strdup("--print"));

    /* Output format */
    if (streaming)
    {
        g_ptr_array_add(args, g_strdup("--output-format"));
        g_ptr_array_add(args, g_strdup("stream-json"));
        g_ptr_array_add(args, g_strdup("--verbose"));
    }
    else
    {
        g_ptr_array_add(args, g_strdup("--output-format"));
        g_ptr_array_add(args, g_strdup("json"));
    }

    /* Model */
    model = ai_cli_client_get_model(client);
    if (model == NULL)
    {
        model = AI_CLAUDE_CODE_DEFAULT_MODEL;
    }
    g_ptr_array_add(args, g_strdup("--model"));
    g_ptr_array_add(args, g_strdup(model));

    /* System prompt */
    if (system_prompt != NULL && system_prompt[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--system-prompt"));
        g_ptr_array_add(args, g_strdup(system_prompt));
    }

    /* Session management - only resume if persistence is enabled */
    persist = ai_cli_client_get_session_persistence(client);
    session_id = ai_cli_client_get_session_id(client);
    if (persist && session_id != NULL && session_id[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--resume"));
        g_ptr_array_add(args, g_strdup(session_id));
    }
    if (!persist)
    {
        g_ptr_array_add(args, g_strdup("--no-session-persistence"));
    }

    /*
     * Build the prompt from messages.
     * For multi-turn conversations, we concatenate them into a single prompt
     * since the CLI handles its own message history via sessions.
     */
    prompt = g_string_new("");
    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        g_autofree gchar *text = ai_message_get_text(msg);
        AiRole role = ai_message_get_role(msg);

        if (text != NULL && text[0] != '\0')
        {
            if (prompt->len > 0)
            {
                g_string_append(prompt, "\n\n");
            }

            /* Add role prefix for multi-message conversations */
            if (role == AI_ROLE_USER)
            {
                g_string_append(prompt, text);
            }
            else if (role == AI_ROLE_ASSISTANT)
            {
                g_string_append_printf(prompt, "Previous assistant response: %s", text);
            }
        }
    }

    g_ptr_array_add(args, g_string_free(prompt, FALSE));

    /* NULL terminate */
    g_ptr_array_add(args, NULL);

    return (gchar **)g_ptr_array_free(args, FALSE);
}

/*
 * Parse JSON output from the claude CLI.
 *
 * Expected format:
 * {
 *     "type": "result",
 *     "result": "response text",
 *     "session_id": "uuid",
 *     "usage": {"input_tokens": N, "output_tokens": N},
 *     "total_cost_usd": 0.001
 * }
 */
static AiResponse *
ai_claude_code_client_parse_json_output(
    AiCliClient *client,
    const gchar *json,
    GError     **error
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(client);
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    const gchar *type;
    const gchar *result_text;
    const gchar *session_id;
    g_autoptr(AiResponse) response = NULL;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, error))
    {
        return NULL;
    }

    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                    "Expected JSON object in CLI output");
        return NULL;
    }

    obj = json_node_get_object(root);

    /* Check type */
    type = json_object_get_string_member_with_default(obj, "type", "");
    if (g_strcmp0(type, "result") != 0)
    {
        /* Check for error */
        if (json_object_has_member(obj, "error"))
        {
            const gchar *err_msg = json_object_get_string_member_with_default(
                obj, "error", "Unknown error");
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                        "CLI error: %s", err_msg);
        }
        else
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                        "Unexpected response type: %s", type);
        }
        return NULL;
    }

    /* Create response */
    session_id = json_object_get_string_member_with_default(obj, "session_id", "");
    response = ai_response_new(session_id, ai_cli_client_get_model(client));
    ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);

    /* Store session ID for continuity - ONLY if persistence is enabled */
    if (session_id[0] != '\0' && ai_cli_client_get_session_persistence(client))
    {
        ai_cli_client_set_session_id(client, session_id);
    }

    /* Parse result text */
    result_text = json_object_get_string_member_with_default(obj, "result", "");
    if (result_text[0] != '\0')
    {
        g_autoptr(AiTextContent) content = ai_text_content_new(result_text);
        ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&content));
    }

    /* Parse usage */
    if (json_object_has_member(obj, "usage"))
    {
        JsonObject *usage_obj = json_object_get_object_member(obj, "usage");
        gint input_tokens = json_object_get_int_member_with_default(usage_obj, "input_tokens", 0);
        gint output_tokens = json_object_get_int_member_with_default(usage_obj, "output_tokens", 0);
        g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

        ai_response_set_usage(response, usage);
    }

    /* Store total cost */
    if (json_object_has_member(obj, "total_cost_usd"))
    {
        self->total_cost = json_object_get_double_member(obj, "total_cost_usd");
    }

    return (AiResponse *)g_steal_pointer(&response);
}

/*
 * Parse a single NDJSON line from streaming output.
 *
 * Streaming events:
 * {"type": "assistant", "message": {"type": "text", "text": "..."}} -> emit delta
 * {"type": "result", ...} -> final usage/session info
 */
static gboolean
ai_claude_code_client_parse_stream_line(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    gchar       **delta_text,
    GError      **error
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(client);
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    const gchar *type;

    *delta_text = NULL;

    if (line == NULL || line[0] == '\0')
    {
        return TRUE;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, error))
    {
        /* Non-JSON lines can be ignored (e.g., blank lines) */
        g_clear_error(error);
        return TRUE;
    }

    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        return TRUE;
    }

    obj = json_node_get_object(root);
    type = json_object_get_string_member_with_default(obj, "type", "");

    if (g_strcmp0(type, "assistant") == 0)
    {
        /* Text delta */
        if (json_object_has_member(obj, "message"))
        {
            JsonObject *msg_obj = json_object_get_object_member(obj, "message");
            const gchar *msg_type = json_object_get_string_member_with_default(msg_obj, "type", "");

            if (g_strcmp0(msg_type, "text") == 0)
            {
                const gchar *text = json_object_get_string_member_with_default(msg_obj, "text", "");
                *delta_text = g_strdup(text);
            }
        }
    }
    else if (g_strcmp0(type, "result") == 0)
    {
        /* Final result with usage info */
        const gchar *result_text = json_object_get_string_member_with_default(obj, "result", "");
        const gchar *session_id = json_object_get_string_member_with_default(obj, "session_id", "");

        /* Store session ID - ONLY if persistence is enabled */
        if (session_id[0] != '\0' && ai_cli_client_get_session_persistence(client))
        {
            ai_cli_client_set_session_id(client, session_id);
        }

        /* Add final text content to response if not already added via deltas */
        if (result_text[0] != '\0' && ai_response_get_content_blocks(response) == NULL)
        {
            g_autoptr(AiTextContent) content = ai_text_content_new(result_text);
            ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&content));
        }

        /* Update usage */
        if (json_object_has_member(obj, "usage"))
        {
            JsonObject *usage_obj = json_object_get_object_member(obj, "usage");
            gint input_tokens = json_object_get_int_member_with_default(usage_obj, "input_tokens", 0);
            gint output_tokens = json_object_get_int_member_with_default(usage_obj, "output_tokens", 0);
            g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

            ai_response_set_usage(response, usage);
        }

        /* Store total cost */
        if (json_object_has_member(obj, "total_cost_usd"))
        {
            self->total_cost = json_object_get_double_member(obj, "total_cost_usd");
        }

        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }

    return TRUE;
}

static void
ai_claude_code_client_class_init(AiClaudeCodeClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

    object_class->get_property = ai_claude_code_client_get_property;

    /* Override virtual methods */
    cli_class->get_executable_path = ai_claude_code_client_get_executable_path;
    cli_class->build_argv = ai_claude_code_client_build_argv;
    cli_class->parse_json_output = ai_claude_code_client_parse_json_output;
    cli_class->parse_stream_line = ai_claude_code_client_parse_stream_line;

    /**
     * AiClaudeCodeClient:total-cost:
     *
     * The total cost in USD from the last response.
     */
    properties[PROP_TOTAL_COST] =
        g_param_spec_double("total-cost",
                            "Total Cost",
                            "The total cost in USD from the last response",
                            0.0, G_MAXDOUBLE, 0.0,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_claude_code_client_init(AiClaudeCodeClient *self)
{
    self->total_cost = 0.0;

    /* Set default model */
    ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_CLAUDE_CODE_DEFAULT_MODEL);
}

/*
 * AiProvider interface implementation
 */

static AiProviderType
ai_claude_code_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_CLAUDE_CODE;
}

static const gchar *
ai_claude_code_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "Claude Code";
}

static const gchar *
ai_claude_code_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_CLAUDE_CODE_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiClaudeCodeClient *client;
    GTask              *task;
    GSubprocess        *subprocess;
} ChatAsyncData;

static void
chat_async_data_free(ChatAsyncData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->subprocess);
    g_slice_free(ChatAsyncData, data);
}

static void
on_chat_communicate_complete(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    ChatAsyncData *data = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *stdout_data = NULL;
    g_autofree gchar *stderr_data = NULL;
    AiCliClientClass *klass;
    AiResponse *response;

    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                               &stdout_data, &stderr_data, &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        chat_async_data_free(data);
        return;
    }

    /* Check exit status */
    if (!g_subprocess_get_successful(data->subprocess))
    {
        gint exit_status = g_subprocess_get_exit_status(data->subprocess);
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                                "CLI exited with status %d: %s",
                                exit_status,
                                stderr_data != NULL ? stderr_data : "Unknown error");
        chat_async_data_free(data);
        return;
    }

    /* Parse output */
    if (stdout_data == NULL || stdout_data[0] == '\0')
    {
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                                "CLI produced no output");
        chat_async_data_free(data);
        return;
    }

    klass = AI_CLI_CLIENT_GET_CLASS(data->client);
    response = klass->parse_json_output(AI_CLI_CLIENT(data->client), stdout_data, &error);

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
ai_claude_code_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(provider);
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(self);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *executable = NULL;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GSubprocess) subprocess = NULL;
    ChatAsyncData *data;
    GTask *task;

    (void)tools;  /* Tools not yet supported via CLI */

    task = g_task_new(self, cancellable, callback, user_data);

    /* Resolve executable path */
    executable = ai_cli_client_resolve_executable(AI_CLI_CLIENT(self), &error);
    if (executable == NULL)
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    /* Build command line arguments */
    argv = klass->build_argv(AI_CLI_CLIENT(self), messages, system_prompt,
                             max_tokens, FALSE);
    if (argv == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build command line arguments");
        g_object_unref(task);
        return;
    }

    /* Replace first element with resolved executable path */
    g_free(argv[0]);
    argv[0] = g_steal_pointer(&executable);

    /* Spawn subprocess */
    subprocess = g_subprocess_newv((const gchar * const *)argv,
                                   G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                   G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                   &error);
    if (subprocess == NULL)
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    /* Set up callback data */
    data = g_slice_new0(ChatAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->subprocess = g_object_ref(subprocess);

    /* Start async communication */
    g_subprocess_communicate_utf8_async(subprocess, NULL, cancellable,
                                        on_chat_communicate_complete, data);
}

static AiResponse *
ai_claude_code_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_claude_code_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    /* Return static list of model aliases */
    task = g_task_new(provider, NULL, callback, user_data);

    models = g_list_append(models, g_strdup("opus"));
    models = g_list_append(models, g_strdup("sonnet"));
    models = g_list_append(models, g_strdup("haiku"));

    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
}

static GList *
ai_claude_code_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_claude_code_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_claude_code_client_get_provider_type;
    iface->get_name = ai_claude_code_client_get_name;
    iface->get_default_model = ai_claude_code_client_get_default_model;
    iface->chat_async = ai_claude_code_client_chat_async;
    iface->chat_finish = ai_claude_code_client_chat_finish;
    iface->list_models_async = ai_claude_code_client_list_models_async;
    iface->list_models_finish = ai_claude_code_client_list_models_finish;
}

/*
 * AiStreamable interface implementation
 */

typedef struct
{
    AiClaudeCodeClient *client;
    GTask              *task;
    GSubprocess        *subprocess;
    GDataInputStream   *data_stream;
    GCancellable       *cancellable;
    AiResponse         *response;
    GString            *accumulated_text;
    gboolean            stream_started;
} StreamAsyncData;

static void
stream_async_data_free(StreamAsyncData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->subprocess);
    g_clear_object(&data->data_stream);
    g_clear_object(&data->cancellable);
    g_clear_object(&data->response);

    if (data->accumulated_text != NULL)
    {
        g_string_free(data->accumulated_text, TRUE);
    }

    g_slice_free(StreamAsyncData, data);
}

static void read_next_stream_line(StreamAsyncData *data);

static void
on_stream_line_read(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    StreamAsyncData *data = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *delta_text = NULL;
    AiCliClientClass *klass;
    gsize length;

    (void)source;

    line = g_data_input_stream_read_line_finish(data->data_stream, result, &length, &error);

    if (error != NULL)
    {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
            g_task_return_error(data->task, g_steal_pointer(&error));
            stream_async_data_free(data);
        }
        return;
    }

    if (line == NULL)
    {
        /* EOF - stream is complete */
        if (data->response != NULL)
        {
            /* Add accumulated text as content block if not already done */
            if (data->accumulated_text != NULL && data->accumulated_text->len > 0 &&
                ai_response_get_content_blocks(data->response) == NULL)
            {
                g_autoptr(AiTextContent) content = ai_text_content_new(data->accumulated_text->str);
                ai_response_add_content_block(data->response, (AiContentBlock *)g_steal_pointer(&content));
            }

            g_signal_emit_by_name(data->client, "stream-end", data->response);
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

    /* Parse the line */
    klass = AI_CLI_CLIENT_GET_CLASS(data->client);
    if (klass->parse_stream_line(AI_CLI_CLIENT(data->client), line, data->response,
                                  &delta_text, &error))
    {
        if (delta_text != NULL && delta_text[0] != '\0')
        {
            /* Emit stream-start on first delta */
            if (!data->stream_started)
            {
                data->stream_started = TRUE;
                g_signal_emit_by_name(data->client, "stream-start");
            }

            /* Accumulate text */
            g_string_append(data->accumulated_text, delta_text);

            /* Emit delta signal */
            g_signal_emit_by_name(data->client, "delta", delta_text);
        }
    }

    /* Read next line */
    read_next_stream_line(data);
}

static void
read_next_stream_line(StreamAsyncData *data)
{
    g_data_input_stream_read_line_async(
        data->data_stream,
        G_PRIORITY_DEFAULT,
        data->cancellable,
        on_stream_line_read,
        data);
}

static void
on_stream_subprocess_started(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    StreamAsyncData *data = user_data;
    g_autoptr(GError) error = NULL;
    GInputStream *stdout_stream;

    (void)source;
    (void)result;

    /* Get stdout pipe */
    stdout_stream = g_subprocess_get_stdout_pipe(data->subprocess);
    if (stdout_stream == NULL)
    {
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                                "Failed to get subprocess stdout");
        stream_async_data_free(data);
        return;
    }

    /* Wrap in data input stream for line-by-line reading */
    data->data_stream = g_data_input_stream_new(stdout_stream);
    g_data_input_stream_set_newline_type(data->data_stream, G_DATA_STREAM_NEWLINE_TYPE_ANY);

    /* Create response object */
    data->response = ai_response_new("", ai_cli_client_get_model(AI_CLI_CLIENT(data->client)));
    data->accumulated_text = g_string_new("");

    /* Start reading lines */
    read_next_stream_line(data);
}

static void
ai_claude_code_client_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(streamable);
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(self);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *executable = NULL;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GSubprocess) subprocess = NULL;
    StreamAsyncData *data;
    GTask *task;

    (void)tools;  /* Tools not yet supported via CLI */

    task = g_task_new(self, cancellable, callback, user_data);

    /* Resolve executable path */
    executable = ai_cli_client_resolve_executable(AI_CLI_CLIENT(self), &error);
    if (executable == NULL)
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    /* Build command line arguments for streaming */
    argv = klass->build_argv(AI_CLI_CLIENT(self), messages, system_prompt,
                             max_tokens, TRUE);
    if (argv == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build command line arguments");
        g_object_unref(task);
        return;
    }

    /* Replace first element with resolved executable path */
    g_free(argv[0]);
    argv[0] = g_steal_pointer(&executable);

    /* Spawn subprocess */
    subprocess = g_subprocess_newv((const gchar * const *)argv,
                                   G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                   G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                   &error);
    if (subprocess == NULL)
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    /* Set up callback data */
    data = g_slice_new0(StreamAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->subprocess = g_object_ref(subprocess);
    data->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;
    data->stream_started = FALSE;

    /* Use idle to start reading (subprocess is already running) */
    on_stream_subprocess_started(NULL, NULL, data);
}

static AiResponse *
ai_claude_code_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    (void)streamable;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_claude_code_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_claude_code_client_chat_stream_async;
    iface->chat_stream_finish = ai_claude_code_client_chat_stream_finish;
}

/*
 * Public API
 */

/**
 * ai_claude_code_client_new:
 *
 * Creates a new #AiClaudeCodeClient.
 * The claude CLI must be available in PATH or specified via
 * %CLAUDE_CODE_PATH environment variable.
 *
 * Returns: (transfer full): a new #AiClaudeCodeClient
 */
AiClaudeCodeClient *
ai_claude_code_client_new(void)
{
    g_autoptr(AiClaudeCodeClient) self = g_object_new(AI_TYPE_CLAUDE_CODE_CLIENT, NULL);

    return (AiClaudeCodeClient *)g_steal_pointer(&self);
}

/**
 * ai_claude_code_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiClaudeCodeClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiClaudeCodeClient
 */
AiClaudeCodeClient *
ai_claude_code_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiClaudeCodeClient) self = g_object_new(AI_TYPE_CLAUDE_CODE_CLIENT,
                                                       "config", config,
                                                       NULL);

    return (AiClaudeCodeClient *)g_steal_pointer(&self);
}

/**
 * ai_claude_code_client_get_total_cost:
 * @self: an #AiClaudeCodeClient
 *
 * Gets the total cost in USD from the last response.
 *
 * Returns: the total cost in USD, or 0.0 if not available
 */
gdouble
ai_claude_code_client_get_total_cost(AiClaudeCodeClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_CODE_CLIENT(self), 0.0);

    return self->total_cost;
}
