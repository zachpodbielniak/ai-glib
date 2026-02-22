/*
 * ai-search-provider.h - Web search provider interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiSearchProvider is a GInterface that abstracts web search backends.
 * Implement this interface to add new search providers (Bing, Brave, etc.).
 * Concrete implementations are registered with AiToolExecutor via
 * ai_tool_executor_set_search_provider() to enable the web_search tool.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define AI_TYPE_SEARCH_PROVIDER (ai_search_provider_get_type())

G_DECLARE_INTERFACE(AiSearchProvider, ai_search_provider, AI, SEARCH_PROVIDER, GObject)

/**
 * AiSearchProviderInterface:
 * @parent_iface: the parent interface
 * @search: perform a web search and return formatted results
 * @_reserved: reserved for future expansion
 *
 * Interface for web search providers used by #AiToolExecutor.
 * Implement this interface to add new search backends.
 */
struct _AiSearchProviderInterface
{
    GTypeInterface parent_iface;

    /* Virtual methods */
    gchar * (*search) (AiSearchProvider  *self,
                       const gchar       *query,
                       GCancellable      *cancellable,
                       GError           **error);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

/**
 * ai_search_provider_search:
 * @self: an #AiSearchProvider
 * @query: the search query string
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Performs a web search and returns the results as a formatted string.
 * Each result is formatted as:
 *   Title\nURL\nSnippet\n---\n
 *
 * Returns: (transfer full) (nullable): the search results string, or %NULL
 *   on error. Free with g_free().
 */
gchar *
ai_search_provider_search (
    AiSearchProvider  *self,
    const gchar       *query,
    GCancellable      *cancellable,
    GError           **error
);

G_END_DECLS
