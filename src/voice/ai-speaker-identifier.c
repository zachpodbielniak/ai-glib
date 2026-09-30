/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-speaker-identifier.h"

/*
 * Who said an utterance, from its audio. The session asks after each final
 * transcript, so several people sharing one microphone are told apart: a
 * device in a room hears everyone as one participant.
 */
G_DEFINE_INTERFACE(AiSpeakerIdentifier, ai_speaker_identifier, G_TYPE_OBJECT)
static void
ai_speaker_identifier_default_init(AiSpeakerIdentifierInterface *iface)
{
}

/**
 * ai_speaker_identifier_identify_async:
 * @self: a #AiSpeakerIdentifier
 * @pcm: the utterance, 16 kHz mono signed 16-bit little-endian
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 */
void
ai_speaker_identifier_identify_async(AiSpeakerIdentifier *self, GBytes *pcm,
									 GCancellable *cancellable, GAsyncReadyCallback callback,
									 gpointer user_data)
{
	AiSpeakerIdentifierInterface *iface;
	g_return_if_fail(AI_IS_SPEAKER_IDENTIFIER(self));
	g_return_if_fail(pcm != NULL);
	iface = AI_SPEAKER_IDENTIFIER_GET_IFACE(self);
	if (iface->identify_async == NULL) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Operation is not implemented");
		return;
	}
	iface->identify_async(self, pcm, cancellable, callback, user_data);
}

/**
 * ai_speaker_identifier_identify_finish:
 * @self: a #AiSpeakerIdentifier
 * @result: asynchronous result
 * @score: (out) (optional): similarity of the best match, 0 to 1
 * @error: return location for an error
 *
 * %NULL with no error means a voice that matches nobody. An utterance too
 * short to tell is %G_IO_ERROR_INVALID_DATA, which a caller can tell apart
 * from a stranger and answer with whoever spoke last.
 *
 * Returns: (transfer full) (nullable): the speaker's name
 */
gchar *
ai_speaker_identifier_identify_finish(AiSpeakerIdentifier *self, GAsyncResult *result,
									  gdouble *score, GError **error)
{
	AiSpeakerIdentifierInterface *iface;
	if (score != NULL)
		*score = 0;
	g_return_val_if_fail(AI_IS_SPEAKER_IDENTIFIER(self), NULL);
	iface = AI_SPEAKER_IDENTIFIER_GET_IFACE(self);
	if (iface->identify_finish == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return NULL;
	}
	return iface->identify_finish(self, result, score, error);
}
