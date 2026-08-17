/*
 * ai-transcript.c - The buffer: an ordered, observable list of blocks
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "view/ai-transcript.h"

/* For the per-kind text getters the exporter reads instead of the
 * rendering -- see export_block_plain_text(). */
#include "view/ai-view-blocks.h"

struct _AiTranscript
{
    GObject    parent_instance;
    GPtrArray *blocks;   /* AiViewBlock, owned */
};

static void ai_transcript_list_model_init(GListModelInterface *iface);

/*
 * GListModel rather than a bare GList, which is what the rest of ai-glib
 * uses for collections.
 *
 * Two reasons. ::items-changed gives insertion and removal notification for
 * free, in the form every GLib consumer already knows. And a stable integer
 * position is exactly what a frontend that maps blocks onto its own
 * addressing -- regions of an Emacs buffer, rows of a pad -- needs to key
 * on. A GList would have neither.
 */
G_DEFINE_TYPE_WITH_CODE(AiTranscript, ai_transcript, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              ai_transcript_list_model_init))

enum
{
    SIGNAL_BLOCK_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
on_block_changed(
    AiViewBlock *block,
    gpointer     user_data
){
    AiTranscript *self = user_data;
    guint position = 0;

    /*
     * The position is looked up rather than captured at connect time,
     * because a block's position moves when earlier ones are removed and a
     * stale index would update the wrong region.
     */
    if (ai_transcript_find_block(self, block, &position))
    {
        g_signal_emit(self, signals[SIGNAL_BLOCK_CHANGED], 0, position, block);
    }
}

static GType
ai_transcript_get_item_type(GListModel *model)
{
    (void)model;
    return AI_TYPE_VIEW_BLOCK;
}

static guint
ai_transcript_get_n_items(GListModel *model)
{
    return AI_TRANSCRIPT(model)->blocks->len;
}

static gpointer
ai_transcript_get_item(GListModel *model, guint position)
{
    AiTranscript *self = AI_TRANSCRIPT(model);

    if (position >= self->blocks->len)
    {
        return NULL;
    }

    return g_object_ref(g_ptr_array_index(self->blocks, position));
}

static void
ai_transcript_list_model_init(GListModelInterface *iface)
{
    iface->get_item_type = ai_transcript_get_item_type;
    iface->get_n_items = ai_transcript_get_n_items;
    iface->get_item = ai_transcript_get_item;
}

static void
ai_transcript_finalize(GObject *object)
{
    AiTranscript *self = AI_TRANSCRIPT(object);
    guint i;

    for (i = 0; i < self->blocks->len; i++)
    {
        g_signal_handlers_disconnect_by_data(
            g_ptr_array_index(self->blocks, i), self);
    }

    g_clear_pointer(&self->blocks, g_ptr_array_unref);

    G_OBJECT_CLASS(ai_transcript_parent_class)->finalize(object);
}

static void
ai_transcript_class_init(AiTranscriptClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_transcript_finalize;

    /**
     * AiTranscript::block-changed:
     * @self: the transcript
     * @position: where the block is
     * @block: the block that changed
     *
     * Emitted when a block's content changed in place.
     *
     * #GListModel::items-changed covers insertion and removal, but a block
     * being streamed into is neither --- prose grows a delta at a time and a
     * tool group's calls change state, all without the list changing shape.
     * A frontend that only watched items-changed would show the first delta
     * of a reply and nothing after it.
     *
     * The position is current as of the emission; act on it before
     * returning, or look the block up again by id.
     */
    signals[SIGNAL_BLOCK_CHANGED] =
        g_signal_new("block-changed",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 2,
                     G_TYPE_UINT, AI_TYPE_VIEW_BLOCK);
}

static void
ai_transcript_init(AiTranscript *self)
{
    self->blocks = g_ptr_array_new_with_free_func(g_object_unref);
}

/**
 * ai_transcript_new:
 *
 * Creates an empty transcript.
 *
 * Returns: (transfer full): a new #AiTranscript
 */
AiTranscript *
ai_transcript_new(void)
{
    return g_object_new(AI_TYPE_TRANSCRIPT, NULL);
}

/**
 * ai_transcript_append:
 * @self: an #AiTranscript
 * @block: (transfer none): the block to add
 *
 * Adds @block to the end, taking a reference.
 *
 * The transcript subscribes to the block's ::changed so it can relay it as
 * ::block-changed with a position. A frontend therefore only ever has to
 * watch the transcript, never the individual blocks.
 */
void
ai_transcript_append(
    AiTranscript *self,
    AiViewBlock  *block
){
    guint position;

    g_return_if_fail(AI_IS_TRANSCRIPT(self));
    g_return_if_fail(AI_IS_VIEW_BLOCK(block));

    position = self->blocks->len;

    g_ptr_array_add(self->blocks, g_object_ref(block));
    g_signal_connect(block, "changed", G_CALLBACK(on_block_changed), self);

    g_list_model_items_changed(G_LIST_MODEL(self), position, 0, 1);
}

/**
 * ai_transcript_get_block:
 * @self: an #AiTranscript
 * @position: which block
 *
 * Like g_list_model_get_item() but without taking a reference, for the
 * common case of looking at a block and being done with it.
 *
 * Returns: (transfer none) (nullable): the block, or %NULL if out of range
 */
AiViewBlock *
ai_transcript_get_block(
    AiTranscript *self,
    guint         position
){
    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), NULL);

    if (position >= self->blocks->len)
    {
        return NULL;
    }

    return g_ptr_array_index(self->blocks, position);
}

