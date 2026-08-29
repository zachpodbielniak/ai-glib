/*
 * ai-claude-code-client.c - Claude Code CLI client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "providers/ai-claude-code-client.h"
#include "providers/ai-claude-code-client-internal.h"
#include "providers/ai-claude-launch.h"
#include "core/ai-cli-client-private.h"
#include "core/ai-error.h"
#include "core/ai-session-limit.h"
#include "core/ai-event.h"
#include "model/ai-text-content.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

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
     * The account's session limit, seen in this run's output.
     *
     * A usage limit is not a failed request: the CLI never contacts the
     * API, writes one assistant message of its own naming no model, and
     * exits non-zero.  From the exit status alone that is
     * indistinguishable from any other failure, so it was retried at
     * once and every retry hit the same wall -- for a limit whose own
     * message said it would not clear for hours.
     *
     * Recorded as the output is parsed and read when the exit is
     * handled, because those are two different places and only the
     * first one can see it.
     */
    gboolean session_limited;
    gint64   session_limit_reset;   /* Unix seconds, or 0 if not stated */

    /*
     * Tool access short of --dangerously-skip-permissions. Each is emitted
     * only when set, so an unconfigured client builds the same argv it
     * always did.
     */
    gchar   *permission_mode;
    gchar   *allowed_tools;
    gchar   *disallowed_tools;
    gchar   *additional_directories;

    /*
     * The rest of what `claude --print` accepts. Each is emitted only when
     * set, so an unconfigured client builds the same argv it always did.
     */
    gchar   *agent;
    gchar   *agents_json;
    gchar   *append_system_prompt;
    gchar   *fallback_model;
    gchar   *json_schema;
    gchar   *settings;
    gchar   *setting_sources;
    gchar   *tools;
    gchar   *betas;
    gchar   *autocompact;
    gchar   *plugin_dirs;
    gchar   *plugin_urls;
    gchar   *debug_filter;
    gchar   *debug_file;
    gdouble  max_budget_usd;          /* 0 means "unset" */
    gboolean strict_mcp_config;
    gboolean disable_slash_commands;
    gboolean fork_session;
    gboolean include_partial_messages;
    gboolean include_hook_events;
    gboolean forward_subagent_text;
    gboolean exclude_dynamic_system_prompt_sections;
    gboolean debug;
    gboolean bare;
    gboolean safe_mode;
    gboolean continue_session;

    /* Cached summary for the re-prompt fallback when the AI
     * produces no text (empty "result" with tool use only). */
    gchar *last_tool_summary;

    /*
     * Where the two system prompts were spilled for the turn being
     * built, so they can be removed again.
     *
     * The client owns them rather than the turn: the re-prompt path
     * builds a second command line from the same client and re-emits
     * --append-system-prompt-file, so a file freed with the first
     * turn's data would be named to a process that had not read it yet.
     */
    gchar *system_prompt_path;
    gchar *append_system_prompt_path;
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
    PROP_AGENT,
    PROP_AGENTS_JSON,
    PROP_APPEND_SYSTEM_PROMPT,
    PROP_FALLBACK_MODEL,
    PROP_JSON_SCHEMA,
    PROP_SETTINGS,
    PROP_SETTING_SOURCES,
    PROP_TOOLS,
    PROP_BETAS,
    PROP_AUTOCOMPACT,
    PROP_PLUGIN_DIRS,
    PROP_PLUGIN_URLS,
    PROP_DEBUG_FILTER,
    PROP_DEBUG_FILE,
    PROP_MAX_BUDGET_USD,
    PROP_STRICT_MCP_CONFIG,
    PROP_DISABLE_SLASH_COMMANDS,
    PROP_FORK_SESSION,
    PROP_INCLUDE_PARTIAL_MESSAGES,
    PROP_INCLUDE_HOOK_EVENTS,
    PROP_FORWARD_SUBAGENT_TEXT,
    PROP_EXCLUDE_DYNAMIC_SYSTEM_PROMPT_SECTIONS,
    PROP_DEBUG,
    PROP_BARE,
    PROP_SAFE_MODE,
    PROP_CONTINUE_SESSION,
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
        case PROP_AGENT:
            g_value_set_string(value, self->agent);
            break;
        case PROP_AGENTS_JSON:
            g_value_set_string(value, self->agents_json);
            break;
        case PROP_APPEND_SYSTEM_PROMPT:
            g_value_set_string(value, self->append_system_prompt);
            break;
        case PROP_FALLBACK_MODEL:
            g_value_set_string(value, self->fallback_model);
            break;
        case PROP_JSON_SCHEMA:
            g_value_set_string(value, self->json_schema);
            break;
        case PROP_SETTINGS:
            g_value_set_string(value, self->settings);
            break;
        case PROP_SETTING_SOURCES:
            g_value_set_string(value, self->setting_sources);
            break;
        case PROP_TOOLS:
            g_value_set_string(value, self->tools);
            break;
        case PROP_BETAS:
            g_value_set_string(value, self->betas);
            break;
        case PROP_AUTOCOMPACT:
            g_value_set_string(value, self->autocompact);
            break;
        case PROP_PLUGIN_DIRS:
            g_value_set_string(value, self->plugin_dirs);
            break;
        case PROP_PLUGIN_URLS:
            g_value_set_string(value, self->plugin_urls);
            break;
        case PROP_DEBUG_FILTER:
            g_value_set_string(value, self->debug_filter);
            break;
        case PROP_DEBUG_FILE:
            g_value_set_string(value, self->debug_file);
            break;
        case PROP_MAX_BUDGET_USD:
            g_value_set_double(value, self->max_budget_usd);
            break;
        case PROP_STRICT_MCP_CONFIG:
            g_value_set_boolean(value, self->strict_mcp_config);
            break;
        case PROP_DISABLE_SLASH_COMMANDS:
            g_value_set_boolean(value, self->disable_slash_commands);
            break;
        case PROP_FORK_SESSION:
            g_value_set_boolean(value, self->fork_session);
            break;
        case PROP_INCLUDE_PARTIAL_MESSAGES:
            g_value_set_boolean(value, self->include_partial_messages);
            break;
        case PROP_INCLUDE_HOOK_EVENTS:
            g_value_set_boolean(value, self->include_hook_events);
            break;
        case PROP_FORWARD_SUBAGENT_TEXT:
            g_value_set_boolean(value, self->forward_subagent_text);
            break;
        case PROP_EXCLUDE_DYNAMIC_SYSTEM_PROMPT_SECTIONS:
            g_value_set_boolean(value,
                self->exclude_dynamic_system_prompt_sections);
            break;
        case PROP_DEBUG:
            g_value_set_boolean(value, self->debug);
            break;
        case PROP_BARE:
            g_value_set_boolean(value, self->bare);
            break;
        case PROP_SAFE_MODE:
            g_value_set_boolean(value, self->safe_mode);
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
        case PROP_AGENT:
            g_free(self->agent);
            self->agent = g_value_dup_string(value);
            break;
        case PROP_AGENTS_JSON:
            g_free(self->agents_json);
            self->agents_json = g_value_dup_string(value);
            break;
        case PROP_APPEND_SYSTEM_PROMPT:
            g_free(self->append_system_prompt);
            self->append_system_prompt = g_value_dup_string(value);
            break;
        case PROP_FALLBACK_MODEL:
            g_free(self->fallback_model);
            self->fallback_model = g_value_dup_string(value);
            break;
        case PROP_JSON_SCHEMA:
            g_free(self->json_schema);
            self->json_schema = g_value_dup_string(value);
            break;
        case PROP_SETTINGS:
            g_free(self->settings);
            self->settings = g_value_dup_string(value);
            break;
        case PROP_SETTING_SOURCES:
            g_free(self->setting_sources);
            self->setting_sources = g_value_dup_string(value);
            break;
        case PROP_TOOLS:
            g_free(self->tools);
            self->tools = g_value_dup_string(value);
            break;
        case PROP_BETAS:
            g_free(self->betas);
            self->betas = g_value_dup_string(value);
            break;
        case PROP_AUTOCOMPACT:
            g_free(self->autocompact);
            self->autocompact = g_value_dup_string(value);
            break;
        case PROP_PLUGIN_DIRS:
            g_free(self->plugin_dirs);
            self->plugin_dirs = g_value_dup_string(value);
            break;
        case PROP_PLUGIN_URLS:
            g_free(self->plugin_urls);
            self->plugin_urls = g_value_dup_string(value);
            break;
        case PROP_DEBUG_FILTER:
            g_free(self->debug_filter);
            self->debug_filter = g_value_dup_string(value);
            break;
        case PROP_DEBUG_FILE:
            g_free(self->debug_file);
            self->debug_file = g_value_dup_string(value);
            break;
        case PROP_MAX_BUDGET_USD:
            self->max_budget_usd = g_value_get_double(value);
            break;
        case PROP_STRICT_MCP_CONFIG:
            self->strict_mcp_config = g_value_get_boolean(value);
            break;
        case PROP_DISABLE_SLASH_COMMANDS:
            self->disable_slash_commands = g_value_get_boolean(value);
            break;
        case PROP_FORK_SESSION:
            self->fork_session = g_value_get_boolean(value);
            break;
        case PROP_INCLUDE_PARTIAL_MESSAGES:
            self->include_partial_messages = g_value_get_boolean(value);
            break;
        case PROP_INCLUDE_HOOK_EVENTS:
            self->include_hook_events = g_value_get_boolean(value);
            break;
        case PROP_FORWARD_SUBAGENT_TEXT:
            self->forward_subagent_text = g_value_get_boolean(value);
            break;
        case PROP_EXCLUDE_DYNAMIC_SYSTEM_PROMPT_SECTIONS:
            self->exclude_dynamic_system_prompt_sections =
                g_value_get_boolean(value);
            break;
        case PROP_DEBUG:
            self->debug = g_value_get_boolean(value);
            break;
        case PROP_BARE:
            self->bare = g_value_get_boolean(value);
            break;
        case PROP_SAFE_MODE:
            self->safe_mode = g_value_get_boolean(value);
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
            g_message("claude-code: skip-permissions and permission-mode '%s' "
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
            g_message("claude-code: unknown permission mode '%s'; omitting "
                      "the flag. Valid modes: acceptEdits, auto, "
                      "bypassPermissions, manual, dontAsk, plan",
                      self->permission_mode);
        }
    }

    emit_list_flag(args, "--allowedTools", self->allowed_tools);
    emit_list_flag(args, "--disallowedTools", self->disallowed_tools);
    emit_list_flag(args, "--add-dir", self->additional_directories);
    emit_list_flag(args, "--tools", self->tools);
}

