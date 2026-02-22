/*
 * ai-opencode-client.c - OpenCode CLI client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-opencode-client.h"
#include "core/ai-error.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

/*
 * Private structure for AiOpenCodeClient.
 */
struct _AiOpenCodeClient
{
    AiCliClient parent_instance;
};

/*
 * Interface implementations forward declarations.
 */
static void ai_opencode_client_provider_init(AiProviderInterface *iface);
static void ai_opencode_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiOpenCodeClient, ai_opencode_client, AI_TYPE_CLI_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_opencode_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_opencode_client_streamable_init))

/*
 * Get the executable path for the opencode CLI.
 * Checks OPENCODE_PATH environment variable first, then falls back to "opencode".
 */
static gchar *
ai_opencode_client_get_executable_path(AiCliClient *client)
{
    const gchar *env_path;

    (void)client;

    /* Check environment variable override */
    env_path = g_getenv("OPENCODE_PATH");
    if (env_path != NULL && env_path[0] != '\0')
    {
        return g_strdup(env_path);
    }

    /* Fall back to searching PATH */
    return g_strdup("opencode");
}

/*
 * Build command line arguments for the opencode CLI.
 *
 * Command: opencode run --format json --model <model>
 * Prompt is piped via stdin (build_stdin) to avoid ARG_MAX limits.
 */
static gchar **
ai_opencode_client_build_argv(
    AiCliClient *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    gboolean     streaming
){
    GPtrArray *args;
    const gchar *model;
    const gchar *session_id;

    (void)messages;       /* prompt goes via stdin */
    (void)max_tokens;     /* opencode has no max tokens flag */
    (void)system_prompt;  /* handled in build_stdin */

    args = g_ptr_array_new();

    /* Executable (will be replaced with resolved path) */
    g_ptr_array_add(args, g_strdup("opencode"));

    /* Run command */
    g_ptr_array_add(args, g_strdup("run"));

    /* Output format — opencode only supports "json", not "stream-json" */
    g_ptr_array_add(args, g_strdup("--format"));
    g_ptr_array_add(args, g_strdup("json"));

    (void)streaming;

    /* Model */
    model = ai_cli_client_get_model(client);
    if (model == NULL)
        model = AI_OPENCODE_DEFAULT_MODEL;
    g_ptr_array_add(args, g_strdup("--model"));
    g_ptr_array_add(args, g_strdup(model));

    /* Session continuation */
    session_id = ai_cli_client_get_session_id(client);
    if (session_id != NULL && session_id[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--session"));
        g_ptr_array_add(args, g_strdup(session_id));
    }

    /* NULL terminate — no positional prompt, stdin is used */
    g_ptr_array_add(args, NULL);

    return (gchar **)g_ptr_array_free(args, FALSE);
}

/*
 * Build the prompt string to pipe via stdin to the opencode CLI.
 * opencode reads from stdin when no positional prompt argument is given.
 * System prompt is prepended in <system> tags since opencode has no
 * --system-prompt flag.
 */
static gchar *
ai_opencode_client_build_stdin(
    AiCliClient *client,
    GList       *messages
){
    GString *prompt;
    const gchar *sys_prompt;
    GList *l;

    prompt = g_string_new("");

    /* Prepend system prompt if set */
    sys_prompt = ai_cli_client_get_system_prompt(client);
    if (sys_prompt != NULL && sys_prompt[0] != '\0')
    {
        g_string_append(prompt, "<system>\n");
        g_string_append(prompt, sys_prompt);
        g_string_append(prompt, "\n</system>\n\n");
    }

    /* Concatenate user messages */
    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        g_autofree gchar *text = ai_message_get_text(msg);
        AiRole role = ai_message_get_role(msg);

        if (text != NULL && text[0] != '\0')
        {
            if (role == AI_ROLE_USER)
            {
                g_string_append(prompt, text);
            }
            else if (role == AI_ROLE_ASSISTANT)
            {
                g_string_append_printf(prompt,
                    "\n\nPrevious assistant response: %s", text);
            }
        }
    }

    return g_string_free(prompt, FALSE);
}

/*
 * Parse JSON output from the opencode CLI.
 *
 * OpenCode returns NDJSON (newline-delimited JSON) with events:
 * {"type":"step_start",...}
 * {"type":"text","part":{"text":"response text",...}}
 * {"type":"step_finish","part":{"tokens":{"input":N,"output":N},...}}
 */
