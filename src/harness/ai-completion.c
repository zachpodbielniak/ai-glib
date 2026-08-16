/*
 * ai-completion.c - Completing / and @ in a line of input
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "harness/ai-completion.h"
#include "harness/ai-mention.h"

/* Directories never worth offering. A completion list is a menu, and
 * these are never the answer. */
static const gchar *IGNORED_DIRECTORIES[] = {
    ".git", ".svn", ".hg", "node_modules", "__pycache__", ".mypy_cache",
    ".pytest_cache", ".venv", NULL
};

#define DEFAULT_MAX_ITEMS (200)

/* ================================================================
 * AiCompletionItem
 * ================================================================ */

G_DEFINE_BOXED_TYPE(AiCompletionItem, ai_completion_item,
                    ai_completion_item_copy, ai_completion_item_free)

static AiCompletionItem *
completion_item_new(
    const gchar      *text,
    const gchar      *display,
    const gchar      *description,
    const gchar      *origin,
    AiCompletionKind  kind,
    gboolean          is_directory
){
    AiCompletionItem *self = g_slice_new0(AiCompletionItem);

    self->text = g_strdup(text);
    self->display = g_strdup(display != NULL ? display : text);
    self->description = g_strdup(description);
    self->origin = g_strdup(origin);
    self->kind = kind;
    self->is_directory = is_directory;

    return self;
}

/**
 * ai_completion_item_copy:
 * @self: (nullable): an #AiCompletionItem
 *
 * Returns: (transfer full) (nullable): a copy
 */
AiCompletionItem *
ai_completion_item_copy(const AiCompletionItem *self)
{
    if (self == NULL)
    {
        return NULL;
    }

    return completion_item_new(self->text, self->display, self->description,
                               self->origin, self->kind, self->is_directory);
}

/**
 * ai_completion_item_free:
 * @self: (nullable): an #AiCompletionItem
 *
 * Frees @self.
 */
void
ai_completion_item_free(AiCompletionItem *self)
{
    if (self == NULL)
    {
        return;
    }

    g_free(self->text);
    g_free(self->display);
    g_free(self->description);
    g_free(self->origin);
    g_slice_free(AiCompletionItem, self);
}

/* ================================================================
 * AiCompletionResult
 * ================================================================ */

struct _AiCompletionResult
{
    GObject           parent_instance;

    AiCompletionKind  kind;
    guint             start;
    guint             end;
    GPtrArray        *items;
};

G_DEFINE_TYPE(AiCompletionResult, ai_completion_result, G_TYPE_OBJECT)

static void
ai_completion_result_finalize(GObject *object)
{
    AiCompletionResult *self = AI_COMPLETION_RESULT(object);

    g_clear_pointer(&self->items, g_ptr_array_unref);

    G_OBJECT_CLASS(ai_completion_result_parent_class)->finalize(object);
}

static void
ai_completion_result_class_init(AiCompletionResultClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ai_completion_result_finalize;
}

static void
ai_completion_result_init(AiCompletionResult *self)
{
    self->items = g_ptr_array_new_with_free_func(
        (GDestroyNotify)ai_completion_item_free);
}

/**
 * ai_completion_result_get_kind:
 * @self: an #AiCompletionResult
 *
 * Returns: what is being completed
 */
AiCompletionKind
ai_completion_result_get_kind(AiCompletionResult *self)
{
    g_return_val_if_fail(AI_IS_COMPLETION_RESULT(self), AI_COMPLETION_NONE);

    return self->kind;
}

/**
 * ai_completion_result_get_start:
 * @self: an #AiCompletionResult
 *
 * The byte offset where the replacement begins.
 *
 * This and ai_completion_result_get_end() are the whole product: they
 * are exactly what `completion-at-point-functions` wants, and what lets
 * a frontend replace a token without re-deriving where it started.
 *
 * Returns: a byte offset into the buffer that was queried
 */
guint
ai_completion_result_get_start(AiCompletionResult *self)
{
    g_return_val_if_fail(AI_IS_COMPLETION_RESULT(self), 0);

    return self->start;
}

/**
 * ai_completion_result_get_end:
 * @self: an #AiCompletionResult
 *
 * Returns: the byte offset just past the replacement
 */
guint
ai_completion_result_get_end(AiCompletionResult *self)
{
    g_return_val_if_fail(AI_IS_COMPLETION_RESULT(self), 0);

    return self->end;
}

