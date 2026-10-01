/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#ifndef AI_GLIB_COMPILATION
#error "Private header"
#endif
#include <glib.h>
G_BEGIN_DECLS
/* gst-launch description of the chain every subscribed track passes through
 * before VAD: a 200 ms queue, conversion to 16 kHz mono S16LE, and an appsink
 * named "pcm". LEVEL is NULL or "off" for no processing, or one of "low",
 * "moderate", "high", "very-high" to insert noise suppression. Transfer full. */
gchar *
ai_voice_input_description(const gchar *level);
/* TRUE for NULL and every name ai_voice_input_description() accepts. */
gboolean
ai_voice_input_level_valid(const gchar *level);
G_END_DECLS
