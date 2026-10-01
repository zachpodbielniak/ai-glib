/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-speech-synthesizer.h"
G_BEGIN_DECLS
#define AI_TYPE_HTTP_SYNTHESIZER (ai_http_synthesizer_get_type())
G_DECLARE_FINAL_TYPE(AiHttpSynthesizer, ai_http_synthesizer, AI, HTTP_SYNTHESIZER,
					 GObject)
AiHttpSynthesizer *
ai_http_synthesizer_new(const gchar *url);
G_END_DECLS
