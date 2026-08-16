/*
 * ai-grok-build-client.c - Grok Build CLI client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Wraps xAI's `grok` CLI (Grok Build TUI) in headless mode. The prompt is
 * piped on stdin and read back through `--prompt-file /dev/stdin`, which is
 * the C equivalent of the shell's `--prompt-file <(echo ...)`: no temporary
 * file, no ARG_MAX ceiling, and nothing sensitive in the process listing.
 */

#include "config.h"

#include <string.h>

#include "providers/ai-grok-build-client.h"
#include "providers/ai-grok-build-client-internal.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

/*
 * The prompt source handed to grok. grok accepts any readable path here --
 * a regular file, a process substitution fifo, or this -- and the base CLI
 * pipeline already writes build_stdin()'s output into the child's stdin.
 */
#define GROK_BUILD_PROMPT_FILE "/dev/stdin"

/*
 * Private structure for AiGrokBuildClient.
 */
struct _AiGrokBuildClient
{
    AiCliClient parent_instance;

    gdouble  total_cost;
    gboolean skip_permissions;

    /*
     * Everything below is emitted only when set, so an unconfigured client
     * builds the shortest argv grok will accept.
     */
    gchar   *permission_mode;
    gchar   *allowed_tools;
    gchar   *disallowed_tools;
    gchar   *sandbox;
    gint     max_turns;          /* 0 means "unset" */
    gchar   *agent;
    gchar   *rules;
    gboolean disable_web_search;
    gboolean verbatim;
    gboolean continue_session;

    /* Cached summary for the re-prompt fallback when the model produces
     * no text (empty "text" with tool use only). */
    gchar   *last_tool_summary;
};

/*
 * Interface implementations forward declarations.
 */
