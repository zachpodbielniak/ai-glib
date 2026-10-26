/*
 * ai-benchmark-case.c - One workload and what a correct answer looks like
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A case is the input (a prompt, a decision request, some texts) plus an
 * optional expectation. The score functions are pure -- no I/O, no main
 * loop -- so every scoring rule is testable without a provider.
 *
 * Scoring rules, which docs/benchmark.org states for a reader:
 *
 *  - Chat: `contains` is a case-insensitive substring match, `equals`
 *    compares after trimming surrounding whitespace, `regex` is a GRegex
 *    search (use `(?i)` for case-insensitivity).
 *  - Noul: the answer is "yes" when P(true) >= the threshold (0.5 by
 *    default). Brier is (p - y)^2.
 *  - Choice and score: the answer is the selected option (choice) or the
 *    most probable level (score). Brier is half the multi-class Brier,
 *    sum_k (p_k - y_k)^2 / 2, which equals the binary formula for two
 *    classes and so stays in 0..1 beside a Noul.
 *  - Log-loss is -ln(p of the labelled answer), with p clamped to 1e-15
 *    so a confident miss costs ~34.5 rather than infinity.
 *  - Box: intersection over union with the labelled box, passing at 0.5
 *    by default. Boxes carry no distribution, so no Brier.
 *  - Embedding: passes when cos(similar pair) > cos(dissimilar pair).
 *
 * A case passes only when every expectation passes.
 */

#include "bench/ai-benchmark-case.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

#include "core/ai-error.h"
#include "core/ai-json-util.h"

#define LOG_LOSS_FLOOR (1e-15)

typedef enum
{
	MATCH_NONE = 0,
	MATCH_CONTAINS,
	MATCH_EQUALS,
	MATCH_REGEX
} MatchMode;

typedef struct
{
	gchar   *question_id;
	gchar   *type;        /* noul, choice, score or box */
	gchar   *expected;    /* as given, for messages */
	gboolean truth;       /* noul */
	gchar   *label;       /* choice label, or score index as a string */
	gdouble  box[4];      /* top, bottom, left, right */
} Expectation;

struct _AiBenchmarkCase
{
	GObject            parent_instance;
	gchar             *id;
	AiBenchmarkKind    kind;

	/* chat and image */
	gchar             *prompt;
	gchar             *system;
	gint               max_tokens;
	MatchMode          match;
	gchar             *match_text;
	GRegex            *match_regex;

	/* decision */
	AiDecisionRequest *request;
	JsonNode          *questions;   /* copy of the request's questions */
	GPtrArray         *expectations;
	gdouble            noul_threshold;
	gdouble            box_iou;

	/* embedding */
	gchar            **texts;
	gboolean           has_similarity;
	guint              similarity[4];

	/* image */
	AiImageSize        image_size;
	gint               image_count;
};

G_DEFINE_TYPE(AiBenchmarkCase, ai_benchmark_case, G_TYPE_OBJECT)

static void
expectation_free(
	gpointer data
){
	Expectation *e;

	e = (Expectation *)data;
	g_free(e->question_id);
	g_free(e->type);
	g_free(e->expected);
	g_free(e->label);
	g_free(e);
}

static void
ai_benchmark_case_finalize(
	GObject *object
){
	AiBenchmarkCase *self;

	self = AI_BENCHMARK_CASE(object);
	g_free(self->id);
	g_free(self->prompt);
	g_free(self->system);
	g_free(self->match_text);
	g_clear_pointer(&self->match_regex, g_regex_unref);
	g_clear_object(&self->request);
	g_clear_pointer(&self->questions, json_node_unref);
	g_ptr_array_unref(self->expectations);
	g_strfreev(self->texts);
	G_OBJECT_CLASS(ai_benchmark_case_parent_class)->finalize(object);
}

static void
ai_benchmark_case_class_init(
	AiBenchmarkCaseClass *klass
){
	G_OBJECT_CLASS(klass)->finalize = ai_benchmark_case_finalize;
}

static void
ai_benchmark_case_init(
	AiBenchmarkCase *self
){
	self->max_tokens = 1024;
	self->expectations = g_ptr_array_new_with_free_func(expectation_free);
	self->noul_threshold = 0.5;
	self->box_iou = 0.5;
	self->image_size = AI_IMAGE_SIZE_AUTO;
	self->image_count = 1;
}

