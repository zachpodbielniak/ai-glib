/*
 * ai-mention.c - @file references in a prompt
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include <string.h>

#include "harness/ai-mention.h"

/* How many entries a directory mention lists before it says "and N more". */
#define DIRECTORY_LISTING_MAX (200)

/* Trailing punctuation that is sentence, not path. "see @foo.c." means the
 * file foo.c followed by a full stop. */
#define TRAILING_PUNCTUATION ".,;:!?)]}>'\"`"

G_DEFINE_BOXED_TYPE(AiMention, ai_mention, ai_mention_copy, ai_mention_free)

/* ================================================================
 * The boxed type
 * ================================================================ */

/**
 * ai_mention_new:
 * @start: byte offset of the `@`
 * @len: length in bytes, including the `@`
 * @path: the path as written
 *
 * Returns: (transfer full): a new #AiMention
 */
AiMention *
ai_mention_new(
    guint        start,
    guint        len,
    const gchar *path
){
    AiMention *self = g_slice_new0(AiMention);

    self->start = start;
    self->len = len;
    self->path = g_strdup(path);

    return self;
}

/**
 * ai_mention_copy:
 * @self: (nullable): an #AiMention
 *
 * Returns: (transfer full) (nullable): a copy
 */
AiMention *
ai_mention_copy(const AiMention *self)
{
    if (self == NULL)
    {
        return NULL;
    }

    return ai_mention_new(self->start, self->len, self->path);
}

/**
 * ai_mention_free:
 * @self: (nullable): an #AiMention
 *
 * Frees @self.
 */
void
ai_mention_free(AiMention *self)
{
    if (self == NULL)
    {
        return;
    }

    g_free(self->path);
    g_slice_free(AiMention, self);
}

/* ================================================================
 * Scanning
 * ================================================================ */

/*
 * May a mention begin at this byte?
 *
 * Only at the start of the text or after whitespace or an opening
 * bracket. That single rule is what keeps an email address out: the `@`
 * in "z.podbielniak@gmail.com" follows a letter, so it is never a
 * candidate.
 */
static gboolean
is_mention_boundary(const gchar *text, gsize offset)
{
    gchar previous;

    if (offset == 0)
    {
        return TRUE;
    }

    previous = text[offset - 1];

    return previous == ' ' || previous == '\t' || previous == '\n' ||
           previous == '\r' || previous == '(' || previous == '[' ||
           previous == '{';
}

/**
 * ai_mention_scan:
 * @text: (nullable): the input to scan
 *
 * Finds every syntactic `@path` in @text.
 *
 * This is pure: it touches no filesystem, because a frontend calls it on
 * every keystroke to highlight the input line. Whether a mention names
 * anything real is decided later, by ai_mention_expand() --- and that
 * split is what leaves a Python `@decorator` alone. It is scanned like
 * any other candidate and then, resolving to nothing, left exactly as
 * written.
 *
 * A quoted form (`@"a path with spaces"`) is understood, because the
 * bare form has to stop at whitespace.
 *
 * Returns: (transfer full) (element-type AiMention): the mentions, in
 *   the order they appear
 */
GList *
ai_mention_scan(const gchar *text)
{
    GList *out = NULL;
    gsize  len;
    gsize  i = 0;

    if (text == NULL)
    {
        return NULL;
    }

    len = strlen(text);

    while (i < len)
    {
        gsize path_start;
        gsize path_end;

        if (text[i] != '@' || !is_mention_boundary(text, i))
        {
            i++;
            continue;
        }

        path_start = i + 1;

        if (path_start < len && text[path_start] == '"')
        {
            gsize closing = path_start + 1;

            while (closing < len && text[closing] != '"')
            {
                closing++;
            }

            if (closing < len)
            {
                g_autofree gchar *path =
                    g_strndup(text + path_start + 1,
                              closing - path_start - 1);

                if (path[0] != '\0')
                {
                    out = g_list_prepend(
                        out, ai_mention_new((guint)i,
                                            (guint)(closing + 1 - i), path));
                }

                i = closing + 1;
                continue;
            }

            /* Unterminated quote: fall through and treat it as a bare
             * mention, which stops at the first space. */
        }

        path_end = path_start;

        while (path_end < len &&
               text[path_end] != ' ' && text[path_end] != '\t' &&
               text[path_end] != '\n' && text[path_end] != '\r')
        {
            path_end++;
        }

        /* "see @foo.c." names foo.c; the stop belongs to the sentence. */
        while (path_end > path_start &&
               strchr(TRAILING_PUNCTUATION, text[path_end - 1]) != NULL)
        {
            path_end--;
        }

        if (path_end > path_start)
        {
            g_autofree gchar *path =
                g_strndup(text + path_start, path_end - path_start);

            out = g_list_prepend(
                out, ai_mention_new((guint)i, (guint)(path_end - i), path));
        }

        i = (path_end > path_start) ? path_end : path_start;
    }

    return g_list_reverse(out);
}

