/*
 * ai-tool-executor.c - Built-in tool executor for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>

#include "convenience/ai-tool-executor.h"
#include "convenience/ai-search-provider.h"
#include "core/ai-error.h"
#include "core/ai-enums.h"
#include "core/ai-provider.h"
#include "model/ai-content-block.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-tool.h"
#include "model/ai-tool-use.h"

#define MAX_TURNS          20
#define WEB_FETCH_MAX_BYTES (100 * 1024)  /* 100 KB */
#define DEFAULT_MAX_TOKENS  4096
#define WEB_FETCH_TIMEOUT_SECS 30
/* Self-cleaning cache TTL: 15 minutes, in microseconds (monotonic clock). */
#define WEB_FETCH_CACHE_TTL_US (15 * 60 * (gint64)G_USEC_PER_SEC)
#define WEB_FETCH_USER_AGENT \
    "cmacs-ai/1.0 (ai-glib web_fetch; +https://github.com/zachp/cmacs)"

/* web_search: formatted-result cache TTL (shorter than web_fetch's, since
 * search freshness matters more than a fetched page's). */
#define WEB_SEARCH_CACHE_TTL_US (5 * 60 * (gint64)G_USEC_PER_SEC)
/* Per-result page-content excerpt cap (bytes) for fetch_content enrichment. */
#define WEB_SEARCH_EXCERPT_MAX 1500
/* Timeout for each enrichment page fetch. */
#define WEB_SEARCH_FETCH_TIMEOUT 15

/* ================================================================
 * Struct definition — must precede any code accessing its fields
 * ================================================================ */

typedef struct
{
    AiToolCallback  fn;
    gpointer        user_data;
    GDestroyNotify  user_data_free;
} CallbackEntry;

static void
callback_entry_free (gpointer data)
{
    CallbackEntry *entry = data;

    if (entry == NULL)
        return;

    if (entry->user_data_free != NULL && entry->user_data != NULL)
        entry->user_data_free (entry->user_data);

    g_slice_free (CallbackEntry, entry);
}

struct _AiToolExecutor
{
    GObject           parent_instance;
    GList            *tools;           /* GList<AiTool>, owned */
    AiSearchProvider *search_provider; /* nullable, ref'd */
    GHashTable       *callbacks;       /* str -> CallbackEntry, owned */
    AiProvider       *active_provider; /* borrowed; set only during a run() */
    gchar            *working_directory; /* nullable, owned */
};

G_DEFINE_TYPE(AiToolExecutor, ai_tool_executor, G_TYPE_OBJECT)

/* ================================================================
 * Internal run context (async -> sync bridge)
 * ================================================================ */

typedef struct
{
    /* Exactly one of these is set.  `loop' means a synchronous caller is
     * blocked in ai_tool_executor_run(); `task' means an asynchronous
     * one is waiting on ai_tool_executor_run_finish().  Everything else
     * about the turn loop is identical, which is why they share one
     * context rather than one being reimplemented in terms of the
     * other. */
    GMainLoop      *loop;
    GTask          *task;

    AiToolExecutor *executor;
    AiProvider     *provider;
    GList          *messages;      /* owned, grows during loop */
    gchar          *system_prompt; /* owned in async mode, borrowed in sync */
    gint            max_tokens;
    gint            max_turns;
    GCancellable   *cancellable;
    gint            turn_count;
    gchar          *result;        /* final text (transfer full to caller) */
    GError         *error;         /* propagated to caller */
} RunContext;

static void run_context_finish (RunContext *ctx);

/* Forward declaration */
static void run_context_send (RunContext *ctx);

/* End the run.  In synchronous mode the blocked caller resumes and does
 * its own teardown; in asynchronous mode nobody is waiting on the stack,
 * so the context owns itself and must clean up here. */
static void
run_context_finish (RunContext *ctx)
{
    if (ctx->loop != NULL)
    {
        g_main_loop_quit (ctx->loop);
        return;
    }

    if (ctx->error != NULL)
        g_task_return_error (ctx->task, g_steal_pointer (&ctx->error));
    else
        g_task_return_pointer (ctx->task, g_steal_pointer (&ctx->result),
                               g_free);

    /* g_task_return_* does not consume the reference from g_task_new. */
    g_clear_object (&ctx->task);
    g_clear_object (&ctx->executor);
    g_clear_object (&ctx->provider);
    g_clear_object (&ctx->cancellable);
    g_list_free_full (ctx->messages, g_object_unref);
    g_free (ctx->system_prompt);
    g_free (ctx->result);
    g_clear_error (&ctx->error);
    g_free (ctx);
}

static void
on_run_response (
    GObject      *source,
    GAsyncResult *async_result,
    gpointer      user_data
){
    RunContext *ctx = user_data;
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError)     err      = NULL;
    GList *iter;

    response = ai_provider_chat_finish (AI_PROVIDER (source), async_result, &err);

    if (err != NULL)
    {
        ctx->error = g_steal_pointer (&err);
        run_context_finish (ctx);
        return;
    }

    ctx->turn_count++;

    if (!ai_response_has_tool_use (response))
    {
        /* Final answer — grab text and quit.
         *
         * The assistant turn is appended to the message list on the way
         * out, even though nothing in this run will read it again.  A
         * caller using ai_tool_executor_run_full() to continue the
         * conversation needs it more than any of the intermediate ones:
         * without it the model's actual answer is the single thing
         * missing from the replayed history, so the next turn reads as a
         * follow-up to a question the model never appeared to answer. */
        ctx->result = ai_response_get_text (response);
        if (ctx->result != NULL)
        {
            AiMessage *final_msg = ai_message_new_assistant (ctx->result);
            ctx->messages = g_list_append (ctx->messages, final_msg);
        }
        run_context_finish (ctx);
        return;
    }

    /* Guard against infinite loops */
    if (ctx->turn_count >= ctx->max_turns)
    {
        g_set_error (&ctx->error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "ai_tool_executor_run: reached maximum turn limit (%d)",
                     ctx->max_turns);
        run_context_finish (ctx);
        return;
    }

    /*
     * Reconstruct the assistant message from the response content blocks.
     * We must include tool_use blocks (not just text) so the provider
     * can match them with our tool_result messages on the next turn.
     */
    {
        AiMessage *assistant_msg = ai_message_new (AI_ROLE_ASSISTANT);

        for (iter = ai_response_get_content_blocks (response);
             iter != NULL;
             iter = iter->next)
        {
            AiContentBlock *block = iter->data;

            ai_message_add_content_block (
                assistant_msg,
                (AiContentBlock *)g_object_ref (block)
            );
        }

        ctx->messages = g_list_append (ctx->messages, assistant_msg);
    }

    /* Execute each tool use and append result messages */
    {
        GList *tool_uses = ai_response_get_tool_uses (response);

        for (iter = tool_uses; iter != NULL; iter = iter->next)
        {
            AiToolUse       *tool_use   = iter->data;
            const gchar     *tool_id    = ai_tool_use_get_id (tool_use);
            const gchar     *tool_name  = ai_tool_use_get_name (tool_use);
            g_autofree gchar *tool_result = NULL;
            gboolean          is_error   = FALSE;
            AiMessage        *result_msg;

            /* g_debug, not g_warning: a tool call is the loop working
             * as designed.  As a warning it aborted any program run
             * with G_DEBUG=fatal-warnings -- including a GTest suite,
             * which is why the loop had no test until now. */
            g_debug ("ToolExecutor turn %d: calling tool '%s' (id=%s)",
                       ctx->turn_count, tool_name, tool_id);

            tool_result = ai_tool_executor_execute (
                ctx->executor, tool_use, ctx->cancellable, NULL);

            if (tool_result == NULL)
            {
                tool_result = g_strdup ("Error: tool execution failed");
                is_error = TRUE;
            }

            /* Pass tool_name so providers like Gemini (whose functionResponse
             * is keyed by name, not id) round-trip correctly. */
            result_msg = ai_message_new_tool_result_with_name (
                tool_id, tool_name, tool_result, is_error);
            ctx->messages = g_list_append (ctx->messages, result_msg);
        }

        g_list_free (tool_uses);
    }

    /* Continue conversation */
    run_context_send (ctx);
}

static void
run_context_send (RunContext *ctx)
{
    ai_provider_chat_async (
        ctx->provider,
        ctx->messages,
        ctx->system_prompt,
        ctx->max_tokens,
        ctx->executor->tools,
        ctx->cancellable,
        on_run_response,
        ctx
    );
}

/* ================================================================
 * Built-in tool implementations
 * ================================================================ */

