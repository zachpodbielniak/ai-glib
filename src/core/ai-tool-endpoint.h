/*
 * ai-tool-endpoint.h - Where an agent's tools live
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * This type was declared in agent/ai-agent-host.h, which is the wrong
 * layer for it: core/ai-cli-client.c has to apply an endpoint, and
 * including the agent header from core would invert the dependency
 * arrow and drag AiAgentHost and AiAgent along with it.  The type keeps
 * its name and every one of its symbols; ai-agent-host.h now includes
 * this file, so nothing that used it had to change.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * AiAgentEndpoint:
 * @kind: how to reach the tools -- see the AI_ENDPOINT_KIND_* macros
 * @value: the path, URL or command line, interpreted per @kind
 * @env: (nullable) (element-type utf8 utf8): variables the worker must
 *   set, typically carrying a credential
 * @ttl_seconds: 0 for no expiry
 *
 * An opaque description of where an agent's tools live.
 *
 * ai-glib does not interpret this beyond handing it to a worker or to
 * an #AiToolEndpointConsumer.  That is the point: the library must not
 * learn what MCP is, so the host describes the arrangement and ai-glib
 * passes it along.
 *
 * The split that makes this work is dialect versus delivery.  The bytes
 * describing a tool server are a *dialect* and belong to the host,
 * because the host owns the server it is publishing.  Which flag,
 * environment variable or directory carries them is *delivery* and
 * belongs to the provider, because it is that CLI's mechanism.  @kind is
 * the whole of the negotiation between the two: a consumer advertises
 * the kinds it accepts and the host mints the one that was asked for,
 * so neither side has to know the other's business.
 *
 * Credentials belong in @env rather than on a command line: argv is
 * readable by any process on the machine through /proc/PID/cmdline,
 * while /proc/PID/environ is 0400 and owner-only.
 */
typedef struct
{
    gchar      *kind;
    gchar      *value;
    GHashTable *env;
    gint64      ttl_seconds;
} AiAgentEndpoint;

#define AI_TYPE_AGENT_ENDPOINT (ai_agent_endpoint_get_type())
GType ai_agent_endpoint_get_type (void) G_GNUC_CONST;

AiAgentEndpoint *ai_agent_endpoint_new  (const gchar *kind, const gchar *value);
AiAgentEndpoint *ai_agent_endpoint_copy (const AiAgentEndpoint *self);
void             ai_agent_endpoint_free (AiAgentEndpoint *self);
void             ai_agent_endpoint_set_env (AiAgentEndpoint *self,
                                            const gchar *key,
                                            const gchar *value);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (AiAgentEndpoint, ai_agent_endpoint_free)

/*
 * One spelling of every kind, shared by hosts and consumers.  A string
 * rather than an enum on purpose: a host can invent a kind for a CLI
 * ai-glib has never heard of, and a consumer that does not advertise it
 * simply refuses.
 */

/**
 * AI_ENDPOINT_KIND_ENV:
 *
 * Everything is in #AiAgentEndpoint.env; @value is unused.  Every
 * #AiCliClient accepts this, so it is the escape hatch for a host that
 * needs to set a variable this library does not know about.
 */
#define AI_ENDPOINT_KIND_ENV "env"

/**
 * AI_ENDPOINT_KIND_MCP_CONFIG:
 *
 * @value is a path to a JSON file in Claude Code's `.mcp.json` schema.
 */
#define AI_ENDPOINT_KIND_MCP_CONFIG "mcp-config"

/**
 * AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE:
 *
 * @value is a path to a JSON file in opencode's own schema, which is
 * not Claude's -- it nests under `mcp` and spells a server
 * `{"type": "local", "command": [...], "environment": {...}}`.
 */
#define AI_ENDPOINT_KIND_MCP_CONFIG_OPENCODE "mcp-config-opencode"

/**
 * AI_ENDPOINT_KIND_MCP_CONFIG_GROK:
 *
 * @value is a path to a TOML fragment declaring `[mcp_servers.NAME]`
 * tables, which the grok client appends to a copy of the user's config.
 */
#define AI_ENDPOINT_KIND_MCP_CONFIG_GROK "mcp-config-grok"

/**
 * AI_ENDPOINT_KIND_HTTP_URL:
 *
 * @value is a URL the worker connects to.
 */
#define AI_ENDPOINT_KIND_HTTP_URL "http-url"

/**
 * AI_ENDPOINT_KIND_STDIO:
 *
 * @value is a command line the worker runs and speaks to over stdio.
 */
#define AI_ENDPOINT_KIND_STDIO "stdio"

G_END_DECLS