static AiBenchmarkCase *
case_new(
	const gchar     *id,
	AiBenchmarkKind  kind
){
	AiBenchmarkCase *self;

	self = (AiBenchmarkCase *)g_object_new(AI_TYPE_BENCHMARK_CASE, NULL);
	self->id = g_strdup(id);
	self->kind = kind;
	return self;
}

/**
 * ai_benchmark_case_new_chat:
 * @id: identifier, unique within a benchmark
 * @prompt: the user message sent each run
 *
 * Returns: (transfer full): a chat case with no expectation
 */
AiBenchmarkCase *
ai_benchmark_case_new_chat(
	const gchar *id,
	const gchar *prompt
){
	AiBenchmarkCase *self;

	g_return_val_if_fail(id != NULL && *id != '\0', NULL);
	g_return_val_if_fail(prompt != NULL, NULL);

	self = case_new(id, AI_BENCHMARK_KIND_CHAT);
	self->prompt = g_strdup(prompt);
	return self;
}

/**
 * ai_benchmark_case_new_decision:
 * @id: identifier, unique within a benchmark
 * @request: the typed questions; the case keeps its own copy
 *
 * Returns: (transfer full): a decision case with no expectation
 */
AiBenchmarkCase *
ai_benchmark_case_new_decision(
	const gchar       *id,
	AiDecisionRequest *request
){
	g_autoptr(AiBenchmarkCase) self = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *json = NULL;
	JsonObject *root;
	JsonObject *questions;

	g_return_val_if_fail(id != NULL && *id != '\0', NULL);
	g_return_val_if_fail(AI_IS_DECISION_REQUEST(request), NULL);

	self = case_new(id, AI_BENCHMARK_KIND_DECISION);

	/*
	 * Snapshot the request, as ai_decider_decide_async() does, so a
	 * question the caller adds later cannot change what was benchmarked.
	 * Keep the question table to check expectations against.
	 */
	json = ai_decision_request_dup_json(request);
	self->request = ai_decision_request_new_from_json(json, NULL);
	g_return_val_if_fail(self->request != NULL, NULL);

	parser = json_parser_new();
	if (json_parser_load_from_data(parser, json, -1, NULL))
	{
		root = ai_json_root_object(parser);
		questions = ai_json_get_object(root, "questions");
		if (questions != NULL)
		{
			self->questions = json_node_new(JSON_NODE_OBJECT);
			json_node_set_object(self->questions, questions);
		}
	}
	return (AiBenchmarkCase *)g_steal_pointer(&self);
}

/**
 * ai_benchmark_case_new_embedding:
 * @id: identifier, unique within a benchmark
 * @texts: (array zero-terminated=1): one or more texts, embedded as one batch
 *
 * Returns: (transfer full): an embedding case with no expectation
 */
AiBenchmarkCase *
ai_benchmark_case_new_embedding(
	const gchar        *id,
	const gchar *const *texts
){
	AiBenchmarkCase *self;

	g_return_val_if_fail(id != NULL && *id != '\0', NULL);
	g_return_val_if_fail(texts != NULL && texts[0] != NULL, NULL);

	self = case_new(id, AI_BENCHMARK_KIND_EMBEDDING);
	self->texts = g_strdupv((gchar **)texts);
	return self;
}

/**
 * ai_benchmark_case_new_image:
 * @id: identifier, unique within a benchmark
 * @prompt: the image prompt
 *
 * Image cases are never scored: there is no label to compare an image
 * against. They measure latency and whether the call succeeds.
 *
 * Returns: (transfer full): an image case
 */
AiBenchmarkCase *
ai_benchmark_case_new_image(
	const gchar *id,
	const gchar *prompt
){
	AiBenchmarkCase *self;

	g_return_val_if_fail(id != NULL && *id != '\0', NULL);
	g_return_val_if_fail(prompt != NULL, NULL);

	self = case_new(id, AI_BENCHMARK_KIND_IMAGE);
	self->prompt = g_strdup(prompt);
	return self;
}

