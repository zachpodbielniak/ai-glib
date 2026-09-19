/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voice-activity.h"

static void
ai_voice_activity_default_init(AiVoiceActivityInterface *iface)
{
}

G_DEFINE_INTERFACE(AiVoiceActivity, ai_voice_activity, G_TYPE_OBJECT)

/**
 * ai_voice_activity_process:
 * @self: a #AiVoiceActivity
 * @speaker: stable stream id
 * @pcm: 16 kHz mono S16LE PCM; a whole number of samples
 * @error: return location for an error
 *
 * Returns: speech/end flags, zero for silence, or -1 on error
 */
gint
ai_voice_activity_process(AiVoiceActivity *self, const gchar *speaker, GBytes *pcm,
						  GError **error)
{
	AiVoiceActivityInterface *iface;
	g_return_val_if_fail(AI_IS_VOICE_ACTIVITY(self), -1);
	iface = AI_VOICE_ACTIVITY_GET_IFACE(self);
	if (iface->process == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
							"Operation is not implemented");
		return -1;
	}
	return iface->process(self, speaker, pcm, error);
}

/**
 * ai_voice_activity_reset:
 * @self: a #AiVoiceActivity
 * @speaker: stable stream id
 */
void
ai_voice_activity_reset(AiVoiceActivity *self, const gchar *speaker)
{
	AiVoiceActivityInterface *iface;
	g_return_if_fail(AI_IS_VOICE_ACTIVITY(self));
	iface = AI_VOICE_ACTIVITY_GET_IFACE(self);
	if (iface->reset == NULL) {
		g_critical("Required voice interface method is missing");
		return;
	}
	iface->reset(self, speaker);
}
