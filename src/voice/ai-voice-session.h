/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-audio-transport.h"
#include "voice/ai-speech-recognizer.h"
#include "voice/ai-speech-synthesizer.h"
#include "voice/ai-voice-activity.h"
#include "view/ai-conversation.h"
G_BEGIN_DECLS
/**
 * AiVoiceState:
 * @AI_VOICE_LISTENING: waiting for speech
 * @AI_VOICE_TRANSCRIBING: receiving an utterance
 * @AI_VOICE_THINKING: a conversation turn is in flight
 * @AI_VOICE_SPEAKING: audio is being played
 */
typedef enum {
	AI_VOICE_LISTENING,
	AI_VOICE_TRANSCRIBING,
	AI_VOICE_THINKING,
	AI_VOICE_SPEAKING
} AiVoiceState;
#define AI_TYPE_VOICE_STATE (ai_voice_state_get_type())
GType
ai_voice_state_get_type(void) G_GNUC_CONST;
#define AI_TYPE_VOICE_SESSION (ai_voice_session_get_type())
G_DECLARE_FINAL_TYPE(AiVoiceSession, ai_voice_session, AI, VOICE_SESSION, GObject)
AiVoiceSession *
ai_voice_session_new(AiAudioTransport *transport, AiSpeechRecognizer *recognizer,
					 AiSpeechSynthesizer *synthesizer, AiVoiceActivity *activity,
					 AiConversation *conversation);
AiVoiceState
ai_voice_session_get_state(AiVoiceSession *self);
void
ai_voice_session_say(AiVoiceSession *self, const gchar *text);
void
ai_voice_session_stop(AiVoiceSession *self);
G_END_DECLS
