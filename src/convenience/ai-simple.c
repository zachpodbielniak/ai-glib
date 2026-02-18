/*
 * ai-simple.c - Simple convenience API for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include "core/ai-client.h"
#include "core/ai-cli-client.h"
#include "core/ai-config.h"
#include "core/ai-enums.h"
#include "core/ai-provider.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "providers/ai-claude-client.h"
#include "providers/ai-openai-client.h"
#include "providers/ai-gemini-client.h"
#include "providers/ai-grok-client.h"
#include "providers/ai-ollama-client.h"
#include "providers/ai-claude-code-client.h"
#include "providers/ai-opencode-client.h"

#include "convenience/ai-simple.h"

struct _AiSimple
{
    GObject     parent_instance;

    AiConfig   *config;         /* owned reference */
    GObject    *provider;       /* owned — concrete provider (AiClient or AiCliClient subclass) */
    GList      *history;        /* element-type AiMessage, owned */
    gchar      *system_prompt;  /* owned, nullable */
};

G_DEFINE_TYPE(AiSimple, ai_simple, G_TYPE_OBJECT)

/*
 * ai_simple_create_provider:
 * @config: an #AiConfig
 * @provider_type: the provider to instantiate
 * @model: (nullable): model override, or NULL for provider default
 *
 * Factory function that creates the correct provider subclass for the
 * given provider type using the provided config. Sets the model if
 * specified.
 *
 * Returns: (transfer full): a new provider as GObject
 */
static GObject *
ai_simple_create_provider(
    AiConfig       *config,
    AiProviderType  provider_type,
    const gchar    *model
){
    GObject *provider;

    switch (provider_type)
    {
    case AI_PROVIDER_CLAUDE:
        provider = G_OBJECT(ai_claude_client_new_with_config(config));
        if (model != NULL)
            ai_client_set_model(AI_CLIENT(provider), model);
        break;

    case AI_PROVIDER_OPENAI:
        provider = G_OBJECT(ai_openai_client_new_with_config(config));
        if (model != NULL)
            ai_client_set_model(AI_CLIENT(provider), model);
        break;

    case AI_PROVIDER_GEMINI:
        provider = G_OBJECT(ai_gemini_client_new_with_config(config));
        if (model != NULL)
            ai_client_set_model(AI_CLIENT(provider), model);
        break;

    case AI_PROVIDER_GROK:
        provider = G_OBJECT(ai_grok_client_new_with_config(config));
        if (model != NULL)
            ai_client_set_model(AI_CLIENT(provider), model);
        break;

    case AI_PROVIDER_OLLAMA:
        provider = G_OBJECT(ai_ollama_client_new_with_config(config));
        if (model != NULL)
            ai_client_set_model(AI_CLIENT(provider), model);
        break;

    case AI_PROVIDER_CLAUDE_CODE:
        provider = G_OBJECT(ai_claude_code_client_new_with_config(config));
        if (model != NULL)
            ai_cli_client_set_model(AI_CLI_CLIENT(provider), model);
        break;

    case AI_PROVIDER_OPENCODE:
        provider = G_OBJECT(ai_opencode_client_new_with_config(config));
        if (model != NULL)
            ai_cli_client_set_model(AI_CLI_CLIENT(provider), model);
        break;

    default:
        /* Fallback to Ollama if we get an unknown type */
        provider = G_OBJECT(ai_ollama_client_new_with_config(config));
        if (model != NULL)
            ai_client_set_model(AI_CLIENT(provider), model);
        break;
    }

    return provider;
}

/*
 * ai_simple_do_chat_sync:
 * @self: an #AiSimple
 * @messages: (element-type AiMessage): the messages to send
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Internal helper that dispatches chat_sync to the correct base class
 * depending on whether the provider is an AiClient or AiCliClient.
 * Also sets the system prompt on the underlying provider before calling.
 *
 * Returns: (transfer full) (nullable): the #AiResponse
 */
static AiResponse *
ai_simple_do_chat_sync(
    AiSimple      *self,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
){
    /* Set system prompt on the underlying provider */
    if (AI_IS_CLIENT(self->provider))
    {
        ai_client_set_system_prompt(
            AI_CLIENT(self->provider), self->system_prompt);
        return ai_client_chat_sync(
            AI_CLIENT(self->provider), messages, cancellable, error);
    }
    else if (AI_IS_CLI_CLIENT(self->provider))
    {
        ai_cli_client_set_system_prompt(
            AI_CLI_CLIENT(self->provider), self->system_prompt);
        return ai_cli_client_chat_sync(
            AI_CLI_CLIENT(self->provider), messages, cancellable, error);
    }

    g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                        "Unknown provider type in AiSimple");
    return NULL;
}

