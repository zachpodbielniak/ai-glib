/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voice-input-private.h"

static const gchar *const levels[] = {"low", "moderate", "high", "very-high", NULL};

gboolean
ai_voice_input_level_valid(const gchar *level)
{
	return level == NULL || g_str_equal(level, "off") ||
		   g_strv_contains(levels, level);
}

/* WebRTC's suppressor runs on the CPU at 16 kHz, after conversion, so it
 * adds nothing to the speech services' GPU. Its high-pass filter removes the
 * rumble of footsteps and handling noise below speech. Gain control and echo
 * cancellation stay off: the caller's client already does both, and doing
 * them twice pumps the noise floor. */
gchar *
ai_voice_input_description(const gchar *level)
{
	const gchar *suppress = "";
	g_autofree gchar *dsp = NULL;
	if (level != NULL && g_strv_contains(levels, level)) {
		dsp = g_strdup_printf("webrtcdsp echo-cancel=false gain-control=false "
							  "voice-detection=false high-pass-filter=true "
							  "noise-suppression=true noise-suppression-level=%s ! ",
							  level);
		suppress = dsp;
	}
	return g_strdup_printf(
		"queue max-size-time=200000000 ! audioconvert ! audioresample ! "
		"audio/x-raw,format=S16LE,rate=16000,channels=1,layout=interleaved ! %s"
		"appsink name=pcm emit-signals=true sync=false max-buffers=20 drop=true",
		suppress);
}
