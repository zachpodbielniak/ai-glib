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
#include "agent/ai-mock-provider.h"

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

/* web_fetch must advertise both 'url' (required) and 'prompt' (optional). */
static void
test_executor_web_fetch_params (void)
{
    g_autoptr(AiToolExecutor) exec = NULL;
    GList            *tools;
    GList            *iter;
    AiTool             *web_fetch = NULL;
    g_autoptr(JsonNode) params = NULL;
    g_autofree gchar   *json = NULL;

    exec  = ai_tool_executor_new ();
    tools = ai_tool_executor_get_tools (exec);

    for (iter = tools; iter != NULL; iter = iter->next)
    {
        if (g_strcmp0 (ai_tool_get_name (iter->data), "web_fetch") == 0)
        {
            web_fetch = iter->data;
            break;
        }
    }
    g_assert_nonnull (web_fetch);

    params = ai_tool_get_parameters_json (web_fetch);
    g_assert_nonnull (params);
    json = json_to_string (params, FALSE);

    g_assert_nonnull (json);
    g_assert_true (g_strstr_len (json, -1, "\"url\"") != NULL);
    g_assert_true (g_strstr_len (json, -1, "\"prompt\"") != NULL);
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
 * register_callback
 * ================================================================ */

typedef struct
{
    gint   call_count;
    gchar *last_query;
} CallbackState;

static gchar *
my_lookup_tool (
    AiToolUse    *tool_use,
    GCancellable *cancellable,
    GError      **error,
    gpointer      user_data
){
    CallbackState *state = user_data;
    const gchar   *query;

    (void)cancellable;
    (void)error;

    state->call_count++;

    query = ai_tool_use_get_input_string (tool_use, "query");
    g_clear_pointer (&state->last_query, g_free);
    state->last_query = g_strdup (query);

    return g_strdup_printf ("looked up: %s", query != NULL ? query : "(none)");
}

static void
test_executor_register_callback (void)
{
    g_autoptr(AiToolExecutor) exec      = NULL;
    g_autoptr(AiTool)         tool      = NULL;
    g_autoptr(AiToolUse)      tool_use  = NULL;
    g_autofree gchar         *result    = NULL;
    g_autoptr(GError)         err       = NULL;
    CallbackState             state     = { 0, NULL };
    GList                    *tools;
    GList                    *iter;
    gboolean                  found_my_tool = FALSE;

    exec = ai_tool_executor_new ();
    tool = ai_tool_new ("my_lookup", "Look something up");
    ai_tool_add_parameter (tool, "query", "string", "what to look up", TRUE);

    ai_tool_executor_register_callback (exec, tool, my_lookup_tool, &state, NULL);

    /* Tool should be visible to the model via get_tools() */
    tools = ai_tool_executor_get_tools (exec);
    for (iter = tools; iter != NULL; iter = iter->next)
    {
        if (g_strcmp0 (ai_tool_get_name (iter->data), "my_lookup") == 0)
        {
            found_my_tool = TRUE;
            break;
        }
    }
    g_assert_true (found_my_tool);

    /* Execute dispatches to the callback */
    tool_use = make_tool_use ("my_lookup", "{\"query\": \"weather\"}");
    result   = ai_tool_executor_execute (exec, tool_use, NULL, &err);

    g_assert_no_error (err);
    g_assert_cmpstr (result, ==, "looked up: weather");
    g_assert_cmpint (state.call_count, ==, 1);
    g_assert_cmpstr (state.last_query, ==, "weather");

    /* Unregister removes it from both the dispatch and the tool list */
    ai_tool_executor_unregister (exec, "my_lookup");

    tools = ai_tool_executor_get_tools (exec);
    found_my_tool = FALSE;
    for (iter = tools; iter != NULL; iter = iter->next)
    {
        if (g_strcmp0 (ai_tool_get_name (iter->data), "my_lookup") == 0)
        {
            found_my_tool = TRUE;
            break;
        }
    }
    g_assert_false (found_my_tool);

    g_clear_pointer (&state.last_query, g_free);
}

/* ================================================================
 * main
 * ================================================================ */

/* ================================================================
 * run_full: handing back the conversation the run produced
 * ================================================================ */

/* The plain run() drops every message it generated, so a caller who
 * wants to carry on has the final text and no record of how the model
 * got there -- and a follow-up sent on top of that shows the model none
 * of its own previous turn.  run_full() is what makes continuing
 * possible, so what matters is that it returns exactly the new messages
 * and none of the caller's. */
static void
test_executor_run_full_returns_new_messages (void)
{
    g_autoptr (AiToolExecutor) exec = ai_tool_executor_new ();
    g_autoptr (AiMockProvider) mock = ai_mock_provider_new ();
    g_autoptr (AiMessage) user = ai_message_new_user ("hello");
    g_autoptr (GError) error = NULL;
    g_autofree gchar *reply = NULL;
    GList *messages = NULL;
    GList *produced = NULL;

    ai_mock_provider_push_text (mock, "hi there");
    messages = g_list_append (NULL, user);

    reply = ai_tool_executor_run_full (exec, AI_PROVIDER (mock), messages,
                                       NULL, 0, NULL, &produced, &error);

    g_assert_no_error (error);
    g_assert_cmpstr (reply, ==, "hi there");

    /* The assistant turn, and only it: the caller's own message must not
     * come back, or appending the result to a session would duplicate
     * every message on every turn. */
    g_assert_nonnull (produced);
    g_assert_cmpuint (g_list_length (produced), ==, 1);
    g_assert_true (produced->data != (gpointer) user);

    g_list_free_full (produced, g_object_unref);
    g_list_free (messages);
}

/* A tool call means the run generates an assistant turn AND a tool
 * result; both have to come back or the replayed history is invalid --
 * a tool_use with no matching tool_result is rejected by every provider
 * that checks. */
static void
test_executor_run_full_includes_tool_results (void)
{
    g_autoptr (AiToolExecutor) exec = ai_tool_executor_new ();
    g_autoptr (AiMockProvider) mock = ai_mock_provider_new ();
    g_autoptr (AiMessage) user = ai_message_new_user ("run something");
    g_autoptr (GError) error = NULL;
    g_autofree gchar *reply = NULL;
    GList *messages = NULL;
    GList *produced = NULL;

    ai_mock_provider_push_tool_use (mock, "bash",
                                    "{\"command\": \"echo hi\"}");
    ai_mock_provider_push_text (mock, "it said hi");
    messages = g_list_append (NULL, user);

    reply = ai_tool_executor_run_full (exec, AI_PROVIDER (mock), messages,
                                       NULL, 0, NULL, &produced, &error);

    g_assert_no_error (error);
    g_assert_cmpstr (reply, ==, "it said hi");
    /* assistant(tool_use) + tool_result + assistant(text) */
    g_assert_cmpuint (g_list_length (produced), >=, 3);

    g_list_free_full (produced, g_object_unref);
    g_list_free (messages);
}

/* Passing NULL must behave exactly like the old entry point, since that
 * is now literally what run() is. */
static void
test_executor_run_full_null_out_is_plain_run (void)
{
    g_autoptr (AiToolExecutor) exec = ai_tool_executor_new ();
    g_autoptr (AiMockProvider) mock = ai_mock_provider_new ();
    g_autoptr (AiMessage) user = ai_message_new_user ("hello");
    g_autoptr (GError) error = NULL;
    g_autofree gchar *reply = NULL;
    GList *messages = NULL;

    ai_mock_provider_push_text (mock, "same answer");
    messages = g_list_append (NULL, user);

    reply = ai_tool_executor_run_full (exec, AI_PROVIDER (mock), messages,
                                       NULL, 0, NULL, NULL, &error);

    g_assert_no_error (error);
    g_assert_cmpstr (reply, ==, "same answer");
    g_list_free (messages);
}

/* On failure the caller gets NULL and no out parameter to remember to
 * free -- a half-finished exchange is not something to graft onto a
 * conversation that is about to be abandoned. */
static void
test_executor_run_full_error_clears_out (void)
{
    g_autoptr (AiToolExecutor) exec = ai_tool_executor_new ();
    g_autoptr (AiMockProvider) mock = ai_mock_provider_new ();
    g_autoptr (AiMessage) user = ai_message_new_user ("hello");
    g_autoptr (GError) error = NULL;
    gchar *reply = NULL;
    GList *messages = NULL;
    GList *produced = (GList *) 0x1; /* must be overwritten, not read */

    ai_mock_provider_push_error (mock, "the sky fell in");
    messages = g_list_append (NULL, user);

    reply = ai_tool_executor_run_full (exec, AI_PROVIDER (mock), messages,
                                       NULL, 0, NULL, &produced, &error);

    g_assert_null (reply);
    g_assert_nonnull (error);
    g_assert_null (produced);
    g_list_free (messages);
}

/*
 * ai_tool_executor_new_empty() must hand the model nothing at all.
 *
 * The difference matters more than it looks: the default constructor
 * includes `bash`, `read`, `write` and `edit`. An application that wants
 * the model confined to its own tools starts empty, so this test pins the
 * guarantee it depends on -- and the companion below pins the other half,
 * that an unadvertised built-in genuinely does not run.
 */
static void
test_new_empty_has_no_tools (void)
{
    g_autoptr(AiToolExecutor) empty = ai_tool_executor_new_empty ();
    g_autoptr(AiToolExecutor) full = ai_tool_executor_new ();
    GList *l;

    g_assert_null (ai_tool_executor_get_tools (empty));

    /* And the default really does carry the dangerous ones, which is why
     * the empty constructor exists. */
    g_assert_nonnull (ai_tool_executor_get_tools (full));

    for (l = ai_tool_executor_get_tools (full); l != NULL; l = l->next)
    {
        if (g_strcmp0 (ai_tool_get_name (AI_TOOL (l->data)), "bash") == 0)
        {
            break;
        }
    }

    g_assert_nonnull (l);
}

/*
 * The tool list is the grant.
 *
 * A built-in dispatches only while the executor advertises it. Without
 * that, `task`'s allowlist would be decoration: an agent whose tools:
 * omits bash would still be able to call bash, because the dispatch table
 * is global.
 */
static void
test_unadvertised_builtin_does_not_run (void)
{
    g_autoptr(AiToolExecutor) empty = ai_tool_executor_new_empty ();
    g_autoptr(AiToolUse)      use = NULL;
    g_autoptr(JsonBuilder)    builder = json_builder_new ();
    g_autoptr(JsonNode)       input = NULL;
    g_autofree gchar         *result = NULL;
    GError                   *error = NULL;

    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "command");
    json_builder_add_string_value (builder, "echo SHOULD_NOT_RUN");
    json_builder_end_object (builder);
    input = json_builder_get_root (builder);

    use = ai_tool_use_new ("id-1", "bash", input);

    result = ai_tool_executor_execute (empty, use, NULL, &error);

    g_assert_null (result);
    g_assert_nonnull (error);
    g_clear_error (&error);
}

