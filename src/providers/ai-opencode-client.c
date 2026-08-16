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
#include "providers/ai-opencode-client-internal.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

/*
 * Private structure for AiOpenCodeClient.
 */
struct _AiOpenCodeClient
{
    AiCliClient parent_instance;

    gboolean skip_permissions;

    /*
     * The rest of `opencode run`. Each is emitted only when set, so an
     * unconfigured client builds the same short command it always did.
     */
    gchar   *agent;
    gchar   *title;
    gchar   *files;          /* CSV -> repeated --file */
    gchar   *attach;         /* URL of a running opencode server */
    gchar   *log_level;
    gint     port;           /* 0 means "unset" */
    gboolean share;
    gboolean fork_session;
    gboolean continue_session;
    gboolean thinking;
    gboolean pure;
    gboolean print_logs;

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
    PROP_AGENT,
    PROP_TITLE,
    PROP_FILES,
    PROP_ATTACH,
    PROP_LOG_LEVEL,
    PROP_PORT,
    PROP_SHARE,
    PROP_FORK_SESSION,
    PROP_CONTINUE_SESSION,
    PROP_THINKING,
    PROP_PURE,
    PROP_PRINT_LOGS,
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
        case PROP_AGENT:
            g_value_set_string(value, self->agent);
            break;
        case PROP_TITLE:
            g_value_set_string(value, self->title);
            break;
        case PROP_FILES:
            g_value_set_string(value, self->files);
            break;
        case PROP_ATTACH:
            g_value_set_string(value, self->attach);
            break;
        case PROP_LOG_LEVEL:
            g_value_set_string(value, self->log_level);
            break;
        case PROP_PORT:
            g_value_set_int(value, self->port);
            break;
        case PROP_SHARE:
            g_value_set_boolean(value, self->share);
            break;
        case PROP_FORK_SESSION:
            g_value_set_boolean(value, self->fork_session);
            break;
        case PROP_CONTINUE_SESSION:
            g_value_set_boolean(value, self->continue_session);
            break;
        case PROP_THINKING:
            g_value_set_boolean(value, self->thinking);
            break;
        case PROP_PURE:
            g_value_set_boolean(value, self->pure);
            break;
        case PROP_PRINT_LOGS:
            g_value_set_boolean(value, self->print_logs);
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
        case PROP_AGENT:
            g_free(self->agent);
            self->agent = g_value_dup_string(value);
            break;
        case PROP_TITLE:
            g_free(self->title);
            self->title = g_value_dup_string(value);
            break;
        case PROP_FILES:
            g_free(self->files);
            self->files = g_value_dup_string(value);
            break;
        case PROP_ATTACH:
            g_free(self->attach);
            self->attach = g_value_dup_string(value);
            break;
        case PROP_LOG_LEVEL:
            g_free(self->log_level);
            self->log_level = g_value_dup_string(value);
            break;
        case PROP_PORT:
            self->port = g_value_get_int(value);
            break;
        case PROP_SHARE:
            self->share = g_value_get_boolean(value);
            break;
        case PROP_FORK_SESSION:
            self->fork_session = g_value_get_boolean(value);
            break;
        case PROP_CONTINUE_SESSION:
            self->continue_session = g_value_get_boolean(value);
            break;
        case PROP_THINKING:
            self->thinking = g_value_get_boolean(value);
            break;
        case PROP_PURE:
            self->pure = g_value_get_boolean(value);
            break;
        case PROP_PRINT_LOGS:
            self->print_logs = g_value_get_boolean(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * Spawn an opencode subprocess.
 *
 * A launcher is used whenever either the working directory or the
 * permission override is set. It used to be used only for the latter,
 * which meant working-directory was accepted, stored, and silently had no
 * effect unless skip-permissions happened to be on -- a caller that named
 * a directory to bound what the CLI could reach got the directory the
 * parent process was started in.
 *
 * OPENCODE_PERMISSION is set alongside --auto rather than instead of it:
 * the flag is the documented mechanism, the variable covers opencode
 * builds that predate it, and they agree.
 */
static GSubprocess *
ai_opencode_client_spawn(
    AiCliClient            *client,
    const gchar *const     *argv,
    GSubprocessFlags        flags,
    GError                **error
){
    AiOpenCodeClient *self = AI_OPENCODE_CLIENT(client);
    const gchar *cwd = ai_cli_client_get_working_directory(client);

    if (self->skip_permissions || (cwd != NULL && cwd[0] != '\0'))
    {
        g_autoptr(GSubprocessLauncher) launcher = NULL;

        launcher = g_subprocess_launcher_new(flags);

        if (self->skip_permissions)
        {
            g_subprocess_launcher_setenv(launcher,
                                          "OPENCODE_PERMISSION",
                                          OPENCODE_PERMISSION_ALLOW_ALL,
                                          TRUE);
        }

        if (cwd != NULL && cwd[0] != '\0')
        {
            g_subprocess_launcher_set_cwd(launcher, cwd);
        }

        return g_subprocess_launcher_spawnv(launcher, argv, error);
    }

    return g_subprocess_newv(argv, flags, error);
}

/*
 * The log levels the opencode CLI accepts. Validated here rather than
 * passed straight through: opencode is strict about its options and
 * rejects the whole invocation with its usage text, which is a much
 * worse error than one clear warning.
 */
static const gchar *AI_OPENCODE_LOG_LEVELS[] = {
    "DEBUG", "INFO", "WARN", "ERROR", NULL
};

static gboolean
log_level_is_valid(const gchar *level)
{
    gsize i;

    for (i = 0; AI_OPENCODE_LOG_LEVELS[i] != NULL; i++)
    {
        if (g_strcmp0(level, AI_OPENCODE_LOG_LEVELS[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Append a comma-separated value as the flag repeated once per item.
 * Empty items are dropped; an all-empty list emits nothing at all rather
 * than a dangling flag.
 */
static void
emit_repeated_flag(GPtrArray *args, const gchar *flag, const gchar *csv)
{
    g_auto(GStrv) parts = NULL;
    gsize i;

    if (csv == NULL || csv[0] == '\0')
        return;

    parts = g_strsplit(csv, ",", -1);

    for (i = 0; parts[i] != NULL; i++)
    {
        g_strstrip(parts[i]);
        if (parts[i][0] == '\0')
            continue;

        g_ptr_array_add(args, g_strdup(flag));
        g_ptr_array_add(args, g_strdup(parts[i]));
    }
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
 * Emit the flags that describe *how* opencode should run, as opposed to
 * what this particular turn is.
 *
 * Shared with the re-prompt path so the follow-up runs in the same agent,
 * the same directory, against the same server, with the same permission
 * posture. A retry that quietly dropped --auto would sit waiting for an
 * approval nobody is there to give.
 */
static void
emit_execution_args(AiOpenCodeClient *self, GPtrArray *args)
{
    const gchar *cwd;

    if (self->agent != NULL && self->agent[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--agent"));
        g_ptr_array_add(args, g_strdup(self->agent));
    }

    /* Attach to a running server instead of starting one. */
    if (self->attach != NULL && self->attach[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--attach"));
        g_ptr_array_add(args, g_strdup(self->attach));
    }

    /*
     * Working directory. The spawn already makes this the child's cwd;
     * --dir additionally tells opencode which project it is in, which is
     * what decides the session store it uses. When attaching, this is a
     * path on the remote server.
     */
    cwd = ai_cli_client_get_working_directory(AI_CLI_CLIENT(self));
    if (cwd != NULL && cwd[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--dir"));
        g_ptr_array_add(args, g_strdup(cwd));
    }

    if (self->port > 0)
    {
        g_ptr_array_add(args, g_strdup("--port"));
        g_ptr_array_add(args, g_strdup_printf("%d", self->port));
    }

    if (self->thinking)
    {
        g_ptr_array_add(args, g_strdup("--thinking"));
    }

    /*
     * Tool access. --auto is opencode's equivalent of Claude Code's
     * --dangerously-skip-permissions: it auto-approves everything not
     * explicitly denied.
     */
    if (self->skip_permissions)
    {
        g_ptr_array_add(args, g_strdup("--auto"));
    }

    /* Global opencode flags; accepted after the `run` subcommand. */
    if (self->pure)
    {
        g_ptr_array_add(args, g_strdup("--pure"));
    }

    if (self->log_level != NULL && self->log_level[0] != '\0')
    {
        if (log_level_is_valid(self->log_level))
        {
            g_ptr_array_add(args, g_strdup("--log-level"));
            g_ptr_array_add(args, g_strdup(self->log_level));
        }
        else
        {
            g_message("opencode: unknown log level '%s'; omitting the flag. "
                      "Valid levels: DEBUG, INFO, WARN, ERROR",
                      self->log_level);
        }
    }

    if (self->print_logs)
    {
        g_ptr_array_add(args, g_strdup("--print-logs"));
    }
}

/*
 * Build command line arguments for the opencode CLI.
 *
 * Command: opencode run --format json --model <model> [...]
 * Prompt is piped via stdin (build_stdin) to avoid ARG_MAX limits.
 */
gchar **
ai_opencode_client_build_argv(
    AiCliClient *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    gboolean     streaming
){
    AiOpenCodeClient *self = AI_OPENCODE_CLIENT(client);
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
    else if (self->continue_session)
    {
        /*
         * Only when there is no explicit id: --continue picks the last
         * session, which would fight a --session that names a different
         * one.
         */
        g_ptr_array_add(args, g_strdup("--continue"));
    }

    /* Fork the session being continued rather than appending to it. */
    if (self->fork_session &&
        ((session_id != NULL && session_id[0] != '\0') ||
         self->continue_session))
    {
        g_ptr_array_add(args, g_strdup("--fork"));
    }

    if (self->share)
    {
        g_ptr_array_add(args, g_strdup("--share"));
    }

    if (self->title != NULL && self->title[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--title"));
        g_ptr_array_add(args, g_strdup(self->title));
    }

    /* File attachments, one --file per item. */
    emit_repeated_flag(args, "--file", self->files);

    /* Variant (effort level) */
    {
        const gchar *effort = ai_cli_client_get_effort_level(client);
        if (effort != NULL && effort[0] != '\0')
        {
            g_ptr_array_add(args, g_strdup("--variant"));
            g_ptr_array_add(args, g_strdup(effort));
        }
    }

    /* Agent, server, directory, permissions and the global flags. */
    emit_execution_args(self, args);

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
        /* NULL root: a bare `null` line. See the streaming parser. */
        if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
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
                if (err_msg == NULL &&
                    json_object_has_member(err_obj, "data"))
                {
                    /*
                     * opencode's own errors nest one level deeper:
                     * {"error":{"name":"UnknownError",
                     *           "data":{"message":"..."}}}
                     * Without this the whole object was serialised into
                     * the message and the caller got raw JSON where a
                     * sentence belonged.
                     */
                    JsonNode *data_node =
                        json_object_get_member(err_obj, "data");

                    if (data_node != NULL && JSON_NODE_HOLDS_OBJECT(data_node))
                    {
                        JsonObject *data_obj = json_node_get_object(data_node);
                        const gchar *name;

                        err_msg = json_object_get_string_member_with_default(
                            data_obj, "message", NULL);

                        /* Prefix the error class when there is one:
                         * "UnknownError: ..." says more than "...". */
                        name = json_object_get_string_member_with_default(
                            err_obj, "name", NULL);
                        if (err_msg != NULL && name != NULL)
                        {
                            err_msg_tmp = g_strdup_printf("%s: %s", name,
                                                          err_msg);
                            err_msg = err_msg_tmp;
                        }
                    }
                }
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
        g_debug("opencode: no text or tool events found in %d bytes of output; "
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
 * Type-checked JSON accessors, for the same reason grok-build has them:
 * json-glib's *_member_with_default() emit a critical on a type mismatch,
 * and subprocess stdout is untrusted input that must not be able to abort a
 * fatal-warnings run.
 */
static const gchar *
oc_get_string(JsonObject *obj, const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return NULL;

    node = json_object_get_member(obj, member);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string(node);
}

static JsonObject *
oc_get_object(JsonObject *obj, const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return NULL;

    node = json_object_get_member(obj, member);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

static gint64
oc_get_int(JsonObject *obj, const gchar *member, gint64 fallback)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return fallback;

    node = json_object_get_member(obj, member);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    if (json_node_get_value_type(node) == G_TYPE_INT64)
        return json_node_get_int(node);

    if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
        return (gint64)json_node_get_double(node);

    return fallback;
}

/*
 * Turn one opencode tool_use part into events.
 *
 * opencode reports a tool as a single part whose state advances -- pending,
 * running, completed, error -- rather than as separate start and stop
 * events. A part that arrives already "completed" is the common case, so it
 * yields both a TOOL_STARTED and a TOOL_FINISHED: the consumer's state
 * machine then looks the same for opencode as for the providers that really
 * do announce a call before answering it.
 *
 * This information used to be assembled into a markdown string and thrown
 * away the moment any text arrived, which is why a tool-heavy opencode run
 * showed nothing of what it did.
 */
static void
oc_emit_tool_part(
    JsonObject *part,
    GPtrArray  *out_events
){
    JsonObject *state;
    const gchar *tool;
    const gchar *status;
    const gchar *id;
    JsonNode *input;
    g_autoptr(AiToolUse) tool_use = NULL;

    if (part == NULL)
        return;

    tool = oc_get_string(part, "tool");

    if (tool == NULL || tool[0] == '\0')
        tool = "tool";

    /* opencode has spelled this both ways across versions. */
    id = oc_get_string(part, "id");
    if (id == NULL)
        id = oc_get_string(part, "callID");
    if (id == NULL)
        id = "";

    state = oc_get_object(part, "state");
    status = state != NULL ? oc_get_string(state, "status") : NULL;

    input = state != NULL && json_object_has_member(state, "input")
        ? json_object_get_member(state, "input")
        : NULL;

    tool_use = ai_tool_use_new(id, tool,
                               input);

    g_ptr_array_add(out_events, ai_event_new_tool_started(tool_use));

    if (g_strcmp0(status, "completed") == 0)
    {
        const gchar *output = state != NULL ? oc_get_string(state, "output") : NULL;
        g_autoptr(AiToolResult) result =
            ai_tool_result_new_with_name(id, tool,
                                         output != NULL ? output : "", FALSE);

        g_ptr_array_add(out_events,
                        ai_event_new_tool_finished(tool_use, result));
    }
    else if (g_strcmp0(status, "error") == 0)
    {
        const gchar *err = state != NULL ? oc_get_string(state, "error") : NULL;
        g_autoptr(AiToolResult) result =
            ai_tool_result_new_with_name(id, tool,
                                         err != NULL ? err : "unknown error",
                                         TRUE);

        g_ptr_array_add(out_events,
                        ai_event_new_tool_finished(tool_use, result));
    }
}

/*
 * Parse a single NDJSON line from opencode into events.
 *
 * opencode wraps everything in {"type": ..., "part": {...}}:
 *   {"type":"text","part":{"text":"..."}}                     -> TEXT_DELTA
 *   {"type":"tool_use","part":{"tool":..,"state":{...}}}       -> tool events
 *   {"type":"step_finish","part":{"tokens":{"input":N,...}}}   -> USAGE
 *
 * Note that --format json is the only format opencode has; there is no
 * stream-json. The lines still arrive as the run progresses, so reading them
 * incrementally is what makes it feel live.
 */
static gboolean
ai_opencode_client_parse_stream_events(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    GPtrArray    *out_events,
    GError      **error
){
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    JsonObject *part;
    const gchar *type;
    const gchar *session_id;

    if (line == NULL || line[0] == '\0')
    {
        return TRUE;
    }

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, line, -1, error))
    {
        g_clear_error(error);
        return TRUE;
    }

    root = json_parser_get_root(parser);

    /*
     * A bare `null` document parses successfully and yields a NULL root, and
     * JSON_NODE_HOLDS_OBJECT() would dereference it -- a critical, which is
     * fatal under G_DEBUG=fatal-warnings. Subprocess stdout is untrusted, so
     * the NULL check comes first.
     */
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
    {
        return TRUE;
    }

    obj = json_node_get_object(root);
    type = oc_get_string(obj, "type");
    part = oc_get_object(obj, "part");

    /* opencode spells this camelCase, unlike the rest of its payload. */
    session_id = oc_get_string(obj, "sessionID");
    if (session_id == NULL && part != NULL)
        session_id = oc_get_string(part, "sessionID");

    if (session_id != NULL && session_id[0] != '\0' &&
        ai_cli_client_get_session_persistence(client))
    {
        ai_cli_client_set_session_id(client, session_id);
    }

    if (g_strcmp0(type, "text") == 0)
    {
        const gchar *text = part != NULL ? oc_get_string(part, "text") : NULL;

        if (text != NULL && text[0] != '\0')
            g_ptr_array_add(out_events, ai_event_new_text_delta(text));
    }
    else if (g_strcmp0(type, "tool_use") == 0)
    {
        oc_emit_tool_part(part, out_events);
    }
    else if (g_strcmp0(type, "step_finish") == 0)
    {
        JsonObject *tokens = part != NULL ? oc_get_object(part, "tokens") : NULL;

        if (tokens != NULL)
        {
            gint input_tokens = (gint)oc_get_int(tokens, "input", 0);
            gint output_tokens = (gint)oc_get_int(tokens, "output", 0);
            g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

            ai_response_set_usage(response, usage);

            /* opencode reports no cost at all, hence -1 rather than 0. */
            g_ptr_array_add(out_events, ai_event_new_usage(usage, -1));
        }
    }

    return TRUE;
}

/*
 * The pre-event contract, kept because callers and tests still use it.
 * A projection of parse_stream_events rather than a second implementation.
 */
static gboolean
ai_opencode_client_parse_stream_line(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    gchar       **delta_text,
    GError      **error
){
    g_autoptr(GPtrArray) events =
        g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);

    *delta_text = NULL;

    if (!ai_opencode_client_parse_stream_events(client, line, response,
                                                events, error))
    {
        return FALSE;
    }

    *delta_text = ai_cli_client_events_to_delta(events);
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
    g_free(self->agent);
    g_free(self->title);
    g_free(self->files);
    g_free(self->attach);
    g_free(self->log_level);

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
    cli_class->parse_stream_events = ai_opencode_client_parse_stream_events;

    /*
     * opencode is the one wrapper that needs more than a working directory
     * from the launcher, so it is the one that overrides spawn.
     */
    cli_class->spawn = ai_opencode_client_spawn;

    /**
     * AiOpenCodeClient:skip-permissions:
     *
     * Whether to auto-approve every permission prompt, enabling fully
     * autonomous headless operation.
     *
     * This passes `--auto` -- opencode's equivalent of Claude Code's
     * `--dangerously-skip-permissions` -- and additionally sets
     * OPENCODE_PERMISSION in the child environment, which covers opencode
     * builds predating the flag. The two agree, so setting both is safe.
     *
     * As with every other provider, the working directory rather than this
     * property is the boundary that matters. See
     * #AiCliClient:working-directory.
     */
    oc_properties[PROP_SKIP_PERMISSIONS] =
        g_param_spec_boolean("skip-permissions",
                             "Skip Permissions",
                             "Whether to auto-approve all opencode permission prompts",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:agent:
     *
     * The agent to run as, passed as `--agent`.
     */
    oc_properties[PROP_AGENT] =
        g_param_spec_string("agent",
                            "Agent",
                            "Agent to use (--agent)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:title:
     *
     * A title for the session, passed as `--title`. Without one opencode
     * derives a title from the prompt, which for a piped prompt means the
     * first line of whatever the caller assembled.
     */
    oc_properties[PROP_TITLE] =
        g_param_spec_string("title",
                            "Title",
                            "Title for the session (--title)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:files:
     *
     * Comma-separated paths to attach to the message. Each becomes its own
     * `--file <path>` pair.
     */
    oc_properties[PROP_FILES] =
        g_param_spec_string("files",
                            "Files",
                            "Comma-separated file attachments (--file)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:attach:
     *
     * URL of a running opencode server to attach to, e.g.
     * "http://localhost:4096", passed as `--attach`. When set,
     * #AiCliClient:working-directory names a path on that server rather
     * than a local one.
     *
     * Credentials for a password-protected server are deliberately not
     * exposed as properties: an argv is visible to every process on the
     * machine. opencode reads OPENCODE_SERVER_USERNAME and
     * OPENCODE_SERVER_PASSWORD from the environment instead.
     */
    oc_properties[PROP_ATTACH] =
        g_param_spec_string("attach",
                            "Attach",
                            "URL of a running opencode server (--attach)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:log-level:
     *
     * One of "DEBUG", "INFO", "WARN" or "ERROR", passed as `--log-level`.
     * Anything else is dropped with a warning, because opencode rejects an
     * invalid value by refusing the whole invocation.
     */
    oc_properties[PROP_LOG_LEVEL] =
        g_param_spec_string("log-level",
                            "Log Level",
                            "CLI --log-level (DEBUG, INFO, WARN, ERROR)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:port:
     *
     * Port for the local opencode server, passed as `--port`. Zero, the
     * default, omits the flag and lets opencode pick one.
     */
    oc_properties[PROP_PORT] =
        g_param_spec_int("port",
                         "Port",
                         "Port for the local server (--port); 0 to omit",
                         0, 65535, 0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:share:
     *
     * Whether to pass `--share`, publishing a shareable link to the
     * session. Off by default: a session can contain anything the prompt
     * and the repository did.
     */
    oc_properties[PROP_SHARE] =
        g_param_spec_boolean("share",
                             "Share",
                             "Whether to share the session (--share)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:fork-session:
     *
     * Whether to pass `--fork`, branching the continued session instead of
     * appending to it. Only emitted when there is a session to fork.
     */
    oc_properties[PROP_FORK_SESSION] =
        g_param_spec_boolean("fork-session",
                             "Fork Session",
                             "Whether to fork the continued session (--fork)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:continue-session:
     *
     * Whether to pass `--continue`, resuming the most recent session when
     * no #AiCliClient:session-id is known. An explicit session id wins.
     */
    oc_properties[PROP_CONTINUE_SESSION] =
        g_param_spec_boolean("continue-session",
                             "Continue Session",
                             "Whether to continue the last session (--continue)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:thinking:
     *
     * Whether to pass `--thinking`, making opencode emit reasoning blocks.
     */
    oc_properties[PROP_THINKING] =
        g_param_spec_boolean("thinking",
                             "Thinking",
                             "Whether to show thinking blocks (--thinking)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:pure:
     *
     * Whether to pass `--pure`, running without external plugins. Useful
     * when a run has to be reproducible regardless of what the host has
     * installed.
     */
    oc_properties[PROP_PURE] =
        g_param_spec_boolean("pure",
                             "Pure",
                             "Whether to run without external plugins (--pure)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiOpenCodeClient:print-logs:
     *
     * Whether to pass `--print-logs`, sending opencode's logs to stderr.
     * They do not disturb the parsed output, which is read from stdout,
     * and they are included in the error message when a run fails.
     */
    oc_properties[PROP_PRINT_LOGS] =
        g_param_spec_boolean("print-logs",
                             "Print Logs",
                             "Whether to print logs to stderr (--print-logs)",
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
    g_clear_object(&data->task);
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
    g_debug("opencode: re-prompt failed, using tool summary as fallback");

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
    /* Same agent, directory, server and permission posture as the turn
     * being retried. */
    emit_execution_args(client, rargs);
    g_ptr_array_add(rargs, NULL);

    rproc = ai_cli_client_spawn(
        AI_CLI_CLIENT(client), (const gchar *const *)rargs->pdata,
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

    g_debug("opencode: no text in response, re-prompting for summary "
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

    /*
     * Parse before judging the exit status.
     *
     * A failing opencode run exits non-zero *and* prints a structured
     * error event on stdout, with stderr empty:
     *
     *   {"type":"error","error":{"name":"UnknownError",
     *                            "data":{"message":"..."}}}
     *
     * Reporting the status first meant reporting an empty stderr, or the
     * raw JSON, instead of the sentence inside it. The parser turns that
     * event into a real message, so it gets first refusal; the status is
     * only consulted when there is nothing parseable to report.
     */
    if (stdout_data != NULL && stdout_data[0] != '\0')
    {
        klass = AI_CLI_CLIENT_GET_CLASS(data->client);
        response = klass->parse_json_output(AI_CLI_CLIENT(data->client),
                                            stdout_data, &error);

        if (response == NULL)
        {
            g_task_return_error(data->task, g_steal_pointer(&error));
            chat_async_data_free(data);
            return;
        }
    }
    else
    {
        if (!g_subprocess_get_successful(data->subprocess))
        {
            g_task_return_new_error(data->task, AI_ERROR,
                                    AI_ERROR_CLI_EXECUTION,
                                    "CLI exited with status %d: %s",
                                    g_subprocess_get_exit_status(
                                        data->subprocess),
                                    (stderr_data != NULL &&
                                     stderr_data[0] != '\0')
                                    ? stderr_data : "Unknown error");
        }
        else
        {
            g_task_return_new_error(data->task, AI_ERROR,
                                    AI_ERROR_CLI_PARSE_ERROR,
                                    "CLI produced no output");
        }

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
    subprocess = ai_cli_client_spawn(
        AI_CLI_CLIENT(self), (const gchar *const *)argv,
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

    /*
     * Anthropic models.  Only IDs that are still served upstream are
     * advertised — the retired SONNET_4 / OPUS_4 / HAIKU_3_5 defines
     * remain in the header for source compat but are deliberately not
     * offered here.
     */
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_FABLE_5));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_OPUS_5));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_SONNET_5));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_HAIKU_4_5));
    models = g_list_append(models, g_strdup(AI_OPENCODE_MODEL_CLAUDE_OPUS_4_5));

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
 *
 * The read loop lives in AiCliClient. All this client contributes is the
 * translation from its wire format into events -- the parse_stream_events
 * vfunc above. Spawning, line reading, signal emission, the deadline and
 * the response assembly are shared with every other CLI wrapper.
 */

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
    (void)tools;  /* Tools not yet supported via CLI */

    ai_cli_client_stream_run_async(AI_CLI_CLIENT(streamable), messages,
                                   system_prompt, max_tokens,
                                   cancellable, callback, user_data);
}

static AiResponse *
ai_opencode_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    return ai_cli_client_stream_run_finish(AI_CLI_CLIENT(streamable),
                                           result, error);
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