/**
 * ai_benchmark_case_get_id:
 * @self: a case
 *
 * Returns: (transfer none): the identifier
 */
const gchar *
ai_benchmark_case_get_id(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), NULL);
	return self->id;
}

/**
 * ai_benchmark_case_get_kind:
 * @self: a case
 *
 * Returns: the kind of target this case runs against
 */
AiBenchmarkKind
ai_benchmark_case_get_kind(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), AI_BENCHMARK_KIND_CHAT);
	return self->kind;
}

/**
 * ai_benchmark_case_get_prompt:
 * @self: a case
 *
 * Returns: (transfer none) (nullable): the chat or image prompt
 */
const gchar *
ai_benchmark_case_get_prompt(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), NULL);
	return self->prompt;
}

/**
 * ai_benchmark_case_get_request:
 * @self: a case
 *
 * Returns: (transfer none) (nullable): the decision request
 */
AiDecisionRequest *
ai_benchmark_case_get_request(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), NULL);
	return self->request;
}

/**
 * ai_benchmark_case_get_texts:
 * @self: a case
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable): the
 *   embedding inputs
 */
const gchar * const *
ai_benchmark_case_get_texts(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), NULL);
	return (const gchar * const *)self->texts;
}

/**
 * ai_benchmark_case_set_system:
 * @self: a chat case
 * @system_prompt: (nullable): the system prompt, or %NULL for none
 */
void
ai_benchmark_case_set_system(
	AiBenchmarkCase *self,
	const gchar     *system_prompt
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	g_free(self->system);
	self->system = g_strdup(system_prompt);
}

/**
 * ai_benchmark_case_get_system:
 * @self: a case
 *
 * Returns: (transfer none) (nullable): the system prompt
 */
const gchar *
ai_benchmark_case_get_system(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), NULL);
	return self->system;
}

/**
 * ai_benchmark_case_set_max_tokens:
 * @self: a chat case
 * @max_tokens: the output cap passed to the provider (default 1024)
 */
void
ai_benchmark_case_set_max_tokens(
	AiBenchmarkCase *self,
	gint             max_tokens
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	g_return_if_fail(max_tokens > 0);
	self->max_tokens = max_tokens;
}

/**
 * ai_benchmark_case_get_max_tokens:
 * @self: a case
 *
 * Returns: the output cap
 */
gint
ai_benchmark_case_get_max_tokens(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), 0);
	return self->max_tokens;
}

static void
set_match(
	AiBenchmarkCase *self,
	MatchMode        mode,
	const gchar     *text,
	GRegex          *regex
){
	g_free(self->match_text);
	g_clear_pointer(&self->match_regex, g_regex_unref);
	self->match = mode;
	self->match_text = g_strdup(text);
	self->match_regex = regex;
}

/**
 * ai_benchmark_case_expect_contains:
 * @self: a chat case
 * @text: text the reply must contain, compared case-insensitively
 *
 * Replaces any earlier chat expectation.
 */
void
ai_benchmark_case_expect_contains(
	AiBenchmarkCase *self,
	const gchar     *text
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	g_return_if_fail(text != NULL);
	set_match(self, MATCH_CONTAINS, text, NULL);
}

/**
 * ai_benchmark_case_expect_equals:
 * @self: a chat case
 * @text: the whole reply, compared after trimming surrounding whitespace
 *
 * Replaces any earlier chat expectation.
 */
void
ai_benchmark_case_expect_equals(
	AiBenchmarkCase *self,
	const gchar     *text
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	g_return_if_fail(text != NULL);
	set_match(self, MATCH_EQUALS, text, NULL);
}

/**
 * ai_benchmark_case_expect_regex:
 * @self: a chat case
 * @pattern: a GRegex pattern the reply must match somewhere
 * @error: return location for an error
 *
 * Replaces any earlier chat expectation.
 *
 * Returns: %FALSE when @pattern does not compile
 */
gboolean
ai_benchmark_case_expect_regex(
	AiBenchmarkCase  *self,
	const gchar      *pattern,
	GError          **error
){
	GRegex *regex;
	g_autoptr(GError) local = NULL;

	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), FALSE);
	g_return_val_if_fail(pattern != NULL, FALSE);

	regex = g_regex_new(pattern, G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, &local);
	if (regex == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "case '%s': invalid regex '%s': %s", self->id, pattern, local->message);
		return FALSE;
	}
	set_match(self, MATCH_REGEX, pattern, regex);
	return TRUE;
}