static void
ai_simple_finalize(GObject *object)
{
    AiSimple *self = AI_SIMPLE(object);

    g_clear_object(&self->config);
    g_clear_object(&self->provider);
    g_list_free_full(self->history, g_object_unref);
    self->history = NULL;
    g_clear_pointer(&self->system_prompt, g_free);

    G_OBJECT_CLASS(ai_simple_parent_class)->finalize(object);
}

static void
ai_simple_class_init(AiSimpleClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_simple_finalize;
}

static void
ai_simple_init(AiSimple *self)
{
    self->config = NULL;
    self->provider = NULL;
    self->history = NULL;
    self->system_prompt = NULL;
}

AiSimple *
ai_simple_new(void)
{
    g_autoptr(AiConfig) config = NULL;
    AiProviderType provider_type;
    const gchar *model;

    config = ai_config_new();
    provider_type = ai_config_get_default_provider(config);
    model = ai_config_get_default_model(config);

    return ai_simple_new_with_provider(provider_type, model);
}

AiSimple *
ai_simple_new_with_provider(
    AiProviderType  provider_type,
    const gchar    *model
){
    AiSimple *self;
    g_autoptr(AiConfig) config = NULL;

    self = g_object_new(AI_TYPE_SIMPLE, NULL);

    config = ai_config_new();
    self->config = (AiConfig *)g_steal_pointer(&config);
    self->provider = ai_simple_create_provider(
        self->config, provider_type, model);

    return self;
}

AiSimple *
ai_simple_new_with_config(AiConfig *config)
{
    AiSimple *self;
    AiProviderType provider_type;
    const gchar *model;

    g_return_val_if_fail(config != NULL, NULL);

    self = g_object_new(AI_TYPE_SIMPLE, NULL);

    self->config = g_object_ref(config);
    provider_type = ai_config_get_default_provider(config);
    model = ai_config_get_default_model(config);
    self->provider = ai_simple_create_provider(
        self->config, provider_type, model);

    return self;
}

gchar *
ai_simple_prompt(
    AiSimple      *self,
    const gchar   *prompt,
    GCancellable  *cancellable,
    GError       **error
){
    g_autoptr(AiMessage) msg = NULL;
    g_autoptr(AiResponse) response = NULL;
    GList *messages;

    g_return_val_if_fail(AI_IS_SIMPLE(self), NULL);
    g_return_val_if_fail(prompt != NULL, NULL);

    /* Build a single-message list */
    msg = ai_message_new_user(prompt);
    messages = g_list_append(NULL, msg);

    /* Send to the provider */
    response = ai_simple_do_chat_sync(self, messages, cancellable, error);
    g_list_free(messages);

    if (response == NULL)
        return NULL;

    return ai_response_get_text(response);
}

gchar *
ai_simple_chat(
    AiSimple      *self,
    const gchar   *prompt,
    GCancellable  *cancellable,
    GError       **error
){
    AiMessage *user_msg;
    g_autoptr(AiResponse) response = NULL;
    gchar *text;

    g_return_val_if_fail(AI_IS_SIMPLE(self), NULL);
    g_return_val_if_fail(prompt != NULL, NULL);

    /* Append user message to history */
    user_msg = ai_message_new_user(prompt);
    self->history = g_list_append(self->history, user_msg);

    /* Send the full history to the provider */
    response = ai_simple_do_chat_sync(
        self, self->history, cancellable, error);

    if (response == NULL)
        return NULL;

    /* Extract text and append assistant message to history */
    text = ai_response_get_text(response);
    if (text != NULL)
    {
        AiMessage *assistant_msg = ai_message_new_assistant(text);
        self->history = g_list_append(self->history, assistant_msg);
    }

    return text;
}

void
ai_simple_set_system_prompt(
    AiSimple    *self,
    const gchar *system_prompt
){
    g_return_if_fail(AI_IS_SIMPLE(self));

    g_free(self->system_prompt);
    self->system_prompt = g_strdup(system_prompt);
}

const gchar *
ai_simple_get_system_prompt(AiSimple *self)
{
    g_return_val_if_fail(AI_IS_SIMPLE(self), NULL);

    return self->system_prompt;
}

void
ai_simple_clear_history(AiSimple *self)
{
    g_return_if_fail(AI_IS_SIMPLE(self));

    g_list_free_full(self->history, g_object_unref);
    self->history = NULL;
}

AiProvider *
ai_simple_get_provider(AiSimple *self)
{
    g_return_val_if_fail(AI_IS_SIMPLE(self), NULL);

    return AI_PROVIDER(self->provider);
}
