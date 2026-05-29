/*
 * ai-duckduckgo-search.h - Keyless DuckDuckGo web search provider
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiDuckDuckGoSearch implements AiSearchProvider against DuckDuckGo's keyless
 * "lite" HTML endpoint. It needs no API key, which makes it the zero-config
 * default for ai_search_provider_new_default(). Because it scrapes an HTML
 * SERP it is BEST-EFFORT: DuckDuckGo may rate-limit or change its markup, in
 * which case a search returns an empty result list rather than an error.
 *
 * Usage:
 *   g_autoptr(AiDuckDuckGoSearch) ddg = ai_duckduckgo_search_new();
 *   ai_tool_executor_set_search_provider(executor, AI_SEARCH_PROVIDER(ddg));
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "convenience/ai-search-provider.h"

G_BEGIN_DECLS

#define AI_TYPE_DUCKDUCKGO_SEARCH (ai_duckduckgo_search_get_type())

G_DECLARE_FINAL_TYPE(AiDuckDuckGoSearch, ai_duckduckgo_search, AI, DUCKDUCKGO_SEARCH, GObject)

/**
 * ai_duckduckgo_search_new:
 *
 * Creates a new keyless #AiDuckDuckGoSearch provider.
 *
 * Returns: (transfer full): a new #AiDuckDuckGoSearch
 */
AiDuckDuckGoSearch *
ai_duckduckgo_search_new (void);

G_END_DECLS
