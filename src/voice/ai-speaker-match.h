/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-voiceprint.h"
G_BEGIN_DECLS
/**
 * AiSpeakerMatchKind:
 * @AI_SPEAKER_MATCH_ENROLLED: an enrolled voice matched
 * @AI_SPEAKER_MATCH_GUEST: a voice nobody enrolled, numbered for the call
 * @AI_SPEAKER_MATCH_UNKNOWN: a voice that matches nobody, not tracked
 * @AI_SPEAKER_MATCH_INHERITED: too short to tell; whoever that microphone
 *   was last named
 * @AI_SPEAKER_MATCH_FALLBACK: naming failed or timed out; the participant
 *
 * How a speaker came to be named.
 */
typedef enum {
	AI_SPEAKER_MATCH_ENROLLED,
	AI_SPEAKER_MATCH_GUEST,
	AI_SPEAKER_MATCH_UNKNOWN,
	AI_SPEAKER_MATCH_INHERITED,
	AI_SPEAKER_MATCH_FALLBACK
} AiSpeakerMatchKind;
#define AI_TYPE_SPEAKER_MATCH_KIND (ai_speaker_match_kind_get_type())
GType
ai_speaker_match_kind_get_type(void) G_GNUC_CONST;

#define AI_TYPE_SPEAKER_MATCH (ai_speaker_match_get_type())
G_DECLARE_DERIVABLE_TYPE(AiSpeakerMatch, ai_speaker_match, AI, SPEAKER_MATCH, GObject)
/**
 * AiSpeakerMatchClass:
 * @parent_class: the parent class
 */
struct _AiSpeakerMatchClass {
	GObjectClass parent_class;
	gpointer _reserved[8];
};
AiSpeakerMatch *
ai_speaker_match_new(AiSpeakerMatchKind kind, const gchar *name, gdouble score);
AiSpeakerMatchKind
ai_speaker_match_get_kind(AiSpeakerMatch *self);
const gchar *
ai_speaker_match_get_name(AiSpeakerMatch *self);
gdouble
ai_speaker_match_get_score(AiSpeakerMatch *self);
guint
ai_speaker_match_get_guest_number(AiSpeakerMatch *self);
AiVoiceprint *
ai_speaker_match_get_voiceprint(AiSpeakerMatch *self);
void
ai_speaker_match_set_voiceprint(AiSpeakerMatch *self, AiVoiceprint *voiceprint);
G_END_DECLS
