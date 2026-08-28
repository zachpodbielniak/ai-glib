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
#include "core/ai-tool-endpoint.h"
#include "core/ai-tool-endpoint-consumer.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "model/ai-message.h"
#include "model/ai-response.h"

G_BEGIN_DECLS

/**
 * AI_CLI_ARG_LIMIT:
 *
 * How long a single `execve` argument may be, in bytes.
 *
 * `MAX_ARG_STRLEN`: 32 pages, and *not* `ARG_MAX`, which is the total and
 * is 2MB on an ordinary machine.  Room in the total buys nothing -- the
 * kernel refuses the whole call over one long word -- which is why the
 * failure reads as impossible until you know the per-argument limit
 * exists.
 *
 * The limit counts the terminating NUL, so the longest argument that
 * works is one byte short of this.  Measured rather than recalled:
 * 131071 bytes in one argument runs, 131072 is `E2BIG`.
 */
#define AI_CLI_ARG_LIMIT (131072)

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
 * @chat_sync: full override of the argv/spawn/parse pipeline
 * @parse_stream_events: parses a single NDJSON line into #AiEvent values
 * @spawn: launches the CLI, so a subclass can add environment or other
 *   launcher setup
 * @endpoint_applied: delivers an already-validated tool endpoint the way
 *   this particular CLI takes one
 * @endpoint_kinds: the #AiAgentEndpoint kinds this class accepts
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

    /*
     * The richer successor to parse_stream_line: appends zero or more
     * #AiEvent values to @out_events instead of returning one delta string.
     *
     * A subclass that leaves this NULL still works -- the default
     * implementation calls parse_stream_line and wraps its delta in an
     * %AI_EVENT_TEXT_DELTA -- but it will only ever report text, which is
     * how every CLI wrapper behaved before this existed.
     */
    gboolean     (*parse_stream_events) (AiCliClient    *self,
                                         const gchar    *line,
                                         AiResponse     *response,
                                         GPtrArray      *out_events,
                                         GError        **error);

    /*
     * Launches the CLI. The default honours "working-directory"; override
     * only to add something else, as opencode does for OPENCODE_PERMISSION.
     */
    GSubprocess * (*spawn)              (AiCliClient          *self,
                                         const gchar * const  *argv,
                                         GSubprocessFlags      flags,
                                         GError              **error);

    /*
     * Per-CLI delivery of a tool endpoint the base has already validated
     * against @endpoint_kinds and stored.  @endpoint is %NULL when the
     * endpoint is being revoked, and an implementation MUST undo there
     * exactly what it did on apply -- a temporary directory removed, an
     * environment variable unset -- or a revoked credential outlives the
     * run that owned it.
     *
     * Leave NULL for a CLI whose only delivery is the environment: the
     * base applies AiAgentEndpoint.env at spawn for every subclass.
     */
    gboolean      (*endpoint_applied)   (AiCliClient           *self,
                                         const AiAgentEndpoint *endpoint,
                                         GError               **error);

    /*
     * Static, NULL-terminated, and must list AI_ENDPOINT_KIND_ENV.  A
     * class datum rather than a vfunc because the answer is a
     * compile-time constant per class; GtkWidgetClass:css_name is the
     * same shape.  NULL means environment delivery only.
     */
    const gchar * const *endpoint_kinds;

    /* Reserved for future expansion */
    gpointer _reserved[2];
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

void
ai_cli_client_set_env(
    AiCliClient *self,
    const gchar *key,
    const gchar *value
);

void
ai_cli_client_unset_env(
    AiCliClient *self,
    const gchar *key
);

const gchar *
ai_cli_client_get_env(
    AiCliClient *self,
    const gchar *key
);

GHashTable *
ai_cli_client_get_environment(AiCliClient *self);

void
ai_cli_client_set_environment(
    AiCliClient *self,
    GHashTable  *env
);

GSubprocessLauncher *
ai_cli_client_create_launcher(
    AiCliClient      *self,
    GSubprocessFlags  flags
);

const AiAgentEndpoint *
ai_cli_client_get_tool_endpoint(AiCliClient *self);

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

gchar *
ai_cli_client_events_to_delta(GPtrArray *events);

GSubprocess *
ai_cli_client_spawn(
    AiCliClient          *self,
    const gchar * const  *argv,
    GSubprocessFlags      flags,
    GError              **error
);

void
ai_cli_client_stream_run_async(
    AiCliClient         *self,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

AiResponse *
ai_cli_client_stream_run_finish(
    AiCliClient   *self,
    GAsyncResult  *result,
    GError       **error
);

G_END_DECLS
