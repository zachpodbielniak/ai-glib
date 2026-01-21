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

/**
 * ai_streamable_chat_stream_async:
 * @self: an #AiStreamable
 * @messages: (element-type AiMessage): the conversation messages
 * @system_prompt: (nullable): system prompt to use
 * @max_tokens: maximum tokens to generate
 * @tools: (nullable) (element-type AiTool): tools available to the model
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): callback to call when done
 * @user_data: user data for the callback
 *
 * Starts an asynchronous streaming chat completion request.
 * Connect to the "delta" signal to receive text as it's generated.
 * Connect to "stream-end" to receive the final response.
 */
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

/**
 * ai_streamable_chat_stream_finish:
 * @self: an #AiStreamable
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes an asynchronous streaming chat completion request.
 *
 * Returns: (transfer full) (nullable): the complete #AiResponse, or %NULL on error
 */
AiResponse *
ai_streamable_chat_stream_finish(
    AiStreamable  *self,
    GAsyncResult  *result,
    GError       **error
);

G_END_DECLS
