/*
 * test-search-providers.c - Unit tests for AiBingSearch and AiBraveSearch
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Live search tests are skipped when API keys are absent.
 * Set BING_API_KEY or BRAVE_API_KEY environment variables to run them.
 */

#include <glib.h>

#include "ai-glib.h"
#include "convenience/ai-search-provider.h"
#include "convenience/ai-bing-search.h"
#include "convenience/ai-brave-search.h"

/* ================================================================
 * AiBingSearch
 * ================================================================ */

static void
test_bing_search_new (void)
{
    g_autoptr(AiBingSearch) bing = NULL;

    bing = ai_bing_search_new ("dummy-key");
    g_assert_nonnull (bing);
    g_assert_true (AI_IS_BING_SEARCH (bing));
    g_assert_true (AI_IS_SEARCH_PROVIDER (bing));
}

static void
test_bing_search_live (void)
{
    const gchar             *key = g_getenv ("BING_API_KEY");
    g_autoptr(AiBingSearch)  bing   = NULL;
    g_autofree gchar        *result = NULL;
    g_autoptr(GError)        err    = NULL;

    if (key == NULL || *key == '\0')
    {
        g_test_skip ("BING_API_KEY not set");
        return;
    }

    bing   = ai_bing_search_new (key);
    result = ai_search_provider_search (AI_SEARCH_PROVIDER (bing),
                                        "ai-glib library",
                                        NULL, &err);
    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_true (strlen (result) > 0);
}

/* ================================================================
 * AiBraveSearch
 * ================================================================ */

static void
test_brave_search_new (void)
{
    g_autoptr(AiBraveSearch) brave = NULL;

    brave = ai_brave_search_new ("dummy-key");
    g_assert_nonnull (brave);
    g_assert_true (AI_IS_BRAVE_SEARCH (brave));
    g_assert_true (AI_IS_SEARCH_PROVIDER (brave));
}

static void
test_brave_search_live (void)
{
    const gchar              *key  = g_getenv ("BRAVE_API_KEY");
    g_autoptr(AiBraveSearch)  brave  = NULL;
    g_autofree gchar         *result = NULL;
    g_autoptr(GError)         err    = NULL;

    if (key == NULL || *key == '\0')
    {
        g_test_skip ("BRAVE_API_KEY not set");
        return;
    }

    brave  = ai_brave_search_new (key);
    result = ai_search_provider_search (AI_SEARCH_PROVIDER (brave),
                                        "ai-glib library",
                                        NULL, &err);
    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_true (strlen (result) > 0);
}

/* ================================================================
 * AiToolExecutor + search provider integration
 * ================================================================ */

static void
test_executor_with_search_provider (void)
{
    g_autoptr(AiToolExecutor) exec  = NULL;
    g_autoptr(AiBingSearch)   bing  = NULL;
    GList                    *tools;
    GList                    *iter;
    gboolean                  has_web_search = FALSE;

    exec = ai_tool_executor_new ();
    bing = ai_bing_search_new ("dummy-key");

    ai_tool_executor_set_search_provider (exec, AI_SEARCH_PROVIDER (bing));

    tools = ai_tool_executor_get_tools (exec);
    for (iter = tools; iter != NULL; iter = iter->next)
    {
        if (g_strcmp0 (ai_tool_get_name (iter->data), "web_search") == 0)
        {
            has_web_search = TRUE;
            break;
        }
    }

    g_assert_true (has_web_search);
}

/* ================================================================
 * main
 * ================================================================ */

int
main (
    int   argc,
    char *argv[]
){
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/ai-glib/search-providers/bing/new",
                     test_bing_search_new);
    g_test_add_func ("/ai-glib/search-providers/bing/live",
                     test_bing_search_live);
    g_test_add_func ("/ai-glib/search-providers/brave/new",
                     test_brave_search_new);
    g_test_add_func ("/ai-glib/search-providers/brave/live",
                     test_brave_search_live);
    g_test_add_func ("/ai-glib/search-providers/executor-integration",
                     test_executor_with_search_provider);

    return g_test_run ();
}
