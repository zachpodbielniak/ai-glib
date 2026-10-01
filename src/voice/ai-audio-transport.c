/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-audio-transport.h"

static void
ai_audio_transport_default_init(AiAudioTransportInterface *iface)
{
	/**
	 * AiAudioTransport::reconnecting:
	 * @self: the transport
	 *
	 * Media recovery began. Preserve the active turn and await reconnected.
	 * Backends that resume queued playback keep write operations pending;
	 * callers can still cancel them explicitly during recovery.
	 */
	g_signal_new("reconnecting", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL,
				 NULL, NULL, G_TYPE_NONE, 0);
	/**
	 * AiAudioTransport::reconnected:
	 * @self: the transport
	 *
	 * Media is available again; new speech can be submitted.
	 */
	g_signal_new("reconnected", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL,
				 NULL, NULL, G_TYPE_NONE, 0);
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

/**
 * ai_audio_transport_write_pcm_async:
 * @self: a #AiAudioTransport
 * @pcm: mono S16LE PCM containing whole samples
 * @sample_rate: rate of this buffer in Hz, from 8000 to 192000
 * @cancellable: (nullable): cancellation token
 * @callback: (scope async): completion callback
 * @user_data: data for @callback
 *
 * Plays native-rate output. The rate travels with each queued buffer; input
 * audio remains 16 kHz. Complete with ai_audio_transport_write_pcm_finish().
 * Backends without native-rate support accept only the legacy 16 kHz rate.
 */
void
ai_audio_transport_write_pcm_async(AiAudioTransport *self, GBytes *pcm, guint sample_rate,
								   GCancellable *cancellable,
								   GAsyncReadyCallback callback, gpointer user_data)
{
	AiAudioTransportInterface *iface;
	g_return_if_fail(AI_IS_AUDIO_TRANSPORT(self));
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (sample_rate < 8000 || sample_rate > 192000 || pcm == NULL ||
		g_bytes_get_size(pcm) % 2 != 0) {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
								"Invalid PCM sample rate or alignment");
	} else if (iface->write_pcm_async != NULL)
		iface->write_pcm_async(self, pcm, sample_rate, cancellable, callback, user_data);
	else if (sample_rate == 16000)
		ai_audio_transport_write_async(self, pcm, cancellable, callback, user_data);
	else {
		g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
								"Transport does not support native-rate PCM");
	}
}

/**
 * ai_audio_transport_write_pcm_finish:
 * @self: a #AiAudioTransport
 * @result: asynchronous result
 * @error: (out) (optional): return location for an error
 *
 * Returns: whether native-rate PCM was played locally
 */
gboolean
ai_audio_transport_write_pcm_finish(AiAudioTransport *self, GAsyncResult *result,
									GError **error)
{
	AiAudioTransportInterface *iface;
	g_return_val_if_fail(AI_IS_AUDIO_TRANSPORT(self), FALSE);
	iface = AI_AUDIO_TRANSPORT_GET_IFACE(self);
	if (iface->write_pcm_finish != NULL)
		return iface->write_pcm_finish(self, result, error);
	return ai_audio_transport_write_finish(self, result, error);
}
