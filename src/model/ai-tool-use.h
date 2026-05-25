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

AiToolUse *
ai_tool_use_new(
    const gchar *id,
    const gchar *name,
    JsonNode    *input
);

AiToolUse *
ai_tool_use_new_from_json_string(
    const gchar *id,
    const gchar *name,
    const gchar *input_json
);

const gchar *
ai_tool_use_get_id(AiToolUse *self);

const gchar *
ai_tool_use_get_name(AiToolUse *self);

JsonNode *
ai_tool_use_get_input(AiToolUse *self);

const gchar *
ai_tool_use_get_input_string(
    AiToolUse   *self,
    const gchar *param_name
);

gint64
ai_tool_use_get_input_int(
    AiToolUse   *self,
    const gchar *param_name,
    gint64       default_value
);

gdouble
ai_tool_use_get_input_double(
    AiToolUse   *self,
    const gchar *param_name,
    gdouble      default_value
);

gboolean
ai_tool_use_get_input_boolean(
    AiToolUse   *self,
    const gchar *param_name,
    gboolean     default_value
);

G_END_DECLS
