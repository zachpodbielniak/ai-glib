/*
 * ai-style.c - The rendering contract between the library and a frontend
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "view/ai-style.h"

struct _AiRenderedText
{
    GString *text;
    GArray  *spans;   /* AiStyleSpan, sorted, non-overlapping */
    gint     ref_count;
};

G_DEFINE_BOXED_TYPE(AiStyleSpan, ai_style_span,
                    ai_style_span_copy, ai_style_span_free)

G_DEFINE_BOXED_TYPE(AiRenderedText, ai_rendered_text,
                    ai_rendered_text_ref, ai_rendered_text_unref)

/*
 * The names a frontend maps to its own notion of appearance.
 *
 * Indexed by AiStyleTag, so the table and the enum must stay in step ---
 * tests/test-view-style.c walks every value and asserts each one is present
 * and distinct, which is what catches a forgotten entry.
 */
static const gchar * const TAG_NAMES[AI_STYLE_N_TAGS] = {
    "default",
    "user-prompt",
    "heading",
    "dim",
    "tool-name",
    "tool-target",
    "tool-pending",
    "tool-ok",
    "tool-failed",
    "added",
    "removed",
    "code",
    "thinking",
    "error",
    "status",
    "link",
    "marker",
    "mention",
    "command",
    "todo-pending",
    "todo-active",
    "todo-done"
};

/**
 * ai_style_span_copy:
 * @self: (nullable): an #AiStyleSpan
 *
 * Returns: (transfer full) (nullable): a copy of @self
 */
AiStyleSpan *
ai_style_span_copy(const AiStyleSpan *self)
{
    AiStyleSpan *copy;

    if (self == NULL)
    {
        return NULL;
    }

    copy = g_new0(AiStyleSpan, 1);
    *copy = *self;

    return copy;
}

/**
 * ai_style_span_free:
 * @self: (nullable): an #AiStyleSpan
 *
 * Frees @self. Does nothing if @self is %NULL.
 */
void
ai_style_span_free(AiStyleSpan *self)
{
    g_free(self);
}

/**
 * ai_rendered_text_new:
 *
 * Creates an empty #AiRenderedText, ready to be built up with
 * ai_rendered_text_append().
 *
 * Returns: (transfer full): a new #AiRenderedText
 */
AiRenderedText *
ai_rendered_text_new(void)
{
    AiRenderedText *self;

    self = g_slice_new0(AiRenderedText);
    self->text = g_string_new(NULL);
    self->spans = g_array_new(FALSE, FALSE, sizeof(AiStyleSpan));
    self->ref_count = 1;

    return self;
}

/**
 * ai_rendered_text_ref:
 * @self: (nullable): an #AiRenderedText
 *
 * Returns: (transfer full) (nullable): @self with one more reference
 */
AiRenderedText *
ai_rendered_text_ref(AiRenderedText *self)
{
    if (self == NULL)
    {
        return NULL;
    }

    g_atomic_int_inc(&self->ref_count);

    return self;
}

/**
 * ai_rendered_text_unref:
 * @self: (nullable): an #AiRenderedText
 *
 * Drops a reference, freeing @self when the last one goes.
 */
void
ai_rendered_text_unref(AiRenderedText *self)
{
    if (self == NULL)
    {
        return;
    }

    if (!g_atomic_int_dec_and_test(&self->ref_count))
    {
        return;
    }

    g_string_free(self->text, TRUE);
    g_array_unref(self->spans);
    g_slice_free(AiRenderedText, self);
}

/**
 * ai_rendered_text_get_text:
 * @self: an #AiRenderedText
 *
 * Returns: (transfer none): the rendered text, never %NULL
 */
const gchar *
ai_rendered_text_get_text(AiRenderedText *self)
{
    g_return_val_if_fail(self != NULL, "");

    return self->text->str;
}

/**
 * ai_rendered_text_get_length:
 * @self: an #AiRenderedText
 *
 * Returns: the length of the text in bytes
 */
