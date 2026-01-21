/*
 * ai-tool-use.h - Tool use content block
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

#include "model/ai-content-block.h"

G_BEGIN_DECLS

#define AI_TYPE_TOOL_USE (ai_tool_use_get_type())

G_DECLARE_FINAL_TYPE(AiToolUse, ai_tool_use, AI, TOOL_USE, AiContentBlock)

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
);

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
);

/**
 * ai_tool_use_get_id:
 * @self: an #AiToolUse
 *
 * Gets the tool use ID.
 *
 * Returns: (transfer none): the tool use ID
 */
const gchar *
ai_tool_use_get_id(AiToolUse *self);

/**
 * ai_tool_use_get_name:
 * @self: an #AiToolUse
 *
 * Gets the tool name.
 *
 * Returns: (transfer none): the tool name
 */
const gchar *
ai_tool_use_get_name(AiToolUse *self);

/**
 * ai_tool_use_get_input:
 * @self: an #AiToolUse
 *
 * Gets the tool input as a JSON node.
 *
 * Returns: (transfer none) (nullable): the input JSON node
 */
JsonNode *
ai_tool_use_get_input(AiToolUse *self);

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
);

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
);

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
);

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
);

G_END_DECLS
