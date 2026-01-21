/*
 * ai-tool-use.c - Tool use content block
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-tool-use.h"

/*
 * Private structure for AiToolUse.
 */
struct _AiToolUse
{
    AiContentBlock parent_instance;

    gchar    *id;
    gchar    *name;
    JsonNode *input;
};

G_DEFINE_TYPE(AiToolUse, ai_tool_use, AI_TYPE_CONTENT_BLOCK)

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_ID,
    PROP_NAME,
    PROP_INPUT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ai_tool_use_finalize(GObject *object)
{
    AiToolUse *self = AI_TOOL_USE(object);

    g_clear_pointer(&self->id, g_free);
    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->input, json_node_unref);

    G_OBJECT_CLASS(ai_tool_use_parent_class)->finalize(object);
}

static void
ai_tool_use_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiToolUse *self = AI_TOOL_USE(object);

    switch (prop_id)
    {
        case PROP_ID:
            g_value_set_string(value, self->id);
            break;
        case PROP_NAME:
            g_value_set_string(value, self->name);
            break;
        case PROP_INPUT:
            g_value_set_boxed(value, self->input);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_tool_use_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiToolUse *self = AI_TOOL_USE(object);

    switch (prop_id)
    {
        case PROP_ID:
            g_clear_pointer(&self->id, g_free);
            self->id = g_value_dup_string(value);
            break;
        case PROP_NAME:
            g_clear_pointer(&self->name, g_free);
            self->name = g_value_dup_string(value);
            break;
        case PROP_INPUT:
            g_clear_pointer(&self->input, json_node_unref);
            self->input = g_value_dup_boxed(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * Override get_content_type to return AI_CONTENT_TYPE_TOOL_USE.
 */
static AiContentType
ai_tool_use_get_content_type(AiContentBlock *block)
{
    (void)block;
    return AI_CONTENT_TYPE_TOOL_USE;
}

/*
 * Serialize to JSON in Claude format:
 * { "type": "tool_use", "id": "...", "name": "...", "input": {...} }
 */
static JsonNode *
ai_tool_use_to_json(AiContentBlock *block)
{
    AiToolUse *self = AI_TOOL_USE(block);
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "tool_use");

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, self->id != NULL ? self->id : "");

    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, self->name != NULL ? self->name : "");

    json_builder_set_member_name(builder, "input");
    if (self->input != NULL)
    {
        json_builder_add_value(builder, json_node_copy(self->input));
    }
    else
    {
        json_builder_begin_object(builder);
        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

static void
ai_tool_use_class_init(AiToolUseClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    AiContentBlockClass *content_class = AI_CONTENT_BLOCK_CLASS(klass);

    object_class->finalize = ai_tool_use_finalize;
    object_class->get_property = ai_tool_use_get_property;
    object_class->set_property = ai_tool_use_set_property;

    /* Override virtual methods */
    content_class->get_content_type = ai_tool_use_get_content_type;
    content_class->to_json = ai_tool_use_to_json;

    /**
     * AiToolUse:id:
     *
     * The tool use ID.
     */
    properties[PROP_ID] =
        g_param_spec_string("id",
                            "ID",
                            "The tool use ID",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiToolUse:name:
     *
     * The tool name.
     */
    properties[PROP_NAME] =
        g_param_spec_string("name",
                            "Name",
                            "The tool name",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiToolUse:input:
     *
     * The tool input as a JSON node.
     */
    properties[PROP_INPUT] =
        g_param_spec_boxed("input",
                           "Input",
                           "The tool input as JSON",
                           JSON_TYPE_NODE,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
                           G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_tool_use_init(AiToolUse *self)
{
    self->id = NULL;
    self->name = NULL;
    self->input = NULL;
}

/**
 * ai_tool_use_new:
 * @id: the tool use ID
 * @name: the tool name
 * @input: (nullable): the input as a JSON node
 *
 * Creates a new #AiToolUse representing a request to invoke a tool.
 *
 * Returns: (transfer full): a new #AiToolUse
 */
AiToolUse *
ai_tool_use_new(
    const gchar *id,
    const gchar *name,
    JsonNode    *input
){
    g_autoptr(AiToolUse) self = g_object_new(AI_TYPE_TOOL_USE,
                                              "id", id,
                                              "name", name,
                                              "input", input,
                                              NULL);

    return (AiToolUse *)g_steal_pointer(&self);
}

/**
 * ai_tool_use_new_from_json_string:
 * @id: the tool use ID
 * @name: the tool name
 * @input_json: (nullable): the input as a JSON string
 *
 * Creates a new #AiToolUse with input parsed from a JSON string.
 *
 * Returns: (transfer full): a new #AiToolUse
 */
AiToolUse *
ai_tool_use_new_from_json_string(
    const gchar *id,
    const gchar *name,
    const gchar *input_json
){
    g_autoptr(JsonParser) parser = NULL;
    JsonNode *input = NULL;

    if (input_json != NULL && input_json[0] != '\0')
    {
        g_autoptr(GError) error = NULL;

        parser = json_parser_new();
        if (json_parser_load_from_data(parser, input_json, -1, &error))
        {
            input = json_parser_get_root(parser);
        }
    }

    return ai_tool_use_new(id, name, input);
}

/**
 * ai_tool_use_get_id:
 * @self: an #AiToolUse
 *
 * Gets the tool use ID.
 *
 * Returns: (transfer none): the tool use ID
 */
const gchar *
ai_tool_use_get_id(AiToolUse *self)
{
    g_return_val_if_fail(AI_IS_TOOL_USE(self), NULL);

    return self->id;
}

/**
 * ai_tool_use_get_name:
 * @self: an #AiToolUse
 *
 * Gets the tool name.
 *
 * Returns: (transfer none): the tool name
 */
const gchar *
ai_tool_use_get_name(AiToolUse *self)
{
    g_return_val_if_fail(AI_IS_TOOL_USE(self), NULL);

    return self->name;
}

/**
 * ai_tool_use_get_input:
 * @self: an #AiToolUse
 *
 * Gets the tool input as a JSON node.
 *
 * Returns: (transfer none) (nullable): the input JSON node
 */
JsonNode *
ai_tool_use_get_input(AiToolUse *self)
{
    g_return_val_if_fail(AI_IS_TOOL_USE(self), NULL);

    return self->input;
}

/**
 * ai_tool_use_get_input_string:
 * @self: an #AiToolUse
 * @param_name: the parameter name
 *
 * Gets a string parameter from the input.
 *
 * Returns: (transfer none) (nullable): the parameter value
 */
const gchar *
ai_tool_use_get_input_string(
    AiToolUse   *self,
    const gchar *param_name
){
    JsonObject *obj;

    g_return_val_if_fail(AI_IS_TOOL_USE(self), NULL);
    g_return_val_if_fail(param_name != NULL, NULL);

    if (self->input == NULL || !JSON_NODE_HOLDS_OBJECT(self->input))
    {
        return NULL;
    }

    obj = json_node_get_object(self->input);
    if (!json_object_has_member(obj, param_name))
    {
        return NULL;
    }

    return json_object_get_string_member(obj, param_name);
}

/**
 * ai_tool_use_get_input_int:
 * @self: an #AiToolUse
 * @param_name: the parameter name
 * @default_value: value to return if parameter not found
 *
 * Gets an integer parameter from the input.
 *
 * Returns: the parameter value or @default_value
 */
gint64
ai_tool_use_get_input_int(
    AiToolUse   *self,
    const gchar *param_name,
    gint64       default_value
){
    JsonObject *obj;

    g_return_val_if_fail(AI_IS_TOOL_USE(self), default_value);
    g_return_val_if_fail(param_name != NULL, default_value);

    if (self->input == NULL || !JSON_NODE_HOLDS_OBJECT(self->input))
    {
        return default_value;
    }

    obj = json_node_get_object(self->input);
    if (!json_object_has_member(obj, param_name))
    {
        return default_value;
    }

    return json_object_get_int_member(obj, param_name);
}

/**
 * ai_tool_use_get_input_double:
 * @self: an #AiToolUse
 * @param_name: the parameter name
 * @default_value: value to return if parameter not found
 *
 * Gets a double parameter from the input.
 *
 * Returns: the parameter value or @default_value
 */
gdouble
ai_tool_use_get_input_double(
    AiToolUse   *self,
    const gchar *param_name,
    gdouble      default_value
){
    JsonObject *obj;

    g_return_val_if_fail(AI_IS_TOOL_USE(self), default_value);
    g_return_val_if_fail(param_name != NULL, default_value);

    if (self->input == NULL || !JSON_NODE_HOLDS_OBJECT(self->input))
    {
        return default_value;
    }

    obj = json_node_get_object(self->input);
    if (!json_object_has_member(obj, param_name))
    {
        return default_value;
    }

    return json_object_get_double_member(obj, param_name);
}

/**
 * ai_tool_use_get_input_boolean:
 * @self: an #AiToolUse
 * @param_name: the parameter name
 * @default_value: value to return if parameter not found
 *
 * Gets a boolean parameter from the input.
 *
 * Returns: the parameter value or @default_value
 */
gboolean
ai_tool_use_get_input_boolean(
    AiToolUse   *self,
    const gchar *param_name,
    gboolean     default_value
){
    JsonObject *obj;

    g_return_val_if_fail(AI_IS_TOOL_USE(self), default_value);
    g_return_val_if_fail(param_name != NULL, default_value);

    if (self->input == NULL || !JSON_NODE_HOLDS_OBJECT(self->input))
    {
        return default_value;
    }

    obj = json_node_get_object(self->input);
    if (!json_object_has_member(obj, param_name))
    {
        return default_value;
    }

    return json_object_get_boolean_member(obj, param_name);
}
