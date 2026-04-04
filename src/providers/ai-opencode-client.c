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

    gboolean skip_permissions;

    /* Cached tool-call summary from the last response, used for
     * the re-prompt fallback when the AI produces no text. */
    gchar *last_tool_summary;
};

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_SKIP_PERMISSIONS,
    N_PROPS
};

static GParamSpec *oc_properties[N_PROPS];

/*
 * The JSON value set as OPENCODE_PERMISSION when skip_permissions is
 * enabled. This auto-approves every permission category including
 * external_directory and doom_loop (the only two that default to "ask").
 */
#define OPENCODE_PERMISSION_ALLOW_ALL "{\"*\":\"allow\"}"

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

static void
ai_opencode_client_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiOpenCodeClient *self = AI_OPENCODE_CLIENT(object);

    switch (prop_id)
    {
        case PROP_SKIP_PERMISSIONS:
            g_value_set_boolean(value, self->skip_permissions);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_opencode_client_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiOpenCodeClient *self = AI_OPENCODE_CLIENT(object);

    switch (prop_id)
    {
        case PROP_SKIP_PERMISSIONS:
            self->skip_permissions = g_value_get_boolean(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * Spawn an opencode subprocess. When skip_permissions is enabled we must
 * use GSubprocessLauncher so we can inject the OPENCODE_PERMISSION env
 * var into the child environment. Otherwise we fall back to the simpler
 * g_subprocess_newv().
 */
static GSubprocess *
ai_opencode_client_spawn(
    AiOpenCodeClient       *self,
    const gchar *const     *argv,
    GSubprocessFlags        flags,
    GError                **error
){
    if (self->skip_permissions)
    {
        g_autoptr(GSubprocessLauncher) launcher = NULL;
        const gchar *cwd;

        launcher = g_subprocess_launcher_new(flags);
        g_subprocess_launcher_setenv(launcher,
                                      "OPENCODE_PERMISSION",
                                      OPENCODE_PERMISSION_ALLOW_ALL,
                                      TRUE);

        /* Honour the working directory if one was set on the base class */
        cwd = ai_cli_client_get_working_directory(AI_CLI_CLIENT(self));
        if (cwd != NULL)
        {
            g_subprocess_launcher_set_cwd(launcher, cwd);
        }

        return g_subprocess_launcher_spawnv(launcher, argv, error);
    }

    return g_subprocess_newv(argv, flags, error);
}

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

    /* Variant (effort level) */
    {
        const gchar *effort = ai_cli_client_get_effort_level(client);
        if (effort != NULL && effort[0] != '\0')
        {
            g_ptr_array_add(args, g_strdup("--variant"));
            g_ptr_array_add(args, g_strdup(effort));
        }
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

    /* Instruct the AI to always produce a plain text response */
    g_string_append(prompt,
        "\n\nIMPORTANT: Always include a plain text response. "
        "Tool use is fine, but you MUST provide a text summary of "
        "your work when finished. Never end your turn on tool calls alone.");

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
    g_autoptr(GString) tool_summary = NULL;
    gchar **lines;
    gint i;
    gint input_tokens = 0;
    gint output_tokens = 0;

    /* Create response */
    response = ai_response_new("", ai_cli_client_get_model(client));
    ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    accumulated_text = g_string_new("");
    tool_summary     = g_string_new("");

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

        /* Check for error — the "error" field may be a plain string or
         * a JSON object with a "message" sub-field (e.g. opencode
         * permission errors).  Handle both forms gracefully. */
        if (json_object_has_member(obj, "error"))
        {
            JsonNode *err_node = json_object_get_member(obj, "error");
            const gchar *err_msg = NULL;
            g_autofree gchar *err_msg_tmp = NULL;

            if (JSON_NODE_HOLDS_VALUE(err_node) &&
                json_node_get_value_type(err_node) == G_TYPE_STRING)
            {
                err_msg = json_node_get_string(err_node);
            }
            else if (JSON_NODE_HOLDS_OBJECT(err_node))
            {
                JsonObject *err_obj = json_node_get_object(err_node);
                /* Try "message" first (most common), then "error" */
                err_msg = json_object_get_string_member_with_default(
                    err_obj, "message", NULL);
                if (err_msg == NULL)
                    err_msg = json_object_get_string_member_with_default(
                        err_obj, "error", NULL);
                if (err_msg == NULL)
                {
                    /* Last resort: serialise the object so logs are useful */
                    g_autoptr(JsonGenerator) gen = json_generator_new();
                    json_generator_set_root(gen, err_node);
                    err_msg_tmp = json_generator_to_data(gen, NULL);
                    err_msg = err_msg_tmp;
                }
            }

            if (err_msg == NULL)
                err_msg = "Unknown error";

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
        else if (g_strcmp0(type, "tool_use") == 0)
        {
            /*
             * The AI made a tool call. Accumulate a human-readable summary
             * so that if no text event follows (e.g. the agentic loop ended
             * on tool-calls or a call was rejected) we have something to
             * show the user rather than failing with "no text content".
             */
            if (json_object_has_member(obj, "part"))
            {
                JsonObject *part  = json_object_get_object_member(obj, "part");
                JsonObject *state = NULL;
                const gchar *tool;
                const gchar *status;

                tool = json_object_has_member(part, "tool")
                    ? json_object_get_string_member_with_default(part, "tool", "tool")
                    : "tool";

                if (json_object_has_member(part, "state"))
                    state = json_object_get_object_member(part, "state");

                status = (state != NULL)
                    ? json_object_get_string_member_with_default(state, "status", "")
                    : "";

                if (g_strcmp0(status, "completed") == 0 && state != NULL)
                {
                    JsonObject *inp = json_object_has_member(state, "input")
                        ? json_object_get_object_member(state, "input")
                        : NULL;
                    const gchar *cmd = (inp != NULL && json_object_has_member(inp, "command"))
                        ? json_object_get_string_member_with_default(inp, "command", "")
                        : "";
                    const gchar *out = json_object_get_string_member_with_default(
                        state, "output", "");

                    if (tool_summary->len > 0)
                        g_string_append_c(tool_summary, '\n');
                    if (cmd[0] != '\0')
                        g_string_append_printf(tool_summary,
                                               "**%s:** `%s`\n```\n%s```", tool, cmd, out);
                    else
                        g_string_append_printf(tool_summary,
                                               "**%s:**\n```\n%s```", tool, out);
                }
                else if (g_strcmp0(status, "error") == 0 && state != NULL)
                {
                    const gchar *err = json_object_get_string_member_with_default(
                        state, "error", "unknown error");

                    if (tool_summary->len > 0)
                        g_string_append_c(tool_summary, '\n');
                    g_string_append_printf(tool_summary,
                                           "**%s:** (failed: %s)", tool, err);
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
    else if (tool_summary->len > 0)
    {
        /*
         * Tool calls happened but no text synthesis. Store the summary
         * so the completion callback can attempt a re-prompt for text.
         * If the re-prompt fails, this summary is used as fallback.
         */
        AiOpenCodeClient *self = AI_OPENCODE_CLIENT(client);
        g_free(self->last_tool_summary);
        self->last_tool_summary = g_strdup(tool_summary->str);
    }
    else
    {
        /* Genuinely empty — log raw output for debugging */
        g_warning("opencode: no text or tool events found in %d bytes of output; "
                  "raw output follows:\n%s",
                  (int)(json_output ? strlen(json_output) : 0),
                  json_output ? json_output : "(null)");
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

/*
 * Destructor — free instance data.
 */
static void
ai_opencode_client_finalize(GObject *object)
{
    AiOpenCodeClient *self = AI_OPENCODE_CLIENT(object);

    g_free(self->last_tool_summary);

    G_OBJECT_CLASS(ai_opencode_client_parent_class)->finalize(object);
}

static void
ai_opencode_client_class_init(AiOpenCodeClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

    object_class->finalize     = ai_opencode_client_finalize;
    object_class->get_property = ai_opencode_client_get_property;
    object_class->set_property = ai_opencode_client_set_property;

    /* Override virtual methods */
    cli_class->get_executable_path = ai_opencode_client_get_executable_path;
    cli_class->build_argv = ai_opencode_client_build_argv;
    cli_class->build_stdin = ai_opencode_client_build_stdin;
    cli_class->parse_json_output = ai_opencode_client_parse_json_output;
    cli_class->parse_stream_line = ai_opencode_client_parse_stream_line;

    /**
     * AiOpenCodeClient:skip-permissions:
     *
     * Whether to set OPENCODE_PERMISSION to auto-approve all
     * permission prompts. When enabled, the opencode subprocess
     * inherits an environment variable that allows every operation
     * (including external_directory access) without interactive
     * approval, enabling fully autonomous headless operation.
     */
    oc_properties[PROP_SKIP_PERMISSIONS] =
        g_param_spec_boolean("skip-permissions",
                             "Skip Permissions",
                             "Whether to auto-approve all opencode permission prompts",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, oc_properties);
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

/*
 * Retry data — used when the AI made tool calls but produced no text.
 * We re-prompt asking for a plain-text summary; if that also fails
 * we fall back to the raw tool_summary string.
 */
typedef struct
{
    AiOpenCodeClient *client;
    GTask            *task;
    GSubprocess      *subprocess;
    gchar            *tool_summary;
} RetryAsyncData;

static void
retry_async_data_free(RetryAsyncData *data)
{
    g_clear_object(&data->client);
    g_clear_object(&data->subprocess);
    g_free(data->tool_summary);
    g_slice_free(RetryAsyncData, data);
}

static void
on_retry_communicate_complete(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
)
{
    RetryAsyncData *data = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *stdout_data = NULL;
    g_autofree gchar *stderr_data = NULL;
    AiCliClientClass *klass;
    AiResponse *response = NULL;

    if (!g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                               &stdout_data, &stderr_data,
                                               &error))
    {
        goto fallback;
    }

    if (!g_subprocess_get_successful(data->subprocess))
        goto fallback;

    if (stdout_data == NULL || stdout_data[0] == '\0')
        goto fallback;

    klass = AI_CLI_CLIENT_GET_CLASS(data->client);
    response = klass->parse_json_output(AI_CLI_CLIENT(data->client),
                                         stdout_data, &error);

    if (response != NULL &&
        ai_response_get_content_blocks(response) != NULL)
    {
        /* Re-prompt succeeded — return the synthesized text */
        g_task_return_pointer(data->task, response, g_object_unref);
        retry_async_data_free(data);
        return;
    }

    g_clear_object(&response);

fallback:
    g_clear_error(&error);
    g_warning("opencode: re-prompt failed, using tool summary as fallback");

    response = ai_response_new("",
        ai_cli_client_get_model(AI_CLI_CLIENT(data->client)));
    {
        g_autoptr(AiTextContent) tc = ai_text_content_new(data->tool_summary);
        ai_response_add_content_block(response,
            (AiContentBlock *)g_steal_pointer(&tc));
    }
    ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    g_task_return_pointer(data->task, response, g_object_unref);
    retry_async_data_free(data);
}

/*
 * Attempt to re-prompt the AI for a plain-text summary of its tool work.
 * Returns TRUE if the retry subprocess was spawned (task ownership
 * transferred to the retry callback), FALSE if it could not start.
 */
static gboolean
attempt_text_retry(
    AiOpenCodeClient *client,
    GTask            *task,
    const gchar      *tool_summary
)
{
    g_autoptr(GError) err = NULL;
    g_autofree gchar *exe = NULL;
    g_autoptr(GPtrArray) rargs = NULL;
    GSubprocess *rproc;
    RetryAsyncData *retry;
    const gchar *model;
    const gchar *sid;

    exe = ai_cli_client_resolve_executable(AI_CLI_CLIENT(client), &err);
    if (exe == NULL)
        return FALSE;

    model = ai_cli_client_get_model(AI_CLI_CLIENT(client));
    sid   = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));

    rargs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(rargs, g_strdup(exe));
    g_ptr_array_add(rargs, g_strdup("run"));
    g_ptr_array_add(rargs, g_strdup("--format"));
    g_ptr_array_add(rargs, g_strdup("json"));
    g_ptr_array_add(rargs, g_strdup("--model"));
    g_ptr_array_add(rargs,
        g_strdup(model ? model : AI_OPENCODE_DEFAULT_MODEL));
    if (sid != NULL && sid[0] != '\0')
    {
        g_ptr_array_add(rargs, g_strdup("--session"));
        g_ptr_array_add(rargs, g_strdup(sid));
    }
    g_ptr_array_add(rargs, NULL);

    rproc = ai_opencode_client_spawn(
        client, (const gchar *const *)rargs->pdata,
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &err);

    if (rproc == NULL)
        return FALSE;

    retry = g_slice_new0(RetryAsyncData);
    retry->client      = g_object_ref(client);
    retry->task        = task;
    retry->subprocess  = rproc;   /* takes ownership */
    retry->tool_summary = g_strdup(tool_summary);

    g_warning("opencode: no text in response, re-prompting for summary "
              "(session=%s)", sid ? sid : "(none)");

    g_subprocess_communicate_utf8_async(
        rproc,
        "Provide a concise plain-text summary of what you just did. "
        "Do NOT use any tools.",
        NULL, on_retry_communicate_complete, retry);

    return TRUE;
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
        chat_async_data_free(data);
        return;
    }

    /*
     * If the AI only made tool calls without synthesizing text, attempt
     * a follow-up prompt asking it to summarize; fall back to raw
     * tool output if the retry also fails.
     */
    if (ai_response_get_content_blocks(response) == NULL &&
        data->client->last_tool_summary != NULL)
    {
        if (attempt_text_retry(data->client, data->task,
                                data->client->last_tool_summary))
        {
            g_object_unref(response);
            data->task = NULL;   /* retry owns the task now */
            chat_async_data_free(data);
            return;
        }

        /* Retry could not start — use tool summary as fallback */
        g_object_unref(response);
        response = ai_response_new("",
            ai_cli_client_get_model(AI_CLI_CLIENT(data->client)));
        {
            g_autoptr(AiTextContent) tc = ai_text_content_new(
                data->client->last_tool_summary);
            ai_response_add_content_block(response,
                (AiContentBlock *)g_steal_pointer(&tc));
        }
        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }

    g_task_return_pointer(data->task, response, g_object_unref);
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
    g_autofree gchar *stdin_buf = NULL;
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

    /* Build prompt to send to opencode via stdin */
    stdin_buf = klass->build_stdin(AI_CLI_CLIENT(self), messages);

    /* Spawn subprocess (with OPENCODE_PERMISSION when skip_permissions) */
    subprocess = ai_opencode_client_spawn(
        self, (const gchar *const *)argv,
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
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

    /* Start async communication — stdin_buf is the prompt piped to opencode */
    g_subprocess_communicate_utf8_async(subprocess, stdin_buf, cancellable,
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

    /* Spawn subprocess (with OPENCODE_PERMISSION when skip_permissions) */
    subprocess = ai_opencode_client_spawn(
        self, (const gchar *const *)argv,
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &error);
    if (subprocess == NULL)
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    /* Write the prompt to stdin and close it so opencode can start */
    {
        g_autofree gchar *stdin_buf =
            klass->build_stdin(AI_CLI_CLIENT(self), messages);
        GOutputStream *stdin_pipe = g_subprocess_get_stdin_pipe(subprocess);

        if (stdin_buf != NULL && stdin_pipe != NULL)
        {
            g_output_stream_write_all(stdin_pipe, stdin_buf, strlen(stdin_buf),
                                      NULL, NULL, NULL);
        }
        if (stdin_pipe != NULL)
            g_output_stream_close(stdin_pipe, NULL, NULL);
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

/**
 * ai_opencode_client_get_skip_permissions:
 * @self: an #AiOpenCodeClient
 *
 * Gets whether permission auto-approval is enabled.
 *
 * Returns: %TRUE if skip permissions is enabled
 */
gboolean
ai_opencode_client_get_skip_permissions(AiOpenCodeClient *self)
{
    g_return_val_if_fail(AI_IS_OPENCODE_CLIENT(self), FALSE);

    return self->skip_permissions;
}

/**
 * ai_opencode_client_set_skip_permissions:
 * @self: an #AiOpenCodeClient
 * @skip: whether to auto-approve all permission prompts
 *
 * Sets whether to auto-approve all opencode permission prompts by
 * injecting the OPENCODE_PERMISSION environment variable into the
 * child process. When enabled, the opencode CLI will not prompt for
 * approval on any operation (including external directory access),
 * allowing fully autonomous headless operation.
 */
void
ai_opencode_client_set_skip_permissions(
    AiOpenCodeClient *self,
    gboolean          skip
){
    g_return_if_fail(AI_IS_OPENCODE_CLIENT(self));

    if (self->skip_permissions != skip)
    {
        self->skip_permissions = skip;
        g_object_notify_by_pspec(G_OBJECT(self),
                                  oc_properties[PROP_SKIP_PERMISSIONS]);
    }
}
