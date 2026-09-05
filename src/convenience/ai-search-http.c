/*
 * ai-search-http.c - Shared robust JSON GET for search providers (private)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "convenience/ai-search-http.h"
#include "core/ai-error.h"
#include "core/ai-http-redirect-private.h"

/* User-Agent sent on every search-provider request. */
#define AI_SEARCH_HTTP_USER_AGENT \
    "cmacs-ai/1.0 (ai-glib web_search; +https://github.com/zachp/cmacs)"

/* Total attempts (1 initial + up to 2 retries). */
#define AI_SEARCH_HTTP_MAX_ATTEMPTS 3
/* Base backoff, doubled each retry. Kept small so the worst case stays well
 * under a second; a server-supplied Retry-After overrides it for 429s. */
#define AI_SEARCH_HTTP_BASE_BACKOFF_US (G_USEC_PER_SEC / 10)   /* 100 ms */
/* Never honor a Retry-After longer than this (defensive against hostile or
 * misconfigured servers stalling the caller). */
#define AI_SEARCH_HTTP_MAX_BACKOFF_US (5 * G_USEC_PER_SEC)

/* Sleep for a retry, honoring a numeric Retry-After (seconds) when present,
 * otherwise exponential backoff for this attempt. Returns FALSE if the wait
 * was interrupted by cancellation. */
static gboolean
backoff_wait (SoupMessage *msg, guint attempt, GCancellable *cancellable)
{
    gint64       delay_us;
    const gchar *retry_after = NULL;

    if (msg != NULL)
        retry_after = soup_message_headers_get_one (
            soup_message_get_response_headers (msg), "Retry-After");

    if (retry_after != NULL)
    {
        gchar  *endptr = NULL;
        gint64  secs   = g_ascii_strtoll (retry_after, &endptr, 10);

        if (endptr != retry_after && secs >= 0)
        {
            delay_us = secs * G_USEC_PER_SEC;
        }
        else
        {
            delay_us = (gint64) AI_SEARCH_HTTP_BASE_BACKOFF_US << attempt;
        }
    }
    else
    {
        delay_us = (gint64) AI_SEARCH_HTTP_BASE_BACKOFF_US << attempt;
    }

    if (delay_us > AI_SEARCH_HTTP_MAX_BACKOFF_US)
        delay_us = AI_SEARCH_HTTP_MAX_BACKOFF_US;

    if (delay_us <= 0)
        return !g_cancellable_is_cancelled (cancellable);

    /* Wait in short slices so cancellation is responsive. */
    while (delay_us > 0)
    {
        gint64 slice = delay_us < (gint64) (G_USEC_PER_SEC / 20)
                       ? delay_us : (gint64) (G_USEC_PER_SEC / 20);

        if (g_cancellable_is_cancelled (cancellable))
            return FALSE;
        g_usleep (slice);
        delay_us -= slice;
    }

    return !g_cancellable_is_cancelled (cancellable);
}

