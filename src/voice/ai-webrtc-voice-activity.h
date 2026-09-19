/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-voice-activity.h"
G_BEGIN_DECLS
#define AI_TYPE_WEBRTC_VOICE_ACTIVITY (ai_webrtc_voice_activity_get_type())
G_DECLARE_FINAL_TYPE(AiWebrtcVoiceActivity, ai_webrtc_voice_activity, AI,
					 WEBRTC_VOICE_ACTIVITY, GObject)
AiWebrtcVoiceActivity *
ai_webrtc_voice_activity_new(void);
G_END_DECLS
