/*
 * test-embedding.c - Unit tests for AiEmbedding and AiEmbedder
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <math.h>

#include "core/ai-embedder.h"
#include "core/ai-error.h"
#include "model/ai-embedding.h"
#include "providers/ai-ollama-client.h"
#include "providers/ai-openai-client.h"
#include "providers/ai-claude-client.h"

static void
test_embedding_holds_vectors(void)
{
	g_autoptr(AiEmbedding) embedding = NULL;
	g_autoptr(GError) error = NULL;
	const gfloat first[3] = { 1.0f, 0.0f, 0.0f };
	const gfloat second[3] = { 0.0f, 1.0f, 0.0f };

	embedding = ai_embedding_new("test-model", 3);

	g_assert_true(ai_embedding_add_vector(embedding, first, 3, &error));
	g_assert_no_error(error);
	g_assert_true(ai_embedding_add_vector(embedding, second, 3, &error));
	g_assert_no_error(error);

	g_assert_cmpuint(ai_embedding_get_n_vectors(embedding), ==, 2);
	g_assert_cmpuint(ai_embedding_get_dimensions(embedding), ==, 3);
	g_assert_cmpstr(ai_embedding_get_model(embedding), ==, "test-model");

	g_assert_nonnull(ai_embedding_get_vector(embedding, 0));
	g_assert_nonnull(ai_embedding_get_vector(embedding, 1));

	/* Out of range is NULL rather than a read past the end. */
	g_assert_null(ai_embedding_get_vector(embedding, 2));
}

/*
 * A set is one width throughout.
 *
 * A ragged set would not fail here; it would fail much later, as a
 * comparison between two passages that happened to arrive in different
 * batches, and by then nothing points at the cause.
 */