/*
 * Append a comma-separated value as the flag repeated once per item,
 * which is the shape --plugin-dir and --plugin-url take (they accumulate
 * rather than accepting a list).
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
 * Remove whatever was spilled for the previous turn.
 *
 * Called before each spill and again at finalize, so at most one pair of
 * files exists per client at any moment and none survives it.  A daemon
 * runs one client per agent for weeks; "cleaned up when the process
 * exits" would be one file per agent per turn in /tmp until it did.
 */
static void
clear_prompt_spills(AiClaudeCodeClient *self)
{
    if (self->system_prompt_path != NULL)
    {
        g_unlink(self->system_prompt_path);
        g_clear_pointer(&self->system_prompt_path, g_free);
    }

    if (self->append_system_prompt_path != NULL)
    {
        g_unlink(self->append_system_prompt_path);
        g_clear_pointer(&self->append_system_prompt_path, g_free);
    }
}

/*
 * Write @text to a private temporary file and return its path.
 *
 * g_file_open_tmp() creates with 0600 and O_EXCL, which is what this
 * needs: the text is the agent's whole system prompt -- its identity,
 * its instructions and whatever the operator put in them -- and it is
 * being put in a directory every user on the machine can list.
 *
 * The caller owns the returned path *through* @slot; it is stored there
 * so clear_prompt_spills() can find it again.
 */
static const gchar *
spill_prompt(gchar **slot, const gchar *template, const gchar *text,
             GError **error)
{
    g_autofree gchar *path = NULL;
    gint fd;

    fd = g_file_open_tmp(template, &path, error);

    if (fd < 0)
        return NULL;

    /*
     * Written through the descriptor g_file_open_tmp() already opened,
     * rather than by path with g_file_set_contents(): that one writes a
     * second temporary and renames it, so the file claude reads would
     * be one this function never chose the mode of.
     */
    {
        gsize remaining = strlen(text);
        const gchar *at = text;

        while (remaining > 0)
        {
            gssize written = write(fd, at, remaining);

            if (written < 0)
            {
                if (errno == EINTR)
                    continue;

                g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                            "could not write the system prompt to %s: %s",
                            path, g_strerror(errno));
                close(fd);
                g_unlink(path);
                return NULL;
            }

            at += written;
            remaining -= (gsize)written;
        }
    }

    if (close(fd) != 0)
    {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "could not close %s: %s", path, g_strerror(errno));
        g_unlink(path);
        return NULL;
    }

    *slot = g_steal_pointer(&path);

    return *slot;
}

