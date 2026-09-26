/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-decision.h"
#include "core/ai-error.h"
#include "core/ai-json-util.h"
#include <math.h>

struct _AiDecisionRequest { GObject parent_instance; JsonNode *root; };
struct _AiDecisionResponse { GObject parent_instance; JsonNode *root; gdouble elapsed_ms; };
G_DEFINE_TYPE(AiDecisionRequest, ai_decision_request, G_TYPE_OBJECT)
G_DEFINE_TYPE(AiDecisionResponse, ai_decision_response, G_TYPE_OBJECT)

static void
ai_decision_request_finalize(GObject *object)
{
	g_clear_pointer(&AI_DECISION_REQUEST(object)->root, json_node_unref);
	G_OBJECT_CLASS(ai_decision_request_parent_class)->finalize(object);
}
static void
ai_decision_request_class_init(AiDecisionRequestClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ai_decision_request_finalize;
}
static void ai_decision_request_init(AiDecisionRequest *self) { (void)self; }
static void
ai_decision_response_finalize(GObject *object)
{
	g_clear_pointer(&AI_DECISION_RESPONSE(object)->root, json_node_unref);
	G_OBJECT_CLASS(ai_decision_response_parent_class)->finalize(object);
}
static void
ai_decision_response_class_init(AiDecisionResponseClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = ai_decision_response_finalize;
}
static void ai_decision_response_init(AiDecisionResponse *self) { (void)self; }

static gboolean
valid_text(const gchar *text)
{
	return text != NULL && *text != '\0' && g_utf8_validate(text, -1, NULL);
}

static gboolean
invalid(GError **error, AiError code, const gchar *message)
{
	g_set_error_literal(error, AI_ERROR, code, message);
	return FALSE;
}

static gboolean
number(JsonObject *object, const gchar *key, gdouble max, gdouble *out)
{
	JsonNode *node = ai_json_member_of_type(object, key, JSON_NODE_VALUE);
	GType type;
	gdouble value;
	if (node == NULL) return FALSE;
	type = json_node_get_value_type(node);
	if (type != G_TYPE_DOUBLE && type != G_TYPE_INT64 && type != G_TYPE_INT) return FALSE;
	value = json_node_get_double(node);
	if (!isfinite(value) || value < 0 || value > max) return FALSE;
	if (out != NULL) *out = value;
	return TRUE;
}

static gboolean
validate_request(JsonObject *root, GError **error)
{
	JsonObject *questions = ai_json_get_object(root, "questions");
	g_autoptr(GList) names = NULL;
	GList *iter;
	const gchar *state = ai_json_get_string(root, "state", NULL);
	guint total = 0;
	if (!valid_text(state) || g_utf8_strlen(state, -1) > 50000 || questions == NULL ||
		json_object_get_size(questions) == 0 || json_object_get_size(questions) > 64)
		return invalid(error, AI_ERROR_INVALID_REQUEST, "A decision needs text (up to 50000 characters) and 1 to 64 questions");
	if (json_object_has_member(root, "model") && !valid_text(ai_json_get_string(root, "model", NULL)))
		return invalid(error, AI_ERROR_INVALID_REQUEST, "model must be a nonempty string");
	names = json_object_get_members(questions);
	for (iter = names; iter != NULL; iter = iter->next)
	{
		JsonObject *q = ai_json_get_object(questions, iter->data);
		const gchar *kind = ai_json_get_string(q, "type", "");
		JsonObject *choices = ai_json_get_object(q, "criteria");
		JsonArray *levels = ai_json_get_array(q, "criteria");
		guint count = 0, i;
		if (!valid_text(iter->data) || !valid_text(ai_json_get_string(q, "instructions", NULL)))
			return invalid(error, AI_ERROR_INVALID_REQUEST, "Questions need an identifier and instructions");
		if (g_str_equal(kind, "noul"))
		{
			if (json_object_has_member(q, "criteria"))
				return invalid(error, AI_ERROR_INVALID_REQUEST, "Boolean questions do not take criteria");
			count = 2;
		}
		else if (g_str_equal(kind, "choice") && choices != NULL)
		{
			g_autoptr(GList) labels = json_object_get_members(choices);
			GList *label;
			count = json_object_get_size(choices);
			if (count == 0 || count > 100) goto bad_criteria;
			for (label = labels; label != NULL; label = label->next)
				if (!valid_text(label->data) || !valid_text(ai_json_get_string(choices, label->data, NULL))) goto bad_criteria;
		}
		else if (g_str_equal(kind, "score") && levels != NULL)
		{
			count = json_array_get_length(levels);
			if (count < 2 || count > 32) goto bad_criteria;
			for (i = 0; i < count; i++)
				if (!valid_text(ai_json_array_get_string(levels, i, NULL))) goto bad_criteria;
		}
		else goto bad_criteria;
		total += count;
	}
	if (total > 512) return invalid(error, AI_ERROR_INVALID_REQUEST, "Too many total decision criteria (maximum 512)");
	return TRUE;
bad_criteria:
	return invalid(error, AI_ERROR_INVALID_REQUEST, "Expected noul, choice with labelled criteria, or score with 2 to 32 ordered levels");
}

