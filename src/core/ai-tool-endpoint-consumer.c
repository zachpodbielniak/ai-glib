/*
 * ai-tool-endpoint-consumer.c - A provider that can be handed tools
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "core/ai-tool-endpoint-consumer.h"
#include "core/ai-error.h"

G_DEFINE_INTERFACE (AiToolEndpointConsumer, ai_tool_endpoint_consumer,
                    G_TYPE_OBJECT)

static void
ai_tool_endpoint_consumer_default_init (
    AiToolEndpointConsumerInterface *iface
){
    (void)iface;
}

/**
 * ai_tool_endpoint_consumer_get_supported_kinds:
 * @self: an #AiToolEndpointConsumer
 *
 * The #AiAgentEndpoint kinds @self can be handed.
 *
 * A host asks this before minting anything, so that a credential is
 * never created for a provider that cannot receive it -- a live token
 * nothing consumes is a secret with no purpose.
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): the
 *   kinds, or %NULL if this consumer accepts none
 */
const gchar * const *
ai_tool_endpoint_consumer_get_supported_kinds (AiToolEndpointConsumer *self)
{
    AiToolEndpointConsumerInterface *iface;

    g_return_val_if_fail(AI_IS_TOOL_ENDPOINT_CONSUMER(self), NULL);

    iface = AI_TOOL_ENDPOINT_CONSUMER_GET_IFACE(self);
    if (iface->get_supported_kinds == NULL)
    {
        return NULL;
    }

    return iface->get_supported_kinds(self);
}

/**
 * ai_tool_endpoint_consumer_supports_kind:
 * @self: an #AiToolEndpointConsumer
 * @kind: an endpoint kind
 *
 * Returns: whether @self accepts an endpoint of @kind
 */
gboolean
ai_tool_endpoint_consumer_supports_kind (
    AiToolEndpointConsumer *self,
    const gchar            *kind
){
    const gchar * const *kinds;
    gsize i;

    g_return_val_if_fail(AI_IS_TOOL_ENDPOINT_CONSUMER(self), FALSE);
    g_return_val_if_fail(kind != NULL, FALSE);

    kinds = ai_tool_endpoint_consumer_get_supported_kinds(self);
    if (kinds == NULL)
    {
        return FALSE;
    }

    for (i = 0; kinds[i] != NULL; i++)
    {
        if (g_strcmp0(kinds[i], kind) == 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/**
 * ai_tool_endpoint_consumer_apply:
 * @self: an #AiToolEndpointConsumer
 * @endpoint: (nullable): the endpoint, or %NULL to revoke
 * @error: return location for a #GError
 *
 * Points @self at the tools @endpoint describes.
 *
 * A %NULL @endpoint revokes: an implementation must undo there exactly
 * what it did on apply, so that a provisioned credential leaves no trace
 * once the run that owned it is over.
 *
 * Applying is the only operation, and there is deliberately no property
 * setter for it: it can fail -- an unsupported kind, a temporary
 * directory that could not be created -- and a property setter has
 * nowhere to put a #GError.  Watch `notify::tool-endpoint` for changes.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_tool_endpoint_consumer_apply (
    AiToolEndpointConsumer *self,
    const AiAgentEndpoint  *endpoint,
    GError                **error
){
    AiToolEndpointConsumerInterface *iface;

    g_return_val_if_fail(AI_IS_TOOL_ENDPOINT_CONSUMER(self), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    iface = AI_TOOL_ENDPOINT_CONSUMER_GET_IFACE(self);
    if (iface->apply_endpoint == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "%s cannot be handed a tool endpoint",
                    G_OBJECT_TYPE_NAME(self));
        return FALSE;
    }

    return iface->apply_endpoint(self, endpoint, error);
}

/**
 * ai_tool_endpoint_consumer_clear:
 * @self: an #AiToolEndpointConsumer
 * @error: return location for a #GError
 *
 * Revokes whatever endpoint is in force.  Equivalent to
 * ai_tool_endpoint_consumer_apply() with %NULL, and named because a
 * revoke reads badly as an apply of nothing.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_tool_endpoint_consumer_clear (
    AiToolEndpointConsumer *self,
    GError                **error
){
    return ai_tool_endpoint_consumer_apply(self, NULL, error);
}

/**
 * ai_tool_endpoint_consumer_get_endpoint:
 * @self: an #AiToolEndpointConsumer
 *
 * Returns: (transfer none) (nullable): the endpoint in force, or %NULL
 */
const AiAgentEndpoint *
ai_tool_endpoint_consumer_get_endpoint (AiToolEndpointConsumer *self)
{
    AiToolEndpointConsumerInterface *iface;

    g_return_val_if_fail(AI_IS_TOOL_ENDPOINT_CONSUMER(self), NULL);

    iface = AI_TOOL_ENDPOINT_CONSUMER_GET_IFACE(self);
    if (iface->get_endpoint == NULL)
    {
        return NULL;
    }

    return iface->get_endpoint(self);
}
