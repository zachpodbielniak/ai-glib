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
 *
 * Starts naming the speaker of @pcm. Calls must complete in any order the
 * implementation likes; the session delivers finals in the order heard.
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
 * @error: return location for an error
 *
 * A voice that matches nobody is a match of kind
 * %AI_SPEAKER_MATCH_UNKNOWN with no name. An utterance too short to tell is
 * %G_IO_ERROR_INVALID_DATA, which a caller can tell apart from a stranger
 * and answer with whoever spoke last. Any other error means naming failed.
 *
 * Returns: (transfer full) (nullable): the match, or %NULL with @error set
 */
AiSpeakerMatch *
ai_speaker_identifier_identify_finish(AiSpeakerIdentifier *self, GAsyncResult *result,
									  GError **error)
{
	AiSpeakerIdentifierInterface *iface;
	g_return_val_if_fail(AI_IS_SPEAKER_IDENTIFIER(self), NULL);
	iface = AI_SPEAKER_IDENTIFIER_GET_IFACE(self);
	if (iface->identify_finish == NULL) {
		/* identify_async() returned NOT_SUPPORTED through a plain GTask. */
		if (G_IS_TASK(result))
			return g_task_propagate_pointer(G_TASK(result), error);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return NULL;
	}
	return iface->identify_finish(self, result, error);
}

/**
 * ai_speaker_identifier_promote:
 * @self: a #AiSpeakerIdentifier
 * @guest: the guest's name, as matched ("Guest 2")
 * @name: the lasting name to give that voice
 * @merge: when @name is already enrolled, average the guest into it rather
 *   than refuse
 * @error: return location for an error
 *
 * Gives a voice heard during the call a lasting name, so it is named that
 * from now on. Without @merge an enrolled @name is %G_IO_ERROR_EXISTS: one
 * person must never silently take over another's voiceprint. An unknown
 * @guest is %G_IO_ERROR_NOT_FOUND; an identifier that cannot promote is
 * %G_IO_ERROR_NOT_SUPPORTED.
 *
 * Returns: whether the voice was saved under @name
 */
gboolean
ai_speaker_identifier_promote(AiSpeakerIdentifier *self, const gchar *guest, const gchar *name,
							  gboolean merge, GError **error)
{
	AiSpeakerIdentifierInterface *iface;
	g_return_val_if_fail(AI_IS_SPEAKER_IDENTIFIER(self), FALSE);
	g_return_val_if_fail(guest != NULL, FALSE);
	g_return_val_if_fail(name != NULL, FALSE);
	iface = AI_SPEAKER_IDENTIFIER_GET_IFACE(self);
	if (iface->promote == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"This speaker identifier cannot save voices");
		return FALSE;
	}
	return iface->promote(self, guest, name, merge, error);
}

/**
 * ai_speaker_identifier_has_name:
 * @self: a #AiSpeakerIdentifier
 * @name: a name
 *
 * Returns: whether @name is already enrolled
 */
gboolean
ai_speaker_identifier_has_name(AiSpeakerIdentifier *self, const gchar *name)
{
	AiSpeakerIdentifierInterface *iface;
	g_return_val_if_fail(AI_IS_SPEAKER_IDENTIFIER(self), FALSE);
	iface = AI_SPEAKER_IDENTIFIER_GET_IFACE(self);
	return name != NULL && iface->has_name != NULL && iface->has_name(self, name);
}