/*
 * Parses "top,bottom,left,right" into four fractions of the image.
 */
static gboolean
parse_box(
	const gchar *text,
	gdouble      box[4]
){
	g_auto(GStrv) parts = NULL;
	guint i;

	parts = g_strsplit(text, ",", -1);
	if (g_strv_length(parts) != 4)
		return FALSE;
	for (i = 0; i < 4; i++)
	{
		gchar *end;

		g_strstrip(parts[i]);
		end = NULL;
		box[i] = g_ascii_strtod(parts[i], &end);
		if (end == parts[i] || *end != '\0' || !isfinite(box[i]) || box[i] < 0 || box[i] > 1)
			return FALSE;
	}
	return box[0] <= box[1] && box[2] <= box[3];
}

/**
 * ai_benchmark_case_expect_answer:
 * @self: a decision case
 * @question_id: a question in the request
 * @expected: the labelled answer: `true`/`false` (or `yes`/`no`) for a
 *   Noul, an option name for a choice, a level name or zero-based index
 *   for a score, `top,bottom,left,right` fractions for a box
 * @error: return location for an error
 *
 * Checked against the request now, so a typo in a label is an error when
 * the suite loads rather than a wall of failed runs.
 *
 * Returns: %TRUE when the expectation fits the question
 */
gboolean
ai_benchmark_case_expect_answer(
	AiBenchmarkCase  *self,
	const gchar      *question_id,
	const gchar      *expected,
	GError          **error
){
	Expectation *e;
	JsonObject *question;
	const gchar *type;

	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), FALSE);
	g_return_val_if_fail(question_id != NULL, FALSE);
	g_return_val_if_fail(expected != NULL, FALSE);

	if (self->kind != AI_BENCHMARK_KIND_DECISION || self->questions == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "case '%s': answers can only be expected of a decision case", self->id);
		return FALSE;
	}

	question = ai_json_get_object(json_node_get_object(self->questions), question_id);
	if (question == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "case '%s': expectation names question '%s', which the request does not ask",
		            self->id, question_id);
		return FALSE;
	}

	type = ai_json_get_string(question, "type", "");
	e = g_new0(Expectation, 1);
	e->question_id = g_strdup(question_id);
	e->type = g_strdup(type);
	e->expected = g_strdup(expected);

	/* Interpret @expected by the question's type. */
	if (g_str_equal(type, "noul"))
	{
		if (g_ascii_strcasecmp(expected, "true") == 0 || g_ascii_strcasecmp(expected, "yes") == 0)
			e->truth = TRUE;
		else if (g_ascii_strcasecmp(expected, "false") == 0 || g_ascii_strcasecmp(expected, "no") == 0)
			e->truth = FALSE;
		else
			goto bad;
	}
	else if (g_str_equal(type, "choice"))
	{
		JsonObject *criteria = ai_json_get_object(question, "criteria");

		if (criteria == NULL || ai_json_get_node(criteria, expected) == NULL)
			goto bad;
		e->label = g_strdup(expected);
	}
	else if (g_str_equal(type, "score"))
	{
		JsonArray *levels = ai_json_get_array(question, "criteria");
		guint count;
		guint i;
		gint found;

		count = levels != NULL ? json_array_get_length(levels) : 0;
		found = -1;
		/* A level's name wins; otherwise a zero-based index. */
		for (i = 0; i < count && found < 0; i++)
			if (g_strcmp0(ai_json_array_get_string(levels, i, NULL), expected) == 0)
				found = (gint)i;
		if (found < 0)
		{
			gchar *end = NULL;
			gint64 index = g_ascii_strtoll(expected, &end, 10);

			if (end != expected && *end == '\0' && index >= 0 && index < (gint64)count)
				found = (gint)index;
		}
		if (found < 0)
			goto bad;
		e->label = g_strdup_printf("%d", found);
	}
	else if (g_str_equal(type, "box"))
	{
		if (!parse_box(expected, e->box))
			goto bad;
	}
	else
		goto bad;

	g_ptr_array_add(self->expectations, e);
	return TRUE;