/* Resolve PATH against the executor's working directory.
 *
 * An agent is given work to do somewhere -- a project, a checkout, a
 * notes tree -- and says "src/main.c", not an absolute path.  Without
 * this every relative path resolved against whatever directory the
 * host process happened to be in, which for an editor is wherever the
 * user last visited a file.  An absolute path is left alone, and with
 * no working directory set the behaviour is exactly as before. */
static gchar *
executor_resolve_path (
    AiToolExecutor  *self,
    const gchar     *path
){
    if (path == NULL)
        path = ".";

    if (self == NULL || self->working_directory == NULL
        || g_path_is_absolute (path))
        return g_strdup (path);

    return g_build_filename (self->working_directory, path, NULL);
}

static gchar *
tool_bash (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar                  *command;
    g_autoptr (GSubprocessLauncher) launcher = NULL;
    g_autoptr (GSubprocess)         proc     = NULL;
    g_autoptr (GBytes)              out      = NULL;
    gconstpointer                   data;
    gsize                           len      = 0;
    gchar                          *result;
    gint                            exit_status;

    command = ai_tool_use_get_input_string (tool_use, "command");
    if (command == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "bash: missing required parameter 'command'");
        return NULL;
    }

    /* GSubprocess rather than popen(): popen inherits the host
     * process's working directory with no way to override it, and
     * cannot be cancelled.  Both matter -- an agent is given work to do
     * in a particular tree, and a caller that gives up on a run should
     * not leave a build running. */
    launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                          | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    if (self != NULL && self->working_directory != NULL)
        g_subprocess_launcher_set_cwd (launcher, self->working_directory);

    proc = g_subprocess_launcher_spawn (launcher, error,
                                        "/bin/sh", "-c", command, NULL);
    if (proc == NULL)
        return NULL;

    if (!g_subprocess_communicate (proc, NULL, cancellable, &out, NULL, error))
        return NULL;

    data = out ? g_bytes_get_data (out, &len) : NULL;
    /* Command output is arbitrary bytes -- a build log carries progress
     * bars and stray locale-encoded text -- and it is about to become a
     * JSON string, so it has to be valid UTF-8 first. */
    result = data ? g_utf8_make_valid ((const gchar *) data, (gssize) len)
                  : g_strdup ("");

    exit_status = g_subprocess_get_if_exited (proc)
                  ? g_subprocess_get_exit_status (proc) : -1;

    if (exit_status > 0)
    {
        gchar *prefixed = g_strdup_printf ("[exit code %d]\n%s",
                                           exit_status, result);
        g_free (result);
        result = prefixed;
    }

    return result;
}

static gchar *
tool_read (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar      *path;
    g_autofree gchar *contents = NULL;
    gsize             length;
    gint              offset;
    gint              limit;
    const gchar      *start;
    gsize             available;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path = ai_tool_use_get_input_string (tool_use, "path");
    if (path == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "read: missing required parameter 'path'");
        return NULL;
    }

    resolved = executor_resolve_path (self, path);
    path = resolved;

    if (!g_file_get_contents (path, &contents, &length, error))
        return NULL;

    offset = ai_tool_use_get_input_int (tool_use, "offset", 0);
    limit  = ai_tool_use_get_input_int (tool_use, "limit", -1);

    if (offset < 0)
        offset = 0;
    if ((gsize)offset >= length)
        return g_strdup ("");

    start     = contents + (gsize)offset;
    available = length   - (gsize)offset;

    if (limit > 0 && (gsize)limit < available)
        available = (gsize)limit;

    return g_strndup (start, available);
}

static gchar *
tool_write (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *path;
    const gchar *content;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path    = ai_tool_use_get_input_string (tool_use, "path");
    content = ai_tool_use_get_input_string (tool_use, "content");

    if (path == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "write: missing required parameter 'path'");
        return NULL;
    }

    resolved = executor_resolve_path (self, path);
    path = resolved;

    if (content == NULL)
        content = "";

    if (!g_file_set_contents (path, content, -1, error))
        return NULL;

    return g_strdup ("OK");
}

static gchar *
tool_edit (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar      *path;
    const gchar      *old_string;
    const gchar      *new_string;
    g_autofree gchar *contents = NULL;
    gsize             length;
    gchar            *found;
    GString          *rebuilt;
    gsize             prefix_len;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path       = ai_tool_use_get_input_string (tool_use, "path");
    old_string = ai_tool_use_get_input_string (tool_use, "old_string");
    new_string = ai_tool_use_get_input_string (tool_use, "new_string");

    if (path == NULL || old_string == NULL || new_string == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "edit: missing required parameter(s): "
                             "path, old_string, new_string");
        return NULL;
    }

    resolved = executor_resolve_path (self, path);
    path = resolved;

    if (!g_file_get_contents (path, &contents, &length, error))
        return NULL;

    found = strstr (contents, old_string);
    if (found == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "edit: old_string not found in '%s'", path);
        return NULL;
    }

    prefix_len = (gsize)(found - contents);
    rebuilt    = g_string_new_len (contents, (gssize)prefix_len);
    g_string_append (rebuilt, new_string);
    g_string_append (rebuilt, found + strlen (old_string));

    if (!g_file_set_contents (path, rebuilt->str, (gssize)rebuilt->len, error))
    {
        g_string_free (rebuilt, TRUE);
        return NULL;
    }

    g_string_free (rebuilt, TRUE);
    return g_strdup ("OK");
}

/* ---- glob helpers ---- */

static void
glob_collect (
    const gchar  *base_dir,
    GPatternSpec *pattern,
    GString      *output
){
    g_autoptr(GError) dir_err = NULL;
    GDir        *dir;
    const gchar *name;

    dir = g_dir_open (base_dir, 0, &dir_err);
    if (dir == NULL)
        return;

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        g_autofree gchar *full = g_build_filename (base_dir, name, NULL);

        if (g_file_test (full, G_FILE_TEST_IS_DIR))
        {
            glob_collect (full, pattern, output);
        }
        else if (g_pattern_spec_match_string (pattern, name))
        {
            g_string_append (output, full);
            g_string_append_c (output, '\n');
        }
    }

    g_dir_close (dir);
}

static gchar *
tool_glob (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar          *pattern_str;
    const gchar          *path;
    g_autoptr(GPatternSpec) pattern = NULL;
    GString              *output;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;
    (void)error;

    pattern_str = ai_tool_use_get_input_string (tool_use, "pattern");
    if (pattern_str == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "glob: missing required parameter 'pattern'");
        return NULL;
    }

    path = ai_tool_use_get_input_string (tool_use, "path");
    resolved = executor_resolve_path (self, path);
    path = resolved;

    pattern = g_pattern_spec_new (pattern_str);
    output  = g_string_new (NULL);

    glob_collect (path, pattern, output);

    return g_string_free (output, FALSE);
}

/* ---- grep helpers ---- */

static void
grep_one_file (
    const gchar *filepath,
    GRegex      *regex,
    GString     *output
){
    g_autofree gchar  *contents = NULL;
    gsize              length;
    gchar            **lines;
    gint               i;
    gint               line_num;

    if (!g_file_get_contents (filepath, &contents, &length, NULL))
        return;

    lines = g_strsplit (contents, "\n", -1);

    for (i = 0, line_num = 1; lines[i] != NULL; i++, line_num++)
    {
        if (g_regex_match (regex, lines[i], 0, NULL))
        {
            g_string_append_printf (output, "%s:%d: %s\n",
                                    filepath, line_num, lines[i]);
        }
    }

    g_strfreev (lines);
}

static void
grep_dir_recurse (
    const gchar  *base_dir,
    GPatternSpec *file_pattern, /* nullable — match all files */
    GRegex       *regex,
    GString      *output
){
    g_autoptr(GError) dir_err = NULL;
    GDir        *dir;
    const gchar *name;

    dir = g_dir_open (base_dir, 0, &dir_err);
    if (dir == NULL)
        return;

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        g_autofree gchar *full = g_build_filename (base_dir, name, NULL);

        if (g_file_test (full, G_FILE_TEST_IS_DIR))
        {
            grep_dir_recurse (full, file_pattern, regex, output);
        }
        else if (file_pattern == NULL
                 || g_pattern_spec_match_string (file_pattern, name))
        {
            grep_one_file (full, regex, output);
        }
    }

    g_dir_close (dir);
}

