/*
 * ai-search-http.h - Shared robust JSON GET for search providers (private)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib. It is INTERNAL: it is compiled into the
 * library but not installed and not part of the public umbrella header.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-search-http.h is private to ai-glib and cannot be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * ai_search_http_get_json:
 * @session: the SoupSession to use
 * @url: the GET URL (all query params already encoded)
 * @header_pairs: (nullable) (array zero-terminated=1): NULL-terminated array of
 *   alternating header name/value strings, e.g.
 *   { "X-Subscription-Token", key, NULL }. May be NULL for no extra headers.
 * @cancellable: (nullable): a GCancellable
 * @error: return location for a GError
 *
 * Performs a GET, injecting a search User-Agent and Accept: application/json
 * plus any @header_pairs. Retries transient failures (HTTP 429 honoring
 * Retry-After, 5xx, and network errors) with exponential backoff, and maps
 * HTTP status to AI_ERROR codes (401 -> INVALID_API_KEY, 403 ->
 * PERMISSION_DENIED, 429 -> RATE_LIMITED, 5xx -> SERVER_ERROR, other 4xx ->
 * INVALID_REQUEST, timeout -> TIMEOUT, other network -> NETWORK_ERROR).
 *
 * Returns: (transfer full) (nullable): the parsed JSON root node (free with
 *   json_node_unref()), or NULL with @error set.
 */
JsonNode *
ai_search_http_get_json (
    SoupSession         *session,
    const gchar         *url,
    const gchar *const  *header_pairs,
    GCancellable        *cancellable,
    GError             **error
);

G_END_DECLS