bad:
	g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
	            "case '%s': '%s' is not a valid answer for %s question '%s'",
	            self->id, expected, *type != '\0' ? type : "this", question_id);
	expectation_free(e);
	return FALSE;
}

/**
 * ai_benchmark_case_set_noul_threshold:
 * @self: a decision case
 * @threshold: P(true) at or above which a Noul counts as "yes" (default 0.5)
 */
void
ai_benchmark_case_set_noul_threshold(
	AiBenchmarkCase *self,
	gdouble          threshold
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	g_return_if_fail(threshold >= 0 && threshold <= 1);
	self->noul_threshold = threshold;
}

/**
 * ai_benchmark_case_get_noul_threshold:
 * @self: a case
 *
 * Returns: the Noul "yes" threshold
 */
gdouble
ai_benchmark_case_get_noul_threshold(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), 0.5);
	return self->noul_threshold;
}

/**
 * ai_benchmark_case_set_box_iou:
 * @self: a decision case
 * @iou: intersection over union at or above which a box passes (default 0.5)
 */
void
ai_benchmark_case_set_box_iou(
	AiBenchmarkCase *self,
	gdouble          iou
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	g_return_if_fail(iou >= 0 && iou <= 1);
	self->box_iou = iou;
}

/**
 * ai_benchmark_case_expect_similarity:
 * @self: an embedding case
 * @similar_a: index of a text
 * @similar_b: index of a text that should be close to @similar_a
 * @dissimilar_a: index of a text
 * @dissimilar_b: index of a text that should be further from @dissimilar_a
 * @error: return location for an error
 *
 * Passes when cos(@similar_a, @similar_b) > cos(@dissimilar_a, @dissimilar_b).
 * A ranking, not a threshold: cosine scales differ between models, and an
 * absolute cut-off tuned on one would mean nothing on the next.
 *
 * Returns: %FALSE when an index is out of range
 */
gboolean
ai_benchmark_case_expect_similarity(
	AiBenchmarkCase  *self,
	guint             similar_a,
	guint             similar_b,
	guint             dissimilar_a,
	guint             dissimilar_b,
	GError          **error
){
	guint n;

	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), FALSE);

	n = self->texts != NULL ? g_strv_length(self->texts) : 0;
	if (self->kind != AI_BENCHMARK_KIND_EMBEDDING ||
	    similar_a >= n || similar_b >= n || dissimilar_a >= n || dissimilar_b >= n)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "case '%s': similarity indices must name texts 0 to %d of an embedding case",
		            self->id, (gint)n - 1);
		return FALSE;
	}
	self->has_similarity = TRUE;
	self->similarity[0] = similar_a;
	self->similarity[1] = similar_b;
	self->similarity[2] = dissimilar_a;
	self->similarity[3] = dissimilar_b;
	return TRUE;
}

/**
 * ai_benchmark_case_set_image_size:
 * @self: an image case
 * @size: requested size
 */
void
ai_benchmark_case_set_image_size(
	AiBenchmarkCase *self,
	AiImageSize      size
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	self->image_size = size;
}

/**
 * ai_benchmark_case_get_image_size:
 * @self: a case
 *
 * Returns: the requested image size
 */
AiImageSize
ai_benchmark_case_get_image_size(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), AI_IMAGE_SIZE_AUTO);
	return self->image_size;
}

/**
 * ai_benchmark_case_set_image_count:
 * @self: an image case
 * @count: images per call (default 1)
 */
void
ai_benchmark_case_set_image_count(
	AiBenchmarkCase *self,
	gint             count
){
	g_return_if_fail(AI_IS_BENCHMARK_CASE(self));
	g_return_if_fail(count > 0);
	self->image_count = count;
}

/**
 * ai_benchmark_case_get_image_count:
 * @self: a case
 *
 * Returns: images per call
 */
gint
ai_benchmark_case_get_image_count(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), 1);
	return self->image_count;
}

/**
 * ai_benchmark_case_has_expectation:
 * @self: a case
 *
 * Returns: whether a successful run of this case is scored
 */
