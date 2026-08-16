/*
 * ai-cli-client.c - Base client class for CLI-based providers
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-cli-client.h"
#include "core/ai-error.h"
#include "core/ai-event-source.h"
#include "core/ai-subprocess-util.h"
#include "model/ai-text-content.h"

/*
 * Private data for AiCliClient.
 */
typedef struct
{
    AiConfig *config;
    gchar    *model;
    gchar    *system_prompt;
    gchar    *executable_path;
    gchar    *session_id;
    gchar    *working_directory;
    gchar    *effort_level;
    gint      max_tokens;
    gint      process_timeout_ms;
    gboolean  session_persistence;
} AiCliClientPrivate;

/*
 * The event-source interface is implemented on the base rather than on each
 * wrapper, so a subclass gets the ::event signal for free and only has to
 * translate its own wire format.  There are no vfuncs to fill in.
 */
G_DEFINE_TYPE_WITH_CODE(AiCliClient, ai_cli_client, G_TYPE_OBJECT,
                        G_ADD_PRIVATE(AiCliClient)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_EVENT_SOURCE, NULL))

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_CONFIG,
    PROP_MODEL,
    PROP_MAX_TOKENS,
    PROP_SYSTEM_PROMPT,
    PROP_EXECUTABLE_PATH,
    PROP_SESSION_ID,
    PROP_SESSION_PERSISTENCE,
    PROP_WORKING_DIRECTORY,
    PROP_EFFORT_LEVEL,
    PROP_PROCESS_TIMEOUT_MS,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/*
 * Signal IDs for streaming.
 */
enum
{
    SIGNAL_DELTA,
    SIGNAL_STREAM_START,
    SIGNAL_STREAM_END,
    SIGNAL_TOOL_USE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean
ai_cli_client_real_parse_stream_events(
    AiCliClient  *self,
    const gchar  *line,
    AiResponse   *response,
    GPtrArray    *out_events,
    GError      **error
);

static GSubprocess *
ai_cli_client_real_spawn(
    AiCliClient          *self,
    const gchar * const  *argv,
    GSubprocessFlags      flags,
    GError              **error
);

static void
ai_cli_client_finalize(GObject *object)
{
    AiCliClient *self = AI_CLI_CLIENT(object);
    AiCliClientPrivate *priv = ai_cli_client_get_instance_private(self);

    g_clear_object(&priv->config);
    g_clear_pointer(&priv->model, g_free);
    g_clear_pointer(&priv->system_prompt, g_free);
    g_clear_pointer(&priv->executable_path, g_free);
    g_clear_pointer(&priv->session_id, g_free);
    g_clear_pointer(&priv->working_directory, g_free);
    g_clear_pointer(&priv->effort_level, g_free);

    G_OBJECT_CLASS(ai_cli_client_parent_class)->finalize(object);
}

static void
ai_cli_client_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiCliClient *self = AI_CLI_CLIENT(object);
    AiCliClientPrivate *priv = ai_cli_client_get_instance_private(self);

