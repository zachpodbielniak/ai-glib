/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-audio-transport.h"
G_BEGIN_DECLS
#define AI_TYPE_LOCAL_AUDIO_TRANSPORT (ai_local_audio_transport_get_type())
G_DECLARE_FINAL_TYPE(AiLocalAudioTransport, ai_local_audio_transport, AI,
					 LOCAL_AUDIO_TRANSPORT, GObject)
AiLocalAudioTransport *
ai_local_audio_transport_new(void);
G_END_DECLS
