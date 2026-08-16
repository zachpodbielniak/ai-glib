/*
 * ai-tool-executor.h - Built-in tool executor for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiToolExecutor provides built-in tool implementations (bash, read, write,
 * edit, glob, grep, ls, web_fetch, web_search) and manages the multi-turn
 * tool-use conversation loop with any AiProvider.
 *
 * Quick start:
 *   g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
 *
 *   // Optionally enable web_search:
 *   g_autoptr(AiBingSearch) bing = ai_bing_search_new("key");
 *   ai_tool_executor_set_search_provider(exec, AI_SEARCH_PROVIDER(bing));
 *
 *   // Run a prompt with full tool support:
 *   g_autoptr(AiMessage) msg = ai_message_new_user("List files in /tmp");
 *   GList *msgs = g_list_append(NULL, msg);
 *   g_autofree gchar *reply = ai_tool_executor_run(
 *       exec, provider, msgs, NULL, 4096, NULL, NULL);
 *   g_list_free(msgs);
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "core/ai-provider.h"
#include "model/ai-tool-use.h"
#include "convenience/ai-search-provider.h"
#include "harness/ai-resource-registry.h"
#include "model/ai-todo.h"

G_BEGIN_DECLS

/**
 * AiToolApproval:
 * @AI_TOOL_APPROVAL_DEFAULT: no opinion; defer to #AiToolExecutor:approval-policy
 * @AI_TOOL_APPROVAL_ALLOW: run this call
 * @AI_TOOL_APPROVAL_DENY: refuse this call, but let the run continue
 * @AI_TOOL_APPROVAL_ALLOW_ALWAYS: run it, and stop asking about this tool
 *   for the rest of the run
 * @AI_TOOL_APPROVAL_DENY_ALL: refuse and abort the whole run
 *
 * What a #AiToolExecutor::approval-requested handler decided.
 *
 * %AI_TOOL_APPROVAL_DEFAULT is zero on purpose. A signal with no handlers
 * accumulates to zero, which resolves through the policy to
 * %AI_TOOL_APPROVAL_ALLOW --- so an executor nobody is watching behaves
 * exactly as it did before approval existed, by construction rather than by
 * a compatibility branch.
 *
 * %AI_TOOL_APPROVAL_DENY continues the run because a refused tool is
 * information the model can act on: it will usually apologise and try
 * something else, which is more useful than an aborted turn. Only
 * %AI_TOOL_APPROVAL_DENY_ALL stops everything.
 */
typedef enum
{
    AI_TOOL_APPROVAL_DEFAULT = 0,
    AI_TOOL_APPROVAL_ALLOW,
    AI_TOOL_APPROVAL_DENY,
    AI_TOOL_APPROVAL_ALLOW_ALWAYS,
    AI_TOOL_APPROVAL_DENY_ALL
} AiToolApproval;

/**
 * AiToolFeatures:
 * @AI_TOOL_FEATURE_NONE: neither
 * @AI_TOOL_FEATURE_SUBAGENTS: the `task` and `skill` tools
 * @AI_TOOL_FEATURE_BACKGROUND: the `agent_spawn`, `agent_status`,
 *   `agent_result`, `agent_wait` and `agent_cancel` tools
 * @AI_TOOL_FEATURE_ALL: both
 *
 * Which optional groups of tools an executor is willing to offer.
 *
 * Both bits are set by default, but a bit only decides whether a group
 * *may* appear --- each group still needs the thing it runs on, a
 * resource registry for subagents and an #AiBrigade for background
 * agents, and neither is created implicitly. So an application that sets
 * up neither is unaffected by any of this, one that wants background
 * agents asks for them by handing over a brigade, and one that wants a
 * brigade for its own purposes while denying the model access to it
 * clears the bit.
 *
 * This is a group switch, not the grant. The grant is the tool list:
 * ai_tool_executor_unregister() takes a single tool away for good, and
 * that is how an agent file's `tools:` allowlist is enforced.
 */
typedef enum
{
    AI_TOOL_FEATURE_NONE       = 0,
    AI_TOOL_FEATURE_SUBAGENTS  = 1 << 0,
    AI_TOOL_FEATURE_BACKGROUND = 1 << 1,
    AI_TOOL_FEATURE_ALL        = (1 << 0) | (1 << 1)
} AiToolFeatures;

#define AI_TYPE_TOOL_EXECUTOR (ai_tool_executor_get_type())

G_DECLARE_FINAL_TYPE(AiToolExecutor, ai_tool_executor, AI, TOOL_EXECUTOR, GObject)

/* Forward decls. AiBrigade cannot be included here: ai-brigade.h reaches
 * back to this header through ai-agent.h. */
typedef struct _AiTool AiTool;
typedef struct _AiBrigade AiBrigade;