/* ================================================================
 * Resolution
 * ================================================================ */

/**
 * ai_mention_resolve:
 * @path: the path as written
 * @cwd: (nullable): what a relative path is relative to
 *
 * Turns a mention's path into a filesystem path.
 *
 * Resolution is literal --- a leading `~`, and otherwise @cwd. There is
 * deliberately no globbing: `@x` must not be able to pull in something
 * the user did not name.
 *
 * Returns: (transfer full) (nullable): an absolute path, or %NULL
 */
gchar *
ai_mention_resolve(
    const gchar *path,
    const gchar *cwd
){
    g_return_val_if_fail(path != NULL, NULL);

    if (path[0] == '\0')
    {
        return NULL;
    }

    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0'))
    {
        const gchar *home = g_get_home_dir();

        if (home == NULL)
        {
            return NULL;
        }

        return g_build_filename(home, path + 1, NULL);
    }

    if (g_path_is_absolute(path))
    {
        return g_strdup(path);
    }

    if (cwd == NULL)
    {
        return g_strdup(path);
    }

    return g_build_filename(cwd, path, NULL);
}

/* A fence language hint, so a model reads the block as code. */
static const gchar *
language_for(const gchar *path)
{
    static const struct
    {
        const gchar *suffix;
        const gchar *language;
    } table[] = {
        { ".c", "c" },           { ".h", "c" },
        { ".py", "python" },     { ".sh", "sh" },
        { ".bash", "sh" },       { ".el", "elisp" },
        { ".hs", "haskell" },    { ".json", "json" },
        { ".yaml", "yaml" },     { ".yml", "yaml" },
        { ".org", "org" },       { ".md", "markdown" },
        { ".toml", "toml" },     { ".mk", "make" },
        { ".rs", "rust" },       { ".go", "go" },
        { NULL, NULL }
    };
    gsize i;

    for (i = 0; table[i].suffix != NULL; i++)
    {
        if (g_str_has_suffix(path, table[i].suffix))
        {
            return table[i].language;
        }
    }

    if (g_str_has_suffix(path, "Makefile") || g_str_has_suffix(path, ".mk"))
    {
        return "make";
    }

    return "";
}

/* Append a directory's entries, directories first and marked. */
static void
append_directory(
    GString     *out,
    const gchar *display,
    const gchar *resolved
){
    g_autoptr(GDir)      dir = NULL;
    g_autoptr(GError)    local_error = NULL;
    g_autoptr(GPtrArray) dirs = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func(g_free);
    const gchar         *entry;
    guint                i;
    guint                shown = 0;

    dir = g_dir_open(resolved, 0, &local_error);

    if (dir == NULL)
    {
        g_string_append_printf(out, "@%s (cannot be listed: %s)\n\n",
                               display, local_error->message);
        return;
    }

    while ((entry = g_dir_read_name(dir)) != NULL)
    {
        g_autofree gchar *child = g_build_filename(resolved, entry, NULL);

        if (g_file_test(child, G_FILE_TEST_IS_DIR))
        {
            g_ptr_array_add(dirs, g_strdup_printf("%s/", entry));
        }
        else
        {
            g_ptr_array_add(files, g_strdup(entry));
        }
    }

    g_ptr_array_sort_values(dirs, (GCompareFunc)g_strcmp0);
    g_ptr_array_sort_values(files, (GCompareFunc)g_strcmp0);

    g_string_append_printf(out, "@%s (directory)\n", display);

    for (i = 0; i < dirs->len && shown < DIRECTORY_LISTING_MAX; i++, shown++)
    {
        g_string_append_printf(out, "  %s\n",
                               (const gchar *)g_ptr_array_index(dirs, i));
    }

    for (i = 0; i < files->len && shown < DIRECTORY_LISTING_MAX; i++, shown++)
    {
        g_string_append_printf(out, "  %s\n",
                               (const gchar *)g_ptr_array_index(files, i));
    }

    if (dirs->len + files->len > shown)
    {
        g_string_append_printf(out, "  ... and %u more\n",
                               dirs->len + files->len - shown);
    }

    g_string_append_c(out, '\n');
}

/*
 * Append one file's contents, within the remaining budget.
 *
 * Returns how many bytes of content were consumed, so the caller can
 * keep the running total.
 */
