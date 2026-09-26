/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "core/ai-decider.h"
G_BEGIN_DECLS
#define AI_LAYA_MODEL_ENGLISH "english"
#define AI_LAYA_MODEL_MULTILINGUAL "multilingual"
#define AI_LAYA_MODEL_TYPED_DECISIONS "typed-decisions"
#define AI_TYPE_LAYA_CLIENT (ai_laya_client_get_type())
G_DECLARE_FINAL_TYPE(AiLayaClient, ai_laya_client, AI, LAYA_CLIENT, GObject)
AiLayaClient *
ai_laya_client_new(
	void
);
G_END_DECLS
