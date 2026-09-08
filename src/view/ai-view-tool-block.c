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
#include "view/ai-tool-preview.h"
#include "core/ai-json-util.h"

struct _AiViewToolBlock
{
    AiViewBlock parent_instance;
    GPtrArray  *calls;   /* AiToolCall, owned, in the order they started */
	gboolean show_previews;
};

G_DEFINE_TYPE(AiViewToolBlock, ai_view_tool_block, AI_TYPE_VIEW_BLOCK)

enum {
	PROP_0,
	PROP_SHOW_PREVIEWS
};

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

/* Codex reports one patch item for many paths; count its distinct files. */
static guint
call_item_count(AiToolCall *call)
{
	AiToolUse *use = ai_tool_call_get_tool_use(call);
	JsonNode *node = use != NULL ? ai_tool_use_get_input(use) : NULL;
	JsonArray *changes;
	g_autoptr(GHashTable) paths = NULL;
	guint i;

	if (g_strcmp0(ai_tool_call_get_name(call), "file_change") != 0 ||
	    node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
		return 1;
	changes = ai_json_get_array(json_node_get_object(node), "changes");
	if (changes == NULL) return 1;

	paths = g_hash_table_new(g_str_hash, g_str_equal);
	for (i = 0; i < json_array_get_length(changes); i++)
	{
		const gchar *path = ai_json_get_string(
			ai_json_array_get_object(changes, i), "path", NULL);
		if (path != NULL && *path != '\0')
			g_hash_table_add(paths, (gpointer)path);
	}
	return MAX(1, g_hash_table_size(paths));
}

/* Bucket categories in first-seen order to preserve the action sequence. */
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

        bucket->count += call_item_count(call);
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
        if (!self->show_previews) return out;
    }
	else
		ai_rendered_text_append(out, " \xe2\x8c\x84", AI_STYLE_MARKER);

	i = self->show_previews && !ai_view_block_get_expanded(block) && self->calls->len > 4
		? self->calls->len - 4 : 0;
	if (i > 0)
		ai_rendered_text_append_printf(out, AI_STYLE_DIM, "\n  ... %u earlier calls; expand for details", i);

    for (; i < self->calls->len; i++)
    {
		AiToolCall *call = g_ptr_array_index(self->calls, i);
		if (!ai_view_block_get_expanded(block) && ai_tool_call_get_category(call) != AI_TOOL_CATEGORY_COMMAND &&
			ai_tool_call_get_category(call) != AI_TOOL_CATEGORY_FILE_WRITE) continue;
        ai_rendered_text_append(out, "\n", AI_STYLE_DEFAULT);
        render_call_line(call, out);
		if (self->show_previews) _ai_tool_preview_append(call, out, ai_view_block_get_expanded(block));
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

/* Keep the legacy summary-only rendering unless an embedder opts in. */
static void
tool_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	if (id == PROP_SHOW_PREVIEWS) g_value_set_boolean(value, AI_VIEW_TOOL_BLOCK(object)->show_previews);
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
tool_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiViewToolBlock *self = AI_VIEW_TOOL_BLOCK(object);
	if (id == PROP_SHOW_PREVIEWS)
	{
		gboolean enabled = g_value_get_boolean(value);
		if (self->show_previews != enabled)
		{
			self->show_previews = enabled;
			ai_view_block_changed(AI_VIEW_BLOCK(self));
		}
	}
	else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}

static void
ai_view_tool_block_class_init(AiViewToolBlockClass *klass)
{
    AiViewBlockClass *block_class = AI_VIEW_BLOCK_CLASS(klass);

    G_OBJECT_CLASS(klass)->finalize = ai_view_tool_block_finalize;
	G_OBJECT_CLASS(klass)->get_property = tool_get_property;
	G_OBJECT_CLASS(klass)->set_property = tool_set_property;
	/**
	 * AiViewToolBlock:show-previews:
	 *
	 * Whether command/output and edit-region previews accompany the summary.
	 * Off by default. Compact groups show the last four calls, up to twelve
	 * diff lines or six output lines per call; expanded groups show up to 64.
	 * Previews use recorded tool data and never read the current filesystem.
	 */
	g_object_class_install_property(G_OBJECT_CLASS(klass), PROP_SHOW_PREVIEWS,
		g_param_spec_boolean("show-previews", NULL, NULL, FALSE,
			G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
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
