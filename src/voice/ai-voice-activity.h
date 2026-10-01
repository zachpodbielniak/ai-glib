/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_VOICE_ACTIVITY (ai_voice_activity_get_type())
G_DECLARE_INTERFACE(AiVoiceActivity, ai_voice_activity, AI, VOICE_ACTIVITY, GObject)
struct _AiVoiceActivityInterface {
	GTypeInterface parent_iface;
	gint (*process)(AiVoiceActivity *self, const gchar *speaker, GBytes *pcm,
					GError **error);
	void (*reset)(AiVoiceActivity *self, const gchar *speaker);
	gpointer _reserved[8];
};

#define AI_VOICE_ACTIVITY_SPEECH 1
#define AI_VOICE_ACTIVITY_END 2

gint
ai_voice_activity_process(AiVoiceActivity *self, const gchar *speaker, GBytes *pcm,
						  GError **error);

void
ai_voice_activity_reset(AiVoiceActivity *self, const gchar *speaker);
G_END_DECLS