static gchar *
tool_grep (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar           *pattern_str;
    const gchar           *path;
    const gchar           *glob_str;
    g_autoptr(GRegex)      regex        = NULL;
    g_autoptr(GPatternSpec) file_pattern = NULL;
    GString               *output;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    pattern_str = ai_tool_use_get_input_string (tool_use, "pattern");
    if (pattern_str == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "grep: missing required parameter 'pattern'");
        return NULL;
    }

    regex = g_regex_new (pattern_str, 0, 0, error);
    if (regex == NULL)
        return NULL;

    path     = ai_tool_use_get_input_string (tool_use, "path");
    glob_str = ai_tool_use_get_input_string (tool_use, "glob");

    if (glob_str != NULL)
        file_pattern = g_pattern_spec_new (glob_str);

    resolved = executor_resolve_path (self, path);
    path = resolved;

    output = g_string_new (NULL);

    if (g_file_test (path, G_FILE_TEST_IS_DIR))
        grep_dir_recurse (path, file_pattern, regex, output);
    else
        grep_one_file (path, regex, output);

    return g_string_free (output, FALSE);
}

static gchar *
tool_ls (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *path;
    GDir        *dir;
    const gchar *name;
    GString     *output;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path = ai_tool_use_get_input_string (tool_use, "path");
    resolved = executor_resolve_path (self, path);
    path = resolved;

    dir = g_dir_open (path, 0, error);
    if (dir == NULL)
        return NULL;

    output = g_string_new (NULL);

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        g_autofree gchar *full = g_build_filename (path, name, NULL);
        struct stat  st;
        const gchar *type;

        if (stat (full, &st) == 0)
        {
            type = S_ISDIR (st.st_mode) ? "d" : "-";
            g_string_append_printf (output, "%s  %10" G_GINT64_FORMAT "  %s\n",
                                    type, (gint64)st.st_size, name);
        }
        else
        {
            g_string_append_printf (output, "?  %s\n", name);
        }
    }

    g_dir_close (dir);

    return g_string_free (output, FALSE);
}

/* ---- web_fetch: response cache (URL -> converted text, 15 min TTL) ---- */

typedef struct
{
    gint64  stamp;   /* g_get_monotonic_time() at insertion (microseconds) */
    gchar  *text;    /* converted/cleaned body, owned */
} WebFetchCacheEntry;

static GHashTable *web_fetch_cache = NULL;     /* url -> WebFetchCacheEntry */
static GMutex      web_fetch_cache_lock;       /* static GMutex: zero-init OK */

static void
web_fetch_cache_entry_free (gpointer data)
{
    WebFetchCacheEntry *entry = data;

    if (entry == NULL)
        return;

    g_free (entry->text);
    g_slice_free (WebFetchCacheEntry, entry);
}

/* Drop every expired entry. Caller must hold web_fetch_cache_lock. */
static void
web_fetch_cache_sweep_locked (gint64 now)
{
    GHashTableIter iter;
    gpointer       key;
    gpointer       value;

    if (web_fetch_cache == NULL)
        return;

    g_hash_table_iter_init (&iter, web_fetch_cache);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        WebFetchCacheEntry *entry = value;

        if (now - entry->stamp > WEB_FETCH_CACHE_TTL_US)
            g_hash_table_iter_remove (&iter);
    }
}

/* Returns a fresh copy of the cached text for URL, or NULL on miss/expiry. */
static gchar *
web_fetch_cache_lookup (const gchar *url)
{
    gchar  *result = NULL;
    gint64  now;

    g_mutex_lock (&web_fetch_cache_lock);
    now = g_get_monotonic_time ();

    if (web_fetch_cache != NULL)
    {
        WebFetchCacheEntry *entry = g_hash_table_lookup (web_fetch_cache, url);

        if (entry != NULL)
        {
            if (now - entry->stamp <= WEB_FETCH_CACHE_TTL_US)
                result = g_strdup (entry->text);
            else
                g_hash_table_remove (web_fetch_cache, url);
        }

        web_fetch_cache_sweep_locked (now);
    }

    g_mutex_unlock (&web_fetch_cache_lock);
    return result;
}

static void
web_fetch_cache_store (const gchar *url, const gchar *text)
{
    WebFetchCacheEntry *entry;

    g_mutex_lock (&web_fetch_cache_lock);

    if (web_fetch_cache == NULL)
        web_fetch_cache = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, web_fetch_cache_entry_free);

    entry = g_slice_new0 (WebFetchCacheEntry);
    entry->stamp = g_get_monotonic_time ();
    entry->text  = g_strdup (text);

    g_hash_table_replace (web_fetch_cache, g_strdup (url), entry);

    g_mutex_unlock (&web_fetch_cache_lock);
}

/* ---- web_search: formatted-result cache (key -> formatted text, 5 min) ----
 * Reuses WebFetchCacheEntry. Keyed by provider + query + options so repeated
 * identical web_search calls within one agent run skip the network. */

static GHashTable *web_search_cache = NULL;     /* key -> WebFetchCacheEntry */
static GMutex      web_search_cache_lock;       /* static GMutex: zero-init OK */

static void
web_search_cache_sweep_locked (gint64 now)
{
    GHashTableIter iter;
    gpointer       key;
    gpointer       value;

    if (web_search_cache == NULL)
        return;

    g_hash_table_iter_init (&iter, web_search_cache);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        WebFetchCacheEntry *entry = value;

        if (now - entry->stamp > WEB_SEARCH_CACHE_TTL_US)
            g_hash_table_iter_remove (&iter);
    }
}

static gchar *
web_search_cache_lookup (const gchar *key)
{
    gchar  *result = NULL;
    gint64  now;

    g_mutex_lock (&web_search_cache_lock);
    now = g_get_monotonic_time ();

    if (web_search_cache != NULL)
    {
        WebFetchCacheEntry *entry = g_hash_table_lookup (web_search_cache, key);

        if (entry != NULL)
        {
            if (now - entry->stamp <= WEB_SEARCH_CACHE_TTL_US)
                result = g_strdup (entry->text);
            else
                g_hash_table_remove (web_search_cache, key);
        }

        web_search_cache_sweep_locked (now);
    }

    g_mutex_unlock (&web_search_cache_lock);
    return result;
}

static void
web_search_cache_store (const gchar *key, const gchar *text)
{
    WebFetchCacheEntry *entry;

    g_mutex_lock (&web_search_cache_lock);

    if (web_search_cache == NULL)
        web_search_cache = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, web_fetch_cache_entry_free);

    entry = g_slice_new0 (WebFetchCacheEntry);
    entry->stamp = g_get_monotonic_time ();
    entry->text  = g_strdup (text);

    g_hash_table_replace (web_search_cache, g_strdup (key), entry);

    g_mutex_unlock (&web_search_cache_lock);
}

/* ---- web_fetch: HTML -> readable markdown-ish text ---- */

static gboolean
node_name_is (xmlNode *node, const gchar *name)
{
    return node->name != NULL
        && g_ascii_strcasecmp ((const gchar *)node->name, name) == 0;
}

/* Find the first <base href> in the tree (HTML's per-document base override),
 * returning its href (caller frees with xmlFree) or NULL. */
static xmlChar *
html_find_base_href (xmlNode *node)
{
    xmlNode *cur;

    for (cur = node; cur != NULL; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
            continue;

        if (node_name_is (cur, "base"))
        {
            xmlChar *href = xmlGetProp (cur, (const xmlChar *)"href");

            if (href != NULL && href[0] != '\0')
                return href;
            if (href != NULL)
                xmlFree (href);
        }

        {
            xmlChar *found = html_find_base_href (cur->children);

            if (found != NULL)
                return found;
        }
    }
    return NULL;
}

/* Walk the HTML tree appending markdown-ish text. BASE (nullable) is the
 * effective base URL used to resolve <img> srcs to absolute URLs. */