/**
 * ai_decision_request_new:
 * @text: input text to classify
 *
 * Creates a request. Add questions before submitting it.
 * Returns: (transfer full): a new request
 */
AiDecisionRequest *
ai_decision_request_new(const gchar *text)
{
	g_autoptr(AiDecisionRequest) self = NULL;
	JsonObject *root;
	g_return_val_if_fail(text != NULL, NULL);
	self = g_object_new(AI_TYPE_DECISION_REQUEST, NULL);
	root = json_object_new();
	json_object_set_string_member(root, "state", text);
	json_object_set_object_member(root, "questions", json_object_new());
	self->root = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(self->root, root);
	return g_steal_pointer(&self);
}

/**
 * ai_decision_request_new_from_json:
 * @json: request with state text and typed questions
 * @error: return location for an error
 * Returns: (transfer full) (nullable): a validated request
 */
AiDecisionRequest *
ai_decision_request_new_from_json(const gchar *json, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(AiDecisionRequest) self = NULL;
	g_return_val_if_fail(json != NULL, NULL);
	if (!g_utf8_validate(json, -1, NULL) || !json_parser_load_from_data(parser, json, -1, NULL))
	{
		invalid(error, AI_ERROR_INVALID_REQUEST, "Invalid decision request JSON");
		return NULL;
	}
	if (!validate_request(ai_json_root_object(parser), error)) return NULL;
	self = g_object_new(AI_TYPE_DECISION_REQUEST, NULL);
	self->root = json_node_copy(json_parser_get_root(parser));
	return g_steal_pointer(&self);
}

static gboolean
add_question(AiDecisionRequest *self, const gchar *id, const gchar *question,
	const gchar *kind, JsonNode *criteria, GError **error)
{
	JsonObject *questions;
	g_autoptr(JsonObject) q = json_object_new();
	g_autoptr(JsonObject) candidate = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_return_val_if_fail(AI_IS_DECISION_REQUEST(self), FALSE);
	if (!valid_text(id) || !valid_text(question))
		return invalid(error, AI_ERROR_INVALID_REQUEST, "Question identifier and instructions must not be empty");
	questions = ai_json_get_object(json_node_get_object(self->root), "questions");
	if (json_object_has_member(questions, id))
		return invalid(error, AI_ERROR_INVALID_REQUEST, "Duplicate question identifier");
	json_object_set_string_member(q, "type", kind);
	json_object_set_string_member(q, "instructions", question);
	if (criteria != NULL) json_object_set_member(q, "criteria", json_node_copy(criteria));
	/* Validate a deep snapshot, so a failed addition leaves the request intact. */
	json = json_to_string(self->root, FALSE);
	json_parser_load_from_data(parser, json, -1, NULL);
	candidate = json_object_ref(ai_json_root_object(parser));
	json_object_set_object_member(ai_json_get_object(candidate, "questions"), id, json_object_ref(q));
	if (!validate_request(candidate, error)) return FALSE;
	json_object_set_object_member(questions, id, g_steal_pointer(&q));
	return TRUE;
}

/**
 * ai_decision_request_add_boolean:
 * @self: a request
 * @id: unique question identifier
 * @question: instructions for a yes/no decision
 * @error: return location for an error
 * Returns: whether the question was added
 */
gboolean
ai_decision_request_add_boolean(AiDecisionRequest *self, const gchar *id, const gchar *question, GError **error)
{
	return add_question(self, id, question, "noul", NULL, error);
}

/**
 * ai_decision_request_add_choice:
 * @self: a request
 * @id: unique question identifier
 * @question: instructions
 * @labels: (array zero-terminated=1): choice labels
 * @descriptions: (array zero-terminated=1): descriptions in matching order
 * @error: return location for an error
 * Returns: whether the question was added
 */
gboolean
ai_decision_request_add_choice(AiDecisionRequest *self, const gchar *id, const gchar *question,
	const gchar *const *labels, const gchar *const *descriptions, GError **error)
{
	g_autoptr(JsonNode) criteria = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();
	guint i;
	json_node_take_object(criteria, object);
	if (labels == NULL || descriptions == NULL || g_strv_length((gchar **)labels) != g_strv_length((gchar **)descriptions))
		return invalid(error, AI_ERROR_INVALID_REQUEST, "Each choice needs a label and description");
	for (i = 0; labels[i] != NULL; i++)
	{
		if (json_object_has_member(object, labels[i]))
			return invalid(error, AI_ERROR_INVALID_REQUEST, "Duplicate choice label");
		json_object_set_string_member(object, labels[i], descriptions[i]);
	}
	return add_question(self, id, question, "choice", criteria, error);
}