static gsize
append_file(
    GString     *out,
    const gchar *display,
    const gchar *resolved,
    gsize        remaining
){
    g_autofree gchar *contents = NULL;
    g_autoptr(GError) local_error = NULL;
    gsize             len = 0;
    gboolean          truncated = FALSE;

    if (!g_file_get_contents(resolved, &contents, &len, &local_error))
    {
        /* Named but unreadable is worth saying: silence would read as
         * "the file was empty". */
        g_string_append_printf(out, "@%s (cannot be read: %s)\n\n",
                               display, local_error->message);
        return 0;
    }

    if (!g_utf8_validate(contents, (gssize)len, NULL))
    {
        g_string_append_printf(out, "@%s (binary, %" G_GSIZE_FORMAT
                               " bytes; contents omitted)\n\n",
                               display, len);
        return 0;
    }

    if (len > remaining)
    {
        const gchar *cut = contents + remaining;
        const gchar *safe;

        /* Never hand a model half a character. */
        safe = g_utf8_find_prev_char(contents, cut);
        len = (safe != NULL) ? (gsize)(safe - contents) : 0;
        truncated = TRUE;
    }

    g_string_append_printf(out, "@%s\n```%s\n", display, language_for(display));
    g_string_append_len(out, contents, (gssize)len);

    if (len > 0 && contents[len - 1] != '\n')
    {
        g_string_append_c(out, '\n');
    }

    g_string_append(out, "```\n");

    if (truncated)
    {
        /* Stated, never silent: a model that does not know it was given
         * half a file will answer confidently about the other half. */
        g_string_append_printf(out,
                               "(truncated at %" G_GSIZE_FORMAT " bytes)\n",
                               len);
    }

    g_string_append_c(out, '\n');

    return len;
}

/**
 * ai_mention_expand:
 * @text: the input to expand
 * @cwd: (nullable): what relative paths resolve against
 * @max_bytes: how much file content to append in total, or 0 for the
 *   default
 * @out_files: (out) (optional) (transfer full) (element-type utf8): the
 *   paths that were resolved and appended
 *
 * Appends the contents of every `@path` that names something real.
 *
 * The prompt itself is left exactly as written and the files follow it,
 * rather than each mention being replaced in place. Replacing inline
 * would take "explain @src/ai-event.c please" and put nine hundred lines
 * between the verb and its object.
 *
 * A mention that resolves to nothing is not an error and produces no
 * output --- which is what makes a Python decorator, an email address
 * that slipped past the boundary rule, and a typo all harmless.
 *
 * When nothing resolves the text is returned unchanged, with no trailer.
 *
 * There is no #GError: every way this can go wrong --- a path that does
 * not exist, a file that cannot be read, a directory that cannot be
 * listed --- is reported inside the returned text, where the model can
 * see it, rather than as a failure that would throw the prompt away.
 *
 * Returns: (transfer full): the expanded text
 */
gchar *
ai_mention_expand(
    const gchar  *text,
    const gchar  *cwd,
    gsize         max_bytes,
    GList       **out_files
){
    g_autoptr(GString) body = NULL;
    GList             *mentions;
    GList             *iter;
    GList             *files = NULL;
    g_autoptr(GHashTable) seen = NULL;
    gsize              budget;

    g_return_val_if_fail(text != NULL, NULL);

    if (out_files != NULL)
    {
        *out_files = NULL;
    }

    budget = (max_bytes > 0) ? max_bytes : AI_MENTION_DEFAULT_MAX_BYTES;
    mentions = ai_mention_scan(text);

    if (mentions == NULL)
    {
        return g_strdup(text);
    }

    body = g_string_new(NULL);

    /* The same file mentioned twice is included once. */
    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (iter = mentions; iter != NULL; iter = iter->next)
    {
        const AiMention  *mention = iter->data;
        g_autofree gchar *resolved = ai_mention_resolve(mention->path, cwd);

        if (resolved == NULL || g_hash_table_contains(seen, resolved))
        {
            continue;
        }

        if (g_file_test(resolved, G_FILE_TEST_IS_DIR))
        {
            g_hash_table_add(seen, g_strdup(resolved));
            append_directory(body, mention->path, resolved);
            files = g_list_prepend(files, g_strdup(resolved));
            continue;
        }

        if (!g_file_test(resolved, G_FILE_TEST_EXISTS))
        {
            continue;
        }

        g_hash_table_add(seen, g_strdup(resolved));

        {
            gsize used = append_file(body, mention->path, resolved, budget);

            budget = (used < budget) ? budget - used : 0;
        }

        files = g_list_prepend(files, g_strdup(resolved));
    }

    g_list_free_full(mentions, (GDestroyNotify)ai_mention_free);

    files = g_list_reverse(files);

    if (out_files != NULL)
    {
        *out_files = files;
    }
    else
    {
        g_list_free_full(files, g_free);
    }

    if (body->len == 0)
    {
        return g_strdup(text);
    }

    return g_strdup_printf("%s\n\n--- Referenced files ---\n\n%s",
                           text, body->str);
}
