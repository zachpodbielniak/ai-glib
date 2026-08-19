/*
 * ai-agent-host.h - What the embedding application provides to agents
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
#include <gio/gio.h>

#include "core/ai-tool-endpoint.h"

G_BEGIN_DECLS

#define AI_TYPE_AGENT_HOST (ai_agent_host_get_type())

G_DECLARE_INTERFACE (AiAgentHost, ai_agent_host, AI, AGENT_HOST, GObject)

typedef struct _AiAgent AiAgent;


/**
 * AiAgentHostInterface:
 * @parent_iface: the parent interface
 * @provision: grants an agent a scoped tool endpoint
 * @revoke: withdraws it
 * @list_tools: exposes the host's tools to an in-process agent
 * @request_approval_async: asks a human to confirm something
 * @request_approval_finish: completes that request
 * @_reserved: reserved for future expansion
 *
 * The contract the embedding application implements so agents can reach
 * its tools and its user.
 *
 * ai-glib calls these and never learns what is behind them.  It must not
 * depend on any particular tool protocol: one host provisions a config
 * file naming a bridge process, another mints a token against its own
 * server, and neither arrangement belongs in this library.
 *
 * @list_tools and @provision are two views of one permission decision --
 * an in-process agent receives tools directly, a subprocess is pointed
 * at an endpoint -- so a host that implements both from the same
 * allowlist gives an agent the same authority whichever way it runs.
 */
struct _AiAgentHostInterface
{
    GTypeInterface parent_iface;

    gboolean (*provision) (AiAgentHost          *self,
                           AiAgent              *agent,
                           const gchar * const  *tool_allowlist,
                           AiAgentEndpoint     **out_endpoint,
                           GError              **error);
    void     (*revoke)    (AiAgentHost          *self,
                           AiAgent              *agent);

    GList *  (*list_tools)(AiAgentHost          *self,
                           const gchar * const  *tool_allowlist);

    void     (*request_approval_async)  (AiAgentHost         *self,
                                         AiAgent             *agent,
                                         const gchar         *tool,
                                         const gchar         *summary,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data);
    gboolean (*request_approval_finish) (AiAgentHost   *self,
                                         GAsyncResult  *result,
                                         GError       **error);

    /*< private >*/
    gpointer _reserved[8];
};

gboolean ai_agent_host_provision (AiAgentHost *self, AiAgent *agent,
                                  const gchar * const *tool_allowlist,
                                  AiAgentEndpoint **out_endpoint,
                                  GError **error);
void     ai_agent_host_revoke    (AiAgentHost *self, AiAgent *agent);
GList   *ai_agent_host_list_tools(AiAgentHost *self,
                                  const gchar * const *tool_allowlist);
void     ai_agent_host_request_approval_async  (AiAgentHost *self,
                                                AiAgent *agent,
                                                const gchar *tool,
                                                const gchar *summary,
                                                GCancellable *cancellable,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data);
gboolean ai_agent_host_request_approval_finish (AiAgentHost *self,
                                                GAsyncResult *result,
                                                GError **error);

G_END_DECLS
