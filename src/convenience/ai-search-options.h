/*
 * ai-search-options.h - Tunable options for a web search
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiSearchOptions carries the optional knobs for ai_search_provider_search().
 * Pass %NULL to a search call to use the defaults. Each #AiSearchProvider
 * maps the options it understands to its backend and ignores the rest.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "core/ai-enums.h"

G_BEGIN_DECLS

/* Hard cap on how many result pages fetch-content will retrieve. */
#define AI_SEARCH_MAX_FETCH_COUNT 5

#define AI_TYPE_SEARCH_OPTIONS (ai_search_options_get_type())

G_DECLARE_FINAL_TYPE(AiSearchOptions, ai_search_options, AI, SEARCH_OPTIONS, GObject)

AiSearchOptions *
ai_search_options_new(void);

AiSearchOptions *
ai_search_options_copy(AiSearchOptions *self);

guint
ai_search_options_get_count(AiSearchOptions *self);

void
ai_search_options_set_count(
    AiSearchOptions *self,
    guint            count
);

AiSearchFreshness
ai_search_options_get_freshness(AiSearchOptions *self);

void
ai_search_options_set_freshness(
    AiSearchOptions   *self,
    AiSearchFreshness  freshness
);

AiSearchSafeSearch
ai_search_options_get_safesearch(AiSearchOptions *self);

void
ai_search_options_set_safesearch(
    AiSearchOptions    *self,
    AiSearchSafeSearch  safesearch
);

const gchar *
ai_search_options_get_country(AiSearchOptions *self);

void
ai_search_options_set_country(
    AiSearchOptions *self,
    const gchar     *country
);

const gchar *
ai_search_options_get_language(AiSearchOptions *self);

void
ai_search_options_set_language(
    AiSearchOptions *self,
    const gchar     *language
);

const gchar *
ai_search_options_get_site(AiSearchOptions *self);

void
ai_search_options_set_site(
    AiSearchOptions *self,
    const gchar     *site
);

guint
ai_search_options_get_offset(AiSearchOptions *self);

void
ai_search_options_set_offset(
    AiSearchOptions *self,
    guint            offset
);

gboolean
ai_search_options_get_fetch_content(AiSearchOptions *self);

void
ai_search_options_set_fetch_content(
    AiSearchOptions *self,
    gboolean         fetch_content
);

guint
ai_search_options_get_fetch_count(AiSearchOptions *self);

void
ai_search_options_set_fetch_count(
    AiSearchOptions *self,
    guint            fetch_count
);

G_END_DECLS
