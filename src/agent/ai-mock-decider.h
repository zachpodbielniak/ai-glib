/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "core/ai-decider.h"
G_BEGIN_DECLS
#define AI_TYPE_MOCK_DECIDER (ai_mock_decider_get_type())
G_DECLARE_FINAL_TYPE(AiMockDecider, ai_mock_decider, AI, MOCK_DECIDER, GObject)
AiMockDecider *
ai_mock_decider_new(
	const gchar *response_json
);
void
ai_mock_decider_set_error(
	AiMockDecider *self,
	const GError *error
);
G_END_DECLS