/**
 * AiToolCallback:
 * @tool_use: the #AiToolUse request from the model
 * @cancellable: (nullable): an optional #GCancellable
 * @error: (out) (optional): return location for a #GError
 * @user_data: the user data registered alongside this callback
 *
 * Function signature for user-supplied tool implementations registered with
 * ai_tool_executor_register_callback().
 *
 * Returns: (transfer full) (nullable): the result string, or %NULL on error.
 *   Free with g_free().
 */
typedef gchar * (*AiToolCallback) (
    AiToolUse    *tool_use,
    GCancellable *cancellable,
    GError      **error,
    gpointer      user_data
);

/**
 * ai_tool_executor_new:
 *
 * Creates a new #AiToolExecutor with all built-in tools pre-registered.
 *
 * Built-in tools: bash, read, write, edit, glob, grep, ls, web_fetch.
 * The web_search tool is only registered after calling
 * ai_tool_executor_set_search_provider().
 *
 * Returns: (transfer full): a new #AiToolExecutor
 */
AiToolExecutor *
ai_tool_executor_new (void);

/**
 * ai_tool_executor_new_empty:
 *
 * Creates an #AiToolExecutor with NO tools at all -- not even the
 * built-ins.
 *
 * Use this whenever the model should be able to do exactly what the host
 * application registers and nothing else. ai_tool_executor_new() hands the
 * model `bash`, `read`, `write` and `edit`, which for an agent that lives
 * inside somebody's accounts, records or infrastructure is a far larger
 * grant than the host usually intends.
 *
 * The tool list is the grant, not a suggestion: a built-in runs only
 * while this executor advertises it, so ai_tool_executor_unregister()
 * genuinely takes one away rather than merely hiding it from the model.
 * That is what lets `task` confine a subagent to the tools its agent file
 * declares.
 *
 * The registration API is identical; only the starting set differs.
 *
 * Returns: (transfer full): a new #AiToolExecutor with no tools
 */
AiToolExecutor *
ai_tool_executor_new_empty (void);

/**
 * AI_TOOL_EXECUTOR_MAX_TASK_DEPTH:
 *
 * How many nested `task` calls are allowed.
 *
 * A subagent delegating to a subagent delegating to a subagent is a
 * runaway, not a plan. The limit is reported to the model rather than
 * silently enforced, so it does the work itself instead of wondering why
 * nothing happened.
 */
#define AI_TOOL_EXECUTOR_MAX_TASK_DEPTH (2)

/**
 * AI_TOOL_EXECUTOR_AGENT_MAX_TOKENS:
 *
 * The token budget one `task` subagent turn is given.
 */
#define AI_TOOL_EXECUTOR_AGENT_MAX_TOKENS (8192)

void
ai_tool_executor_set_resource_registry (
    AiToolExecutor     *self,
    AiResourceRegistry *registry
);

AiResourceRegistry *
ai_tool_executor_get_resource_registry (AiToolExecutor *self);

void
ai_tool_executor_set_features (
    AiToolExecutor *self,
    AiToolFeatures  features
);

AiToolFeatures
ai_tool_executor_get_features (AiToolExecutor *self);

void
ai_tool_executor_set_brigade (
    AiToolExecutor *self,
    AiBrigade      *brigade
);

AiBrigade *
ai_tool_executor_get_brigade (AiToolExecutor *self);

/**
 * AI_TOOL_EXECUTOR_AGENT_WAIT_MAX_SECONDS:
 *
 * The longest `agent_wait` will block for, whatever it is asked for.
 *
 * A tool that waits forever is a hung program with extra steps: the main
 * loop keeps turning, but the conversation the model is holding never
 * gets another turn and the person in front of it has no way to know why.
 */
#define AI_TOOL_EXECUTOR_AGENT_WAIT_MAX_SECONDS (600)

guint
ai_tool_executor_get_n_todos (AiToolExecutor *self);

const AiTodo *
ai_tool_executor_get_todo (
    AiToolExecutor *self,
    guint           index
);

gboolean
ai_tool_executor_get_todo_fields (
    AiToolExecutor  *self,
    guint            index,
    const gchar    **out_label,
    AiTodoState     *out_state
);

GPtrArray *
ai_tool_executor_get_todos (AiToolExecutor *self);

void
ai_tool_executor_clear_todos (AiToolExecutor *self);

/**
 * ai_tool_executor_set_search_provider:
 * @self: an #AiToolExecutor
 * @provider: an #AiSearchProvider implementation
 *
 * Sets the search provider and registers the web_search tool.
 * The tool is only available to the AI after this call.
 *
 * Supported providers: #AiBingSearch, #AiBraveSearch.
 * To add a new provider, implement #AiSearchProviderInterface.
 */
void
ai_tool_executor_set_search_provider (
    AiToolExecutor   *self,
    AiSearchProvider *provider
);