static void
html_walk (xmlNode *node, GString *out, const gchar *base)
{
    xmlNode *cur;

    for (cur = node; cur != NULL; cur = cur->next)
    {
        if (cur->type == XML_TEXT_NODE)
        {
            if (cur->content != NULL)
                g_string_append (out, (const gchar *)cur->content);
            continue;
        }

        if (cur->type != XML_ELEMENT_NODE)
            continue;

        /* Skip non-content subtrees entirely. */
        if (node_name_is (cur, "script")   || node_name_is (cur, "style")
            || node_name_is (cur, "head")  || node_name_is (cur, "noscript")
            || node_name_is (cur, "svg")   || node_name_is (cur, "template"))
            continue;

        if (node_name_is (cur, "br"))
        {
            g_string_append_c (out, '\n');
            continue;
        }

        /* Headings h1..h6 -> markdown '#' prefixes. */
        if (cur->name != NULL
            && (cur->name[0] == 'h' || cur->name[0] == 'H')
            && cur->name[1] >= '1' && cur->name[1] <= '6'
            && cur->name[2] == '\0')
        {
            gint level = cur->name[1] - '0';
            gint i;

            g_string_append_c (out, '\n');
            for (i = 0; i < level; i++)
                g_string_append_c (out, '#');
            g_string_append_c (out, ' ');
            html_walk (cur->children, out, base);
            g_string_append_c (out, '\n');
            continue;
        }

        /* Links -> [text](href). */
        if (node_name_is (cur, "a"))
        {
            xmlChar *href = xmlGetProp (cur, (const xmlChar *)"href");

            if (href != NULL && href[0] != '\0')
            {
                g_string_append_c (out, '[');
                html_walk (cur->children, out, base);
                g_string_append_printf (out, "](%s)", (const gchar *)href);
            }
            else
            {
                html_walk (cur->children, out, base);
            }

            if (href != NULL)
                xmlFree (href);
            continue;
        }

        /* Images -> ![alt](src).  The src is resolved to an absolute URL
         * against the document base (set from the page URL at parse time, or
         * a <base href>), so the model gets a directly-fetchable link rather
         * than a site-relative or protocol-relative path. */
        if (node_name_is (cur, "img"))
        {
            xmlChar *src = xmlGetProp (cur, (const xmlChar *)"src");

            if (src != NULL && src[0] != '\0')
            {
                xmlChar *alt = xmlGetProp (cur, (const xmlChar *)"alt");
                xmlChar *abs = xmlBuildURI (src, (const xmlChar *)base);
                const xmlChar *url = (abs != NULL) ? abs : src;

                g_string_append_printf (out, "![%s](%s)",
                                        (alt != NULL && alt[0] != '\0')
                                          ? (const gchar *)alt : "",
                                        (const gchar *)url);
                if (abs != NULL) xmlFree (abs);
                if (alt != NULL) xmlFree (alt);
            }
            if (src != NULL)
                xmlFree (src);
            continue;   /* void element: no children */
        }

        /* List items get a bullet; block elements get surrounding newlines. */
        if (node_name_is (cur, "li"))
        {
            g_string_append (out, "\n- ");
            html_walk (cur->children, out, base);
            g_string_append_c (out, '\n');
            continue;
        }

        {
            gboolean block =
                node_name_is (cur, "p")       || node_name_is (cur, "div")
                || node_name_is (cur, "tr")   || node_name_is (cur, "ul")
                || node_name_is (cur, "ol")   || node_name_is (cur, "table")
                || node_name_is (cur, "section") || node_name_is (cur, "article")
                || node_name_is (cur, "header")  || node_name_is (cur, "footer")
                || node_name_is (cur, "blockquote");

            if (block)
                g_string_append_c (out, '\n');

            html_walk (cur->children, out, base);

            if (block)
                g_string_append_c (out, '\n');
        }
    }
}

/* Parse HTML and return cleaned text (transfer full), or NULL on parse fail.
 * ENCODING is the charset hint (e.g. from the HTTP Content-Type); when NULL we
 * default to UTF-8 rather than libxml's legacy ISO-8859-1, since that is the
 * dominant encoding for modern pages that omit a charset declaration.
 * BASE_URL (nullable) is the page's URL; it becomes the document base so that
 * <img> srcs (and <base href>) resolve to absolute, fetchable URLs. */
static gchar *
html_to_text (const gchar *html, gsize len, const gchar *encoding,
              const gchar *base_url)
{
    htmlDocPtr   doc;
    xmlNode     *root;
    GString     *out;
    gchar       *raw;
    GString     *clean;
    const gchar *p;
    const gchar *enc;
    gint         nl_run;
    gboolean     space_pending;
    gboolean     at_line_start;

    enc = (encoding != NULL && encoding[0] != '\0') ? encoding : "UTF-8";
    doc = htmlReadMemory (html, (int)len, base_url, enc,
                          HTML_PARSE_RECOVER | HTML_PARSE_NOERROR
                          | HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (doc == NULL)
        return NULL;

    out  = g_string_new (NULL);
    root = xmlDocGetRootElement (doc);
    if (root != NULL)
    {
        g_autofree gchar *eff_base = NULL;
        xmlChar          *bhref    = html_find_base_href (root);

        if (bhref != NULL)
        {
            /* A <base href> can itself be relative to the page URL. */
            xmlChar *abs = (base_url != NULL)
                           ? xmlBuildURI (bhref, (const xmlChar *)base_url)
                           : NULL;
            eff_base = g_strdup ((const gchar *)(abs != NULL ? abs : bhref));
            if (abs != NULL)
                xmlFree (abs);
            xmlFree (bhref);
        }
        else if (base_url != NULL)
        {
            eff_base = g_strdup (base_url);
        }

        html_walk (root, out, eff_base);
    }
    xmlFreeDoc (doc);

    /* Whitespace cleanup: collapse spaces/tabs to one, drop leading spaces,
     * and limit consecutive blank lines to a single one. */
    raw           = g_string_free (out, FALSE);
    clean         = g_string_new (NULL);
    nl_run        = 0;
    space_pending = FALSE;
    at_line_start = TRUE;

    for (p = raw; *p != '\0'; p++)
    {
        gchar c = *p;

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            nl_run++;
            space_pending = FALSE;
            continue;
        }

        if (c == ' ' || c == '\t')
        {
            space_pending = TRUE;
            continue;
        }

        if (nl_run > 0)
        {
            gint emit = (nl_run >= 2) ? 2 : 1;
            gint i;

            for (i = 0; i < emit; i++)
                g_string_append_c (clean, '\n');

            nl_run        = 0;
            at_line_start = TRUE;
            space_pending = FALSE;
        }

        if (space_pending && !at_line_start)
            g_string_append_c (clean, ' ');
        space_pending = FALSE;

        g_string_append_c (clean, c);
        at_line_start = FALSE;
    }

    g_free (raw);

    while (clean->len > 0
           && (clean->str[clean->len - 1] == '\n'
               || clean->str[clean->len - 1] == ' '))
        g_string_truncate (clean, clean->len - 1);

    return g_string_free (clean, FALSE);
}

/* Truncate BODY in place to at most MAX bytes on a valid UTF-8 boundary,
 * appending a marker when truncation occurred. No-op if already within MAX. */
static void
web_fetch_truncate (GString *body, gsize max)
{
    const gchar *valid_end;
    gsize        cut = max;

    if (body->len <= max)
        return;

    /* Never split a multi-byte UTF-8 sequence: if the prefix up to the cap
     * is not valid UTF-8, back the cut up to the last good boundary. */
    if (!g_utf8_validate (body->str, (gssize)cut, &valid_end))
        cut = (gsize)(valid_end - body->str);

    g_string_truncate (body, cut);
    g_string_append (body, "\n\n[truncated at 100 KB]");
}

/* Build the model-facing body (transfer full) from a fetched response.
 * Prepends a redirect notice when the final host differs from the requested
 * one, converts HTML to text (other text-ish types pass through, binary gets a
 * placeholder), and truncates to the size cap. Pure: no network or Soup types,
 * so it is unit-testable with synthetic inputs.
 *   CONTENT_TYPE / CHARSET : from the response (both nullable).
 *   REQ_HOST / FINAL_HOST  : requested vs final host (nullable; notice emitted
 *                            only when both are set and differ).
 *   FINAL_URL              : full final URL for the notice text (nullable). */
static gchar *
web_fetch_build_body (
    const gchar *data,
    gsize        size,
    const gchar *content_type,
    const gchar *charset,
    const gchar *req_host,
    const gchar *final_host,
    const gchar *final_url
){
    GString *body = g_string_new (NULL);

    if (req_host != NULL && final_host != NULL
        && g_ascii_strcasecmp (req_host, final_host) != 0)
    {
        g_string_append_printf (body, "[redirected to %s]\n\n",
                                final_url ? final_url : final_host);
    }

    if (content_type != NULL
        && (g_ascii_strcasecmp (content_type, "text/html") == 0
            || g_ascii_strcasecmp (content_type, "application/xhtml+xml") == 0))
    {
        g_autofree gchar *text = html_to_text (data, size, charset, final_url);

        if (text != NULL)
            g_string_append (body, text);
        else
            g_string_append_len (body, data, (gssize)size);
    }
    else if (content_type == NULL
             || g_str_has_prefix (content_type, "text/")
             || g_ascii_strcasecmp (content_type, "application/json") == 0
             || g_ascii_strcasecmp (content_type, "application/xml") == 0
             || g_ascii_strcasecmp (content_type, "application/javascript") == 0)
    {
        g_string_append_len (body, data, (gssize)size);
    }
    else
    {
        g_string_append_printf (body,
            "[binary content: %" G_GSIZE_FORMAT " bytes, type %s "
            "\xe2\x80\x94 not returned as text]", size, content_type);
    }

    web_fetch_truncate (body, WEB_FETCH_MAX_BYTES);

    return g_string_free (body, FALSE);
}

