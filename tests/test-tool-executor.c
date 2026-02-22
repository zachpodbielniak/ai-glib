/*
 * test-tool-executor.c - Unit tests for AiToolExecutor
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "ai-glib.h"
#include "convenience/ai-tool-executor.h"
#include "convenience/ai-search-provider.h"
#include "model/ai-tool-use.h"

/* ================================================================
 * Helpers
 * ================================================================ */

static AiToolUse *
make_tool_use (const gchar *name, const gchar *input_json)
{
    return ai_tool_use_new_from_json_string ("test-id-1", name, input_json);
}

/* ================================================================
 * Construction tests
 * ================================================================ */

static void
test_executor_new (void)
{
    g_autoptr(AiToolExecutor) exec = NULL;
    GList   *tools;
    GList   *iter;
    gboolean has_bash       = FALSE;
    gboolean has_read       = FALSE;
    gboolean has_write      = FALSE;
    gboolean has_edit       = FALSE;
    gboolean has_glob       = FALSE;
    gboolean has_grep       = FALSE;
    gboolean has_ls         = FALSE;
    gboolean has_web_fetch  = FALSE;
    gboolean has_web_search = FALSE;

    exec  = ai_tool_executor_new ();
    g_assert_nonnull (exec);
    g_assert_true (AI_IS_TOOL_EXECUTOR (exec));

    tools = ai_tool_executor_get_tools (exec);
    g_assert_nonnull (tools);

    for (iter = tools; iter != NULL; iter = iter->next)
    {
        const gchar *n = ai_tool_get_name (iter->data);

        if (g_strcmp0 (n, "bash")       == 0) has_bash       = TRUE;
        if (g_strcmp0 (n, "read")       == 0) has_read       = TRUE;
        if (g_strcmp0 (n, "write")      == 0) has_write      = TRUE;
        if (g_strcmp0 (n, "edit")       == 0) has_edit       = TRUE;
        if (g_strcmp0 (n, "glob")       == 0) has_glob       = TRUE;
        if (g_strcmp0 (n, "grep")       == 0) has_grep       = TRUE;
        if (g_strcmp0 (n, "ls")         == 0) has_ls         = TRUE;
        if (g_strcmp0 (n, "web_fetch")  == 0) has_web_fetch  = TRUE;
        if (g_strcmp0 (n, "web_search") == 0) has_web_search = TRUE;
    }

    g_assert_true  (has_bash);
    g_assert_true  (has_read);
    g_assert_true  (has_write);
    g_assert_true  (has_edit);
    g_assert_true  (has_glob);
    g_assert_true  (has_grep);
    g_assert_true  (has_ls);
    g_assert_true  (has_web_fetch);
    /* web_search must NOT be present without a provider */
    g_assert_false (has_web_search);
}

/* ================================================================
 * bash
 * ================================================================ */

static void
test_executor_bash_echo (void)
{
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;

    exec     = ai_tool_executor_new ();
    tool_use = make_tool_use ("bash", "{\"command\": \"echo hello\"}");
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_true (g_strstr_len (result, -1, "hello") != NULL);
}

static void
test_executor_bash_exit_code (void)
{
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;

    exec     = ai_tool_executor_new ();
    tool_use = make_tool_use ("bash", "{\"command\": \"exit 42\"}");
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    /* Should succeed (return a string) but prefix with the exit code */
    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_true (g_strstr_len (result, -1, "42") != NULL);
}

/* ================================================================
 * read / write
 * ================================================================ */

