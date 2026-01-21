/*
 * ai-tool-result.c - Tool result content block
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-tool-result.h"

#include <json-glib/json-glib.h>

/*
 * Private structure for AiToolResult.
 */
struct _AiToolResult
{
    AiContentBlock parent_instance;

    gchar   *tool_use_id;
    gchar   *content;
    gboolean is_error;
};

G_DEFINE_TYPE(AiToolResult, ai_tool_result, AI_TYPE_CONTENT_BLOCK)

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_TOOL_USE_ID,
    PROP_CONTENT,
    PROP_IS_ERROR,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_tool_result_finalize(GObject *object)
{
    AiToolResult *self = AI_TOOL_RESULT(object);

    g_clear_pointer(&self->tool_use_id, g_free);
    g_clear_pointer(&self->content, g_free);

    G_OBJECT_CLASS(ai_tool_result_parent_class)->finalize(object);
}

static void
ai_tool_result_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiToolResult *self = AI_TOOL_RESULT(object);

    switch (prop_id)
    {
        case PROP_TOOL_USE_ID:
            g_value_set_string(value, self->tool_use_id);
            break;
        case PROP_CONTENT:
            g_value_set_string(value, self->content);
            break;
        case PROP_IS_ERROR:
            g_value_set_boolean(value, self->is_error);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_tool_result_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiToolResult *self = AI_TOOL_RESULT(object);

    switch (prop_id)
    {
        case PROP_TOOL_USE_ID:
            g_clear_pointer(&self->tool_use_id, g_free);
            self->tool_use_id = g_value_dup_string(value);
            break;
        case PROP_CONTENT:
            g_clear_pointer(&self->content, g_free);
            self->content = g_value_dup_string(value);
            break;
        case PROP_IS_ERROR:
            self->is_error = g_value_get_boolean(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * Override get_content_type to return AI_CONTENT_TYPE_TOOL_RESULT.
 */
static AiContentType
ai_tool_result_get_content_type(AiContentBlock *block)
{
    (void)block;
    return AI_CONTENT_TYPE_TOOL_RESULT;
}

/*
 * Serialize to JSON in Claude format:
 * { "type": "tool_result", "tool_use_id": "...", "content": "...", "is_error": false }
 */
static JsonNode *
ai_tool_result_to_json(AiContentBlock *block)
{
    AiToolResult *self = AI_TOOL_RESULT(block);
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "tool_result");

    json_builder_set_member_name(builder, "tool_use_id");
    json_builder_add_string_value(builder, self->tool_use_id != NULL ? self->tool_use_id : "");

    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, self->content != NULL ? self->content : "");

    if (self->is_error)
    {
        json_builder_set_member_name(builder, "is_error");
        json_builder_add_boolean_value(builder, TRUE);
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static void
ai_tool_result_class_init(AiToolResultClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiContentBlockClass *content_class = AI_CONTENT_BLOCK_CLASS(klass);

    object_class->finalize = ai_tool_result_finalize;
    object_class->get_property = ai_tool_result_get_property;
    object_class->set_property = ai_tool_result_set_property;

    /* Override virtual methods */
    content_class->get_content_type = ai_tool_result_get_content_type;
    content_class->to_json = ai_tool_result_to_json;

    /**
     * AiToolResult:tool-use-id:
     *
     * The tool use ID this result corresponds to.
     */
    properties[PROP_TOOL_USE_ID] =
        g_param_spec_string("tool-use-id",
                            "Tool Use ID",
                            "The tool use ID this result corresponds to",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiToolResult:content:
     *
     * The result content.
     */
    properties[PROP_CONTENT] =
        g_param_spec_string("content",
                            "Content",
                            "The result content",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiToolResult:is-error:
     *
     * Whether this result indicates an error.
     */
    properties[PROP_IS_ERROR] =
        g_param_spec_boolean("is-error",
                             "Is Error",
                             "Whether this result indicates an error",
                             FALSE,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                             G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_tool_result_init(AiToolResult *self)
{
    self->tool_use_id = NULL;
    self->content = NULL;
    self->is_error = FALSE;
}

/**
 * ai_tool_result_new:
 * @tool_use_id: the ID of the tool use this is responding to
 * @content: the result content
 * @is_error: whether this result indicates an error
 *
 * Creates a new #AiToolResult with the given content.
 *
 * Returns: (transfer full): a new #AiToolResult
 */
AiToolResult *
ai_tool_result_new(
    const gchar *tool_use_id,
    const gchar *content,
    gboolean     is_error
){
    g_autoptr(AiToolResult) self = g_object_new(AI_TYPE_TOOL_RESULT,
                                                 "tool-use-id", tool_use_id,
                                                 "content", content,
                                                 "is-error", is_error,
                                                 NULL);

    return (AiToolResult *)g_steal_pointer(&self);
}

/**
 * ai_tool_result_get_tool_use_id:
 * @self: an #AiToolResult
 *
 * Gets the tool use ID this result corresponds to.
 *
 * Returns: (transfer none): the tool use ID
 */
const gchar *
ai_tool_result_get_tool_use_id(AiToolResult *self)
{
    g_return_val_if_fail(AI_IS_TOOL_RESULT(self), NULL);

    return self->tool_use_id;
}

/**
 * ai_tool_result_get_content:
 * @self: an #AiToolResult
 *
 * Gets the result content.
 *
 * Returns: (transfer none): the result content
 */
const gchar *
ai_tool_result_get_content(AiToolResult *self)
{
    g_return_val_if_fail(AI_IS_TOOL_RESULT(self), NULL);

    return self->content;
}

/**
 * ai_tool_result_get_is_error:
 * @self: an #AiToolResult
 *
 * Gets whether this result indicates an error.
 *
 * Returns: %TRUE if this is an error result
 */
gboolean
ai_tool_result_get_is_error(AiToolResult *self)
{
    g_return_val_if_fail(AI_IS_TOOL_RESULT(self), FALSE);

    return self->is_error;
}