JsonNode *
ai_search_http_get_json (
    SoupSession         *session,
    const gchar         *url,
    const gchar *const  *header_pairs,
    GCancellable        *cancellable,
    GError             **error
){
    guint   attempt;
    guint   last_status = 0;

    g_return_val_if_fail (SOUP_IS_SESSION (session), NULL);
    g_return_val_if_fail (url != NULL, NULL);

    for (attempt = 0; attempt < AI_SEARCH_HTTP_MAX_ATTEMPTS; attempt++)
    {
        g_autoptr(SoupMessage) msg       = NULL;
        g_autoptr(GBytes)      bytes     = NULL;
        g_autoptr(GError)      local_err = NULL;
        SoupMessageHeaders    *req_headers;
        guint                  status;

        if (g_cancellable_set_error_if_cancelled (cancellable, error))
            return NULL;

        msg = soup_message_new ("GET", url);
        if (msg == NULL)
        {
            g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                         "search: failed to create request for URL: %s", url);
            return NULL;
        }

        req_headers = soup_message_get_request_headers (msg);
        g_signal_connect (msg, "got-headers", G_CALLBACK (ai_http_check_redirect), NULL);
        soup_message_headers_replace (req_headers, "User-Agent",
                                      AI_SEARCH_HTTP_USER_AGENT);
        soup_message_headers_replace (req_headers, "Accept",
                                      "application/json");
        if (header_pairs != NULL)
        {
            guint i;

            for (i = 0; header_pairs[i] != NULL && header_pairs[i + 1] != NULL;
                 i += 2)
            {
                soup_message_headers_replace (req_headers, header_pairs[i],
                                              header_pairs[i + 1]);
            }
        }

        bytes = soup_session_send_and_read (session, msg, cancellable,
                                            &local_err);

        if (bytes == NULL)
        {
            /* Cancellation is terminal. */
            if (g_error_matches (local_err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            {
                g_propagate_error (error, g_steal_pointer (&local_err));
                return NULL;
            }

            /* A timeout is terminal: the attempt already waited the full
             * timeout, and retrying would multiply that latency (e.g. 3x30s)
             * for little gain. */
            if (g_error_matches (local_err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
            {
                g_set_error (error, AI_ERROR, AI_ERROR_TIMEOUT,
                             "search: request timed out: %s",
                             local_err->message);
                return NULL;
            }

            /* Other (fast) network errors: retry unless this was the last
             * attempt. */
            if (attempt + 1 < AI_SEARCH_HTTP_MAX_ATTEMPTS)
            {
                if (!backoff_wait (NULL, attempt, cancellable))
                {
                    g_cancellable_set_error_if_cancelled (cancellable, error);
                    return NULL;
                }
                continue;
            }

            g_set_error (error, AI_ERROR, AI_ERROR_NETWORK_ERROR,
                         "search: network error: %s",
                         local_err != NULL ? local_err->message : "unknown");
            return NULL;
        }

        status      = soup_message_get_status (msg);
        last_status = status;

        if (status >= 200 && status < 300)
        {
            g_autoptr(JsonParser) parser = json_parser_new ();
            const gchar          *data;
            gsize                 size;
            JsonNode             *root;

            data = g_bytes_get_data (bytes, &size);

            if (!json_parser_load_from_data (parser, data, (gssize) size,
                                             &local_err))
            {
                g_set_error (error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                             "search: invalid JSON response: %s",
                             local_err != NULL ? local_err->message
                                                : "parse error");
                return NULL;
            }

            root = json_parser_get_root (parser);
            if (root == NULL)
            {
                g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                     "search: empty JSON response");
                return NULL;
            }

            return json_node_copy (root);
        }

        if (status == 429)   /* Too Many Requests */
        {
            if (attempt + 1 < AI_SEARCH_HTTP_MAX_ATTEMPTS)
            {
                if (!backoff_wait (msg, attempt, cancellable))
                {
                    g_cancellable_set_error_if_cancelled (cancellable, error);
                    return NULL;
                }
                continue;
            }
            g_set_error (error, AI_ERROR, AI_ERROR_RATE_LIMITED,
                         "search: rate limited (HTTP 429)");
            return NULL;
        }

        if (status >= 500)
        {
            if (attempt + 1 < AI_SEARCH_HTTP_MAX_ATTEMPTS)
            {
                if (!backoff_wait (msg, attempt, cancellable))
                {
                    g_cancellable_set_error_if_cancelled (cancellable, error);
                    return NULL;
                }
                continue;
            }
            g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                         "search: server error (HTTP %u)", status);
            return NULL;
        }

        if (status == SOUP_STATUS_UNAUTHORIZED)         /* 401 */
        {
            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
                         "search: invalid or missing API key (HTTP 401)");
            return NULL;
        }

        if (status == SOUP_STATUS_FORBIDDEN)            /* 403 */
        {
            g_set_error (error, AI_ERROR, AI_ERROR_PERMISSION_DENIED,
                         "search: permission denied (HTTP 403)");
            return NULL;
        }

        /* Any other 4xx: a definite client error, no retry. */
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "search: HTTP %u", status);
        return NULL;
    }

    /* Exhausted attempts without categorizing (shouldn't normally happen). */
    g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                 "search: failed after %d attempts (last HTTP %u)",
                 AI_SEARCH_HTTP_MAX_ATTEMPTS, last_status);
    return NULL;
}
