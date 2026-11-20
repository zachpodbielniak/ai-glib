/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
#include "voice/ai-voiceprint.h"
G_BEGIN_DECLS
#define AI_TYPE_SPEAKER_EMBEDDER (ai_speaker_embedder_get_type())
G_DECLARE_INTERFACE(AiSpeakerEmbedder, ai_speaker_embedder, AI, SPEAKER_EMBEDDER, GObject)
/**
 * AiSpeakerEmbedderInterface:
 * @parent_iface: the parent interface
 * @embed_async: start turning 16 kHz mono S16LE audio into a voiceprint
 * @embed_finish: the unnamed #AiVoiceprint, or %G_IO_ERROR_INVALID_DATA
 *   when the audio is too short
 * @get_min_bytes: the least audio worth embedding; optional, half a second
 *   when unset
 *
 * Turns audio into a voiceprint. #AiHttpSpeakerEmbedder asks a service;
 * #AiOnnxSpeakerEmbedder runs a model in process. #AiVoiceprintIdentifier
 * takes either, or an application's own.
 */
struct _AiSpeakerEmbedderInterface {
	GTypeInterface parent_iface;
	void (*embed_async)(AiSpeakerEmbedder *self, GBytes *pcm, GCancellable *cancellable,
						GAsyncReadyCallback callback, gpointer user_data);
	AiVoiceprint *(*embed_finish)(AiSpeakerEmbedder *self, GAsyncResult *result,
								  GError **error);
	gsize (*get_min_bytes)(AiSpeakerEmbedder *self);
	gpointer _reserved[8];
};

/**
 * AI_SPEAKER_EMBEDDER_DEFAULT_MIN_BYTES:
 *
 * Half a second of 16 kHz mono 16-bit audio: less says too little about a
 * voice to embed.
 */
#define AI_SPEAKER_EMBEDDER_DEFAULT_MIN_BYTES ((gsize)16000)

void
ai_speaker_embedder_embed_async(AiSpeakerEmbedder *self, GBytes *pcm, GCancellable *cancellable,
								GAsyncReadyCallback callback, gpointer user_data);
AiVoiceprint *
ai_speaker_embedder_embed_finish(AiSpeakerEmbedder *self, GAsyncResult *result, GError **error);
gsize
ai_speaker_embedder_get_min_bytes(AiSpeakerEmbedder *self);
G_END_DECLS
