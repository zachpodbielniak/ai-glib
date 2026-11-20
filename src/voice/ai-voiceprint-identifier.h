/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-speaker-identifier.h"
#include "voice/ai-speaker-embedder.h"
#include "voice/ai-voiceprint-store.h"
G_BEGIN_DECLS
#define AI_TYPE_VOICEPRINT_IDENTIFIER (ai_voiceprint_identifier_get_type())
G_DECLARE_DERIVABLE_TYPE(AiVoiceprintIdentifier, ai_voiceprint_identifier, AI,
						 VOICEPRINT_IDENTIFIER, GObject)
/**
 * AiVoiceprintIdentifierClass:
 * @parent_class: the parent class
 * @score: how alike an enrolled or guest voiceprint and an utterance's are;
 *   ai_voiceprint_similarity() by default
 * @match: decide who an utterance's voiceprint is, given @audio_bytes of
 *   audio behind it. The default names the closest enrolled voice at
 *   #AiVoiceprintIdentifier:threshold, else a guest (with
 *   #AiVoiceprintIdentifier:track-guests), else an unknown voice. Called in
 *   the order utterances were heard. Return %NULL with
 *   %G_IO_ERROR_INVALID_DATA for "too short to tell".
 * @promoted: class handler of #AiVoiceprintIdentifier::promoted
 *
 * Subclass to change how voices are compared or how a stranger is
 * treated, keeping the embedding, ordering and storage.
 */
struct _AiVoiceprintIdentifierClass {
	GObjectClass parent_class;
	gdouble (*score)(AiVoiceprintIdentifier *self, AiVoiceprint *known, AiVoiceprint *heard);
	AiSpeakerMatch *(*match)(AiVoiceprintIdentifier *self, AiVoiceprint *heard,
							 gsize audio_bytes, GError **error);
	void (*promoted)(AiVoiceprintIdentifier *self, const gchar *guest, const gchar *name);
	gpointer _reserved[8];
};

AiVoiceprintIdentifier *
ai_voiceprint_identifier_new(const gchar *url, const gchar *directory);
AiVoiceprintIdentifier *
ai_voiceprint_identifier_new_full(AiSpeakerEmbedder *embedder, AiVoiceprintStore *store);

AiSpeakerEmbedder *
ai_voiceprint_identifier_get_embedder(AiVoiceprintIdentifier *self);
AiVoiceprintStore *
ai_voiceprint_identifier_get_store(AiVoiceprintIdentifier *self);
guint
ai_voiceprint_identifier_get_n_voices(AiVoiceprintIdentifier *self);
void
ai_voiceprint_identifier_reload(AiVoiceprintIdentifier *self);

gchar *
ai_voiceprint_identifier_closest(AiVoiceprintIdentifier *self, AiVoiceprint *heard,
								 const gchar *exclude, gdouble *score);
GPtrArray *
ai_voiceprint_identifier_list_guests(AiVoiceprintIdentifier *self);
void
ai_voiceprint_identifier_forget_guests(AiVoiceprintIdentifier *self);

void
ai_voiceprint_identifier_enroll_async(AiVoiceprintIdentifier *self, const gchar *name,
									  GBytes *pcm, GCancellable *cancellable,
									  GAsyncReadyCallback callback, gpointer user_data);
AiVoiceprint *
ai_voiceprint_identifier_enroll_finish(AiVoiceprintIdentifier *self, GAsyncResult *result,
									   GError **error);
G_END_DECLS
