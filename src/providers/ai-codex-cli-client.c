/*
 * ai-codex-cli-client.c - Codex exec JSONL provider
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "config.h"
#include <string.h>
#include <math.h>
#include "providers/ai-codex-cli-client.h"
#include "core/ai-cli-client-private.h"
#include "core/ai-json-util.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-result.h"

struct _AiCodexCliClient
{
    AiCliClient parent_instance;
    gchar *sandbox;
    gchar *additional_directories;
    gchar *profile;
    gchar *turn_system_prompt;
    gboolean skip_permissions;
    gboolean continue_session;
    gboolean search;
};

static void provider_init(AiProviderInterface *iface);
static void streamable_init(AiStreamableInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiCodexCliClient, ai_codex_cli_client, AI_TYPE_CLI_CLIENT,
    G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER, provider_init)
    G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE, streamable_init))

enum { PROP_0, PROP_SANDBOX, PROP_SKIP_PERMISSIONS, PROP_CONTINUE_SESSION,
       PROP_ADDITIONAL_DIRECTORIES, PROP_PROFILE, PROP_SEARCH, N_PROPS };
static GParamSpec *properties[N_PROPS];

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(object);
    switch (id)
    {
        case PROP_SANDBOX: g_value_set_string(value, self->sandbox); break;
        case PROP_ADDITIONAL_DIRECTORIES: g_value_set_string(value, self->additional_directories); break;
        case PROP_PROFILE: g_value_set_string(value, self->profile); break;
        case PROP_SKIP_PERMISSIONS: g_value_set_boolean(value, self->skip_permissions); break;
        case PROP_CONTINUE_SESSION: g_value_set_boolean(value, self->continue_session); break;
        case PROP_SEARCH: g_value_set_boolean(value, self->search); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    }
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(object);
    switch (id)
    {
        case PROP_SANDBOX:
            g_free(self->sandbox); self->sandbox = g_value_dup_string(value); break;
        case PROP_ADDITIONAL_DIRECTORIES:
            g_free(self->additional_directories); self->additional_directories = g_value_dup_string(value); break;
        case PROP_PROFILE:
            g_free(self->profile); self->profile = g_value_dup_string(value); break;
        case PROP_SKIP_PERMISSIONS: self->skip_permissions = g_value_get_boolean(value); break;
        case PROP_CONTINUE_SESSION: self->continue_session = g_value_get_boolean(value); break;
        case PROP_SEARCH: self->search = g_value_get_boolean(value); break;
        default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
    }
}

static gchar *
get_executable(AiCliClient *client)
{
    const gchar *path = g_getenv("CODEX_PATH");
    (void)client;
    return g_strdup(path != NULL && path[0] != '\0' ? path : "codex");
}

static void
arg(GPtrArray *args, const gchar *value)
{
    g_ptr_array_add(args, g_strdup(value));
}

static gchar **
build_argv(AiCliClient *client, GList *messages, const gchar *system_prompt,
           gint max_tokens, gboolean streaming)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(client);
    g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
    const gchar *session = ai_cli_client_get_session_id(client);
    const gchar *effort = ai_cli_client_get_effort_level(client);
    gboolean persist = ai_cli_client_get_session_persistence(client);
    gboolean resume = persist && ((session != NULL && *session) || self->continue_session);
    (void)messages; (void)max_tokens; (void)streaming;

    /* Invalid policy must fail closed, including when set through GObject. */
    if (self->sandbox != NULL &&
        g_strcmp0(self->sandbox, "read-only") != 0 &&
        g_strcmp0(self->sandbox, "workspace-write") != 0 &&
        g_strcmp0(self->sandbox, "danger-full-access") != 0)
        return NULL;
    if (self->skip_permissions && self->sandbox != NULL &&
        g_strcmp0(self->sandbox, "danger-full-access") != 0)
        return NULL;

    g_free(self->turn_system_prompt);
    self->turn_system_prompt = g_strdup(system_prompt);
    arg(args, "codex");
    /* Global flags precede exec; exec-only flags precede resume. */
    arg(args, "--ask-for-approval"); arg(args, "never");
    if (self->search) arg(args, "--search");
    if (self->profile != NULL && *self->profile)
    { arg(args, "--profile"); arg(args, self->profile); }
    arg(args, "exec");
    arg(args, "--json");
    arg(args, "--skip-git-repo-check");
    if (self->skip_permissions)
        arg(args, "--dangerously-bypass-approvals-and-sandbox");
    else
    {
        arg(args, "--sandbox");
        arg(args, self->sandbox != NULL ? self->sandbox : "read-only");
    }
    if (!persist) arg(args, "--ephemeral");
    arg(args, "--model"); arg(args, ai_cli_client_get_model(client));
    if (effort != NULL && *effort)
    {
        /* Known values only: this value is TOML, not a shell argument. */
        const gchar *valid[] = { "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", NULL };
        if (!g_strv_contains(valid, effort)) return NULL;
        arg(args, "-c");
        g_ptr_array_add(args, g_strdup_printf("model_reasoning_effort=\"%s\"", effort));
    }
    if (self->additional_directories != NULL)
    {
        g_auto(GStrv) dirs = g_strsplit(self->additional_directories, ",", -1);
        guint i;
        for (i = 0; dirs[i] != NULL; i++)
        {
            g_strstrip(dirs[i]);
            if (*dirs[i]) { arg(args, "--add-dir"); arg(args, dirs[i]); }
        }
    }
    if (resume)
    {
        arg(args, "resume");
        if (session != NULL && *session) arg(args, session);
        else arg(args, "--last");
    }
    arg(args, "-");
    g_ptr_array_add(args, NULL);
    return (gchar **)g_ptr_array_free(g_steal_pointer(&args), FALSE);
}