/*
 * Emit a system prompt as `<flag>-file <path>` rather than as its own
 * argv word.
 *
 * The kernel caps a *single* argument at MAX_ARG_STRLEN -- 32 pages,
 * 131,072 bytes -- which is not ARG_MAX and which no headroom in the
 * total will buy back.  A system prompt is assembled by concatenating
 * an agent's identity files, so it grows with what the product itself
 * writes, and past that figure execve refuses with E2BIG: the agent
 * answers "Failed to execute child process (Argument list too long)"
 * and can never start again.  The user prompt was moved to stdin for
 * this exact reason and the comment saying so is at the bottom of
 * build_argv(); the rule had been applied to one of the two.
 *
 * Unconditionally, not above a size threshold.  A branch that only runs
 * past 128KB is exercised by nobody until the day it breaks, which is
 * how the inline form survived this long -- one path means an
 * unsupported flag or an unwritable temporary directory surfaces on the
 * first turn instead of at scale.
 */
static gboolean
emit_prompt_file_flag(GPtrArray *args, const gchar *flag, gchar **slot,
                      const gchar *template, const gchar *text,
                      GError **error)
{
    const gchar *path;

    if (text == NULL || text[0] == '\0')
        return TRUE;

    path = spill_prompt(slot, template, text, error);

    if (path == NULL)
        return FALSE;

    g_ptr_array_add(args, g_strdup(flag));
    g_ptr_array_add(args, g_strdup(path));

    return TRUE;
}

/*
 * Emit a flag with a value, when the value is set.
 */
static void
emit_value_flag(GPtrArray *args, const gchar *flag, const gchar *value)
{
    if (value == NULL || value[0] == '\0')
        return;

    g_ptr_array_add(args, g_strdup(flag));
    g_ptr_array_add(args, g_strdup(value));
}

/*
 * Emit the session-independent knobs: which agent and tools the session
 * gets, where its settings and plugins come from, what it may spend, and
 * the diagnostics.
 *
 * Shared with the re-prompt path, so a follow-up runs under the same
 * settings, budget and plugin set as the turn it is summarising.
 */
