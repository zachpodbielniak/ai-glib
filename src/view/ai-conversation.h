/*
 * ai-conversation.h - Drives a provider and folds what happens into blocks
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

#include <glib-object.h>
#include <gio/gio.h>

#include "view/ai-transcript.h"
#include "convenience/ai-tool-executor.h"
#include "harness/ai-command.h"

G_BEGIN_DECLS

#define AI_TYPE_CONVERSATION (ai_conversation_get_type())

G_DECLARE_FINAL_TYPE(AiConversation, ai_conversation, AI, CONVERSATION, GObject)

AiConversation *
ai_conversation_new(GObject *provider);

AiTranscript *
ai_conversation_get_transcript(AiConversation *self);

AiToolExecutor *
ai_conversation_get_executor(AiConversation *self);

GObject *
ai_conversation_get_provider(AiConversation *self);

GList *
ai_conversation_get_messages(AiConversation *self);

const gchar *
ai_conversation_get_system_prompt(AiConversation *self);

void
ai_conversation_set_system_prompt(
    AiConversation *self,
    const gchar    *prompt
);

gint
ai_conversation_get_max_tokens(AiConversation *self);

void
ai_conversation_set_max_tokens(
    AiConversation *self,
    gint            max_tokens
);

gboolean
ai_conversation_get_stream(AiConversation *self);

void
ai_conversation_set_stream(
    AiConversation *self,
    gboolean        stream
);

gboolean
ai_conversation_get_local_tools(AiConversation *self);

void
ai_conversation_set_local_tools(
    AiConversation *self,
    gboolean        local_tools
);

gboolean
ai_conversation_get_busy(AiConversation *self);

const gchar *
ai_conversation_get_activity(AiConversation *self);

gint64
ai_conversation_get_activity_elapsed(AiConversation *self);

void
ai_conversation_send_async(
    AiConversation      *self,
    const gchar         *text,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

gboolean
ai_conversation_send_finish(
    AiConversation  *self,
    GAsyncResult    *result,
    GError         **error
);

void
ai_conversation_cancel(AiConversation *self);

void
ai_conversation_clear(AiConversation *self);

void
ai_conversation_send_full_async(
    AiConversation      *self,
    const gchar         *display_text,
    const gchar         *text,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

gboolean
ai_conversation_send_full_finish(
    AiConversation  *self,
    GAsyncResult    *result,
    GError         **error
);

void
ai_conversation_set_command_set(
    AiConversation *self,
    AiCommandSet   *commands
);

AiCommandSet *
ai_conversation_get_command_set(AiConversation *self);

void
ai_conversation_set_working_directory(
    AiConversation *self,
    const gchar    *path
);

const gchar *
ai_conversation_get_working_directory(AiConversation *self);

void
ai_conversation_set_passthrough_commands(
    AiConversation *self,
    gboolean        passthrough
);

gboolean
ai_conversation_get_passthrough_commands(AiConversation *self);

AiCommandResult *
ai_conversation_resolve_input(
    AiConversation  *self,
    const gchar     *line,
    GCancellable    *cancellable,
    GError         **error
);

void
ai_conversation_send_input_async(
    AiConversation      *self,
    const gchar         *line,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

gboolean
ai_conversation_send_input_finish(
    AiConversation   *self,
    GAsyncResult     *result,
    AiCommandResult **out_command,
    GError          **error
);

G_END_DECLS