/* ---- web_fetch: optional prompt-based extraction via a sub-model ---- */

typedef struct
{
    GMainLoop *loop;
    gchar     *result;
    GError    *error;
} WebFetchExtractCtx;

static void
on_web_fetch_extract (GObject *source, GAsyncResult *res, gpointer user_data)
{
    WebFetchExtractCtx   *ec       = user_data;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish (AI_PROVIDER (source), res, &ec->error);
    if (response != NULL)
        ec->result = ai_response_get_text (response);

    g_main_loop_quit (ec->loop);
}

/* Run PROMPT over CONTENT with PROVIDER and return the model's text, or NULL
 * (with a warning) so the caller can fall back to the raw content. */
static gchar *
web_fetch_extract (
    AiProvider   *provider,
    const gchar  *url,
    const gchar  *content,
    const gchar  *prompt,
    GCancellable *cancellable
){
    WebFetchExtractCtx  ec        = { NULL, NULL, NULL };
    GList              *messages  = NULL;
    g_autofree gchar   *user_text = NULL;
    AiMessage          *msg;

    user_text = g_strdup_printf (
        "Content fetched from %s:\n\n%s\n\nTask: %s", url, content, prompt);
    msg      = ai_message_new_user (user_text);
    messages = g_list_append (NULL, msg);

    ec.loop = g_main_loop_new (NULL, FALSE);
    ai_provider_chat_async (
        provider, messages,
        "You extract and summarise fetched web content. Return only what the "
        "task asks for, concisely, with no preamble.",
        DEFAULT_MAX_TOKENS,
        NULL,            /* no tools for the sub-call */
        cancellable,
        on_web_fetch_extract, &ec);
    g_main_loop_run (ec.loop);
    g_main_loop_unref (ec.loop);

    g_list_free_full (messages, g_object_unref);

    if (ec.error != NULL)
    {
        g_warning ("web_fetch: prompt extraction failed: %s", ec.error->message);
        g_clear_error (&ec.error);
        return NULL;
    }

    return ec.result;   /* may be NULL if the model returned no text */
}

/* TRUE if an http:// URL should be speculatively upgraded to https://.
 * False for localhost and IP-literal hosts (dev/internal endpoints). */
static gboolean
web_fetch_should_upgrade (const gchar *url)
{
    g_autoptr(GUri)  uri  = NULL;
    const gchar     *host;

    uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
    if (uri == NULL)
        return FALSE;

    host = g_uri_get_host (uri);
    if (host == NULL || host[0] == '\0')
        return FALSE;

    if (g_ascii_strcasecmp (host, "localhost") == 0)
        return FALSE;

    if (g_hostname_is_ip_address (host))
        return FALSE;

    return TRUE;
}

/* GET URL with our headers; returns body bytes and (transfer full) *out_msg. */
static GBytes *
web_fetch_get (
    SoupSession   *session,
    const gchar   *url,
    SoupMessage  **out_msg,
    GCancellable  *cancellable,
    GError       **error
){
    SoupMessage        *msg;
    SoupMessageHeaders *req_headers;
    GBytes             *bytes;

    msg = soup_message_new ("GET", url);
    if (msg == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "web_fetch: invalid URL '%s'", url);
        return NULL;
    }

    req_headers = soup_message_get_request_headers (msg);
    soup_message_headers_replace (req_headers, "User-Agent",
                                  WEB_FETCH_USER_AGENT);
    soup_message_headers_replace (req_headers, "Accept",
        "text/html,application/xhtml+xml,text/plain,"
        "application/json;q=0.9,*/*;q=0.8");

    bytes = soup_session_send_and_read (session, msg, cancellable, error);
    if (bytes == NULL)
    {
        g_object_unref (msg);
        return NULL;
    }

    *out_msg = msg;
    return bytes;
}

static gchar *
tool_web_fetch (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar            *url;
    const gchar            *prompt;
    g_autofree gchar       *converted = NULL;

    url = ai_tool_use_get_input_string (tool_use, "url");
    if (url == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "web_fetch: missing required parameter 'url'");
        return NULL;
    }

    prompt = ai_tool_use_get_input_string (tool_use, "prompt");

    /* 1. Cache lookup (keyed on the original URL; stores converted text). */
    converted = web_fetch_cache_lookup (url);

    /* 2. On miss, fetch and convert. */
    if (converted == NULL)
    {
        g_autoptr(SoupSession)  session    = NULL;
        g_autoptr(SoupMessage)  msg        = NULL;
        g_autoptr(GBytes)       bytes      = NULL;
        g_autoptr(GError)       local_err  = NULL;
        g_autofree gchar       *fetch_url  = NULL;
        g_autoptr(GUri)         req_uri    = NULL;
        gboolean                upgraded   = FALSE;
        guint                   status_code;
        const gchar            *data;
        gsize                   size;
        const gchar            *content_type;
        GHashTable             *ct_params = NULL;
        const gchar            *charset;
        SoupMessageHeaders     *resp_headers;
        GUri                   *final_uri;
        const gchar            *req_host;
        const gchar            *final_host;
        gchar                  *final_str;

        session = soup_session_new ();
        g_object_set (session, "timeout", (guint)WEB_FETCH_TIMEOUT_SECS, NULL);

        /* http:// -> https:// upgrade, with fall-back to the original.
         * Skipped for localhost / IP-literal hosts: those are dev or internal
         * endpoints that are rarely TLS, and a speculative https probe there
         * just stalls until the timeout before falling back. */
        if (g_str_has_prefix (url, "http://") && web_fetch_should_upgrade (url))
        {
            fetch_url = g_strconcat ("https://",
                                     url + strlen ("http://"), NULL);
            upgraded = TRUE;
        }
        else
        {
            fetch_url = g_strdup (url);
        }

        bytes = web_fetch_get (session, fetch_url, &msg, cancellable, &local_err);
        if (bytes == NULL && upgraded)
        {
            g_clear_error (&local_err);
            bytes = web_fetch_get (session, url, &msg, cancellable, &local_err);
        }
        if (bytes == NULL)
        {
            g_propagate_error (error, g_steal_pointer (&local_err));
            return NULL;
        }

        status_code = soup_message_get_status (msg);
        if (status_code < 200 || status_code >= 300)
        {
            g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                         "web_fetch: HTTP %u for '%s'", status_code, url);
            return NULL;
        }

        data         = g_bytes_get_data (bytes, &size);
        resp_headers = soup_message_get_response_headers (msg);
        content_type = soup_message_headers_get_content_type (resp_headers,
                                                              &ct_params);
        charset      = (ct_params != NULL)
                       ? g_hash_table_lookup (ct_params, "charset") : NULL;

        req_uri    = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
        req_host   = (req_uri != NULL) ? g_uri_get_host (req_uri) : NULL;
        final_uri  = soup_message_get_uri (msg);
        final_host = (final_uri != NULL) ? g_uri_get_host (final_uri) : NULL;
        final_str  = (final_uri != NULL) ? g_uri_to_string (final_uri) : NULL;

        converted = web_fetch_build_body (data, size, content_type, charset,
                                          req_host, final_host, final_str);
        g_free (final_str);
        g_clear_pointer (&ct_params, g_hash_table_unref);

        web_fetch_cache_store (url, converted);
    }

    /* 3. Optional prompt-based extraction via the active run's provider. */
    if (prompt != NULL && prompt[0] != '\0'
        && self != NULL && self->active_provider != NULL)
    {
        gchar *extracted = web_fetch_extract (self->active_provider, url,
                                              converted, prompt, cancellable);
        if (extracted != NULL)
            return extracted;
        /* On failure, fall through and return the converted content. */
    }

    return g_steal_pointer (&converted);
}

/* UTF-8-safe truncation of TEXT to at most MAX bytes, with an ellipsis when
 * cut. Used to keep fetch_content excerpts small in the model's context. */
static gchar *
web_search_excerpt (const gchar *text, gsize max)
{
    gsize len;
    gsize cut;

    if (text == NULL)
        return NULL;

    len = strlen (text);
    if (len <= max)
        return g_strdup (text);

    cut = max;
    /* Back off any UTF-8 continuation bytes so we cut on a char boundary. */
    while (cut > 0 && (((guchar) text[cut]) & 0xC0) == 0x80)
        cut--;

    return g_strdup_printf ("%.*s\xe2\x80\xa6", (int) cut, text);
}

