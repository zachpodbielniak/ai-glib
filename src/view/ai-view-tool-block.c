/*
 * ai-view-tool-block.c - A group of tool calls, summarised
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * This is the file the whole view layer exists for. Everything else models
 * a conversation; this decides that five calls read as
 *
 *     Edited 3 files, ran 2 commands  +21-6 >
 *
 * and one call reads as
 *
 *     Edited ai-style.c  +9-12 >
 *
 * Getting that right once, here, is what lets an ncurses frontend and an
 * Emacs one show the same thing without either of them knowing what a tool
 * is.
 */

#include "config.h"

#include "view/ai-view-tool-block.h"
#include "view/ai-tool-style.h"

struct _AiViewToolBlock
{
    AiViewBlock parent_instance;
    GPtrArray  *calls;   /* AiToolCall, owned, in the order they started */
};

G_DEFINE_TYPE(AiViewToolBlock, ai_view_tool_block, AI_TYPE_VIEW_BLOCK)

/* Everything one category contributes to the summary. */
typedef struct
{
    guint        count;
    guint        failures;
    AiToolCall  *only;      /* the sole call, when count == 1 */
    const gchar *verb;
    const gchar *noun_singular;
    const gchar *noun_plural;
} Bucket;

static AiViewBlockKind
tool_get_kind(AiViewBlock *block)
{
    (void)block;
    return AI_VIEW_BLOCK_TOOL;
}

/*
 * Bucket the calls by category, in first-seen order.
 *
 * Order matters: "Edited 3 files, ran 2 commands" reads as a sequence of
 * what happened, so the categories have to appear in the order they were
 * first used rather than in enum order.
 */
static void
collect_buckets(
    AiViewToolBlock *self,
    Bucket          *buckets,
    AiToolCategory  *order,
    guint           *n_order
){
    guint i;

    *n_order = 0;

    for (i = 0; i < self->calls->len; i++)
    {
        AiToolCall *call = g_ptr_array_index(self->calls, i);
        AiToolCategory category = ai_tool_call_get_category(call);
        const AiToolStyle *style = ai_tool_style_lookup(ai_tool_call_get_name(call));
        Bucket *bucket = &buckets[category];

        if (bucket->count == 0)
        {
            order[(*n_order)++] = category;

            /*
             * The first call's wording speaks for the bucket. Two tools can
             * share a category with different verbs -- `write` says
             * "Created" and `edit` says "Edited" -- and picking one is
             * better than inventing a third that fits neither.
             */
            bucket->verb = style != NULL
                ? style->verb
                : ai_tool_category_verb(category);
            bucket->noun_singular = style != NULL
                ? style->noun_singular
                : ai_tool_category_noun(category, FALSE);
            bucket->noun_plural = style != NULL
                ? style->noun_plural
                : ai_tool_category_noun(category, TRUE);
        }

        bucket->count++;
        bucket->only = bucket->count == 1 ? call : NULL;

        if (ai_tool_call_get_state(call) == AI_TOOL_CALL_FAILED ||
            ai_tool_call_get_state(call) == AI_TOOL_CALL_DENIED)
        {
            bucket->failures++;
        }
    }
}

/* Lowercase the first character of a phrase, for joining clauses. */
static void
append_lowercased_first(
    AiRenderedText *out,
    const gchar    *word,
    AiStyleTag      tag
){
    g_autofree gchar *lowered = NULL;

    if (word == NULL || word[0] == '\0')
    {
        return;
    }

    lowered = g_utf8_strdown(word, g_utf8_next_char(word) - word);
    ai_rendered_text_append(out, lowered, tag);
    ai_rendered_text_append(out, g_utf8_next_char(word), tag);
}

/*
 * The collapsed one-line summary.
 *
 * "Edited ai-style.c" when a bucket holds exactly one call with a known
 * target, "Edited 3 files" otherwise, joined with commas and with every
 * clause after the first lowercased.
 */
