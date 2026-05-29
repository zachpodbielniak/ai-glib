/*
 * ai-search-provider.h - Web search provider interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiSearchProvider is a GInterface that abstracts web search backends.
 * Implement this interface to add new search providers (Bing, Brave,
 * DuckDuckGo, ...). Concrete implementations are registered with
 * AiToolExecutor via ai_tool_executor_set_search_provider() to enable the
 * web_search tool.
 *
 * A search returns a GList of #AiSearchResult (transfer full); free with
 * g_list_free_full(list, g_object_unref). Use ai_search_results_format() to
 * render the list into a model-facing string.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "convenience/ai-search-result.h"
#include "convenience/ai-search-options.h"

G_BEGIN_DECLS

#define AI_TYPE_SEARCH_PROVIDER (ai_search_provider_get_type())

G_DECLARE_INTERFACE(AiSearchProvider, ai_search_provider, AI, SEARCH_PROVIDER, GObject)

/**
 * AiSearchProviderInterface:
 * @parent_iface: the parent interface
 * @search: perform a web search and return a list of #AiSearchResult
 * @_reserved: reserved for future expansion
 *
 * Interface for web search providers used by #AiToolExecutor.
 * Implement this interface to add new search backends.
 */
struct _AiSearchProviderInterface
{
    GTypeInterface parent_iface;

    /* Virtual methods */
    GList * (*search) (AiSearchProvider  *self,
                       const gchar       *query,
                       AiSearchOptions   *options,
                       GCancellable      *cancellable,
                       GError           **error);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

GList *
ai_search_provider_search (
    AiSearchProvider  *self,
    const gchar       *query,
    AiSearchOptions   *options,
    GCancellable      *cancellable,
    GError           **error
);

gchar *
ai_search_results_format (
    GList        *results,
    const gchar  *query,
    gboolean      include_content
);

AiSearchProvider *
ai_search_provider_new_default (GError **error);

G_END_DECLS
