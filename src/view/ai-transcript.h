/*
 * ai-transcript.h - The buffer: an ordered, observable list of blocks
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

#include "view/ai-view-block.h"

G_BEGIN_DECLS

/**
 * AiExportFormat:
 * @AI_EXPORT_FORMAT_TEXT: plain text, as the terminal renders it
 * @AI_EXPORT_FORMAT_MARKDOWN: CommonMark with collapsible `<details>`
 * @AI_EXPORT_FORMAT_ORG: Org mode with headings and drawers
 *
 * What #ai_transcript_export writes.
 *
 * Text is what a terminal already shows. The other two carry the same
 * content with the structure the transcript has anyway --- who said what,
 * which calls belong to which group --- made explicit, so a session can be
 * filed somewhere that understands headings.
 */
typedef enum
{
    AI_EXPORT_FORMAT_TEXT = 0,
    AI_EXPORT_FORMAT_MARKDOWN,
    AI_EXPORT_FORMAT_ORG
} AiExportFormat;

#define AI_TYPE_TRANSCRIPT (ai_transcript_get_type())

G_DECLARE_FINAL_TYPE(AiTranscript, ai_transcript, AI, TRANSCRIPT, GObject)

AiTranscript *
ai_transcript_new(void);

void
ai_transcript_append(
    AiTranscript *self,
    AiViewBlock  *block
);

AiViewBlock *
ai_transcript_get_block(
    AiTranscript *self,
    guint         position
);

AiViewBlock *
ai_transcript_get_last(AiTranscript *self);

guint
ai_transcript_get_n_blocks(AiTranscript *self);

gboolean
ai_transcript_find_block(
    AiTranscript *self,
    AiViewBlock  *block,
    guint        *out_position
);

void
ai_transcript_clear(AiTranscript *self);

gchar *
ai_transcript_to_text(
    AiTranscript *self,
    guint         width
);

gchar *
ai_transcript_export(
    AiTranscript   *self,
    AiExportFormat  format
);

const gchar *
ai_export_format_to_string(AiExportFormat format);

gboolean
ai_export_format_from_string(
    const gchar    *name,
    AiExportFormat *out_format
);

const gchar *
ai_export_format_extension(AiExportFormat format);

G_END_DECLS
