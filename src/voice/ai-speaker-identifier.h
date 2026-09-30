/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_SPEAKER_IDENTIFIER (ai_speaker_identifier_get_type())
G_DECLARE_INTERFACE(AiSpeakerIdentifier, ai_speaker_identifier, AI, SPEAKER_IDENTIFIER,
					GObject)
struct _AiSpeakerIdentifierInterface {
	GTypeInterface parent_iface;
	void (*identify_async)(AiSpeakerIdentifier *self, GBytes *pcm, GCancellable *cancellable,
						   GAsyncReadyCallback callback, gpointer user_data);
	gchar *(*identify_finish)(AiSpeakerIdentifier *self, GAsyncResult *result, gdouble *score,
							  GError **error);
	gpointer _reserved[8];
};

void
ai_speaker_identifier_identify_async(AiSpeakerIdentifier *self, GBytes *pcm,
									 GCancellable *cancellable, GAsyncReadyCallback callback,
									 gpointer user_data);

gchar *
ai_speaker_identifier_identify_finish(AiSpeakerIdentifier *self, GAsyncResult *result,
									  gdouble *score, GError **error);
G_END_DECLS