static void ai_grok_build_client_provider_init(AiProviderInterface *iface);
static void ai_grok_build_client_streamable_init(AiStreamableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(AiGrokBuildClient, ai_grok_build_client, AI_TYPE_CLI_CLIENT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              ai_grok_build_client_provider_init)
                        G_IMPLEMENT_INTERFACE(AI_TYPE_STREAMABLE,
                                              ai_grok_build_client_streamable_init))

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_TOTAL_COST,
    PROP_SKIP_PERMISSIONS,
    PROP_PERMISSION_MODE,
    PROP_ALLOWED_TOOLS,
    PROP_DISALLOWED_TOOLS,
    PROP_SANDBOX,
    PROP_MAX_TURNS,
    PROP_AGENT,
    PROP_RULES,
    PROP_DISABLE_WEB_SEARCH,
    PROP_VERBATIM,
    PROP_CONTINUE_SESSION,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_grok_build_client_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiGrokBuildClient *self = AI_GROK_BUILD_CLIENT(object);

    switch (prop_id)
    {
        case PROP_TOTAL_COST:
            g_value_set_double(value, self->total_cost);
            break;
        case PROP_SKIP_PERMISSIONS:
            g_value_set_boolean(value, self->skip_permissions);
            break;
        case PROP_PERMISSION_MODE:
            g_value_set_string(value, self->permission_mode);
            break;
        case PROP_ALLOWED_TOOLS:
            g_value_set_string(value, self->allowed_tools);
            break;
        case PROP_DISALLOWED_TOOLS:
            g_value_set_string(value, self->disallowed_tools);
            break;
        case PROP_SANDBOX:
            g_value_set_string(value, self->sandbox);
            break;
        case PROP_MAX_TURNS:
            g_value_set_int(value, self->max_turns);
            break;
        case PROP_AGENT:
            g_value_set_string(value, self->agent);
            break;
        case PROP_RULES:
            g_value_set_string(value, self->rules);
            break;
        case PROP_DISABLE_WEB_SEARCH:
            g_value_set_boolean(value, self->disable_web_search);
            break;
        case PROP_VERBATIM:
            g_value_set_boolean(value, self->verbatim);
            break;
        case PROP_CONTINUE_SESSION:
            g_value_set_boolean(value, self->continue_session);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_grok_build_client_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiGrokBuildClient *self = AI_GROK_BUILD_CLIENT(object);

    switch (prop_id)
    {
        case PROP_SKIP_PERMISSIONS:
            self->skip_permissions = g_value_get_boolean(value);
            break;
        case PROP_PERMISSION_MODE:
            g_free(self->permission_mode);
            self->permission_mode = g_value_dup_string(value);
            break;
        case PROP_ALLOWED_TOOLS:
            g_free(self->allowed_tools);
            self->allowed_tools = g_value_dup_string(value);
            break;
        case PROP_DISALLOWED_TOOLS:
            g_free(self->disallowed_tools);
            self->disallowed_tools = g_value_dup_string(value);
            break;
        case PROP_SANDBOX:
            g_free(self->sandbox);
            self->sandbox = g_value_dup_string(value);
            break;
        case PROP_MAX_TURNS:
            self->max_turns = g_value_get_int(value);
            break;
        case PROP_AGENT:
            g_free(self->agent);
            self->agent = g_value_dup_string(value);
            break;
        case PROP_RULES:
            g_free(self->rules);
            self->rules = g_value_dup_string(value);
            break;
        case PROP_DISABLE_WEB_SEARCH:
            self->disable_web_search = g_value_get_boolean(value);
            break;
        case PROP_VERBATIM:
            self->verbatim = g_value_get_boolean(value);
            break;
        case PROP_CONTINUE_SESSION:
            self->continue_session = g_value_get_boolean(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * The permission modes the grok CLI accepts. Validated here rather than
 * passed straight through so a typo produces one clear warning instead of a
 * subprocess that exits with the CLI's own usage text.
 */
static const gchar *AI_GROK_BUILD_PERMISSION_MODES[] = {
    "default", "acceptEdits", "auto", "dontAsk", "bypassPermissions", "plan",
    NULL
};

static gboolean
permission_mode_is_valid(const gchar *mode)
{
    gsize i;

    for (i = 0; AI_GROK_BUILD_PERMISSION_MODES[i] != NULL; i++)
    {
        if (g_strcmp0(mode, AI_GROK_BUILD_PERMISSION_MODES[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Map an #AiCliClient:effort-level string onto what grok accepts.
 *
 * grok takes exactly low/medium/high/xhigh. #AiEffortLevel also has "max",
 * which grok rejects -- and rejects in the worst possible way: it prints
 * {"type":"error",...} and still exits 0, so without this mapping the run
 * would look successful and produce nothing. "max" folds onto the highest
 * level grok has; anything else is dropped with a warning.
 *
 * Returns: (nullable): a static string to pass to --reasoning-effort, or
 *   %NULL to omit the flag entirely.
 */
static const gchar *
grok_effort_arg(const gchar *effort)
{
    if (effort == NULL || effort[0] == '\0')
        return NULL;

    if (g_strcmp0(effort, "low") == 0)
        return "low";
    if (g_strcmp0(effort, "medium") == 0)
        return "medium";
    if (g_strcmp0(effort, "high") == 0)
        return "high";
    if (g_strcmp0(effort, "xhigh") == 0)
        return "xhigh";

    /* AI_EFFORT_MAX has no grok equivalent; xhigh is the nearest. */
    if (g_strcmp0(effort, "max") == 0)
        return "xhigh";

    g_warning("grok-build: unknown effort level '%s'; omitting "
              "--reasoning-effort. Valid levels: low, medium, high, xhigh",
              effort);

    return NULL;
}

/*
 * Append a comma-separated value as the flag repeated once per item, which
 * is the shape grok's --allow and --deny rules take. Empty items are
 * dropped; an all-empty list emits nothing rather than a dangling flag.
 */
static void
emit_rule_flag(GPtrArray *args, const gchar *flag, const gchar *csv)
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
 * Emit the tool-permission arguments shared by the one-shot and retry argv
 * builders.
 *
 * skip-permissions wins when both are set: the two say different things
 * about the same session and the CLI should not be left to arbitrate. A
 * caller that set a narrow mode and silently got a full bypass is exactly
 * the failure worth surfacing.
 */
static void
emit_permission_args(AiGrokBuildClient *self, GPtrArray *args)
{
    if (self->skip_permissions)
    {
        if (self->permission_mode != NULL && self->permission_mode[0] != '\0' &&
            g_strcmp0(self->permission_mode, "bypassPermissions") != 0)
        {
            g_warning("grok-build: skip-permissions and permission-mode '%s' "
                      "are both set; using bypassPermissions",
                      self->permission_mode);
        }

        g_ptr_array_add(args, g_strdup("--permission-mode"));
        g_ptr_array_add(args, g_strdup("bypassPermissions"));
    }
    else if (self->permission_mode != NULL && self->permission_mode[0] != '\0')
    {
        if (permission_mode_is_valid(self->permission_mode))
        {
            g_ptr_array_add(args, g_strdup("--permission-mode"));
            g_ptr_array_add(args, g_strdup(self->permission_mode));
        }
        else
        {
            g_warning("grok-build: unknown permission mode '%s'; omitting "
                      "the flag. Valid modes: default, acceptEdits, auto, "
                      "dontAsk, bypassPermissions, plan",
                      self->permission_mode);
        }
    }

    emit_rule_flag(args, "--allow", self->allowed_tools);
    emit_rule_flag(args, "--deny", self->disallowed_tools);
}

/*
 * Emit the session-independent execution knobs: sandbox profile, turn
 * budget, agent profile, extra rules, and the two booleans.
 */
static void
emit_execution_args(AiGrokBuildClient *self, GPtrArray *args)
{
    if (self->sandbox != NULL && self->sandbox[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--sandbox"));
        g_ptr_array_add(args, g_strdup(self->sandbox));
    }

    if (self->max_turns > 0)
    {
        g_ptr_array_add(args, g_strdup("--max-turns"));
        g_ptr_array_add(args, g_strdup_printf("%d", self->max_turns));
    }

    if (self->agent != NULL && self->agent[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--agent"));
        g_ptr_array_add(args, g_strdup(self->agent));
    }

    if (self->rules != NULL && self->rules[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--rules"));
        g_ptr_array_add(args, g_strdup(self->rules));
    }

    if (self->disable_web_search)
    {
        g_ptr_array_add(args, g_strdup("--disable-web-search"));
    }

    /*
     * --verbatim stops grok from reinterpreting the prompt -- a library
     * caller's text that happens to begin with "/" is a prompt, not a slash
     * command. Hence the TRUE default.
     */
    if (self->verbatim)
    {
        g_ptr_array_add(args, g_strdup("--verbatim"));
    }
}

/*
 * Get the executable path for the grok CLI.
 * Checks the GROK_PATH environment variable first, then falls back to
 * searching PATH for "grok".
 */
static gchar *
ai_grok_build_client_get_executable_path(AiCliClient *client)
{
    const gchar *env_path;

    (void)client;

    env_path = g_getenv("GROK_PATH");
    if (env_path != NULL && env_path[0] != '\0')
    {
        return g_strdup(env_path);
    }

    return g_strdup("grok");
}

/*
 * Build command line arguments for the grok CLI.
 *
 * Non-streaming:
 *   grok --prompt-file /dev/stdin --output-format json --model <model> ...
 * Streaming:
 *   grok --prompt-file /dev/stdin --output-format streaming-messages-json
 *        --include-partial-messages --model <model> ...
 */
gchar **
ai_grok_build_client_build_argv(
    AiCliClient *client,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    gboolean     streaming
){
    AiGrokBuildClient *self = AI_GROK_BUILD_CLIENT(client);
    GPtrArray *args;
    const gchar *model;
    const gchar *session_id;
    const gchar *effort;
    gboolean persist;

    (void)messages;    /* prompt goes via stdin */
    (void)max_tokens;  /* grok has no max tokens flag */

    args = g_ptr_array_new();

    /* Executable placeholder — the caller overwrites it with the
     * resolved path before spawning. */
    g_ptr_array_add(args, g_strdup("grok"));

    /*
     * Prompt source. The prompt itself never enters argv: build_stdin()
     * produces it, the caller pipes it in, and grok reads it back here.
     */
    g_ptr_array_add(args, g_strdup("--prompt-file"));
    g_ptr_array_add(args, g_strdup(GROK_BUILD_PROMPT_FILE));

    /* Output format */
    if (streaming)
    {
        /*
         * streaming-messages-json is the Anthropic Messages wire format;
         * --include-partial-messages is what turns whole messages into
         * per-token text_delta events.
         */
        g_ptr_array_add(args, g_strdup("--output-format"));
        g_ptr_array_add(args, g_strdup("streaming-messages-json"));
        g_ptr_array_add(args, g_strdup("--include-partial-messages"));
    }
    else
    {
        g_ptr_array_add(args, g_strdup("--output-format"));
        g_ptr_array_add(args, g_strdup("json"));
    }

    /* Model */
    model = ai_cli_client_get_model(client);
    if (model == NULL || model[0] == '\0')
        model = AI_GROK_BUILD_DEFAULT_MODEL;
    g_ptr_array_add(args, g_strdup("--model"));
    g_ptr_array_add(args, g_strdup(model));

    /* Tool access: a full bypass, or a permission mode and rule lists. */
    emit_permission_args(self, args);

    /* Sandbox, turn budget, agent profile, extra rules, booleans. */
    emit_execution_args(self, args);

    /* Session management — resolve session_id before the system prompt */
    persist = ai_cli_client_get_session_persistence(client);
    session_id = ai_cli_client_get_session_id(client);
    if (persist && session_id != NULL && session_id[0] != '\0')
    {
        /*
         * Resume an existing session. Do NOT pass the system prompt: the
         * session already carries it from the initial call, and re-sending
         * it wastes tokens and re-injects the whole prompt.
         */
        g_ptr_array_add(args, g_strdup("--resume"));
        g_ptr_array_add(args, g_strdup(session_id));
    }
    else if (persist && self->continue_session)
    {
        /*
         * No id to resume, but the caller asked to pick up where this
         * directory left off. Like --resume, the session already carries
         * its system prompt, so it is not re-sent.
         */
        g_ptr_array_add(args, g_strdup("--continue"));
    }
    else if (system_prompt != NULL && system_prompt[0] != '\0')
    {
        /* New session — prime it with the system prompt. */
        g_ptr_array_add(args, g_strdup("--system-prompt-override"));
        g_ptr_array_add(args, g_strdup(system_prompt));
    }

    /* Reasoning effort */
    effort = grok_effort_arg(ai_cli_client_get_effort_level(client));
    if (effort != NULL)
    {
        g_ptr_array_add(args, g_strdup("--reasoning-effort"));
        g_ptr_array_add(args, g_strdup(effort));
    }

    /* NULL terminate */
    g_ptr_array_add(args, NULL);

    return (gchar **)g_ptr_array_free(args, FALSE);
}

/*
 * Build the prompt to pipe via stdin to the grok CLI, which reads it back
 * through `--prompt-file /dev/stdin`. Piping rather than passing argv
 * avoids the ARG_MAX limit on large conversations.
 */
static gchar *
ai_grok_build_client_build_stdin(
    AiCliClient *client,
    GList       *messages
){
    GString *prompt;
    GList *l;

    (void)client;

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

            /* Add a role prefix for multi-message conversations */
            if (role == AI_ROLE_USER)
            {
                g_string_append(prompt, text);
            }
            else if (role == AI_ROLE_ASSISTANT)
            {
                g_string_append_printf(prompt,
                    "Previous assistant response: %s", text);
            }
        }
    }

    /* Instruct the model to always produce a plain text response */
    g_string_append(prompt,
        "\n\nIMPORTANT: Always include a plain text response. "
        "Tool use is fine, but you MUST provide a text summary of "
        "your work when finished. Never end your turn on tool calls alone.");

    return g_string_free(prompt, FALSE);
}

/*
 * Type-safe JSON member accessors.
 *
 * json-glib's *_member_with_default() helpers emit a GLib critical when a
 * member exists but holds the wrong type, and json_object_get_object_member()
 * does the same. Subprocess output is untrusted input -- a future CLI
 * version, a truncated write, or a plugin writing to stdout can all produce
 * a shape we did not expect -- and none of that should turn into criticals
 * or an abort under a fatal-warnings test run. These return the fallback
 * instead, so every parse below is total.
 */

static const gchar *
grok_get_string(JsonObject *obj, const gchar *name)
{
    JsonNode *node;

    if (obj == NULL || name == NULL)
        return NULL;

    node = json_object_get_member(obj, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;

    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string(node);
}

static JsonObject *
grok_get_object(JsonObject *obj, const gchar *name)
{
    JsonNode *node;

    if (obj == NULL || name == NULL)
        return NULL;

    node = json_object_get_member(obj, name);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

static gint
grok_get_int(JsonObject *obj, const gchar *name, gint fallback)
{
    JsonNode *node;
    GType value_type;

    if (obj == NULL || name == NULL)
        return fallback;

    node = json_object_get_member(obj, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    value_type = json_node_get_value_type(node);
    if (value_type != G_TYPE_INT64 && value_type != G_TYPE_INT &&
        value_type != G_TYPE_DOUBLE)
        return fallback;

    return (gint)json_node_get_int(node);
}

static gdouble
grok_get_double(JsonObject *obj, const gchar *name, gdouble fallback)
{
    JsonNode *node;
    GType value_type;

    if (obj == NULL || name == NULL)
        return fallback;

    node = json_object_get_member(obj, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    value_type = json_node_get_value_type(node);
    if (value_type == G_TYPE_DOUBLE)
        return json_node_get_double(node);
    if (value_type == G_TYPE_INT64 || value_type == G_TYPE_INT)
        return (gdouble)json_node_get_int(node);

    return fallback;
}

static gboolean
grok_get_boolean(JsonObject *obj, const gchar *name, gboolean fallback)
{
    JsonNode *node;

    if (obj == NULL || name == NULL)
        return fallback;

    node = json_object_get_member(obj, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    if (json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return fallback;

    return json_node_get_boolean(node);
}

/*
 * Look a string member up under two spellings.
 *
 * grok's --output-format json speaks camelCase ("sessionId", "stopReason")
 * while its NDJSON result line speaks snake_case ("session_id",
 * "stop_reason"). Accepting both lets one parser serve either.
 */
static const gchar *
grok_string_member(
    JsonObject  *obj,
    const gchar *primary,
    const gchar *fallback
){
    const gchar *value;

    value = grok_get_string(obj, primary);
    if (value != NULL)
        return value;

    return grok_get_string(obj, fallback);
}

/*
 * Pull the usage counts out of an object's "usage" member, if it has one
 * shaped the way we expect, and attach them to @response.
 */
static void
grok_apply_usage(JsonObject *obj, AiResponse *response)
{
    JsonObject *usage_obj;
    g_autoptr(AiUsage) usage = NULL;

    usage_obj = grok_get_object(obj, "usage");
    if (usage_obj == NULL)
        return;

    usage = ai_usage_new(grok_get_int(usage_obj, "input_tokens", 0),
                         grok_get_int(usage_obj, "output_tokens", 0));

    ai_response_set_usage(response, usage);
}

/*
 * Turn an error-shaped object into a GError.
 *
 * Two shapes reach here: the standalone {"type":"error","message":...} grok
 * prints for a rejected argument, and a result line whose "is_error" is
 * true, where the human-readable text lives in "result" instead.
 */
static void
grok_set_error_from_object(JsonObject *obj, GError **error)
{
    const gchar *message;

    message = grok_string_member(obj, "message", "error");
    if (message == NULL || message[0] == '\0')
        message = grok_string_member(obj, "result", "text");
    if (message == NULL || message[0] == '\0')
        message = "Unknown error";

    g_set_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                "CLI error: %s", message);
}

/*
 * Load @candidate and return the JSON object it carries, or NULL.
 *
 * Three things make this less obvious than it looks:
 *
 *   - an empty document loads successfully with a NULL root, so the root is
 *     what must be checked, not the return value;
 *   - json-glib folds a newline-separated *sequence* of objects into a
 *     single array, which is exactly what NDJSON output parses as -- the
 *     last object in it is the result line, so that is the one to take;
 *   - anything else (a bare scalar, an array of numbers) is not a response.
 */
static JsonObject *
grok_try_parse(JsonParser *parser, const gchar *candidate)
{
    JsonNode *root;
    JsonArray *array;
    guint i;

    if (candidate == NULL || candidate[0] == '\0')
        return NULL;

    if (!json_parser_load_from_data(parser, candidate, -1, NULL))
        return NULL;

    root = json_parser_get_root(parser);
    if (root == NULL)
        return NULL;

    if (JSON_NODE_HOLDS_OBJECT(root))
        return json_node_get_object(root);

    if (!JSON_NODE_HOLDS_ARRAY(root))
        return NULL;

    /* NDJSON: walk back to the last object, which is the result line. */
    array = json_node_get_array(root);
    for (i = json_array_get_length(array); i > 0; i--)
    {
        JsonNode *element = json_array_get_element(array, i - 1);

        if (element != NULL && JSON_NODE_HOLDS_OBJECT(element))
            return json_node_get_object(element);
    }

    return NULL;
}

/*
 * Parse a JSON object out of the CLI's stdout.
 *
 * grok normally prints one pretty-printed object, but it does not promise
 * that is all stdout carries, and an argument error prints a single-line
 * {"type":"error",...} instead -- sometimes with a zero exit status. Three
 * attempts, cheapest first:
 *
 *   1. the whole buffer, which covers both the normal case and NDJSON
 *      (see grok_try_parse for how a sequence collapses to its last
 *      object);
 *   2. the last non-empty line, for output whose earlier lines are not
 *      JSON at all;
 *   3. from the first "{" to the end, which catches a pretty-printed
 *      object preceded by a banner or upgrade notice -- the line-based
 *      attempt cannot help there, because the object spans lines.
 *
 * Anything still unparseable is a parse error naming the problem, not a
 * silent empty response.
 */
static JsonObject *
grok_parse_output_object(
    JsonParser  *parser,
    const gchar *json,
    GError     **error
){
    g_autofree gchar *last_line = NULL;
    g_auto(GStrv) lines = NULL;
    const gchar *brace;
    JsonObject *obj;
    gint i;

    obj = grok_try_parse(parser, json);
    if (obj != NULL)
        return obj;

    lines = g_strsplit(json, "\n", -1);
    for (i = 0; lines[i] != NULL; i++)
    {
        g_strstrip(lines[i]);
        if (lines[i][0] != '\0')
        {
            g_free(last_line);
            last_line = g_strdup(lines[i]);
        }
    }

    obj = grok_try_parse(parser, last_line);
    if (obj != NULL)
        return obj;

    brace = strchr(json, '{');
    if (brace != NULL && brace != json)
    {
        obj = grok_try_parse(parser, brace);
        if (obj != NULL)
            return obj;
    }

    g_set_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                "Could not parse a JSON object from CLI output");

    return NULL;
}

/*
 * Parse JSON output from the grok CLI.
 *
 * Expected format (--output-format json):
 * {
 *     "text": "response text",
 *     "stopReason": "end_turn",
 *     "sessionId": "uuid",
 *     "usage": {"input_tokens": N, "output_tokens": N, ...},
 *     "total_cost_usd": 0.007
 * }
 *
 * An error instead arrives as {"type":"error","message":"..."} — on stdout,
 * and not always with a non-zero exit status, so it is checked first and
 * independently of how the process exited.
 */
static AiResponse *
ai_grok_build_client_parse_json_output(
    AiCliClient *client,
    const gchar *json,
    GError     **error
){
    AiGrokBuildClient *self = AI_GROK_BUILD_CLIENT(client);
    g_autoptr(JsonParser) parser = NULL;
    g_autoptr(AiResponse) response = NULL;
    JsonObject *obj;
    const gchar *type;
    const gchar *text;
    const gchar *session_id;
    const gchar *stop_reason;

    parser = json_parser_new();

    obj = grok_parse_output_object(parser, json, error);
    if (obj == NULL)
    {
        return NULL;
    }

    /*
     * Errors first — they can arrive with a zero exit status, so this check
     * is what stands between a rejected argument and a run that reports
     * success with an empty response.
     *
     * Two shapes: a standalone error object, and a result line flagged with
     * "is_error". The second only appears in the NDJSON stream, but the
     * fallbacks above can land us on one, so both are handled here.
     */
    type = grok_get_string(obj, "type");
    if (g_strcmp0(type, "error") == 0 ||
        grok_get_boolean(obj, "is_error", FALSE))
    {
        grok_set_error_from_object(obj, error);
        return NULL;
    }

    /* Create response */
    session_id = grok_string_member(obj, "sessionId", "session_id");
    if (session_id == NULL)
        session_id = "";

    response = ai_response_new(session_id, ai_cli_client_get_model(client));

    stop_reason = grok_string_member(obj, "stopReason", "stop_reason");
    if (stop_reason != NULL)
    {
        AiStopReason reason = ai_stop_reason_from_string(stop_reason);

        ai_response_set_stop_reason(response,
            reason != AI_STOP_REASON_NONE ? reason : AI_STOP_REASON_END_TURN);
    }
    else
    {
        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }

    /* Store session ID for continuity — ONLY if persistence is enabled */
    if (session_id[0] != '\0' && ai_cli_client_get_session_persistence(client))
    {
        ai_cli_client_set_session_id(client, session_id);
    }

    /* Response text. "result" is the NDJSON result line's spelling. */
    text = grok_string_member(obj, "text", "result");
    if (text != NULL && text[0] != '\0')
    {
        g_autoptr(AiTextContent) content = ai_text_content_new(text);
        ai_response_add_content_block(response,
            (AiContentBlock *)g_steal_pointer(&content));
    }
    else
    {
        /*
         * Empty text — the model finished on tool calls without a summary.
         * Flag it so the completion callback can re-prompt for one; if that
         * also fails this string is returned as a last resort.
         */
        g_free(self->last_tool_summary);
        self->last_tool_summary = g_strdup(
            "(completed tool operations — no text summary was provided)");
    }

    /* Usage */
    grok_apply_usage(obj, response);

    /* Total cost */
    self->total_cost = grok_get_double(obj, "total_cost_usd",
                                       self->total_cost);

    return (AiResponse *)g_steal_pointer(&response);
}

/*
 * Turn one Anthropic tool_use block into an AI_EVENT_TOOL_STARTED.
 *
 * Shared by the content_block_start path, where the input is still empty,
 * and the whole-message path, where it is complete. Both emit for the same
 * id on purpose; consumers key on the id and update.
 */
static gboolean
ai_grok_build_client_parse_stream_line(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    gchar       **delta_text,
    GError      **error
);

static void
grok_emit_tool_use_block(
    JsonObject *block,
    GPtrArray  *out_events
){
    const gchar *id;
    const gchar *name;
    JsonNode *input;
    g_autoptr(AiToolUse) tool_use = NULL;

    id = grok_get_string(block, "id");
    name = grok_get_string(block, "name");

    if (name == NULL || name[0] == '\0')
    {
        return;
    }

    input = json_object_has_member(block, "input")
        ? json_object_get_member(block, "input")
        : NULL;

    tool_use = ai_tool_use_new(id != NULL ? id : "",
                               name,
                               input != NULL ? json_node_copy(input) : NULL);

    g_ptr_array_add(out_events, ai_event_new_tool_started(tool_use));
}

/*
 * Parse a single NDJSON line from streaming output into events.
 *
 * With --include-partial-messages grok emits Anthropic-shaped events:
 *   {"type":"stream_event","event":{"type":"content_block_start",
 *    "content_block":{"type":"tool_use","id":..,"name":..}}} -> TOOL_STARTED
 *   {"type":"stream_event","event":{"type":"content_block_delta",
 *    "delta":{"type":"text_delta","text":"..."}}}            -> TEXT_DELTA
 *    "delta":{"type":"thinking_delta","thinking":"..."}      -> THINKING_DELTA
 *    "delta":{"type":"input_json_delta","partial_json":".."} -> TOOL_INPUT_DELTA
 *   {"type":"assistant","message":{...}}                     -> tool_use only
 *   {"type":"result",...}                                    -> USAGE, session
 *
 * The whole-message "assistant" line repeats text already delivered as
 * deltas, so its text is still ignored -- acting on it would double every
 * streamed reply. Its tool_use blocks are not ignored: they are the only
 * place the *complete* arguments appear, since content_block_start carries
 * the name with an empty input and the rest arrives as input_json_delta
 * fragments that are not individually valid JSON. Emitting TOOL_STARTED
 * twice for one id is deliberate and documented -- consumers key on the id
 * and update, so the second one simply fills in what the first could not
 * know yet.
 */
static gboolean
ai_grok_build_client_parse_stream_events(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    GPtrArray    *out_events,
    GError      **error
){
    AiGrokBuildClient *self = AI_GROK_BUILD_CLIENT(client);
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    const gchar *type;

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
    type = grok_get_string(obj, "type");

    if (g_strcmp0(type, "error") == 0)
    {
        grok_set_error_from_object(obj, error);
        ai_response_set_stop_reason(response, AI_STOP_REASON_ERROR);
        return FALSE;
    }

    if (g_strcmp0(type, "stream_event") == 0)
    {
        JsonObject *event;
        const gchar *event_type;

        event = grok_get_object(obj, "event");
        if (event == NULL)
            return TRUE;

        event_type = grok_get_string(event, "type");

        if (g_strcmp0(event_type, "content_block_start") == 0)
        {
            /*
             * A tool call announcing itself. The input is normally an empty
             * object here; the arguments follow as input_json_delta.
             */
            JsonObject *block = grok_get_object(event, "content_block");

            if (block != NULL &&
                g_strcmp0(grok_get_string(block, "type"), "tool_use") == 0)
            {
                grok_emit_tool_use_block(block, out_events);
            }
        }
        else if (g_strcmp0(event_type, "content_block_delta") == 0)
        {
            JsonObject *delta = grok_get_object(event, "delta");
            const gchar *delta_type;

            if (delta == NULL)
                return TRUE;

            delta_type = grok_get_string(delta, "type");

            if (g_strcmp0(delta_type, "text_delta") == 0)
            {
                const gchar *text = grok_get_string(delta, "text");

                if (text != NULL && text[0] != '\0')
                    g_ptr_array_add(out_events, ai_event_new_text_delta(text));
            }
            else if (g_strcmp0(delta_type, "thinking_delta") == 0)
            {
                /*
                 * Reasoning used to be dropped here on the grounds that it
                 * is not the answer. That is still true, and is now
                 * expressed by giving it its own kind rather than by
                 * discarding it -- a frontend can collapse what a caller
                 * assembling the answer must exclude.
                 */
                const gchar *text = grok_get_string(delta, "thinking");

                if (text != NULL && text[0] != '\0')
                    g_ptr_array_add(out_events,
                                    ai_event_new_thinking_delta(text));
            }
            else if (g_strcmp0(delta_type, "input_json_delta") == 0)
            {
                const gchar *fragment = grok_get_string(delta, "partial_json");

                if (fragment != NULL && fragment[0] != '\0')
                    g_ptr_array_add(
                        out_events,
                        ai_event_new_tool_input_delta(NULL, fragment));
            }
        }
    }
    else if (g_strcmp0(type, "assistant") == 0)
    {
        /*
         * Text here would double the deltas, but the tool_use blocks carry
         * the assembled arguments and appear nowhere else.
         */
        JsonObject *message = grok_get_object(obj, "message");
        JsonNode *content = message != NULL
            ? json_object_get_member(message, "content")
            : NULL;

        if (content != NULL && JSON_NODE_HOLDS_ARRAY(content))
        {
            JsonArray *blocks = json_node_get_array(content);
            guint n = json_array_get_length(blocks);
            guint i;

            for (i = 0; i < n; i++)
            {
                JsonNode *bn = json_array_get_element(blocks, i);
                JsonObject *b;

                if (bn == NULL || !JSON_NODE_HOLDS_OBJECT(bn))
                    continue;

                b = json_node_get_object(bn);

                if (g_strcmp0(grok_get_string(b, "type"), "tool_use") == 0)
                    grok_emit_tool_use_block(b, out_events);
            }
        }
    }
    else if (g_strcmp0(type, "result") == 0)
    {
        /* Final result with session and usage info */
        const gchar *result_text = grok_string_member(obj, "result", "text");
        const gchar *session_id = grok_string_member(obj, "session_id",
                                                     "sessionId");

        /*
         * A run that failed part-way through still emits a result line, with
         * is_error set and the reason in "result". Reporting it as a normal
         * end-of-turn would hand the caller the failure text as if it were
         * the answer.
         */
        if (grok_get_boolean(obj, "is_error", FALSE))
        {
            grok_set_error_from_object(obj, error);
            ai_response_set_stop_reason(response, AI_STOP_REASON_ERROR);
            return FALSE;
        }

        /* Store session ID — ONLY if persistence is enabled */
        if (session_id != NULL && session_id[0] != '\0' &&
            ai_cli_client_get_session_persistence(client))
        {
            ai_cli_client_set_session_id(client, session_id);
        }

        /* Add the final text if the deltas never produced any */
        if (result_text != NULL && result_text[0] != '\0' &&
            ai_response_get_content_blocks(response) == NULL)
        {
            g_autoptr(AiTextContent) content = ai_text_content_new(result_text);
            ai_response_add_content_block(response,
                (AiContentBlock *)g_steal_pointer(&content));
        }

        /* Usage */
        grok_apply_usage(obj, response);

        /* Total cost */
        self->total_cost = grok_get_double(obj, "total_cost_usd",
                                           self->total_cost);

        /*
         * Report the counts as an event too. -1 rather than 0 when grok
         * says nothing about cost: zero would read as "this turn was free".
         */
        {
            AiUsage *usage = ai_response_get_usage(response);
            gdouble cost = grok_get_double(obj, "total_cost_usd", -1.0);

            g_ptr_array_add(out_events,
                ai_event_new_usage(usage,
                                   cost >= 0.0 ? (gint64)(cost * 1000000.0) : -1));
        }

        /*
         * Prefer the reported stop reason; a truncated turn is not an
         * end_turn, and a caller checking for AI_STOP_REASON_MAX_TOKENS
         * needs to see it.
         */
        {
            const gchar *stop = grok_string_member(obj, "stop_reason",
                                                   "stopReason");
            AiStopReason reason = ai_stop_reason_from_string(stop);

            ai_response_set_stop_reason(response,
                reason != AI_STOP_REASON_NONE ? reason
                                              : AI_STOP_REASON_END_TURN);
        }
    }

    return TRUE;
}

/*
 * Destructor — free instance data.
 */
static void
ai_grok_build_client_finalize(GObject *object)
{
    AiGrokBuildClient *self = AI_GROK_BUILD_CLIENT(object);

    g_free(self->last_tool_summary);
    g_free(self->permission_mode);
    g_free(self->allowed_tools);
    g_free(self->disallowed_tools);
    g_free(self->sandbox);
    g_free(self->agent);
    g_free(self->rules);

    G_OBJECT_CLASS(ai_grok_build_client_parent_class)->finalize(object);
}

static void
ai_grok_build_client_class_init(AiGrokBuildClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

    object_class->finalize     = ai_grok_build_client_finalize;
    object_class->get_property = ai_grok_build_client_get_property;
    object_class->set_property = ai_grok_build_client_set_property;

    /* Override virtual methods */
    cli_class->get_executable_path = ai_grok_build_client_get_executable_path;
    cli_class->build_argv          = ai_grok_build_client_build_argv;
    cli_class->build_stdin         = ai_grok_build_client_build_stdin;
    cli_class->parse_json_output   = ai_grok_build_client_parse_json_output;
    cli_class->parse_stream_line   = ai_grok_build_client_parse_stream_line;
    cli_class->parse_stream_events = ai_grok_build_client_parse_stream_events;

    /**
     * AiGrokBuildClient:total-cost:
     *
     * The total cost in USD from the last response.
     */
    properties[PROP_TOTAL_COST] =
        g_param_spec_double("total-cost",
                            "Total Cost",
                            "The total cost in USD from the last response",
                            0.0, G_MAXDOUBLE, 0.0,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:skip-permissions:
     *
     * Whether to run the CLI with `--permission-mode bypassPermissions`.
     * When enabled the CLI never prompts for tool-use approval, allowing
     * fully autonomous operation.
     */
    properties[PROP_SKIP_PERMISSIONS] =
        g_param_spec_boolean("skip-permissions",
                             "Skip Permissions",
                             "Whether to run the CLI with bypassPermissions",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:permission-mode:
     *
     * The CLI's --permission-mode. Grants tool use without the blanket
     * bypass of #AiGrokBuildClient:skip-permissions -- "acceptEdits" in
     * particular lets the model edit files under the working directory
     * while still refusing everything else.
     *
     * The working directory, not this property, is the boundary that
     * matters: every mode that can write can write anywhere the process
     * can reach. See #AiCliClient:working-directory.
     */
    properties[PROP_PERMISSION_MODE] =
        g_param_spec_string("permission-mode",
                            "Permission Mode",
                            "CLI --permission-mode (default, acceptEdits, "
                            "auto, dontAsk, bypassPermissions, plan)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:allowed-tools:
     *
     * Comma-separated permission rules for --allow. Each becomes its own
     * `--allow <rule>` pair, matching how grok takes repeated rules.
     */
    properties[PROP_ALLOWED_TOOLS] =
        g_param_spec_string("allowed-tools",
                            "Allowed Tools",
                            "Comma-separated permission rules for --allow",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:disallowed-tools:
     *
     * Comma-separated permission rules for --deny.
     */
    properties[PROP_DISALLOWED_TOOLS] =
        g_param_spec_string("disallowed-tools",
                            "Disallowed Tools",
                            "Comma-separated permission rules for --deny",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:sandbox:
     *
     * The sandbox profile passed as --sandbox, bounding filesystem and
     * network access. The profile must already exist in ~/.grok/sandbox.toml
     * or .grok/sandbox.toml -- grok refuses to start on an unknown name
     * rather than running unsandboxed.
     */
    properties[PROP_SANDBOX] =
        g_param_spec_string("sandbox",
                            "Sandbox",
                            "Sandbox profile passed as --sandbox",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:max-turns:
     *
     * Maximum number of agent turns, passed as --max-turns. Zero, the
     * default, leaves the flag off and lets the CLI decide.
     */
    properties[PROP_MAX_TURNS] =
        g_param_spec_int("max-turns",
                         "Max Turns",
                         "Maximum agent turns (--max-turns); 0 to omit",
                         0, G_MAXINT, 0,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:agent:
     *
     * An agent name or definition file path, passed as --agent.
     */
    properties[PROP_AGENT] =
        g_param_spec_string("agent",
                            "Agent",
                            "Agent name or definition path (--agent)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:rules:
     *
     * Extra rules appended to the system prompt, passed as --rules. Unlike
     * #AiCliClient:system-prompt these are additive rather than a
     * replacement, and they survive a --resume.
     */
    properties[PROP_RULES] =
        g_param_spec_string("rules",
                            "Rules",
                            "Extra rules appended to the system prompt",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:disable-web-search:
     *
     * Whether to pass --disable-web-search, removing the web search and
     * web fetch tools from the session.
     */
    properties[PROP_DISABLE_WEB_SEARCH] =
        g_param_spec_boolean("disable-web-search",
                             "Disable Web Search",
                             "Whether to pass --disable-web-search",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:verbatim:
     *
     * Whether to pass --verbatim, sending the prompt exactly as given.
     *
     * Defaults to %TRUE: a library caller's prompt is text, and text that
     * happens to begin with "/" must not be taken for a slash command.
     */
    properties[PROP_VERBATIM] =
        g_param_spec_boolean("verbatim",
                             "Verbatim",
                             "Whether to send the prompt exactly as given",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiGrokBuildClient:continue-session:
     *
     * Whether to pass `--continue`, resuming the most recent session for
     * the working directory when no #AiCliClient:session-id is known.
     *
     * An explicit session id wins: the two name different sessions, and
     * silently preferring the wrong one is worse than either. Ignored when
     * #AiCliClient:session-persistence is off, which asks for a fresh
     * session every time.
     */
    properties[PROP_CONTINUE_SESSION] =
        g_param_spec_boolean("continue-session",
                             "Continue Session",
                             "Continue the most recent session (--continue)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_grok_build_client_init(AiGrokBuildClient *self)
{
    self->total_cost = 0.0;
    self->skip_permissions = FALSE;
    self->max_turns = 0;
    self->disable_web_search = FALSE;
    self->verbatim = TRUE;

    /* Set default model */
    ai_cli_client_set_model(AI_CLI_CLIENT(self), AI_GROK_BUILD_DEFAULT_MODEL);
}

/*
 * The pre-event contract, kept because callers and tests still use it.
 * It is a projection of parse_stream_events, not a second implementation:
 * one parser means the two can never disagree about what a line meant.
 */
static gboolean
ai_grok_build_client_parse_stream_line(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    gchar       **delta_text,
    GError      **error
){
    g_autoptr(GPtrArray) events =
        g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);

    *delta_text = NULL;

    if (!ai_grok_build_client_parse_stream_events(client, line, response,
                                                  events, error))
    {
        return FALSE;
    }

    *delta_text = ai_cli_client_events_to_delta(events);
    return TRUE;
}

/*
 * AiProvider interface implementation
 */

static AiProviderType
ai_grok_build_client_get_provider_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_GROK_BUILD;
}

static const gchar *
ai_grok_build_client_get_name(AiProvider *provider)
{
    (void)provider;
    return "Grok Build";
}

static const gchar *
ai_grok_build_client_get_default_model(AiProvider *provider)
{
    (void)provider;
    return AI_GROK_BUILD_DEFAULT_MODEL;
}

/*
 * Async chat completion callback data.
 */
typedef struct
{
    AiGrokBuildClient *client;
    GTask             *task;
    GSubprocess       *subprocess;
    gchar             *stdin_data;
} ChatAsyncData;

static void
chat_async_data_free(ChatAsyncData *data)
{
    /*
     * g_task_return_*() does NOT consume the reference chat_async() took
     * from g_task_new(); the async function owns it until the operation is
     * finished with.
     *
     * Safe against the retry hand-off, which sets data->task to NULL before
     * freeing precisely so the task survives into the second attempt.
     */
    g_clear_object(&data->task);
    g_clear_object(&data->client);
    g_clear_object(&data->subprocess);
    g_clear_pointer(&data->stdin_data, g_free);
    g_slice_free(ChatAsyncData, data);
}

/*
 * Retry data — used when the model made tool calls but produced no text.
 * We re-prompt asking for a plain-text summary; if that also fails we fall
 * back to the generic tool_summary string.
 */
typedef struct
{
    AiGrokBuildClient *client;
    GTask             *task;
    GSubprocess       *subprocess;
    gchar             *tool_summary;
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
){
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

    if (stdout_data == NULL || stdout_data[0] == '\0')
        goto fallback;

    /*
     * Exit status is not consulted here: grok reports some failures as a
     * JSON error object with a zero status, and parse_json_output already
     * turns those into a GError. A non-zero status with parseable output
     * would have produced that error too.
     */
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
    g_warning("grok-build: re-prompt failed, using tool summary as fallback");

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
 * Attempt to re-prompt grok for a plain-text summary of its tool work.
 * Returns TRUE if the retry subprocess was spawned (task ownership
 * transferred to the retry callback), FALSE if it could not start.
 * Requires an active session ID to resume the conversation.
 */
static gboolean
attempt_text_retry(
    AiGrokBuildClient *client,
    GTask             *task,
    const gchar       *tool_summary
){
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

    /* Can only re-prompt if we have a session to resume */
    if (sid == NULL || sid[0] == '\0')
        return FALSE;

    rargs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(rargs, g_strdup(exe));
    g_ptr_array_add(rargs, g_strdup("--prompt-file"));
    g_ptr_array_add(rargs, g_strdup(GROK_BUILD_PROMPT_FILE));
    g_ptr_array_add(rargs, g_strdup("--output-format"));
    g_ptr_array_add(rargs, g_strdup("json"));
    g_ptr_array_add(rargs, g_strdup("--model"));
    g_ptr_array_add(rargs,
        g_strdup((model != NULL && model[0] != '\0')
                 ? model : AI_GROK_BUILD_DEFAULT_MODEL));
    emit_permission_args(client, rargs);
    emit_execution_args(client, rargs);
    g_ptr_array_add(rargs, g_strdup("--resume"));
    g_ptr_array_add(rargs, g_strdup(sid));
    g_ptr_array_add(rargs, NULL);

    rproc = ai_cli_client_spawn(
        AI_CLI_CLIENT(client),
        (const gchar *const *)rargs->pdata,
        G_SUBPROCESS_FLAGS_STDIN_PIPE |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE,
        &err);

    if (rproc == NULL)
        return FALSE;

    retry = g_slice_new0(RetryAsyncData);
    retry->client       = g_object_ref(client);
    retry->task         = task;
    retry->subprocess   = rproc;   /* takes ownership */
    retry->tool_summary = g_strdup(tool_summary);

    g_warning("grok-build: no text in response, re-prompting for summary "
              "(session=%s)", sid);

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
                                               &stdout_data, &stderr_data,
                                               &error))
    {
        g_task_return_error(data->task, g_steal_pointer(&error));
        chat_async_data_free(data);
        return;
    }

    /*
     * Parse before judging the exit status. grok reports argument errors as
     * a JSON error object on stdout and still exits 0, and reports others
     * with a non-zero status *and* the same object -- so the parsed message
     * is a better error than the status is in both cases. Only fall back to
     * the status when there is nothing parseable to report.
     */
    if (stdout_data != NULL && stdout_data[0] != '\0')
    {
        klass = AI_CLI_CLIENT_GET_CLASS(data->client);
        response = klass->parse_json_output(AI_CLI_CLIENT(data->client),
                                            stdout_data, &error);

        if (response != NULL)
        {
            /*
             * If the model only made tool calls without synthesizing text,
             * attempt a follow-up prompt asking it to summarize; fall back
             * to the generic tool_summary message if the retry can't start.
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

                /* Retry couldn't start — inject the summary as text */
                g_warning("grok-build: re-prompt could not start, using "
                          "fallback text");
                {
                    g_autoptr(AiTextContent) tc = ai_text_content_new(
                        data->client->last_tool_summary);
                    ai_response_add_content_block(response,
                        (AiContentBlock *)g_steal_pointer(&tc));
                }
            }

            g_task_return_pointer(data->task, response, g_object_unref);
            chat_async_data_free(data);
            return;
        }

        /* Parsed as an error — report that, not the exit status. */
        g_task_return_error(data->task, g_steal_pointer(&error));
        chat_async_data_free(data);
        return;
    }

    /* Nothing on stdout: the exit status is all we have. */
    if (!g_subprocess_get_successful(data->subprocess))
    {
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_CLI_EXECUTION,
                                "CLI exited with status %d: %s",
                                g_subprocess_get_exit_status(data->subprocess),
                                (stderr_data != NULL && stderr_data[0] != '\0')
                                ? stderr_data : "Unknown error");
    }
    else
    {
        g_task_return_new_error(data->task, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR,
                                "CLI produced no output");
    }

    chat_async_data_free(data);
}

static void
ai_grok_build_client_chat_async(
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiGrokBuildClient *self = AI_GROK_BUILD_CLIENT(provider);
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(self);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *executable = NULL;
    g_auto(GStrv) argv = NULL;
    gchar *stdin_data = NULL;
    g_autoptr(GSubprocess) subprocess = NULL;
    ChatAsyncData *data;
    GTask *task;
    GSubprocessFlags flags;

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

    /* Build stdin data if the subclass provides it */
    if (klass->build_stdin != NULL)
    {
        stdin_data = klass->build_stdin(AI_CLI_CLIENT(self), messages);
    }

    /* Replace the placeholder with the resolved executable path */
    g_free(argv[0]);
    argv[0] = g_steal_pointer(&executable);

    /* Spawn subprocess — add STDIN_PIPE if we have stdin data */
    flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE;
    if (stdin_data != NULL)
    {
        flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
    }

    subprocess = ai_cli_client_spawn(AI_CLI_CLIENT(self),
                                     (const gchar * const *)argv,
                                     flags, &error);
    if (subprocess == NULL)
    {
        g_free(stdin_data);
        g_task_return_error(task, g_steal_pointer(&error));
        g_object_unref(task);
        return;
    }

    /* Set up callback data */
    data = g_slice_new0(ChatAsyncData);
    data->client = g_object_ref(self);
    data->task = task;
    data->subprocess = g_object_ref(subprocess);
    data->stdin_data = stdin_data;  /* ownership transferred */

    /* Start async communication — pipe the prompt via stdin */
    g_subprocess_communicate_utf8_async(subprocess, data->stdin_data,
                                        cancellable,
                                        on_chat_communicate_complete, data);
}

static AiResponse *
ai_grok_build_client_chat_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_grok_build_client_list_models_async(
    AiProvider          *provider,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    GList *models = NULL;

    (void)cancellable;

    /*
     * Static list. `grok models` is the authoritative answer and needs a
     * network round trip; these are the ids the CLI ships with.
     */
    task = g_task_new(provider, NULL, callback, user_data);

    models = g_list_append(models, g_strdup(AI_GROK_BUILD_MODEL_GROK_4_6));
    models = g_list_append(models, g_strdup(AI_GROK_BUILD_MODEL_GROK_4_5));

    g_task_return_pointer(task, models, NULL);
    g_object_unref(task);
}

static GList *
ai_grok_build_client_list_models_finish(
    AiProvider    *provider,
    GAsyncResult  *result,
    GError       **error
){
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
ai_grok_build_client_provider_init(AiProviderInterface *iface)
{
    iface->get_provider_type = ai_grok_build_client_get_provider_type;
    iface->get_name = ai_grok_build_client_get_name;
    iface->get_default_model = ai_grok_build_client_get_default_model;
    iface->chat_async = ai_grok_build_client_chat_async;
    iface->chat_finish = ai_grok_build_client_chat_finish;
    iface->list_models_async = ai_grok_build_client_list_models_async;
    iface->list_models_finish = ai_grok_build_client_list_models_finish;
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
ai_grok_build_client_chat_stream_async(
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
ai_grok_build_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    return ai_cli_client_stream_run_finish(AI_CLI_CLIENT(streamable),
                                           result, error);
}

static void
ai_grok_build_client_streamable_init(AiStreamableInterface *iface)
{
    iface->chat_stream_async = ai_grok_build_client_chat_stream_async;
    iface->chat_stream_finish = ai_grok_build_client_chat_stream_finish;
}

/*
 * Public API
 */

/**
 * ai_grok_build_client_new:
 *
 * Creates a new #AiGrokBuildClient.
 * The grok CLI must be available in PATH or specified via the
 * %GROK_PATH environment variable.
 *
 * Returns: (transfer full): a new #AiGrokBuildClient
 */
AiGrokBuildClient *
ai_grok_build_client_new(void)
{
    g_autoptr(AiGrokBuildClient) self = g_object_new(AI_TYPE_GROK_BUILD_CLIENT,
                                                     NULL);

    return (AiGrokBuildClient *)g_steal_pointer(&self);
}

/**
 * ai_grok_build_client_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiGrokBuildClient with the specified configuration.
 *
 * Returns: (transfer full): a new #AiGrokBuildClient
 */
AiGrokBuildClient *
ai_grok_build_client_new_with_config(AiConfig *config)
{
    g_autoptr(AiGrokBuildClient) self = g_object_new(AI_TYPE_GROK_BUILD_CLIENT,
                                                     "config", config,
                                                     NULL);

    return (AiGrokBuildClient *)g_steal_pointer(&self);
}

/**
 * ai_grok_build_client_get_total_cost:
 * @self: an #AiGrokBuildClient
 *
 * Gets the total cost in USD from the last response.
 *
 * Returns: the total cost in USD, or 0.0 if not available
 */
gdouble
ai_grok_build_client_get_total_cost(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), 0.0);

    return self->total_cost;
}

/**
 * ai_grok_build_client_get_skip_permissions:
 * @self: an #AiGrokBuildClient
 *
 * Gets whether the CLI runs with `--permission-mode bypassPermissions`.
 *
 * Returns: %TRUE if skip permissions is enabled
 */
gboolean
ai_grok_build_client_get_skip_permissions(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), FALSE);

    return self->skip_permissions;
}

/**
 * ai_grok_build_client_set_skip_permissions:
 * @self: an #AiGrokBuildClient
 * @skip: whether to bypass tool-use approval
 *
 * Sets whether to run the grok CLI with `--permission-mode
 * bypassPermissions`. When enabled the CLI never prompts for tool-use
 * approval, allowing fully autonomous operation.
 */
void
ai_grok_build_client_set_skip_permissions(
    AiGrokBuildClient *self,
    gboolean           skip
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (self->skip_permissions == skip)
        return;

    self->skip_permissions = skip;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SKIP_PERMISSIONS]);
}

/**
 * ai_grok_build_client_get_permission_mode:
 * @self: an #AiGrokBuildClient
 *
 * Returns: (transfer none) (nullable): the configured permission mode
 */
const gchar *
ai_grok_build_client_get_permission_mode(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), NULL);

    return self->permission_mode;
}

/**
 * ai_grok_build_client_set_permission_mode:
 * @self: an #AiGrokBuildClient
 * @mode: (nullable): a grok permission mode, or %NULL to clear
 *
 * Sets the CLI's `--permission-mode`. Valid modes are "default",
 * "acceptEdits", "auto", "dontAsk", "bypassPermissions" and "plan";
 * anything else is dropped with a warning when the argv is built.
 */
void
ai_grok_build_client_set_permission_mode(
    AiGrokBuildClient *self,
    const gchar       *mode
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (g_strcmp0(self->permission_mode, mode) == 0)
        return;

    g_free(self->permission_mode);
    self->permission_mode = g_strdup(mode);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PERMISSION_MODE]);
}

/**
 * ai_grok_build_client_get_allowed_tools:
 * @self: an #AiGrokBuildClient
 *
 * Returns: (transfer none) (nullable): the comma-separated allow rules
 */
const gchar *
ai_grok_build_client_get_allowed_tools(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), NULL);

    return self->allowed_tools;
}

/**
 * ai_grok_build_client_set_allowed_tools:
 * @self: an #AiGrokBuildClient
 * @tools: (nullable): comma-separated permission rules, or %NULL to clear
 *
 * Sets the rules passed as `--allow`. Each comma-separated item becomes
 * its own `--allow <rule>` pair.
 */
void
ai_grok_build_client_set_allowed_tools(
    AiGrokBuildClient *self,
    const gchar       *tools
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (g_strcmp0(self->allowed_tools, tools) == 0)
        return;

    g_free(self->allowed_tools);
    self->allowed_tools = g_strdup(tools);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ALLOWED_TOOLS]);
}

/**
 * ai_grok_build_client_get_disallowed_tools:
 * @self: an #AiGrokBuildClient
 *
 * Returns: (transfer none) (nullable): the comma-separated deny rules
 */
const gchar *
ai_grok_build_client_get_disallowed_tools(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), NULL);

    return self->disallowed_tools;
}

/**
 * ai_grok_build_client_set_disallowed_tools:
 * @self: an #AiGrokBuildClient
 * @tools: (nullable): comma-separated permission rules, or %NULL to clear
 *
 * Sets the rules passed as `--deny`.
 */
void
ai_grok_build_client_set_disallowed_tools(
    AiGrokBuildClient *self,
    const gchar       *tools
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (g_strcmp0(self->disallowed_tools, tools) == 0)
        return;

    g_free(self->disallowed_tools);
    self->disallowed_tools = g_strdup(tools);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_DISALLOWED_TOOLS]);
}

/**
 * ai_grok_build_client_get_sandbox:
 * @self: an #AiGrokBuildClient
 *
 * Returns: (transfer none) (nullable): the configured sandbox profile
 */
const gchar *
ai_grok_build_client_get_sandbox(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), NULL);

    return self->sandbox;
}

/**
 * ai_grok_build_client_set_sandbox:
 * @self: an #AiGrokBuildClient
 * @profile: (nullable): a sandbox profile name, or %NULL to clear
 *
 * Sets the profile passed as `--sandbox`. The profile must already be
 * defined in `~/.grok/sandbox.toml` or `.grok/sandbox.toml` — grok refuses
 * to start on an unknown name rather than running unsandboxed.
 */
void
ai_grok_build_client_set_sandbox(
    AiGrokBuildClient *self,
    const gchar       *profile
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (g_strcmp0(self->sandbox, profile) == 0)
        return;

    g_free(self->sandbox);
    self->sandbox = g_strdup(profile);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SANDBOX]);
}

/**
 * ai_grok_build_client_get_max_turns:
 * @self: an #AiGrokBuildClient
 *
 * Returns: the configured turn budget, or 0 when unset
 */
gint
ai_grok_build_client_get_max_turns(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), 0);

    return self->max_turns;
}

