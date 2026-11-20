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
#include "voice/ai-speaker-identifier.h"
#include "voice/ai-voice-room.h"
#include "voice/ai-address-detector.h"
#include "voice/ai-relevance-judge.h"
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
G_DECLARE_DERIVABLE_TYPE(AiVoiceSession, ai_voice_session, AI, VOICE_SESSION, GObject)
/**
 * AiVoiceSessionClass:
 * @parent_class: the parent class
 * @transcript: class handler of #AiVoiceSession::transcript
 * @reply: class handler of #AiVoiceSession::reply
 * @command: class handler of #AiVoiceSession::command
 * @relevance: class handler of #AiVoiceSession::relevance
 * @speaker_named: class handler of #AiVoiceSession::speaker-named
 * @allow_promotion: class handler of #AiVoiceSession::allow-promotion;
 *   %TRUE by default
 * @guest_promoted: class handler of #AiVoiceSession::guest-promoted
 *
 * A subclass can react to everything the session reports without
 * connecting handlers, and decide in one place whether a guest may be
 * given a lasting name. The pieces a room session is made of -- who spoke
 * (#AiSpeakerIdentifier), whether it was said to the assistant
 * (#AiAddressDetector), whether to chime in (#AiRelevanceJudge), and what
 * is remembered between turns (#AiVoiceRoom) -- are properties, each
 * replaceable on its own.
 */
struct _AiVoiceSessionClass {
	GObjectClass parent_class;
	void (*transcript)(AiVoiceSession *self, const gchar *speaker, const gchar *text,
					   gboolean final);
	void (*reply)(AiVoiceSession *self, const gchar *text);
	void (*command)(AiVoiceSession *self, const gchar *name);
	void (*relevance)(AiVoiceSession *self, guint score, const gchar *reason, gboolean chimed);
	void (*speaker_named)(AiVoiceSession *self, const gchar *participant, AiSpeakerMatch *match);
	gboolean (*allow_promotion)(AiVoiceSession *self, const gchar *guest, const gchar *name);
	void (*guest_promoted)(AiVoiceSession *self, const gchar *guest, const gchar *name);
	gpointer _reserved[12];
};
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