/* Does the executor advertise a tool by this name? */
static gboolean
tool_list_contains (AiToolExecutor *executor, const gchar *name)
{
    GList *l;

    for (l = ai_tool_executor_get_tools (executor); l != NULL; l = l->next)
    {
        if (g_strcmp0 (ai_tool_get_name (AI_TOOL (l->data)), name) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * The compatibility guarantee, stated as an assertion.
 *
 * An executor with no resource registry offers exactly the tools it
 * always has; a registry is what adds `task` and `skill`.
 */
static void
test_registry_adds_exactly_two_tools (void)
{
    g_autoptr(AiToolExecutor)     executor = ai_tool_executor_new ();
    g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new ();
    guint                         before;
    guint                         after;

    before = g_list_length (ai_tool_executor_get_tools (executor));

    g_assert_null (ai_tool_executor_get_resource_registry (executor));
    g_assert_false (tool_list_contains (executor, "task"));

    ai_tool_executor_set_resource_registry (executor, registry);
    after = g_list_length (ai_tool_executor_get_tools (executor));

    g_assert_cmpuint (after, ==, before + 2);
    g_assert_true (tool_list_contains (executor, "task"));
    g_assert_true (tool_list_contains (executor, "skill"));
    g_assert_true (ai_tool_executor_get_resource_registry (executor) ==
                   registry);

    /* And clearing it takes them away again. */
    ai_tool_executor_set_resource_registry (executor, NULL);
    g_assert_cmpuint (g_list_length (ai_tool_executor_get_tools (executor)),
                      ==, before);
}

static gchar *
stub_callback (AiToolUse    *tool_use,
               GCancellable *cancellable,
               GError      **error,
               gpointer      user_data)
{
    return g_strdup ("{}");
}

/*
 * Registering onto an empty executor yields exactly one tool: the host's.
 */
static void
test_new_empty_registers_only_host_tools (void)
{
    g_autoptr(AiToolExecutor) executor = ai_tool_executor_new_empty ();
    g_autoptr(AiTool) tool = ai_tool_new ("app_query", "The app's own tool");
    GList *tools;

    ai_tool_executor_register_callback (executor, tool, stub_callback, NULL,
                                        NULL);

    tools = ai_tool_executor_get_tools (executor);
    g_assert_cmpint ((gint) g_list_length (tools), ==, 1);
    g_assert_cmpstr (ai_tool_get_name (AI_TOOL (tools->data)), ==, "app_query");
}

int
main (
    int   argc,
    char *argv[]
){
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/ai-glib/tool-executor/new",
                     test_executor_new);
    g_test_add_func ("/ai-glib/tool-executor/web-fetch-params",
                     test_executor_web_fetch_params);
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
    g_test_add_func ("/ai-glib/tool-executor/register-callback",
                     test_executor_register_callback);
    g_test_add_func ("/ai-glib/tool-executor/new-empty/has-no-tools",
                     test_new_empty_has_no_tools);
    g_test_add_func ("/ai-glib/tool-executor/new-empty/only-host-tools",
                     test_new_empty_registers_only_host_tools);
    g_test_add_func ("/ai-glib/tool-executor/unadvertised-builtin",
                     test_unadvertised_builtin_does_not_run);
    g_test_add_func ("/ai-glib/tool-executor/registry-adds-tools",
                     test_registry_adds_exactly_two_tools);
    g_test_add_func ("/ai-glib/tool-executor/run-full/new-messages",
                     test_executor_run_full_returns_new_messages);
    g_test_add_func ("/ai-glib/tool-executor/run-full/tool-results",
                     test_executor_run_full_includes_tool_results);
    g_test_add_func ("/ai-glib/tool-executor/run-full/null-out",
                     test_executor_run_full_null_out_is_plain_run);
    g_test_add_func ("/ai-glib/tool-executor/run-full/error-clears-out",
                     test_executor_run_full_error_clears_out);

    return g_test_run ();
}