    switch (prop_id)
    {
        case PROP_CONFIG:
            g_value_set_object(value, priv->config);
            break;
        case PROP_MODEL:
            g_value_set_string(value, priv->model);
            break;
        case PROP_MAX_TOKENS:
            g_value_set_int(value, priv->max_tokens);
            break;
        case PROP_SYSTEM_PROMPT:
            g_value_set_string(value, priv->system_prompt);
            break;
        case PROP_EXECUTABLE_PATH:
            g_value_set_string(value, priv->executable_path);
            break;
        case PROP_SESSION_ID:
            g_value_set_string(value, priv->session_id);
            break;
        case PROP_SESSION_PERSISTENCE:
            g_value_set_boolean(value, priv->session_persistence);
            break;
        case PROP_WORKING_DIRECTORY:
            g_value_set_string(value, priv->working_directory);
            break;
        case PROP_EFFORT_LEVEL:
            g_value_set_string(value, priv->effort_level);
            break;
        case PROP_PROCESS_TIMEOUT_MS:
            g_value_set_int(value, priv->process_timeout_ms);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_cli_client_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiCliClient *self = AI_CLI_CLIENT(object);
    AiCliClientPrivate *priv = ai_cli_client_get_instance_private(self);

    switch (prop_id)
    {
        case PROP_CONFIG:
            g_clear_object(&priv->config);
            priv->config = g_value_dup_object(value);
            break;
        case PROP_MODEL:
            g_clear_pointer(&priv->model, g_free);
            priv->model = g_value_dup_string(value);
            break;
        case PROP_MAX_TOKENS:
            priv->max_tokens = g_value_get_int(value);
            break;
        case PROP_SYSTEM_PROMPT:
            g_clear_pointer(&priv->system_prompt, g_free);
            priv->system_prompt = g_value_dup_string(value);
            break;
        case PROP_EXECUTABLE_PATH:
            g_clear_pointer(&priv->executable_path, g_free);
            priv->executable_path = g_value_dup_string(value);
            break;
        case PROP_SESSION_ID:
            g_clear_pointer(&priv->session_id, g_free);
            priv->session_id = g_value_dup_string(value);
            break;
        case PROP_SESSION_PERSISTENCE:
            priv->session_persistence = g_value_get_boolean(value);
            break;
        case PROP_WORKING_DIRECTORY:
            g_clear_pointer(&priv->working_directory, g_free);
            priv->working_directory = g_value_dup_string(value);
            break;
        case PROP_EFFORT_LEVEL:
            g_clear_pointer(&priv->effort_level, g_free);
            priv->effort_level = g_value_dup_string(value);
            break;
        case PROP_PROCESS_TIMEOUT_MS:
            priv->process_timeout_ms = g_value_get_int(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_cli_client_constructed(GObject *object)
{
    AiCliClient *self = AI_CLI_CLIENT(object);
    AiCliClientPrivate *priv = ai_cli_client_get_instance_private(self);

    G_OBJECT_CLASS(ai_cli_client_parent_class)->constructed(object);

    /* Create config if not provided */
    if (priv->config == NULL)
    {
        priv->config = ai_config_new();
    }
}

static void
ai_cli_client_class_init(AiCliClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_cli_client_finalize;
    object_class->get_property = ai_cli_client_get_property;
    object_class->set_property = ai_cli_client_set_property;
    object_class->constructed = ai_cli_client_constructed;

    /* Virtual methods default to NULL - subclasses must implement */
    klass->get_executable_path = NULL;
    klass->build_argv = NULL;
    klass->parse_json_output = NULL;
    klass->parse_stream_line = NULL;
    klass->build_stdin = NULL;
    klass->chat_sync = NULL;

    /*
     * These two do have defaults. parse_stream_events falls back to
     * parse_stream_line so an unmigrated subclass keeps working, and spawn
     * handles the working directory every wrapper needs.
     */
    klass->parse_stream_events = ai_cli_client_real_parse_stream_events;
    klass->spawn = ai_cli_client_real_spawn;

    /**
     * AiCliClient:config:
     *
     * The configuration for this client.
     */
    properties[PROP_CONFIG] =
        g_param_spec_object("config",
                            "Config",
                            "The configuration for this client",
                            AI_TYPE_CONFIG,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:model:
     *
     * The model to use for requests.
     */
    properties[PROP_MODEL] =
        g_param_spec_string("model",
                            "Model",
                            "The model to use for requests",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:max-tokens:
     *
     * The default maximum tokens to generate.
     */
    properties[PROP_MAX_TOKENS] =
        g_param_spec_int("max-tokens",
                         "Max Tokens",
                         "The default maximum tokens to generate",
                         1, G_MAXINT, 4096,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:system-prompt:
     *
     * The default system prompt.
     */
    properties[PROP_SYSTEM_PROMPT] =
        g_param_spec_string("system-prompt",
                            "System Prompt",
                            "The default system prompt",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:executable-path:
     *
     * Override path to the CLI executable.
     */
    properties[PROP_EXECUTABLE_PATH] =
        g_param_spec_string("executable-path",
                            "Executable Path",
                            "Override path to the CLI executable",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:session-id:
     *
     * Current session ID for conversation continuity.
     */
    properties[PROP_SESSION_ID] =
        g_param_spec_string("session-id",
                            "Session ID",
                            "Current session ID for conversation continuity",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:session-persistence:
     *
     * Whether to persist sessions for conversation continuity.
     */
    properties[PROP_SESSION_PERSISTENCE] =
        g_param_spec_boolean("session-persistence",
                             "Session Persistence",
                             "Whether to persist sessions",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:working-directory:
     *
     * The working directory for the CLI subprocess.
     */
    properties[PROP_WORKING_DIRECTORY] =
        g_param_spec_string("working-directory",
                            "Working Directory",
                            "The working directory for the CLI subprocess",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:effort-level:
     *
     * The reasoning effort level for the CLI provider.
     * Maps to --effort for Claude Code and --variant for OpenCode.
     * Accepts: "low", "medium", "high", "max". Defaults to "medium".
     */
    properties[PROP_EFFORT_LEVEL] =
        g_param_spec_string("effort-level",
                            "Effort Level",
                            "Reasoning effort level (low/medium/high/xhigh/max)",
                            "medium",
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCliClient:process-timeout-ms:
     *
     * Wall-clock deadline for one synchronous CLI subprocess run.
     * On expiry the child is killed and the call fails with
     * %AI_ERROR_TIMEOUT, so a wedged CLI (for example a half-open
     * network connection the child is blocked on) can never pin the
     * calling thread forever.  0 disables the deadline.
     *
     * Since: 0.23.3
     */
    properties[PROP_PROCESS_TIMEOUT_MS] =
        g_param_spec_int("process-timeout-ms",
                         "Process Timeout (ms)",
                         "Wall-clock deadline for one CLI subprocess "
                         "run (0 disables)",
                         0, G_MAXINT, 1800000,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);

    /**
     * AiCliClient::delta:
     * @self: the #AiCliClient
     * @text: the new text content
     *
     * Emitted when new text content is available during streaming.
     */
    signals[SIGNAL_DELTA] =
        g_signal_new("delta",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1, G_TYPE_STRING);

    /**
     * AiCliClient::stream-start:
     * @self: the #AiCliClient
     *
     * Emitted when streaming begins.
     */
    signals[SIGNAL_STREAM_START] =
        g_signal_new("stream-start",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 0);

    /**
     * AiCliClient::stream-end:
     * @self: the #AiCliClient
     * @response: the final #AiResponse
     *
     * Emitted when streaming ends.
     */
    signals[SIGNAL_STREAM_END] =
        g_signal_new("stream-end",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1, AI_TYPE_RESPONSE);

    /**
     * AiCliClient::tool-use:
     * @self: the #AiCliClient
     * @tool_use: the #AiToolUse
     *
     * Emitted when a tool use is detected during streaming.
     */
    signals[SIGNAL_TOOL_USE] =
        g_signal_new("tool-use",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1, AI_TYPE_TOOL_USE);
}

static void
ai_cli_client_init(AiCliClient *self)
{
    AiCliClientPrivate *priv = ai_cli_client_get_instance_private(self);

    priv->config = NULL;
    priv->model = NULL;
    priv->system_prompt = NULL;
    priv->executable_path = NULL;
    priv->session_id = NULL;
    priv->max_tokens = 4096;
    priv->process_timeout_ms = 1800000;   /* 30 min */
    priv->session_persistence = TRUE;
    priv->working_directory = NULL;
    priv->effort_level = g_strdup("medium");
}

/**
 * ai_cli_client_format_exit_error:
 *
 * See header for documentation.
 */
gchar *
ai_cli_client_format_exit_error(
    gint         exit_status,
    const gchar *stderr_data,
    const gchar *stdout_data
){
    const gchar *detail;

    /* Prefer stderr — that is where CLIs conventionally write errors.
     * But some CLIs exit non-zero with empty stderr and an error
     * message on stdout (notably anything that prints usage-on-error
     * or a JSON error envelope).  A previous version of this code
     * only checked for stderr being non-NULL, which produced the
     * unhelpful message "CLI exited with status N: " when stderr was
     * an empty string.  The empty-string check below is the fix. */
    if (stderr_data != NULL && stderr_data[0] != '\0')
    {
        detail = stderr_data;
    }
    else if (stdout_data != NULL && stdout_data[0] != '\0')
    {
        detail = stdout_data;
    }
    else
    {
        detail = "(no output on stderr or stdout)";
    }

    return g_strdup_printf("CLI exited with status %d: %s",
                           exit_status, detail);
}

/**
 * ai_cli_client_get_config:
 * @self: an #AiCliClient
 *
 * Gets the configuration for this client.
 *
 * Returns: (transfer none): the #AiConfig
 */
AiConfig *
ai_cli_client_get_config(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    priv = ai_cli_client_get_instance_private(self);
    return priv->config;
}

/**
 * ai_cli_client_get_model:
 * @self: an #AiCliClient
 *
 * Gets the model name.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_cli_client_get_model(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    priv = ai_cli_client_get_instance_private(self);
    return priv->model;
}

/**
 * ai_cli_client_set_model:
 * @self: an #AiCliClient
 * @model: the model name
 *
 * Sets the model to use for requests.
 */
void
ai_cli_client_set_model(
    AiCliClient *self,
    const gchar *model
){
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    g_clear_pointer(&priv->model, g_free);
    priv->model = g_strdup(model);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MODEL]);
}

/**
 * ai_cli_client_get_max_tokens:
 * @self: an #AiCliClient
 *
 * Gets the default max tokens setting.
 *
 * Returns: the max tokens
 */
gint
ai_cli_client_get_max_tokens(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), 4096);

    priv = ai_cli_client_get_instance_private(self);
    return priv->max_tokens;
}

/**
 * ai_cli_client_set_max_tokens:
 * @self: an #AiCliClient
 * @max_tokens: the max tokens
 *
 * Sets the default max tokens for requests.
 */
void
ai_cli_client_set_max_tokens(
    AiCliClient *self,
    gint         max_tokens
){
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    priv->max_tokens = max_tokens;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAX_TOKENS]);
}

/**
 * ai_cli_client_get_system_prompt:
 * @self: an #AiCliClient
 *
 * Gets the default system prompt.
 *
 * Returns: (transfer none) (nullable): the system prompt
 */
const gchar *
ai_cli_client_get_system_prompt(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    priv = ai_cli_client_get_instance_private(self);
    return priv->system_prompt;
}

/**
 * ai_cli_client_set_system_prompt:
 * @self: an #AiCliClient
 * @system_prompt: (nullable): the system prompt
 *
 * Sets the default system prompt for requests.
 */
void
ai_cli_client_set_system_prompt(
    AiCliClient *self,
    const gchar *system_prompt
){
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    g_clear_pointer(&priv->system_prompt, g_free);
    priv->system_prompt = g_strdup(system_prompt);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SYSTEM_PROMPT]);
}

/**
 * ai_cli_client_get_executable_path:
 * @self: an #AiCliClient
 *
 * Gets the path to the CLI executable.
 *
 * Returns: (transfer none) (nullable): the executable path, or %NULL to search PATH
 */
const gchar *
ai_cli_client_get_executable_path(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    priv = ai_cli_client_get_instance_private(self);
    return priv->executable_path;
}

/**
 * ai_cli_client_set_executable_path:
 * @self: an #AiCliClient
 * @path: (nullable): the executable path, or %NULL to search PATH
 *
 * Sets the path to the CLI executable.
 */
void
ai_cli_client_set_executable_path(
    AiCliClient *self,
    const gchar *path
){
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    g_clear_pointer(&priv->executable_path, g_free);
    priv->executable_path = g_strdup(path);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_EXECUTABLE_PATH]);
}

/**
 * ai_cli_client_get_session_id:
 * @self: an #AiCliClient
 *
 * Gets the current session ID for conversation continuity.
 *
 * Returns: (transfer none) (nullable): the session ID
 */
const gchar *
ai_cli_client_get_session_id(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    priv = ai_cli_client_get_instance_private(self);
    return priv->session_id;
}

/**
 * ai_cli_client_set_session_id:
 * @self: an #AiCliClient
 * @session_id: (nullable): the session ID
 *
 * Sets the session ID for conversation continuity.
 */
void
ai_cli_client_set_session_id(
    AiCliClient *self,
    const gchar *session_id
){
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    g_clear_pointer(&priv->session_id, g_free);
    priv->session_id = g_strdup(session_id);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SESSION_ID]);
}

/**
 * ai_cli_client_get_session_persistence:
 * @self: an #AiCliClient
 *
 * Gets whether session persistence is enabled.
 *
 * Returns: %TRUE if session persistence is enabled
 */
gboolean
ai_cli_client_get_session_persistence(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), FALSE);

    priv = ai_cli_client_get_instance_private(self);
    return priv->session_persistence;
}

