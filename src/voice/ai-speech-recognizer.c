/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-speech-recognizer.h"

static void
ai_speech_recognizer_default_init(AiSpeechRecognizerInterface *iface)
{
	/**
	 * AiSpeechRecognizer::transcript:
	 * @self: the emitter
	 * @speaker: stream id
	 * @text: UTF-8 transcript
	 * @final: whether the utterance is complete
	 */
	g_signal_new("transcript", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL,
				 NULL, NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING,
				 G_TYPE_BOOLEAN);
	/**
	 * AiSpeechRecognizer::error:
	 * @self: the emitter
	 * @speaker: stream id
	 * @error: failure
	 */
	g_signal_new("error", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_ERROR);
}

G_DEFINE_INTERFACE(AiSpeechRecognizer, ai_speech_recognizer, G_TYPE_OBJECT)

/**
 * ai_speech_recognizer_begin:
 * @self: a #AiSpeechRecognizer
 * @speaker: stable stream id
 * @error: return location for an error
 *
 * Returns: whether the operation succeeded
 */
gboolean
ai_speech_recognizer_begin(AiSpeechRecognizer *self, const gchar *speaker, GError **error)
{
	AiSpeechRecognizerInterface *iface;
	g_return_val_if_fail(AI_IS_SPEECH_RECOGNIZER(self), FALSE);
	iface = AI_SPEECH_RECOGNIZER_GET_IFACE(self);
	if (iface->begin == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return FALSE;
	}
	return iface->begin(self, speaker, error);
}

/**
 * ai_speech_recognizer_feed:
 * @self: a #AiSpeechRecognizer
 * @speaker: stable stream id
 * @pcm: 16 kHz mono S16LE PCM; a whole number of samples
 * @error: return location for an error
 *
 * Returns: whether the operation succeeded
 */
gboolean
ai_speech_recognizer_feed(AiSpeechRecognizer *self, const gchar *speaker, GBytes *pcm,
						  GError **error)
{
	AiSpeechRecognizerInterface *iface;
	g_return_val_if_fail(AI_IS_SPEECH_RECOGNIZER(self), FALSE);
	iface = AI_SPEECH_RECOGNIZER_GET_IFACE(self);
	if (iface->feed == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return FALSE;
	}
	return iface->feed(self, speaker, pcm, error);
}

/**
 * ai_speech_recognizer_end:
 * @self: a #AiSpeechRecognizer
 * @speaker: stable stream id
 */
void
ai_speech_recognizer_end(AiSpeechRecognizer *self, const gchar *speaker)
{
	AiSpeechRecognizerInterface *iface;
	g_return_if_fail(AI_IS_SPEECH_RECOGNIZER(self));
	iface = AI_SPEECH_RECOGNIZER_GET_IFACE(self);
	if (iface->end == NULL) {
		g_critical("Required voice interface method is missing");
		return;
	}
	iface->end(self, speaker);
}

/**
 * ai_speech_recognizer_cancel:
 * @self: a #AiSpeechRecognizer
 * @speaker: stable stream id
 */
void
ai_speech_recognizer_cancel(AiSpeechRecognizer *self, const gchar *speaker)
{
	AiSpeechRecognizerInterface *iface;
	g_return_if_fail(AI_IS_SPEECH_RECOGNIZER(self));
	iface = AI_SPEECH_RECOGNIZER_GET_IFACE(self);
	if (iface->cancel == NULL) {
		g_critical("Required voice interface method is missing");
		return;
	}
	iface->cancel(self, speaker);
}
