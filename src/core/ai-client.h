/*
 * ai-client.h - Base client class
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
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "core/ai-config.h"
#include "core/ai-provider.h"
#include "core/ai-streamable.h"
#include "model/ai-message.h"
#include "model/ai-response.h"

G_BEGIN_DECLS

#define AI_TYPE_CLIENT (ai_client_get_type())

G_DECLARE_DERIVABLE_TYPE(AiClient, ai_client, AI, CLIENT, GObject)

/**
 * AiClientClass:
 * @parent_class: the parent class
 * @build_request: builds the JSON request body for the provider
 * @parse_response: parses the JSON response from the provider
 * @get_endpoint_url: gets the API endpoint URL
 * @add_auth_headers: adds authentication headers to the request
 * @_reserved: reserved for future expansion
 *
 * Class structure for #AiClient.
 * Subclasses should override the virtual methods to implement provider-specific
 * request/response handling.
 */
struct _AiClientClass
{
    GObjectClass parent_class;

    /* Virtual methods for subclasses */
    JsonNode *   (*build_request)     (AiClient       *self,
                                       GList          *messages,
                                       const gchar    *system_prompt,
                                       gint            max_tokens,
                                       GList          *tools);
    AiResponse * (*parse_response)    (AiClient       *self,
                                       JsonNode       *json,
                                       GError        **error);
    gchar *      (*get_endpoint_url)  (AiClient       *self);
    void         (*add_auth_headers)  (AiClient       *self,
                                       SoupMessage    *msg);
    void         (*parse_stream_chunk)(AiClient       *self,
                                       const gchar    *chunk,
                                       gsize           length,
                                       GString        *buffer,
                                       AiResponse     *response);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

/**
 * ai_client_get_config:
 * @self: an #AiClient
 *
 * Gets the configuration for this client.
 *
 * Returns: (transfer none): the #AiConfig
 */
AiConfig *
ai_client_get_config(AiClient *self);

/**
 * ai_client_get_model:
 * @self: an #AiClient
 *
 * Gets the model name.
 *
 * Returns: (transfer none) (nullable): the model name
 */
const gchar *
ai_client_get_model(AiClient *self);

/**
 * ai_client_set_model:
 * @self: an #AiClient
 * @model: the model name
 *
 * Sets the model to use for requests.
 */
void
ai_client_set_model(
    AiClient    *self,
    const gchar *model
);

/**
 * ai_client_get_max_tokens:
 * @self: an #AiClient
 *
 * Gets the default max tokens setting.
 *
 * Returns: the max tokens
 */
gint
ai_client_get_max_tokens(AiClient *self);

/**
 * ai_client_set_max_tokens:
 * @self: an #AiClient
 * @max_tokens: the max tokens
 *
 * Sets the default max tokens for requests.
 */
void
ai_client_set_max_tokens(
    AiClient *self,
    gint      max_tokens
);

/**
 * ai_client_get_temperature:
 * @self: an #AiClient
 *
 * Gets the temperature setting.
 *
 * Returns: the temperature
 */
gdouble
ai_client_get_temperature(AiClient *self);

/**
 * ai_client_set_temperature:
 * @self: an #AiClient
 * @temperature: the temperature (0.0 to 2.0)
 *
 * Sets the temperature for response generation.
 */
void
ai_client_set_temperature(
    AiClient *self,
    gdouble   temperature
);

/**
 * ai_client_get_system_prompt:
 * @self: an #AiClient
 *
 * Gets the default system prompt.
 *
 * Returns: (transfer none) (nullable): the system prompt
 */
const gchar *
ai_client_get_system_prompt(AiClient *self);

/**
 * ai_client_set_system_prompt:
 * @self: an #AiClient
 * @system_prompt: (nullable): the system prompt
 *
 * Sets the default system prompt for requests.
 */
void
ai_client_set_system_prompt(
    AiClient    *self,
    const gchar *system_prompt
);

/**
 * ai_client_get_soup_session:
 * @self: an #AiClient
 *
 * Gets the SoupSession used for HTTP requests.
 *
 * Returns: (transfer none): the #SoupSession
 */
SoupSession *
ai_client_get_soup_session(AiClient *self);

/**
 * ai_client_chat_sync:
 * @self: an #AiClient
 * @messages: (element-type AiMessage): the conversation messages
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Performs a synchronous chat completion request.
 *
 * Returns: (transfer full) (nullable): the #AiResponse, or %NULL on error
 */
AiResponse *
ai_client_chat_sync(
    AiClient      *self,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
);

G_END_DECLS
