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
#include "core/ai-json-util.h"
#include <math.h>
#include <float.h>

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
    {
        JsonNode *node = json_array_get_element(numbers, i);
        GType type = node != NULL ? json_node_get_value_type(node) : G_TYPE_INVALID;
        gdouble value;

        if (type != G_TYPE_DOUBLE && type != G_TYPE_INT64 && type != G_TYPE_INT)
        {
            g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                "Embedding contains a non-numeric value");
            return FALSE;
        }
        value = json_node_get_double(node);
        if (!isfinite(value) || fabs(value) > FLT_MAX)
        {
            g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                "Embedding contains an out-of-range value");
            return FALSE;
        }
        vector[i] = (gfloat)value;
    }

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
    g_autofree JsonArray **ordered = NULL;

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                            "The embedding response was not an object");
        return NULL;
    }

    object = json_node_get_object(root);
    member = (AI_EMBEDDING_WIRE_OLLAMA == wire) ? "embeddings" : "data";

    if (ai_json_get_node(object, "error") != NULL)
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                            "The embedding service refused the request");
        return NULL;
    }
    list = ai_json_get_array(object, member);
    if (list == NULL)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "The embedding response's \"%s\" was not a list", member);
        return NULL;
    }

    out = ai_embedding_new(model, 0);
    length = json_array_get_length(list);

    if (length != expected)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                    "Asked for %" G_GSIZE_FORMAT " embeddings and got %u", expected, length);
        return NULL;
    }
    ordered = g_new0(JsonArray *, length);
    for (i = 0; i < length; i++)
    {
        JsonArray *numbers;
        guint index = i;

        if (AI_EMBEDDING_WIRE_OLLAMA == wire)
        {
            JsonNode *node = json_array_get_element(list, i);
            numbers = node != NULL && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
        }
        else
        {
            JsonObject *entry;

            JsonNode *index_node;

            entry = ai_json_array_get_object(list, i);
            numbers = ai_json_get_array(entry, "embedding");
            index_node = ai_json_get_node(entry, "index");
            if (index_node != NULL)
            {
                gdouble value = ai_json_get_double(entry, "index", -1);
                if (value < 0 || value >= length || floor(value) != value)
                {
                    g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                        "Invalid embedding index");
                    return NULL;
                }
                index = (guint)value;
            }
        }

        if (NULL == numbers)
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                        "Embedding %u was not an array of numbers", i);
            return NULL;
        }

        if (ordered[index] != NULL)
        {
            g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                "Duplicate embedding index");
            return NULL;
        }
        ordered[index] = numbers;
    }
    for (i = 0; i < length; i++)
    {
        if (!ai_embedding_shared_take_vector(out, ordered[i], error))
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
