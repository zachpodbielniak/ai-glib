/*
 * ai-tool-endpoint-consumer.h - A provider that can be handed tools
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

#include "core/ai-tool-endpoint.h"

G_BEGIN_DECLS

#define AI_TYPE_TOOL_ENDPOINT_CONSUMER (ai_tool_endpoint_consumer_get_type())

G_DECLARE_INTERFACE (AiToolEndpointConsumer, ai_tool_endpoint_consumer,
                     AI, TOOL_ENDPOINT_CONSUMER, GObject)

/**
 * AiToolEndpointConsumerInterface:
 * @parent_iface: the parent interface
 * @get_supported_kinds: the #AiAgentEndpoint kinds this consumer accepts
 * @apply_endpoint: takes an endpoint, or %NULL to revoke the current one
 * @get_endpoint: the endpoint in force, or %NULL
 * @_reserved: reserved for future expansion
 *
 * Implemented by a provider that can be pointed at tools it does not
 * host itself -- in practice a CLI wrapper, which runs its own tools in
 * its own process and takes a config file, an environment variable or a
 * directory saying where to find more.
 *
 * An interface rather than a class vfunc for three reasons.  Callers
 * hold a #GObject -- #AiConversation:provider is constrained only by
 * %AI_IS_PROVIDER -- so a vfunc would force an %AI_IS_CLI_CLIENT check
 * at every call site, which is exactly the per-provider branching this
 * exists to delete.  The capability is not #AiCliClient's to monopolise:
 * a remote-agent bridge or a detached worker is a legitimate consumer
 * and subclasses nothing.  And a host has to ask "can you take one, and
 * of what kind?" before it mints a credential, which is a runtime type
 * question.
 *
 * #AiCliClient implements this once for all four CLI wrappers, with the
 * per-CLI delivery step as an #AiCliClientClass.endpoint_applied vfunc.
 * That is not fence-sitting: the interface is the contract, the class
 * vfunc is an implementation detail of the single implementer, and it
 * keeps validation, storage and notification in one place instead of
 * repeated behind four %G_IMPLEMENT_INTERFACE blocks.
 */
struct _AiToolEndpointConsumerInterface
{
    GTypeInterface parent_iface;

    const gchar * const *   (*get_supported_kinds) (AiToolEndpointConsumer  *self);
    gboolean                (*apply_endpoint)      (AiToolEndpointConsumer  *self,
                                                    const AiAgentEndpoint   *endpoint,
                                                    GError                 **error);
    const AiAgentEndpoint * (*get_endpoint)        (AiToolEndpointConsumer  *self);

    /*< private >*/
    gpointer _reserved[8];
};

const gchar * const *
ai_tool_endpoint_consumer_get_supported_kinds (AiToolEndpointConsumer *self);

gboolean
ai_tool_endpoint_consumer_supports_kind (
    AiToolEndpointConsumer *self,
    const gchar            *kind
);

gboolean
ai_tool_endpoint_consumer_apply (
    AiToolEndpointConsumer *self,
    const AiAgentEndpoint  *endpoint,
    GError                **error
);

gboolean
ai_tool_endpoint_consumer_clear (
    AiToolEndpointConsumer *self,
    GError                **error
);

const AiAgentEndpoint *
ai_tool_endpoint_consumer_get_endpoint (AiToolEndpointConsumer *self);

G_END_DECLS
