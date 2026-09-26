/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <glib-object.h>
G_BEGIN_DECLS
#define AI_TYPE_DECISION_REQUEST (ai_decision_request_get_type())
G_DECLARE_FINAL_TYPE(AiDecisionRequest, ai_decision_request, AI, DECISION_REQUEST, GObject)
#define AI_TYPE_DECISION_RESPONSE (ai_decision_response_get_type())
G_DECLARE_FINAL_TYPE(AiDecisionResponse, ai_decision_response, AI, DECISION_RESPONSE, GObject)
AiDecisionRequest *
ai_decision_request_new(
	const gchar *text
);
AiDecisionRequest *
ai_decision_request_new_from_json(
	const gchar *json,
	GError **error
);
gboolean
ai_decision_request_add_boolean(
	AiDecisionRequest *self,
	const gchar *id,
	const gchar *question,
	GError **error
);
gboolean
ai_decision_request_add_choice(
	AiDecisionRequest *self,
	const gchar *id,
	const gchar *question,
	const gchar *const *labels,
	const gchar *const *descriptions,
	GError **error
);
gboolean
ai_decision_request_add_score(
	AiDecisionRequest *self,
	const gchar *id,
	const gchar *question,
	const gchar *const *levels,
	GError **error
);
gchar *
ai_decision_request_dup_json(
	AiDecisionRequest *self
);
AiDecisionResponse *
ai_decision_response_new_from_json(
	AiDecisionRequest *request,
	const gchar *json,
	gdouble elapsed_ms,
	GError **error
);
const gchar *
ai_decision_response_get_model(
	AiDecisionResponse *self
);
const gchar *
ai_decision_response_get_checkpoint(
	AiDecisionResponse *self
);
const gchar *
ai_decision_response_get_choice(
	AiDecisionResponse *self,
	const gchar *id
);
gdouble
ai_decision_response_get_probability(
	AiDecisionResponse *self,
	const gchar *id,
	const gchar *label
);
gdouble
ai_decision_response_get_score(
	AiDecisionResponse *self,
	const gchar *id
);
gdouble
ai_decision_response_get_elapsed_ms(
	AiDecisionResponse *self
);
gchar *
ai_decision_response_dup_json(
	AiDecisionResponse *self
);
gchar *
ai_decision_response_format(
	AiDecisionResponse *self
);
G_END_DECLS
