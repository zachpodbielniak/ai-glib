/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_SPEECH_SYNTHESIZER (ai_speech_synthesizer_get_type())
G_DECLARE_INTERFACE(AiSpeechSynthesizer, ai_speech_synthesizer, AI, SPEECH_SYNTHESIZER,
					GObject)
struct _AiSpeechSynthesizerInterface {
	GTypeInterface parent_iface;
	void (*synthesize_async)(AiSpeechSynthesizer *self, const gchar *text,
							 GCancellable *cancellable, GAsyncReadyCallback callback,
							 gpointer user_data);
	gboolean (*synthesize_finish)(AiSpeechSynthesizer *self, GAsyncResult *result,
								  GError **error);
	gpointer _reserved[8];
};

void
ai_speech_synthesizer_synthesize_async(AiSpeechSynthesizer *self, const gchar *text,
									   GCancellable *cancellable,
									   GAsyncReadyCallback callback, gpointer user_data);

gboolean
ai_speech_synthesizer_synthesize_finish(AiSpeechSynthesizer *self, GAsyncResult *result,
										GError **error);
G_END_DECLS
