/*
 * ai-provider.h - Provider interface
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

#include "core/ai-enums.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-tool.h"

G_BEGIN_DECLS

#define AI_TYPE_PROVIDER (ai_provider_get_type())

G_DECLARE_INTERFACE(AiProvider, ai_provider, AI, PROVIDER, GObject)

/**
 * AiProviderInterface:
 * @parent_iface: the parent interface
 * @get_provider_type: gets the provider type
 * @get_name: gets the provider name
 * @get_default_model: gets the default model name
 * @chat_async: starts an async chat completion
 * @chat_finish: finishes an async chat completion
 * @list_models_async: starts listing available models
 * @list_models_finish: finishes listing available models
 * @_reserved: reserved for future expansion
 *
 * Interface for AI providers.
 */
struct _AiProviderInterface
{
    GTypeInterface parent_iface;

    /* Virtual methods */
    AiProviderType (*get_provider_type) (AiProvider *self);
    const gchar *  (*get_name)          (AiProvider *self);
    const gchar *  (*get_default_model) (AiProvider *self);

    void           (*chat_async)        (AiProvider          *self,
                                         GList               *messages,
                                         const gchar         *system_prompt,
                                         gint                 max_tokens,
                                         GList               *tools,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data);
    AiResponse *   (*chat_finish)       (AiProvider          *self,
                                         GAsyncResult        *result,
                                         GError             **error);

    void           (*list_models_async) (AiProvider          *self,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data);
    GList *        (*list_models_finish)(AiProvider          *self,
                                         GAsyncResult        *result,
                                         GError             **error);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

/**
 * ai_provider_get_provider_type:
 * @self: an #AiProvider
 *
 * Gets the provider type.
 *
 * Returns: the #AiProviderType
 */
AiProviderType
ai_provider_get_provider_type(AiProvider *self);

/**
 * ai_provider_get_name:
 * @self: an #AiProvider
 *
 * Gets the human-readable provider name.
 *
 * Returns: (transfer none): the provider name
 */
const gchar *
ai_provider_get_name(AiProvider *self);

/**
 * ai_provider_get_default_model:
 * @self: an #AiProvider
 *
 * Gets the default model name for this provider.
 *
 * Returns: (transfer none): the default model name
 */
const gchar *
ai_provider_get_default_model(AiProvider *self);

/**
 * ai_provider_chat_async:
 * @self: an #AiProvider
 * @messages: (element-type AiMessage): the conversation messages
 * @system_prompt: (nullable): system prompt to use
 * @max_tokens: maximum tokens to generate
 * @tools: (nullable) (element-type AiTool): tools available to the model
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): callback to call when done
 * @user_data: user data for the callback
 *
 * Starts an asynchronous chat completion request.
 */
void
ai_provider_chat_async(
    AiProvider          *self,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

/**
 * ai_provider_chat_finish:
 * @self: an #AiProvider
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes an asynchronous chat completion request.
 *
 * Returns: (transfer full) (nullable): the #AiResponse, or %NULL on error
 */
AiResponse *
ai_provider_chat_finish(
    AiProvider    *self,
    GAsyncResult  *result,
    GError       **error
);

/**
 * ai_provider_list_models_async:
 * @self: an #AiProvider
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): callback to call when done
 * @user_data: user data for the callback
 *
 * Lists available models from the provider.
 */
void
ai_provider_list_models_async(
    AiProvider          *self,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

/**
 * ai_provider_list_models_finish:
 * @self: an #AiProvider
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes listing available models.
 *
 * Returns: (transfer full) (element-type utf8) (nullable): list of model names
 */
GList *
ai_provider_list_models_finish(
    AiProvider    *self,
    GAsyncResult  *result,
    GError       **error
);

G_END_DECLS