/**
 * ai_cli_client_set_session_persistence:
 * @self: an #AiCliClient
 * @persist: whether to persist sessions
 *
 * Sets whether to persist sessions for conversation continuity.
 */
void
ai_cli_client_set_session_persistence(
    AiCliClient *self,
    gboolean     persist
){
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    priv->session_persistence = persist;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SESSION_PERSISTENCE]);
}

/**
 * ai_cli_client_get_working_directory:
 * @self: an #AiCliClient
 *
 * Gets the working directory for the CLI subprocess.
 *
 * Returns: (transfer none) (nullable): the working directory path
 */
const gchar *
ai_cli_client_get_working_directory(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    priv = ai_cli_client_get_instance_private(self);
    return priv->working_directory;
}

/**
 * ai_cli_client_set_working_directory:
 * @self: an #AiCliClient
 * @directory: (nullable): the working directory path, or %NULL to inherit
 *
 * Sets the working directory for the CLI subprocess. When set, the
 * subprocess will be spawned with this as its current working directory.
 * When %NULL, the subprocess inherits the parent process working directory.
 */
void
ai_cli_client_set_working_directory(
    AiCliClient *self,
    const gchar *directory
)
{
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    g_clear_pointer(&priv->working_directory, g_free);
    priv->working_directory = g_strdup(directory);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WORKING_DIRECTORY]);
}

