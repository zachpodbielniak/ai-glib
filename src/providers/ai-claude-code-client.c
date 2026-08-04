/*
 * ai-claude-code-client.c - Claude Code CLI client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "providers/ai-claude-code-client.h"
#include "providers/ai-claude-code-client-internal.h"
#include "providers/ai-claude-launch.h"
#include "core/ai-error.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"

/*
 * Private structure for AiClaudeCodeClient.
 */
struct _AiClaudeCodeClient
{
    AiCliClient parent_instance;

    gdouble  total_cost;
    gboolean skip_permissions;
    gchar   *mcp_config_path;    /* nullable: --mcp-config <path> */
    gint     last_input_tokens;  /* tracks input tokens for compaction detection */

    /*
     * Tool access short of --dangerously-skip-permissions. Each is emitted
     * only when set, so an unconfigured client builds the same argv it
     * always did.
     */
    gchar   *permission_mode;
    gchar   *allowed_tools;
    gchar   *disallowed_tools;
    gchar   *additional_directories;

    /* Cached summary for the re-prompt fallback when the AI
     * produces no text (empty "result" with tool use only). */
    gchar *last_tool_summary;
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
    PROP_SKIP_PERMISSIONS,
    PROP_MCP_CONFIG_PATH,
    PROP_PERMISSION_MODE,
    PROP_ALLOWED_TOOLS,
    PROP_DISALLOWED_TOOLS,
    PROP_ADDITIONAL_DIRECTORIES,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/*
 * Signal IDs for AiClaudeCodeClient-specific events.
 */
enum
{
    SIGNAL_CONTEXT_COMPACTED,
    N_CC_SIGNALS
};

static guint cc_signals[N_CC_SIGNALS];

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
        case PROP_SKIP_PERMISSIONS:
            g_value_set_boolean(value, self->skip_permissions);
            break;
        case PROP_MCP_CONFIG_PATH:
            g_value_set_string(value, self->mcp_config_path);
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
        case PROP_ADDITIONAL_DIRECTORIES:
            g_value_set_string(value, self->additional_directories);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_claude_code_client_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(object);

