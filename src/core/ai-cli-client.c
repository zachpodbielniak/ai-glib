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
    gint      max_tokens;
    gboolean  session_persistence;
} AiCliClientPrivate;

G_DEFINE_TYPE_WITH_PRIVATE(AiCliClient, ai_cli_client, G_TYPE_OBJECT)

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
    priv->session_persistence = TRUE;
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

    subprocess = g_subprocess_newv((const gchar * const *)argv,
                                   flags, error);
    if (subprocess == NULL)
    {
        return NULL;
    }

    /* Wait for completion and capture output */
    if (!g_subprocess_communicate_utf8(subprocess,
                                       stdin_data,  /* pipe prompt via stdin */
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
        gint exit_status = g_subprocess_get_exit_status(subprocess);
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                    "CLI exited with status %d: %s",
                    exit_status,
                    stderr_data != NULL ? stderr_data : "Unknown error");
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
