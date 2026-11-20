/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-speaker-embedder.h"
G_BEGIN_DECLS
/**
 * AiSpeakerFrontend:
 * @AI_SPEAKER_FRONTEND_AUTO: from the model's metadata, else its input shape
 * @AI_SPEAKER_FRONTEND_KALDI: Kaldi filterbanks, input [batch, frames, bins]
 *   (wespeaker, 3D-Speaker)
 * @AI_SPEAKER_FRONTEND_NEMO: NeMo mel spectrogram, input [batch, bins,
 *   frames] and a length (TitaNet)
 *
 * The features a speaker-embedding model was trained on.
 */
typedef enum {
	AI_SPEAKER_FRONTEND_AUTO,
	AI_SPEAKER_FRONTEND_KALDI,
	AI_SPEAKER_FRONTEND_NEMO
} AiSpeakerFrontend;
#define AI_TYPE_SPEAKER_FRONTEND (ai_speaker_frontend_get_type())
GType
ai_speaker_frontend_get_type(void) G_GNUC_CONST;

#define AI_TYPE_ONNX_SPEAKER_EMBEDDER (ai_onnx_speaker_embedder_get_type())
G_DECLARE_DERIVABLE_TYPE(AiOnnxSpeakerEmbedder, ai_onnx_speaker_embedder, AI,
						 ONNX_SPEAKER_EMBEDDER, GObject)
/**
 * AiOnnxSpeakerEmbedderClass:
 * @parent_class: the parent class
 * @features: compute the model's input features from samples in [-1, 1];
 *   returns frames * bins floats, frame-major. The default follows
 *   #AiOnnxSpeakerEmbedder:frontend.
 */
struct _AiOnnxSpeakerEmbedderClass {
	GObjectClass parent_class;
	gfloat *(*features)(AiOnnxSpeakerEmbedder *self, const gfloat *samples, gsize n,
						guint bins, gsize *frames);
	gpointer _reserved[8];
};
gboolean
ai_onnx_speaker_embedder_is_available(void);
AiOnnxSpeakerEmbedder *
ai_onnx_speaker_embedder_new(const gchar *model_path, GError **error);
const gchar *
ai_onnx_speaker_embedder_get_model_id(AiOnnxSpeakerEmbedder *self);
guint
ai_onnx_speaker_embedder_get_dimensions(AiOnnxSpeakerEmbedder *self);
AiSpeakerFrontend
ai_onnx_speaker_embedder_get_frontend(AiOnnxSpeakerEmbedder *self);
AiVoiceprint *
ai_onnx_speaker_embedder_embed(AiOnnxSpeakerEmbedder *self, GBytes *pcm,
							   GCancellable *cancellable, GError **error);
G_END_DECLS
