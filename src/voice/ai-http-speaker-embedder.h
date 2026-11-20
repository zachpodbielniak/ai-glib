/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-speaker-embedder.h"
G_BEGIN_DECLS
#define AI_TYPE_HTTP_SPEAKER_EMBEDDER (ai_http_speaker_embedder_get_type())
G_DECLARE_DERIVABLE_TYPE(AiHttpSpeakerEmbedder, ai_http_speaker_embedder, AI,
						 HTTP_SPEAKER_EMBEDDER, GObject)
/**
 * AiHttpSpeakerEmbedderClass:
 * @parent_class: the parent class
 * @prepare_message: adjust the request before it is sent (add a header, say);
 *   a header carrying a credential stops the request following any redirect
 * @parse_reply: turn a successful reply body into a voiceprint; reads
 *   {"model": ..., "embedding": [...]} by default
 */
struct _AiHttpSpeakerEmbedderClass {
	GObjectClass parent_class;
	void (*prepare_message)(AiHttpSpeakerEmbedder *self, gpointer message);
	AiVoiceprint *(*parse_reply)(AiHttpSpeakerEmbedder *self, GBytes *body, GError **error);
	gpointer _reserved[8];
};

/**
 * AI_HTTP_SPEAKER_EMBEDDER_MAX_REPLY:
 *
 * The largest reply body read, in bytes.
 */
#define AI_HTTP_SPEAKER_EMBEDDER_MAX_REPLY ((gsize)1024 * 1024)

AiHttpSpeakerEmbedder *
ai_http_speaker_embedder_new(const gchar *url);
const gchar *
ai_http_speaker_embedder_get_url(AiHttpSpeakerEmbedder *self);
G_END_DECLS