/**
 * ai_completion_result_get_n_items:
 * @self: an #AiCompletionResult
 *
 * Returns: how many candidates there are, possibly zero
 */
guint
ai_completion_result_get_n_items(AiCompletionResult *self)
{
    g_return_val_if_fail(AI_IS_COMPLETION_RESULT(self), 0);

    return self->items->len;
}

/**
 * ai_completion_result_get_item:
 * @self: an #AiCompletionResult
 * @index: which candidate
 *
 * Returns: (transfer none) (nullable): the candidate, or %NULL if
 *   @index is out of range
 */
const AiCompletionItem *
ai_completion_result_get_item(
    AiCompletionResult *self,
    guint               index
){
    g_return_val_if_fail(AI_IS_COMPLETION_RESULT(self), NULL);

    if (index >= self->items->len)
    {
        return NULL;
    }

    return g_ptr_array_index(self->items, index);
}

/**
 * ai_completion_result_get_item_fields:
 * @self: an #AiCompletionResult
 * @index: which candidate
 * @out_text: (out) (optional) (transfer none): what to insert
 * @out_display: (out) (optional) (transfer none): what to show
 * @out_description: (out) (optional) (transfer none) (nullable): a
 *   one-line summary
 * @out_origin: (out) (optional) (transfer none) (nullable): which
 *   harness's directory it came from
 * @out_is_directory: (out) (optional): whether it names a directory
 *
 * Reads one candidate through out-parameters.
 *
 * This exists alongside ai_completion_result_get_item() for the same
 * reason ai_rendered_text_get_span() exists: a plain struct behind a
 * pointer does not survive g-ir-scanner usefully, and this API has to
 * work from bindings --- the Emacs frontend reaches it exactly this way.
 *
 * Returns: %FALSE if @index is out of range, leaving the outputs alone
 */
gboolean
ai_completion_result_get_item_fields(
    AiCompletionResult  *self,
    guint                index,
    const gchar        **out_text,
    const gchar        **out_display,
    const gchar        **out_description,
    const gchar        **out_origin,
    gboolean            *out_is_directory
){
    const AiCompletionItem *item;

    g_return_val_if_fail(AI_IS_COMPLETION_RESULT(self), FALSE);

    if (index >= self->items->len)
    {
        return FALSE;
    }

    item = g_ptr_array_index(self->items, index);

    if (out_text != NULL)
    {
        *out_text = item->text;
    }

    if (out_display != NULL)
    {
        *out_display = item->display;
    }

    if (out_description != NULL)
    {
        *out_description = item->description;
    }

    if (out_origin != NULL)
    {
        *out_origin = item->origin;
    }

    if (out_is_directory != NULL)
    {
        *out_is_directory = item->is_directory;
    }

    return TRUE;
}

/**
 * ai_completion_result_get_common_prefix:
 * @self: an #AiCompletionResult
 *
 * The longest prefix every candidate shares.
 *
 * This is what makes one Tab useful when several candidates match: the
 * frontend inserts the prefix and shows the menu, rather than making the
 * user pick between eight things that all start the same way.
 *
 * Returns: (transfer full) (nullable): the prefix, or %NULL if there are
 *   no candidates
 */
gchar *
ai_completion_result_get_common_prefix(AiCompletionResult *self)
{
    const AiCompletionItem *first;
    gsize                   prefix;
    guint                   i;

    g_return_val_if_fail(AI_IS_COMPLETION_RESULT(self), NULL);

    if (self->items->len == 0)
    {
        return NULL;
    }

    first = g_ptr_array_index(self->items, 0);
    prefix = strlen(first->text);

    for (i = 1; i < self->items->len; i++)
    {
        const AiCompletionItem *item = g_ptr_array_index(self->items, i);
        gsize                   j = 0;

        while (j < prefix && item->text[j] != '\0' &&
               item->text[j] == first->text[j])
        {
            j++;
        }

        prefix = j;
    }

    /* Never return a partial character: a frontend inserts this
     * verbatim, and half a UTF-8 sequence in a buffer is corruption. */
    while (prefix > 0 &&
           (first->text[prefix] & 0xC0) == 0x80)
    {
        prefix--;
    }

    return g_strndup(first->text, prefix);
}

/* ================================================================
 * AiCompletionContext
 * ================================================================ */

struct _AiCompletionContext
{
    GObject       parent_instance;

