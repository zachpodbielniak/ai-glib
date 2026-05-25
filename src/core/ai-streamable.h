/*
 * ai-streamable.h - Streaming interface
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

#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-tool.h"

G_BEGIN_DECLS

#define AI_TYPE_STREAMABLE (ai_streamable_get_type())

G_DECLARE_INTERFACE(AiStreamable, ai_streamable, AI, STREAMABLE, GObject)

/**
 * AiStreamableInterface:
 * @parent_iface: the parent interface
 * @chat_stream_async: starts a streaming chat completion
 * @chat_stream_finish: finishes a streaming chat completion
 * @_reserved: reserved for future expansion
 *
 * Interface for streaming AI responses.
 *
 * Signals:
 * - "delta": (gchar *text) - emitted when new text is received
 * - "stream-start": () - emitted when streaming starts
 * - "stream-end": (AiResponse *response) - emitted when streaming ends
 * - "tool-use": (AiToolUse *tool_use) - emitted when a tool use is detected
 */
struct _AiStreamableInterface
{
    GTypeInterface parent_iface;

    /* Virtual methods */
    void         (*chat_stream_async)  (AiStreamable        *self,
                                        GList               *messages,
                                        const gchar         *system_prompt,
                                        gint                 max_tokens,
                                        GList               *tools,
                                        GCancellable        *cancellable,
                                        GAsyncReadyCallback  callback,
                                        gpointer             user_data);
    AiResponse * (*chat_stream_finish) (AiStreamable        *self,
                                        GAsyncResult        *result,
                                        GError             **error);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

void
ai_streamable_chat_stream_async(
    AiStreamable        *self,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

AiResponse *
ai_streamable_chat_stream_finish(
    AiStreamable  *self,
    GAsyncResult  *result,
    GError       **error
);

G_END_DECLS
