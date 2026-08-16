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

AiTool *
ai_tool_new(
    const gchar *name,
    const gchar *description
);

const gchar *
ai_tool_get_name(AiTool *self);

const gchar *
ai_tool_get_description(AiTool *self);

void
ai_tool_add_parameter(
    AiTool      *self,
    const gchar *name,
    const gchar *type,
    const gchar *description,
    gboolean     required
);

void
ai_tool_add_enum_parameter(
    AiTool       *self,
    const gchar  *name,
    const gchar  *description,
    const gchar **enum_values,
    gboolean      required
);

void
ai_tool_add_array_parameter(
    AiTool      *self,
    const gchar *name,
    const gchar *description,
    const gchar *item_schema,
    gboolean     required
);

JsonNode *
ai_tool_get_parameters_json(AiTool *self);

JsonNode *
ai_tool_to_json(
    AiTool         *self,
    AiProviderType  provider
);

G_END_DECLS
