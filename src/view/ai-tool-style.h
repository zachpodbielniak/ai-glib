/*
 * ai-tool-style.h - How a tool name reads in a transcript
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "view/ai-tool-call.h"

G_BEGIN_DECLS

/**
 * AiToolStyle:
 * @tool_name: the name the model uses, matched case-sensitively
 * @verb: past tense, capitalised --- "Edited", "Ran", "Read"
 * @noun_singular: what one of them is --- "file", "command"
 * @noun_plural: what several are --- "files", "commands"
 * @category: which bucket it groups into
 * @target_key: the input parameter naming what it acted on, or %NULL
 * @counts_diff: whether it contributes to the +N-M summary
 *
 * How one tool should read when a transcript summarises it.
 *
 * This table *is* the registration. Teaching the transcript about a new
 * provider's tool vocabulary is one struct literal --- the same pattern
 * #AiImageModelInfo uses for image models. A tool with no entry still
 * renders, under a generic verb, rather than rendering wrongly.
 */
typedef struct
{
    const gchar    *tool_name;
    const gchar    *verb;
    const gchar    *noun_singular;
    const gchar    *noun_plural;
    AiToolCategory  category;
    const gchar    *target_key;
    gboolean        counts_diff;
} AiToolStyle;

#define AI_TYPE_TOOL_STYLE (ai_tool_style_get_type())

GType
ai_tool_style_get_type(void) G_GNUC_CONST;

AiToolStyle *
ai_tool_style_copy(const AiToolStyle *self);

void
ai_tool_style_free(AiToolStyle *self);

const AiToolStyle *
ai_tool_style_lookup(const gchar *tool_name);

void
ai_tool_style_register(const AiToolStyle *style);

const gchar *
ai_tool_category_verb(AiToolCategory category);

const gchar *
ai_tool_category_noun(
    AiToolCategory category,
    gboolean       plural
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiToolStyle, ai_tool_style_free)

G_END_DECLS
