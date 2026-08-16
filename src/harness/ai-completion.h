/*
 * ai-completion.h - Completing / and @ in a line of input
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
#include <gio/gio.h>

#include "harness/ai-command.h"

G_BEGIN_DECLS

/**
 * AiCompletionKind:
 * @AI_COMPLETION_NONE: nothing at the cursor can be completed
 * @AI_COMPLETION_COMMAND: a `/name`
 * @AI_COMPLETION_PATH: an `@path`
 *
 * What is being completed.
 */
typedef enum
{
    AI_COMPLETION_NONE = 0,
    AI_COMPLETION_COMMAND,
    AI_COMPLETION_PATH
} AiCompletionKind;

/* ---- AiCompletionItem ---- */

/**
 * AiCompletionItem:
 * @text: what to put in the buffer, replacing the range
 * @display: what to show in a menu
 * @description: (nullable): a second column --- a summary, or an origin
 * @kind: which sort of completion this is
 * @is_directory: whether a path candidate names a directory
 *
 * One candidate.
 */
typedef struct
{
    gchar            *text;
    gchar            *display;
    gchar            *description;
    AiCompletionKind  kind;
    gboolean          is_directory;
} AiCompletionItem;

#define AI_TYPE_COMPLETION_ITEM (ai_completion_item_get_type())

GType
ai_completion_item_get_type(void) G_GNUC_CONST;

AiCompletionItem *
ai_completion_item_copy(const AiCompletionItem *self);

void
ai_completion_item_free(AiCompletionItem *self);

/* ---- AiCompletionResult ---- */

#define AI_TYPE_COMPLETION_RESULT (ai_completion_result_get_type())

G_DECLARE_FINAL_TYPE(AiCompletionResult, ai_completion_result,
                     AI, COMPLETION_RESULT, GObject)

AiCompletionKind
ai_completion_result_get_kind(AiCompletionResult *self);

guint
ai_completion_result_get_start(AiCompletionResult *self);

guint
ai_completion_result_get_end(AiCompletionResult *self);

guint
ai_completion_result_get_n_items(AiCompletionResult *self);

const AiCompletionItem *
ai_completion_result_get_item(
    AiCompletionResult *self,
    guint               index
);

gboolean
ai_completion_result_get_item_fields(
    AiCompletionResult  *self,
    guint                index,
    const gchar        **out_text,
    const gchar        **out_display,
    const gchar        **out_description,
    gboolean            *out_is_directory
);

gchar *
ai_completion_result_get_common_prefix(AiCompletionResult *self);

/* ---- AiCompletionContext ---- */

#define AI_TYPE_COMPLETION_CONTEXT (ai_completion_context_get_type())

G_DECLARE_FINAL_TYPE(AiCompletionContext, ai_completion_context,
                     AI, COMPLETION_CONTEXT, GObject)

AiCompletionContext *
ai_completion_context_new(
    AiCommandSet *commands,
    const gchar  *working_directory
);

void
ai_completion_context_set_working_directory(
    AiCompletionContext *self,
    const gchar         *path
);

const gchar *
ai_completion_context_get_working_directory(AiCompletionContext *self);

void
ai_completion_context_set_max_items(
    AiCompletionContext *self,
    guint                max_items
);

guint
ai_completion_context_get_max_items(AiCompletionContext *self);

AiCompletionResult *
ai_completion_query(
    AiCompletionContext *self,
    const gchar         *buffer,
    guint                cursor
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiCompletionItem, ai_completion_item_free)

G_END_DECLS
