/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-speaker-identifier.h"
G_BEGIN_DECLS
#define AI_TYPE_VOICEPRINT_IDENTIFIER (ai_voiceprint_identifier_get_type())
G_DECLARE_FINAL_TYPE(AiVoiceprintIdentifier, ai_voiceprint_identifier, AI,
					 VOICEPRINT_IDENTIFIER, GObject)
AiVoiceprintIdentifier *
ai_voiceprint_identifier_new(const gchar *url, const gchar *directory);

guint
ai_voiceprint_identifier_get_n_voices(AiVoiceprintIdentifier *self);

void
ai_voiceprint_identifier_enroll_async(AiVoiceprintIdentifier *self, const gchar *name,
									  GBytes *pcm, GCancellable *cancellable,
									  GAsyncReadyCallback callback, gpointer user_data);

gboolean
ai_voiceprint_identifier_enroll_finish(AiVoiceprintIdentifier *self, GAsyncResult *result,
									   GError **error);
G_END_DECLS