/* Fetch URL and return a short readable excerpt of its page text, reusing the
 * web_fetch pipeline + cache. Returns NULL on any failure (best-effort). */
static gchar *
web_search_fetch_excerpt (const gchar *url, GCancellable *cancellable)
{
    g_autofree gchar *converted = NULL;

    converted = web_fetch_cache_lookup (url);

    if (converted == NULL)
    {
        g_autoptr(SoupSession)  session   = NULL;
        g_autoptr(SoupMessage)  msg       = NULL;
        g_autoptr(GBytes)       bytes     = NULL;
        g_autoptr(GError)       local_err = NULL;
        SoupMessageHeaders     *resp_headers;
        const gchar            *data;
        gsize                   size;
        const gchar            *content_type;
        GHashTable             *ct_params = NULL;
        const gchar            *charset;
        guint                   status;

        session = soup_session_new ();
        g_object_set (session, "timeout", (guint) WEB_SEARCH_FETCH_TIMEOUT, NULL);

        bytes = web_fetch_get (session, url, &msg, cancellable, &local_err);
        if (bytes == NULL)
            return NULL;

        status = soup_message_get_status (msg);
        if (status < 200 || status >= 300)
            return NULL;

        data         = g_bytes_get_data (bytes, &size);
        resp_headers = soup_message_get_response_headers (msg);
        content_type = soup_message_headers_get_content_type (resp_headers,
                                                              &ct_params);
        charset      = (ct_params != NULL)
                       ? g_hash_table_lookup (ct_params, "charset") : NULL;

        converted = web_fetch_build_body (data, size, content_type, charset,
                                          NULL, NULL, NULL);
        g_clear_pointer (&ct_params, g_hash_table_unref);

        if (converted != NULL)
            web_fetch_cache_store (url, converted);
    }

    if (converted == NULL)
        return NULL;

    return web_search_excerpt (converted, WEB_SEARCH_EXCERPT_MAX);
}

/* Build an AiSearchOptions from the web_search tool inputs. */
static AiSearchOptions *
web_search_build_options (AiToolUse *tool_use)
{
    AiSearchOptions *options = ai_search_options_new ();
    const gchar     *s;
    gint64           count;

    count = ai_tool_use_get_input_int (tool_use, "count", -1);
    if (count > 0)
        ai_search_options_set_count (options, (guint) count);

    s = ai_tool_use_get_input_string (tool_use, "freshness");
    if (s != NULL)
        ai_search_options_set_freshness (options,
                                         ai_search_freshness_from_string (s));

    s = ai_tool_use_get_input_string (tool_use, "safesearch");
    if (s != NULL)
        ai_search_options_set_safesearch (options,
                                          ai_search_safe_search_from_string (s));

    s = ai_tool_use_get_input_string (tool_use, "country");
    if (s != NULL)
        ai_search_options_set_country (options, s);

    s = ai_tool_use_get_input_string (tool_use, "language");
    if (s != NULL)
        ai_search_options_set_language (options, s);

    s = ai_tool_use_get_input_string (tool_use, "site");
    if (s != NULL)
        ai_search_options_set_site (options, s);

    ai_search_options_set_fetch_content (
        options, ai_tool_use_get_input_boolean (tool_use, "fetch_content",
                                                FALSE));

    return options;
}

/* Cache key: provider type + query + every option that affects the output. */
static gchar *
web_search_cache_key (
    AiSearchProvider *provider,
    const gchar      *query,
    AiSearchOptions  *options
){
    const gchar *country  = ai_search_options_get_country (options);
    const gchar *language = ai_search_options_get_language (options);
    const gchar *site     = ai_search_options_get_site (options);

    return g_strdup_printf (
        "%s|%s|c=%u|f=%d|s=%d|cc=%s|lang=%s|site=%s|off=%u|fc=%d|fn=%u",
        G_OBJECT_TYPE_NAME (provider), query,
        ai_search_options_get_count (options),
        (int) ai_search_options_get_freshness (options),
        (int) ai_search_options_get_safesearch (options),
        country  != NULL ? country  : "",
        language != NULL ? language : "",
        site     != NULL ? site     : "",
        ai_search_options_get_offset (options),
        ai_search_options_get_fetch_content (options) ? 1 : 0,
        ai_search_options_get_fetch_count (options));
}

static gchar *
tool_web_search (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar              *query;
    g_autoptr(AiSearchOptions) options   = NULL;
    g_autofree gchar         *cache_key  = NULL;
    g_autofree gchar         *cached     = NULL;
    g_autofree gchar         *formatted  = NULL;
    g_autoptr(GError)         local_err  = NULL;
    GList                    *results;
    gboolean                  fetch_content;

    if (self->search_provider == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "web_search: no search provider configured; "
                             "call ai_tool_executor_set_search_provider() first");
        return NULL;
    }

    query = ai_tool_use_get_input_string (tool_use, "query");
    if (query == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "web_search: missing required parameter 'query'");
        return NULL;
    }

    options       = web_search_build_options (tool_use);
    fetch_content = ai_search_options_get_fetch_content (options);

    /* Cache hit: identical query+options within the TTL skips the network. */
    cache_key = web_search_cache_key (self->search_provider, query, options);
    cached    = web_search_cache_lookup (cache_key);
    if (cached != NULL)
        return g_steal_pointer (&cached);

    results = ai_search_provider_search (self->search_provider, query, options,
                                         cancellable, &local_err);
    if (local_err != NULL)
    {
        g_propagate_error (error, g_steal_pointer (&local_err));
        return NULL;
    }

    /* Optional enrichment: fetch the top results' pages and attach excerpts. */
    if (fetch_content && results != NULL)
    {
        guint  budget = ai_search_options_get_fetch_count (options);
        guint  done   = 0;
        GList *l;

        for (l = results; l != NULL && done < budget; l = l->next)
        {
            AiSearchResult *r   = l->data;
            const gchar    *url = ai_search_result_get_url (r);
            gchar          *excerpt;

            if (url == NULL || *url == '\0')
                continue;

            excerpt = web_search_fetch_excerpt (url, cancellable);
            if (excerpt != NULL)
            {
                ai_search_result_set_content (r, excerpt);
                g_free (excerpt);
                done++;
            }
        }
    }

    formatted = ai_search_results_format (results, query, fetch_content);
    g_list_free_full (results, g_object_unref);

    if (formatted != NULL)
        web_search_cache_store (cache_key, formatted);

    return g_steal_pointer (&formatted);
}

/* ================================================================
 * Tool dispatch table
 * ================================================================ */

typedef gchar * (*ToolFn) (AiToolExecutor *, AiToolUse *,
                            GCancellable *, GError **);

typedef struct
{
    const gchar *name;
    ToolFn       fn;
} ToolEntry;

static const ToolEntry BUILTIN_TOOLS[] = {
    { "bash",       tool_bash       },
    { "read",       tool_read       },
    { "write",      tool_write      },
    { "edit",       tool_edit       },
    { "glob",       tool_glob       },
    { "grep",       tool_grep       },
    { "ls",         tool_ls         },
    { "web_fetch",  tool_web_fetch  },
    { "web_search", tool_web_search },
    { NULL, NULL }
};

/* ================================================================
 * GObject plumbing
 * ================================================================ */

static void
ai_tool_executor_finalize (GObject *object)
{
    AiToolExecutor *self = AI_TOOL_EXECUTOR (object);

    g_list_free_full (self->tools, g_object_unref);
    self->tools = NULL;
    g_clear_object (&self->search_provider);
    g_clear_pointer (&self->callbacks, g_hash_table_unref);
    g_clear_pointer (&self->working_directory, g_free);

    G_OBJECT_CLASS (ai_tool_executor_parent_class)->finalize (object);
}

static void
ai_tool_executor_class_init (AiToolExecutorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = ai_tool_executor_finalize;
}

static void
ai_tool_executor_init (AiToolExecutor *self)
{
    self->tools           = NULL;
    self->search_provider = NULL;
    self->callbacks       = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, callback_entry_free);
}

/* ================================================================
 * Public API
 * ================================================================ */

AiToolExecutor *
ai_tool_executor_new_empty (void)
{
    return g_object_new (AI_TYPE_TOOL_EXECUTOR, NULL);
}

