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
 * @build_stdin: builds the stdin data to pipe to the CLI subprocess,
 *   or returns %NULL if the prompt is passed via argv instead
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
    gchar *      (*build_stdin)         (AiCliClient    *self,
                                         GList          *messages);

    /*
     * Subclasses with a fundamentally different chat flow (e.g. the
     * tmux client, which doesn't directly spawn the CLI) can override
     * chat_sync to take full control.  When non-NULL, this is called
     * by ai_cli_client_chat_sync() instead of the default
     * argv/spawn/parse pipeline.
     */
    AiResponse * (*chat_sync)           (AiCliClient    *self,
                                         GList          *messages,
                                         GCancellable   *cancellable,
                                         GError        **error);

    /* Reserved for future expansion */
    gpointer _reserved[6];
};

AiConfig *
ai_cli_client_get_config(AiCliClient *self);

const gchar *
ai_cli_client_get_model(AiCliClient *self);

void
ai_cli_client_set_model(
    AiCliClient *self,
    const gchar *model
);

gint
ai_cli_client_get_max_tokens(AiCliClient *self);

void
ai_cli_client_set_max_tokens(
    AiCliClient *self,
    gint         max_tokens
);

const gchar *
ai_cli_client_get_system_prompt(AiCliClient *self);

void
ai_cli_client_set_system_prompt(
    AiCliClient *self,
    const gchar *system_prompt
);

const gchar *
ai_cli_client_get_executable_path(AiCliClient *self);

void
ai_cli_client_set_executable_path(
    AiCliClient *self,
    const gchar *path
);

const gchar *
ai_cli_client_get_session_id(AiCliClient *self);

void
ai_cli_client_set_session_id(
    AiCliClient *self,
    const gchar *session_id
);

gboolean
ai_cli_client_get_session_persistence(AiCliClient *self);

void
ai_cli_client_set_session_persistence(
    AiCliClient *self,
    gboolean     persist
);

const gchar *
ai_cli_client_get_working_directory(AiCliClient *self);

void
ai_cli_client_set_working_directory(
    AiCliClient *self,
    const gchar *directory
);

gint
ai_cli_client_get_process_timeout_ms(AiCliClient *self);

void
ai_cli_client_set_process_timeout_ms(
    AiCliClient *self,
    gint         timeout_ms
);

const gchar *
ai_cli_client_get_effort_level(AiCliClient *self);

void
ai_cli_client_set_effort_level(
    AiCliClient *self,
    const gchar *effort_level
);

AiResponse *
ai_cli_client_chat_sync(
    AiCliClient   *self,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
);

gchar *
ai_cli_client_resolve_executable(
    AiCliClient  *self,
    GError      **error
);

gchar *
ai_cli_client_format_exit_error(
    gint         exit_status,
    const gchar *stderr_data,
    const gchar *stdout_data
);

G_END_DECLS
