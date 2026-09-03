/*
 * ai-embedding-shared.c - Shared embedding plumbing for providers (private)
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "providers/ai-embedding-shared.h"
#include "core/ai-error.h"

JsonNode *
ai_embedding_shared_build_request (
    const gchar *const *texts,
    const gchar        *model
){
    g_autoptr(JsonBuilder) builder = NULL;
    gsize i;

    g_return_val_if_fail(NULL != texts, NULL);

    builder = json_builder_new();
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(builder, model);

    /*
     * Always an array, even for one input. Both services accept that, and
     * a caller embedding one passage then several would otherwise be
     * exercising two different request shapes for no reason.
     */
    json_builder_set_member_name(builder, "input");
    json_builder_begin_array(builder);

    for (i = 0; NULL != texts[i]; i++)
        json_builder_add_string_value(builder, texts[i]);

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * Reads one JSON array of numbers into @out, normalised.
 */
static gboolean
ai_embedding_shared_take_vector (
    AiEmbedding  *out,
    JsonArray    *numbers,
    GError      **error
){
    g_autofree gfloat *vector = NULL;
    guint length;
    guint i;

    length = json_array_get_length(numbers);

    if (0 == length)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                            "The service returned an empty vector");
        return FALSE;
    }

    vector = g_new0(gfloat, length);

    for (i = 0; i < length; i++)
        vector[i] = (gfloat)json_array_get_double_element(numbers, i);

    ai_embedding_normalize(vector, length);

    return ai_embedding_add_vector(out, vector, length, error);
}

AiEmbedding *
ai_embedding_shared_parse (
    AiEmbeddingWire   wire,
    JsonNode         *root,
    const gchar      *model,
    gsize             expected,
    GError          **error
){
    g_autoptr(AiEmbedding) out = NULL;
    JsonObject *object;
    JsonArray *list;
    const gchar *member;
    guint length;
    guint i;

    g_return_val_if_fail(NULL != root, NULL);

    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                            "The embedding response was not an object");
        return NULL;
    }

    object = json_node_get_object(root);
    member = (AI_EMBEDDING_WIRE_OLLAMA == wire) ? "embeddings" : "data";

    /*
     * Checked by presence and by type. A service that answered an error as
     * a 200 with {"error": "..."} would otherwise read as a response with
     * no vectors, and the message the caller sees would be about shape
     * rather than about what went wrong.
     */
    if (!json_object_has_member(object, member))
    {
        const gchar *message = NULL;

        if (json_object_has_member(object, "error"))
        {
            JsonNode *node = json_object_get_member(object, "error");

            if (JSON_NODE_HOLDS_VALUE(node))
                message = json_node_get_string(node);
            else if (JSON_NODE_HOLDS_OBJECT(node))
                message = json_object_get_string_member_with_default(
                    json_node_get_object(node), "message", NULL);
        }

        if (NULL != message)
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                        "The embedding service refused: %s", message);
        else
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                        "The embedding response has no \"%s\"", member);

        return NULL;
    }

    list = json_object_get_array_member(object, member);

    if (NULL == list)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "The embedding response's \"%s\" was not a list", member);
        return NULL;
    }

    out = ai_embedding_new(model, 0);
    length = json_array_get_length(list);

    for (i = 0; i < length; i++)
    {
        JsonArray *numbers;

        if (AI_EMBEDDING_WIRE_OLLAMA == wire)
        {
            numbers = json_array_get_array_element(list, i);
        }
        else
        {
            JsonObject *entry;

            entry = json_array_get_object_element(list, i);
            numbers = (NULL != entry)
                ? json_object_get_array_member(entry, "embedding")
                : NULL;
        }

        if (NULL == numbers)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                        "Embedding %u was not an array of numbers", i);
            return NULL;
        }

        if (!ai_embedding_shared_take_vector(out, numbers, error))
            return NULL;
    }

    /*
     * One vector per input, in order. A short response is not a partial
     * success: the caller pairs these with its passages positionally, so a
     * dropped input silently attaches every later vector to the wrong text
     * and nothing downstream can tell.
     */
    if (ai_embedding_get_n_vectors(out) != expected)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Asked for %" G_GSIZE_FORMAT " embeddings and got %"
                    G_GSIZE_FORMAT, expected,
                    ai_embedding_get_n_vectors(out));
        return NULL;
    }

    return g_steal_pointer(&out);
}
