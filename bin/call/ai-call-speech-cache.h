/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_CALL_SPEECH_CACHE (ai_call_speech_cache_get_type())
G_DECLARE_FINAL_TYPE(AiCallSpeechCache, ai_call_speech_cache, AI, CALL_SPEECH_CACHE,
					 GObject)
AiCallSpeechCache *
ai_call_speech_cache_new(const gchar *url, const gchar *text);
void
ai_call_speech_cache_start(AiCallSpeechCache *self);
void
ai_call_speech_cache_cancel(AiCallSpeechCache *self);
/* Borrowed, NULL until a complete synthesis has succeeded. */
GBytes *
ai_call_speech_cache_get_pcm(AiCallSpeechCache *self);
guint
ai_call_speech_cache_get_sample_rate(AiCallSpeechCache *self);
G_END_DECLS