static void
test_embedding_refuses_a_different_width(void)
{
	g_autoptr(AiEmbedding) embedding = NULL;
	g_autoptr(GError) error = NULL;
	const gfloat three[3] = { 1.0f, 0.0f, 0.0f };
	const gfloat four[4] = { 1.0f, 0.0f, 0.0f, 0.0f };

	embedding = ai_embedding_new("test-model", 0);

	/* The first vector settles the width when none was given. */
	g_assert_true(ai_embedding_add_vector(embedding, three, 3, &error));
	g_assert_no_error(error);
	g_assert_cmpuint(ai_embedding_get_dimensions(embedding), ==, 3);

	g_assert_false(ai_embedding_add_vector(embedding, four, 4, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	g_assert_cmpuint(ai_embedding_get_n_vectors(embedding), ==, 1);
}

static void
test_embedding_normalize(void)
{
	gfloat vector[3] = { 3.0f, 4.0f, 0.0f };
	gfloat zero[3] = { 0.0f, 0.0f, 0.0f };
	gdouble length;

	ai_embedding_normalize(vector, 3);

	length = sqrt((gdouble)vector[0] * vector[0] +
	              (gdouble)vector[1] * vector[1] +
	              (gdouble)vector[2] * vector[2]);

	g_assert_cmpfloat(fabs(length - 1.0), <, 1e-6);

	/*
	 * All zeroes is left alone rather than divided by. That is what an
	 * empty passage embeds to, and a NaN would propagate silently through
	 * every comparison it later touched.
	 */
	ai_embedding_normalize(zero, 3);
	g_assert_cmpfloat(zero[0], ==, 0.0f);
	g_assert_cmpfloat(zero[1], ==, 0.0f);
	g_assert_cmpfloat(zero[2], ==, 0.0f);
}

static void
test_embedding_cosine(void)
{
	const gfloat a[3] = { 1.0f, 0.0f, 0.0f };
	const gfloat same[3] = { 1.0f, 0.0f, 0.0f };
	const gfloat orthogonal[3] = { 0.0f, 1.0f, 0.0f };
	const gfloat opposite[3] = { -1.0f, 0.0f, 0.0f };
	const gfloat longer[3] = { 5.0f, 0.0f, 0.0f };
	const gfloat zero[3] = { 0.0f, 0.0f, 0.0f };

	g_assert_cmpfloat(fabs(ai_embedding_cosine(a, same, 3) - 1.0), <, 1e-6);
	g_assert_cmpfloat(fabs(ai_embedding_cosine(a, orthogonal, 3)), <, 1e-6);
	g_assert_cmpfloat(fabs(ai_embedding_cosine(a, opposite, 3) + 1.0), <, 1e-6);

	/*
	 * Scale must not matter. This is what lets a vector that came from
	 * somewhere else -- a store written before normalising, another
	 * library -- still score correctly rather than plausibly.
	 */
	g_assert_cmpfloat(fabs(ai_embedding_cosine(a, longer, 3) - 1.0), <, 1e-6);

	/* No similarity, rather than a division by zero. */
	g_assert_cmpfloat(ai_embedding_cosine(a, zero, 3), ==, 0.0);
}

/*
 * The providers that embed say so, and the ones that do not are not merely
 * unimplemented -- asking them is a clean refusal rather than a crash.
 */
static void
test_embedder_is_implemented_where_expected(void)
{
	g_autoptr(AiOllamaClient) ollama = NULL;
	g_autoptr(AiOpenAIClient) openai = NULL;
	g_autoptr(AiClaudeClient) claude = NULL;

	ollama = ai_ollama_client_new();
	openai = ai_openai_client_new();
	claude = ai_claude_client_new();

	g_assert_true(AI_IS_EMBEDDER(ollama));
	g_assert_true(AI_IS_EMBEDDER(openai));

	/* Anthropic has no embeddings API, so the interface is absent
	 * rather than present and failing. */
	g_assert_false(AI_IS_EMBEDDER(claude));
}

static void
test_embedder_default_models(void)
{
	g_autoptr(AiOllamaClient) ollama = NULL;
	g_autoptr(AiOpenAIClient) openai = NULL;

	ollama = ai_ollama_client_new();
	openai = ai_openai_client_new();

	g_assert_cmpstr(
		ai_embedder_get_default_embedding_model(AI_EMBEDDER(ollama)),
		==, AI_OLLAMA_MODEL_NOMIC_EMBED);
	g_assert_cmpstr(
		ai_embedder_get_default_embedding_model(AI_EMBEDDER(openai)),
		==, AI_OPENAI_EMBEDDING_MODEL_3_SMALL);
}

/*
 * The model table is the registration, so what it says about a model is
 * what every caller sizing a vector store will believe.
 */
static void
test_embedder_model_table(void)
{
	g_autoptr(AiOllamaClient) ollama = NULL;
	g_autoptr(AiOpenAIClient) openai = NULL;
	g_autoptr(GList) models = NULL;
	const AiEmbeddingModelInfo *info;

	ollama = ai_ollama_client_new();
	openai = ai_openai_client_new();

	models = ai_embedder_list_embedding_models(AI_EMBEDDER(ollama));
	g_assert_nonnull(models);

	/* The default must be in the table it is the default of. */
	info = ai_embedder_get_model_info(AI_EMBEDDER(ollama), NULL);
	g_assert_nonnull(info);
	g_assert_cmpstr(info->id, ==, AI_OLLAMA_MODEL_NOMIC_EMBED);
	g_assert_cmpuint(info->dimensions, ==, 768);

	info = ai_embedder_get_model_info(AI_EMBEDDER(openai),
	                                  AI_OPENAI_EMBEDDING_MODEL_3_LARGE);
	g_assert_nonnull(info);
	g_assert_cmpuint(info->dimensions, ==, 3072);

	/* A model this provider does not serve is not invented. */
	g_assert_null(ai_embedder_get_model_info(AI_EMBEDDER(openai),
	                                         "nomic-embed-text:v1.5"));
}

/*
 * Embedding nothing is refused before a request is built. An empty batch
 * would otherwise reach the provider as {"input": []}, whose answer is an
 * empty list that reads exactly like a service returning nothing useful.
 */
static void
test_embedder_refuses_an_empty_batch(void)
{
	g_autoptr(AiOllamaClient) ollama = NULL;
	g_autoptr(AiEmbedding) embedding = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *empty[1];

	empty[0] = NULL;

	ollama = ai_ollama_client_new();
	embedding = ai_embedder_embed(AI_EMBEDDER(ollama), empty, NULL, NULL,
	                              &error);

	g_assert_null(embedding);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/embedding/holds-vectors",
	                test_embedding_holds_vectors);
	g_test_add_func("/embedding/refuses-a-different-width",
	                test_embedding_refuses_a_different_width);
	g_test_add_func("/embedding/normalize", test_embedding_normalize);
	g_test_add_func("/embedding/cosine", test_embedding_cosine);
	g_test_add_func("/embedder/implemented-where-expected",
	                test_embedder_is_implemented_where_expected);
	g_test_add_func("/embedder/default-models",
	                test_embedder_default_models);
	g_test_add_func("/embedder/model-table", test_embedder_model_table);
	g_test_add_func("/embedder/refuses-an-empty-batch",
	                test_embedder_refuses_an_empty_batch);

	return g_test_run();
}
