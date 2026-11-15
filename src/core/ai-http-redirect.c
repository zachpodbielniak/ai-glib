/*
 * ai-http-redirect.c - Never follow a redirect with a credential
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <string.h>

#include "core/ai-error.h"
#include "core/ai-http-redirect-private.h"

/* Message data: why the guard refused the redirect, for the error. */
#define REFUSED_KEY "ai-http-redirect-refused"

/* Message data: the caller says the request carries a credential. */
#define CREDENTIALS_KEY "ai-http-redirect-credentials"

/* Message data: the caller says the URL came from outside (AI_HTTP_PUBLIC_URL). */
#define PUBLIC_URL_KEY "ai-http-redirect-public-url"

/*
 * Words that mark a request header as holding a credential, matched
 * anywhere in the lower-cased name.  "auth" covers Authorization,
 * Proxy-Authorization and X-Auth-Token; the rest are how API keys are
 * spelled by the providers and search engines ai-glib talks to.
 */
static const gchar *const credential_words[] = {
    "auth",
    "cookie",
    "token",
    "secret",
    "password",
    "api-key",
    "apikey",
    "api_key",
    "subscription-key",
    "access-key",
    NULL
};

static gboolean
header_is_credential(const gchar *name)
{
    g_autofree gchar *lower = g_ascii_strdown(name, -1);
    guint i;

    for (i = 0; credential_words[i] != NULL; i++)
        if (strstr(lower, credential_words[i]) != NULL)
            return TRUE;

    return FALSE;
}

gboolean
ai_http_request_has_credentials(SoupMessageHeaders *headers,
                                GUri               *uri)
{
    SoupMessageHeadersIter iter;
    const gchar *name;
    const gchar *value;

    /* user:password@host is sent as Basic auth by libsoup. */
    if (uri != NULL && g_uri_get_password(uri) != NULL)
        return TRUE;

    if (headers == NULL)
        return FALSE;

    soup_message_headers_iter_init(&iter, headers);

    while (soup_message_headers_iter_next(&iter, &name, &value))
        if (header_is_credential(name))
            return TRUE;

    return FALSE;
}

void
ai_http_message_mark_credentials(SoupMessage *message)
{
    g_return_if_fail(SOUP_IS_MESSAGE(message));

    g_object_set_data(G_OBJECT(message), CREDENTIALS_KEY, GINT_TO_POINTER(1));
}

/* The port a URI really uses, so "https://h/" and "https://h:443/" agree. */
static gint
effective_port(GUri *uri)
{
    gint port = g_uri_get_port(uri);

    if (port > 0)
        return port;

    if (g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0 ||
        g_ascii_strcasecmp(g_uri_get_scheme(uri), "wss") == 0)
        return 443;

    return 80;
}

gboolean
ai_http_redirect_allowed(GUri        *current,
                         const gchar *location,
                         gboolean     has_credentials)
{
    g_autoptr(GUri) next = NULL;

    /*
     * Credentials first and unconditionally.  libsoup keeps every custom
     * header across a redirect and resends a 307/308's body, so even a
     * same-origin hop replays the key and the prompt to a path nobody
     * configured.  The caller gets a clear error instead and can fix its
     * URL.
     */
    if (has_credentials)
        return FALSE;

    if (current == NULL || location == NULL)
        return FALSE;

    next = g_uri_parse_relative(current, location, SOUP_HTTP_URI_FLAGS, NULL);

    if (next == NULL || g_uri_get_host(next) == NULL ||
        g_uri_get_scheme(current) == NULL || g_uri_get_host(current) == NULL)
        return FALSE;

    return g_ascii_strcasecmp(g_uri_get_scheme(current), g_uri_get_scheme(next)) == 0 &&
           g_ascii_strcasecmp(g_uri_get_host(current), g_uri_get_host(next)) == 0 &&
           effective_port(current) == effective_port(next);
}

void
ai_http_check_redirect(SoupMessage *message,
                       gpointer     user_data)
{
    const gchar *location;
    gboolean credentials;

    (void)user_data;

    if (!SOUP_STATUS_IS_REDIRECTION(soup_message_get_status(message)))
        return;

    location = soup_message_headers_get_one(
        soup_message_get_response_headers(message), "Location");

    /* No Location: libsoup has nothing to follow either. */
    if (location == NULL)
        return;

    credentials = g_object_get_data(G_OBJECT(message), CREDENTIALS_KEY) != NULL ||
                  ai_http_request_has_credentials(
                      soup_message_get_request_headers(message),
                      soup_message_get_uri(message));

    /*
     * A public URL without a credential goes wherever libsoup would take
     * it; anything else is held to ai_http_redirect_allowed().  The
     * credential test above runs on every hop, so a public request that
     * picked up an Authorization header is refused like any other.
     */
    if (!credentials &&
        g_object_get_data(G_OBJECT(message), PUBLIC_URL_KEY) != NULL)
        return;

    if (ai_http_redirect_allowed(soup_message_get_uri(message), location,
                                 credentials))
        return;

    soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    g_object_set_data_full(G_OBJECT(message), REFUSED_KEY,
        g_strdup_printf("redirect to %s was not followed: %s", location,
                        credentials
                        ? "a request that carries credentials is never redirected"
                        : "it leaves the request's origin"),
        g_free);
}