static AiResponse *
ai_opencode_client_parse_json_output(
    AiCliClient *client,
    const gchar *json_output,
    GError     **error
){
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GString) accumulated_text = NULL;
    gchar **lines;
    gint i;
    gint input_tokens = 0;
    gint output_tokens = 0;

    /* Create response */
    response = ai_response_new("", ai_cli_client_get_model(client));
    ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    accumulated_text = g_string_new("");

    /* Split into lines and parse each */
    lines = g_strsplit(json_output, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        g_autoptr(JsonParser) parser = NULL;
        JsonNode *root;
        JsonObject *obj;
        const gchar *type;
        const gchar *line = lines[i];

        /* Skip empty lines */
        if (line[0] == '\0')
        {
            continue;
        }

        parser = json_parser_new();
        if (!json_parser_load_from_data(parser, line, -1, NULL))
        {
            /* Skip unparseable lines */
            continue;
        }

        root = json_parser_get_root(parser);
        if (!JSON_NODE_HOLDS_OBJECT(root))
        {
            continue;
        }

        obj = json_node_get_object(root);

        /* Check for error */
        if (json_object_has_member(obj, "error"))
        {
            const gchar *err_msg = json_object_get_string_member_with_default(
                obj, "error", "Unknown error");
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                        "CLI error: %s", err_msg);
            g_strfreev(lines);
            return NULL;
        }

        /* Capture sessionID for session persistence */
        if (json_object_has_member(obj, "sessionID"))
        {
            const gchar *sid = json_object_get_string_member_with_default(
                obj, "sessionID", "");
            if (sid[0] != '\0' && ai_cli_client_get_session_persistence(client))
            {
                ai_cli_client_set_session_id(client, sid);
            }
        }

        type = json_object_get_string_member_with_default(obj, "type", "");

        if (g_strcmp0(type, "text") == 0)
        {
            /* Extract text from part.text */
            if (json_object_has_member(obj, "part"))
            {
                JsonObject *part = json_object_get_object_member(obj, "part");
                if (part != NULL && json_object_has_member(part, "text"))
                {
                    const gchar *text = json_object_get_string_member_with_default(
                        part, "text", "");
                    g_string_append(accumulated_text, text);
                }
            }
        }
        else if (g_strcmp0(type, "step_finish") == 0)
        {
            /* Extract usage from part.tokens */
            if (json_object_has_member(obj, "part"))
            {
                JsonObject *part = json_object_get_object_member(obj, "part");
                if (part != NULL && json_object_has_member(part, "tokens"))
                {
                    JsonObject *tokens = json_object_get_object_member(part, "tokens");
                    if (tokens != NULL)
                    {
                        input_tokens = json_object_get_int_member_with_default(
                            tokens, "input", 0);
                        output_tokens = json_object_get_int_member_with_default(
                            tokens, "output", 0);
                    }
                }
            }
        }
    }

    g_strfreev(lines);

    /* Add accumulated text as content */
    if (accumulated_text->len > 0)
    {
        g_autoptr(AiTextContent) text_content = ai_text_content_new(accumulated_text->str);
        ai_response_add_content_block(response, (AiContentBlock *)g_steal_pointer(&text_content));
    }

    /* Set usage if we got tokens */
    if (input_tokens > 0 || output_tokens > 0)
    {
        g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);
        ai_response_set_usage(response, usage);
    }

    return (AiResponse *)g_steal_pointer(&response);
}

/*
 * Parse a single NDJSON line from streaming output.
 *
 * Streaming events:
 * {"type":"text","part":{"text":"..."}} -> emit delta
 * {"type":"step_finish","part":{"tokens":{"input":N,"output":N}}} -> final usage
 */
