/*
 * ai-streamable.c - Streaming interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-streamable.h"

G_DEFINE_INTERFACE(AiStreamable, ai_streamable, G_TYPE_OBJECT)

/*
 * Signal IDs for streaming events.
 */
enum
{
    SIGNAL_DELTA,
    SIGNAL_STREAM_START,
    SIGNAL_STREAM_END,
    SIGNAL_TOOL_USE,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
ai_streamable_default_init(AiStreamableInterface *iface)
{
    (void)iface;

    /**
     * AiStreamable::delta:
     * @self: the object that received the signal
     * @text: the new text chunk
     *
     * Emitted when new text is received during streaming.
     */
    signals[SIGNAL_DELTA] =
        g_signal_new("delta",
                     G_TYPE_FROM_INTERFACE(iface),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1,
                     G_TYPE_STRING);

    /**
     * AiStreamable::stream-start:
     * @self: the object that received the signal
     *
     * Emitted when streaming begins.
     */
    signals[SIGNAL_STREAM_START] =
        g_signal_new("stream-start",
                     G_TYPE_FROM_INTERFACE(iface),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 0);

    /**
     * AiStreamable::stream-end:
     * @self: the object that received the signal
     * @response: the complete response
     *
     * Emitted when streaming ends with the complete response.
     */
    signals[SIGNAL_STREAM_END] =
        g_signal_new("stream-end",
                     G_TYPE_FROM_INTERFACE(iface),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1,
                     G_TYPE_OBJECT);

    /**
     * AiStreamable::tool-use:
     * @self: the object that received the signal
     * @tool_use: the tool use request
     *
     * Emitted when a tool use request is detected in the stream.
     */
    signals[SIGNAL_TOOL_USE] =
        g_signal_new("tool-use",
                     G_TYPE_FROM_INTERFACE(iface),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1,
                     G_TYPE_OBJECT);
}

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
){
    AiStreamableInterface *iface;

    g_return_if_fail(AI_IS_STREAMABLE(self));

    iface = AI_STREAMABLE_GET_IFACE(self);
    g_return_if_fail(iface->chat_stream_async != NULL);

    iface->chat_stream_async(self, messages, system_prompt, max_tokens, tools,
                             cancellable, callback, user_data);
}

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
){
    AiStreamableInterface *iface;

    g_return_val_if_fail(AI_IS_STREAMABLE(self), NULL);

    iface = AI_STREAMABLE_GET_IFACE(self);
    g_return_val_if_fail(iface->chat_stream_finish != NULL, NULL);

    return iface->chat_stream_finish(self, result, error);
}
