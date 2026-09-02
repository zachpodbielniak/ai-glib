/*
 * ai-json-util.h - Type-checked JSON member accessors
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API. Every
 * symbol is `static inline`, so nothing new is exported and nothing new
 * is introspected.
 *
 * What these exist to stop:
 *
 * `json_object_has_member()` answers "is the key present", which is a
 * different question from "is the value the type I am about to read it
 * as". `"usage": null` and `"content": {}` both pass it, and the typed
 * accessor that follows logs a Json-CRITICAL and hands back NULL --- which
 * a caller who checked only `has_member` does not expect, and which then
 * travels into the next accessor for a second critical. A critical is
 * fatal under GTest and under `G_DEBUG=fatal-warnings`; without those it
 * is a turn that silently yields an empty response.
 *
 * A server does not have to be hostile to produce it. `ai_config_set_base_url()`
 * points a provider at anything, and a proxy, a gateway or the next
 * version of an API is enough to change a shape.
 *
 * These are the *only* way anything in this library reads a member: the
 * five HTTP providers, the shared image parser, the six CLI providers
 * and `ai-http-error.c`. It lives in `core/` rather than `providers/`
 * for the last of those --- a base-layer file including a provider
 * header to get an accessor is the wrong way round.
 *
 * There were six other implementations of this idea beside it --- one in
 * each of five CLI providers, plus `ai_http_error__string_member()` ---
 * and they had drifted: `grok_get_int()` and `agy_get_int()` accepted
 * `G_TYPE_INT` where `cc_get_int()` and `oc_get_int()` did not, and
 * returned `gint` where the others returned `gint64`. Nothing chose
 * either behaviour --- they were written at different times against the
 * same reasoning. The accessors below are a superset of all six, so the
 * fold changed no answer; what it buys is that the seventh copy cannot
 * drift, because there is nowhere left to write it.
 *
 * JSON null is treated as absent throughout: a server that sends
 * `"usage": null` means the same thing as one that omits it, and every
 * caller would otherwise have to spell that out again.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-json-util.h is an internal header"
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/*
 * The member's node, only when it is present and actually holds @type.
 * Everything else here is a wrapper on this one question.
 */
static inline JsonNode *
ai_json_member_of_type(
    JsonObject   *obj,
    const gchar  *member,
    JsonNodeType  type
){
    JsonNode *node;

    if (obj == NULL || member == NULL)
    {
        return NULL;
    }

    node = json_object_get_member(obj, member);

    if (node == NULL || json_node_get_node_type(node) != type)
    {
        return NULL;
    }

    return node;
}

/*
 * The member's object, or NULL when it is absent, null, or of another
 * type. Unlike json_object_get_object_member() this does not log.
 */
static inline JsonObject *
ai_json_get_object(JsonObject *obj, const gchar *member)
{
    JsonNode *node = ai_json_member_of_type(obj, member, JSON_NODE_OBJECT);

    return node != NULL ? json_node_get_object(node) : NULL;
}

/* The member's array, or NULL when absent, null, or of another type. */
static inline JsonArray *
ai_json_get_array(JsonObject *obj, const gchar *member)
{
    JsonNode *node = ai_json_member_of_type(obj, member, JSON_NODE_ARRAY);

    return node != NULL ? json_node_get_array(node) : NULL;
}

/*
 * The member's string, or @fallback when it is absent, null, or of
 * another type. @fallback may be NULL, which is how a caller asks
 * "is this member a string at all".
 */
static inline const gchar *
ai_json_get_string(
    JsonObject  *obj,
    const gchar *member,
    const gchar *fallback
){
    JsonNode *node = ai_json_member_of_type(obj, member, JSON_NODE_VALUE);

    if (node == NULL || json_node_get_value_type(node) != G_TYPE_STRING)
    {
        return fallback;
    }

    return json_node_get_string(node);
}

/*
 * The member's integer value, or @fallback when absent or not a number.
 *
 * A document that went through a JavaScript encoder may spell a whole
 * number as a double, so G_TYPE_DOUBLE is accepted; G_TYPE_INT is too,
 * for a node a caller built rather than parsed. This is the detail the
 * private CLI copies had drifted on --- `grok_get_int()` and
 * `agy_get_int()` took all three, `cc_get_int()` and `oc_get_int()` two
 * --- and taking all three is what makes this a superset of both, so
 * the fold could not change an answer. A caller that wants to reject a
 * float has to say so; none does, because a token count arriving as
 * 1024.0 is a counter, not a different quantity.
 */