/**
 * ai_decision_request_add_score:
 * @self: a request
 * @id: unique question identifier
 * @question: instructions
 * @levels: (array zero-terminated=1): descriptions ordered from level zero
 * @error: return location for an error
 * Returns: whether the question was added
 */
gboolean
ai_decision_request_add_score(AiDecisionRequest *self, const gchar *id, const gchar *question,
	const gchar *const *levels, GError **error)
{
	g_autoptr(JsonNode) criteria = json_node_new(JSON_NODE_ARRAY);
	JsonArray *array = json_array_new();
	guint i;
	json_node_take_array(criteria, array);
	for (i = 0; levels != NULL && levels[i] != NULL; i++) json_array_add_string_element(array, levels[i]);
	return add_question(self, id, question, "score", criteria, error);
}

/**
 * ai_decision_request_dup_json:
 * @self: a request
 * Returns: (transfer full): request JSON
 */
gchar *
ai_decision_request_dup_json(AiDecisionRequest *self)
{
	g_return_val_if_fail(AI_IS_DECISION_REQUEST(self), NULL);
	return json_to_string(self->root, FALSE);
}

static gboolean
validate_answer(JsonObject *q, JsonObject *answer)
{
	const gchar *kind = ai_json_get_string(q, "type", "");
	JsonObject *probabilities = ai_json_get_object(answer, "probabilities");
	JsonObject *choices = ai_json_get_object(q, "criteria");
	JsonArray *levels = ai_json_get_array(q, "criteria");
	g_autoptr(GList) labels = NULL;
	GList *iter;
	guint count, i;
	gdouble sum = 0, value;
	if (g_strcmp0(kind, ai_json_get_string(answer, "type", NULL)) != 0) return FALSE;
	if (g_str_equal(kind, "noul")) return number(answer, "noul", 1, NULL);
	count = choices != NULL ? json_object_get_size(choices) : json_array_get_length(levels);
	if (probabilities == NULL || json_object_get_size(probabilities) != count) return FALSE;
	if (choices != NULL)
	{
		const gchar *selected = ai_json_get_string(answer, "choice", NULL);
		if (selected == NULL || !json_object_has_member(choices, selected)) return FALSE;
		labels = json_object_get_members(choices);
		for (iter = labels; iter != NULL; iter = iter->next)
		{
			if (!number(probabilities, iter->data, 1, &value)) return FALSE;
			sum += value;
		}
	}
	else
	{
		if (!number(answer, "score", count - 1, NULL)) return FALSE;
		for (i = 0; i < count; i++)
		{
			g_autofree gchar *key = g_strdup_printf("%u", i);
			if (!number(probabilities, key, 1, &value)) return FALSE;
			sum += value;
		}
	}
	/* Laya rounds each probability to four decimal places. */
	return fabs(sum - 1.0) <= count * 0.00005 + 0.000001;
}

/**
 * ai_decision_response_new_from_json:
 * @request: the original request, used to validate every answer
 * @json: response JSON
 * @elapsed_ms: elapsed wall time in milliseconds, not inference time
 * @error: return location for an error
 * Returns: (transfer full) (nullable): validated response
 */
AiDecisionResponse *
ai_decision_response_new_from_json(AiDecisionRequest *request, const gchar *json, gdouble elapsed_ms, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(AiDecisionResponse) self = NULL;
	g_autoptr(GList) names = NULL;
	JsonObject *root, *questions, *answers;
	GList *iter;
	g_return_val_if_fail(AI_IS_DECISION_REQUEST(request), NULL);
	if (!validate_request(json_node_get_object(request->root), error)) return NULL;
	if (json == NULL || !g_utf8_validate(json, -1, NULL) || !isfinite(elapsed_ms) || elapsed_ms < 0 || !json_parser_load_from_data(parser, json, -1, NULL)) goto bad;
	root = ai_json_root_object(parser);
	if (!valid_text(ai_json_get_string(root, "model", NULL))) goto bad;
	answers = ai_json_get_object(root, "answers");
	questions = ai_json_get_object(json_node_get_object(request->root), "questions");
	if (answers == NULL || json_object_get_size(answers) != json_object_get_size(questions)) goto bad;
	names = json_object_get_members(questions);
	for (iter = names; iter != NULL; iter = iter->next)
		if (!validate_answer(ai_json_get_object(questions, iter->data), ai_json_get_object(answers, iter->data))) goto bad;
	self = g_object_new(AI_TYPE_DECISION_RESPONSE, NULL);
	self->root = json_node_copy(json_parser_get_root(parser));
	self->elapsed_ms = elapsed_ms;
	return g_steal_pointer(&self);
bad:
	invalid(error, AI_ERROR_INVALID_RESPONSE, "Malformed or incomplete decision response");
	return NULL;
}

