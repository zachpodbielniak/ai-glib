/*
 * ai-libresearch-search.h - libresearch metasearch provider
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiLibresearchSearch talks to a running libresearch server (libresearchd)
 * over its REST API.  It never links libresearch: the server may be local
 * (http://127.0.0.1:8087, the libresearch default) or anywhere else the
 * caller can reach.
 *
 * It is an #AiSearchProvider, so it drops into ai_tool_executor_set_search_provider()
 * for the web_search tool like any other backend.  It also covers what
 * only libresearch offers, each as an asynchronous request answering the
 * server's own JSON document:
 *
 *  - web search with categories (/api/v1/search, libresearch.search/1)
 *  - a page's readable text, optionally from the Wayback Machine
 *    (/api/v1/fetch, libresearch.fetch/1)
 *  - an LQL query over search results (/api/v1/query, libresearch.query/1)
 *  - a cited research answer (/api/v1/research, libresearch.research/1)
 *  - the instance's LQL environment (/api/v1/query/environment)
 *
 * The ai_libresearch_format_*() helpers render those documents as compact
 * text for a model, so every host shows the same thing.
 *
 * Usage:
 *   g_autoptr(AiLibresearchSearch) lr =
 *       ai_libresearch_search_new("http://127.0.0.1:8087", NULL);
 *   ai_libresearch_search_query_async(lr,
 *       "search \"gtk4\" | filter site(\"docs.gtk.org\") | fields title, url",
 *       FALSE, NULL, on_query, NULL);
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include "convenience/ai-search-provider.h"

G_BEGIN_DECLS

/**
 * AI_LIBRESEARCH_DEFAULT_ENDPOINT:
 *
 * Where libresearchd listens when nobody configured it otherwise.
 */
#define AI_LIBRESEARCH_DEFAULT_ENDPOINT "http://127.0.0.1:8087"

#define AI_TYPE_LIBRESEARCH_SEARCH (ai_libresearch_search_get_type())

G_DECLARE_FINAL_TYPE(AiLibresearchSearch, ai_libresearch_search, AI, LIBRESEARCH_SEARCH, GObject)

/**
 * ai_libresearch_search_new:
 * @endpoint: (nullable): the server's base URL, such as
 *   "http://127.0.0.1:8087"; %NULL or empty means
 *   %AI_LIBRESEARCH_DEFAULT_ENDPOINT
 * @api_key: (nullable): a bearer token from the server's `api.tokens`, or
 *   %NULL when the server does not require one
 *
 * Creates a client for one libresearch server.
 *
 * Returns: (transfer full): a new #AiLibresearchSearch
 */
AiLibresearchSearch *
ai_libresearch_search_new(
	const gchar *endpoint,
	const gchar *api_key
);

/**
 * ai_libresearch_search_get_endpoint:
 * @self: an #AiLibresearchSearch
 *
 * Returns: (transfer none): the base URL requests are sent to
 */
const gchar *
ai_libresearch_search_get_endpoint(
	AiLibresearchSearch *self
);

/**
 * ai_libresearch_search_web_async:
 * @self: an #AiLibresearchSearch
 * @query: what to search for; bangs such as `!gh` work
 * @options: (nullable): count, freshness, language, safe search and site
 * @category: (nullable): `web`, `images`, `videos`, `news` or `code`;
 *   %NULL for the server's default
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the libresearch.search/1 document
 * @user_data: data for @callback
 *
 * Searches through `GET /api/v1/search`.  Finish with
 * ai_libresearch_search_request_finish().
 *
 * Like every request here, it runs on the thread-default main context of
 * the calling thread: call it from the thread whose loop will dispatch
 * @callback.
 */