guint
ai_rendered_text_get_length(AiRenderedText *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return (guint)self->text->len;
}

/**
 * ai_rendered_text_get_n_spans:
 * @self: an #AiRenderedText
 *
 * Returns: how many styled runs there are
 */
guint
ai_rendered_text_get_n_spans(AiRenderedText *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->spans->len;
}

/**
 * ai_rendered_text_get_span:
 * @self: an #AiRenderedText
 * @index_: which span, from 0 to ai_rendered_text_get_n_spans()
 * @out_start: (out) (optional): byte offset of the run
 * @out_len: (out) (optional): length of the run in bytes
 * @out_tag: (out) (optional): how it should look
 *
 * Reads one styled run.
 *
 * Out parameters rather than a returned array because a #GArray of a plain
 * struct does not survive GObject Introspection, and this API has to be
 * usable from the bindings an editor would drive it through. The array stays
 * private; this is the whole of the read interface.
 *
 * Spans are sorted by @start, never overlap, and are never empty.
 *
 * Returns: %TRUE if @index_ was in range; the out parameters are untouched
 *   otherwise
 */
gboolean
ai_rendered_text_get_span(
    AiRenderedText *self,
    guint           index_,
    guint          *out_start,
    guint          *out_len,
    AiStyleTag     *out_tag
){
    AiStyleSpan *span;

    g_return_val_if_fail(self != NULL, FALSE);

    if (index_ >= self->spans->len)
    {
        return FALSE;
    }

    span = &g_array_index(self->spans, AiStyleSpan, index_);

    if (out_start != NULL) *out_start = span->start;
    if (out_len != NULL)   *out_len = span->len;
    if (out_tag != NULL)   *out_tag = span->tag;

    return TRUE;
}

/**
 * ai_rendered_text_get_tag_at:
 * @self: an #AiRenderedText
 * @offset: a byte offset into the text
 *
 * The tag covering @offset, or %AI_STYLE_DEFAULT where no span does.
 *
 * Convenient for a renderer that walks the text a character at a time
 * instead of a span at a time.
 *
 * Returns: the tag at @offset
 */
AiStyleTag
ai_rendered_text_get_tag_at(
    AiRenderedText *self,
    guint           offset
){
    guint i;

    g_return_val_if_fail(self != NULL, AI_STYLE_DEFAULT);

    for (i = 0; i < self->spans->len; i++)
    {
        AiStyleSpan *span = &g_array_index(self->spans, AiStyleSpan, i);

        if (offset < span->start)
        {
            break;   /* sorted, so nothing later can cover it either */
        }

        if (offset < span->start + span->len)
        {
            return span->tag;
        }
    }

    return AI_STYLE_DEFAULT;
}

/**
 * ai_rendered_text_append:
 * @self: an #AiRenderedText
 * @text: (nullable): the text to append
 * @tag: how it should look
 *
 * Appends @text, recording a span for it unless @tag is %AI_STYLE_DEFAULT.
 *
 * Adjacent runs of the same tag are merged, so a builder can append a word
 * at a time without producing a span per word.
 */
void
ai_rendered_text_append(
    AiRenderedText *self,
    const gchar    *text,
    AiStyleTag      tag
){
    guint start;
    gsize len;

    g_return_if_fail(self != NULL);

    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    start = (guint)self->text->len;
    len = strlen(text);

    g_string_append(self->text, text);

    if (tag == AI_STYLE_DEFAULT)
    {
        return;
    }

    if (self->spans->len > 0)
    {
        AiStyleSpan *last =
            &g_array_index(self->spans, AiStyleSpan, self->spans->len - 1);

        if (last->tag == tag && last->start + last->len == start)
        {
            last->len += (guint)len;
            return;
        }
    }

    {
        AiStyleSpan span;

        span.start = start;
        span.len = (guint)len;
        span.tag = tag;

        g_array_append_val(self->spans, span);
    }
}