/**
 * ai_cli_client_get_effort_level:
 * @self: an #AiCliClient
 *
 * Gets the reasoning effort level.
 *
 * Returns: (transfer none) (nullable): the effort level string
 */
const gchar *
ai_cli_client_get_effort_level(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), "medium");

    priv = ai_cli_client_get_instance_private(self);
    return priv->effort_level;
}

/**
 * ai_cli_client_set_effort_level:
 * @self: an #AiCliClient
 * @effort_level: (nullable): the effort level
 *   (low/medium/high/xhigh/max), or %NULL to reset to default (medium)
 *
 * Sets the reasoning effort level. Maps to --effort for Claude Code
 * and --variant for OpenCode.
 */
void
ai_cli_client_set_effort_level(
    AiCliClient *self,
    const gchar *effort_level
)
{
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    priv = ai_cli_client_get_instance_private(self);
    g_clear_pointer(&priv->effort_level, g_free);
    priv->effort_level = g_strdup(effort_level != NULL ? effort_level : "medium");

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_EFFORT_LEVEL]);
}

/**
 * ai_cli_client_get_process_timeout_ms:
 * @self: an #AiCliClient
 *
 * Returns: the wall-clock deadline (in ms) applied to one synchronous
 *   CLI subprocess run, or 0 when disabled.  Default 1800000 (30 min).
 *
 * Since: 0.23.3
 */
gint
ai_cli_client_get_process_timeout_ms(AiCliClient *self)
{
    AiCliClientPrivate *priv;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), 0);

    priv = ai_cli_client_get_instance_private(self);
    return priv->process_timeout_ms;
}

/**
 * ai_cli_client_set_process_timeout_ms:
 * @self: an #AiCliClient
 * @timeout_ms: deadline in milliseconds; 0 disables
 *
 * Bounds one synchronous CLI subprocess run.  On expiry the child is
 * killed and the call fails with %AI_ERROR_TIMEOUT.  Providers that
 * override chat_sync with their own orchestration (the tmux client)
 * bound their turns with their own knobs instead.
 *
 * Since: 0.23.3
 */
void
ai_cli_client_set_process_timeout_ms(
    AiCliClient *self,
    gint         timeout_ms
)
{
    AiCliClientPrivate *priv;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));
    g_return_if_fail(timeout_ms >= 0);

    priv = ai_cli_client_get_instance_private(self);
    priv->process_timeout_ms = timeout_ms;

    g_object_notify_by_pspec(G_OBJECT(self),
                             properties[PROP_PROCESS_TIMEOUT_MS]);
}

/**
 * ai_cli_client_resolve_executable:
 * @self: an #AiCliClient
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves the CLI executable path. Uses the subclass virtual method
 * to get the executable name, then checks for environment variable
 * overrides before searching PATH.
 *
 * Returns: (transfer full) (nullable): the resolved path, or %NULL on error
 */
gchar *
ai_cli_client_resolve_executable(
    AiCliClient  *self,
    GError      **error
){
    AiCliClientClass *klass;
    AiCliClientPrivate *priv;
    g_autofree gchar *cli_path = NULL;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    klass = AI_CLI_CLIENT_GET_CLASS(self);
    priv = ai_cli_client_get_instance_private(self);

    /* Check explicit path override first */
    if (priv->executable_path != NULL && priv->executable_path[0] != '\0')
    {
        if (g_file_test(priv->executable_path, G_FILE_TEST_IS_EXECUTABLE))
        {
            return g_strdup(priv->executable_path);
        }
        else
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND,
                        "Specified CLI executable not found: %s",
                        priv->executable_path);
            return NULL;
        }
    }

    /* Ask subclass for executable path (may check env vars) */
    if (klass->get_executable_path != NULL)
    {
        cli_path = klass->get_executable_path(self);
    }

    if (cli_path == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND,
                    "CLI executable path not configured");
        return NULL;
    }

    /* If it's an absolute path, verify it exists */
    if (g_path_is_absolute(cli_path))
    {
        if (g_file_test(cli_path, G_FILE_TEST_IS_EXECUTABLE))
        {
            return g_steal_pointer(&cli_path);
        }
        else
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND,
                        "CLI executable not found: %s", cli_path);
            return NULL;
        }
    }

    /* Search PATH for the executable */
    {
        g_autofree gchar *found_path = g_find_program_in_path(cli_path);
        if (found_path != NULL)
        {
            return g_steal_pointer(&found_path);
        }
    }

    g_set_error(error, AI_ERROR, AI_ERROR_CLI_NOT_FOUND,
                "CLI executable '%s' not found in PATH", cli_path);
    return NULL;
}

/**
 * ai_cli_client_chat_sync:
 * @self: an #AiCliClient
 * @messages: (element-type AiMessage): the conversation messages
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Performs a synchronous chat completion request via the CLI.
 * Spawns the CLI subprocess, waits for completion, and parses the output.
 *
 * Returns: (transfer full) (nullable): the #AiResponse, or %NULL on error
 */