/**
 * ai_transcript_get_last:
 * @self: an #AiTranscript
 *
 * The most recent block.
 *
 * This is what the event folding keys on: whether a text delta continues
 * the current prose or starts a new one after a tool group is decided by
 * looking at what is currently last.
 *
 * Returns: (transfer none) (nullable): the last block, or %NULL when empty
 */
AiViewBlock *
ai_transcript_get_last(AiTranscript *self)
{
    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), NULL);

    if (self->blocks->len == 0)
    {
        return NULL;
    }

    return g_ptr_array_index(self->blocks, self->blocks->len - 1);
}

/**
 * ai_transcript_get_n_blocks:
 * @self: an #AiTranscript
 *
 * Returns: how many blocks there are
 */
guint
ai_transcript_get_n_blocks(AiTranscript *self)
{
    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), 0);

    return self->blocks->len;
}

/**
 * ai_transcript_find_block:
 * @self: an #AiTranscript
 * @block: (transfer none): the block to locate
 * @out_position: (out) (optional): where it is
 *
 * Finds a block's current position.
 *
 * Returns: %TRUE if @block is in this transcript
 */
gboolean
ai_transcript_find_block(
    AiTranscript *self,
    AiViewBlock  *block,
    guint        *out_position
){
    guint i;

    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), FALSE);

    for (i = 0; i < self->blocks->len; i++)
    {
        if (g_ptr_array_index(self->blocks, i) == block)
        {
            if (out_position != NULL)
            {
                *out_position = i;
            }

            return TRUE;
        }
    }

    return FALSE;
}

/**
 * ai_transcript_clear:
 * @self: an #AiTranscript
 *
 * Removes every block.
 *
 * Handlers are disconnected first: a block a caller still holds a reference
 * to may go on changing, and it must not still be reporting those changes
 * to a transcript it is no longer part of.
 */
void
ai_transcript_clear(AiTranscript *self)
{
    guint removed;
    guint i;

    g_return_if_fail(AI_IS_TRANSCRIPT(self));

    removed = self->blocks->len;

    if (removed == 0)
    {
        return;
    }

    for (i = 0; i < self->blocks->len; i++)
    {
        g_signal_handlers_disconnect_by_data(
            g_ptr_array_index(self->blocks, i), self);
    }

    g_ptr_array_set_size(self->blocks, 0);
    g_list_model_items_changed(G_LIST_MODEL(self), 0, removed, 0);
}

/**
 * ai_transcript_to_text:
 * @self: an #AiTranscript
 * @width: the width to wrap to, or 0 for no wrapping
 *
 * The whole transcript as plain text, one blank line between blocks.
 *
 * What makes the layer testable and pipeable: `ai-tui --dump` is this
 * function and a printf. It is also the fallback for a frontend that cannot
 * do styling at all.
 *
 * Returns: (transfer full): the text
 */
gchar *
ai_transcript_to_text(
    AiTranscript *self,
    guint         width
){
    g_autoptr(GString) out = NULL;
    guint i;

    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), g_strdup(""));

    out = g_string_new(NULL);

    for (i = 0; i < self->blocks->len; i++)
    {
        AiViewBlock *block = g_ptr_array_index(self->blocks, i);
        g_autofree gchar *text = ai_view_block_render_text(block, width);

        if (text == NULL || text[0] == '\0')
        {
            continue;
        }

        if (out->len > 0)
        {
            g_string_append(out, "\n\n");
        }

        g_string_append(out, text);
    }

    if (out->len > 0)
    {
        g_string_append_c(out, '\n');
    }

    return g_strdup(out->str);
}