static inline gint64
ai_json_get_int(JsonObject *obj, const gchar *member, gint64 fallback)
{
    JsonNode *node = ai_json_member_of_type(obj, member, JSON_NODE_VALUE);
    GType     value_type;

    if (node == NULL)
    {
        return fallback;
    }

    value_type = json_node_get_value_type(node);

    if (value_type == G_TYPE_INT64 || value_type == G_TYPE_INT)
    {
        return json_node_get_int(node);
    }

    if (value_type == G_TYPE_DOUBLE)
    {
        return (gint64)json_node_get_double(node);
    }

    return fallback;
}

/*
 * The member's floating-point value, or @fallback when absent or not a
 * number. An integer is a number too --- json-glib types 1 as an int and
 * 1.0 as a double, and a server is entitled to send either.
 */
static inline gdouble
ai_json_get_double(JsonObject *obj, const gchar *member, gdouble fallback)
{
    JsonNode *node = ai_json_member_of_type(obj, member, JSON_NODE_VALUE);
    GType     value_type;

    if (node == NULL)
    {
        return fallback;
    }

    value_type = json_node_get_value_type(node);

    if (value_type == G_TYPE_DOUBLE)
    {
        return json_node_get_double(node);
    }

    if (value_type == G_TYPE_INT64 || value_type == G_TYPE_INT)
    {
        return (gdouble)json_node_get_int(node);
    }

    return fallback;
}

/*
 * The member's boolean, or @fallback when absent or not a boolean.
 *
 * A JSON boolean read with the string reader fails to the fallback in
 * silence and has no misspelling in it, which is why the type is checked
 * rather than assumed.
 */
static inline gboolean
ai_json_get_boolean(JsonObject *obj, const gchar *member, gboolean fallback)
{
    JsonNode *node = ai_json_member_of_type(obj, member, JSON_NODE_VALUE);

    if (node == NULL || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    {
        return fallback;
    }

    return json_node_get_boolean(node);
}

/*
 * The member's node whatever it holds, or NULL when absent or null.
 *
 * For the places that legitimately pass a value through without reading
 * it --- a tool call's `input`, which is whatever the model wrote.
 */
static inline JsonNode *
ai_json_get_node(JsonObject *obj, const gchar *member)
{
    JsonNode *node;

    if (obj == NULL || member == NULL)
    {
        return NULL;
    }

    node = json_object_get_member(obj, member);

    if (node == NULL || JSON_NODE_HOLDS_NULL(node))
    {
        return NULL;
    }

    return node;
}

/*
 * Element @index of @array when it is an object, NULL otherwise ---
 * including when @array is NULL, so a caller can chain a get_array()
 * straight into a loop.
 *
 * json_array_get_object_element() logs and returns NULL for an element
 * of the wrong type, so `"content": [1,2,3]` is one critical per element.
 */
static inline JsonObject *
ai_json_array_get_object(JsonArray *array, guint index)
{
    JsonNode *node;

    if (array == NULL || index >= json_array_get_length(array))
    {
        return NULL;
    }

    node = json_array_get_element(array, index);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
    {
        return NULL;
    }

    return json_node_get_object(node);
}

/* Element @index of @array when it is a string, @fallback otherwise. */
static inline const gchar *
ai_json_array_get_string(
    JsonArray   *array,
    guint        index,
    const gchar *fallback
){
    JsonNode *node;

    if (array == NULL || index >= json_array_get_length(array))
    {
        return fallback;
    }

    node = json_array_get_element(array, index);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
    {
        return fallback;
    }

    return json_node_get_string(node);
}

/*
 * The root of @parser as an object, or NULL.
 *
 * json_parser_get_root() returns NULL for a bare `null` document, which
 * JSON_NODE_HOLDS_OBJECT() would then dereference --- so the NULL check
 * has to come first, every time, and did not always.
 */
static inline JsonObject *
ai_json_root_object(JsonParser *parser)
{
    JsonNode *root;

    if (parser == NULL)
    {
        return NULL;
    }

    root = json_parser_get_root(parser);

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
    {
        return NULL;
    }

    return json_node_get_object(root);
}

G_END_DECLS