AiResponse *
ai_cli_client_chat_sync(
    AiCliClient   *self,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
){
    AiCliClientClass *klass;
    AiCliClientPrivate *priv;
    g_autofree gchar *executable = NULL;
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *stdin_data = NULL;
    g_autoptr(GSubprocess) subprocess = NULL;
    g_autofree gchar *stdout_data = NULL;
    g_autofree gchar *stderr_data = NULL;
    AiResponse *response;
    GSubprocessFlags flags;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);

    klass = AI_CLI_CLIENT_GET_CLASS(self);
    priv = ai_cli_client_get_instance_private(self);

    /*
     * If a subclass installs its own chat_sync, defer to it entirely.
     * Used by AiClaudeTmuxClient whose flow (tmux + Stop hook) bears
     * no resemblance to the default argv/spawn/parse pipeline.
     */
    if (klass->chat_sync != NULL)
    {
        return klass->chat_sync(self, messages, cancellable, error);
    }

    g_return_val_if_fail(klass->build_argv != NULL, NULL);
    g_return_val_if_fail(klass->parse_json_output != NULL, NULL);

    /* Resolve executable path */
    executable = ai_cli_client_resolve_executable(self, error);
    if (executable == NULL)
    {
        return NULL;
    }

    /* Build command line arguments */
    argv = klass->build_argv(self, messages, priv->system_prompt,
                             priv->max_tokens, FALSE);
    if (argv == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "Failed to build command line arguments");
        return NULL;
    }

    /* Build stdin data if subclass provides it (e.g. for large prompts) */
    if (klass->build_stdin != NULL)
    {
        stdin_data = klass->build_stdin(self, messages);
    }

    /* Replace first element with resolved executable path */
    g_free(argv[0]);
    argv[0] = g_steal_pointer(&executable);

    /* Spawn subprocess — add STDIN_PIPE if we have stdin data */
    flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    if (stdin_data != NULL)
    {
        flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
    }

    /*
     * Through the vfunc, so a subclass that needs more than a working
     * directory -- opencode wants OPENCODE_PERMISSION in the environment --
     * gets it on the synchronous path too, not only when streaming.
     */
    subprocess = ai_cli_client_spawn(self, (const gchar * const *)argv,
                                     flags, error);

    if (subprocess == NULL)
    {
        return NULL;
    }

    /*
     * Wait for completion and capture output — bounded.  The deadline
     * (and the caller's cancellable) kill the child on expiry, so a
     * CLI wedged on a dead network connection can never pin this
     * thread forever; before this bound, one such hang froze a
     * libreclaw session permanently.
     */
    if (!ai_subprocess_communicate_utf8_bounded(subprocess,
                                                stdin_data,
                                                priv->process_timeout_ms,
                                                cancellable,
                                                &stdout_data,
                                                &stderr_data,
                                                error))
    {
        return NULL;
    }

    /* Check exit status */
    if (!g_subprocess_get_successful(subprocess))
    {
        gint              exit_status;
        g_autofree gchar *msg = NULL;

        /*
         * A failing CLI usually explains itself in its own output format
         * before it exits -- opencode prints a structured error event on
         * stdout with stderr empty, and grok does much the same. Give the
         * subclass's parser first refusal at turning that into a sentence;
         * reporting the exit status over the top of it hands the caller
         * raw JSON, or an empty string, where a message belongs.
         *
         * Only the parser's *error* is taken. A non-zero exit means the
         * run failed, whatever it managed to print.
         */
        if (stdout_data != NULL && stdout_data[0] != '\0' &&
            klass->parse_json_output != NULL)
        {
            g_autoptr(GError) parse_error = NULL;
            g_autoptr(AiResponse) parsed = NULL;

            parsed = klass->parse_json_output(self, stdout_data,
                                              &parse_error);

            if (parsed == NULL && parse_error != NULL &&
                parse_error->domain == AI_ERROR &&
                parse_error->code == AI_ERROR_CLI_EXECUTION)
            {
                g_propagate_error(error, g_steal_pointer(&parse_error));
                return NULL;
            }
        }

        exit_status = g_subprocess_get_exit_status(subprocess);
        msg = ai_cli_client_format_exit_error(exit_status,
                                              stderr_data,
                                              stdout_data);
        g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, msg);
        return NULL;
    }

    /* Parse output */
    if (stdout_data == NULL || stdout_data[0] == '\0')
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                    "CLI produced no output");
        return NULL;
    }

    response = klass->parse_json_output(self, stdout_data, error);
    return response;
}

/*
 * Default spawn: honour "working-directory" and nothing else.
 *
 * A subclass overrides this only to add something to the launcher --- see
 * AiOpenCodeClient, which sets OPENCODE_PERMISSION alongside --auto. Before
 * this vfunc existed each wrapper carried its own near-identical copy, and
 * the base's own chat_sync carried a fourth.
 */
static GSubprocess *
ai_cli_client_real_spawn(
    AiCliClient          *self,
    const gchar * const  *argv,
    GSubprocessFlags      flags,
    GError              **error
){
    const gchar *cwd;

    cwd = ai_cli_client_get_working_directory(self);

    if (cwd != NULL && cwd[0] != '\0')
    {
        g_autoptr(GSubprocessLauncher) launcher = NULL;

        launcher = g_subprocess_launcher_new(flags);
        g_subprocess_launcher_set_cwd(launcher, cwd);

        return g_subprocess_launcher_spawnv(launcher, argv, error);
    }

    return g_subprocess_newv(argv, flags, error);
}

/**
 * ai_cli_client_events_to_delta:
 * @events: (nullable) (element-type AiEvent): events from one parsed line
 *
 * Concatenates the text of every %AI_EVENT_TEXT_DELTA in @events.
 *
 * This is the inverse of the default @parse_stream_events, and it is what
 * lets a subclass implement the richer vfunc as its single source of truth
 * while still answering the older @parse_stream_line contract:
 *
 * |[<!-- language="C" -->
 * static gboolean
 * my_parse_stream_line (AiCliClient *c, const gchar *line, AiResponse *r,
 *                       gchar **delta_text, GError **error)
 * {
 *     g_autoptr(GPtrArray) events =
 *         g_ptr_array_new_with_free_func ((GDestroyNotify) ai_event_unref);
 *
 *     if (!my_parse_stream_events (c, line, r, events, error))
 *         return FALSE;
 *
 *     *delta_text = ai_cli_client_events_to_delta (events);
 *     return TRUE;
 * }
 * ]|
 *
 * Reasoning is deliberately excluded: the old contract's caller treats what
 * it gets back as the answer.
 *
 * Returns: (transfer full) (nullable): the concatenated text, or %NULL when
 *   there was none
 */