static gchar *
build_stdin(AiCliClient *client, GList *messages)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(client);
    GString *prompt = g_string_new(NULL);
    const gchar *system = self->turn_system_prompt;
    const gchar *session = ai_cli_client_get_session_id(client);
    GList *l;
    if (system == NULL) system = ai_cli_client_get_system_prompt(client);
    if (system != NULL && *system &&
        (!ai_cli_client_get_session_persistence(client) ||
         ((session == NULL || !*session) && !self->continue_session)))
        g_string_append_printf(prompt, "<system>\n%s\n</system>\n\n", system);
    messages = ai_cli_client_messages_for_prompt(client, messages);
    for (l = messages; l != NULL; l = l->next)
    {
        g_autofree gchar *text = ai_cli_client_project_message(l->data);
        if (text != NULL && *text)
        {
            if (prompt->len > 0) g_string_append(prompt, "\n\n");
            g_string_append(prompt, text);
        }
    }
    return g_string_free(prompt, FALSE);
}

/* Clamp before converting floating-point counters to an integer. */
static gint
usage_tokens(JsonObject *usage, const gchar *name)
{
    gdouble count = ai_json_get_double(usage, name, 0);
    if (isnan(count) || count <= 0) return 0;
    if (count >= G_MAXINT) return G_MAXINT;
    return (gint)count;
}

