/*
 * ai-tool.h - Tool/function definition
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "core/ai-enums.h"

G_BEGIN_DECLS

#define AI_TYPE_TOOL (ai_tool_get_type())

G_DECLARE_FINAL_TYPE(AiTool, ai_tool, AI, TOOL, GObject)

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
);

/**
 * ai_tool_get_name:
 * @self: an #AiTool
 *
 * Gets the tool name.
 *
 * Returns: (transfer none): the tool name
 */
const gchar *
ai_tool_get_name(AiTool *self);

/**
 * ai_tool_get_description:
 * @self: an #AiTool
 *
 * Gets the tool description.
 *
 * Returns: (transfer none): the tool description
 */
const gchar *
ai_tool_get_description(AiTool *self);

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
);

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
);

/**
 * ai_tool_get_parameters_json:
 * @self: an #AiTool
 *
 * Gets the parameters schema as a JSON node.
 *
 * Returns: (transfer full) (nullable): the parameters schema
 */
JsonNode *
ai_tool_get_parameters_json(AiTool *self);

/**
 * ai_tool_to_json:
 * @self: an #AiTool
 * @provider: the target provider (affects output format)
 *
 * Serializes the tool definition to JSON for the specified provider.
 * Different providers have slightly different tool schemas.
 *
 * Returns: (transfer full): the tool definition as JSON
 */
JsonNode *
ai_tool_to_json(
    AiTool         *self,
    AiProviderType  provider
);

G_END_DECLS
