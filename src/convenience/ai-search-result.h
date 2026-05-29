/*
 * ai-search-result.h - A single web search result
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiSearchResult is a structured web search hit returned by an
 * #AiSearchProvider. A provider returns a GList of these (transfer full);
 * free with g_list_free_full(list, g_object_unref).
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

#define AI_TYPE_SEARCH_RESULT (ai_search_result_get_type())

G_DECLARE_FINAL_TYPE(AiSearchResult, ai_search_result, AI, SEARCH_RESULT, GObject)

AiSearchResult *
ai_search_result_new(
    const gchar *title,
    const gchar *url,
    const gchar *snippet
);

const gchar *
ai_search_result_get_title(AiSearchResult *self);

const gchar *
ai_search_result_get_url(AiSearchResult *self);

const gchar *
ai_search_result_get_snippet(AiSearchResult *self);

const gchar *
ai_search_result_get_age(AiSearchResult *self);

const gchar *
ai_search_result_get_source(AiSearchResult *self);

guint
ai_search_result_get_rank(AiSearchResult *self);

const gchar *
ai_search_result_get_content(AiSearchResult *self);

void
ai_search_result_set_title(
    AiSearchResult *self,
    const gchar    *title
);

void
ai_search_result_set_url(
    AiSearchResult *self,
    const gchar    *url
);

void
ai_search_result_set_snippet(
    AiSearchResult *self,
    const gchar    *snippet
);

void
ai_search_result_set_age(
    AiSearchResult *self,
    const gchar    *age
);

void
ai_search_result_set_source(
    AiSearchResult *self,
    const gchar    *source
);

void
ai_search_result_set_rank(
    AiSearchResult *self,
    guint           rank
);

void
ai_search_result_set_content(
    AiSearchResult *self,
    const gchar    *content
);

G_END_DECLS