static gboolean
emit_session_args(AiClaudeCodeClient *self, GPtrArray *args, GError **error)
{
    emit_value_flag(args, "--agent", self->agent);
    emit_value_flag(args, "--agents", self->agents_json);

    if (!emit_prompt_file_flag(args, "--append-system-prompt-file",
                               &self->append_system_prompt_path,
                               "ai-glib-append-system-prompt-XXXXXX",
                               self->append_system_prompt, error))
        return FALSE;

    emit_value_flag(args, "--fallback-model", self->fallback_model);
    emit_value_flag(args, "--json-schema", self->json_schema);
    emit_value_flag(args, "--settings", self->settings);
    emit_value_flag(args, "--setting-sources", self->setting_sources);
    emit_value_flag(args, "--autocompact", self->autocompact);

    emit_list_flag(args, "--betas", self->betas);
    emit_repeated_flag(args, "--plugin-dir", self->plugin_dirs);
    emit_repeated_flag(args, "--plugin-url", self->plugin_urls);

    if (self->max_budget_usd > 0.0)
    {
        /*
         * Locale-independent: %f would emit a comma for the decimal
         * separator under a locale that uses one, and claude parses this
         * as a number.
         */
        gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

        g_ascii_dtostr(buf, sizeof buf, self->max_budget_usd);
        g_ptr_array_add(args, g_strdup("--max-budget-usd"));
        g_ptr_array_add(args, g_strdup(buf));
    }

    if (self->strict_mcp_config)
        g_ptr_array_add(args, g_strdup("--strict-mcp-config"));

    if (self->disable_slash_commands)
        g_ptr_array_add(args, g_strdup("--disable-slash-commands"));

    if (self->exclude_dynamic_system_prompt_sections)
        g_ptr_array_add(args,
            g_strdup("--exclude-dynamic-system-prompt-sections"));

    if (self->bare)
        g_ptr_array_add(args, g_strdup("--bare"));

    if (self->safe_mode)
        g_ptr_array_add(args, g_strdup("--safe-mode"));

    /*
     * --debug takes an optional filter. A filter on its own is enough to
     * mean "debug with this filter"; the boolean covers the plain case.
     */
    if (self->debug_filter != NULL && self->debug_filter[0] != '\0')
    {
        g_ptr_array_add(args, g_strdup("--debug"));
        g_ptr_array_add(args, g_strdup(self->debug_filter));
    }
    else if (self->debug)
    {
        g_ptr_array_add(args, g_strdup("--debug"));
    }

    emit_value_flag(args, "--debug-file", self->debug_file);

    return TRUE;
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
    g_autoptr(GError) spill_error = NULL;

    (void)max_tokens;  /* Claude Code CLI doesn't have a max tokens flag */

    model = ai_cli_client_get_model(client);

    /*
     * The previous turn's spills go before this one's are written, so a
     * client that has run a thousand turns is holding two files rather
     * than two thousand.
     */
    clear_prompt_spills(self);

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

        /*
         * These three only mean anything with stream-json, and claude
         * rejects them elsewhere, so they are gated on the format rather
         * than left to the caller to remember.
         */
        if (self->include_partial_messages)
            g_ptr_array_add(args, g_strdup("--include-partial-messages"));

        if (self->include_hook_events)
            g_ptr_array_add(args, g_strdup("--include-hook-events"));

        if (self->forward_subagent_text)
            g_ptr_array_add(args, g_strdup("--forward-subagent-text"));
    }
    else
    {
        g_ptr_array_add(args, g_strdup("--output-format"));
        g_ptr_array_add(args, g_strdup("json"));
    }

    /* Agent, settings, plugins, budget and diagnostics. */
    if (!emit_session_args(self, args, &spill_error))
    {
        g_warning("claude-code: %s", spill_error->message);
        g_ptr_array_add(args, NULL);
        g_strfreev((gchar **)g_ptr_array_free(args, FALSE));
        return NULL;
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

        /*
         * Branch the resumed conversation instead of extending it. Only
         * valid alongside --resume, which is why it lives here.
         */
        if (self->fork_session)
            g_ptr_array_add(args, g_strdup("--fork-session"));
    }
    else if (persist && self->continue_session)
    {
        /*
         * No id to resume, but the caller asked to pick up where this
         * directory left off. As with --resume the conversation already
         * carries its system prompt, so it is not re-sent -- and claude
         * accepts --fork-session here too.
         */
        g_ptr_array_add(args, g_strdup("--continue"));

        if (self->fork_session)
            g_ptr_array_add(args, g_strdup("--fork-session"));
    }
    else
    {
        /*
         * New session — pass the system prompt to prime it.
         * Only sent on the first call; subsequent calls use --resume.
         */
        if (!emit_prompt_file_flag(args, "--system-prompt-file",
                                   &self->system_prompt_path,
                                   "ai-glib-system-prompt-XXXXXX",
                                   system_prompt, &spill_error))
        {
            g_warning("claude-code: %s", spill_error->message);
            g_ptr_array_add(args, NULL);
            g_strfreev((gchar **)g_ptr_array_free(args, FALSE));
            return NULL;
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

    prompt = g_string_new("");
    messages = ai_cli_client_messages_for_prompt(client, messages);
    for (l = messages; l != NULL; l = l->next)
    {
        AiMessage *msg = l->data;
        g_autofree gchar *projected = ai_cli_client_project_message(msg);

        if (projected != NULL && projected[0] != '\0')
        {
            if (prompt->len > 0)
            {
                g_string_append(prompt, "\n\n");
            }

            g_string_append(prompt, projected);
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

    /* NULL root: see the note in the streaming parser. */
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
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
 * Type-checked JSON accessors.
 *
 * json-glib's *_member_with_default() emit a critical when the member is
 * present but of another type, and subprocess stdout is untrusted input --
 * a CLI that changed a field from a number to a string could abort a
 * fatal-warnings run rather than being ignored. Same reasoning as the
 * grok_get_* helpers; keep new fields on these.
 */
static const gchar *
cc_get_string(JsonObject *obj, const gchar *member)
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
cc_get_object(JsonObject *obj, const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return NULL;

    node = json_object_get_member(obj, member);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

static JsonArray *
cc_get_array(JsonObject *obj, const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return NULL;

    node = json_object_get_member(obj, member);

    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

static gint64
cc_get_int(JsonObject *obj, const gchar *member, gint64 fallback)
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

static gdouble
cc_get_double(JsonObject *obj, const gchar *member, gdouble fallback)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return fallback;

    node = json_object_get_member(obj, member);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;

    if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
        return json_node_get_double(node);

    if (json_node_get_value_type(node) == G_TYPE_INT64)
        return (gdouble)json_node_get_int(node);

    return fallback;
}

static gboolean
cc_get_boolean(JsonObject *obj, const gchar *member, gboolean fallback)
{
    JsonNode *node;

    if (obj == NULL || !json_object_has_member(obj, member))
        return fallback;

    node = json_object_get_member(obj, member);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_BOOLEAN)
        return fallback;

    return json_node_get_boolean(node);
}

/*
 * Emit one Anthropic content block as events.
 *
 * The three kinds are kept apart deliberately. Text is the answer; thinking
 * is not, and used to be dropped here on exactly that reasoning -- it is now
 * reported under its own kind so a frontend can show what a caller
 * assembling the answer must still exclude. tool_use was dropped too, under
 * a comment saying the caller handled it, and no caller did.
 */
static void
cc_emit_content_block(
    JsonObject *block,
    GPtrArray  *out_events
){
    const gchar *block_type;

    if (block == NULL)
        return;

    block_type = cc_get_string(block, "type");

    if (g_strcmp0(block_type, "text") == 0)
    {
        const gchar *text = cc_get_string(block, "text");

        if (text != NULL && text[0] != '\0')
            g_ptr_array_add(out_events, ai_event_new_text_delta(text));
    }
    else if (g_strcmp0(block_type, "thinking") == 0)
    {
        const gchar *text = cc_get_string(block, "thinking");

        if (text != NULL && text[0] != '\0')
            g_ptr_array_add(out_events, ai_event_new_thinking_delta(text));
    }
    else if (g_strcmp0(block_type, "tool_use") == 0)
    {
        const gchar *id = cc_get_string(block, "id");
        const gchar *name = cc_get_string(block, "name");
        JsonNode *input;
        g_autoptr(AiToolUse) tool_use = NULL;

        if (name == NULL || name[0] == '\0')
            return;

        input = json_object_has_member(block, "input")
            ? json_object_get_member(block, "input")
            : NULL;

        tool_use = ai_tool_use_new(id != NULL ? id : "", name,
                                   input);

        g_ptr_array_add(out_events, ai_event_new_tool_started(tool_use));
    }
    else if (g_strcmp0(block_type, "tool_result") == 0)
    {
        /*
         * The CLI reports results on a "user" line, because that is how the
         * transcript models a tool answering the model. Its content is
         * either a plain string or an array of blocks.
         */
        const gchar *id = cc_get_string(block, "tool_use_id");
        gboolean is_error = cc_get_boolean(block, "is_error", FALSE);
        g_autoptr(GString) text = g_string_new(NULL);
        g_autoptr(AiToolResult) result = NULL;
        JsonNode *content;

        content = json_object_has_member(block, "content")
            ? json_object_get_member(block, "content")
            : NULL;

        if (content != NULL && JSON_NODE_HOLDS_VALUE(content) &&
            json_node_get_value_type(content) == G_TYPE_STRING)
        {
            g_string_append(text, json_node_get_string(content));
        }
        else if (content != NULL && JSON_NODE_HOLDS_ARRAY(content))
        {
            JsonArray *parts = json_node_get_array(content);
            guint n = json_array_get_length(parts);
            guint i;

            for (i = 0; i < n; i++)
            {
                JsonNode *pn = json_array_get_element(parts, i);
                const gchar *part_text;

                if (pn == NULL || !JSON_NODE_HOLDS_OBJECT(pn))
                    continue;

                part_text = cc_get_string(json_node_get_object(pn), "text");

                if (part_text != NULL)
                    g_string_append(text, part_text);
            }
        }

        result = ai_tool_result_new(id != NULL ? id : "", text->str, is_error);
        g_ptr_array_add(out_events, ai_event_new_tool_finished(NULL, result));
    }
}

/*
 * Walk the content array of an Anthropic message, emitting each block.
 */
static void
cc_emit_message_content(
    JsonObject *message,
    GPtrArray  *out_events
){
    JsonArray *blocks;
    guint n;
    guint i;

    blocks = cc_get_array(message, "content");

    if (blocks == NULL)
        return;

    n = json_array_get_length(blocks);

    for (i = 0; i < n; i++)
    {
        JsonNode *bn = json_array_get_element(blocks, i);

        if (bn != NULL && JSON_NODE_HOLDS_OBJECT(bn))
            cc_emit_content_block(json_node_get_object(bn), out_events);
    }
}

/*
 * The first text block of a message, borrowed.
 *
 * A limit message carries exactly one, but the shape is a content array
 * like any other, so this walks rather than assuming index zero.
 */
static const gchar *
cc_first_text(JsonObject *message)
{
    JsonArray *blocks;
    guint n;
    guint i;

    blocks = cc_get_array(message, "content");

    if (blocks == NULL)
        return NULL;

    n = json_array_get_length(blocks);

    for (i = 0; i < n; i++)
    {
        JsonNode *bn = json_array_get_element(blocks, i);
        JsonObject *block;

        if (bn == NULL || !JSON_NODE_HOLDS_OBJECT(bn))
            continue;

        block = json_node_get_object(bn);

        if (g_strcmp0(cc_get_string(block, "type"), "text") == 0)
            return cc_get_string(block, "text");
    }

    return NULL;
}

/*
 * Notices the account's session limit in an assistant message.
 *
 * The CLI answers a limit without reaching the API: it writes one
 * message naming AI_SESSION_LIMIT_SYNTHETIC_MODEL, reports every token
 * counter as zero, and exits non-zero.  The exit status alone says only
 * that something went wrong, which is why this was retried immediately
 * and indefinitely against a wall that would not move for hours.
 *
 * The reset time is in the text, so it is parsed here while the text is
 * in hand.  ai-glib owns both halves so that the agent and the
 * supervisor above it read the same answer rather than each writing
 * their own matcher against a sentence somebody may reword.
 */
static gboolean
cc_message_is_session_limit(JsonObject *msg_obj, gint64 now, gint64 *reset_out)
{
    JsonObject *usage_obj;
    const gchar *model;
    gint64 input_tokens = 0;
    gint64 output_tokens = 0;
    gint64 cache_creation = 0;
    gint64 cache_read = 0;

    if (msg_obj == NULL)
        return FALSE;

    model = cc_get_string(msg_obj, "model");
    usage_obj = cc_get_object(msg_obj, "usage");

    if (usage_obj != NULL)
    {
        input_tokens = cc_get_int(usage_obj, "input_tokens", 0);
        output_tokens = cc_get_int(usage_obj, "output_tokens", 0);
        cache_creation = cc_get_int(usage_obj, "cache_creation_input_tokens", 0);
        cache_read = cc_get_int(usage_obj, "cache_read_input_tokens", 0);
    }

    if (!ai_session_limit_looks_synthetic(model, input_tokens, output_tokens,
                                          cache_creation, cache_read))
        return FALSE;

    /*
     * A limit with no stated reset is still a limit.  The reset stays 0
     * and every layer above treats that as "unknown", which is a
     * different thing from "no limit" and must not be confused with it.
     */
    if (reset_out != NULL)
    {
        const gchar *text = cc_first_text(msg_obj);
        gint64 reset = 0;

        if (text != NULL &&
            ai_session_limit_parse_reset(text, now, &reset))
            *reset_out = reset;
        else
            *reset_out = 0;
    }

    return TRUE;
}

gboolean
ai_claude_code_line_is_session_limit(
    const gchar *line,
    gint64       now,
    gint64      *reset_out
){
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *root;
    JsonObject *obj;
    JsonObject *msg_obj;

    if (line == NULL || line[0] == '\0')
        return FALSE;

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, line, -1, NULL))
        return FALSE;

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
        return FALSE;

    obj = json_node_get_object(root);

    if (g_strcmp0(cc_get_string(obj, "type"), "assistant") != 0)
        return FALSE;

    msg_obj = cc_get_object(obj, "message");

    return cc_message_is_session_limit(msg_obj, now, reset_out);
}

static void
cc_note_session_limit(AiClaudeCodeClient *self, JsonObject *msg_obj)
{
    gint64 reset = 0;

    if (self == NULL || msg_obj == NULL)
        return;

    if (!cc_message_is_session_limit(
            msg_obj, g_get_real_time() / G_USEC_PER_SEC, &reset))
        return;

    self->session_limited = TRUE;
    self->session_limit_reset = reset;
}

/*
 * Parse a single NDJSON line from `claude --print --output-format stream-json`
 * into events.
 *
 * The lines that matter:
 *   {"type":"system","subtype":"init",...}  -> STATUS
 *   {"type":"assistant","message":{...}}    -> text / thinking / tool_use
 *   {"type":"user","message":{...}}         -> tool_result
 *   {"type":"stream_event","event":{...}}   -> token-level deltas, only with
 *                                              --include-partial-messages
 *   {"type":"result",...}                   -> session, usage, cost
 *
 * Reading msg_obj->text instead of walking message.content once found
 * nothing on every event, so a streamed reply arrived empty while the run
 * itself reported success. The content array is the shape the CLI actually
 * emits.
 *
 * When --include-partial-messages is on, both stream_event and the
 * whole-message assistant line describe the same text. Only the deltas are
 * taken as text; the whole message is still walked for its tool_use blocks,
 * which appear nowhere else, and TOOL_STARTED may therefore be emitted twice
 * for one id -- consumers key on the id and update.
 */
static gboolean
ai_claude_code_client_parse_stream_events(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    GPtrArray    *out_events,
    GError      **error
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(client);
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
    type = cc_get_string(obj, "type");

    if (g_strcmp0(type, "system") == 0)
    {
        const gchar *subtype = cc_get_string(obj, "subtype");
        const gchar *session_id = cc_get_string(obj, "session_id");

        /*
         * The init line is the first thing a run says, and carries the
         * session id well before the result line does. Capturing it here
         * means a run interrupted mid-turn can still be resumed.
         */
        if (session_id != NULL && session_id[0] != '\0' &&
            ai_cli_client_get_session_persistence(client))
        {
            ai_cli_client_set_session_id(client, session_id);
        }

        if (subtype != NULL && subtype[0] != '\0')
        {
            g_autofree gchar *text = g_strdup_printf("claude: %s", subtype);
            g_ptr_array_add(out_events, ai_event_new_status(text));
        }
    }
    else if (g_strcmp0(type, "assistant") == 0)
    {
        JsonObject *msg_obj = cc_get_object(obj, "message");
        const gchar *msg_type;

        if (msg_obj == NULL)
            return TRUE;

        cc_note_session_limit(self, msg_obj);

        msg_type = cc_get_string(msg_obj, "type");

        if (g_strcmp0(msg_type, "text") == 0)
        {
            /* Flat shape: {"message": {"type": "text", "text": ...}} */
            const gchar *text = cc_get_string(msg_obj, "text");

            if (text != NULL && text[0] != '\0')
                g_ptr_array_add(out_events, ai_event_new_text_delta(text));
        }
        else
        {
            cc_emit_message_content(msg_obj, out_events);
        }
    }
    else if (g_strcmp0(type, "user") == 0)
    {
        /* Tool results come back as a user message full of tool_result. */
        JsonObject *msg_obj = cc_get_object(obj, "message");

        if (msg_obj != NULL)
            cc_emit_message_content(msg_obj, out_events);
    }
    else if (g_strcmp0(type, "stream_event") == 0)
    {
        JsonObject *event = cc_get_object(obj, "event");
        const gchar *event_type;

        if (event == NULL)
            return TRUE;

        event_type = cc_get_string(event, "type");

        if (g_strcmp0(event_type, "content_block_start") == 0)
        {
            JsonObject *block = cc_get_object(event, "content_block");

            if (block != NULL &&
                g_strcmp0(cc_get_string(block, "type"), "tool_use") == 0)
                cc_emit_content_block(block, out_events);
        }
        else if (g_strcmp0(event_type, "content_block_delta") == 0)
        {
            JsonObject *delta = cc_get_object(event, "delta");
            const gchar *delta_type;

            if (delta == NULL)
                return TRUE;

            delta_type = cc_get_string(delta, "type");

            if (g_strcmp0(delta_type, "text_delta") == 0)
            {
                const gchar *text = cc_get_string(delta, "text");

                if (text != NULL && text[0] != '\0')
                    g_ptr_array_add(out_events, ai_event_new_text_delta(text));
            }
            else if (g_strcmp0(delta_type, "thinking_delta") == 0)
            {
                const gchar *text = cc_get_string(delta, "thinking");

                if (text != NULL && text[0] != '\0')
                    g_ptr_array_add(out_events,
                                    ai_event_new_thinking_delta(text));
            }
            else if (g_strcmp0(delta_type, "input_json_delta") == 0)
            {
                const gchar *fragment = cc_get_string(delta, "partial_json");

                if (fragment != NULL && fragment[0] != '\0')
                    g_ptr_array_add(
                        out_events,
                        ai_event_new_tool_input_delta(NULL, fragment));
            }
        }
    }
    else if (g_strcmp0(type, "result") == 0)
    {
        const gchar *result_text = cc_get_string(obj, "result");
        const gchar *session_id = cc_get_string(obj, "session_id");
        JsonObject *usage_obj;
        gdouble cost;

        /* Store session ID - ONLY if persistence is enabled */
        if (session_id != NULL && session_id[0] != '\0' &&
            ai_cli_client_get_session_persistence(client))
        {
            ai_cli_client_set_session_id(client, session_id);
        }

        /* Add final text content to response if not already added via deltas */
        if (result_text != NULL && result_text[0] != '\0' &&
            ai_response_get_content_blocks(response) == NULL)
        {
            g_autoptr(AiTextContent) content = ai_text_content_new(result_text);
            ai_response_add_content_block(response,
                (AiContentBlock *)g_steal_pointer(&content));
        }

        /* Update usage and check for context compaction */
        usage_obj = cc_get_object(obj, "usage");
        cost = cc_get_double(obj, "total_cost_usd", -1.0);

        if (usage_obj != NULL)
        {
            gint input_tokens = (gint)cc_get_int(usage_obj, "input_tokens", 0);
            gint output_tokens = (gint)cc_get_int(usage_obj, "output_tokens", 0);
            g_autoptr(AiUsage) usage = ai_usage_new(input_tokens, output_tokens);

            ai_response_set_usage(response, usage);
            check_and_emit_compaction(self, input_tokens);

            g_ptr_array_add(out_events,
                ai_event_new_usage(usage,
                    cost >= 0.0 ? (gint64)(cost * 1000000.0) : -1));
        }

        /*
         * The CLI's own total, set whether or not it reported a usage
         * object.  It is the only figure that accounts for cache reads,
         * which dominate a Claude Code turn: recomputing from the two
         * token counts above understates the bill several times over.
         *
         * Outside the usage branch on purpose -- a turn can report a
         * cost with no usage object, and that cost is still the truth.
         */
        if (cost >= 0.0)
            ai_response_set_cost_micros(response, (gint64)(cost * 1000000.0));

        /* Store total cost */
        if (cost >= 0.0)
        {
            self->total_cost = cost;
        }

        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }

    return TRUE;
}

/*
 * The pre-event contract, kept because callers and tests still use it.
 * A projection of parse_stream_events rather than a second implementation,
 * so the two can never disagree about what a line meant.
 */
static gboolean
ai_claude_code_client_parse_stream_line(
    AiCliClient  *client,
    const gchar  *line,
    AiResponse   *response,
    gchar       **delta_text,
    GError      **error
){
    g_autoptr(GPtrArray) events =
        g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);

    *delta_text = NULL;

    if (!ai_claude_code_client_parse_stream_events(client, line, response,
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
ai_claude_code_client_finalize(GObject *object)
{
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(object);

    g_free(self->last_tool_summary);
    g_free(self->mcp_config_path);
    g_free(self->permission_mode);
    g_free(self->allowed_tools);
    g_free(self->disallowed_tools);
    g_free(self->additional_directories);
    g_free(self->agent);
    g_free(self->agents_json);
    g_free(self->append_system_prompt);
    g_free(self->fallback_model);
    g_free(self->json_schema);
    g_free(self->settings);
    g_free(self->setting_sources);
    g_free(self->tools);
    g_free(self->betas);
    g_free(self->autocompact);
    g_free(self->plugin_dirs);
    g_free(self->plugin_urls);
    g_free(self->debug_filter);
    g_free(self->debug_file);

    /*
     * Last chance to take the two system prompts out of the temporary
     * directory.  Every ordinary path already removed them at the start
     * of the next turn; this is the client that ran one turn and was
     * dropped, which is every short-lived tool built on this library.
     */
    clear_prompt_spills(self);

    G_OBJECT_CLASS(ai_claude_code_client_parent_class)->finalize(object);
}

/*
 * claude-code takes extra MCP servers as a config file named by
 * --mcp-config, so delivery is the existing property.  The interface is
 * a second door onto the same state rather than a parallel path.
 */
static const gchar * const claude_code_endpoint_kinds[] = {
    AI_ENDPOINT_KIND_ENV,
    AI_ENDPOINT_KIND_MCP_CONFIG,
    NULL
};

static gboolean
ai_claude_code_client_endpoint_applied(
    AiCliClient            *client,
    const AiAgentEndpoint  *endpoint,
    GError                **error
){
    AiClaudeCodeClient *self = AI_CLAUDE_CODE_CLIENT(client);

    (void)error;

    if (endpoint != NULL
        && g_strcmp0(endpoint->kind, AI_ENDPOINT_KIND_MCP_CONFIG) == 0)
    {
        ai_claude_code_client_set_mcp_config_path(self, endpoint->value);
    }
    else
    {
        /* Revoked, or environment-only: drop the path so a later run
         * does not point at a file that has been deleted. */
        ai_claude_code_client_set_mcp_config_path(self, NULL);
    }

    return TRUE;
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
    cli_class->endpoint_applied = ai_claude_code_client_endpoint_applied;
    cli_class->endpoint_kinds = claude_code_endpoint_kinds;
    cli_class->build_argv = ai_claude_code_client_build_argv;
    cli_class->build_stdin = ai_claude_code_client_build_stdin;
    cli_class->parse_json_output = ai_claude_code_client_parse_json_output;
    cli_class->parse_stream_line = ai_claude_code_client_parse_stream_line;
    cli_class->parse_stream_events = ai_claude_code_client_parse_stream_events;

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

    /**
     * AiClaudeCodeClient:agent:
     *
     * The agent to run as, passed as `--agent`. Overrides the `agent`
     * setting for this session only.
     */
    properties[PROP_AGENT] =
        g_param_spec_string("agent", "Agent",
                            "Agent for the session (--agent)", NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:agents-json:
     *
     * A JSON object defining custom subagents, passed as `--agents`, e.g.
     * `{"reviewer": {"description": "Reviews code", "prompt": "..."}}`.
     */
    properties[PROP_AGENTS_JSON] =
        g_param_spec_string("agents-json", "Agents JSON",
                            "JSON object defining custom agents (--agents)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:append-system-prompt:
     *
     * Text appended to the default system prompt, passed as
     * `--append-system-prompt`.
     *
     * Unlike #AiCliClient:system-prompt this adds to the default prompt
     * rather than replacing it, and it is sent on every turn rather than
     * only when a session is created.
     */
    properties[PROP_APPEND_SYSTEM_PROMPT] =
        g_param_spec_string("append-system-prompt", "Append System Prompt",
                            "Text appended to the default system prompt",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:fallback-model:
     *
     * Model(s) to fall back to when the primary is overloaded or
     * unavailable, passed as `--fallback-model`. A comma-separated list is
     * tried in order. Only meaningful in print mode, which is the only
     * mode this client uses.
     */
    properties[PROP_FALLBACK_MODEL] =
        g_param_spec_string("fallback-model", "Fallback Model",
                            "Model(s) to fall back to (--fallback-model)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:json-schema:
     *
     * A JSON Schema the reply must validate against, passed as
     * `--json-schema`. The schema constrains the model's output; the
     * result still arrives through the usual response text.
     */
    properties[PROP_JSON_SCHEMA] =
        g_param_spec_string("json-schema", "JSON Schema",
                            "JSON Schema for structured output",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:settings:
     *
     * A settings JSON file path or JSON string, passed as `--settings`.
     */
    properties[PROP_SETTINGS] =
        g_param_spec_string("settings", "Settings",
                            "Settings file path or JSON string (--settings)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:setting-sources:
     *
     * Comma-separated setting sources to load (user, project, local),
     * passed as `--setting-sources`. Use this to stop a run picking up
     * settings from a repository it does not control.
     */
    properties[PROP_SETTING_SOURCES] =
        g_param_spec_string("setting-sources", "Setting Sources",
                            "Setting sources to load (--setting-sources)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:tools:
     *
     * Comma-separated built-in tool names, passed as `--tools`. This
     * selects which built-ins exist at all, where
     * #AiClaudeCodeClient:allowed-tools decides which of the existing ones
     * may run without approval.
     */
    properties[PROP_TOOLS] =
        g_param_spec_string("tools", "Tools",
                            "Comma-separated built-in tools (--tools)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:betas:
     *
     * Comma-separated beta headers for API requests, passed as `--betas`.
     * API-key users only.
     */
    properties[PROP_BETAS] =
        g_param_spec_string("betas", "Betas",
                            "Comma-separated beta headers (--betas)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:autocompact:
     *
     * Auto-compact window size, passed as `--autocompact`: "auto", or a
     * token count between 100k and 1M.
     *
     * Compaction is what #AiClaudeCodeClient::context-compacted reports
     * after the fact; this is the knob that governs when it happens.
     */
    properties[PROP_AUTOCOMPACT] =
        g_param_spec_string("autocompact", "Autocompact",
                            "Auto-compact window size (--autocompact)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:plugin-dirs:
     *
     * Comma-separated plugin directories or .zip paths for this session,
     * each emitted as its own `--plugin-dir`.
     */
    properties[PROP_PLUGIN_DIRS] =
        g_param_spec_string("plugin-dirs", "Plugin Dirs",
                            "Comma-separated plugin paths (--plugin-dir)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:plugin-urls:
     *
     * Comma-separated plugin .zip URLs for this session, each emitted as
     * its own `--plugin-url`. These are fetched and run, so point them
     * only at sources you trust.
     */
    properties[PROP_PLUGIN_URLS] =
        g_param_spec_string("plugin-urls", "Plugin URLs",
                            "Comma-separated plugin URLs (--plugin-url)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:debug-filter:
     *
     * A category filter for `--debug`, e.g. "api,hooks" or "!1p,!file".
     * Setting this enables debug mode on its own; see
     * #AiClaudeCodeClient:debug for the unfiltered case.
     */
    properties[PROP_DEBUG_FILTER] =
        g_param_spec_string("debug-filter", "Debug Filter",
                            "Category filter for --debug",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:debug-file:
     *
     * Path for claude's own debug log, passed as `--debug-file`. Setting
     * it implicitly enables debug mode. The log goes to this file rather
     * than to stderr, so it does not disturb the parsed output.
     */
    properties[PROP_DEBUG_FILE] =
        g_param_spec_string("debug-file", "Debug File",
                            "Path for claude's debug log (--debug-file)",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:max-budget-usd:
     *
     * A ceiling in USD on what this run may spend on API calls, passed as
     * `--max-budget-usd`. Zero, the default, omits the flag.
     *
     * The most direct bound available on an autonomous run: unlike a turn
     * limit it caps the thing that actually costs money.
     */
    properties[PROP_MAX_BUDGET_USD] =
        g_param_spec_double("max-budget-usd", "Max Budget USD",
                            "Spend ceiling in USD (--max-budget-usd)",
                            0.0, G_MAXDOUBLE, 0.0,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:strict-mcp-config:
     *
     * Whether to pass `--strict-mcp-config`, using only the servers from
     * #AiClaudeCodeClient:mcp-config-path and ignoring every other MCP
     * configuration.
     *
     * Without it --mcp-config is additive: the session keeps whatever
     * servers the workspace's own .mcp.json declares and gains these as
     * well.
     */
    properties[PROP_STRICT_MCP_CONFIG] =
        g_param_spec_boolean("strict-mcp-config", "Strict MCP Config",
                             "Use only the servers from --mcp-config",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:disable-slash-commands:
     *
     * Whether to pass `--disable-slash-commands`, disabling all skills.
     */
    properties[PROP_DISABLE_SLASH_COMMANDS] =
        g_param_spec_boolean("disable-slash-commands", "Disable Slash Commands",
                             "Whether to disable all skills",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:fork-session:
     *
     * Whether to pass `--fork-session`, branching a resumed conversation
     * into a new session id instead of extending the original. Only
     * emitted alongside `--resume`, which is the only place claude accepts
     * it.
     */
    properties[PROP_FORK_SESSION] =
        g_param_spec_boolean("fork-session", "Fork Session",
                             "Branch a resumed session (--fork-session)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:include-partial-messages:
     *
     * Whether to pass `--include-partial-messages` when streaming, making
     * claude emit partial chunks as they arrive rather than whole
     * messages. Ignored for non-streaming calls, where claude rejects it.
     */
    properties[PROP_INCLUDE_PARTIAL_MESSAGES] =
        g_param_spec_boolean("include-partial-messages",
                             "Include Partial Messages",
                             "Emit partial chunks while streaming",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:include-hook-events:
     *
     * Whether to pass `--include-hook-events` when streaming, adding hook
     * lifecycle events to the output stream. Ignored for non-streaming
     * calls.
     */
    properties[PROP_INCLUDE_HOOK_EVENTS] =
        g_param_spec_boolean("include-hook-events", "Include Hook Events",
                             "Include hook lifecycle events while streaming",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:forward-subagent-text:
     *
     * Whether to pass `--forward-subagent-text` when streaming, forwarding
     * subagent text and thinking as messages with parent_tool_use_id set.
     * Ignored for non-streaming calls.
     */
    properties[PROP_FORWARD_SUBAGENT_TEXT] =
        g_param_spec_boolean("forward-subagent-text", "Forward Subagent Text",
                             "Forward subagent output while streaming",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:exclude-dynamic-system-prompt-sections:
     *
     * Whether to pass `--exclude-dynamic-system-prompt-sections`, moving
     * per-machine sections (cwd, env, memory paths, git status) out of the
     * system prompt and into the first user message.
     *
     * Worth setting for a fleet of identical runs: it is what lets them
     * share a prompt cache rather than each carrying a unique prefix.
     * Only applies with the default system prompt.
     */
    properties[PROP_EXCLUDE_DYNAMIC_SYSTEM_PROMPT_SECTIONS] =
        g_param_spec_boolean("exclude-dynamic-system-prompt-sections",
                             "Exclude Dynamic System Prompt Sections",
                             "Move per-machine sections out of the system prompt",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:debug:
     *
     * Whether to pass `--debug`. #AiClaudeCodeClient:debug-filter narrows
     * it to particular categories and enables it on its own.
     */
    properties[PROP_DEBUG] =
        g_param_spec_boolean("debug", "Debug",
                             "Whether to enable claude's debug mode",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:bare:
     *
     * Whether to pass `--bare`: skip hooks, LSP, plugin sync, auto-memory
     * and CLAUDE.md discovery, and read Anthropic auth strictly from
     * ANTHROPIC_API_KEY.
     *
     * The most predictable way to run: what the session sees is what this
     * client passed, not whatever the host machine happens to have
     * configured.
     */
    properties[PROP_BARE] =
        g_param_spec_boolean("bare", "Bare",
                             "Skip hooks, plugins and auto-discovery (--bare)",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:safe-mode:
     *
     * Whether to pass `--safe-mode`, starting with all customizations
     * (CLAUDE.md, skills, plugins, hooks, MCP servers, commands, agents)
     * disabled. Auth, model selection, built-in tools and permissions
     * still work.
     */
    properties[PROP_SAFE_MODE] =
        g_param_spec_boolean("safe-mode", "Safe Mode",
                             "Start with all customizations disabled",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiClaudeCodeClient:continue-session:
     *
     * Whether to pass `--continue`, resuming the most recent conversation
     * in the working directory when no #AiCliClient:session-id is known.
     *
     * An explicit session id wins: the two name different conversations,
     * and silently preferring the wrong one is worse than either. Ignored
     * when #AiCliClient:session-persistence is off, which asks for a fresh
     * conversation every time.
     *
     * #AiClaudeCodeClient:fork-session applies here as well as to
     * `--resume`.
     */
    properties[PROP_CONTINUE_SESSION] =
        g_param_spec_boolean("continue-session", "Continue Session",
                             "Continue the most recent conversation (--continue)",
                             FALSE,
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
    g_debug("claude-code: re-prompt failed, using tool summary as fallback");

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
    /* Same agent, settings, plugins and budget as the turn being retried. */
    if (!emit_session_args(client, rargs, &err))
    {
        g_warning("claude-code: %s", err->message);
        return FALSE;
    }
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

    g_debug("claude-code: no text in response, re-prompting for summary "
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

        /*
         * A session limit first.  It is not a failure of this request --
         * nothing was asked and nothing was billed -- and retrying it
         * before the stated reset cannot succeed, so it must be
         * distinguishable from every other non-zero exit before anything
         * upstream decides whether to try again.
         *
         * Reported with the reset time in the message, from
         * ai_session_limit_format() rather than a sentence of this
         * file's own: three layers describe this condition and three
         * descriptions of one fact reads to an operator like three
         * problems.
         */
        if (data->client != NULL && data->client->session_limited)
        {
            g_autofree gchar *why =
                ai_session_limit_format(data->client->session_limit_reset);
            g_autofree gchar *notice =
                ai_session_limit_notice_new(data->client->session_limit_reset);

            /*
             * One string for two audiences: the sentence a person reads
             * and the notice a supervisor matches.  Composed here so
             * that whatever logs this error carries both without having
             * to know it should -- the alternative was re-deriving the
             * reset from the prose further up, which silently produced
             * "no reset" because the sentence reads "resets at 18:50"
             * and the time parser expects a clock after "resets".
             */
            g_task_return_new_error(data->task, AI_ERROR,
                                    AI_ERROR_SESSION_LIMIT, "%s %s",
                                    why, notice);
            chat_async_data_free(data);
            return;
        }

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
        g_debug("claude-code: re-prompt could not start, using fallback text");
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

    /*
     * A limit belongs to the run that hit it.
     *
     * Cleared here rather than left standing, because a client is
     * reused: without this the first limit would make every later run
     * report one, including the ones after the reset had passed, and
     * the agent would never be allowed to work again.
     */
    self->session_limited = FALSE;
    self->session_limit_reset = 0;

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
 *
 * The read loop lives in AiCliClient. All this client contributes is the
 * translation from its wire format into events -- the parse_stream_events
 * vfunc above. Spawning, line reading, signal emission, the deadline and
 * the response assembly are shared with every other CLI wrapper.
 */

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
    (void)tools;  /* Tools not yet supported via CLI */

    ai_cli_client_stream_run_async(AI_CLI_CLIENT(streamable), messages,
                                   system_prompt, max_tokens,
                                   cancellable, callback, user_data);
}

static AiResponse *
ai_claude_code_client_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
){
    return ai_cli_client_stream_run_finish(AI_CLI_CLIENT(streamable),
                                           result, error);
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
