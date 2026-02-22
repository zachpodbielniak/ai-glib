/*
 * ai-bing-search.h - Bing Web Search provider
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiBingSearch implements AiSearchProvider using the Bing Web Search API v7.
 * Requires a valid Ocp-Apim-Subscription-Key (Azure Cognitive Services).
 *
 * Usage:
 *   g_autoptr(AiBingSearch) bing = ai_bing_search_new("your-api-key");
 *   ai_tool_executor_set_search_provider(executor, AI_SEARCH_PROVIDER(bing));
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "convenience/ai-search-provider.h"

G_BEGIN_DECLS

#define AI_TYPE_BING_SEARCH (ai_bing_search_get_type())

G_DECLARE_FINAL_TYPE(AiBingSearch, ai_bing_search, AI, BING_SEARCH, GObject)

/**
 * ai_bing_search_new:
 * @api_key: the Bing Web Search API key (Ocp-Apim-Subscription-Key)
 *
 * Creates a new #AiBingSearch provider.
 *
 * The API key is obtained from Azure Cognitive Services.
 * Pass the resulting object to ai_tool_executor_set_search_provider()
 * to enable the web_search tool.
 *
 * Returns: (transfer full): a new #AiBingSearch
 */
AiBingSearch *
ai_bing_search_new (const gchar *api_key);

G_END_DECLS
