/*
 * ai-tool.c - Tool/function definition
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "model/ai-tool.h"

/*
 * Structure for storing parameter information.
 */
typedef struct
{
    gchar    *name;
    gchar    *type;
    gchar    *description;
    gchar   **enum_values;
    gboolean  required;
} AiToolParameter;

/*
 * Private structure for AiTool.
 */
struct _AiTool
{
    GObject parent_instance;

    gchar *name;
    gchar *description;
    GList *parameters;      /* List of AiToolParameter */
    GList *required_params; /* List of required parameter names (borrowed) */
};

G_DEFINE_TYPE(AiTool, ai_tool, G_TYPE_OBJECT)

/*
 * Property IDs.
 */
enum
{
    PROP_0,
    PROP_NAME,
    PROP_DESCRIPTION,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

/*
 * Free a tool parameter structure.
 */
static void
ai_tool_parameter_free(AiToolParameter *param)
{
    if (param == NULL)
    {
        return;
    }

    g_free(param->name);
    g_free(param->type);
    g_free(param->description);
    g_strfreev(param->enum_values);
    g_slice_free(AiToolParameter, param);
}

static void
ai_tool_finalize(GObject *object)
{
    AiTool *self = AI_TOOL(object);

    g_clear_pointer(&self->name, g_free);
    g_clear_pointer(&self->description, g_free);
    g_list_free_full(self->parameters, (GDestroyNotify)ai_tool_parameter_free);
    g_list_free(self->required_params);

    G_OBJECT_CLASS(ai_tool_parent_class)->finalize(object);
}

static void
ai_tool_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiTool *self = AI_TOOL(object);

