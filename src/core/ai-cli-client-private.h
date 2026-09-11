/*
 * ai-cli-client-private.h - Helpers shared by CLI provider implementations
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "This is a private ai-glib header."
#endif

#include "core/ai-cli-client.h"

G_BEGIN_DECLS

gboolean ai_cli_messages_have_images(GList *messages);
GList *ai_cli_client_messages_for_images(AiCliClient *self, GList *messages);
gchar *ai_cli_image_user_event(GList *messages, const gchar *text);

gboolean
ai_cli_client_mcp_config_has_host(const gchar *path, gboolean toml);

gchar *
ai_cli_client_project_message(AiMessage *message);

gchar *
ai_cli_client_project_multimodal_message(AiMessage *message);

void
ai_cli_client_mark_portable_context(AiCliClient *self);

GList *
ai_cli_client_messages_for_prompt(
    AiCliClient *self,
    GList       *messages
);

G_END_DECLS
