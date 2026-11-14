/*
 * ai-acp-client.h - Any Agent Client Protocol (ACP) agent as a CLI client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include "core/ai-cli-client.h"

G_BEGIN_DECLS

#define AI_TYPE_ACP_CLIENT (ai_acp_client_get_type())

G_DECLARE_FINAL_TYPE(AiAcpClient, ai_acp_client, AI, ACP_CLIENT, AiCliClient)

/**
 * AI_ACP_PROTOCOL_VERSION:
 *
 * The Agent Client Protocol major version this client speaks, sent as
 * `protocolVersion` in `initialize`.  An agent that answers with another
 * version is refused before a session is opened.
 */
#define AI_ACP_PROTOCOL_VERSION (1)

/**
 * AI_ACP_MODEL_DEFAULT:
 *
 * The only model id the client reports: whatever the agent runs by
 * default.  ACP v1 has no stable request that selects a model, so the
 * agent's own configuration (or its command line) chooses it.
 */
#define AI_ACP_MODEL_DEFAULT "default"

AiAcpClient *
ai_acp_client_new(void);

AiAcpClient *
ai_acp_client_new_with_config(AiConfig *config);

AiAcpClient *
ai_acp_client_new_for_command(const gchar * const *argv);

const gchar *
ai_acp_client_get_command(AiAcpClient *self);

void
ai_acp_client_set_command(
    AiAcpClient *self,
    const gchar *command
);

gchar **
ai_acp_client_dup_argv(
    AiAcpClient  *self,
    GError      **error
);

void
ai_acp_client_set_argv(
    AiAcpClient         *self,
    const gchar * const *argv
);

gboolean
ai_acp_client_get_skip_permissions(AiAcpClient *self);

void
ai_acp_client_set_skip_permissions(
    AiAcpClient *self,
    gboolean     skip
);

gboolean
ai_acp_client_get_client_fs(AiAcpClient *self);

void
ai_acp_client_set_client_fs(
    AiAcpClient *self,
    gboolean     client_fs
);

guint
ai_acp_client_get_shutdown_timeout_ms(AiAcpClient *self);

void
ai_acp_client_set_shutdown_timeout_ms(
    AiAcpClient *self,
    guint        timeout_ms
);

G_END_DECLS
