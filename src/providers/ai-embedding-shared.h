/*
 * ai-embedding-shared.h - Shared embedding plumbing for providers (private)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib. It is INTERNAL: it is compiled into the
 * library but not installed and not part of the public umbrella header.
 *
 * Two wire shapes cover every embedding service worth talking to. The
 * OpenAI one -- POST /v1/embeddings, {"data":[{"embedding":[...]}]} -- is
 * spoken by OpenAI itself and by every compatible server (vLLM, LM Studio,
 * llama.cpp, Together and the rest). ollama has its own: POST /api/embed,
 * {"embeddings":[[...]]}. They differ in the path and in where the numbers
 * sit, and in nothing else that matters, so both live here rather than in
 * two nearly identical providers.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-embedding-shared.h is private to ai-glib and cannot be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "model/ai-embedding.h"

G_BEGIN_DECLS

typedef enum
{
    AI_EMBEDDING_WIRE_OPENAI = 0,
    AI_EMBEDDING_WIRE_OLLAMA
} AiEmbeddingWire;

/*
 * ai_embedding_shared_build_request:
 * @texts: (array zero-terminated=1): the passages
 * @model: the model id to name in the request
 *
 * Builds the request body. Both wire shapes take {"model", "input"} with
 * input as an array, so one builder serves both.
 *
 * Returns: (transfer full): the request body
 */
JsonNode *
ai_embedding_shared_build_request (
    const gchar *const *texts,
    const gchar        *model
);

/*
 * ai_embedding_shared_parse:
 * @wire: which response shape to expect
 * @root: the parsed response
 * @model: the model to record on the result
 * @expected: how many vectors the request asked for
 * @error: return location for a GError
 *
 * Reads the vectors out of a response, normalising each to unit length.
 *
 * Normalising here rather than at each call site is what makes a stored
 * vector comparable to any other without the store having to remember what
 * scale it was on. A count that does not match @expected is an error: a
 * provider that silently dropped an input would otherwise leave the caller
 * pairing vectors with the wrong passages, which no later check catches.
 *
 * Returns: (transfer full) (nullable): the vectors, or %NULL
 */
AiEmbedding *
ai_embedding_shared_parse (
    AiEmbeddingWire   wire,
    JsonNode         *root,
    const gchar      *model,
    gsize             expected,
    GError          **error
);

G_END_DECLS
