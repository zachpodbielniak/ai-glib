/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-speech-recognizer.h"
G_BEGIN_DECLS
#define AI_TYPE_WEBSOCKET_RECOGNIZER (ai_websocket_recognizer_get_type())
G_DECLARE_FINAL_TYPE(AiWebsocketRecognizer, ai_websocket_recognizer, AI,
					 WEBSOCKET_RECOGNIZER, GObject)
AiWebsocketRecognizer *
ai_websocket_recognizer_new(const gchar *url);
G_END_DECLS