    switch (prop_id)
    {
        case PROP_NAME:
            g_value_set_string(value, self->name);
            break;
        case PROP_DESCRIPTION:
            g_value_set_string(value, self->description);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_tool_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiTool *self = AI_TOOL(object);

    switch (prop_id)
    {
        case PROP_NAME:
            g_clear_pointer(&self->name, g_free);
            self->name = g_value_dup_string(value);
            break;
        case PROP_DESCRIPTION:
            g_clear_pointer(&self->description, g_free);
            self->description = g_value_dup_string(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_tool_class_init(AiToolClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_tool_finalize;
    object_class->get_property = ai_tool_get_property;
    object_class->set_property = ai_tool_set_property;

    /**
     * AiTool:name:
     *
     * The tool name.
     */
    properties[PROP_NAME] =
        g_param_spec_string("name",
                            "Name",
                            "The tool name",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                            G_PARAM_STATIC_STRINGS);

    /**
     * AiTool:description:
     *
     * The tool description.
     */
    properties[PROP_DESCRIPTION] =
        g_param_spec_string("description",
                            "Description",
                            "The tool description",
                            NULL,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                            G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_tool_init(AiTool *self)
{
    self->parameters = NULL;
    self->required_params = NULL;
}

/**
 * ai_tool_new:
 * @name: the tool name
 * @description: the tool description
 *
 * Creates a new #AiTool with the given name and description.
 *
 * Returns: (transfer full): a new #AiTool
 */
AiTool *
ai_tool_new(
    const gchar *name,
    const gchar *description
){
    g_autoptr(AiTool) self = g_object_new(AI_TYPE_TOOL,
                                          "name", name,
                                          "description", description,
                                          NULL);

    return (AiTool *)g_steal_pointer(&self);
}

/**
 * ai_tool_get_name:
 * @self: an #AiTool
 *
 * Gets the tool name.
 *
 * Returns: (transfer none): the tool name
 */
const gchar *
ai_tool_get_name(AiTool *self)
{
    g_return_val_if_fail(AI_IS_TOOL(self), NULL);

    return self->name;
}

/**
 * ai_tool_get_description:
 * @self: an #AiTool
 *
 * Gets the tool description.
 *
 * Returns: (transfer none): the tool description
 */
const gchar *
ai_tool_get_description(AiTool *self)
{
    g_return_val_if_fail(AI_IS_TOOL(self), NULL);

    return self->description;
}

/**
 * ai_tool_add_parameter:
 * @self: an #AiTool
 * @name: the parameter name
 * @type: the parameter type (e.g., "string", "number", "boolean", "object", "array")
 * @description: the parameter description
 * @required: whether the parameter is required
 *
 * Adds a parameter to the tool's input schema.
 */
void
ai_tool_add_parameter(
    AiTool      *self,
    const gchar *name,
    const gchar *type,
    const gchar *description,
    gboolean     required
){
    AiToolParameter *param;

    g_return_if_fail(AI_IS_TOOL(self));
    g_return_if_fail(name != NULL);
    g_return_if_fail(type != NULL);

    param = g_slice_new0(AiToolParameter);
    param->name = g_strdup(name);
    param->type = g_strdup(type);
    param->description = g_strdup(description);
    param->enum_values = NULL;
    param->required = required;

    self->parameters = g_list_append(self->parameters, param);

    if (required)
    {
        self->required_params = g_list_append(self->required_params, param->name);
    }
}

/**
 * ai_tool_add_enum_parameter:
 * @self: an #AiTool
 * @name: the parameter name
 * @description: the parameter description
 * @enum_values: (array zero-terminated=1): array of allowed values
 * @required: whether the parameter is required
 *
 * Adds an enumeration parameter to the tool's input schema.
 */
void
ai_tool_add_enum_parameter(
    AiTool       *self,
    const gchar  *name,
    const gchar  *description,
    const gchar **enum_values,
    gboolean      required
){
    AiToolParameter *param;

    g_return_if_fail(AI_IS_TOOL(self));
    g_return_if_fail(name != NULL);
    g_return_if_fail(enum_values != NULL);

    param = g_slice_new0(AiToolParameter);
    param->name = g_strdup(name);
    param->type = g_strdup("string");
    param->description = g_strdup(description);
    param->enum_values = g_strdupv((gchar **)enum_values);
    param->required = required;

    self->parameters = g_list_append(self->parameters, param);

    if (required)
    {
        self->required_params = g_list_append(self->required_params, param->name);
    }
}

/**
 * ai_tool_get_parameters_json:
 * @self: an #AiTool
 *
 * Gets the parameters schema as a JSON node.
 *
 * Returns: (transfer full) (nullable): the parameters schema
 */
JsonNode *
ai_tool_get_parameters_json(AiTool *self)
{
    g_autoptr(JsonBuilder) builder = NULL;
    GList *l;

    g_return_val_if_fail(AI_IS_TOOL(self), NULL);

    builder = json_builder_new();

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "object");

    /* Properties */
    json_builder_set_member_name(builder, "properties");
    json_builder_begin_object(builder);

    for (l = self->parameters; l != NULL; l = l->next)
    {
        AiToolParameter *param = l->data;

        json_builder_set_member_name(builder, param->name);
        json_builder_begin_object(builder);

        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, param->type);

        if (param->description != NULL)
        {
            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(builder, param->description);
        }

        if (param->enum_values != NULL)
        {
            gint i;

            json_builder_set_member_name(builder, "enum");
            json_builder_begin_array(builder);

            for (i = 0; param->enum_values[i] != NULL; i++)
            {
                json_builder_add_string_value(builder, param->enum_values[i]);
            }

            json_builder_end_array(builder);
        }

        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);

    /* Required parameters */
    if (self->required_params != NULL)
    {
        json_builder_set_member_name(builder, "required");
        json_builder_begin_array(builder);

        for (l = self->required_params; l != NULL; l = l->next)
        {
            json_builder_add_string_value(builder, (const gchar *)l->data);
        }

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/**
 * ai_tool_to_json:
 * @self: an #AiTool
 * @provider: the target provider (affects output format)
 *
 * Serializes the tool definition to JSON for the specified provider.
 * Different providers have slightly different tool schemas:
 * - Claude: { "name", "description", "input_schema" }
 * - OpenAI/Grok: { "type": "function", "function": { "name", "description", "parameters" } }
 * - Gemini: Similar to OpenAI
 *
 * Returns: (transfer full): the tool definition as JSON
 */
JsonNode *
ai_tool_to_json(
    AiTool         *self,
    AiProviderType  provider
){
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonNode) params_node = NULL;

    g_return_val_if_fail(AI_IS_TOOL(self), NULL);

    builder = json_builder_new();
    params_node = ai_tool_get_parameters_json(self);

    switch (provider)
    {
        case AI_PROVIDER_CLAUDE:
            /* Claude format: { name, description, input_schema } */
            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, self->name);

            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(builder,
                self->description != NULL ? self->description : "");

            json_builder_set_member_name(builder, "input_schema");
            json_builder_add_value(builder, g_steal_pointer(&params_node));

            json_builder_end_object(builder);
            break;

        case AI_PROVIDER_OPENAI:
        case AI_PROVIDER_GROK:
        case AI_PROVIDER_OLLAMA:
            /* OpenAI format: { type: "function", function: { name, description, parameters } } */
            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "type");
            json_builder_add_string_value(builder, "function");

            json_builder_set_member_name(builder, "function");
            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, self->name);

            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(builder,
                self->description != NULL ? self->description : "");

            json_builder_set_member_name(builder, "parameters");
            json_builder_add_value(builder, g_steal_pointer(&params_node));

            json_builder_end_object(builder);
            json_builder_end_object(builder);
            break;

        case AI_PROVIDER_GEMINI:
            /* Gemini format: similar structure to OpenAI */
            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, self->name);

            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(builder,
                self->description != NULL ? self->description : "");

            json_builder_set_member_name(builder, "parameters");
            json_builder_add_value(builder, g_steal_pointer(&params_node));

            json_builder_end_object(builder);
            break;

        default:
            /* Default to Claude format */
            json_builder_begin_object(builder);

            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, self->name);

            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(builder,
                self->description != NULL ? self->description : "");

            json_builder_set_member_name(builder, "input_schema");
            json_builder_add_value(builder, g_steal_pointer(&params_node));

            json_builder_end_object(builder);
            break;
    }

    return json_builder_get_root(builder);
}