static gboolean
parse_events(AiCliClient *client, const gchar *line, AiResponse *response,
             GPtrArray *events, GError **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *obj, *item;
    const gchar *type, *kind;
    while (line != NULL && g_ascii_isspace(*line)) line++;
    if (line == NULL || !*line) return TRUE;
    if (!json_parser_load_from_data(parser, line, -1, NULL)) goto malformed;
    obj = ai_json_root_object(parser);
    type = ai_json_get_string(obj, "type", NULL);
    if (type == NULL) goto malformed;
    if (g_str_equal(type, "thread.started"))
    {
        const gchar *id = ai_json_get_string(obj, "thread_id", NULL);
        if (id == NULL || !*id) goto malformed;
        if (ai_cli_client_get_session_persistence(client))
            ai_cli_client_set_session_id(client, id);
    }
    else if (g_str_equal(type, "turn.started"))
    {
        ai_response_set_stop_reason(response, AI_STOP_REASON_NONE);
        g_ptr_array_add(events, ai_event_new(AI_EVENT_STREAM_START));
    }
    else if (g_str_equal(type, "turn.failed") || g_str_equal(type, "error"))
    {
        const gchar *message = ai_json_get_string(obj, "message", NULL);
        if (message == NULL)
            message = ai_json_get_string(ai_json_get_object(obj, "error"), "message", "Codex turn failed");
        g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "%s", message);
        return FALSE;
    }
    else if (g_str_equal(type, "turn.completed"))
    {
        JsonObject *u = ai_json_get_object(obj, "usage");
        if (u != NULL)
        {
            g_autoptr(AiUsage) usage = ai_usage_new(
                usage_tokens(u, "input_tokens"),
                usage_tokens(u, "output_tokens"));
            ai_response_set_usage(response, usage);
            g_ptr_array_add(events, ai_event_new_usage(usage, 0));
        }
        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }
    else if (g_str_has_prefix(type, "item."))
    {
        gboolean completed = g_str_equal(type, "item.completed");
        item = ai_json_get_object(obj, "item");
        kind = ai_json_get_string(item, "type", NULL);
        if (kind == NULL) goto malformed;
        if (g_str_equal(kind, "agent_message") || g_str_equal(kind, "reasoning"))
        {
            const gchar *text = ai_json_get_string(item, "text", NULL);
            if (!completed) return TRUE; /* updates are cumulative snapshots */
            if (text == NULL) goto malformed;
            if (g_str_equal(kind, "reasoning"))
                g_ptr_array_add(events, ai_event_new_thinking_delta(text));
            else
            {
                g_autoptr(AiTextContent) block = ai_text_content_new(text);
                ai_response_add_content_block(response, AI_CONTENT_BLOCK(g_steal_pointer(&block)));
                g_ptr_array_add(events, ai_event_new_text_delta(text));
            }
        }
        else if (g_str_equal(kind, "command_execution") || g_str_equal(kind, "file_change") ||
                 g_str_equal(kind, "mcp_tool_call") || g_str_equal(kind, "web_search"))
        {
            const gchar *id = ai_json_get_string(item, "id", NULL);
            g_autoptr(AiToolUse) tool = NULL;
            if (id == NULL) goto malformed;
            tool = ai_tool_use_new(id, kind, ai_json_get_node(obj, "item"));
            if (g_str_equal(type, "item.started"))
                g_ptr_array_add(events, ai_event_new_tool_started(tool));
            if (completed)
            {
                g_autofree gchar *serialized = json_to_string(ai_json_get_node(obj, "item"), FALSE);
                const gchar *output = ai_json_get_string(item, "aggregated_output", serialized);
                gboolean failed = g_strcmp0(ai_json_get_string(item, "status", ""), "failed") == 0 ||
                                  ai_json_get_int(item, "exit_code", 0) != 0 ||
                                  ai_json_get_node(item, "error") != NULL;
                g_autoptr(AiToolResult) result = ai_tool_result_new_with_name(id, kind, output, failed);
                g_ptr_array_add(events, ai_event_new_tool_finished(tool, result));
            }
        }
        else if (g_str_equal(kind, "error"))
        {
            g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION, "%s",
                        ai_json_get_string(item, "message", "Codex item failed"));
            return FALSE;
        }
        else
        {
            g_autofree gchar *text = json_to_string(ai_json_get_node(obj, "item"), FALSE);
            g_ptr_array_add(events, ai_event_new_status(text));
        }
    }
    return TRUE;
malformed:
    g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Malformed Codex JSONL event");
    return FALSE;
}

static gboolean
parse_line(AiCliClient *client, const gchar *line, AiResponse *response,
           gchar **delta, GError **error)
{
    g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
    *delta = NULL;
    if (!parse_events(client, line, response, events, error)) return FALSE;
    *delta = ai_cli_client_events_to_delta(events);
    return TRUE;
}

