/*
 * ai-style.h - The rendering contract between the library and a frontend
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

G_BEGIN_DECLS

/**
 * AiStyleTag:
 * @AI_STYLE_DEFAULT: ordinary text
 * @AI_STYLE_USER_PROMPT: something the user typed
 * @AI_STYLE_HEADING: a section heading
 * @AI_STYLE_DIM: present but secondary --- counts, timings, hints
 * @AI_STYLE_TOOL_NAME: the verb of a tool group ("Edited", "Ran")
 * @AI_STYLE_TOOL_TARGET: what it acted on --- a file name or a command
 * @AI_STYLE_TOOL_PENDING: a call that has not finished
 * @AI_STYLE_TOOL_OK: a call that succeeded
 * @AI_STYLE_TOOL_FAILED: a call that failed or was refused
 * @AI_STYLE_ADDED: lines added, the "+21" of a diff summary
 * @AI_STYLE_REMOVED: lines removed, the "-6"
 * @AI_STYLE_CODE: literal text --- a command, a path, a snippet
 * @AI_STYLE_THINKING: reasoning, which is not the answer
 * @AI_STYLE_ERROR: a failure message
 * @AI_STYLE_STATUS: an informational note
 * @AI_STYLE_LINK: a URL
 * @AI_STYLE_MARKER: the expand/collapse affordance
 *
 * How a run of characters should look.
 *
 * These are *roles*, not colours. The library has no opinion about whether
 * %AI_STYLE_ADDED is green, bold, or underlined --- it says only that this
 * run of bytes is an addition, and the frontend decides what that means on
 * its display. That is what lets one renderer map them to ncurses attribute
 * pairs and another to Emacs faces without either of them re-deriving the
 * structure.
 */
typedef enum
{
    AI_STYLE_DEFAULT = 0,
    AI_STYLE_USER_PROMPT,
    AI_STYLE_HEADING,
    AI_STYLE_DIM,
    AI_STYLE_TOOL_NAME,
    AI_STYLE_TOOL_TARGET,
    AI_STYLE_TOOL_PENDING,
    AI_STYLE_TOOL_OK,
    AI_STYLE_TOOL_FAILED,
    AI_STYLE_ADDED,
    AI_STYLE_REMOVED,
    AI_STYLE_CODE,
    AI_STYLE_THINKING,
    AI_STYLE_ERROR,
    AI_STYLE_STATUS,
    AI_STYLE_LINK,
    AI_STYLE_MARKER,

    /*< private >*/
    AI_STYLE_N_TAGS
} AiStyleTag;

/**
 * AiStyleSpan:
 * @start: byte offset into the rendered text
 * @len: length in bytes
 * @tag: how this run should look
 *
 * One styled run.
 *
 * Offsets are *byte* offsets into UTF-8 text, which is what C and ncurses
 * both want. Emacs counts characters instead, and converts with
 * `byte-to-position`; see the recipe in docs/transcript.org. A span never
 * begins or ends inside a multi-byte sequence.
 */
typedef struct
{
    guint      start;
    guint      len;
    AiStyleTag tag;
} AiStyleSpan;

#define AI_TYPE_STYLE_SPAN (ai_style_span_get_type())

GType
ai_style_span_get_type(void) G_GNUC_CONST;

AiStyleSpan *
ai_style_span_copy(const AiStyleSpan *self);

void
ai_style_span_free(AiStyleSpan *self);

#define AI_TYPE_RENDERED_TEXT (ai_rendered_text_get_type())

/**
 * AiRenderedText:
 *
 * A block rendered to text, plus the spans that say how it should look.
 *
 * Refcounted and treated as immutable once handed out --- the builder
 * functions are for whoever is constructing it.
 */
typedef struct _AiRenderedText AiRenderedText;

GType
ai_rendered_text_get_type(void) G_GNUC_CONST;

AiRenderedText *
ai_rendered_text_new(void);

AiRenderedText *
ai_rendered_text_ref(AiRenderedText *self);

void
ai_rendered_text_unref(AiRenderedText *self);

const gchar *
ai_rendered_text_get_text(AiRenderedText *self);

guint
ai_rendered_text_get_length(AiRenderedText *self);

guint
ai_rendered_text_get_n_spans(AiRenderedText *self);

gboolean
ai_rendered_text_get_span(
    AiRenderedText *self,
    guint           index_,
    guint          *out_start,
    guint          *out_len,
    AiStyleTag     *out_tag
);

AiStyleTag
ai_rendered_text_get_tag_at(
    AiRenderedText *self,
    guint           offset
);

void
ai_rendered_text_append(
    AiRenderedText *self,
    const gchar    *text,
    AiStyleTag      tag
);

void
ai_rendered_text_append_printf(
    AiRenderedText *self,
    AiStyleTag      tag,
    const gchar    *format,
    ...
) G_GNUC_PRINTF(3, 4);

AiRenderedText *
ai_rendered_text_wrap(
    AiRenderedText *self,
    guint           width
);

const gchar *
ai_style_tag_to_string(AiStyleTag tag);

AiStyleTag
ai_style_tag_from_string(const gchar *name);

guint
ai_style_text_width(const gchar *text);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiRenderedText, ai_rendered_text_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiStyleSpan, ai_style_span_free)

G_END_DECLS