gchar *
ai_cli_client_events_to_delta(GPtrArray *events)
{
    GString *acc;
    guint i;

    if (events == NULL || events->len == 0)
    {
        return NULL;
    }

    acc = g_string_new(NULL);

    for (i = 0; i < events->len; i++)
    {
        AiEvent *event = g_ptr_array_index(events, i);

        if (ai_event_get_kind(event) == AI_EVENT_TEXT_DELTA)
        {
            const gchar *text = ai_event_get_text(event);

            if (text != NULL)
            {
                g_string_append(acc, text);
            }
        }
    }

    if (acc->len == 0)
    {
        g_string_free(acc, TRUE);
        return NULL;
    }

    return g_string_free(acc, FALSE);
}

/**
 * ai_cli_client_spawn:
 * @self: an #AiCliClient
 * @argv: (array zero-terminated=1): the command line, argv[0] already resolved
 * @flags: subprocess flags
 * @error: (out) (optional): return location for a #GError
 *
 * Launches the CLI through the @spawn vfunc.
 *
 * Returns: (transfer full) (nullable): the child, or %NULL on error
 */
GSubprocess *
ai_cli_client_spawn(
    AiCliClient          *self,
    const gchar * const  *argv,
    GSubprocessFlags      flags,
    GError              **error
){
    AiCliClientClass *klass;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);
    g_return_val_if_fail(argv != NULL, NULL);

    klass = AI_CLI_CLIENT_GET_CLASS(self);
    g_return_val_if_fail(klass->spawn != NULL, NULL);

    return klass->spawn(self, argv, flags, error);
}

/*
 * Default parse_stream_events: delegate to the older parse_stream_line and
 * wrap whatever delta it produced in a text event.
 *
 * This is what keeps a subclass that has not been migrated working. It can
 * only ever report text, which is exactly how every CLI wrapper behaved
 * before events existed, so the fallback is a faithful one.
 */
static gboolean
ai_cli_client_real_parse_stream_events(
    AiCliClient  *self,
    const gchar  *line,
    AiResponse   *response,
    GPtrArray    *out_events,
    GError      **error
){
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(self);
    g_autofree gchar *delta_text = NULL;

    if (klass->parse_stream_line == NULL)
    {
        return TRUE;
    }

    if (!klass->parse_stream_line(self, line, response, &delta_text, error))
    {
        return FALSE;
    }

    if (delta_text != NULL && delta_text[0] != '\0')
    {
        g_ptr_array_add(out_events, ai_event_new_text_delta(delta_text));
    }

    return TRUE;
}

/*
 * State for one streaming run.
 *
 * This used to be duplicated, near-verbatim, in the claude-code, opencode
 * and grok-build clients. They had already drifted: only grok-build failed
 * the task on a mid-stream parse error, and none of the three enforced
 * "process-timeout-ms" the way the synchronous path does. Both behaviours
 * are now uniform because there is only one copy left.
 */
typedef struct
{
    AiCliClient      *client;
    GTask            *task;
    GSubprocess      *subprocess;
    GDataInputStream *data_stream;
    GCancellable     *cancellable;
    AiResponse       *response;
    GString          *accumulated_text;
    gboolean          stream_started;
    gboolean          finished;

    /*
     * Whether a read_line_async is outstanding, which decides who frees
     * this. The deadline can complete the task while a read is still in
     * flight; freeing here would leave that read's callback holding a
     * dangling pointer, so it is left to the callback instead.
     */
    gboolean          read_pending;

    gchar            *stdin_data;
    guint             timeout_id;
} StreamRun;

static void stream_run_read_next(StreamRun *run);

static void
stream_run_free(StreamRun *run)
{
    if (run->timeout_id != 0)
    {
        g_source_remove(run->timeout_id);
        run->timeout_id = 0;
    }

    g_clear_object(&run->task);
    g_clear_object(&run->client);
    g_clear_object(&run->subprocess);
    g_clear_object(&run->data_stream);
    g_clear_object(&run->cancellable);
    g_clear_object(&run->response);
    g_clear_pointer(&run->stdin_data, g_free);

    if (run->accumulated_text != NULL)
    {
        g_string_free(run->accumulated_text, TRUE);
    }

    g_slice_free(StreamRun, run);
}

/*
 * Complete the task exactly once.
 *
 * The deadline, a read error and EOF can all reach for the task, and racing
 * them would return it twice. Taking @error means the caller can hand over a
 * failure without also having to remember to free it.
 *
 * Freeing is conditional on there being no outstanding read. The deadline
 * fires while one is always in flight -- that is what it is waiting on --
 * and force-exiting the child makes that read complete with EOF moments
 * later. Freeing here would hand that callback a dangling pointer, so the
 * callback frees instead, having seen @finished already set.
 */
static void
stream_run_finish_once(
    StreamRun  *run,
    AiResponse *response,
    GError     *error
){
    if (run->finished)
    {
        g_clear_error(&error);
        g_clear_object(&response);
        return;
    }

    run->finished = TRUE;

    if (error != NULL)
    {
        g_task_return_error(run->task, error);
        g_clear_object(&response);
    }
    else
    {
        g_task_return_pointer(run->task, response, g_object_unref);
    }

    if (!run->read_pending)
    {
        stream_run_free(run);
    }
}

