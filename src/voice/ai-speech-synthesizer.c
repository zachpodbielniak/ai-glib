/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-speech-synthesizer.h"

static void
ai_speech_synthesizer_default_init(AiSpeechSynthesizerInterface *iface)
{
	/**
	 * AiSpeechSynthesizer::audio:
	 * @self: the emitter
	 * @pcm: 16 kHz mono signed little-endian PCM
	 */
	g_signal_new("audio", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 1, G_TYPE_BYTES);
}

G_DEFINE_INTERFACE(AiSpeechSynthesizer, ai_speech_synthesizer, G_TYPE_OBJECT)

/**
 * ai_speech_synthesizer_synthesize_async:
 * @self: a #AiSpeechSynthesizer
 * @text: UTF-8 text to speak
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 */
void
ai_speech_synthesizer_synthesize_async(AiSpeechSynthesizer *self, const gchar *text,
									   GCancellable *cancellable,
									   GAsyncReadyCallback callback, gpointer user_data)
{
	AiSpeechSynthesizerInterface *iface;
	g_return_if_fail(AI_IS_SPEECH_SYNTHESIZER(self));
	iface = AI_SPEECH_SYNTHESIZER_GET_IFACE(self);
	if (iface->synthesize_async == NULL) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Operation is not implemented");
		return;
	}
	iface->synthesize_async(self, text, cancellable, callback, user_data);
}

/**
 * ai_speech_synthesizer_synthesize_finish:
 * @self: a #AiSpeechSynthesizer
 * @result: asynchronous result
 * @error: return location for an error
 *
 * Returns: whether the operation succeeded
 */
gboolean
ai_speech_synthesizer_synthesize_finish(AiSpeechSynthesizer *self, GAsyncResult *result,
										GError **error)
{
	AiSpeechSynthesizerInterface *iface;
	g_return_val_if_fail(AI_IS_SPEECH_SYNTHESIZER(self), FALSE);
	iface = AI_SPEECH_SYNTHESIZER_GET_IFACE(self);
	if (iface->synthesize_finish == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return FALSE;
	}
	return iface->synthesize_finish(self, result, error);
}
