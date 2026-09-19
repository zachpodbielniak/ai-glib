/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_AUDIO_TRANSPORT (ai_audio_transport_get_type())
G_DECLARE_INTERFACE(AiAudioTransport, ai_audio_transport, AI, AUDIO_TRANSPORT, GObject)
struct _AiAudioTransportInterface {
	GTypeInterface parent_iface;
	void (*join_async)(AiAudioTransport *self, const gchar *room, const gchar *token,
					   GCancellable *cancellable, GAsyncReadyCallback callback,
					   gpointer user_data);
	gboolean (*join_finish)(AiAudioTransport *self, GAsyncResult *result, GError **error);
	void (*leave_async)(AiAudioTransport *self, GCancellable *cancellable,
						GAsyncReadyCallback callback, gpointer user_data);
	gboolean (*leave_finish)(AiAudioTransport *self, GAsyncResult *result,
							 GError **error);
	void (*write_async)(AiAudioTransport *self, GBytes *pcm, GCancellable *cancellable,
						GAsyncReadyCallback callback, gpointer user_data);
	gboolean (*write_finish)(AiAudioTransport *self, GAsyncResult *result,
							 GError **error);
	void (*flush)(AiAudioTransport *self);
	void (*write_pcm_async)(AiAudioTransport *self, GBytes *pcm, guint sample_rate,
							GCancellable *cancellable, GAsyncReadyCallback callback,
							gpointer user_data);
	gboolean (*write_pcm_finish)(AiAudioTransport *self, GAsyncResult *result, GError **error);
	gpointer _reserved[8];
};

void
ai_audio_transport_join_async(AiAudioTransport *self, const gchar *room,
							  const gchar *token, GCancellable *cancellable,
							  GAsyncReadyCallback callback, gpointer user_data);

gboolean
ai_audio_transport_join_finish(AiAudioTransport *self, GAsyncResult *result,
							   GError **error);

void
ai_audio_transport_leave_async(AiAudioTransport *self, GCancellable *cancellable,
							   GAsyncReadyCallback callback, gpointer user_data);

gboolean
ai_audio_transport_leave_finish(AiAudioTransport *self, GAsyncResult *result,
								GError **error);

void
ai_audio_transport_write_async(AiAudioTransport *self, GBytes *pcm,
							   GCancellable *cancellable, GAsyncReadyCallback callback,
							   gpointer user_data);

gboolean
ai_audio_transport_write_finish(AiAudioTransport *self, GAsyncResult *result,
								GError **error);

void
ai_audio_transport_flush(AiAudioTransport *self);
void
ai_audio_transport_write_pcm_async(AiAudioTransport *self, GBytes *pcm, guint sample_rate,
								   GCancellable *cancellable,
								   GAsyncReadyCallback callback, gpointer user_data);
gboolean
ai_audio_transport_write_pcm_finish(AiAudioTransport *self, GAsyncResult *result,
									GError **error);
G_END_DECLS
