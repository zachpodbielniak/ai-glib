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

G_BEGIN_DECLS

#define AI_TYPE_TOOL_EXECUTOR (ai_tool_executor_get_type())

G_DECLARE_FINAL_TYPE(AiToolExecutor, ai_tool_executor, AI, TOOL_EXECUTOR, GObject)

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

G_END_DECLS
