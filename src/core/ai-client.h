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

AiConfig *
ai_client_get_config(AiClient *self);

const gchar *
ai_client_get_model(AiClient *self);

void
ai_client_set_model(
    AiClient    *self,
    const gchar *model
);

gint
ai_client_get_max_tokens(AiClient *self);

void
ai_client_set_max_tokens(
    AiClient *self,
    gint      max_tokens
);

gdouble
ai_client_get_temperature(AiClient *self);

void
ai_client_set_temperature(
    AiClient *self,
    gdouble   temperature
);

const gchar *
ai_client_get_system_prompt(AiClient *self);

void
ai_client_set_system_prompt(
    AiClient    *self,
    const gchar *system_prompt
);

SoupSession *
ai_client_get_soup_session(AiClient *self);

AiResponse *
ai_client_chat_sync(
    AiClient      *self,
    GList         *messages,
    GCancellable  *cancellable,
    GError       **error
);

G_END_DECLS