/* ================================================================
 * Export
 * ================================================================ */

/*
 * One row per format, so that adding a fourth is a table entry rather
 * than a fourth branch in each of five functions. Same reasoning as
 * AiToolStyle and AiImageModelInfo: the table is the registration.
 *
 * `heading` takes the section title, `open`/`close` wrap the content a
 * reader would rather fold away, and `verbatim` wraps content that must
 * not be re-interpreted as markup.
 */
/*
 * Titles are surrounded by a prefix/suffix pair rather than substituted
 * into a "## %s\n" template. A format string pulled from a table is a
 * -Wformat-nonliteral, and suppressing that warning to keep a cosmetic
 * convenience is the wrong trade when concatenation reads the same.
 */
typedef struct
{
    AiExportFormat  format;
    const gchar    *name;
    const gchar    *extension;
    const gchar    *heading_open;
    const gchar    *heading_close;
    const gchar    *fold_open;       /* precedes the title */
    const gchar    *fold_mid;        /* follows it */
    const gchar    *fold_close;
    const gchar    *verbatim_open;
    const gchar    *verbatim_close;
    const gchar    *quote_open;      /* used when quote_prefix is NULL */
    const gchar    *quote_close;
    const gchar    *quote_prefix;    /* per line, for formats without a block */
} ExportStyle;

/*
 * Markdown has no block quote, only a per-line marker, so it carries a
 * prefix where org carries a wrapper. Both spellings are here rather than
 * a per-format branch in the writer.
 */
static const ExportStyle EXPORT_STYLES[] = {
    { AI_EXPORT_FORMAT_TEXT, "text", "txt",
      "", "\n",
      "", "\n", "",
      "", "",
      "", "", NULL },
    { AI_EXPORT_FORMAT_MARKDOWN, "markdown", "md",
      "## ", "\n",
      "<details>\n<summary>", "</summary>\n\n", "\n</details>\n",
      "```\n", "```\n",
      NULL, NULL, "> " },
    { AI_EXPORT_FORMAT_ORG, "org", "org",
      "** ", "\n",
      "** ", "\n", "",
      "#+begin_example\n", "#+end_example\n",
      "#+begin_quote\n", "#+end_quote\n", NULL },
    { 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
      NULL }
};

static const ExportStyle *
export_style_for(AiExportFormat format)
{
    const ExportStyle *style;
    const ExportStyle *fallback = EXPORT_STYLES;

    for (style = EXPORT_STYLES; style->name != NULL; style++)
    {
        if (style->format == format)
        {
            return style;
        }

        if (style->format == AI_EXPORT_FORMAT_MARKDOWN)
        {
            fallback = style;
        }
    }

    /* Found by value, not by index: a row moved in the table above must
     * not silently change what an out-of-range format resolves to. */
    return fallback;
}

/**
 * ai_export_format_to_string:
 * @format: an #AiExportFormat
 *
 * The name a user types: "text", "markdown" or "org".
 *
 * Returns: (transfer none): the name, never %NULL
 */
const gchar *
ai_export_format_to_string(AiExportFormat format)
{
    return export_style_for(format)->name;
}

/**
 * ai_export_format_extension:
 * @format: an #AiExportFormat
 *
 * The file extension, without the dot.
 *
 * Here rather than in each frontend because `/export org` has to pick a
 * filename when the user gives a directory, and `ai-tui` and an Emacs
 * client choosing differently would be a difference with no reason.
 *
 * Returns: (transfer none): the extension, never %NULL
 */
const gchar *
ai_export_format_extension(AiExportFormat format)
{
    return export_style_for(format)->extension;
}

/**
 * ai_export_format_from_string:
 * @name: (nullable): a format name, or %NULL
 * @out_format: (out) (optional): the format @name names
 *
 * Parses a format name, case-insensitively. "md" and "markdown" are the
 * same answer, as are "org" and "org-mode".
 *
 * Returns %FALSE rather than defaulting, so that `/export mardkown` says
 * so instead of quietly writing a format the user did not ask for.
 *
 * Returns: %TRUE when @name is a format
 */
