/*
 * ai-cli-client.h - Base client class for CLI-based providers
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "core/ai-config.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "model/ai-message.h"
#include "model/ai-response.h"

G_BEGIN_DECLS

#define AI_TYPE_CLI_CLIENT (ai_cli_client_get_type())

G_DECLARE_DERIVABLE_TYPE(AiCliClient, ai_cli_client, AI, CLI_CLIENT, GObject)

/**
 * AiCliClientClass:
 * @parent_class: the parent class
 * @get_executable_path: gets the path to the CLI executable
 * @build_argv: builds the command line arguments for the CLI
 * @parse_json_output: parses the JSON output from the CLI
 * @parse_stream_line: parses a single NDJSON line from streaming output
 * @_reserved: reserved for future expansion
 *
 * Class structure for #AiCliClient.
 * Subclasses should override the virtual methods to implement CLI-specific
 * argument building and output parsing.
 */
struct _AiCliClientClass
{
    GObjectClass parent_class;

    /* Virtual methods for subclasses */
    gchar *      (*get_executable_path) (AiCliClient    *self);
    gchar **     (*build_argv)          (AiCliClient    *self,
                                         GList          *messages,
                                         const gchar    *system_prompt,
                                         gint            max_tokens,
                                         gboolean        streaming);
    AiResponse * (*parse_json_output)   (AiCliClient    *self,
                                         const gchar    *json,
                                         GError        **error);
    gboolean     (*parse_stream_line)   (AiCliClient    *self,
                                         const gchar    *line,
                                         AiResponse     *response,
                                         gchar         **delta_text,
                                         GError        **error);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

/**
 * ai_cli_client_get_config:
 * @self: an #AiCliClient
 *
 * Gets the configuration for this client.
 *
 * Returns: (transfer none): the #AiConfig
 */
AiConfig *
ai_cli_client_get_config(AiCliClient *self);

/**
 * ai_cli_client_get_model:
 * @self: an #AiCliClient
 *
 * Gets the model name.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_cli_client_get_model(AiCliClient *self);

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
);

/**
 * ai_cli_client_get_max_tokens:
 * @self: an #AiCliClient
 *
 * Gets the default max tokens setting.
 *
 * Returns: the max tokens
 */
gint
ai_cli_client_get_max_tokens(AiCliClient *self);

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
);

/**
 * ai_cli_client_get_system_prompt:
 * @self: an #AiCliClient
 *
 * Gets the default system prompt.
 *
 * Returns: (transfer none) (nullable): the system prompt
 */
const gchar *
ai_cli_client_get_system_prompt(AiCliClient *self);

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
);

/**
 * ai_cli_client_get_executable_path:
 * @self: an #AiCliClient
 *
 * Gets the path to the CLI executable.
 *
 * Returns: (transfer none) (nullable): the executable path, or %NULL to search PATH
 */
const gchar *
ai_cli_client_get_executable_path(AiCliClient *self);

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
);

/**
 * ai_cli_client_get_session_id:
 * @self: an #AiCliClient
 *
 * Gets the current session ID for conversation continuity.
 *
 * Returns: (transfer none) (nullable): the session ID
 */
const gchar *
ai_cli_client_get_session_id(AiCliClient *self);

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
);

/**
 * ai_cli_client_get_session_persistence:
 * @self: an #AiCliClient
 *
 * Gets whether session persistence is enabled.
 *
 * Returns: %TRUE if session persistence is enabled
 */
gboolean
ai_cli_client_get_session_persistence(AiCliClient *self);

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
);

/**
 * ai_cli_client_chat_sync:
 * @self: an #AiCliClient
 * @messages: (element-type AiMessage): the conversation messages
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Performs a synchronous chat completion request via the CLI.
 *
 * Returns: (transfer full) (nullable): the #AiResponse, or %NULL on error
 */
AiResponse *
ai_cli_client_chat_sync(
    AiCliClient   *self,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
);

/**
 * ai_cli_client_resolve_executable:
 * @self: an #AiCliClient
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves the CLI executable path. Checks environment variable overrides,
 * then falls back to searching PATH.
 *
 * Returns: (transfer full) (nullable): the resolved path, or %NULL on error
 */
gchar *
ai_cli_client_resolve_executable(
    AiCliClient  *self,
    GError      **error
);

G_END_DECLS