/*
 * The wall-clock deadline for one streaming run.
 *
 * The synchronous path has always enforced "process-timeout-ms"; the three
 * streaming readers never did, so a CLI blocked on a half-open connection
 * would hold the stream open indefinitely. The child is killed so it cannot
 * outlive the task that was waiting on it.
 */
static gboolean
on_stream_timeout(gpointer user_data)
{
    StreamRun *run = user_data;
    GError *error;

    run->timeout_id = 0;

    error = g_error_new(AI_ERROR, AI_ERROR_TIMEOUT,
                        "CLI process exceeded its %d ms deadline",
                        ai_cli_client_get_process_timeout_ms(run->client));

    g_subprocess_force_exit(run->subprocess);
    stream_run_finish_once(run, NULL, error);

    return G_SOURCE_REMOVE;
}

/*
 * Turn one parsed event into the side effects a subscriber expects.
 *
 * Every event goes out on ::event. Text, tool use and the stream boundaries
 * additionally fire the older signals, so code written against "delta"
 * keeps working -- and "tool-use", which was declared on both AiCliClient
 * and AiStreamable but emitted by nothing, finally fires.
 */
static void
stream_run_dispatch_event(
    StreamRun *run,
    AiEvent   *event
){
    AiEventKind kind = ai_event_get_kind(event);
    const gchar *text;

    if (!run->stream_started &&
        (kind == AI_EVENT_TEXT_DELTA || kind == AI_EVENT_STREAM_START))
    {
        run->stream_started = TRUE;
        g_signal_emit(run->client, signals[SIGNAL_STREAM_START], 0);
    }

    ai_event_source_emit(AI_EVENT_SOURCE(run->client), event);

    switch (kind)
    {
        case AI_EVENT_TEXT_DELTA:
            text = ai_event_get_text(event);

            if (text != NULL && text[0] != '\0')
            {
                g_string_append(run->accumulated_text, text);
                g_signal_emit(run->client, signals[SIGNAL_DELTA], 0, text);
            }
            break;

        case AI_EVENT_TOOL_STARTED:
            if (ai_event_get_tool_use(event) != NULL)
            {
                g_signal_emit(run->client, signals[SIGNAL_TOOL_USE], 0,
                              ai_event_get_tool_use(event));
            }
            break;

        default:
            break;
    }
}

/*
 * EOF: the child closed stdout, so the turn is over.
 *
 * The accumulated text becomes a content block only when the parser did not
 * already add one. A parser that assembled the response itself -- from a
 * "result" envelope, say -- must not have the deltas appended a second time.
 */
static void
stream_run_complete(StreamRun *run)
{
    g_autoptr(AiEvent) end_event = NULL;

    if (run->response == NULL)
    {
        stream_run_finish_once(run, NULL,
            g_error_new_literal(AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                "Stream ended without a valid response"));
        return;
    }

    if (run->accumulated_text->len > 0 &&
        ai_response_get_content_blocks(run->response) == NULL)
    {
        g_autoptr(AiTextContent) content =
            ai_text_content_new(run->accumulated_text->str);

        ai_response_add_content_block(run->response,
            (AiContentBlock *)g_steal_pointer(&content));
    }

    end_event = ai_event_new(AI_EVENT_STREAM_END);
    ai_event_source_emit(AI_EVENT_SOURCE(run->client), end_event);

    g_signal_emit(run->client, signals[SIGNAL_STREAM_END], 0, run->response);

    stream_run_finish_once(run, g_object_ref(run->response), NULL);
}

static void
on_stream_line_read(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    StreamRun *run = user_data;
    g_autoptr(GPtrArray) events = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = NULL;
    AiCliClientClass *klass;
    gboolean (*parse) (AiCliClient *, const gchar *, AiResponse *,
                       GPtrArray *, GError **);
    gsize length;
    guint i;

    (void)source;

    run->read_pending = FALSE;

    /*
     * The task was already completed -- by the deadline, which then killed
     * the child, producing the EOF or error being reported now. This
     * callback is the last thing holding the run, so it does the freeing.
     */
    if (run->finished)
    {
        stream_run_free(run);
        return;
    }

    line = g_data_input_stream_read_line_finish(run->data_stream, result,
                                                &length, &error);

    if (error != NULL)
    {
        stream_run_finish_once(run, NULL, g_steal_pointer(&error));
        return;
    }

    if (line == NULL)
    {
        stream_run_complete(run);
        return;
    }

    events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
    klass = AI_CLI_CLIENT_GET_CLASS(run->client);

    /*
     * NULL is a documented value for this vfunc, so honour it rather than
     * trusting that class_init's default survived: a subclass may clear the
     * slot deliberately to opt back into the text-only contract.
     */
    parse = klass->parse_stream_events != NULL
        ? klass->parse_stream_events
        : ai_cli_client_real_parse_stream_events;

    if (!parse(run->client, line, run->response, events, &error))
    {
        /*
         * The CLI reported an error mid-stream. Fail rather than reading on
         * to EOF and returning the empty response that would produce.
         */
        if (error == NULL)
        {
            error = g_error_new_literal(AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                                        "Failed to parse CLI stream output");
        }

        stream_run_finish_once(run, NULL, g_steal_pointer(&error));
        return;
    }

    for (i = 0; i < events->len; i++)
    {
        stream_run_dispatch_event(run, (AiEvent *)g_ptr_array_index(events, i));
    }

    stream_run_read_next(run);
}

static void
stream_run_read_next(StreamRun *run)
{
    run->read_pending = TRUE;

    g_data_input_stream_read_line_async(
        run->data_stream,
        G_PRIORITY_DEFAULT,
        run->cancellable,
        on_stream_line_read,
        run);
}

/*
 * Write the prompt into the child and start reading its stdout.
 *
 * The stdin pipe is closed straight after writing: a CLI reading the prompt
 * back through --prompt-file /dev/stdin needs the EOF to know it is
 * complete, and would otherwise wait forever for more.
 */
