/*
 * ai-provider.c - Provider interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-provider.h"

G_DEFINE_INTERFACE(AiProvider, ai_provider, G_TYPE_OBJECT)

static void
ai_provider_default_init(AiProviderInterface *iface)
{
    /* Default implementations are NULL - subclasses must implement */
    (void)iface;
}

/**
 * ai_provider_get_provider_type:
 * @self: an #AiProvider
 *
 * Gets the provider type enumeration value for this provider.
 *
 * Returns: the #AiProviderType
 */
AiProviderType
ai_provider_get_provider_type(AiProvider *self)
{
    AiProviderInterface *iface;

    g_return_val_if_fail(AI_IS_PROVIDER(self), AI_PROVIDER_CLAUDE);

    iface = AI_PROVIDER_GET_IFACE(self);
    g_return_val_if_fail(iface->get_provider_type != NULL, AI_PROVIDER_CLAUDE);

    return iface->get_provider_type(self);
}

/**
 * ai_provider_get_name:
 * @self: an #AiProvider
 *
 * Gets the human-readable provider name (e.g., "Claude", "OpenAI").
 *
 * Returns: (transfer none): the provider name
 */
const gchar *
ai_provider_get_name(AiProvider *self)
{
    AiProviderInterface *iface;

    g_return_val_if_fail(AI_IS_PROVIDER(self), NULL);

    iface = AI_PROVIDER_GET_IFACE(self);
    g_return_val_if_fail(iface->get_name != NULL, NULL);

    return iface->get_name(self);
}

/**
 * ai_provider_get_default_model:
 * @self: an #AiProvider
 *
 * Gets the default model name for this provider.
 *
 * Returns: (transfer none): the default model name
 */
const gchar *
ai_provider_get_default_model(AiProvider *self)
{
    AiProviderInterface *iface;

    g_return_val_if_fail(AI_IS_PROVIDER(self), NULL);

    iface = AI_PROVIDER_GET_IFACE(self);
    g_return_val_if_fail(iface->get_default_model != NULL, NULL);

    return iface->get_default_model(self);
}

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
 * Call ai_provider_chat_finish() from the callback to get the result.
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
){
    AiProviderInterface *iface;

    g_return_if_fail(AI_IS_PROVIDER(self));

    iface = AI_PROVIDER_GET_IFACE(self);
    g_return_if_fail(iface->chat_async != NULL);

    iface->chat_async(self, messages, system_prompt, max_tokens, tools,
                      cancellable, callback, user_data);
}

/**
 * ai_provider_chat_finish:
 * @self: an #AiProvider
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes an asynchronous chat completion request started with
 * ai_provider_chat_async().
 *
 * Returns: (transfer full) (nullable): the #AiResponse, or %NULL on error
 */
AiResponse *
ai_provider_chat_finish(
    AiProvider    *self,
    GAsyncResult  *result,
    GError       **error
){
    AiProviderInterface *iface;

    g_return_val_if_fail(AI_IS_PROVIDER(self), NULL);

    iface = AI_PROVIDER_GET_IFACE(self);
    g_return_val_if_fail(iface->chat_finish != NULL, NULL);

    return iface->chat_finish(self, result, error);
}

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
){
    AiProviderInterface *iface;

    g_return_if_fail(AI_IS_PROVIDER(self));

    iface = AI_PROVIDER_GET_IFACE(self);
    g_return_if_fail(iface->list_models_async != NULL);

    iface->list_models_async(self, cancellable, callback, user_data);
}

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
){
    AiProviderInterface *iface;

    g_return_val_if_fail(AI_IS_PROVIDER(self), NULL);

    iface = AI_PROVIDER_GET_IFACE(self);
    g_return_val_if_fail(iface->list_models_finish != NULL, NULL);

    return iface->list_models_finish(self, result, error);
}