static void
render_summary_line(
    AiViewToolBlock *self,
    AiRenderedText  *out
){
    Bucket buckets[AI_TOOL_N_CATEGORIES];
    AiToolCategory order[AI_TOOL_N_CATEGORIES];
    guint n_order = 0;
    guint added = 0;
    guint removed = 0;
    guint failures = 0;
    guint i;

    memset(buckets, 0, sizeof buckets);
    collect_buckets(self, buckets, order, &n_order);

    for (i = 0; i < n_order; i++)
    {
        Bucket *bucket = &buckets[order[i]];
        gboolean first = (i == 0);
        const gchar *target = bucket->only != NULL
            ? ai_tool_call_get_target(bucket->only)
            : NULL;

        if (!first)
        {
            ai_rendered_text_append(out, ", ", AI_STYLE_DEFAULT);
        }

        if (first)
        {
            ai_rendered_text_append(out, bucket->verb, AI_STYLE_TOOL_NAME);
        }
        else
        {
            append_lowercased_first(out, bucket->verb, AI_STYLE_TOOL_NAME);
        }

        ai_rendered_text_append(out, " ", AI_STYLE_DEFAULT);

        if (target != NULL)
        {
            /* One call with a name worth showing: show the name. */
            ai_rendered_text_append(out, target, AI_STYLE_TOOL_TARGET);
        }
        else
        {
            ai_rendered_text_append_printf(out, AI_STYLE_DEFAULT, "%u ",
                                           bucket->count);
            ai_rendered_text_append(out,
                                    bucket->count == 1
                                        ? bucket->noun_singular
                                        : bucket->noun_plural,
                                    AI_STYLE_DEFAULT);
        }

        failures += bucket->failures;
    }

    added = ai_view_tool_block_get_lines_added(self);
    removed = ai_view_tool_block_get_lines_removed(self);

    /*
     * The diff figure is omitted entirely when nothing changed, rather than
     * shown as "+0-0" -- a group that only ran commands should not carry a
     * diff summary at all.
     */
    if (added > 0 || removed > 0)
    {
        ai_rendered_text_append(out, "  ", AI_STYLE_DEFAULT);
        ai_rendered_text_append_printf(out, AI_STYLE_ADDED, "+%u", added);
        ai_rendered_text_append_printf(out, AI_STYLE_REMOVED, "-%u", removed);
    }

    if (failures > 0)
    {
        ai_rendered_text_append(out, "  ", AI_STYLE_DEFAULT);
        ai_rendered_text_append_printf(out, AI_STYLE_TOOL_FAILED,
                                       failures == 1
                                           ? "(%u failed)"
                                           : "(%u failed)",
                                       failures);
    }
}

/* The per-call detail an expanded group shows. */
static void
render_call_line(
    AiToolCall     *call,
    AiRenderedText *out
){
    const gchar *target;
    AiStyleTag state_tag;
    const gchar *bullet;

    switch (ai_tool_call_get_state(call))
    {
        case AI_TOOL_CALL_OK:
            state_tag = AI_STYLE_TOOL_OK;
            bullet = "  \xe2\x9c\x93 ";
            break;
        case AI_TOOL_CALL_FAILED:
            state_tag = AI_STYLE_TOOL_FAILED;
            bullet = "  \xe2\x9c\x96 ";
            break;
        case AI_TOOL_CALL_DENIED:
            state_tag = AI_STYLE_TOOL_FAILED;
            bullet = "  \xe2\x8a\x98 ";
            break;
        default:
            state_tag = AI_STYLE_TOOL_PENDING;
            bullet = "  \xe2\x97\x8b ";
            break;
    }

    ai_rendered_text_append(out, bullet, state_tag);
    ai_rendered_text_append(out, ai_tool_call_get_name(call), AI_STYLE_TOOL_NAME);

    target = ai_tool_call_get_target(call);

    if (target != NULL)
    {
        ai_rendered_text_append(out, " ", AI_STYLE_DEFAULT);
        ai_rendered_text_append(out, target, AI_STYLE_CODE);
    }

    if (ai_tool_call_get_state(call) == AI_TOOL_CALL_DENIED)
    {
        ai_rendered_text_append(out, "  denied", AI_STYLE_TOOL_FAILED);
    }
}