/**
 * ai_grok_build_client_set_max_turns:
 * @self: an #AiGrokBuildClient
 * @max_turns: the maximum number of agent turns, or 0 to omit the flag
 *
 * Bounds how many turns the agent may take before it must stop.
 */
void
ai_grok_build_client_set_max_turns(
    AiGrokBuildClient *self,
    gint               max_turns
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));
    g_return_if_fail(max_turns >= 0);

    if (self->max_turns == max_turns)
        return;

    self->max_turns = max_turns;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAX_TURNS]);
}

/**
 * ai_grok_build_client_get_agent:
 * @self: an #AiGrokBuildClient
 *
 * Returns: (transfer none) (nullable): the configured agent
 */
const gchar *
ai_grok_build_client_get_agent(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), NULL);

    return self->agent;
}

/**
 * ai_grok_build_client_set_agent:
 * @self: an #AiGrokBuildClient
 * @agent: (nullable): an agent name or definition path, or %NULL to clear
 *
 * Sets the agent passed as `--agent`.
 */
void
ai_grok_build_client_set_agent(
    AiGrokBuildClient *self,
    const gchar       *agent
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (g_strcmp0(self->agent, agent) == 0)
        return;

    g_free(self->agent);
    self->agent = g_strdup(agent);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_AGENT]);
}