void
ai_http_guard_message(SoupMessage *message)
{
    g_return_if_fail(SOUP_IS_MESSAGE(message));

    /*
     * Decided before the request is sent, not when the 3xx arrives: the
     * headers that carry the credential are already set, and a flag set
     * up front cannot lose a race with libsoup's own redirect handling.
     * The "got-headers" check still runs, to record why a redirect was
     * not followed and to keep a keyless request on its origin.
     */
    if (g_object_get_data(G_OBJECT(message), CREDENTIALS_KEY) != NULL ||
        ai_http_request_has_credentials(soup_message_get_request_headers(message),
                                        soup_message_get_uri(message)))
    {
        ai_http_message_mark_credentials(message);
        soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    }

    if (g_signal_handler_find(message, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                              (gpointer)ai_http_check_redirect, NULL) == 0)
        g_signal_connect(message, "got-headers",
                         G_CALLBACK(ai_http_check_redirect), NULL);
}

/*
 * Applies @credentials to a freshly built message: the up-front flag for
 * a credentialed one, the marker for a public URL, then the guard.  The
 * headers are still empty here, which is why the caller has to say what
 * it will carry rather than leave it to ai_http_request_has_credentials().
 */
static SoupMessage *
apply_credentials(SoupMessage       *message,
                  AiHttpCredentials  credentials)
{
    if (message == NULL)
        return NULL;

    switch (credentials)
    {
    case AI_HTTP_NO_CREDENTIALS:
        break;
    case AI_HTTP_PUBLIC_URL:
        g_object_set_data(G_OBJECT(message), PUBLIC_URL_KEY, GINT_TO_POINTER(1));
        break;
    case AI_HTTP_CARRIES_CREDENTIALS:
    default:
        /* An unknown value is treated as the safe one. */
        ai_http_message_mark_credentials(message);
        break;
    }

    ai_http_guard_message(message);
    return message;
}

SoupMessage *
ai_http_message_new(const gchar       *method,
                    const gchar       *uri_string,
                    AiHttpCredentials  credentials)
{
    g_return_val_if_fail(method != NULL, NULL);
    g_return_val_if_fail(uri_string != NULL, NULL);

    /* The one place in ai-glib a bare constructor is allowed. */
    return apply_credentials(soup_message_new(method, uri_string), credentials);
}

SoupMessage *
ai_http_message_new_from_uri(const gchar       *method,
                             GUri              *uri,
                             AiHttpCredentials  credentials)
{
    g_return_val_if_fail(method != NULL, NULL);
    g_return_val_if_fail(uri != NULL, NULL);

    return apply_credentials(soup_message_new_from_uri(method, uri), credentials);
}

SoupSession *
ai_http_session_new(void)
{
    SoupSession *session = soup_session_new();

    ai_http_guard_session(session);
    return session;
}

static void
guard_queued(SoupSession *session,
             SoupMessage *message,
             gpointer     user_data)
{
    (void)session;
    (void)user_data;

    ai_http_guard_message(message);
}

static void
guard_unqueued(SoupSession *session,
               SoupMessage *message,
               gpointer     user_data)
{
    (void)session;
    (void)user_data;

    g_signal_handlers_disconnect_by_func(message, ai_http_check_redirect, NULL);
}

void
ai_http_guard_session(SoupSession *session)
{
    g_return_if_fail(SOUP_IS_SESSION(session));

    g_signal_connect(session, "request-queued", G_CALLBACK(guard_queued), NULL);
    g_signal_connect(session, "request-unqueued", G_CALLBACK(guard_unqueued), NULL);
}

gboolean
ai_http_redirect_refused(SoupMessage  *message,
                         GError      **error)
{
    const gchar *why;

    if (message == NULL ||
        !SOUP_STATUS_IS_REDIRECTION(soup_message_get_status(message)))
        return FALSE;

    why = g_object_get_data(G_OBJECT(message), REFUSED_KEY);

    if (why == NULL)
        return FALSE;

    g_set_error(error, AI_ERROR, AI_ERROR_NETWORK_ERROR,
                "Redirect refused (HTTP %u): the %s",
                soup_message_get_status(message), why);
    return TRUE;
}