static void
test_executor_read_write (void)
{
    g_autoptr(AiToolExecutor) exec      = NULL;
    g_autofree gchar         *tmp_path  = NULL;
    g_autofree gchar         *write_json = NULL;
    g_autofree gchar         *read_json  = NULL;
    g_autoptr(AiToolUse)      write_use  = NULL;
    g_autoptr(AiToolUse)      read_use   = NULL;
    g_autofree gchar         *wr         = NULL;
    g_autofree gchar         *rd         = NULL;
    g_autoptr(GError)         err        = NULL;
    gint                      fd;

    exec = ai_tool_executor_new ();

    fd       = g_file_open_tmp ("ai-glib-test-XXXXXX", &tmp_path, &err);
    g_assert_no_error (err);
    close (fd);

    write_json = g_strdup_printf (
        "{\"path\": \"%s\", \"content\": \"hello world\"}",
        tmp_path);
    write_use = make_tool_use ("write", write_json);
    wr        = ai_tool_executor_execute (exec, write_use, NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (wr, ==, "OK");

    read_json = g_strdup_printf ("{\"path\": \"%s\"}", tmp_path);
    read_use  = make_tool_use ("read", read_json);
    rd        = ai_tool_executor_execute (exec, read_use, NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (rd, ==, "hello world");

    g_unlink (tmp_path);
}

/* ================================================================
 * edit
 * ================================================================ */

static void
test_executor_edit (void)
{
    g_autoptr(AiToolExecutor) exec      = NULL;
    g_autofree gchar         *tmp_path  = NULL;
    g_autofree gchar         *edit_json = NULL;
    g_autofree gchar         *read_json = NULL;
    g_autoptr(AiToolUse)      edit_use  = NULL;
    g_autoptr(AiToolUse)      read_use  = NULL;
    g_autofree gchar         *er        = NULL;
    g_autofree gchar         *rd        = NULL;
    g_autoptr(GError)         err       = NULL;
    gint                      fd;

    exec = ai_tool_executor_new ();

    fd      = g_file_open_tmp ("ai-glib-test-XXXXXX", &tmp_path, &err);
    g_assert_no_error (err);
    close (fd);
    g_file_set_contents (tmp_path, "foo bar baz", -1, NULL);

    edit_json = g_strdup_printf (
        "{\"path\": \"%s\", \"old_string\": \"bar\", \"new_string\": \"qux\"}",
        tmp_path);
    edit_use = make_tool_use ("edit", edit_json);
    er       = ai_tool_executor_execute (exec, edit_use, NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (er, ==, "OK");

    read_json = g_strdup_printf ("{\"path\": \"%s\"}", tmp_path);
    read_use  = make_tool_use ("read", read_json);
    rd        = ai_tool_executor_execute (exec, read_use, NULL, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (rd, ==, "foo qux baz");

    g_unlink (tmp_path);
}

/* ================================================================
 * glob
 * ================================================================ */

static void
test_executor_glob (void)
{
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autofree gchar         *tmp_path = NULL;
    g_autofree gchar         *tmp_dir  = NULL;
    g_autofree gchar         *json     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;
    gint                      fd;

    exec = ai_tool_executor_new ();

    /* Create a known temp file ending in .tmp so glob can find it */
    fd = g_file_open_tmp ("ai-glib-glob-test-XXXXXX.tmp", &tmp_path, &err);
    g_assert_no_error (err);
    close (fd);

    tmp_dir = g_path_get_dirname (tmp_path);

    json = g_strdup_printf (
        "{\"pattern\": \"ai-glib-glob-test-*.tmp\", \"path\": \"%s\"}",
        tmp_dir);
    tool_use = make_tool_use ("glob", json);
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_true (g_strstr_len (result, -1, ".tmp") != NULL);

    g_unlink (tmp_path);
}

/* ================================================================
 * grep
 * ================================================================ */

static void
test_executor_grep (void)
{
    g_autoptr(AiToolExecutor) exec      = NULL;
    g_autofree gchar         *tmp_path  = NULL;
    g_autofree gchar         *grep_json = NULL;
    g_autoptr(AiToolUse)      grep_use  = NULL;
    g_autofree gchar         *result    = NULL;
    g_autoptr(GError)         err       = NULL;
    gint                      fd;

    exec = ai_tool_executor_new ();

    fd = g_file_open_tmp ("ai-glib-test-XXXXXX", &tmp_path, &err);
    g_assert_no_error (err);
    close (fd);
    g_file_set_contents (tmp_path,
                         "line one\nFOUND_MARKER here\nline three\n",
                         -1, NULL);

    grep_json = g_strdup_printf (
        "{\"pattern\": \"FOUND_MARKER\", \"path\": \"%s\"}", tmp_path);
    grep_use = make_tool_use ("grep", grep_json);
    result   = ai_tool_executor_execute (exec, grep_use, NULL, &err);

    g_assert_no_error (err);
    g_assert_nonnull (result);
    g_assert_true (g_strstr_len (result, -1, "FOUND_MARKER") != NULL);

    g_unlink (tmp_path);
}

/* ================================================================
 * ls
 * ================================================================ */

static void
test_executor_ls (void)
{
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;

    exec     = ai_tool_executor_new ();
    tool_use = make_tool_use ("ls", "{\"path\": \"/tmp\"}");
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    g_assert_no_error (err);
    g_assert_nonnull (result);
    /* /tmp should have at least one entry */
    g_assert_true (strlen (result) > 0);
}

/* ================================================================
 * web_search without provider
 * ================================================================ */

static void
test_executor_web_search_no_provider (void)
{
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;

    exec     = ai_tool_executor_new ();
    tool_use = make_tool_use ("web_search", "{\"query\": \"test\"}");
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    /* Should return an error since no provider is configured */
    g_assert_null (result);
    g_assert_nonnull (err);
}

/* ================================================================
 * Unknown tool
 * ================================================================ */

static void
test_executor_unknown_tool (void)
{
    g_autoptr(AiToolExecutor) exec     = NULL;
    g_autoptr(AiToolUse)      tool_use = NULL;
    g_autofree gchar         *result   = NULL;
    g_autoptr(GError)         err      = NULL;

    exec     = ai_tool_executor_new ();
    tool_use = make_tool_use ("nonexistent_tool", "{}");
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    g_assert_null (result);
    g_assert_nonnull (err);
}

/* ================================================================
 * main
 * ================================================================ */

int
main (
    int   argc,
    char *argv[]
){
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/ai-glib/tool-executor/new",
                     test_executor_new);
    g_test_add_func ("/ai-glib/tool-executor/bash/echo",
                     test_executor_bash_echo);
    g_test_add_func ("/ai-glib/tool-executor/bash/exit-code",
                     test_executor_bash_exit_code);
    g_test_add_func ("/ai-glib/tool-executor/read-write",
                     test_executor_read_write);
    g_test_add_func ("/ai-glib/tool-executor/edit",
                     test_executor_edit);
    g_test_add_func ("/ai-glib/tool-executor/glob",
                     test_executor_glob);
    g_test_add_func ("/ai-glib/tool-executor/grep",
                     test_executor_grep);
    g_test_add_func ("/ai-glib/tool-executor/ls",
                     test_executor_ls);
    g_test_add_func ("/ai-glib/tool-executor/web-search-no-provider",
                     test_executor_web_search_no_provider);
    g_test_add_func ("/ai-glib/tool-executor/unknown-tool",
                     test_executor_unknown_tool);

    return g_test_run ();
}