/**
 * ai_grok_build_client_get_rules:
 * @self: an #AiGrokBuildClient
 *
 * Returns: (transfer none) (nullable): the configured extra rules
 */
const gchar *
ai_grok_build_client_get_rules(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), NULL);

    return self->rules;
}

/**
 * ai_grok_build_client_set_rules:
 * @self: an #AiGrokBuildClient
 * @rules: (nullable): extra rules, or %NULL to clear
 *
 * Sets rules appended to the system prompt via `--rules`. Unlike
 * #AiCliClient:system-prompt these are additive rather than a replacement,
 * and they are re-sent when a session is resumed.
 */
void
ai_grok_build_client_set_rules(
    AiGrokBuildClient *self,
    const gchar       *rules
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (g_strcmp0(self->rules, rules) == 0)
        return;

    g_free(self->rules);
    self->rules = g_strdup(rules);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RULES]);
}

/**
 * ai_grok_build_client_get_disable_web_search:
 * @self: an #AiGrokBuildClient
 *
 * Returns: %TRUE if the web search and fetch tools are disabled
 */
gboolean
ai_grok_build_client_get_disable_web_search(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), FALSE);

    return self->disable_web_search;
}

/**
 * ai_grok_build_client_set_disable_web_search:
 * @self: an #AiGrokBuildClient
 * @disable: whether to remove the web tools
 *
 * Sets whether to pass `--disable-web-search`, removing the web search and
 * web fetch tools from the session.
 */