static AiResponse *
parse_output(AiCliClient *client, const gchar *json, GError **error)
{
    g_autoptr(AiResponse) response = ai_response_new("", ai_cli_client_get_model(client));
    g_auto(GStrv) lines = g_strsplit(json != NULL ? json : "", "\n", -1);
    guint i;
    for (i = 0; lines[i] != NULL; i++)
    {
        g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
        if (!parse_events(client, lines[i], response, events, error)) return NULL;
    }
    if (ai_response_get_stop_reason(response) != AI_STOP_REASON_END_TURN)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR, "Codex output ended before turn.completed");
        return NULL;
    }
    return g_steal_pointer(&response);
}

static void
finalize(GObject *object)
{
    AiCodexCliClient *self = AI_CODEX_CLI_CLIENT(object);
    g_free(self->sandbox);
    g_free(self->additional_directories);
    g_free(self->profile);
    g_free(self->turn_system_prompt);
    G_OBJECT_CLASS(ai_codex_cli_client_parent_class)->finalize(object);
}

static void
ai_codex_cli_client_class_init(AiCodexCliClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiCliClientClass *cli = AI_CLI_CLIENT_CLASS(klass);
    object_class->get_property = get_property;
    object_class->set_property = set_property;
    object_class->finalize = finalize;
    cli->check_exit_status = TRUE;
    cli->get_executable_path = get_executable;
    cli->build_argv = build_argv;
    cli->build_stdin = build_stdin;
    cli->parse_json_output = parse_output;
    cli->parse_stream_line = parse_line;
    cli->parse_stream_events = parse_events;
    properties[PROP_SANDBOX] = g_param_spec_string("sandbox", "Sandbox",
        "read-only, workspace-write, or danger-full-access; NULL uses read-only",
        NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_SKIP_PERMISSIONS] = g_param_spec_boolean("skip-permissions", "Skip Permissions",
        "Bypass approvals and sandbox; conflicts with a restrictive explicit sandbox",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_CONTINUE_SESSION] = g_param_spec_boolean("continue-session", "Continue Session",
        "Resume the latest session when no session-id is set", FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_ADDITIONAL_DIRECTORIES] = g_param_spec_string("additional-directories", "Additional Directories",
        "Comma-separated additional writable directories (--add-dir)", NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_PROFILE] = g_param_spec_string("profile", "Profile",
        "Codex configuration profile", NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    properties[PROP_SEARCH] = g_param_spec_boolean("search", "Search",
        "Enable live web search", FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_codex_cli_client_init(AiCodexCliClient *self)
{
    ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_CODEX_CLI_DEFAULT_MODEL);
}

static AiProviderType get_provider_type(AiProvider *p) { (void)p; return AI_PROVIDER_CODEX_CLI; }
static const gchar *get_name(AiProvider *p) { (void)p; return "Codex CLI"; }
static const gchar *get_default_model(AiProvider *p) { (void)p; return AI_CODEX_CLI_DEFAULT_MODEL; }

/* Both asynchronous entry points share the base's cancellable, timeout-aware
 * subprocess pipeline. Codex always returns JSONL, even for buffered chat. */
static void
chat_async(AiProvider *p, GList *messages, const gchar *system, gint max_tokens,
           GList *tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    (void)tools;
    ai_cli_client_stream_run_async(AI_CLI_CLIENT(p), messages, system, max_tokens, cancellable, callback, data);
}
static AiResponse *
chat_finish(AiProvider *p, GAsyncResult *result, GError **error)
{
    g_autoptr(AiResponse) response = ai_cli_client_stream_run_finish(AI_CLI_CLIENT(p), result, error);
    if (response != NULL && ai_response_get_stop_reason(response) != AI_STOP_REASON_END_TURN)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                            "Codex output ended before turn.completed");
        return NULL;
    }
    return g_steal_pointer(&response);
}
static void
stream_async(AiStreamable *p, GList *messages, const gchar *system, gint max_tokens,
             GList *tools, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    chat_async(AI_PROVIDER(p), messages, system, max_tokens, tools, cancellable, callback, data);
}
static AiResponse *
stream_finish(AiStreamable *p, GAsyncResult *result, GError **error)
{
    return chat_finish(AI_PROVIDER(p), result, error);
}
static void free_models(gpointer data) { g_list_free_full(data, g_free); }
static void
list_models_async(AiProvider *provider, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data)
{
    const gchar *models[] = { AI_CODEX_CLI_MODEL_GPT_6_ASTRA, AI_CODEX_CLI_MODEL_GPT_5_6_SOL,
        AI_CODEX_CLI_MODEL_GPT_5_6_TERRA, AI_CODEX_CLI_MODEL_GPT_5_6_LUNA, AI_CODEX_CLI_MODEL_GPT_5_5,
        AI_CODEX_CLI_MODEL_GPT_5_4_MINI, AI_CODEX_CLI_MODEL_GPT_5_3_CODEX_SPARK };
    g_autoptr(GTask) task = g_task_new(provider, cancellable, callback, data);
    GList *list = NULL;
    guint i;
    for (i = 0; i < G_N_ELEMENTS(models); i++) list = g_list_append(list, g_strdup(models[i]));
    g_task_return_pointer(task, list, free_models);
}
static GList *
list_models_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}
static void
provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = get_provider_type;
    iface->get_name = get_name;
    iface->get_default_model = get_default_model;
    iface->chat_async = chat_async;
    iface->chat_finish = chat_finish;
    iface->list_models_async = list_models_async;
    iface->list_models_finish = list_models_finish;
}
static void
streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = stream_async;
    iface->chat_stream_finish = stream_finish;
}