gboolean
ai_benchmark_case_has_expectation(
	AiBenchmarkCase *self
){
	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), FALSE);
	switch (self->kind)
	{
		case AI_BENCHMARK_KIND_CHAT:
			return self->match != MATCH_NONE;
		case AI_BENCHMARK_KIND_DECISION:
			return self->expectations->len > 0;
		case AI_BENCHMARK_KIND_EMBEDDING:
			return self->has_similarity;
		case AI_BENCHMARK_KIND_IMAGE:
		default:
			return FALSE;
	}
}

/**
 * ai_benchmark_case_score_text:
 * @self: a chat case
 * @text: (nullable): the reply text
 * @detail: (out) (optional) (transfer full) (nullable): why it failed
 *
 * Returns: %AI_BENCHMARK_OUTCOME_UNSCORED with no expectation, otherwise
 *   pass or fail
 */
AiBenchmarkOutcome
ai_benchmark_case_score_text(
	AiBenchmarkCase  *self,
	const gchar      *text,
	gchar           **detail
){
	gboolean ok;

	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), AI_BENCHMARK_OUTCOME_UNSCORED);

	if (detail != NULL)
		*detail = NULL;
	if (self->match == MATCH_NONE)
		return AI_BENCHMARK_OUTCOME_UNSCORED;
	if (text == NULL)
		text = "";

	ok = FALSE;
	switch (self->match)
	{
		case MATCH_CONTAINS:
		{
			g_autofree gchar *hay = g_utf8_casefold(text, -1);
			g_autofree gchar *needle = g_utf8_casefold(self->match_text, -1);

			ok = strstr(hay, needle) != NULL;
			break;
		}
		case MATCH_EQUALS:
		{
			g_autofree gchar *got = g_strstrip(g_strdup(text));
			g_autofree gchar *want = g_strstrip(g_strdup(self->match_text));

			ok = g_str_equal(got, want);
			break;
		}
		case MATCH_REGEX:
			ok = g_regex_match(self->match_regex, text, G_REGEX_MATCH_DEFAULT, NULL);
			break;
		case MATCH_NONE:
		default:
			break;
	}

	if (!ok && detail != NULL)
		*detail = g_strdup_printf("expected a reply %s \"%s\"",
		                          self->match == MATCH_CONTAINS ? "containing"
		                          : self->match == MATCH_EQUALS ? "equal to" : "matching",
		                          self->match_text);
	return ok ? AI_BENCHMARK_OUTCOME_PASS : AI_BENCHMARK_OUTCOME_FAIL;
}

/*
 * Brier (half multi-class) and log-loss for a labelled distribution over
 * @labels, read from @response. Returns FALSE when the response carries
 * no usable distribution for this question.
 */
static gboolean
distribution_scores(
	AiDecisionResponse *response,
	const gchar        *question_id,
	GPtrArray          *labels,
	const gchar        *expected,
	gdouble            *brier,
	gdouble            *log_loss
){
	gdouble sum;
	gdouble p_expected;
	guint i;

	sum = 0;
	p_expected = -1;
	for (i = 0; i < labels->len; i++)
	{
		const gchar *label = (const gchar *)g_ptr_array_index(labels, i);
		gdouble p = ai_decision_response_get_probability(response, question_id, label);
		gdouble y = g_str_equal(label, expected) ? 1.0 : 0.0;

		if (p < 0)
			return FALSE;
		sum += (p - y) * (p - y);
		if (y > 0)
			p_expected = p;
	}
	if (p_expected < 0)
		return FALSE;
	*brier = sum / 2.0;
	*log_loss = -log(MAX(p_expected, LOG_LOSS_FLOOR));
	return TRUE;
}

/*
 * The labels a choice or score question's distribution is keyed by:
 * option names for a choice, "0".."n-1" for a score.
 */
static GPtrArray *
question_labels(
	JsonObject  *question,
	const gchar *type
){
	GPtrArray *labels;

	labels = g_ptr_array_new_with_free_func(g_free);
	if (g_str_equal(type, "choice"))
	{
		JsonObject *criteria = ai_json_get_object(question, "criteria");
		g_autoptr(GList) names = criteria != NULL ? json_object_get_members(criteria) : NULL;
		GList *iter;

		for (iter = names; iter != NULL; iter = iter->next)
			g_ptr_array_add(labels, g_strdup((const gchar *)iter->data));
	}
	else
	{
		JsonArray *levels = ai_json_get_array(question, "criteria");
		guint count = levels != NULL ? json_array_get_length(levels) : 0;
		guint i;

		for (i = 0; i < count; i++)
			g_ptr_array_add(labels, g_strdup_printf("%u", i));
	}
	return labels;
}