gboolean
ai_export_format_from_string(
    const gchar    *name,
    AiExportFormat *out_format
){
    const ExportStyle *style;

    if (name == NULL || name[0] == '\0')
    {
        return FALSE;
    }

    for (style = EXPORT_STYLES; style->name != NULL; style++)
    {
        if (g_ascii_strcasecmp(name, style->name) == 0)
        {
            if (out_format != NULL)
            {
                *out_format = style->format;
            }

            return TRUE;
        }
    }

    if (g_ascii_strcasecmp(name, "md") == 0)
    {
        if (out_format != NULL)
        {
            *out_format = AI_EXPORT_FORMAT_MARKDOWN;
        }

        return TRUE;
    }

    if (g_ascii_strcasecmp(name, "org-mode") == 0)
    {
        if (out_format != NULL)
        {
            *out_format = AI_EXPORT_FORMAT_ORG;
        }

        return TRUE;
    }

    return FALSE;
}

/*
 * The title a block gets in an export.
 *
 * Deliberately says who is speaking rather than naming the block class:
 * a document is read by someone who does not know this library's types.
 */
static const gchar *
export_block_title(AiViewBlock *block)
{
    switch (ai_view_block_get_kind(block))
    {
        case AI_VIEW_BLOCK_TURN:     return "You";
        case AI_VIEW_BLOCK_TEXT:     return "Assistant";
        case AI_VIEW_BLOCK_THINKING: return "Thinking";
        case AI_VIEW_BLOCK_TOOL:     return "Tools";
        case AI_VIEW_BLOCK_STATUS:   return "Status";
        case AI_VIEW_BLOCK_TODO:     return "Todo";
        case AI_VIEW_BLOCK_AGENT:    return "Background agents";
        default:                     return "Block";
    }
}

/*
 * How a block's content has to be treated by the target format.
 *
 * Assistant and user prose is already markdown in practice --- models
 * write fenced code and lists --- so re-escaping it would turn a working
 * document into a wall of backslashes. Tool output is the opposite: it is
 * whatever a command printed, and a diff line beginning with `#` must not
 * become a heading. A status note is neither: it is this program talking
 * about the conversation, and a quote is what says so.
 */
typedef enum
{
    EXPORT_BODY_PROSE = 0,
    EXPORT_BODY_VERBATIM,
    EXPORT_BODY_QUOTE
} ExportBodyKind;

static ExportBodyKind
export_block_body_kind(AiViewBlock *block)
{
    switch (ai_view_block_get_kind(block))
    {
        case AI_VIEW_BLOCK_TOOL:
        case AI_VIEW_BLOCK_TODO:
        case AI_VIEW_BLOCK_AGENT:
            return EXPORT_BODY_VERBATIM;
        case AI_VIEW_BLOCK_STATUS:
            return EXPORT_BODY_QUOTE;
        default:
            return EXPORT_BODY_PROSE;
    }
}

/*
 * Whether a reader would rather this block started folded.
 *
 * The same judgement the display makes, for the same reason -- reasoning
 * and tool output are worth keeping and not worth reading first.
 */
static gboolean
export_block_is_folded(AiViewBlock *block)
{
    AiViewBlockKind kind = ai_view_block_get_kind(block);

    return kind == AI_VIEW_BLOCK_THINKING || kind == AI_VIEW_BLOCK_TOOL;
}

/* @text with a trailing newline guaranteed, so every caller below can
 * append its closing delimiter on a line of its own. */
static void
export_append_terminated(
    GString     *out,
    const gchar *text
){
    gsize len = strlen(text);

    g_string_append(out, text);

    if (len == 0 || text[len - 1] != '\n')
    {
        g_string_append_c(out, '\n');
    }
}

static void
export_append_quoted(
    GString     *out,
    const gchar *prefix,
    const gchar *text
){
    g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
    gsize i;

    for (i = 0; lines[i] != NULL; i++)
    {
        /* g_strsplit on a trailing newline leaves a final empty field;
         * emitting "> " for it would put a stray marker at the end. */
        if (lines[i][0] == '\0' && lines[i + 1] == NULL)
        {
            break;
        }

        g_string_append(out, prefix);
        g_string_append(out, lines[i]);
        g_string_append_c(out, '\n');
    }
}