static JsonObject *
answer_for(AiDecisionResponse *self, const gchar *id)
{
	return ai_json_get_object(ai_json_get_object(json_node_get_object(self->root), "answers"), id);
}

/**
 * ai_decision_response_get_model:
 * @self: a response
 * Returns: (transfer none): server-reported model identity
 */
const gchar *ai_decision_response_get_model(AiDecisionResponse *self)
{
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), NULL);
	return ai_json_get_string(json_node_get_object(self->root), "model", NULL);
}
/**
 * ai_decision_response_get_checkpoint:
 * @self: a response
 * Returns: (transfer none) (nullable): routed checkpoint name, when reported
 */
const gchar *ai_decision_response_get_checkpoint(AiDecisionResponse *self)
{
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), NULL);
	return ai_json_get_string(ai_json_get_object(json_node_get_object(self->root), "routing"), "model", NULL);
}
/**
 * ai_decision_response_get_choice:
 * @self: a response
 * @id: question identifier
 * Returns: (transfer none) (nullable): selected label, or %NULL for other kinds
 */
const gchar *ai_decision_response_get_choice(AiDecisionResponse *self, const gchar *id)
{
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), NULL);
	return ai_json_get_string(answer_for(self, id), "choice", NULL);
}
/**
 * ai_decision_response_get_probability:
 * @self: a response
 * @id: question identifier
 * @label: (nullable): choice label or score index string; %NULL for boolean P(true)
 * Returns: the original probability, or -1 if not available
 */
gdouble ai_decision_response_get_probability(AiDecisionResponse *self, const gchar *id, const gchar *label)
{
	JsonObject *answer;
	gdouble value;
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), -1);
	answer = answer_for(self, id);
	return number(label != NULL ? ai_json_get_object(answer, "probabilities") : answer,
		label != NULL ? label : "noul", 1, &value) ? value : -1;
}
/**
 * ai_decision_response_get_score:
 * @self: a response
 * @id: question identifier
 * Returns: expected ordinal score, or -1 if not available
 */
gdouble ai_decision_response_get_score(AiDecisionResponse *self, const gchar *id)
{
	gdouble value;
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), -1);
	return number(answer_for(self, id), "score", G_MAXDOUBLE, &value) ? value : -1;
}
/**
 * ai_decision_response_get_elapsed_ms:
 * @self: a response
 * Returns: whole request wall time, in milliseconds
 */
gdouble ai_decision_response_get_elapsed_ms(AiDecisionResponse *self)
{
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), 0);
	return self->elapsed_ms;
}
/**
 * ai_decision_response_dup_json:
 * @self: a response
 * Returns: (transfer full): original response JSON, including provider metadata
 */
gchar *ai_decision_response_dup_json(AiDecisionResponse *self)
{
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), NULL);
	return json_to_string(self->root, FALSE);
}
/**
 * ai_decision_response_format:
 * @self: a response
 * Returns: (transfer full): readable answers and distributions
 */
gchar *ai_decision_response_format(AiDecisionResponse *self)
{
	g_autoptr(GString) text = g_string_new(NULL);
	g_autoptr(GList) ids = NULL;
	GList *iter;
	JsonObject *answers;
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(self), NULL);
	answers = ai_json_get_object(json_node_get_object(self->root), "answers");
	ids = json_object_get_members(answers);
	for (iter = ids; iter != NULL; iter = iter->next)
	{
		JsonObject *answer = ai_json_get_object(answers, iter->data);
		JsonObject *probabilities = ai_json_get_object(answer, "probabilities");
		const gchar *choice = ai_json_get_string(answer, "choice", NULL);
		gdouble value;
		if (choice != NULL) g_string_append_printf(text, "%s: %s\n", (gchar *)iter->data, choice);
		else
		{
			number(answer, g_str_equal(ai_json_get_string(answer, "type", ""), "noul") ? "noul" : "score", G_MAXDOUBLE, &value);
			g_string_append_printf(text, "%s: %.4f\n", (gchar *)iter->data, value);
		}
		if (probabilities != NULL)
		{
			g_autoptr(GList) labels = json_object_get_members(probabilities);
			GList *label;
			for (label = labels; label != NULL; label = label->next)
			{
				number(probabilities, label->data, 1, &value);
				g_string_append_printf(text, "  %s: %.4f\n", (gchar *)label->data, value);
			}
		}
	}
	g_string_append_printf(text, "Model: %s\nElapsed: %.2f ms\n", ai_decision_response_get_model(self), self->elapsed_ms);
	return g_string_free(g_steal_pointer(&text), FALSE);
}