void
ai_grok_build_client_set_disable_web_search(
    AiGrokBuildClient *self,
    gboolean           disable
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (self->disable_web_search == disable)
        return;

    self->disable_web_search = disable;
    g_object_notify_by_pspec(G_OBJECT(self),
                             properties[PROP_DISABLE_WEB_SEARCH]);
}

/**
 * ai_grok_build_client_get_verbatim:
 * @self: an #AiGrokBuildClient
 *
 * Returns: %TRUE if the prompt is sent exactly as given
 */
gboolean
ai_grok_build_client_get_verbatim(AiGrokBuildClient *self)
{
    g_return_val_if_fail(AI_IS_GROK_BUILD_CLIENT(self), TRUE);

    return self->verbatim;
}

/**
 * ai_grok_build_client_set_verbatim:
 * @self: an #AiGrokBuildClient
 * @verbatim: whether to pass --verbatim
 *
 * Sets whether the prompt is sent exactly as given. Enabled by default:
 * a caller's prompt is text, and text that happens to begin with "/" must
 * not be taken for a slash command.
 */
void
ai_grok_build_client_set_verbatim(
    AiGrokBuildClient *self,
    gboolean           verbatim
){
    g_return_if_fail(AI_IS_GROK_BUILD_CLIENT(self));

    if (self->verbatim == verbatim)
        return;

    self->verbatim = verbatim;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_VERBATIM]);
}
