/*
 * ai-brave-search.h - Brave Web Search provider
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiBraveSearch implements AiSearchProvider using the Brave Search API.
 * Requires a valid X-Subscription-Token from Brave Search API.
 *
 * Usage:
 *   g_autoptr(AiBraveSearch) brave = ai_brave_search_new("your-api-key");
 *   ai_tool_executor_set_search_provider(executor, AI_SEARCH_PROVIDER(brave));
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "convenience/ai-search-provider.h"

G_BEGIN_DECLS

#define AI_TYPE_BRAVE_SEARCH (ai_brave_search_get_type())

G_DECLARE_FINAL_TYPE(AiBraveSearch, ai_brave_search, AI, BRAVE_SEARCH, GObject)

/**
 * ai_brave_search_new:
 * @api_key: the Brave Search API key (X-Subscription-Token)
 *
 * Creates a new #AiBraveSearch provider.
 *
 * The API key is obtained from https://brave.com/search/api/.
 * Pass the resulting object to ai_tool_executor_set_search_provider()
 * to enable the web_search tool.
 *
 * Returns: (transfer full): a new #AiBraveSearch
 */
AiBraveSearch *
ai_brave_search_new (const gchar *api_key);

G_END_DECLS