void
ai_libresearch_search_web_async(
	AiLibresearchSearch *self,
	const gchar         *query,
	AiSearchOptions     *options,
	const gchar         *category,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * ai_libresearch_search_fetch_async:
 * @self: an #AiLibresearchSearch
 * @url: the page to read
 * @max_chars: the longest text to answer, 100 to 1000000; 0 for the
 *   server's default
 * @archive: (nullable): `never`, `fallback` or `only`, deciding whether the
 *   Wayback Machine may stand in for the page; %NULL means `never`
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the libresearch.fetch/1 document
 * @user_data: data for @callback
 *
 * Reads a page's text through `GET /api/v1/fetch`.  The server applies its
 * own SSRF guard.  Finish with ai_libresearch_search_request_finish().
 */
void
ai_libresearch_search_fetch_async(
	AiLibresearchSearch *self,
	const gchar         *url,
	guint                max_chars,
	const gchar         *archive,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * ai_libresearch_search_query_async:
 * @self: an #AiLibresearchSearch
 * @lql: the LQL query, such as
 *   `search "glib", category: CODE | filter extra.stars > 100`; the pipe
 *   shorthand (`gtk docs | filter site("docs.gtk.org")`) works too
 * @explain: %TRUE to plan the query without running it
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the libresearch.query/1 document
 * @user_data: data for @callback
 *
 * Runs an LQL query through `GET /api/v1/query`.  A query whose source is
 * `research` takes minutes, so it is sent with the long timeout.  An
 * invalid query fails with %AI_ERROR_INVALID_REQUEST and a message
 * carrying the server's position and caret excerpt.  Finish with
 * ai_libresearch_search_request_finish().
 */
void
ai_libresearch_search_query_async(
	AiLibresearchSearch *self,
	const gchar         *lql,
	gboolean             explain,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * ai_libresearch_search_research_async:
 * @self: an #AiLibresearchSearch
 * @question: the question to answer
 * @deep: %TRUE to let the server's model run follow-up searches
 * @pages: pages to read, 1 to 20; 0 for the server's default
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the libresearch.research/1 document
 * @user_data: data for @callback
 *
 * Asks for a cited answer through `GET /api/v1/research`.  A run takes
 * minutes; it uses #AiLibresearchSearch:research-timeout.  The server
 * answers 404 when its research module is not active, which fails here
 * with %AI_ERROR_NOT_SUPPORTED.  Finish with
 * ai_libresearch_search_request_finish().
 */
void
ai_libresearch_search_research_async(
	AiLibresearchSearch *self,
	const gchar         *question,
	gboolean             deep,
	guint                pages,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * ai_libresearch_search_environment_async:
 * @self: an #AiLibresearchSearch
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the lql-environment/1 document
 * @user_data: data for @callback
 *
 * Reads the instance's LQL environment through
 * `GET /api/v1/query/environment`: its commands, sources, functions and
 * constants, modules included.  It is the authoritative list of what a
 * query on this server may name.  Finish with
 * ai_libresearch_search_request_finish().
 */
void
ai_libresearch_search_environment_async(
	AiLibresearchSearch *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * ai_libresearch_search_request_finish:
 * @self: an #AiLibresearchSearch
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes any of the asynchronous requests above; the per-request
 * `_finish()` functions are the same call under the name introspection
 * pairs with each `_async()`.
 *
 * Returns: (transfer full) (nullable): the server's JSON document, or
 *   %NULL with @error set
 */
JsonNode *
ai_libresearch_search_request_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
);

/**
 * ai_libresearch_search_web_finish:
 * @self: an #AiLibresearchSearch
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes ai_libresearch_search_web_async().
 *
 * Returns: (transfer full) (nullable): the libresearch.search/1 document, or %NULL with
 *   @error set
 */
JsonNode *
ai_libresearch_search_web_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
);

/**
 * ai_libresearch_search_fetch_finish:
 * @self: an #AiLibresearchSearch
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes ai_libresearch_search_fetch_async().
 *
 * Returns: (transfer full) (nullable): the libresearch.fetch/1 document, or %NULL with
 *   @error set
 */
JsonNode *
ai_libresearch_search_fetch_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
);

/**
 * ai_libresearch_search_query_finish:
 * @self: an #AiLibresearchSearch
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes ai_libresearch_search_query_async().
 *
 * Returns: (transfer full) (nullable): the libresearch.query/1 document, or %NULL with
 *   @error set
 */
JsonNode *
ai_libresearch_search_query_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
);

/**
 * ai_libresearch_search_research_finish:
 * @self: an #AiLibresearchSearch
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes ai_libresearch_search_research_async().
 *
 * Returns: (transfer full) (nullable): the libresearch.research/1 document, or %NULL with
 *   @error set
 */
JsonNode *
ai_libresearch_search_research_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
);

/**
 * ai_libresearch_search_environment_finish:
 * @self: an #AiLibresearchSearch
 * @result: the #GAsyncResult passed to the callback
 * @error: return location for a #GError
 *
 * Finishes ai_libresearch_search_environment_async().
 *
 * Returns: (transfer full) (nullable): the lql-environment/1 document, or %NULL with
 *   @error set
 */
JsonNode *
ai_libresearch_search_environment_finish(
	AiLibresearchSearch  *self,
	GAsyncResult         *result,
	GError              **error
);

/**
 * ai_libresearch_results_from_document:
 * @document: a libresearch.search/1 document
 *
 * Converts a search document's results into #AiSearchResult objects, the
 * shape every other provider answers.
 *
 * Returns: (transfer full) (element-type AiSearchResult): the results;
 *   free with g_list_free_full(list, g_object_unref)
 */
GList *
ai_libresearch_results_from_document(
	JsonNode *document
);

/**
 * ai_libresearch_format_search:
 * @document: a libresearch.search/1 document
 * @max_results: the most results to show; 0 for all
 *
 * Renders a search document as text for a model: instant answers first,
 * then numbered results with their URL, engines and date, then any
 * engines that failed, so a short list is not mistaken for a complete one.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_libresearch_format_search(
	JsonNode *document,
	guint     max_results
);

/**
 * ai_libresearch_format_fetch:
 * @document: a libresearch.fetch/1 document
 *
 * Renders a fetched page: its title, URL, the archived copy when one stood
 * in for it, then the text, marked when the server truncated it.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_libresearch_format_fetch(
	JsonNode *document
);

/**
 * ai_libresearch_format_query:
 * @document: a libresearch.query/1 document
 * @max_rows: the most rows to show; 0 for all
 *
 * Renders an LQL result: the plan when the query was explained, otherwise
 * one block per row with every column by name, and a count of the rows
 * left out.  A research query's answer row is shown as prose.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_libresearch_format_query(
	JsonNode *document,
	guint     max_rows
);

/**
 * ai_libresearch_format_research:
 * @document: a libresearch.research/1 document
 *
 * Renders a research answer: the cited answer, then the numbered sources it
 * cites by `[n]`, marking the ones that were not read or not cited.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_libresearch_format_research(
	JsonNode *document
);

G_END_DECLS