static AiRenderedText *
tool_render(AiViewBlock *block)
{
    AiViewToolBlock *self = AI_VIEW_TOOL_BLOCK(block);
    AiRenderedText *out = ai_rendered_text_new();
    guint i;

    if (self->calls->len == 0)
    {
        return out;
    }

    render_summary_line(self, out);

    if (!ai_view_block_get_expanded(block))
    {
        ai_rendered_text_append(out, " \xe2\x80\xba", AI_STYLE_MARKER);
        return out;
    }

    ai_rendered_text_append(out, " \xe2\x8c\x84", AI_STYLE_MARKER);

    for (i = 0; i < self->calls->len; i++)
    {
        ai_rendered_text_append(out, "\n", AI_STYLE_DEFAULT);
        render_call_line(g_ptr_array_index(self->calls, i), out);
    }

    return out;
}

static void
ai_view_tool_block_finalize(GObject *object)
{
    AiViewToolBlock *self = AI_VIEW_TOOL_BLOCK(object);

    g_clear_pointer(&self->calls, g_ptr_array_unref);

    G_OBJECT_CLASS(ai_view_tool_block_parent_class)->finalize(object);
}

static void
ai_view_tool_block_class_init(AiViewToolBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_tool_block_finalize;
    block_class->render = tool_render;
    block_class->get_kind = tool_get_kind;
}

static void
ai_view_tool_block_init(AiViewToolBlock *self)
{
    self->calls = g_ptr_array_new_with_free_func(g_object_unref);
}

/**
 * ai_view_tool_block_new:
 *
 * Creates an empty tool group.
 *
 * Returns: (transfer full): a new #AiViewBlock
 */
AiViewBlock *
ai_view_tool_block_new(void)
{
    return AI_VIEW_BLOCK(g_object_new(AI_TYPE_VIEW_TOOL_BLOCK, NULL));
}

/**
 * ai_view_tool_block_add_call:
 * @self: an #AiViewToolBlock
 * @tool_use: (transfer none) (nullable): the request
 *
 * Adds a call to the group, or updates one already there.
 *
 * %AI_EVENT_TOOL_STARTED can arrive twice for one id --- a streamed call
 * announces its name before its arguments exist --- so this looks up by id
 * first and fills in what the second event knows rather than adding a
 * duplicate. That is why the event stream documents consumers as keying on
 * the id.
 *
 * Returns: (transfer none): the call, new or existing
 */
AiToolCall *
ai_view_tool_block_add_call(
    AiViewToolBlock *self,
    AiToolUse       *tool_use
){
    AiToolCall *existing;
    AiToolCall *call;

    g_return_val_if_fail(AI_IS_VIEW_TOOL_BLOCK(self), NULL);

    if (tool_use != NULL)
    {
        existing = ai_view_tool_block_find_call(self,
                                                ai_tool_use_get_id(tool_use));

        if (existing != NULL)
        {
            ai_tool_call_set_tool_use(existing, tool_use);
            ai_view_block_changed(AI_VIEW_BLOCK(self));
            return existing;
        }
    }

    call = ai_tool_call_new(tool_use);
    g_ptr_array_add(self->calls, call);
    ai_view_block_changed(AI_VIEW_BLOCK(self));

    return call;
}

/**
 * ai_view_tool_block_find_call:
 * @self: an #AiViewToolBlock
 * @tool_use_id: (nullable): the id to look for
 *
 * Finds a call by its tool use id.
 *
 * An empty or %NULL id matches nothing: several providers omit the id, and
 * treating all of those as one call would merge unrelated work.
 *
 * Returns: (transfer none) (nullable): the call, or %NULL
 */