static gdouble
box_iou(
	const gdouble a[4],
	const gdouble b[4]
){
	gdouble top;
	gdouble bottom;
	gdouble left;
	gdouble right;
	gdouble inter;
	gdouble uni;

	top = MAX(a[0], b[0]);
	bottom = MIN(a[1], b[1]);
	left = MAX(a[2], b[2]);
	right = MIN(a[3], b[3]);
	inter = (bottom > top && right > left) ? (bottom - top) * (right - left) : 0;
	uni = (a[1] - a[0]) * (a[3] - a[2]) + (b[1] - b[0]) * (b[3] - b[2]) - inter;
	return uni > 0 ? inter / uni : 0;
}

/**
 * ai_benchmark_case_score_decision:
 * @self: a decision case
 * @response: the provider's validated answer
 * @brier: (out) (optional): mean Brier over the scored probabilistic
 *   questions, or -1 when there were none
 * @log_loss: (out) (optional): mean log-loss over the same, or -1
 * @detail: (out) (optional) (transfer full) (nullable): each miss, `; `-separated
 *
 * Returns: unscored with no expectation, pass when every expectation
 *   holds, fail otherwise
 */
AiBenchmarkOutcome
ai_benchmark_case_score_decision(
	AiBenchmarkCase     *self,
	AiDecisionResponse  *response,
	gdouble             *brier,
	gdouble             *log_loss,
	gchar              **detail
){
	g_autoptr(GString) misses = NULL;
	gdouble brier_sum;
	gdouble loss_sum;
	guint probabilistic;
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), AI_BENCHMARK_OUTCOME_UNSCORED);
	g_return_val_if_fail(AI_IS_DECISION_RESPONSE(response), AI_BENCHMARK_OUTCOME_UNSCORED);

	if (brier != NULL)
		*brier = -1;
	if (log_loss != NULL)
		*log_loss = -1;
	if (detail != NULL)
		*detail = NULL;
	if (self->expectations->len == 0)
		return AI_BENCHMARK_OUTCOME_UNSCORED;

	misses = g_string_new(NULL);
	brier_sum = 0;
	loss_sum = 0;
	probabilistic = 0;

	for (i = 0; i < self->expectations->len; i++)
	{
		Expectation *e = (Expectation *)g_ptr_array_index(self->expectations, i);
		JsonObject *question = ai_json_get_object(json_node_get_object(self->questions), e->question_id);
		g_autofree gchar *miss = NULL;

		if (g_str_equal(e->type, "noul"))
		{
			/* P(true) against the threshold; Brier is the binary formula. */
			gdouble p = ai_decision_response_get_probability(response, e->question_id, NULL);
			gdouble y = e->truth ? 1.0 : 0.0;

			if (p < 0)
				miss = g_strdup_printf("%s: no probability in the response", e->question_id);
			else
			{
				brier_sum += (p - y) * (p - y);
				loss_sum += -log(MAX(e->truth ? p : 1.0 - p, LOG_LOSS_FLOOR));
				probabilistic++;
				if ((p >= self->noul_threshold) != e->truth)
					miss = g_strdup_printf("%s: expected %s, P(true) was %.4f",
					                       e->question_id, e->truth ? "true" : "false", p);
			}
		}
		else if (g_str_equal(e->type, "choice") || g_str_equal(e->type, "score"))
		{
			g_autoptr(GPtrArray) labels = question_labels(question, e->type);
			const gchar *picked = NULL;
			g_autofree gchar *picked_owned = NULL;
			gdouble b;
			gdouble l;

			/* The answer: the selected option, or the most probable level. */
			if (g_str_equal(e->type, "choice"))
				picked = ai_decision_response_get_choice(response, e->question_id);
			else
			{
				gdouble best = -1;
				guint k;

				for (k = 0; k < labels->len; k++)
				{
					gdouble p = ai_decision_response_get_probability(response, e->question_id,
					                                                  (const gchar *)g_ptr_array_index(labels, k));
					if (p > best)
					{
						best = p;
						g_free(picked_owned);
						picked_owned = g_strdup((const gchar *)g_ptr_array_index(labels, k));
					}
				}
				picked = picked_owned;
			}

			if (distribution_scores(response, e->question_id, labels, e->label, &b, &l))
			{
				brier_sum += b;
				loss_sum += l;
				probabilistic++;
			}
			if (g_strcmp0(picked, e->label) != 0)
				miss = g_strdup_printf("%s: expected %s, got %s", e->question_id,
				                       e->expected, picked != NULL ? picked : "nothing");
		}
		else if (g_str_equal(e->type, "box"))
		{
			gdouble got[4];

			if (!ai_decision_response_get_box(response, e->question_id, &got[0], &got[1], &got[2], &got[3]))
				miss = g_strdup_printf("%s: no box in the response", e->question_id);
			else
			{
				gdouble iou = box_iou(e->box, got);

				if (iou < self->box_iou)
					miss = g_strdup_printf("%s: box IoU %.3f is below %.3f",
					                       e->question_id, iou, self->box_iou);
			}
		}

		if (miss != NULL)
		{
			if (misses->len > 0)
				g_string_append(misses, "; ");
			g_string_append(misses, miss);
		}
	}

	if (probabilistic > 0)
	{
		if (brier != NULL)
			*brier = brier_sum / (gdouble)probabilistic;
		if (log_loss != NULL)
			*log_loss = loss_sum / (gdouble)probabilistic;
	}
	if (misses->len == 0)
		return AI_BENCHMARK_OUTCOME_PASS;
	if (detail != NULL)
		*detail = g_strdup(misses->str);
	return AI_BENCHMARK_OUTCOME_FAIL;
}

