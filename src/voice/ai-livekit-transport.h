/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-audio-transport.h"
G_BEGIN_DECLS
#define AI_TYPE_LIVEKIT_TRANSPORT (ai_livekit_transport_get_type())
G_DECLARE_FINAL_TYPE(AiLivekitTransport, ai_livekit_transport, AI, LIVEKIT_TRANSPORT,
					 GObject)
AiLivekitTransport *
ai_livekit_transport_new(const gchar *url, const gchar *receive_token);
gboolean
ai_livekit_transport_is_available(void);
G_END_DECLS