/**
 * ai_rendered_text_append_printf:
 * @self: an #AiRenderedText
 * @tag: how the result should look
 * @format: a printf format string
 * @...: arguments for @format
 *
 * Formats and appends, as ai_rendered_text_append() would.
 */
void
ai_rendered_text_append_printf(
    AiRenderedText *self,
    AiStyleTag      tag,
    const gchar    *format,
    ...
){
    g_autofree gchar *formatted = NULL;
    va_list args;

    g_return_if_fail(self != NULL);
    g_return_if_fail(format != NULL);

    va_start(args, format);
    formatted = g_strdup_vprintf(format, args);
    va_end(args);

    ai_rendered_text_append(self, formatted, tag);
}

/**
 * ai_style_text_width:
 * @text: (nullable): UTF-8 text
 *
 * How many terminal columns @text occupies.
 *
 * Columns, not characters: a CJK ideograph is one character and two columns,
 * and a renderer that counted characters would misalign every line
 * containing one. Control characters count as zero.
 *
 * Returns: the width in columns
 */
guint
ai_style_text_width(const gchar *text)
{
    const gchar *p;
    guint width = 0;

    if (text == NULL)
    {
        return 0;
    }

    for (p = text; *p != '\0'; p = g_utf8_next_char(p))
    {
        gunichar c = g_utf8_get_char(p);

        if (c == '\n' || g_unichar_iszerowidth(c))
        {
            continue;
        }

        width += g_unichar_iswide(c) ? 2 : 1;
    }

    return width;
}

/*
 * Copy the spans covering [from, to) into @dest, shifted to land at
 * @dest_start.
 *
 * A span that straddles either end is clipped rather than dropped, which is
 * what keeps wrapping from losing the styling of a word it had to break.
 */
static void
copy_spans_range(
    AiRenderedText *dest,
    AiRenderedText *src,
    guint           from,
    guint           to,
    guint           dest_start
){
    guint i;

    for (i = 0; i < src->spans->len; i++)
    {
        AiStyleSpan *span = &g_array_index(src->spans, AiStyleSpan, i);
        guint span_end = span->start + span->len;
        guint clipped_start;
        guint clipped_end;
        AiStyleSpan out;

        if (span_end <= from)
        {
            continue;
        }

        if (span->start >= to)
        {
            break;   /* sorted */
        }

        clipped_start = MAX(span->start, from);
        clipped_end = MIN(span_end, to);

        if (clipped_end <= clipped_start)
        {
            continue;
        }

        out.start = dest_start + (clipped_start - from);
        out.len = clipped_end - clipped_start;
        out.tag = span->tag;

        /* Merge with the previous run when they meet and agree. */
        if (dest->spans->len > 0)
        {
            AiStyleSpan *last = &g_array_index(dest->spans, AiStyleSpan,
                                               dest->spans->len - 1);

            if (last->tag == out.tag && last->start + last->len == out.start)
            {
                last->len += out.len;
                continue;
            }
        }

        g_array_append_val(dest->spans, out);
    }
}

/**
 * ai_rendered_text_wrap:
 * @self: an #AiRenderedText
 * @width: the width to wrap to in terminal columns, or 0 for no wrapping
 *
 * Re-flows @self to @width, breaking at spaces where it can.
 *
 * Returns a new instance; @self is unchanged, which is what lets a block
 * cache one unwrapped rendering and wrap it to whatever width each frontend
 * currently is. A width of 0 means "do not wrap" --- the right answer for
 * Emacs, which does its own filling, and it hands back a plain reference
 * rather than a copy.
 *
 * A word longer than @width is broken rather than allowed to overflow, and
 * the break never falls inside a multi-byte character. Spans that straddle a
 * break are split so both halves keep their styling.
 *
 * Returns: (transfer full): the wrapped text
 */
