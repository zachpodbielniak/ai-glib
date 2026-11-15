/*
 * ai-client-private.h - AiClient internals shared with the providers
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-client-private.h is an internal header"
#endif

#include "core/ai-client.h"
#include "core/ai-http-redirect-private.h"

G_BEGIN_DECLS

/*
 * What a request from @self carries, for ai_http_message_new(): the
 * provider's add_auth_headers() is run on a scratch message and
 * AI_HTTP_CARRIES_CREDENTIALS returned when what it attached is a
 * credential (an API key is configured), AI_HTTP_NO_CREDENTIALS when it
 * attached none (Ollama, a keyless local server).
 *
 * Asked before the message is built, so a credentialed provider request
 * has SOUP_MESSAGE_NO_REDIRECT from the start and never depends on its
 * session's guard noticing the header later.  A keyless one keeps the
 * documented same-origin behaviour.
 */
AiHttpCredentials ai_client_http_credentials(AiClient *self);

G_END_DECLS