static gboolean
ai_opencode_client_parse_stream_line(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    gchar       **delta_text,
    GError      **error
){
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    const gchar *type;

    (void)client;
    *delta_text = NULL;

    if (line == NULL || line[0] == '\0')
    {
        return TRUE;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, error))
    {
        /* Non-JSON lines can be ignored */
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

    if (g_strcmp0(type, "text") == 0)
    {
        /* Text delta - extract from part.text */
        if (json_object_has_member(obj, "part"))
        {
            JsonObject *part = json_object_get_object_member(obj, "part");
            if (part != NULL && json_object_has_member(part, "text"))
            {
                const gchar *text = json_object_get_string_member_with_default(
                    part, "text", "");
                if (text[0] != '\0')
                {
                    *delta_text = g_strdup(text);
                }
            }
        }
    }
    else if (g_strcmp0(type, "step_finish") == 0)
    {
        /* Final result with usage info - extract from part.tokens */
        if (json_object_has_member(obj, "part"))
        {
            JsonObject *part = json_object_get_object_member(obj, "part");
            if (part != NULL && json_object_has_member(part, "tokens"))
            {
                JsonObject *tokens = json_object_get_object_member(part, "tokens");
                if (tokens != NULL)
                {
                    gint input_tokens = json_object_get_int_member_with_default(
                        tokens, "input", 0);
                    gint output_tokens = json_object_get_int_member_with_default(
                        tokens, "output", 0);
                    g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

                    ai_response_set_usage(response, usage);
                }
            }
        }

        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }

    return TRUE;
}

static void
ai_opencode_client_class_init(AiOpenCodeClientClass *klass)
{
    AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

    /* Override virtual methods */
    cli_class->get_executable_path = ai_opencode_client_get_executable_path;
    cli_class->build_argv = ai_opencode_client_build_argv;
    cli_class->build_stdin = ai_opencode_client_build_stdin;
    cli_class->parse_json_output = ai_opencode_client_parse_json_output;
    cli_class->parse_stream_line = ai_opencode_client_parse_stream_line;
}

static void
ai_opencode_client_init(AiOpenCodeClient *self)
{
    (void)self;

    /* Set default model */
    ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_OPENCODE_DEFAULT_MODEL);
}

/*
 * AiProvider interface implementation
 */

static AiProviderType
ai_opencode_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_OPENCODE;
}

static const gchar *
ai_opencode_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "OpenCode";
}

static const gchar *
ai_opencode_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_OPENCODE_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiOpenCodeClient *client;
    GTask            *task;
    GSubprocess      *subprocess;
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
ai_opencode_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOpenCodeClient *self = AI_OPENCODE_CLIENT(provider);
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
ai_opencode_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_opencode_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    /* Return static list of popular models */
    task = g_task_new(provider, NULL, callback, user_data);

    /* Anthropic models */
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_SONNET_4));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_OPUS_4));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_OPUS_4_5));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_HAIKU));

    /* OpenAI models */
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_GPT_4O));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_O3));

    /* Google models */
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_GEMINI_2_FLASH));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_GEMINI_2_5_PRO));

    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
}

static GList *
ai_opencode_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_opencode_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_opencode_client_get_provider_type;
    iface->get_name = ai_opencode_client_get_name;
    iface->get_default_model = ai_opencode_client_get_default_model;
    iface->chat_async = ai_opencode_client_chat_async;
    iface->chat_finish = ai_opencode_client_chat_finish;
    iface->list_models_async = ai_opencode_client_list_models_async;
    iface->list_models_finish = ai_opencode_client_list_models_finish;
}

/*
 * AiStreamable interface implementation
 */

typedef struct
{
    AiOpenCodeClient *client;
    GTask            *task;
    GSubprocess      *subprocess;
    GDataInputStream *data_stream;
    GCancellable     *cancellable;
    AiResponse       *response;
    GString          *accumulated_text;
    gboolean          stream_started;
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
ai_opencode_client_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiOpenCodeClient *self = AI_OPENCODE_CLIENT(streamable);
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
ai_opencode_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    (void)streamable;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_opencode_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_opencode_client_chat_stream_async;
    iface->chat_stream_finish = ai_opencode_client_chat_stream_finish;
}

/*
 * Public API
 */

/**
 * ai_opencode_client_new:
 *
 * Creates a new #AiOpenCodeClient.
 * The opencode CLI must be available in PATH or specified via
 * %OPENCODE_PATH environment variable.
 *
 * Returns: (transfer full): a new #AiOpenCodeClient
 */
AiOpenCodeClient *
ai_opencode_client_new(void)
{
    g_autoptr(AiOpenCodeClient) self = g_object_new(AI_TYPE_OPENCODE_CLIENT, NULL);

    return (AiOpenCodeClient *)g_steal_pointer(&self);
}

/**
 * ai_opencode_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiOpenCodeClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiOpenCodeClient
 */
AiOpenCodeClient *
ai_opencode_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiOpenCodeClient) self = g_object_new(AI_TYPE_OPENCODE_CLIENT,
                                                     "config", config,
                                                     NULL);

    return (AiOpenCodeClient *)g_steal_pointer(&self);
}
