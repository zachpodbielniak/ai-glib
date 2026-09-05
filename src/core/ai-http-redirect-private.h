/*
 * ai-http-redirect-private.h - Keep authenticated requests on their origin
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-http-redirect-private.h is an internal header"
#endif

#include <libsoup/soup.h>

static inline void
ai_http_check_redirect(SoupMessage *message, gpointer user_data)
{
    const gchar *location;
    GUri *current;
    g_autoptr(GUri) next = NULL;

    if (!SOUP_STATUS_IS_REDIRECTION(soup_message_get_status(message)))
        return;

    location = soup_message_headers_get_one(
        soup_message_get_response_headers(message), "Location");
    if (location == NULL)
        return;

    current = soup_message_get_uri(message);
    next = g_uri_parse_relative(current, location, SOUP_HTTP_URI_FLAGS, NULL);

    /* Provider headers such as x-api-key are not HTTP Authorization and
     * can survive libsoup's automatic redirects. Keep every API request
     * on its configured origin, including its scheme and port. */
    if (next == NULL || g_uri_get_host(next) == NULL ||
        g_strcmp0(g_uri_get_scheme(current), g_uri_get_scheme(next)) != 0 ||
        g_ascii_strcasecmp(g_uri_get_host(current), g_uri_get_host(next)) != 0 ||
        g_uri_get_port(current) != g_uri_get_port(next))
        soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
}

