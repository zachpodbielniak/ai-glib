/*
 * ai-http-redirect-private.h - Never follow a redirect with a credential
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-http-redirect-private.h is an internal header"
#endif

#include <libsoup/soup.h>

G_BEGIN_DECLS

/*
 * AiHttpCredentials:
 * @AI_HTTP_CARRIES_CREDENTIALS: the request carries a credential -- an
 *   Authorization or API-key header, a token in its body -- or is a
 *   provider request whose body is the prompt; it never follows a
 *   redirect, not even to its own origin
 * @AI_HTTP_NO_CREDENTIALS: the request carries nothing secret and goes
 *   to an endpoint somebody configured; it follows a redirect only on
 *   the same scheme, host and port
 * @AI_HTTP_PUBLIC_URL: the request carries nothing secret and fetches a
 *   URL that came from outside -- a web page, a signed download link a
 *   provider answered with -- where a hop to another host (http to
 *   https, a CDN, an object store) is normal; it follows a redirect
 *   wherever libsoup would, unless a credential header is on it when it
 *   is sent, which turns it back into the first case
 *
 * What a request carries, which decides where it may be redirected.
 * Zero is the safe value on purpose, so a zeroed field or a forgotten
 * argument refuses rather than follows.
 *
 * Private: not in a public header, not in the GIR.
 */
typedef enum {
    AI_HTTP_CARRIES_CREDENTIALS = 0,
    AI_HTTP_NO_CREDENTIALS,
    AI_HTTP_PUBLIC_URL
} AiHttpCredentials;

/*
 * The only way ai-glib builds a SoupMessage.  Creates it as
 * soup_message_new() does, then applies @credentials and
 * ai_http_guard_message() before returning it, so no caller can add a
 * credential first and decide later, or forget to decide.
 * tests/check-http-constructors.sh fails `make test` on a bare
 * soup_message_new*() anywhere else in src/, bin/ or gui/.
 *
 * Returns NULL when @uri_string does not parse, as soup_message_new()
 * does.
 */
SoupMessage *ai_http_message_new(const gchar       *method,
                                 const gchar       *uri_string,
                                 AiHttpCredentials  credentials);

/* ai_http_message_new() for a URI that is already parsed. */
SoupMessage *ai_http_message_new_from_uri(const gchar       *method,
                                          GUri              *uri,
                                          AiHttpCredentials  credentials);

/*
 * The only way ai-glib builds a SoupSession: soup_session_new() with
 * ai_http_guard_session() applied, so a message that reaches it from
 * anywhere -- a caller's own, one built before this rule -- is still
 * judged by its headers when it is queued.  Set options such as
 * "timeout" with g_object_set() afterwards.
 */
SoupSession *ai_http_session_new(void);

/*
 * Whether a request carries a credential: Authorization,
 * Proxy-Authorization or Cookie, any header whose name says it holds a
 * key, token or secret (x-api-key, x-goog-api-key, X-Subscription-Token,
 * Ocp-Apim-Subscription-Key, ...), or a password in the URI's userinfo.
 *
 * Deliberately broad.  A header that is wrongly counted as a credential
 * costs one redirect not followed; a credential that is missed is sent
 * to wherever the server pointed it.
 */
gboolean ai_http_request_has_credentials(SoupMessageHeaders *headers,
                                         GUri               *uri);

/*
 * Marks @message as carrying a credential the headers do not show -- an
 * OpenID token in a JSON body, say -- so ai_http_check_redirect() treats
 * it as it treats an Authorization header and never follows a redirect.
 */
void ai_http_message_mark_credentials(SoupMessage *message);

/*
 * The redirect decision, separate from libsoup so it can be tested
 * without a network.
 *
 * A request carrying credentials is never redirected, not even to its
 * own origin: libsoup resends the body of a 307 or 308 and keeps every
 * custom header, so following one replays the credential -- and the
 * prompt -- to a path the caller never chose.  Without credentials a
 * redirect is followed only when it stays on the same scheme, host and
 * port.  A Location that does not parse is refused.
 */
gboolean ai_http_redirect_allowed(GUri        *current,
                                  const gchar *location,
                                  gboolean     has_credentials);

/*
 * The "got-headers" handler that applies ai_http_redirect_allowed() to a
 * message, by setting SOUP_MESSAGE_NO_REDIRECT when it says no.  The
 * message then completes with its 3xx status, and
 * ai_http_redirect_refused() turns that into an error naming why.
 */
void ai_http_check_redirect(SoupMessage *message,
                            gpointer     user_data);

/*
 * Guards one message before it is sent: a credentialed one gets
 * SOUP_MESSAGE_NO_REDIRECT at once, and every one gets
 * ai_http_check_redirect() on "got-headers".  Call it after the request
 * headers are set.
 */
void ai_http_guard_message(SoupMessage *message);

/*
 * Applies ai_http_guard_message() to every message @session sends, by
 * attaching it as each is queued.  For a session that is shared by
 * requests with and without credentials, or whose requests are built in
 * many places; one call where the session is created covers them all.
 */
void ai_http_guard_session(SoupSession *session);

/*
 * If @message finished on a redirect the guard refused, sets @error
 * (AI_ERROR_NETWORK_ERROR) saying where it pointed and why it was not
 * followed, and returns TRUE.
 */
gboolean ai_http_redirect_refused(SoupMessage  *message,
                                  GError      **error);

G_END_DECLS