AiToolExecutor *
ai_tool_executor_new (void)
{
    AiToolExecutor *self = g_object_new (AI_TYPE_TOOL_EXECUTOR, NULL);
    AiTool         *tool;

    /* bash */
    tool = ai_tool_new ("bash",
                        "Run a shell command and return its stdout and stderr "
                        "combined. Nonzero exit codes are reported in the output.");
    ai_tool_add_parameter (tool, "command", "string",
                           "The shell command to execute.", TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* read */
    tool = ai_tool_new ("read",
                        "Read the contents of a file from disk.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Absolute or relative path to the file.", TRUE);
    ai_tool_add_parameter (tool, "offset", "number",
                           "Byte offset to start reading from (default: 0).", FALSE);
    ai_tool_add_parameter (tool, "limit", "number",
                           "Maximum number of bytes to read (default: entire file).", FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* write */
    tool = ai_tool_new ("write",
                        "Write content to a file, creating or overwriting it.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Absolute or relative path to the file.", TRUE);
    ai_tool_add_parameter (tool, "content", "string",
                           "The content to write to the file.", TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* edit */
    tool = ai_tool_new ("edit",
                        "Replace the first occurrence of old_string with "
                        "new_string in a file. The file must exist and "
                        "old_string must be found exactly once.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Absolute or relative path to the file.", TRUE);
    ai_tool_add_parameter (tool, "old_string", "string",
                           "The exact string to find and replace.", TRUE);
    ai_tool_add_parameter (tool, "new_string", "string",
                           "The replacement string.", TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* glob */
    tool = ai_tool_new ("glob",
                        "Find files whose names match a glob pattern, "
                        "searched recursively under a directory.");
    ai_tool_add_parameter (tool, "pattern", "string",
                           "Glob pattern to match filenames (e.g. '*.c', '*.h').",
                           TRUE);
    ai_tool_add_parameter (tool, "path", "string",
                           "Directory to search in (default: current directory).",
                           FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* grep */
    tool = ai_tool_new ("grep",
                        "Search file contents for a regular expression pattern. "
                        "Returns matching lines with file name and line number.");
    ai_tool_add_parameter (tool, "pattern", "string",
                           "Regular expression pattern to search for.", TRUE);
    ai_tool_add_parameter (tool, "path", "string",
                           "File or directory to search (default: current directory).",
                           FALSE);
    ai_tool_add_parameter (tool, "glob", "string",
                           "Glob pattern to filter files when path is a directory "
                           "(e.g. '*.c' to search only C source files).", FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* ls */
    tool = ai_tool_new ("ls",
                        "List the contents of a directory with type and size.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Directory to list (default: current directory).", FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* web_fetch */
    tool = ai_tool_new ("web_fetch",
                        "Fetch a URL over HTTP or HTTPS. HTML is converted to "
                        "readable markdown-style text (scripts/styles stripped, "
                        "headings and links preserved; images become "
                        "![alt](absolute-url) so you can reuse the URLs); JSON "
                        "and plain text are returned as-is. http:// is upgraded "
                        "to https://. "
                        "Responses are cached for 15 minutes. Returns up to "
                        "100 KB. If 'prompt' is given, a model reads the page "
                        "and returns only the requested information.");
    ai_tool_add_parameter (tool, "url", "string",
                           "The URL to fetch (must start with http:// or https://).",
                           TRUE);
    ai_tool_add_parameter (tool, "prompt", "string",
                           "Optional: what to extract from the page. When set, "
                           "a model summarises the fetched content and only the "
                           "result is returned instead of the full page.",
                           FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* web_search is registered on demand by set_search_provider() */

    return self;
}

void
ai_tool_executor_set_search_provider (
    AiToolExecutor   *self,
    AiSearchProvider *provider
){
    GList       *iter;
    gboolean     already_registered = FALSE;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (AI_IS_SEARCH_PROVIDER (provider));

    g_set_object (&self->search_provider, provider);

    /* Register the web_search tool if not already present */
    for (iter = self->tools; iter != NULL; iter = iter->next)
    {
        if (g_strcmp0 (ai_tool_get_name (iter->data), "web_search") == 0)
        {
            already_registered = TRUE;
            break;
        }
    }

    if (!already_registered)
    {
        static const gchar *freshness_values[] =
            { "any", "day", "week", "month", "year", NULL };
        static const gchar *safesearch_values[] =
            { "off", "moderate", "strict", NULL };
        AiTool *tool = ai_tool_new ("web_search",
                                    "Search the web and return ranked results "
                                    "(title, URL, source, snippet). Optionally "
                                    "fetch the top results' page text.");

        ai_tool_add_parameter (tool, "query", "string",
                               "The search query string.", TRUE);
        ai_tool_add_parameter (tool, "count", "number",
                               "Maximum number of results to return "
                               "(default 10).", FALSE);
        ai_tool_add_enum_parameter (tool, "freshness",
                                    "Restrict results by recency.",
                                    (const gchar **) freshness_values, FALSE);
        ai_tool_add_enum_parameter (tool, "safesearch",
                                    "Safe-search filtering level.",
                                    (const gchar **) safesearch_values, FALSE);
        ai_tool_add_parameter (tool, "country", "string",
                               "Two-letter region/country code, e.g. \"US\".",
                               FALSE);
        ai_tool_add_parameter (tool, "language", "string",
                               "Two-letter language code, e.g. \"en\".", FALSE);
        ai_tool_add_parameter (tool, "site", "string",
                               "Restrict results to a single domain, e.g. "
                               "\"example.com\".", FALSE);
        ai_tool_add_parameter (tool, "fetch_content", "boolean",
                               "When true, fetch the top results' pages and "
                               "include their extracted text.", FALSE);
        self->tools = g_list_append (self->tools, tool);
    }
}

GList *
ai_tool_executor_get_tools (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    return self->tools;
}

gchar *
ai_tool_executor_execute (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar     *name;
    const ToolEntry *entry;
    CallbackEntry   *user_entry;

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (tool_use != NULL, NULL);

    name = ai_tool_use_get_name (tool_use);

    /* User-registered callbacks win over built-ins so callers can override
     * a built-in tool by registering their own version with the same name. */
    user_entry = (name != NULL) ? g_hash_table_lookup (self->callbacks, name) : NULL;
    if (user_entry != NULL)
        return user_entry->fn (tool_use, cancellable, error, user_entry->user_data);

    for (entry = BUILTIN_TOOLS; entry->name != NULL; entry++)
    {
        if (g_strcmp0 (entry->name, name) == 0)
            return entry->fn (self, tool_use, cancellable, error);
    }

    g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                 "ai_tool_executor_execute: unknown tool '%s'", name);
    return NULL;
}

void
ai_tool_executor_register_callback (
    AiToolExecutor  *self,
    AiTool          *tool,
    AiToolCallback   callback,
    gpointer         user_data,
    GDestroyNotify   user_data_free
){
    const gchar   *name;
    GList         *iter;
    CallbackEntry *entry;
    gboolean       tool_already_listed = FALSE;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (AI_IS_TOOL (tool));
    g_return_if_fail (callback != NULL);

    name = ai_tool_get_name (tool);
    g_return_if_fail (name != NULL);

    /* Register the callback (replaces any existing entry for this name). */
    entry = g_slice_new0 (CallbackEntry);
    entry->fn = callback;
    entry->user_data = user_data;
    entry->user_data_free = user_data_free;
    g_hash_table_replace (self->callbacks, g_strdup (name), entry);

    /* Ensure the tool definition appears in self->tools so the model sees
     * it in the request. Replace an existing entry with the same name. */
    for (iter = self->tools; iter != NULL; iter = iter->next)
    {
        AiTool *existing = iter->data;
        const gchar *existing_name = ai_tool_get_name (existing);

        if (g_strcmp0 (existing_name, name) == 0)
        {
            g_object_unref (existing);
            iter->data = g_object_ref (tool);
            tool_already_listed = TRUE;
            break;
        }
    }

    if (!tool_already_listed)
    {
        self->tools = g_list_append (self->tools, g_object_ref (tool));
    }
}

void
ai_tool_executor_unregister (
    AiToolExecutor *self,
    const gchar    *tool_name
){
    GList *iter;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (tool_name != NULL);

    g_hash_table_remove (self->callbacks, tool_name);

    for (iter = self->tools; iter != NULL; iter = iter->next)
    {
        AiTool *tool = iter->data;

        if (g_strcmp0 (ai_tool_get_name (tool), tool_name) == 0)
        {
            g_object_unref (tool);
            self->tools = g_list_delete_link (self->tools, iter);
            break;
        }
    }
}

/**
 * ai_tool_executor_set_working_directory:
 * @self: an #AiToolExecutor
 * @path: (nullable): directory the built-in tools work in, or %NULL
 *
 * Set the directory the built-in tools resolve relative paths against
 * and run commands in.
 *
 * An agent is given work to do somewhere -- a project, a checkout, a
 * notes tree -- and refers to "src/main.c", not to an absolute path.
 * Without this, every relative path and every `bash` command resolved
 * against the host process's own working directory, which for an editor
 * is wherever the user last visited a file.
 *
 * %NULL restores that behaviour.  An absolute path from the model is
 * always used as given.
 */
void
ai_tool_executor_set_working_directory (
    AiToolExecutor  *self,
    const gchar     *path
){
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));

    g_free (self->working_directory);
    self->working_directory = g_strdup (path);
}

/**
 * ai_tool_executor_get_working_directory:
 * @self: an #AiToolExecutor
 *
 * Returns: (nullable) (transfer none): the directory set by
 *   ai_tool_executor_set_working_directory(), or %NULL.
 */
const gchar *
ai_tool_executor_get_working_directory (
    AiToolExecutor  *self
){
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    return self->working_directory;
}

/* g_clear_pointer needs a one-argument free; g_list_free_full takes two. */
static void
ai_tool_executor__free_messages (
    GList   *messages
){
    g_list_free_full (messages, g_object_unref);
}

/**
 * ai_tool_executor_run_full:
 * @self: an #AiToolExecutor
 * @provider: the #AiProvider to send requests to
 * @messages: (element-type AiMessage): initial conversation messages
 * @system_prompt: (nullable): optional system prompt
 * @max_tokens: maximum tokens for each response (0 for default 4096)
 * @cancellable: (nullable): a #GCancellable
 * @out_new_messages: (out) (optional) (nullable) (element-type AiMessage)
 *   (transfer full): return location for the messages the run produced --
 *   the assistant turns and tool results appended during the loop, and
 *   NOT the caller's originals. Free with
 *   g_list_free_full(list, g_object_unref).
 * @error: (out) (optional): return location for a #GError
 *
 * Like ai_tool_executor_run(), but also hands back the conversation the
 * run generated.
 *
 * The plain ai_tool_executor_run() drops it. It works on a private copy
 * of @messages, appends each assistant turn and tool result to that
 * copy, and frees the lot on the way out -- so a caller who wants to
 * continue the conversation afterwards has the final text and no record
 * of how the model got there. Sending a follow-up on top of that means
 * the model cannot see its own previous turn, which is not a
 * continuation at all.
 *
 * Pass @out_new_messages to get those messages back and append them to
 * whatever holds the conversation. Passing %NULL is exactly
 * ai_tool_executor_run().
 *
 * Returns: (transfer full) (nullable): the final response text, or %NULL on
 *   error. Free with g_free().
 */
gchar *
ai_tool_executor_run_full (
    AiToolExecutor  *self,
    AiProvider      *provider,
    GList           *messages,
    const gchar     *system_prompt,
    gint             max_tokens,
    GCancellable    *cancellable,
    GList          **out_new_messages,
    GError         **error
){
    RunContext  ctx;
    GList      *iter;
    gchar      *result;
    guint       n_input;
    guint       i;

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (AI_IS_PROVIDER (provider), NULL);
    g_return_val_if_fail (messages != NULL, NULL);

    if (out_new_messages != NULL)
        *out_new_messages = NULL;

    /* Bind the loop to the caller's thread-default context (NULL when
     * none is pushed, i.e. the process-global default -- unchanged from
     * the historical behaviour).  A synchronous caller can push a
     * private GMainContext to isolate this loop (and the provider's
     * thread-default-bound async I/O) from the caller's own default
     * context. */
    ctx.loop          = g_main_loop_new (g_main_context_get_thread_default (), FALSE);
    ctx.task          = NULL;
    ctx.executor      = self;
    ctx.provider      = provider;
    ctx.messages      = NULL;
    /* Borrowed in synchronous mode: the caller outlives the run. */
    ctx.system_prompt = (gchar *) system_prompt;
    ctx.max_tokens    = (max_tokens > 0) ? max_tokens : DEFAULT_MAX_TOKENS;
    ctx.max_turns     = MAX_TURNS;
    ctx.cancellable   = cancellable;
    ctx.turn_count    = 0;
    ctx.result        = NULL;
    ctx.error         = NULL;

    /* Expose the active provider to built-in tools (e.g. web_fetch's optional
     * prompt-based extraction) for the duration of this run only. */
    self->active_provider = provider;

    /* Shallow-copy the caller's messages so we can extend the list */
    n_input = 0;
    for (iter = messages; iter != NULL; iter = iter->next)
    {
        ctx.messages = g_list_append (ctx.messages, g_object_ref (iter->data));
        n_input++;
    }

    run_context_send (&ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);

    self->active_provider = NULL;

    /* Split our list at the caller's originals.  Everything past the
     * first n_input entries was appended during the loop -- the
     * assistant turns and the tool results -- and is what a caller
     * continuing the conversation needs.  Our ref on each is handed over
     * rather than dropped and re-taken. */
    if (out_new_messages != NULL)
    {
        GList *tail = NULL;

        i = 0;
        for (iter = ctx.messages; iter != NULL; iter = iter->next, i++)
        {
            if (i < n_input)
                g_object_unref (iter->data);
            else
                tail = g_list_append (tail, iter->data);
        }
        g_list_free (ctx.messages);
        *out_new_messages = tail;
    }
    else
    {
        /* Free our message list (caller keeps ownership of their originals) */
        g_list_free_full (ctx.messages, g_object_unref);
    }

    if (ctx.error != NULL)
    {
        /* Nothing on the error path: a caller who got NULL should not
         * have to remember to free an out parameter as well, and a
         * half-finished exchange is not something to graft onto a
         * conversation that is about to be abandoned anyway. */
        if (out_new_messages != NULL)
            g_clear_pointer (out_new_messages, ai_tool_executor__free_messages);
        g_propagate_error (error, ctx.error);
        return NULL;
    }

    result = ctx.result;
    return result;
}

gchar *
ai_tool_executor_run (
    AiToolExecutor  *self,
    AiProvider      *provider,
    GList           *messages,
    const gchar     *system_prompt,
    gint             max_tokens,
    GCancellable    *cancellable,
    GError         **error
){
    return ai_tool_executor_run_full (self, provider, messages, system_prompt,
                                      max_tokens, cancellable, NULL, error);
}

/**
 * ai_tool_executor_run_async:
 * @self: an #AiToolExecutor
 * @provider: the #AiProvider to send requests to
 * @messages: (element-type AiMessage): initial conversation messages
 * @system_prompt: (nullable): optional system prompt
 * @max_tokens: maximum tokens per response, or 0 for the default
 * @max_turns: maximum tool-use turns, or 0 for the default of 20
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the loop finishes
 * @user_data: data for @callback
 *
 * Runs the tool-use conversation loop without blocking.
 *
 * This is what lets several agents run at once.  The synchronous
 * ai_tool_executor_run() drives its own nested #GMainLoop, so a caller
 * wanting two conversations in flight had to find two threads; this
 * version simply leaves callbacks pending on the caller's context, and N
 * concurrent runs are N sets of pending callbacks on one thread.
 *
 * @max_turns is a parameter rather than a constant because an
 * orchestrator budgets turns per agent, and a limit the caller cannot
 * set is a limit they have to work around.
 *
 * One executor supports one run at a time: it holds the tool list and
 * the active provider for the duration.  That is why an agent owns its
 * own executor rather than sharing one.
 */
void
ai_tool_executor_run_async (
    AiToolExecutor      *self,
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    gint                 max_turns,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    RunContext *ctx;
    GList      *iter;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (AI_IS_PROVIDER (provider));

    ctx = g_new0 (RunContext, 1);
    ctx->loop          = NULL;
    ctx->task          = g_task_new (self, cancellable, callback, user_data);
    ctx->executor      = g_object_ref (self);
    ctx->provider      = g_object_ref (provider);
    ctx->messages      = NULL;
    /* Owned here: the caller's string may not outlive an async run. */
    ctx->system_prompt = g_strdup (system_prompt);
    ctx->max_tokens    = (max_tokens > 0) ? max_tokens : DEFAULT_MAX_TOKENS;
    ctx->max_turns     = (max_turns  > 0) ? max_turns  : MAX_TURNS;
    ctx->cancellable   = cancellable ? g_object_ref (cancellable) : NULL;

    self->active_provider = provider;

    for (iter = messages; iter != NULL; iter = iter->next)
        ctx->messages = g_list_append (ctx->messages, g_object_ref (iter->data));

    run_context_send (ctx);
}

/**
 * ai_tool_executor_run_finish:
 * @self: an #AiToolExecutor
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the final response text, or %NULL
 *   on error.
 */
gchar *
ai_tool_executor_run_finish (
    AiToolExecutor  *self,
    GAsyncResult    *result,
    GError         **error
){
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    self->active_provider = NULL;
    return g_task_propagate_pointer (G_TASK (result), error);
}
