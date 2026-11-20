/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-speaker-embedder.h"

/*
 * Audio in, voiceprint out. The wrapper checks what every implementation
 * would otherwise check for itself -- whole samples, enough of them -- so
 * an implementation only ever sees audio worth embedding.
 */
G_DEFINE_INTERFACE(AiSpeakerEmbedder, ai_speaker_embedder, G_TYPE_OBJECT)
static void
ai_speaker_embedder_default_init(AiSpeakerEmbedderInterface *iface)
{
}

/**
 * ai_speaker_embedder_get_min_bytes:
 * @self: an embedder
 *
 * Returns: the least audio, in bytes of 16 kHz mono S16LE, worth embedding
 */
gsize
ai_speaker_embedder_get_min_bytes(AiSpeakerEmbedder *self)
{
	AiSpeakerEmbedderInterface *iface;
	g_return_val_if_fail(AI_IS_SPEAKER_EMBEDDER(self), AI_SPEAKER_EMBEDDER_DEFAULT_MIN_BYTES);
	iface = AI_SPEAKER_EMBEDDER_GET_IFACE(self);
	return iface->get_min_bytes != NULL ? iface->get_min_bytes(self)
										: AI_SPEAKER_EMBEDDER_DEFAULT_MIN_BYTES;
}

/**
 * ai_speaker_embedder_embed_async:
 * @self: an embedder
 * @pcm: 16 kHz mono signed 16-bit little-endian audio
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 *
 * Audio shorter than ai_speaker_embedder_get_min_bytes(), or not a whole
 * number of samples, fails with %G_IO_ERROR_INVALID_DATA without reaching
 * the implementation.
 */
void
ai_speaker_embedder_embed_async(AiSpeakerEmbedder *self, GBytes *pcm, GCancellable *cancellable,
								GAsyncReadyCallback callback, gpointer user_data)
{
	AiSpeakerEmbedderInterface *iface;
	gsize size;
	g_return_if_fail(AI_IS_SPEAKER_EMBEDDER(self));
	g_return_if_fail(pcm != NULL);
	iface = AI_SPEAKER_EMBEDDER_GET_IFACE(self);
	size = g_bytes_get_size(pcm);
	if (iface->embed_async == NULL || size < ai_speaker_embedder_get_min_bytes(self) ||
		size % 2 != 0) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_set_source_tag(task, ai_speaker_embedder_embed_async);
		if (iface->embed_async == NULL)
			g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
									"Operation is not implemented");
		else
			g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
									"Too little audio to tell who spoke");
		return;
	}
	iface->embed_async(self, pcm, cancellable, callback, user_data);
}

/**
 * ai_speaker_embedder_embed_finish:
 * @self: an embedder
 * @result: asynchronous result
 * @error: return location for an error
 *
 * Returns: (transfer full) (nullable): the utterance's voiceprint, unnamed
 */
AiVoiceprint *
ai_speaker_embedder_embed_finish(AiSpeakerEmbedder *self, GAsyncResult *result, GError **error)
{
	AiSpeakerEmbedderInterface *iface;
	g_return_val_if_fail(AI_IS_SPEAKER_EMBEDDER(self), NULL);
	/* Refused by the wrapper above, before the implementation saw it. */
	if (g_async_result_is_tagged(result, ai_speaker_embedder_embed_async))
		return g_task_propagate_pointer(G_TASK(result), error);
	iface = AI_SPEAKER_EMBEDDER_GET_IFACE(self);
	return iface->embed_finish(self, result, error);
}
