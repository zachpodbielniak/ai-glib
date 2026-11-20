/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
#include "voice/ai-speaker-match.h"
G_BEGIN_DECLS
#define AI_TYPE_SPEAKER_IDENTIFIER (ai_speaker_identifier_get_type())
G_DECLARE_INTERFACE(AiSpeakerIdentifier, ai_speaker_identifier, AI, SPEAKER_IDENTIFIER,
					GObject)
/**
 * AiSpeakerIdentifierInterface:
 * @parent_iface: the parent interface
 * @identify_async: start naming the speaker of 16 kHz mono S16LE audio
 * @identify_finish: the #AiSpeakerMatch, or %G_IO_ERROR_INVALID_DATA when
 *   the audio is too short to tell
 * @promote: give a voice heard during the call (a guest) a lasting name;
 *   optional, %G_IO_ERROR_NOT_SUPPORTED when unset
 * @has_name: whether a name is already enrolled; optional, %FALSE when unset
 *
 * Who said an utterance, from its audio. #AiVoiceprintIdentifier is one
 * implementation; an application can supply its own (a diarization
 * service, a per-seat microphone map) and hand it to
 * #AiVoiceSession:speaker-identifier.
 */
struct _AiSpeakerIdentifierInterface {
	GTypeInterface parent_iface;
	void (*identify_async)(AiSpeakerIdentifier *self, GBytes *pcm, GCancellable *cancellable,
						   GAsyncReadyCallback callback, gpointer user_data);
	AiSpeakerMatch *(*identify_finish)(AiSpeakerIdentifier *self, GAsyncResult *result,
									   GError **error);
	gboolean (*promote)(AiSpeakerIdentifier *self, const gchar *guest, const gchar *name,
						gboolean merge, GError **error);
	gboolean (*has_name)(AiSpeakerIdentifier *self, const gchar *name);
	gpointer _reserved[8];
};

void
ai_speaker_identifier_identify_async(AiSpeakerIdentifier *self, GBytes *pcm,
									 GCancellable *cancellable, GAsyncReadyCallback callback,
									 gpointer user_data);

AiSpeakerMatch *
ai_speaker_identifier_identify_finish(AiSpeakerIdentifier *self, GAsyncResult *result,
									  GError **error);

gboolean
ai_speaker_identifier_promote(AiSpeakerIdentifier *self, const gchar *guest, const gchar *name,
							  gboolean merge, GError **error);

gboolean
ai_speaker_identifier_has_name(AiSpeakerIdentifier *self, const gchar *name);
G_END_DECLS