AiToolCall *
ai_view_tool_block_find_call(
    AiViewToolBlock *self,
    const gchar     *tool_use_id
){
    guint i;

    g_return_val_if_fail(AI_IS_VIEW_TOOL_BLOCK(self), NULL);

    if (tool_use_id == NULL || tool_use_id[0] == '\0')
    {
        return NULL;
    }

    for (i = 0; i < self->calls->len; i++)
    {
        AiToolCall *call = g_ptr_array_index(self->calls, i);

        if (g_strcmp0(ai_tool_call_get_id(call), tool_use_id) == 0)
        {
            return call;
        }
    }

    return NULL;
}

/**
 * ai_view_tool_block_get_call:
 * @self: an #AiViewToolBlock
 * @index_: which call
 *
 * Returns: (transfer none) (nullable): the call, or %NULL if out of range
 */
AiToolCall *
ai_view_tool_block_get_call(
    AiViewToolBlock *self,
    guint            index_
){
    g_return_val_if_fail(AI_IS_VIEW_TOOL_BLOCK(self), NULL);

    if (index_ >= self->calls->len)
    {
        return NULL;
    }

    return g_ptr_array_index(self->calls, index_);
}

/**
 * ai_view_tool_block_get_n_calls:
 * @self: an #AiViewToolBlock
 *
 * Returns: how many calls are in the group
 */
guint
ai_view_tool_block_get_n_calls(AiViewToolBlock *self)
{
    g_return_val_if_fail(AI_IS_VIEW_TOOL_BLOCK(self), 0);

    return self->calls->len;
}

/**
 * ai_view_tool_block_get_lines_added:
 * @self: an #AiViewToolBlock
 *
 * Returns: the group's total added lines
 */
guint
ai_view_tool_block_get_lines_added(AiViewToolBlock *self)
{
    guint total = 0;
    guint i;

    g_return_val_if_fail(AI_IS_VIEW_TOOL_BLOCK(self), 0);

    for (i = 0; i < self->calls->len; i++)
    {
        total += ai_tool_call_get_lines_added(g_ptr_array_index(self->calls, i));
    }

    return total;
}

/**
 * ai_view_tool_block_get_lines_removed:
 * @self: an #AiViewToolBlock
 *
 * Returns: the group's total removed lines
 */
guint
ai_view_tool_block_get_lines_removed(AiViewToolBlock *self)
{
    guint total = 0;
    guint i;

    g_return_val_if_fail(AI_IS_VIEW_TOOL_BLOCK(self), 0);

    for (i = 0; i < self->calls->len; i++)
    {
        total += ai_tool_call_get_lines_removed(g_ptr_array_index(self->calls, i));
    }

    return total;
}

/**
 * ai_view_tool_block_get_summary:
 * @self: an #AiViewToolBlock
 *
 * The collapsed summary as plain text, without the expand marker.
 *
 * The rendering path is what a frontend uses; this exists so a caller can
 * log or assert on the wording without picking it back out of the spans.
 *
 * Returns: (transfer full): the summary
 */
gchar *
ai_view_tool_block_get_summary(AiViewToolBlock *self)
{
    g_autoptr(AiRenderedText) out = NULL;

    g_return_val_if_fail(AI_IS_VIEW_TOOL_BLOCK(self), g_strdup(""));

    out = ai_rendered_text_new();

    if (self->calls->len > 0)
    {
        render_summary_line(self, out);
    }

    return g_strdup(ai_rendered_text_get_text(out));
}

/**
 * ai_view_tool_block_call_changed:
 * @self: an #AiViewToolBlock
 *
 * Tells the group that one of its calls changed.
 *
 * The calls are plain objects with no signals of their own, so whoever
 * mutates one --- normally #AiConversation, on a tool finishing --- says so
 * here. That keeps the notification in one place instead of every call
 * needing a connection to its group.
 */
void
ai_view_tool_block_call_changed(AiViewToolBlock *self)
{
    g_return_if_fail(AI_IS_VIEW_TOOL_BLOCK(self));

    ai_view_block_changed(AI_VIEW_BLOCK(self));
}
