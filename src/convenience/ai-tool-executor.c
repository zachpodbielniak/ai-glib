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

/* ================================================================
 * Struct definition — must precede any code accessing its fields
 * ================================================================ */

struct _AiToolExecutor
{
    GObject           parent_instance;
    GList            *tools;           /* GList<AiTool>, owned */
    AiSearchProvider *search_provider; /* nullable, ref'd */
};

G_DEFINE_TYPE(AiToolExecutor, ai_tool_executor, G_TYPE_OBJECT)

/* ================================================================
 * Internal run context (async -> sync bridge)
 * ================================================================ */

typedef struct
{
    GMainLoop      *loop;
    AiToolExecutor *executor;
    AiProvider     *provider;
    GList          *messages;      /* owned, grows during loop */
    const gchar    *system_prompt;
    gint            max_tokens;
    GCancellable   *cancellable;
    gint            turn_count;
    gchar          *result;        /* final text (transfer full to caller) */
    GError         *error;         /* propagated to caller */
} RunContext;

/* Forward declaration */
static void run_context_send (RunContext *ctx);

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
        g_main_loop_quit (ctx->loop);
        return;
    }

    ctx->turn_count++;

    if (!ai_response_has_tool_use (response))
    {
        /* Final answer — grab text and quit */
        ctx->result = ai_response_get_text (response);
        g_main_loop_quit (ctx->loop);
        return;
    }

    /* Guard against infinite loops */
    if (ctx->turn_count >= MAX_TURNS)
    {
        g_set_error (&ctx->error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "ai_tool_executor_run: reached maximum turn limit (%d)",
                     MAX_TURNS);
        g_main_loop_quit (ctx->loop);
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

            g_warning ("ToolExecutor turn %d: calling tool '%s' (id=%s)",
                       ctx->turn_count, tool_name, tool_id);

            tool_result = ai_tool_executor_execute (
                ctx->executor, tool_use, ctx->cancellable, NULL);

            if (tool_result == NULL)
            {
                tool_result = g_strdup ("Error: tool execution failed");
                is_error = TRUE;
            }

            result_msg = ai_message_new_tool_result (tool_id, tool_result, is_error);
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

static gchar *
tool_bash (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *command;
    FILE        *fp;
    GString     *output;
    gchar        buf[4096];
    int          exit_status;
    gchar       *result;

    (void)self;
    (void)cancellable;

    command = ai_tool_use_get_input_string (tool_use, "command");
    if (command == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "bash: missing required parameter 'command'");
        return NULL;
    }

    {
        g_autofree gchar *cmd_merged = g_strdup_printf ("%s 2>&1", command);
        fp = popen (cmd_merged, "r");
    }

    if (fp == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_TOOL_ERROR,
                             "bash: popen() failed");
        return NULL;
    }

    output = g_string_new (NULL);
    while (fgets (buf, (int)sizeof (buf), fp) != NULL)
        g_string_append (output, buf);

    exit_status = pclose (fp);

    result = g_string_free (output, FALSE);

    /* Prefix with exit code on failure */
    if (exit_status != 0 && exit_status != -1 && WIFEXITED (exit_status)
        && WEXITSTATUS (exit_status) != 0)
    {
        gchar *prefixed = g_strdup_printf ("[exit code %d]\n%s",
                                           WEXITSTATUS (exit_status), result);
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

    (void)self;
    (void)cancellable;

    path = ai_tool_use_get_input_string (tool_use, "path");
    if (path == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "read: missing required parameter 'path'");
        return NULL;
    }

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

    (void)self;
    (void)cancellable;

    path    = ai_tool_use_get_input_string (tool_use, "path");
    content = ai_tool_use_get_input_string (tool_use, "content");

    if (path == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "write: missing required parameter 'path'");
        return NULL;
    }

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

    (void)self;
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

    (void)self;
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
    if (path == NULL)
        path = ".";

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

    (void)self;
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

    if (path == NULL)
        path = ".";

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

    (void)self;
    (void)cancellable;

    path = ai_tool_use_get_input_string (tool_use, "path");
    if (path == NULL)
        path = ".";

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

static gchar *
tool_web_fetch (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar           *url;
    g_autoptr(SoupSession)  session = NULL;
    g_autoptr(SoupMessage)  msg     = NULL;
    g_autoptr(GBytes)       bytes   = NULL;
    guint        status_code;
    const gchar *data;
    gsize        size;

    (void)self;

    url = ai_tool_use_get_input_string (tool_use, "url");
    if (url == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "web_fetch: missing required parameter 'url'");
        return NULL;
    }

    session = soup_session_new ();
    msg     = soup_message_new ("GET", url);

    if (msg == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "web_fetch: invalid URL '%s'", url);
        return NULL;
    }

    bytes = soup_session_send_and_read (session, msg, cancellable, error);
    if (bytes == NULL)
        return NULL;

    status_code = soup_message_get_status (msg);
    if (status_code < 200 || status_code >= 300)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                     "web_fetch: HTTP %u for '%s'", status_code, url);
        return NULL;
    }

    data = g_bytes_get_data (bytes, &size);

    if (size > WEB_FETCH_MAX_BYTES)
        size = WEB_FETCH_MAX_BYTES;

    return g_strndup (data, size);
}

static gchar *
tool_web_search (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *query;

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

    return ai_search_provider_search (self->search_provider, query,
                                      cancellable, error);
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
}

/* ================================================================
 * Public API
 * ================================================================ */

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
                        "Fetch the raw contents of a URL over HTTP or HTTPS. "
                        "Returns up to 100 KB of the response body.");
    ai_tool_add_parameter (tool, "url", "string",
                           "The URL to fetch (must start with http:// or https://).",
                           TRUE);
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
        AiTool *tool = ai_tool_new ("web_search",
                                    "Search the web and return the top results "
                                    "with title, URL, and description.");
        ai_tool_add_parameter (tool, "query", "string",
                               "The search query string.", TRUE);
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

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (tool_use != NULL, NULL);

    name = ai_tool_use_get_name (tool_use);

    for (entry = BUILTIN_TOOLS; entry->name != NULL; entry++)
    {
        if (g_strcmp0 (entry->name, name) == 0)
            return entry->fn (self, tool_use, cancellable, error);
    }

    g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                 "ai_tool_executor_execute: unknown tool '%s'", name);
    return NULL;
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
    RunContext  ctx;
    GList      *iter;
    gchar      *result;

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (AI_IS_PROVIDER (provider), NULL);
    g_return_val_if_fail (messages != NULL, NULL);

    ctx.loop          = g_main_loop_new (NULL, FALSE);
    ctx.executor      = self;
    ctx.provider      = provider;
    ctx.messages      = NULL;
    ctx.system_prompt = system_prompt;
    ctx.max_tokens    = (max_tokens > 0) ? max_tokens : DEFAULT_MAX_TOKENS;
    ctx.cancellable   = cancellable;
    ctx.turn_count    = 0;
    ctx.result        = NULL;
    ctx.error         = NULL;

    /* Shallow-copy the caller's messages so we can extend the list */
    for (iter = messages; iter != NULL; iter = iter->next)
        ctx.messages = g_list_append (ctx.messages, g_object_ref (iter->data));

    run_context_send (&ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);

    /* Free our message list (caller keeps ownership of their originals) */
    g_list_free_full (ctx.messages, g_object_unref);

    if (ctx.error != NULL)
    {
        g_propagate_error (error, ctx.error);
        return NULL;
    }

    result = ctx.result;
    return result;
}