/**
 * ai_tool_executor_get_tools:
 * @self: an #AiToolExecutor
 *
 * Returns the list of #AiTool definitions managed by this executor.
 * Pass this list to ai_provider_chat_async() or use ai_tool_executor_run()
 * which handles the full loop automatically.
 *
 * Returns: (transfer none) (element-type AiTool): the tool list.
 *   Do not free; the executor owns it.
 */
GList *
ai_tool_executor_get_tools (AiToolExecutor *self);

/**
 * ai_tool_executor_execute:
 * @self: an #AiToolExecutor
 * @tool_use: the tool use request from the model
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Executes a single tool use request and returns the result as a string.
 * On fatal error (unknown tool, missing required param), sets @error and
 * returns %NULL. Non-fatal errors (nonzero exit codes, file not found)
 * are returned as content strings prefixed with an error indicator.
 *
 * Returns: (transfer full) (nullable): the result string, or %NULL on error.
 *   Free with g_free().
 */
gchar *
ai_tool_executor_execute (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
);

/**
 * ai_tool_executor_register_callback:
 * @self: an #AiToolExecutor
 * @tool: (transfer none): the #AiTool describing the tool (name, description,
 *    parameters)
 * @callback: (scope notified) (closure user_data) (destroy user_data_free):
 *    function to invoke when the model calls this tool
 * @user_data: opaque pointer passed to @callback on each call
 * @user_data_free: (nullable): destroy notify invoked when @user_data is
 *    no longer needed (e.g. on unregister or executor finalize)
 *
 * Registers a user-supplied tool callback. The executor takes a ref on @tool
 * and merges it into its tool list so that ai_tool_executor_get_tools()
 * (and the multi-turn loop) will advertise it to the model.
 *
 * If a tool with the same name is already registered (built-in or
 * user-supplied), the new registration replaces it.
 *
 * This lets you wire arbitrary application logic into the same multi-turn
 * loop that drives the built-in tools.
 */
void
ai_tool_executor_register_callback (
    AiToolExecutor  *self,
    AiTool          *tool,
    AiToolCallback   callback,
    gpointer         user_data,
    GDestroyNotify   user_data_free
);

/**
 * ai_tool_executor_unregister:
 * @self: an #AiToolExecutor
 * @tool_name: the name of the tool to remove
 *
 * Removes a previously-registered user tool. No-op if the name is not
 * registered. Does not remove built-in tools (the same name would simply
 * become callable again via the built-in implementation).
 */
void
ai_tool_executor_unregister (
    AiToolExecutor *self,
    const gchar    *tool_name
);

/**
 * ai_tool_executor_run:
 * @self: an #AiToolExecutor
 * @provider: the #AiProvider to send requests to
 * @messages: (element-type AiMessage): initial conversation messages
 * @system_prompt: (nullable): optional system prompt
 * @max_tokens: maximum tokens for each response (0 for default 4096)
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Runs the full tool-use conversation loop synchronously.
 *
 * Sends @messages to @provider with all registered tools, executes any
 * tool calls returned by the model, and continues the conversation until
 * the model produces a final text response. Capped at 20 turns.
 *
 * Returns: (transfer full) (nullable): the final response text, or %NULL on
 *   error. Free with g_free().
 */
/* Directory the built-in tools resolve relative paths against and run
 * commands in.  NULL (the default) means the host process's own working
 * directory, which for an editor is wherever the user last was. */
void
ai_tool_executor_set_working_directory (
    AiToolExecutor  *self,
    const gchar     *path
);

const gchar *
ai_tool_executor_get_working_directory (
    AiToolExecutor  *self
);

gchar *
ai_tool_executor_run (
    AiToolExecutor  *self,
    AiProvider      *provider,
    GList           *messages,
    const gchar     *system_prompt,
    gint             max_tokens,
    GCancellable    *cancellable,
    GError         **error
);

gchar *
ai_tool_executor_run_full (
    AiToolExecutor  *self,
    AiProvider      *provider,
    GList           *messages,
    const gchar     *system_prompt,
    gint             max_tokens,
    GCancellable    *cancellable,
    GList          **out_new_messages,
    GError         **error
);

void
ai_tool_executor_run_async (
    AiToolExecutor      *self,
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    gint                 max_turns,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

gchar *
ai_tool_executor_run_finish (
    AiToolExecutor  *self,
    GAsyncResult    *result,
    GError         **error
);

AiToolApproval
ai_tool_executor_get_approval_policy (AiToolExecutor *self);

void
ai_tool_executor_set_approval_policy (
    AiToolExecutor *self,
    AiToolApproval  policy
);

gboolean
ai_tool_executor_get_stream (AiToolExecutor *self);

void
ai_tool_executor_set_stream (
    AiToolExecutor *self,
    gboolean        stream
);

G_END_DECLS