    AiCommandSet *commands;
    gchar        *working_directory;
    guint         max_items;
};

enum
{
    PROP_0,
    PROP_COMMANDS,
    PROP_WORKING_DIRECTORY,
    PROP_MAX_ITEMS,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_TYPE(AiCompletionContext, ai_completion_context, G_TYPE_OBJECT)

static void
ai_completion_context_finalize(GObject *object)
{
    AiCompletionContext *self = AI_COMPLETION_CONTEXT(object);

    g_clear_object(&self->commands);
    g_clear_pointer(&self->working_directory, g_free);

    G_OBJECT_CLASS(ai_completion_context_parent_class)->finalize(object);
}

static void
ai_completion_context_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiCompletionContext *self = AI_COMPLETION_CONTEXT(object);

    switch (prop_id)
    {
        case PROP_COMMANDS:
            g_value_set_object(value, self->commands);
            break;

        case PROP_WORKING_DIRECTORY:
            g_value_set_string(value, self->working_directory);
            break;

        case PROP_MAX_ITEMS:
            g_value_set_uint(value, self->max_items);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_completion_context_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiCompletionContext *self = AI_COMPLETION_CONTEXT(object);

    switch (prop_id)
    {
        case PROP_COMMANDS:
            g_set_object(&self->commands, g_value_get_object(value));
            break;

        case PROP_WORKING_DIRECTORY:
            ai_completion_context_set_working_directory(
                self, g_value_get_string(value));
            break;

        case PROP_MAX_ITEMS:
            self->max_items = g_value_get_uint(value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_completion_context_class_init(AiCompletionContextClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_completion_context_finalize;
    object_class->get_property = ai_completion_context_get_property;
    object_class->set_property = ai_completion_context_set_property;

    /**
     * AiCompletionContext:commands:
     *
     * Where `/name` candidates come from. %NULL completes paths only.
     */
    properties[PROP_COMMANDS] =
        g_param_spec_object("commands", NULL, NULL, AI_TYPE_COMMAND_SET,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCompletionContext:working-directory:
     *
     * What a relative `@path` completes against.
     */
    properties[PROP_WORKING_DIRECTORY] =
        g_param_spec_string("working-directory", NULL, NULL, NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiCompletionContext:max-items:
     *
     * How many candidates a query returns at most.
     *
     * A bound rather than a nicety: this runs on a keystroke, and a
     * directory of forty thousand files must not turn one into a pause.
     */
    properties[PROP_MAX_ITEMS] =
        g_param_spec_uint("max-items", NULL, NULL, 1, G_MAXUINT,
                          DEFAULT_MAX_ITEMS,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_completion_context_init(AiCompletionContext *self)
{
    self->max_items = DEFAULT_MAX_ITEMS;
    self->working_directory = g_get_current_dir();
}

/**
 * ai_completion_context_new:
 * @commands: (nullable) (transfer none): where `/name` candidates come from
 * @working_directory: (nullable): what `@path` completes against
 *
 * Returns: (transfer full): a new #AiCompletionContext
 */
AiCompletionContext *
ai_completion_context_new(
    AiCommandSet *commands,
    const gchar  *working_directory
){
    AiCompletionContext *self =
        g_object_new(AI_TYPE_COMPLETION_CONTEXT, "commands", commands, NULL);

    if (working_directory != NULL)
    {
        ai_completion_context_set_working_directory(self, working_directory);
    }

    return self;
}

/**
 * ai_completion_context_set_working_directory:
 * @self: an #AiCompletionContext
 * @path: (nullable): the directory, or %NULL for the current one
 *
 * Sets what a relative path completes against.
 */
void
ai_completion_context_set_working_directory(
    AiCompletionContext *self,
    const gchar         *path
){
    g_return_if_fail(AI_IS_COMPLETION_CONTEXT(self));

    if (g_strcmp0(self->working_directory, path) == 0)
    {
        return;
    }

    g_free(self->working_directory);
    self->working_directory = (path != NULL) ? g_strdup(path)
                                             : g_get_current_dir();

    g_object_notify_by_pspec(G_OBJECT(self),
                             properties[PROP_WORKING_DIRECTORY]);
}

/**
 * ai_completion_context_get_working_directory:
 * @self: an #AiCompletionContext
 *
 * Returns: (transfer none): the directory relative paths resolve against
 */
const gchar *
ai_completion_context_get_working_directory(AiCompletionContext *self)
{
    g_return_val_if_fail(AI_IS_COMPLETION_CONTEXT(self), NULL);

    return self->working_directory;
}

/**
 * ai_completion_context_set_max_items:
 * @self: an #AiCompletionContext
 * @max_items: the cap, at least 1
 *
 * Sets how many candidates a query returns at most.
 */
void
ai_completion_context_set_max_items(
    AiCompletionContext *self,
    guint                max_items
){
    g_return_if_fail(AI_IS_COMPLETION_CONTEXT(self));
    g_return_if_fail(max_items >= 1);

    if (self->max_items == max_items)
    {
        return;
    }

    self->max_items = max_items;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAX_ITEMS]);
}

/**
 * ai_completion_context_get_max_items:
 * @self: an #AiCompletionContext
 *
 * Returns: the cap on candidates per query
 */
guint
ai_completion_context_get_max_items(AiCompletionContext *self)
{
    g_return_val_if_fail(AI_IS_COMPLETION_CONTEXT(self), 0);

    return self->max_items;
}

/* ================================================================
 * Querying
 * ================================================================ */

static gboolean
is_ignored_directory(const gchar *name)
{
    gsize i;

    for (i = 0; IGNORED_DIRECTORIES[i] != NULL; i++)
    {
        if (g_strcmp0(name, IGNORED_DIRECTORIES[i]) == 0)
        {
            return TRUE;
        }
    }

    return FALSE;
}

/*
 * Ordering for g_ptr_array_sort_values(), which hands the comparator the
 * elements themselves --- not pointers to them, the way the older
 * g_ptr_array_sort() does.
 */
static gint
compare_items(gconstpointer a, gconstpointer b)
{
    const AiCompletionItem *ia = a;
    const AiCompletionItem *ib = b;

    /* Directories first: completing a path is usually a walk down, and
     * the next step is nearly always a directory. */
    if (ia->is_directory != ib->is_directory)
    {
        return ia->is_directory ? -1 : 1;
    }

    return g_strcmp0(ia->text, ib->text);
}

/* Complete a `/name` against the command set. */
static void
complete_commands(
    AiCompletionContext *self,
    AiCompletionResult  *result,
    const gchar         *fragment
){
    GList *commands;
    GList *iter;

    if (self->commands == NULL)
    {
        return;
    }

    commands = ai_command_set_list(self->commands);

    for (iter = commands; iter != NULL; iter = iter->next)
    {
        AiCommand   *command = iter->data;
        const gchar *name = ai_command_get_name(command);

        if (name == NULL || !g_str_has_prefix(name, fragment))
        {
            continue;
        }

        if (result->items->len >= self->max_items)
        {
            break;
        }

        g_ptr_array_add(
            result->items,
            completion_item_new(name, name,
                                ai_command_get_description(command),
                                ai_command_get_origin(command),
                                AI_COMPLETION_COMMAND, FALSE));
    }

    g_list_free_full(commands, g_object_unref);
}

/* Complete an `@path` against the filesystem. */
static void
complete_paths(
    AiCompletionContext *self,
    AiCompletionResult  *result,
    const gchar         *fragment
){
    g_autofree gchar *directory_part = NULL;
    g_autofree gchar *file_part = NULL;
    g_autofree gchar *search_dir = NULL;
    g_autoptr(GDir)   dir = NULL;
    g_autoptr(GError) local_error = NULL;
    const gchar      *entry;
    const gchar      *slash;

    slash = strrchr(fragment, '/');

    if (slash != NULL)
    {
        directory_part = g_strndup(fragment, (gsize)(slash - fragment) + 1);
        file_part = g_strdup(slash + 1);
    }
    else
    {
        directory_part = g_strdup("");
        file_part = g_strdup(fragment);
    }

    search_dir = ai_mention_resolve(
        directory_part[0] != '\0' ? directory_part : ".",
        self->working_directory);

    if (search_dir == NULL)
    {
        return;
    }

    dir = g_dir_open(search_dir, 0, &local_error);

    if (dir == NULL)
    {
        /* Half-typed paths name directories that do not exist yet. That
         * is the normal case here, not a problem. */
        g_debug("ai_completion: %s", local_error->message);
        return;
    }

    while ((entry = g_dir_read_name(dir)) != NULL)
    {
        g_autofree gchar *child = NULL;
        g_autofree gchar *text = NULL;
        gboolean          is_dir;

        if (!g_str_has_prefix(entry, file_part))
        {
            continue;
        }

        /* Hidden entries only when the user has committed to one by
         * typing the dot. */
        if (entry[0] == '.' && file_part[0] != '.')
        {
            continue;
        }

        child = g_build_filename(search_dir, entry, NULL);
        is_dir = g_file_test(child, G_FILE_TEST_IS_DIR);

        if (is_dir && is_ignored_directory(entry))
        {
            continue;
        }

        /* A trailing slash on a directory means one Tab walks into it
         * instead of stopping at its name. */
        text = g_strdup_printf("%s%s%s", directory_part, entry,
                               is_dir ? "/" : "");

        g_ptr_array_add(result->items,
                        completion_item_new(text, entry, NULL, NULL,
                                            AI_COMPLETION_PATH, is_dir));

        if (result->items->len >= self->max_items)
        {
            break;
        }
    }

    g_ptr_array_sort_values(result->items, compare_items);
}

/**
 * ai_completion_context_query:
 * @self: an #AiCompletionContext
 * @buffer: the whole input line
 * @cursor: a byte offset into @buffer
 *
 * Finds what can be completed at @cursor.
 *
 * The returned range covers the token being replaced --- for `@src/co`
 * that is everything after the `@`, directory component included, so a
 * frontend replaces the whole path rather than splicing a fragment. Text
 * to the right of @cursor is ignored, which is what makes completing in
 * the middle of a line behave.
 *
 * A `/name` only completes at the very start of the buffer, because that
 * is the only place a slash means a command.
 *
 * Returns: (transfer full): the result, never %NULL. A result with no
 *   items and kind %AI_COMPLETION_NONE means "nothing to do here".
 */
AiCompletionResult *
ai_completion_context_query(
    AiCompletionContext *self,
    const gchar         *buffer,
    guint                cursor
){
    g_autoptr(AiCompletionResult) result = NULL;
    g_autofree gchar             *fragment = NULL;
    gsize                         len;
    gsize                         start;

    g_return_val_if_fail(AI_IS_COMPLETION_CONTEXT(self), NULL);

    result = g_object_new(AI_TYPE_COMPLETION_RESULT, NULL);

    if (buffer == NULL)
    {
        return (AiCompletionResult *)g_steal_pointer(&result);
    }

    len = strlen(buffer);

    /* A cursor past the end is a frontend bug, not a reason to read out
     * of bounds. */
    if (cursor > len)
    {
        cursor = (guint)len;
    }

    result->start = cursor;
    result->end = cursor;

    /* A slash command: only at offset 0, and only while the cursor is
     * still inside the name. */
    if (buffer[0] == '/')
    {
        gsize i;

        for (i = 1; i < cursor; i++)
        {
            if (buffer[i] == ' ' || buffer[i] == '\t')
            {
                return (AiCompletionResult *)g_steal_pointer(&result);
            }
        }

        fragment = g_strndup(buffer + 1, cursor - 1);
        result->kind = AI_COMPLETION_COMMAND;
        result->start = 1;
        result->end = cursor;

        complete_commands(self, result, fragment);

        return (AiCompletionResult *)g_steal_pointer(&result);
    }

    /* A mention: scan back to the `@` that opens the token at the
     * cursor, stopping at whitespace. */
    start = cursor;

    while (start > 0 &&
           buffer[start - 1] != ' ' && buffer[start - 1] != '\t' &&
           buffer[start - 1] != '\n' && buffer[start - 1] != '\r')
    {
        start--;

        if (buffer[start] == '@')
        {
            break;
        }
    }

    if (buffer[start] != '@' || (start > 0 &&
                                 buffer[start - 1] != ' ' &&
                                 buffer[start - 1] != '\t' &&
                                 buffer[start - 1] != '\n' &&
                                 buffer[start - 1] != '\r' &&
                                 buffer[start - 1] != '(' &&
                                 buffer[start - 1] != '[' &&
                                 buffer[start - 1] != '{'))
    {
        return (AiCompletionResult *)g_steal_pointer(&result);
    }

    fragment = g_strndup(buffer + start + 1, cursor - start - 1);
    result->kind = AI_COMPLETION_PATH;
    result->start = (guint)start + 1;
    result->end = cursor;

    complete_paths(self, result, fragment);

    return (AiCompletionResult *)g_steal_pointer(&result);
}
