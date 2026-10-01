/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_SPEECH_RECOGNIZER (ai_speech_recognizer_get_type())
G_DECLARE_INTERFACE(AiSpeechRecognizer, ai_speech_recognizer, AI, SPEECH_RECOGNIZER,
					GObject)
struct _AiSpeechRecognizerInterface {
	GTypeInterface parent_iface;
	gboolean (*begin)(AiSpeechRecognizer *self, const gchar *speaker, GError **error);
	gboolean (*feed)(AiSpeechRecognizer *self, const gchar *speaker, GBytes *pcm,
					 GError **error);
	void (*end)(AiSpeechRecognizer *self, const gchar *speaker);
	void (*cancel)(AiSpeechRecognizer *self, const gchar *speaker);
	gpointer _reserved[8];
};

gboolean
ai_speech_recognizer_begin(AiSpeechRecognizer *self, const gchar *speaker,
						   GError **error);

gboolean
ai_speech_recognizer_feed(AiSpeechRecognizer *self, const gchar *speaker, GBytes *pcm,
						  GError **error);

void
ai_speech_recognizer_end(AiSpeechRecognizer *self, const gchar *speaker);

void
ai_speech_recognizer_cancel(AiSpeechRecognizer *self, const gchar *speaker);
G_END_DECLS
