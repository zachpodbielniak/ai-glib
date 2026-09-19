/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-audio-transport.h"

static void
ai_audio_transport_default_init(AiAudioTransportInterface *iface)
{
	/**
	 * AiAudioTransport::audio:
	 * @self: the emitter
	 * @speaker: stable participant id
	 * @pcm: 16 kHz mono signed little-endian PCM
	 */
	g_signal_new("audio", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BYTES);
	/**
	 * AiAudioTransport::participant-joined:
	 * @self: the emitter
	 * @speaker: stable participant id
	 * @name: display name
	 */
	g_signal_new("participant-joined", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0,
				 NULL, NULL, NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
	/**
	 * AiAudioTransport::participant-left:
	 * @self: the emitter
	 * @speaker: stable participant id
	 */
	g_signal_new("participant-left", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0,
				 NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
	/**
	 * AiAudioTransport::error:
	 * @self: the emitter
	 * @error: failure
	 */
	g_signal_new("error", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
}

G_DEFINE_INTERFACE(AiAudioTransport, ai_audio_transport, G_TYPE_OBJECT)

/**
 * ai_audio_transport_join_async:
 * @self: a #AiAudioTransport
 * @room: room id
 * @token: access token; never logged
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 */
void
ai_audio_transport_join_async(AiAudioTransport *self, const gchar *room,
							  const gchar *token, GCancellable *cancellable,
							  GAsyncReadyCallback callback, gpointer user_data)
{
	AiAudioTransportInterface *iface;
	g_return_if_fail(AI_IS_AUDIO_TRANSPORT(self));
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->join_async == NULL) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Operation is not implemented");
		return;
	}
	iface->join_async(self, room, token, cancellable, callback, user_data);
}

/**
 * ai_audio_transport_join_finish:
 * @self: a #AiAudioTransport
 * @result: asynchronous result
 * @error: return location for an error
 *
 * Returns: whether the operation succeeded
 */
gboolean
ai_audio_transport_join_finish(AiAudioTransport *self, GAsyncResult *result,
							   GError **error)
{
	AiAudioTransportInterface *iface;
	g_return_val_if_fail(AI_IS_AUDIO_TRANSPORT(self), FALSE);
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->join_finish == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return FALSE;
	}
	return iface->join_finish(self, result, error);
}

/**
 * ai_audio_transport_leave_async:
 * @self: a #AiAudioTransport
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 */
void
ai_audio_transport_leave_async(AiAudioTransport *self, GCancellable *cancellable,
							   GAsyncReadyCallback callback, gpointer user_data)
{
	AiAudioTransportInterface *iface;
	g_return_if_fail(AI_IS_AUDIO_TRANSPORT(self));
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->leave_async == NULL) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Operation is not implemented");
		return;
	}
	iface->leave_async(self, cancellable, callback, user_data);
}

/**
 * ai_audio_transport_leave_finish:
 * @self: a #AiAudioTransport
 * @result: asynchronous result
 * @error: return location for an error
 *
 * Returns: whether the operation succeeded
 */
gboolean
ai_audio_transport_leave_finish(AiAudioTransport *self, GAsyncResult *result,
								GError **error)
{
	AiAudioTransportInterface *iface;
	g_return_val_if_fail(AI_IS_AUDIO_TRANSPORT(self), FALSE);
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->leave_finish == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return FALSE;
	}
	return iface->leave_finish(self, result, error);
}

/**
 * ai_audio_transport_write_async:
 * @self: a #AiAudioTransport
 * @pcm: 16 kHz mono S16LE PCM; a whole number of samples
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 *
 * Completes after local playout, not merely after queuing. Cancellation must
 * discard queued audio. Remote receipt cannot be guaranteed by RTP.
 */
void
ai_audio_transport_write_async(AiAudioTransport *self, GBytes *pcm,
							   GCancellable *cancellable, GAsyncReadyCallback callback,
							   gpointer user_data)
{
	AiAudioTransportInterface *iface;
	g_return_if_fail(AI_IS_AUDIO_TRANSPORT(self));
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->write_async == NULL) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Operation is not implemented");
		return;
	}
	iface->write_async(self, pcm, cancellable, callback, user_data);
}

/**
 * ai_audio_transport_write_finish:
 * @self: a #AiAudioTransport
 * @result: asynchronous result
 * @error: return location for an error
 *
 * Returns: whether the operation succeeded
 */
gboolean
ai_audio_transport_write_finish(AiAudioTransport *self, GAsyncResult *result,
								GError **error)
{
	AiAudioTransportInterface *iface;
	g_return_val_if_fail(AI_IS_AUDIO_TRANSPORT(self), FALSE);
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->write_finish == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return FALSE;
	}
	return iface->write_finish(self, result, error);
}

/**
 * ai_audio_transport_flush:
 * @self: a #AiAudioTransport
 */
void
ai_audio_transport_flush(AiAudioTransport *self)
{
	AiAudioTransportInterface *iface;
	g_return_if_fail(AI_IS_AUDIO_TRANSPORT(self));
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->flush == NULL) {
		g_critical("Required voice interface method is missing");
		return;
	}
	iface->flush(self);
}