/**
 * ai_codex_cli_client_new:
 *
 * Creates a Codex exec client using saved CLI authentication.
 * CODEX_PATH may override the executable found on PATH.
 *
 * Returns: (transfer full): a new #AiCodexCliClient
 */
AiCodexCliClient *ai_codex_cli_client_new(void)
{ return g_object_new(AI_TYPE_CODEX_CLI_CLIENT, NULL); }
/**
 * ai_codex_cli_client_new_with_config:
 * @config: an #AiConfig
 *
 * Returns: (transfer full): a new #AiCodexCliClient
 */
AiCodexCliClient *ai_codex_cli_client_new_with_config(AiConfig *config)
{ return g_object_new(AI_TYPE_CODEX_CLI_CLIENT, "config", config, NULL); }
/**
 * ai_codex_cli_client_get_sandbox:
 * @self: a Codex client
 * Returns: (nullable): the explicit sandbox policy, or NULL for read-only
 */
const gchar *ai_codex_cli_client_get_sandbox(AiCodexCliClient *self)
{ g_return_val_if_fail(AI_IS_CODEX_CLI_CLIENT(self), NULL); return self->sandbox; }
/**
 * ai_codex_cli_client_set_sandbox:
 * @self: a Codex client
 * @sandbox: (nullable): read-only, workspace-write, or danger-full-access
 *
 * Invalid policies or conflicts with skip-permissions fail before spawning.
 */
void ai_codex_cli_client_set_sandbox(AiCodexCliClient *self, const gchar *sandbox)
{ g_return_if_fail(AI_IS_CODEX_CLI_CLIENT(self)); g_object_set(self, "sandbox", sandbox, NULL); }
/**
 * ai_codex_cli_client_get_skip_permissions:
 * @self: a Codex client
 * Returns: whether approvals and sandbox are bypassed
 */
gboolean ai_codex_cli_client_get_skip_permissions(AiCodexCliClient *self)
{ g_return_val_if_fail(AI_IS_CODEX_CLI_CLIENT(self), FALSE); return self->skip_permissions; }
/**
 * ai_codex_cli_client_set_skip_permissions:
 * @self: a Codex client
 * @skip: whether to bypass approvals and sandbox
 */
void ai_codex_cli_client_set_skip_permissions(AiCodexCliClient *self, gboolean skip)
{ g_return_if_fail(AI_IS_CODEX_CLI_CLIENT(self)); g_object_set(self, "skip-permissions", skip, NULL); }
