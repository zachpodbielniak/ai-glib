/*
 * web-search.c - Web search example
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Demonstrates the ai-glib web search subsystem:
 *   1. the zero-config env factory (Brave/Bing key, else keyless DuckDuckGo)
 *   2. a direct provider search with options
 *   3. rendering results for a model
 *
 * Usage:
 *   ./web-search "your query"
 *   # optionally: export BRAVE_API_KEY=... (or BING_API_KEY=...)
 */

#include <stdio.h>

#include "ai-glib.h"

int
main (int argc, char *argv[])
{
    const gchar                *query   = (argc > 1) ? argv[1] : "gnu emacs";
    g_autoptr(AiSearchProvider)  sp      = NULL;
    g_autoptr(AiSearchOptions)   options = NULL;
    g_autoptr(GError)            error   = NULL;
    g_autofree gchar            *rendered = NULL;
    GList                       *results;
    GList                       *l;

    /* Pick a provider from the environment (keyless DuckDuckGo fallback). */
    sp = ai_search_provider_new_default (&error);
    if (sp == NULL)
    {
        g_printerr ("Failed to create a search provider: %s\n",
                    error != NULL ? error->message : "unknown");
        return 1;
    }
    g_print ("Provider: %s\n\n", G_OBJECT_TYPE_NAME (sp));

    options = ai_search_options_new ();
    ai_search_options_set_count (options, 5);
    ai_search_options_set_freshness (options, AI_SEARCH_FRESHNESS_MONTH);

    results = ai_search_provider_search (sp, query, options, NULL, &error);
    if (error != NULL)
    {
        g_printerr ("Search failed: %s\n", error->message);
        return 1;
    }

    for (l = results; l != NULL; l = l->next)
    {
        AiSearchResult *r = l->data;

        g_print ("%u. %s\n   %s\n   (%s)\n",
                 ai_search_result_get_rank (r),
                 ai_search_result_get_title (r),
                 ai_search_result_get_url (r),
                 ai_search_result_get_source (r));
    }

    /* The same rendering the web_search tool hands back to the model. */
    rendered = ai_search_results_format (results, query, FALSE);
    g_print ("\n--- model-facing rendering ---\n%s\n", rendered);

    g_list_free_full (results, g_object_unref);
    return 0;
}