    switch (prop_id)
    {
        case PROP_SKIP_PERMISSIONS:
            self->skip_permissions = g_value_get_boolean(value);
            break;
        case PROP_MCP_CONFIG_PATH:
            g_free(self->mcp_config_path);
            self->mcp_config_path = g_value_dup_string(value);
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
        case PROP_ADDITIONAL_DIRECTORIES:
            g_free(self->additional_directories);
            self->additional_directories = g_value_dup_string(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * The permission modes the claude CLI accepts. Validated here rather than
 * passed straight through so a typo produces one clear warning instead of a
 * subprocess that exits non-zero with the CLI's own usage text.
 */
static const gchar *AI_CLAUDE_PERMISSION_MODES[] = {
    "acceptEdits", "auto", "bypassPermissions", "manual", "dontAsk", "plan",
    NULL
};

static gboolean
permission_mode_is_valid(const gchar *mode)
{
    gsize i;

    for (i = 0; AI_CLAUDE_PERMISSION_MODES[i] != NULL; i++)
    {
        if (g_strcmp0(mode, AI_CLAUDE_PERMISSION_MODES[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Append a comma-separated value as one flag followed by each item as its
 * own argv word, which is the shape --allowedTools and --add-dir take.
 * Empty items are dropped; an all-empty list emits nothing at all rather
 * than a dangling flag.
 */
static void
emit_list_flag(GPtrArray *args, const gchar *flag, const gchar *csv)
{
    g_auto(GStrv) parts = NULL;
    gsize i;
    gboolean emitted_flag = FALSE;

    if (csv == NULL || csv[0] == '\0')
        return;

    parts = g_strsplit(csv, ",", -1);

    for (i = 0; parts[i] != NULL; i++)
    {
        g_strstrip(parts[i]);
        if (parts[i][0] == '\0')
            continue;

        if (!emitted_flag)
        {
            g_ptr_array_add(args, g_strdup(flag));
            emitted_flag = TRUE;
        }

        g_ptr_array_add(args, g_strdup(parts[i]));
    }
}

/*
 * Emit the tool-permission arguments shared by the one-shot and resume
 * argv builders.
 *
 * --dangerously-skip-permissions wins when both are set: the two say
 * different things about the same session and the CLI should not be left to
 * arbitrate. Saying so is the point -- a caller that set a narrow mode and
 * silently got a full bypass is exactly the failure worth surfacing.
 */
static void
emit_permission_args(AiClaudeCodeClient *self, GPtrArray *args)
{
    if (self->skip_permissions)
    {
        if (self->permission_mode != NULL && self->permission_mode[0] != '\0')
        {
            g_warning("claude-code: skip-permissions and permission-mode '%s' "
                      "are both set; using --dangerously-skip-permissions",
                      self->permission_mode);
        }

        g_ptr_array_add(args, g_strdup("--dangerously-skip-permissions"));
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
            g_warning("claude-code: unknown permission mode '%s'; omitting "
                      "the flag. Valid modes: acceptEdits, auto, "
                      "bypassPermissions, manual, dontAsk, plan",
                      self->permission_mode);
        }
    }

    emit_list_flag(args, "--allowedTools", self->allowed_tools);
    emit_list_flag(args, "--disallowedTools", self->disallowed_tools);
    emit_list_flag(args, "--add-dir", self->additional_directories);
}

/*
 * Spawn with the client's working directory applied.
 *
 * AiCliClient already does this (ai-cli-client.c), but this client builds and
 * spawns its own argv, and for a long time it did so with a bare
 * g_subprocess_newv() -- so working-directory was accepted, stored, and had
 * no effect. Callers that named a directory to bound what the CLI could
 * reach got the directory the parent process happened to be started in.
 */
static GSubprocess *
claude_code_spawn(
    AiClaudeCodeClient   *self,
    const gchar * const  *argv,
    GSubprocessFlags      flags,
    GError              **error
){
    const gchar *cwd;

    cwd = ai_cli_client_get_working_directory(AI_CLI_CLIENT(self));

    if (cwd != NULL && cwd[0] != '\0')
    {
        g_autoptr(GSubprocessLauncher) launcher = NULL;

        launcher = g_subprocess_launcher_new(flags);
        g_subprocess_launcher_set_cwd(launcher, cwd);

        return g_subprocess_launcher_spawnv(launcher, argv, error);
    }

    return g_subprocess_newv(argv, flags, error);
}

/*
 * Get the executable path for the claude CLI.
 *
 * For a normal model this is the CLAUDE_CODE_PATH env override, else
 * "claude". For an "ollama/<model>" transport model it is the launcher
 * binary instead (OLLAMA_PATH env, else "ollama") -- the base CLI pipeline
 * resolves this and overwrites argv[0], so it must agree with the program
 * token build_argv emits. Both decisions live in ai_claude_launch_*.
 */
static gchar *
ai_claude_code_client_get_executable_path(AiCliClient *client)
{
    return ai_claude_launch_executable_name(ai_cli_client_get_model(client));
}

/*
 * Build command line arguments for the claude CLI.
 *
 * Non-streaming: claude --print --output-format json --model <model> --system-prompt "..." "prompt"
 * Streaming: claude --print --output-format stream-json --verbose --model <model> "prompt"
 */
gchar **
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
    g_autofree gchar *program = NULL;

    (void)max_tokens;  /* Claude Code CLI doesn't have a max tokens flag */

    model = ai_cli_client_get_model(client);

    args = g_ptr_array_new();

    /*
     * Program token + (in Ollama mode) the `ollama launch claude --model
     * <m> --` wrapper. The program token is a placeholder the base CLI
     * pipeline overwrites with the resolved executable path; everything
     * appended below is the claude arg tail (rides after the `--` in
     * Ollama mode).
     */
    program = ai_claude_launch_executable_name(model);
    ai_claude_launch_emit_tokens(args, model, program);

    /* Print mode (required for non-interactive use) */
    g_ptr_array_add(args, g_strdup("--print"));

    /* Tool access: skip-permissions, or a permission mode and allow-lists. */
    emit_permission_args(self, args);

    /* Extra MCP servers for this session. */
    if (self->mcp_config_path != NULL && self->mcp_config_path[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--mcp-config"));
        g_ptr_array_add(args, g_strdup(self->mcp_config_path));
    }

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

    /*
     * Model. Omitted in Ollama mode -- there the model is carried solely
     * by `ollama launch --model <suffix>` (emitted above), and re-passing
     * it to claude would be wrong.
     */
    if (ai_claude_launch_should_emit_claude_model(model))
    {
        const gchar *claude_model =
            (model != NULL) ? model : AI_CLAUDE_CODE_DEFAULT_MODEL;
        g_ptr_array_add(args, g_strdup("--model"));
        g_ptr_array_add(args, g_strdup(claude_model));
    }

    /* Session management - resolve session_id before system prompt */
    persist = ai_cli_client_get_session_persistence(client);
    session_id = ai_cli_client_get_session_id(client);
    if (persist && session_id != NULL && session_id[0] != '\0')
    {
        /*
         * Resume an existing session. Do NOT pass --system-prompt
         * because the session already has it from the initial call.
         * Re-sending it wastes tokens and re-injects the full prompt.
         */
        g_ptr_array_add(args, g_strdup("--resume"));
        g_ptr_array_add(args, g_strdup(session_id));
    }
    else
    {
        /*
         * New session — pass the system prompt to prime it.
         * Only sent on the first call; subsequent calls use --resume.
         */
        if (system_prompt != NULL && system_prompt[0] != '\0')
        {
            g_ptr_array_add(args, g_strdup("--system-prompt"));
            g_ptr_array_add(args, g_strdup(system_prompt));
        }
    }
    if (!persist)
    {
        g_ptr_array_add(args, g_strdup("--no-session-persistence"));
    }

    /* Effort level */
    {
        const gchar *effort = ai_cli_client_get_effort_level(client);
        if (effort != NULL && effort[0] != '\0')
        {
            g_ptr_array_add(args, g_strdup("--effort"));
            g_ptr_array_add(args, g_strdup(effort));
        }
    }

    /*
     * Prompt is piped via stdin (build_stdin) to avoid ARG_MAX limits
     * on large prompts. Do not append it to argv.
     */

    /* NULL terminate */
    g_ptr_array_add(args, NULL);

    return (gchar **)g_ptr_array_free(args, FALSE);
}

/*
 * Build the prompt string to pipe via stdin to the claude CLI.
 * This avoids the ARG_MAX limit for large prompts. The claude CLI
 * reads from stdin when no positional prompt argument is given.
 */
static gchar *
ai_claude_code_client_build_stdin(
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

    /* Instruct the AI to always produce a plain text response */
    g_string_append(prompt,
        "\n\nIMPORTANT: Always include a plain text response. "
        "Tool use is fine, but you MUST provide a text summary of "
        "your work when finished. Never end your turn on tool calls alone.");

    return g_string_free(prompt, FALSE);
}

/*
 * check_and_emit_compaction:
 * @self: an #AiClaudeCodeClient
 * @input_tokens: the input token count from the current response
 *
 * Compares the current input token count against the previously
 * stored value. If the count has dropped, a context window
 * compaction is inferred and the "context-compacted" signal is
 * emitted. The stored value is always updated afterwards.
 */
static void
check_and_emit_compaction(
    AiClaudeCodeClient *self,
    gint                input_tokens
){
    if (self->last_input_tokens > 0 && input_tokens < self->last_input_tokens)
    {
        g_signal_emit(self, cc_signals[SIGNAL_CONTEXT_COMPACTED], 0,
                      self->last_input_tokens, input_tokens);
    }
    self->last_input_tokens = input_tokens;
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
    else
    {
        /*
         * Empty result — Claude finished with tool calls but produced no
         * text summary. Store a flag so the completion callback can attempt
         * a re-prompt for synthesized text. If that also fails we return
         * this generic message as a last resort.
         */
        g_free(self->last_tool_summary);
        self->last_tool_summary = g_strdup(
            "(completed tool operations — no text summary was provided)");
    }

    /* Parse usage and check for context compaction */
    if (json_object_has_member(obj, "usage"))
    {
        JsonObject *usage_obj = json_object_get_object_member(obj, "usage");
        gint input_tokens = json_object_get_int_member_with_default(usage_obj, "input_tokens", 0);
        gint output_tokens = json_object_get_int_member_with_default(usage_obj, "output_tokens", 0);
        g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

        ai_response_set_usage(response, usage);
        check_and_emit_compaction(self, input_tokens);
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
                /* Flat shape: {"message": {"type": "text", "text": ...}} */
                const gchar *text = json_object_get_string_member_with_default(msg_obj, "text", "");
                *delta_text = g_strdup(text);
            }
            else if (json_object_has_member(msg_obj, "content"))
            {
                /*
                 * What the CLI actually emits: an Anthropic message whose
                 * "type" is "message" and whose text lives in a "content"
                 * array of blocks.  Reading msg_obj->text instead found
                 * nothing on every event, so a streamed reply arrived
                 * empty while the run itself reported success.
                 *
                 * Only "text" blocks are emitted as deltas.  A "thinking"
                 * block is not the answer, and "tool_use" is handled by
                 * the caller.
                 */
                JsonNode *content = json_object_get_member(msg_obj, "content");

                if (content != NULL && JSON_NODE_HOLDS_ARRAY(content))
                {
                    JsonArray *blocks = json_node_get_array(content);
                    guint n = json_array_get_length(blocks);
                    GString *acc = g_string_new(NULL);
                    guint i;

                    for (i = 0; i < n; i++)
                    {
                        JsonNode *bn = json_array_get_element(blocks, i);
                        JsonObject *b;
                        const gchar *bt;

                        if (bn == NULL || !JSON_NODE_HOLDS_OBJECT(bn)) continue;
                        b = json_node_get_object(bn);
                        bt = json_object_get_string_member_with_default(b, "type", "");
                        if (g_strcmp0(bt, "text") == 0)
                        {
                            g_string_append(
                                acc,
                                json_object_get_string_member_with_default(
                                    b, "text", ""));
                        }
                    }

                    if (acc->len > 0)
                        *delta_text = g_string_free(acc, FALSE);
                    else
                        g_string_free(acc, TRUE);
                }
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

        /* Update usage and check for context compaction */
        if (json_object_has_member(obj, "usage"))
        {
            JsonObject *usage_obj = json_object_get_object_member(obj, "usage");
            gint input_tokens = json_object_get_int_member_with_default(usage_obj, "input_tokens", 0);
            gint output_tokens = json_object_get_int_member_with_default(usage_obj, "output_tokens", 0);
            g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

            ai_response_set_usage(response, usage);
            check_and_emit_compaction(self, input_tokens);
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

/*
 * Destructor — free instance data.
 */
static void
ai_claude_code_client_finalize(GObject *object)
{
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(object);

    g_free(self->last_tool_summary);
    g_free(self->mcp_config_path);
    g_free(self->permission_mode);
    g_free(self->allowed_tools);
    g_free(self->disallowed_tools);
    g_free(self->additional_directories);

    G_OBJECT_CLASS(ai_claude_code_client_parent_class)->finalize(object);
}

static void
ai_claude_code_client_class_init(AiClaudeCodeClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiCliClientClass *cli_class = AI_CLI_CLIENT_CLASS(klass);

    object_class->finalize     = ai_claude_code_client_finalize;
    object_class->get_property = ai_claude_code_client_get_property;
    object_class->set_property = ai_claude_code_client_set_property;

    /* Override virtual methods */
    cli_class->get_executable_path = ai_claude_code_client_get_executable_path;
    cli_class->build_argv = ai_claude_code_client_build_argv;
    cli_class->build_stdin = ai_claude_code_client_build_stdin;
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

    /**
     * AiClaudeCodeClient:skip-permissions:
     *
     * Whether to pass --dangerously-skip-permissions to the claude CLI.
     * When enabled, the CLI will not prompt for tool-use approval,
     * allowing fully autonomous operation.
     */
    properties[PROP_SKIP_PERMISSIONS] =
        g_param_spec_boolean("skip-permissions",
                             "Skip Permissions",
                             "Whether to pass --dangerously-skip-permissions to the CLI",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /*
     * Path to an extra MCP server config, emitted as `--mcp-config
     * <path>`.  Deliberately additive: without --strict-mcp-config the
     * session keeps whatever servers its workspace .mcp.json already
     * declares and gains these as well, so pointing this at a
     * per-session file cannot clobber a user-authored one.
     */
    properties[PROP_MCP_CONFIG_PATH] =
        g_param_spec_string("mcp-config-path",
                            "MCP Config Path",
                            "Path passed to claude as --mcp-config",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:permission-mode:
     *
     * The CLI's --permission-mode. Grants tool use without the blanket
     * bypass of #AiClaudeCodeClient:skip-permissions -- "acceptEdits" in
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
                            "CLI --permission-mode (acceptEdits, auto, "
                            "bypassPermissions, manual, dontAsk, plan)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:allowed-tools:
     *
     * Comma-separated tool names for --allowedTools, e.g. "Read,Edit,Write".
     * Each becomes its own argv word.
     */
    properties[PROP_ALLOWED_TOOLS] =
        g_param_spec_string("allowed-tools",
                            "Allowed Tools",
                            "Comma-separated tool names for --allowedTools",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:disallowed-tools:
     *
     * Comma-separated tool names for --disallowedTools.
     */
    properties[PROP_DISALLOWED_TOOLS] =
        g_param_spec_string("disallowed-tools",
                            "Disallowed Tools",
                            "Comma-separated tool names for --disallowedTools",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:additional-directories:
     *
     * Comma-separated paths for --add-dir, for trees the model must read
     * that sit outside the working directory.
     */
    properties[PROP_ADDITIONAL_DIRECTORIES] =
        g_param_spec_string("additional-directories",
                            "Additional Directories",
                            "Comma-separated paths for --add-dir",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);

    /**
     * AiClaudeCodeClient::context-compacted:
     * @self: the client that detected compaction
     * @previous_tokens: the input token count before compaction
     * @current_tokens: the input token count after compaction
     *
     * Emitted when the context window appears to have been compacted.
     * Detected by inference: input_tokens dropped between consecutive
     * calls on the same session. This fires during both synchronous
     * and streaming chat calls, from within the response parsing path.
     */
    cc_signals[SIGNAL_CONTEXT_COMPACTED] =
        g_signal_new("context-compacted",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 2,
                     G_TYPE_INT,
                     G_TYPE_INT);
}

static void
ai_claude_code_client_init(AiClaudeCodeClient *self)
{
    self->total_cost = 0.0;
    self->skip_permissions = FALSE;
    self->last_input_tokens = -1;

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
    gchar              *stdin_data;
} ChatAsyncData;

static void
chat_async_data_free(ChatAsyncData *data)
{
    /*
     * g_task_return_*() does NOT consume the reference chat_async() took
     * from g_task_new(); the async function owns it until the operation is
     * finished with. The early-error paths unref directly, so only the
     * completion paths -- which hand the task to `data` -- came through
     * here, and every one of them leaked a GTask and its result.
     *
     * Safe against the retry hand-off, which sets data->task to NULL before
     * freeing precisely so the task survives into the second attempt.
     *
     * Every other provider in this library already does this.
     */
    g_clear_object(&data->task);
    g_clear_object(&data->client);
    g_clear_object(&data->subprocess);
    g_clear_pointer(&data->stdin_data, g_free);
    g_slice_free(ChatAsyncData, data);
}

/*
 * Retry data — used when the AI made tool calls but produced no text.
 * We re-prompt asking for a plain-text summary; if that also fails we
 * fall back to the generic tool_summary string.
 */
typedef struct
{
    AiClaudeCodeClient *client;
    GTask              *task;
    GSubprocess        *subprocess;
    gchar              *tool_summary;
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
    g_warning("claude-code: re-prompt failed, using tool summary as fallback");

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
 * Attempt to re-prompt Claude for a plain-text summary of its tool work.
 * Returns TRUE if the retry subprocess was spawned (task ownership
 * transferred to the retry callback), FALSE if it could not start.
 * Requires an active session ID to resume the conversation.
 */
static gboolean
attempt_text_retry(
    AiClaudeCodeClient *client,
    GTask              *task,
    const gchar        *tool_summary
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
    /*
     * Program token (the already-resolved executable -- ollama in Ollama
     * mode, claude otherwise) plus, in Ollama mode, the
     * `launch claude --model <suffix> --` wrapper.
     */
    ai_claude_launch_emit_tokens(rargs, model, exe);
    g_ptr_array_add(rargs, g_strdup("--print"));
    emit_permission_args(client, rargs);
    if (client->mcp_config_path != NULL && client->mcp_config_path[0] != '\0')
    {
        g_ptr_array_add(rargs, g_strdup("--mcp-config"));
        g_ptr_array_add(rargs, g_strdup(client->mcp_config_path));
    }
    g_ptr_array_add(rargs, g_strdup("--output-format"));
    g_ptr_array_add(rargs, g_strdup("json"));
    /* claude's own --model is omitted in Ollama mode (see build_argv). */
    if (ai_claude_launch_should_emit_claude_model(model))
    {
        g_ptr_array_add(rargs, g_strdup("--model"));
        g_ptr_array_add(rargs,
            g_strdup(model ? model : AI_CLAUDE_CODE_DEFAULT_MODEL));
    }
    g_ptr_array_add(rargs, g_strdup("--resume"));
    g_ptr_array_add(rargs, g_strdup(sid));
    g_ptr_array_add(rargs, NULL);

    rproc = claude_code_spawn(
        client,
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

    g_warning("claude-code: no text in response, re-prompting for summary "
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
        gint error_code = AI_ERROR_CLI_EXECUTION;

        /* Check if this is a context-window-full error */
        if (stderr_data != NULL &&
            (strstr(stderr_data, "context") != NULL ||
             strstr(stderr_data, "window") != NULL ||
             strstr(stderr_data, "tokens") != NULL ||
             strstr(stderr_data, "max_tokens") != NULL ||
             strstr(stderr_data, "maximum tokens") != NULL))
        {
            error_code = AI_ERROR_CONTEXT_LENGTH_EXCEEDED;
        }

        g_task_return_new_error(data->task, AI_ERROR, error_code,
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
     * a follow-up prompt asking it to summarize; fall back to the generic
     * tool_summary message if the retry can't start.
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

        /* Retry couldn't start — inject tool_summary as text directly */
        g_warning("claude-code: re-prompt could not start, using fallback text");
        {
            g_autoptr(AiTextContent) tc = ai_text_content_new(
                data->client->last_tool_summary);
            ai_response_add_content_block(response,
                (AiContentBlock *)g_steal_pointer(&tc));
        }
    }

    g_task_return_pointer(data->task, response, g_object_unref);
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

    /* Build stdin data if subclass provides it */
    if (klass->build_stdin != NULL)
    {
        stdin_data = klass->build_stdin(AI_CLI_CLIENT(self), messages);
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

    subprocess = claude_code_spawn(self, (const gchar * const *)argv,
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

    /* Start async communication — pipe prompt via stdin */
    g_subprocess_communicate_utf8_async(subprocess, data->stdin_data,
                                        cancellable,
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

    /* Return static list of model aliases, then the pinned Claude 5 IDs */
    task = g_task_new(provider, NULL, callback, user_data);

    models = g_list_append(models, g_strdup(AI_CLAUDE_CODE_MODEL_FABLE));
    models = g_list_append(models, g_strdup(AI_CLAUDE_CODE_MODEL_OPUS));
    models = g_list_append(models, g_strdup(AI_CLAUDE_CODE_MODEL_SONNET));
    models = g_list_append(models, g_strdup(AI_CLAUDE_CODE_MODEL_HAIKU));
    models = g_list_append(models, g_strdup(AI_CLAUDE_CODE_MODEL_FABLE_5));
    models = g_list_append(models, g_strdup(AI_CLAUDE_CODE_MODEL_OPUS_5));
    models = g_list_append(models, g_strdup(AI_CLAUDE_CODE_MODEL_SONNET_5));

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
    gchar              *stdin_data;
} StreamAsyncData;

static void
stream_async_data_free(StreamAsyncData *data)
{
    g_clear_object(&data->task);
    g_clear_object(&data->client);
    g_clear_object(&data->subprocess);
    g_clear_object(&data->data_stream);
    g_clear_object(&data->cancellable);
    g_clear_object(&data->response);
    g_clear_pointer(&data->stdin_data, g_free);

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
    GOutputStream *stdin_stream;

    (void)source;
    (void)result;

    /*
     * Write stdin data to the subprocess if provided, then close
     * the stdin pipe so the CLI knows input is complete.
     */
    if (data->stdin_data != NULL)
    {
        stdin_stream = g_subprocess_get_stdin_pipe(data->subprocess);
        if (stdin_stream != NULL)
        {
            g_output_stream_write_all(stdin_stream,
                                       data->stdin_data,
                                       strlen(data->stdin_data),
                                       NULL, NULL, &error);
            g_output_stream_close(stdin_stream, NULL, NULL);
        }

        if (error != NULL)
        {
            g_task_return_error(data->task, g_steal_pointer(&error));
            stream_async_data_free(data);
            return;
        }
    }

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
    gchar *stdin_data = NULL;
    g_autoptr(GSubprocess) subprocess = NULL;
    StreamAsyncData *data;
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

    /* Build stdin data if subclass provides it */
    if (klass->build_stdin != NULL)
    {
        stdin_data = klass->build_stdin(AI_CLI_CLIENT(self), messages);
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

    subprocess = claude_code_spawn(self, (const gchar * const *)argv,
                                   flags, &error);
    if (subprocess == NULL)
    {
        g_free(stdin_data);
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
    data->stdin_data = stdin_data;  /* ownership transferred */

    /* Write stdin and start reading stdout */
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

/**
 * ai_claude_code_client_get_skip_permissions:
 * @self: an #AiClaudeCodeClient
 *
 * Gets whether --dangerously-skip-permissions is enabled.
 *
 * Returns: %TRUE if skip permissions is enabled
 */
gboolean
ai_claude_code_client_get_skip_permissions(AiClaudeCodeClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_CODE_CLIENT(self), FALSE);

    return self->skip_permissions;
}

/**
 * ai_claude_code_client_set_skip_permissions:
 * @self: an #AiClaudeCodeClient
 * @skip: whether to pass --dangerously-skip-permissions
 *
 * Sets whether to pass --dangerously-skip-permissions to the
 * claude CLI. When enabled, the CLI will not prompt for
 * tool-use approval, allowing fully autonomous operation.
 */
void
ai_claude_code_client_set_skip_permissions(
    AiClaudeCodeClient *self,
    gboolean            skip
){
    g_return_if_fail(AI_IS_CLAUDE_CODE_CLIENT(self));

    self->skip_permissions = skip;

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SKIP_PERMISSIONS]);
}

/**
 * ai_claude_code_client_get_mcp_config_path:
 * @self: an #AiClaudeCodeClient
 *
 * Returns: (transfer none) (nullable): the configured MCP config path
 *
 * Since: 0.2.0
 */
const gchar *
ai_claude_code_client_get_mcp_config_path(AiClaudeCodeClient *self)
{
    g_return_val_if_fail(AI_IS_CLAUDE_CODE_CLIENT(self), NULL);

    return self->mcp_config_path;
}

/**
 * ai_claude_code_client_set_mcp_config_path:
 * @self: an #AiClaudeCodeClient
 * @path: (nullable): path to an MCP server config, or %NULL to clear
 *
 * Makes the client pass `--mcp-config @path` to claude, adding those
 * servers on top of whatever the workspace already configures.
 *
 * Since: 0.2.0
 */
void
ai_claude_code_client_set_mcp_config_path(AiClaudeCodeClient *self,
                                          const gchar        *path)
{
    g_return_if_fail(AI_IS_CLAUDE_CODE_CLIENT(self));

    if (g_strcmp0(self->mcp_config_path, path) == 0)
        return;

    g_free(self->mcp_config_path);
    self->mcp_config_path = g_strdup(path);
    g_object_notify_by_pspec(G_OBJECT(self),
                             properties[PROP_MCP_CONFIG_PATH]);
}