static void
stream_run_start(StreamRun *run)
{
    g_autoptr(GError) error = NULL;
    GInputStream *stdout_stream;
    GOutputStream *stdin_stream;
    gint timeout_ms;

    if (run->stdin_data != NULL)
    {
        stdin_stream = g_subprocess_get_stdin_pipe(run->subprocess);

        if (stdin_stream != NULL)
        {
            g_output_stream_write_all(stdin_stream,
                                      run->stdin_data,
                                      strlen(run->stdin_data),
                                      NULL, NULL, &error);
            g_output_stream_close(stdin_stream, NULL, NULL);
        }

        if (error != NULL)
        {
            stream_run_finish_once(run, NULL, g_steal_pointer(&error));
            return;
        }
    }

    stdout_stream = g_subprocess_get_stdout_pipe(run->subprocess);

    if (stdout_stream == NULL)
    {
        stream_run_finish_once(run, NULL,
            g_error_new_literal(AI_ERROR, AI_ERROR_CLI_EXECUTION,
                                "Failed to get subprocess stdout"));
        return;
    }

    run->data_stream = g_data_input_stream_new(stdout_stream);
    g_data_input_stream_set_newline_type(run->data_stream,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);

    /*
     * No line-length cap is needed: g_data_input_stream_read_line() doubles
     * its buffer until it finds the newline, and a single assistant message
     * carrying a large content array routinely exceeds the 4 KiB default.
     */

    run->response = ai_response_new("", ai_cli_client_get_model(run->client));
    run->accumulated_text = g_string_new("");

    timeout_ms = ai_cli_client_get_process_timeout_ms(run->client);

    if (timeout_ms > 0)
    {
        /*
         * Attached to the thread-default context, not the global default
         * g_timeout_add() would use: a caller driving this from a nested
         * loop on a private context would never see a global-default timer.
         */
        GSource *source = g_timeout_source_new(timeout_ms);

        g_source_set_callback(source, on_stream_timeout, run, NULL);
        run->timeout_id = g_source_attach(source,
                                          g_main_context_get_thread_default());
        g_source_unref(source);
    }

    stream_run_read_next(run);
}

/**
 * ai_cli_client_stream_run_async:
 * @self: an #AiCliClient
 * @messages: (element-type AiMessage): the conversation messages
 * @system_prompt: (nullable): system prompt to use
 * @max_tokens: maximum tokens to generate
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the stream ends
 * @user_data: user data for @callback
 *
 * Spawns the CLI in streaming mode and reads its NDJSON output line by line,
 * translating each line through the @parse_stream_events vfunc and
 * publishing the results.
 *
 * This is the whole streaming implementation for a CLI provider. A subclass
 * implements #AiStreamable by forwarding to it:
 *
 * |[<!-- language="C" -->
 * static void
 * my_client_chat_stream_async (AiStreamable *streamable, GList *messages,
 *                              const gchar *system_prompt, gint max_tokens,
 *                              GList *tools, GCancellable *cancellable,
 *                              GAsyncReadyCallback callback, gpointer user_data)
 * {
 *     ai_cli_client_stream_run_async (AI_CLI_CLIENT (streamable), messages,
 *                                     system_prompt, max_tokens,
 *                                     cancellable, callback, user_data);
 * }
 * ]|
 *
 * Along the way it emits #AiEventSource::event for every event, and the
 * older #AiCliClient::delta, ::stream-start, ::stream-end and ::tool-use
 * signals for the ones those cover.
 */
void
ai_cli_client_stream_run_async(
    AiCliClient         *self,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiCliClientClass *klass;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *executable = NULL;
    g_auto(GStrv) argv = NULL;
    g_autoptr(GSubprocess) subprocess = NULL;
    gchar *stdin_data = NULL;
    StreamRun *run;
    GTask *task;
    GSubprocessFlags flags;

    g_return_if_fail(AI_IS_CLI_CLIENT(self));

    klass = AI_CLI_CLIENT_GET_CLASS(self);
    task = g_task_new(self, cancellable, callback, user_data);

    executable = ai_cli_client_resolve_executable(self, &error);
    if (executable == NULL)
    {
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    if (klass->build_argv == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "%s does not implement build_argv",
                                G_OBJECT_TYPE_NAME(self));
        g_object_unref(task);
        return;
    }

    argv = klass->build_argv(self, messages, system_prompt, max_tokens, TRUE);
    if (argv == NULL)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "Failed to build command line arguments");
        g_object_unref(task);
        return;
    }

    if (klass->build_stdin != NULL)
    {
        stdin_data = klass->build_stdin(self, messages);
    }

    /* build_argv leaves a placeholder in argv[0]; the resolved path wins. */
    g_free(argv[0]);
    argv[0] = g_steal_pointer(&executable);

    flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    if (stdin_data != NULL)
    {
        flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
    }

    subprocess = ai_cli_client_spawn(self, (const gchar * const *)argv,
                                     flags, &error);
    if (subprocess == NULL)
    {
        g_free(stdin_data);
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    run = g_slice_new0(StreamRun);
    run->client = g_object_ref(self);
    run->task = task;
    run->subprocess = g_object_ref(subprocess);
    run->cancellable = cancellable != NULL ? g_object_ref(cancellable) : NULL;
    run->stdin_data = stdin_data;   /* ownership transferred */

    stream_run_start(run);
}

/**
 * ai_cli_client_stream_run_finish:
 * @self: an #AiCliClient
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes an ai_cli_client_stream_run_async() call.
 *
 * Returns: (transfer full) (nullable): the assembled #AiResponse, or %NULL
 */
AiResponse *
ai_cli_client_stream_run_finish(
    AiCliClient   *self,
    GAsyncResult  *result,
    GError       **error
){
    g_return_val_if_fail(AI_IS_CLI_CLIENT(self), NULL);
    g_return_val_if_fail(g_task_is_valid(result, self), NULL);

    return g_task_propagate_pointer(G_TASK(result), error);
}