/*
 * The words a block carries, without the display's chrome.
 *
 * The rendering is not the content. A turn renders with a leading "> ", a
 * thinking block with "✳ thinking ⌄", a status with "✖" -- markers that
 * orient a reader on a screen and are noise in a file, where the heading
 * above already says which is which. Worse, they are actively wrong in
 * org, whose syntax gives "> " no meaning at all.
 *
 * So the four text-bearing kinds are read through their own getters.
 * Tool, todo and agent blocks have no such getter, and should not: their
 * markers (✓, ✗, +21-6) *are* the information, and a summary stripped of
 * them would say a tool ran without saying whether it worked.
 *
 * Returns %NULL when the block has no plain text of its own, which means
 * "render it instead".
 */
static const gchar *
export_block_plain_text(AiViewBlock *block)
{
    switch (ai_view_block_get_kind(block))
    {
        case AI_VIEW_BLOCK_TURN:
            return ai_view_turn_block_get_text(AI_VIEW_TURN_BLOCK(block));
        case AI_VIEW_BLOCK_TEXT:
            return ai_view_text_block_get_text(AI_VIEW_TEXT_BLOCK(block));
        case AI_VIEW_BLOCK_THINKING:
            return ai_view_thinking_block_get_text(
                AI_VIEW_THINKING_BLOCK(block));
        case AI_VIEW_BLOCK_STATUS:
            return ai_view_status_block_get_text(
                AI_VIEW_STATUS_BLOCK(block));
        default:
            return NULL;
    }
}

static void
export_append_block(
    GString           *out,
    const ExportStyle *style,
    AiViewBlock       *block
){
    g_autoptr(AiRenderedText) rendered = NULL;
    const gchar *text = export_block_plain_text(block);
    const gchar *title = export_block_title(block);
    gboolean folded = export_block_is_folded(block);

    if (text == NULL)
    {
        /* Expanded, always: a collapsed tool group renders as one summary
         * line, and a file that recorded "Edited 3 files" without saying
         * which is not a record of anything. */
        rendered = ai_view_block_render_expanded(block, 0);
        text = ai_rendered_text_get_text(rendered);
    }

    if (text == NULL || text[0] == '\0')
    {
        return;
    }

    if (out->len > 0)
    {
        g_string_append_c(out, '\n');
    }

    g_string_append(out, folded ? style->fold_open : style->heading_open);
    g_string_append(out, title);
    g_string_append(out, folded ? style->fold_mid : style->heading_close);

    if (!folded)
    {
        g_string_append_c(out, '\n');
    }

    switch (export_block_body_kind(block))
    {
        case EXPORT_BODY_VERBATIM:
            g_string_append(out, style->verbatim_open);
            export_append_terminated(out, text);
            g_string_append(out, style->verbatim_close);
            break;

        case EXPORT_BODY_QUOTE:
            if (style->quote_prefix != NULL)
            {
                export_append_quoted(out, style->quote_prefix, text);
            }
            else
            {
                g_string_append(out, style->quote_open);
                export_append_terminated(out, text);
                g_string_append(out, style->quote_close);
            }
            break;

        case EXPORT_BODY_PROSE:
        default:
            export_append_terminated(out, text);
            break;
    }

    if (folded)
    {
        g_string_append(out, style->fold_close);
    }
}

/**
 * ai_transcript_export:
 * @self: an #AiTranscript
 * @format: what to write
 *
 * Renders the whole transcript as a document.
 *
 * %AI_EXPORT_FORMAT_TEXT is exactly ai_transcript_to_text() at width 0, so
 * a frontend has one export entry point rather than a special case for the
 * format it started with.
 *
 * Every block is rendered *expanded* whatever its display state --- see
 * ai_view_block_render_expanded(). Tool output and todo lists are wrapped
 * verbatim, because a command's stdout is not markdown and a diff line
 * starting with `#` must not become a heading. Prose is passed through, on
 * the grounds that models already write markdown and escaping it would
 * ruin every code fence in the answer.
 *
 * Returns: (transfer full): the document
 */
gchar *
ai_transcript_export(
    AiTranscript   *self,
    AiExportFormat  format
){
    const ExportStyle *style;
    g_autoptr(GString) out = NULL;
    guint i;

    g_return_val_if_fail(AI_IS_TRANSCRIPT(self), g_strdup(""));

    if (format == AI_EXPORT_FORMAT_TEXT)
    {
        return ai_transcript_to_text(self, 0);
    }

    style = export_style_for(format);
    out = g_string_new(NULL);

    for (i = 0; i < self->blocks->len; i++)
    {
        export_append_block(out, style,
                            g_ptr_array_index(self->blocks, i));
    }

    return g_strdup(out->str);
}