/**
 * ai_benchmark_case_score_embedding:
 * @self: an embedding case
 * @embedding: the provider's vectors, one per text
 * @detail: (out) (optional) (transfer full) (nullable): why it failed
 *
 * Returns: unscored with no expectation, otherwise pass or fail
 */
AiBenchmarkOutcome
ai_benchmark_case_score_embedding(
	AiBenchmarkCase  *self,
	AiEmbedding      *embedding,
	gchar           **detail
){
	gsize n;
	gsize dims;
	gdouble close_pair;
	gdouble far_pair;
	guint i;

	g_return_val_if_fail(AI_IS_BENCHMARK_CASE(self), AI_BENCHMARK_OUTCOME_UNSCORED);
	g_return_val_if_fail(embedding != NULL, AI_BENCHMARK_OUTCOME_UNSCORED);

	if (detail != NULL)
		*detail = NULL;
	if (!self->has_similarity)
		return AI_BENCHMARK_OUTCOME_UNSCORED;

	n = ai_embedding_get_n_vectors(embedding);
	dims = ai_embedding_get_dimensions(embedding);
	for (i = 0; i < 4; i++)
	{
		if (self->similarity[i] >= n)
		{
			if (detail != NULL)
				*detail = g_strdup_printf("expected at least %u vectors, got %" G_GSIZE_FORMAT,
				                          self->similarity[i] + 1, n);
			return AI_BENCHMARK_OUTCOME_FAIL;
		}
	}

	close_pair = ai_embedding_cosine(ai_embedding_get_vector(embedding, self->similarity[0]),
	                                 ai_embedding_get_vector(embedding, self->similarity[1]), dims);
	far_pair = ai_embedding_cosine(ai_embedding_get_vector(embedding, self->similarity[2]),
	                               ai_embedding_get_vector(embedding, self->similarity[3]), dims);
	if (close_pair > far_pair)
		return AI_BENCHMARK_OUTCOME_PASS;

	if (detail != NULL)
		*detail = g_strdup_printf("cos(%u,%u)=%.4f is not above cos(%u,%u)=%.4f",
		                          self->similarity[0], self->similarity[1], close_pair,
		                          self->similarity[2], self->similarity[3], far_pair);
	return AI_BENCHMARK_OUTCOME_FAIL;
}
