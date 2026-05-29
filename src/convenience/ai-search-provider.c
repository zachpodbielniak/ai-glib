/*
 * ai-search-provider.c - Web search provider interface
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include "convenience/ai-search-provider.h"
#include "convenience/ai-bing-search.h"
#include "convenience/ai-brave-search.h"
#include "convenience/ai-duckduckgo-search.h"
#include "core/ai-error.h"

G_DEFINE_INTERFACE(AiSearchProvider, ai_search_provider, G_TYPE_OBJECT)

static void
ai_search_provider_default_init (AiSearchProviderInterface *iface)
{
    (void)iface;
}

/**
 * ai_search_provider_search:
 * @self: an #AiSearchProvider
 * @query: the search query string
 * @options: (nullable): an #AiSearchOptions, or %NULL for defaults
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Performs a web search and returns the matching results in rank order.
 *
 * Returns: (transfer full) (element-type AiSearchResult) (nullable): a list of
 *   #AiSearchResult, or %NULL on error. The list may be empty when there are
 *   no results. Free with g_list_free_full(list, g_object_unref).
 */
GList *
ai_search_provider_search (
    AiSearchProvider  *self,
    const gchar       *query,
    AiSearchOptions   *options,
    GCancellable      *cancellable,
    GError           **error
){
    AiSearchProviderInterface *iface;

    g_return_val_if_fail (AI_IS_SEARCH_PROVIDER (self), NULL);
    g_return_val_if_fail (query != NULL, NULL);

    iface = AI_SEARCH_PROVIDER_GET_IFACE (self);

    if (iface->search == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "search provider has no search() implementation");
        return NULL;
    }

    return iface->search (self, query, options, cancellable, error);
}

/**
 * ai_search_results_format:
 * @results: (element-type AiSearchResult) (nullable): the results to render
 * @query: (nullable): the original query, used in the no-results message
 * @include_content: whether to include each result's fetched page content
 *
 * Renders a list of #AiSearchResult into a numbered, model-facing string.
 * Each entry shows its rank, title, URL, source domain, age (when known) and
 * snippet; when @include_content is %TRUE and a result carries fetched page
 * text, that text is appended too. An empty/NULL list yields an explicit
 * "No results found" message rather than an empty string.
 *
 * Returns: (transfer full): the formatted results string. Free with g_free().
 */
gchar *
ai_search_results_format (
    GList        *results,
    const gchar  *query,
    gboolean      include_content
){
    GString *out;
    GList   *l;
    guint    n = 0;

    out = g_string_new (NULL);

    for (l = results; l != NULL; l = l->next)
    {
        AiSearchResult *r = l->data;
        const gchar    *title;
        const gchar    *url;
        const gchar    *snippet;
        const gchar    *source;
        const gchar    *age;
        const gchar    *content;
        guint           rank;

        if (!AI_IS_SEARCH_RESULT (r))
            continue;

        n++;
        title   = ai_search_result_get_title (r);
        url     = ai_search_result_get_url (r);
        snippet = ai_search_result_get_snippet (r);
        source  = ai_search_result_get_source (r);
        age     = ai_search_result_get_age (r);
        content = ai_search_result_get_content (r);
        rank    = ai_search_result_get_rank (r);
        if (rank == 0)
            rank = n;

        if (out->len > 0)
            g_string_append_c (out, '\n');

        g_string_append_printf (out, "%u. %s\n", rank,
                                (title != NULL && *title != '\0')
                                ? title : "(untitled)");

        if (url != NULL && *url != '\0')
            g_string_append_printf (out, "   URL: %s\n", url);

        if ((source != NULL && *source != '\0') ||
            (age != NULL && *age != '\0'))
        {
            g_string_append (out, "   ");
            if (source != NULL && *source != '\0')
                g_string_append_printf (out, "Source: %s", source);
            if (age != NULL && *age != '\0')
            {
                if (source != NULL && *source != '\0')
                    g_string_append (out, " \xc2\xb7 ");   /* middle dot */
                g_string_append_printf (out, "Age: %s", age);
            }
            g_string_append_c (out, '\n');
        }

        if (snippet != NULL && *snippet != '\0')
            g_string_append_printf (out, "   %s\n", snippet);

        if (include_content && content != NULL && *content != '\0')
            g_string_append_printf (out, "   Content:\n%s\n", content);
    }

    if (out->len == 0)
    {
        g_string_free (out, TRUE);
        if (query != NULL && *query != '\0')
            return g_strdup_printf ("No results found for: %s", query);
        return g_strdup ("No results found.");
    }

    return g_string_free (out, FALSE);
}

/**
 * ai_search_provider_new_default:
 * @error: (out) (optional): return location for a #GError
 *
 * Creates a search provider chosen from the environment: %AiBraveSearch when
 * BRAVE_API_KEY is set, else %AiBingSearch when BING_API_KEY is set, else the
 * keyless %AiDuckDuckGoSearch (so web search works with zero configuration).
 *
 * Returns: (transfer full) (nullable): a new #AiSearchProvider, or %NULL on
 *   error (currently never, since the DuckDuckGo fallback is keyless).
 */
AiSearchProvider *
ai_search_provider_new_default (GError **error)
{
    const gchar *brave = g_getenv ("BRAVE_API_KEY");
    const gchar *bing  = g_getenv ("BING_API_KEY");

    (void) error;   /* keyless fallback means this never fails today */

    if (brave != NULL && *brave != '\0')
        return AI_SEARCH_PROVIDER (ai_brave_search_new (brave));

    if (bing != NULL && *bing != '\0')
        return AI_SEARCH_PROVIDER (ai_bing_search_new (bing));

    return AI_SEARCH_PROVIDER (ai_duckduckgo_search_new ());
}
