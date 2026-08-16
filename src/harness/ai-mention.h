/*
 * ai-mention.h - @file references in a prompt
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

G_BEGIN_DECLS

/**
 * AiMention:
 * @start: byte offset of the `@` within the text
 * @len: length in bytes, including the `@`
 * @path: the path as written, with no resolution applied
 *
 * One `@path` reference found in a line of input.
 *
 * Offsets are byte offsets into UTF-8, the same convention #AiStyleSpan
 * uses --- one rule for the whole library, and what Emacs converts with
 * `byte-to-position`.
 */
typedef struct
{
    guint  start;
    guint  len;
    gchar *path;
} AiMention;

#define AI_TYPE_MENTION (ai_mention_get_type())

GType
ai_mention_get_type(void) G_GNUC_CONST;

AiMention *
ai_mention_new(
    guint        start,
    guint        len,
    const gchar *path
);

AiMention *
ai_mention_copy(const AiMention *self);

void
ai_mention_free(AiMention *self);

GList *
ai_mention_scan(const gchar *text);

gchar *
ai_mention_resolve(
    const gchar *path,
    const gchar *cwd
);

gchar *
ai_mention_expand(
    const gchar  *text,
    const gchar  *cwd,
    gsize         max_bytes,
    GList       **out_files
);

/**
 * AI_MENTION_DEFAULT_MAX_BYTES:
 *
 * How much file content ai_mention_expand() will append by default.
 */
#define AI_MENTION_DEFAULT_MAX_BYTES (256 * 1024)

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiMention, ai_mention_free)

G_END_DECLS
