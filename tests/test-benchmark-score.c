/*
 * test-benchmark-score.c - AiBenchmarkCase expectations and scoring
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The score functions are pure, so every rule is checked here without a
 * provider: chat matching, Noul thresholds, choice and score answers,
 * box IoU, embedding rankings, and the Brier and log-loss figures each
 * one produces. Expectations that cannot fit their question are refused
 * when they are added, by name.
 */

#include <ai-glib.h>
#include <math.h>
#include <string.h>

#define EPS (1e-9)

/* ---- chat ------------------------------------------------------- */

static void
test_chat_unscored(
	void
){
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_chat("p", "hi");
	g_autofree gchar *detail = NULL;

	g_assert_false(ai_benchmark_case_has_expectation(c));
	g_assert_cmpint(ai_benchmark_case_score_text(c, "anything", &detail), ==, AI_BENCHMARK_OUTCOME_UNSCORED);
	g_assert_null(detail);
	g_assert_cmpint(ai_benchmark_case_get_max_tokens(c), ==, 1024);
}

/* contains: case-insensitive, by Unicode case folding rather than ASCII. */
static void
test_chat_contains(
	void
){
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_chat("p", "Capital of France?");
	g_autofree gchar *detail = NULL;

	ai_benchmark_case_expect_contains(c, "paris");
	g_assert_true(ai_benchmark_case_has_expectation(c));
	g_assert_cmpint(ai_benchmark_case_score_text(c, "It is PARIS.", NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
	g_assert_cmpint(ai_benchmark_case_score_text(c, "Lyon", &detail), ==, AI_BENCHMARK_OUTCOME_FAIL);
	g_assert_cmpstr(detail, ==, "expected a reply containing \"paris\"");
	g_assert_cmpint(ai_benchmark_case_score_text(c, NULL, NULL), ==, AI_BENCHMARK_OUTCOME_FAIL);

	ai_benchmark_case_expect_contains(c, "ÉCOLE");
	g_assert_cmpint(ai_benchmark_case_score_text(c, "une école", NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
}

/* equals: the whole reply, after trimming surrounding whitespace only. */
static void
test_chat_equals(
	void
){
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_chat("p", "2+2?");

	ai_benchmark_case_expect_equals(c, "4");
	g_assert_cmpint(ai_benchmark_case_score_text(c, "  4\n", NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
	g_assert_cmpint(ai_benchmark_case_score_text(c, "4.", NULL), ==, AI_BENCHMARK_OUTCOME_FAIL);
	g_assert_cmpint(ai_benchmark_case_score_text(c, "The answer is 4", NULL), ==, AI_BENCHMARK_OUTCOME_FAIL);
}

static void
test_chat_regex(
	void
){
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_chat("p", "Give a year");
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_benchmark_case_expect_regex(c, "\\b19[0-9]{2}\\b", &error));
	g_assert_no_error(error);
	g_assert_cmpint(ai_benchmark_case_score_text(c, "It was 1969.", NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
	g_assert_cmpint(ai_benchmark_case_score_text(c, "It was 2001.", NULL), ==, AI_BENCHMARK_OUTCOME_FAIL);

	/* A bad pattern is refused and leaves the previous expectation in place. */
	g_assert_false(ai_benchmark_case_expect_regex(c, "(unclosed", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "case 'p': invalid regex '(unclosed'"));
	g_assert_cmpint(ai_benchmark_case_score_text(c, "1984", NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
}

/* ---- decisions -------------------------------------------------- */

static AiDecisionRequest *
make_request(
	void
){
	static const gchar *const labels[] = { "refund", "other", NULL };
	static const gchar *const descriptions[] = { "asks for money back", "anything else", NULL };
	static const gchar *const levels[] = { "low", "medium", "high", NULL };
	AiDecisionRequest *request;
	g_autoptr(GError) error = NULL;

	request = ai_decision_request_new("Please refund the duplicate charge.");
	g_assert_true(ai_decision_request_add_boolean(request, "spam", "Is this spam?", &error));
	g_assert_true(ai_decision_request_add_choice(request, "intent", "What is asked?", labels, descriptions, &error));
	g_assert_true(ai_decision_request_add_score(request, "urgency", "How urgent?", levels, &error));
	g_assert_true(ai_decision_request_add_box(request, "logo", "Where is the logo?", &error));
	g_assert_no_error(error);
	return request;
}

static AiDecisionResponse *
make_response(
	AiDecisionRequest *request,
	gdouble            spam,
	const gchar       *choice,
	gdouble            refund_p,
	const gchar       *score_probs,
	const gchar       *box
){
	g_autofree gchar *json = NULL;
	g_autoptr(GError) error = NULL;
	gchar spam_text[G_ASCII_DTOSTR_BUF_SIZE];
	gchar refund_text[G_ASCII_DTOSTR_BUF_SIZE];
	gchar other_text[G_ASCII_DTOSTR_BUF_SIZE];
	AiDecisionResponse *response;

	g_ascii_dtostr(spam_text, sizeof(spam_text), spam);
	g_ascii_dtostr(refund_text, sizeof(refund_text), refund_p);
	g_ascii_dtostr(other_text, sizeof(other_text), 1.0 - refund_p);
	json = g_strdup_printf(
		"{\"model\":\"m\",\"answers\":{"
		"\"spam\":{\"type\":\"noul\",\"noul\":%s},"
		"\"intent\":{\"type\":\"choice\",\"choice\":\"%s\",\"probabilities\":{\"refund\":%s,\"other\":%s}},"
		"\"urgency\":{\"type\":\"score\",\"score\":1.6,\"probabilities\":%s},"
		"\"logo\":{\"type\":\"box\",\"box\":%s}}}",
		spam_text, choice, refund_text, other_text, score_probs, box);
	response = ai_decision_response_new_from_json(request, json, 1.0, &error);
	g_assert_no_error(error);
	return response;
}

/*
 * Noul: P(true) against the threshold. Brier is (p - y)^2 and log-loss
 * is -ln of the probability given to the labelled answer.
 */
static void
test_noul(
	void
){
	g_autoptr(AiDecisionRequest) request = make_request();
	g_autoptr(AiDecisionResponse) response = make_response(request, 0.8, "refund", 0.7,
		"{\"0\":0.1,\"1\":0.2,\"2\":0.7}", "{\"top\":0,\"bottom\":0.5,\"left\":0,\"right\":0.5}");
	g_autoptr(GError) error = NULL;
	gdouble brier;
	gdouble loss;

	/* expected true, p = 0.8 */
	{
		g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
		g_autofree gchar *detail = NULL;

		g_assert_true(ai_benchmark_case_expect_answer(c, "spam", "true", &error));
		g_assert_cmpint(ai_benchmark_case_score_decision(c, response, &brier, &loss, &detail),
		                ==, AI_BENCHMARK_OUTCOME_PASS);
		g_assert_null(detail);
		g_assert_cmpfloat_with_epsilon(brier, 0.04, EPS);
		g_assert_cmpfloat_with_epsilon(loss, -log(0.8), EPS);
	}

	/* expected false ("no" spelling), p = 0.8: a confident miss */
	{
		g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
		g_autofree gchar *detail = NULL;

		g_assert_true(ai_benchmark_case_expect_answer(c, "spam", "no", &error));
		g_assert_cmpint(ai_benchmark_case_score_decision(c, response, &brier, &loss, &detail),
		                ==, AI_BENCHMARK_OUTCOME_FAIL);
		g_assert_cmpstr(detail, ==, "spam: expected false, P(true) was 0.8000");
		g_assert_cmpfloat_with_epsilon(brier, 0.64, EPS);
		g_assert_cmpfloat_with_epsilon(loss, -log(0.2), 1e-6);
	}

	/* A stricter threshold turns the same answer into a miss. */
	{
		g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);

		ai_benchmark_case_set_noul_threshold(c, 0.9);
		g_assert_cmpfloat(ai_benchmark_case_get_noul_threshold(c), ==, 0.9);
		g_assert_true(ai_benchmark_case_expect_answer(c, "spam", "yes", &error));
		g_assert_cmpint(ai_benchmark_case_score_decision(c, response, NULL, NULL, NULL),
		                ==, AI_BENCHMARK_OUTCOME_FAIL);
	}
}

/* A probability of exactly 0 for the right answer costs a bounded log-loss. */
static void
test_log_loss_is_bounded(
	void
){
	g_autoptr(AiDecisionRequest) request = make_request();
	g_autoptr(AiDecisionResponse) response = make_response(request, 1.0, "refund", 1.0,
		"{\"0\":0,\"1\":0,\"2\":1}", "{\"top\":0,\"bottom\":0.5,\"left\":0,\"right\":0.5}");
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
	g_autoptr(GError) error = NULL;
	gdouble brier;
	gdouble loss;

	g_assert_true(ai_benchmark_case_expect_answer(c, "spam", "false", &error));
	ai_benchmark_case_score_decision(c, response, &brier, &loss, NULL);
	g_assert_cmpfloat_with_epsilon(brier, 1.0, EPS);
	g_assert_true(isfinite(loss));
	g_assert_cmpfloat_with_epsilon(loss, -log(1e-15), 1e-6);
}

/*
 * Choice: the selected option. Brier is half the multi-class formula, so
 * for two options it matches the binary one.
 */
static void
test_choice(
	void
){
	g_autoptr(AiDecisionRequest) request = make_request();
	g_autoptr(AiDecisionResponse) response = make_response(request, 0.1, "refund", 0.7,
		"{\"0\":0.1,\"1\":0.2,\"2\":0.7}", "{\"top\":0,\"bottom\":0.5,\"left\":0,\"right\":0.5}");
	g_autoptr(GError) error = NULL;
	gdouble brier;
	gdouble loss;

	{
		g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);

		g_assert_true(ai_benchmark_case_expect_answer(c, "intent", "refund", &error));
		g_assert_cmpint(ai_benchmark_case_score_decision(c, response, &brier, &loss, NULL),
		                ==, AI_BENCHMARK_OUTCOME_PASS);
		/* ((0.7 - 1)^2 + (0.3 - 0)^2) / 2 */
		g_assert_cmpfloat_with_epsilon(brier, 0.09, 1e-9);
		g_assert_cmpfloat_with_epsilon(loss, -log(0.7), 1e-9);
	}
	{
		g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
		g_autofree gchar *detail = NULL;

		g_assert_true(ai_benchmark_case_expect_answer(c, "intent", "other", &error));
		g_assert_cmpint(ai_benchmark_case_score_decision(c, response, &brier, NULL, &detail),
		                ==, AI_BENCHMARK_OUTCOME_FAIL);
		g_assert_cmpstr(detail, ==, "intent: expected other, got refund");
		g_assert_cmpfloat_with_epsilon(brier, 0.49, 1e-9);
	}
}

/*
 * Score: the most probable level, named or by zero-based index; the
 * expected ordinal (1.6 here) is not what is compared.
 */
static void
test_score(
	void
){
	g_autoptr(AiDecisionRequest) request = make_request();
	g_autoptr(AiDecisionResponse) response = make_response(request, 0.1, "refund", 0.7,
		"{\"0\":0.1,\"1\":0.2,\"2\":0.7}", "{\"top\":0,\"bottom\":0.5,\"left\":0,\"right\":0.5}");
	g_autoptr(GError) error = NULL;
	gdouble brier;

	{
		g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);

		g_assert_true(ai_benchmark_case_expect_answer(c, "urgency", "high", &error));
		g_assert_cmpint(ai_benchmark_case_score_decision(c, response, &brier, NULL, NULL),
		                ==, AI_BENCHMARK_OUTCOME_PASS);
		/* (0.1^2 + 0.2^2 + 0.3^2) / 2 */
		g_assert_cmpfloat_with_epsilon(brier, 0.07, 1e-9);
	}
	{
		g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
		g_autofree gchar *detail = NULL;

		g_assert_true(ai_benchmark_case_expect_answer(c, "urgency", "1", &error));
		g_assert_cmpint(ai_benchmark_case_score_decision(c, response, NULL, NULL, &detail),
		                ==, AI_BENCHMARK_OUTCOME_FAIL);
		g_assert_cmpstr(detail, ==, "urgency: expected 1, got 2");
	}
}

/* Box: intersection over union, passing at 0.5 by default. No Brier. */
static void
test_box(
	void
){
	g_autoptr(AiDecisionRequest) request = make_request();
	g_autoptr(AiDecisionResponse) exact = make_response(request, 0.1, "refund", 0.7,
		"{\"0\":0.1,\"1\":0.2,\"2\":0.7}", "{\"top\":0,\"bottom\":0.5,\"left\":0,\"right\":0.5}");
	g_autoptr(AiDecisionResponse) half = make_response(request, 0.1, "refund", 0.7,
		"{\"0\":0.1,\"1\":0.2,\"2\":0.7}", "{\"top\":0,\"bottom\":0.5,\"left\":0,\"right\":0.25}");
	g_autoptr(AiDecisionResponse) apart = make_response(request, 0.1, "refund", 0.7,
		"{\"0\":0.1,\"1\":0.2,\"2\":0.7}", "{\"top\":0.5,\"bottom\":1,\"left\":0.5,\"right\":1}");
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *detail = NULL;
	gdouble brier;

	g_assert_true(ai_benchmark_case_expect_answer(c, "logo", "0, 0.5, 0, 0.5", &error));
	g_assert_cmpint(ai_benchmark_case_score_decision(c, exact, &brier, NULL, NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
	g_assert_cmpfloat(brier, ==, -1);
	/* IoU 0.5 passes at the default 0.5 ... */
	g_assert_cmpint(ai_benchmark_case_score_decision(c, half, NULL, NULL, NULL), ==, AI_BENCHMARK_OUTCOME_PASS);
	/* ... and fails at a stricter one. */
	ai_benchmark_case_set_box_iou(c, 0.6);
	g_assert_cmpint(ai_benchmark_case_score_decision(c, half, NULL, NULL, NULL), ==, AI_BENCHMARK_OUTCOME_FAIL);
	g_assert_cmpint(ai_benchmark_case_score_decision(c, apart, NULL, NULL, &detail), ==, AI_BENCHMARK_OUTCOME_FAIL);
	g_assert_cmpstr(detail, ==, "logo: box IoU 0.000 is below 0.600");
}

/*
 * Several expectations: a case passes only when all do, every miss is
 * reported, and Brier is the mean over the probabilistic ones.
 */
static void
test_multiple(
	void
){
	g_autoptr(AiDecisionRequest) request = make_request();
	g_autoptr(AiDecisionResponse) response = make_response(request, 0.8, "refund", 0.7,
		"{\"0\":0.1,\"1\":0.2,\"2\":0.7}", "{\"top\":0,\"bottom\":0.5,\"left\":0,\"right\":0.5}");
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *detail = NULL;
	gdouble brier;

	g_assert_true(ai_benchmark_case_expect_answer(c, "spam", "false", &error));
	g_assert_true(ai_benchmark_case_expect_answer(c, "intent", "refund", &error));
	g_assert_true(ai_benchmark_case_expect_answer(c, "urgency", "low", &error));
	g_assert_cmpint(ai_benchmark_case_score_decision(c, response, &brier, NULL, &detail),
	                ==, AI_BENCHMARK_OUTCOME_FAIL);
	g_assert_cmpstr(detail, ==, "spam: expected false, P(true) was 0.8000; urgency: expected low, got 2");
	/* (0.64 + 0.09 + (0.9^2 + 0.2^2 + 0.7^2) / 2) / 3 */
	g_assert_cmpfloat_with_epsilon(brier, (0.64 + 0.09 + 0.67) / 3.0, 1e-9);
}

/* An expectation that cannot fit its question is refused by name. */
static void
test_expectation_refusals(
	void
){
	static const struct
	{
		const gchar *question;
		const gchar *answer;
		const gchar *message;
	} rows[] = {
		{ "nope", "true", "case 'd': expectation names question 'nope', which the request does not ask" },
		{ "spam", "maybe", "case 'd': 'maybe' is not a valid answer for noul question 'spam'" },
		{ "intent", "chargeback", "case 'd': 'chargeback' is not a valid answer for choice question 'intent'" },
		{ "urgency", "extreme", "case 'd': 'extreme' is not a valid answer for score question 'urgency'" },
		{ "urgency", "3", "case 'd': '3' is not a valid answer for score question 'urgency'" },
		{ "urgency", "-1", "case 'd': '-1' is not a valid answer for score question 'urgency'" },
		{ "logo", "0,0.5,0", "case 'd': '0,0.5,0' is not a valid answer for box question 'logo'" },
		{ "logo", "0.5,0.1,0,1", "case 'd': '0.5,0.1,0,1' is not a valid answer for box question 'logo'" },
		{ "logo", "0,1.5,0,1", "case 'd': '0,1.5,0,1' is not a valid answer for box question 'logo'" },
	};
	g_autoptr(AiDecisionRequest) request = make_request();
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_decision("d", request);
	g_autoptr(AiBenchmarkCase) chat = ai_benchmark_case_new_chat("p", "hi");
	guint i;

	for (i = 0; i < G_N_ELEMENTS(rows); i++)
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(ai_benchmark_case_expect_answer(c, rows[i].question, rows[i].answer, &error));
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_cmpstr(error->message, ==, rows[i].message);
	}
	g_assert_false(ai_benchmark_case_has_expectation(c));

	{
		g_autoptr(GError) error = NULL;

		g_assert_false(ai_benchmark_case_expect_answer(chat, "spam", "true", &error));
		g_assert_cmpstr(error->message, ==, "case 'p': answers can only be expected of a decision case");
	}
}

/*
 * The case keeps its own snapshot: a question added to the caller's
 * request afterwards is neither sent nor expectable.
 */
static void
test_request_snapshot(
	void
){
	g_autoptr(AiDecisionRequest) request = ai_decision_request_new("text");
	g_autoptr(AiBenchmarkCase) c = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL;

	g_assert_true(ai_decision_request_add_boolean(request, "a", "A?", &error));
	c = ai_benchmark_case_new_decision("d", request);
	g_assert_true(ai_decision_request_add_boolean(request, "b", "B?", &error));
	json = ai_decision_request_dup_json(ai_benchmark_case_get_request(c));
	g_assert_null(strstr(json, "\"b\""));
	g_assert_false(ai_benchmark_case_expect_answer(c, "b", "true", &error));
}

/* ---- embedding -------------------------------------------------- */

static AiEmbedding *
make_embedding(
	void
){
	static const gfloat a[] = { 1, 0, 0 };
	static const gfloat b[] = { 0.9f, 0.1f, 0 };
	static const gfloat c[] = { 0, 0, 1 };
	AiEmbedding *embedding;

	embedding = ai_embedding_new("test-embed", 3);
	g_assert_true(ai_embedding_add_vector(embedding, a, 3, NULL));
	g_assert_true(ai_embedding_add_vector(embedding, b, 3, NULL));
	g_assert_true(ai_embedding_add_vector(embedding, c, 3, NULL));
	return embedding;
}

static void
test_embedding(
	void
){
	static const gchar *const texts[] = { "cat", "kitten", "invoice", NULL };
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_embedding("e", texts);
	g_autoptr(AiEmbedding) embedding = make_embedding();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *detail = NULL;

	g_assert_cmpint(ai_benchmark_case_score_embedding(c, embedding, NULL), ==, AI_BENCHMARK_OUTCOME_UNSCORED);
	g_assert_cmpuint(g_strv_length((gchar **)ai_benchmark_case_get_texts(c)), ==, 3);

	g_assert_true(ai_benchmark_case_expect_similarity(c, 0, 1, 0, 2, &error));
	g_assert_cmpint(ai_benchmark_case_score_embedding(c, embedding, NULL), ==, AI_BENCHMARK_OUTCOME_PASS);

	g_assert_true(ai_benchmark_case_expect_similarity(c, 0, 2, 0, 1, &error));
	g_assert_cmpint(ai_benchmark_case_score_embedding(c, embedding, &detail), ==, AI_BENCHMARK_OUTCOME_FAIL);
	g_assert_nonnull(strstr(detail, "cos(0,2)="));

	g_assert_false(ai_benchmark_case_expect_similarity(c, 0, 3, 0, 1, &error));
	g_assert_cmpstr(error->message, ==,
	                "case 'e': similarity indices must name texts 0 to 2 of an embedding case");
}

/* An embedding that returned fewer vectors than the indices name fails. */
static void
test_embedding_short_batch(
	void
){
	static const gchar *const texts[] = { "a", "b", "c", NULL };
	static const gfloat v[] = { 1, 0 };
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_embedding("e", texts);
	g_autoptr(AiEmbedding) embedding = ai_embedding_new("m", 2);
	g_autofree gchar *detail = NULL;

	g_assert_true(ai_embedding_add_vector(embedding, v, 2, NULL));
	g_assert_true(ai_benchmark_case_expect_similarity(c, 0, 1, 0, 2, NULL));
	g_assert_cmpint(ai_benchmark_case_score_embedding(c, embedding, &detail), ==, AI_BENCHMARK_OUTCOME_FAIL);
	g_assert_cmpstr(detail, ==, "expected at least 2 vectors, got 1");
}

/* ---- image ------------------------------------------------------ */

static void
test_image_is_never_scored(
	void
){
	g_autoptr(AiBenchmarkCase) c = ai_benchmark_case_new_image("i", "a red fox");

	g_assert_false(ai_benchmark_case_has_expectation(c));
	g_assert_cmpint(ai_benchmark_case_get_kind(c), ==, AI_BENCHMARK_KIND_IMAGE);
	g_assert_cmpint(ai_benchmark_case_get_image_count(c), ==, 1);
	g_assert_cmpint(ai_benchmark_case_get_image_size(c), ==, AI_IMAGE_SIZE_AUTO);
	ai_benchmark_case_set_image_size(c, AI_IMAGE_SIZE_512);
	ai_benchmark_case_set_image_count(c, 2);
	g_assert_cmpint(ai_benchmark_case_get_image_size(c), ==, AI_IMAGE_SIZE_512);
	g_assert_cmpint(ai_benchmark_case_get_image_count(c), ==, 2);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/benchmark/score/chat-unscored", test_chat_unscored);
	g_test_add_func("/benchmark/score/chat-contains", test_chat_contains);
	g_test_add_func("/benchmark/score/chat-equals", test_chat_equals);
	g_test_add_func("/benchmark/score/chat-regex", test_chat_regex);
	g_test_add_func("/benchmark/score/noul", test_noul);
	g_test_add_func("/benchmark/score/log-loss-bounded", test_log_loss_is_bounded);
	g_test_add_func("/benchmark/score/choice", test_choice);
	g_test_add_func("/benchmark/score/score-level", test_score);
	g_test_add_func("/benchmark/score/box-iou", test_box);
	g_test_add_func("/benchmark/score/multiple-expectations", test_multiple);
	g_test_add_func("/benchmark/score/expectation-refusals", test_expectation_refusals);
	g_test_add_func("/benchmark/score/request-snapshot", test_request_snapshot);
	g_test_add_func("/benchmark/score/embedding", test_embedding);
	g_test_add_func("/benchmark/score/embedding-short-batch", test_embedding_short_batch);
	g_test_add_func("/benchmark/score/image-never-scored", test_image_is_never_scored);
	return g_test_run();
}