AiRenderedText *
ai_rendered_text_wrap(
    AiRenderedText *self,
    guint           width
){
    AiRenderedText *out;
    const gchar *text;
    const gchar *line_start;
    const gchar *p;
    const gchar *last_space;   /* last break opportunity, or NULL */
    guint column = 0;

    g_return_val_if_fail(self != NULL, NULL);

    if (width == 0)
    {
        return ai_rendered_text_ref(self);
    }

    out = ai_rendered_text_new();
    text = self->text->str;
    line_start = text;
    last_space = NULL;

    for (p = text; *p != '\0'; )
    {
        gunichar c = g_utf8_get_char(p);
        const gchar *next = g_utf8_next_char(p);
        guint cw;

        if (c == '\n')
        {
            /* An existing newline ends the line as it stands. */
            guint from = (guint)(line_start - text);
            guint to = (guint)(p - text);

            copy_spans_range(out, self, from, to,
                             (guint)out->text->len);
            g_string_append_len(out->text, line_start, p - line_start);
            g_string_append_c(out->text, '\n');

            p = next;
            line_start = p;
            last_space = NULL;
            column = 0;
            continue;
        }

        cw = g_unichar_iszerowidth(c) ? 0 : (g_unichar_iswide(c) ? 2 : 1);

        if (column + cw > width && p > line_start)
        {
            /*
             * Break at the last space when there was one, otherwise here ---
             * a single word longer than the line has to be split somewhere,
             * and overflowing is worse than breaking.
             */
            const gchar *break_at = last_space != NULL ? last_space : p;
            const gchar *resume = last_space != NULL
                ? g_utf8_next_char(last_space)
                : p;
            guint from = (guint)(line_start - text);
            guint to = (guint)(break_at - text);

            copy_spans_range(out, self, from, to, (guint)out->text->len);
            g_string_append_len(out->text, line_start, break_at - line_start);
            g_string_append_c(out->text, '\n');

            line_start = resume;
            last_space = NULL;
            column = 0;

            /* Re-measure from the resume point; p may be behind it. */
            if (p < line_start)
            {
                p = line_start;
            }

            {
                const gchar *q;

                column = 0;
                for (q = line_start; q < p; q = g_utf8_next_char(q))
                {
                    gunichar qc = g_utf8_get_char(q);
                    column += g_unichar_iszerowidth(qc)
                        ? 0
                        : (g_unichar_iswide(qc) ? 2 : 1);
                }
            }

            continue;
        }

        if (c == ' ')
        {
            last_space = p;
        }

        column += cw;
        p = next;
    }

    /* Whatever is left over is the final line. */
    if (p > line_start)
    {
        guint from = (guint)(line_start - text);
        guint to = (guint)(p - text);

        copy_spans_range(out, self, from, to, (guint)out->text->len);
        g_string_append_len(out->text, line_start, p - line_start);
    }

    return out;
}

/**
 * ai_style_tag_to_string:
 * @tag: an #AiStyleTag
 *
 * A stable lowercase name for @tag.
 *
 * This is what makes a frontend's mapping table cheap: Emacs turns
 * "tool-name" into `ai-glib-face-tool-name` with one `intern`, and an
 * ncurses renderer uses it to look up a colour pair. The names are part of
 * the API and will not change.
 *
 * Returns: (transfer none): the name, never %NULL
 */
const gchar *
ai_style_tag_to_string(AiStyleTag tag)
{
    if ((guint)tag >= AI_STYLE_N_TAGS)
    {
        return "default";
    }

    return TAG_NAMES[tag];
}

/**
 * ai_style_tag_from_string:
 * @name: (nullable): a tag name
 *
 * The inverse of ai_style_tag_to_string().
 *
 * Returns: the tag, or %AI_STYLE_DEFAULT if @name is not one
 */
AiStyleTag
ai_style_tag_from_string(const gchar *name)
{
    guint i;

    if (name == NULL)
    {
        return AI_STYLE_DEFAULT;
    }

    for (i = 0; i < AI_STYLE_N_TAGS; i++)
    {
        if (g_strcmp0(TAG_NAMES[i], name) == 0)
        {
            return (AiStyleTag)i;
        }
    }

    return AI_STYLE_DEFAULT;
}
