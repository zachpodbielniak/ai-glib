/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include "providers/ai-claude-sandbox-private.h"
#include "core/ai-cli-client-private.h"

static const gchar *ok_json =
    "{\"type\":\"thread.started\",\"thread_id\":\"thread-test\"}\n"
    "{\"type\":\"turn.started\"}\n"
    "{\"type\":\"item.completed\",\"item\":{\"id\":\"r\",\"type\":\"reasoning\",\"text\":\"Thinking\"}}\n"
    "{\"type\":\"item.started\",\"item\":{\"id\":\"c\",\"type\":\"command_execution\",\"command\":\"pwd\",\"status\":\"in_progress\"}}\n"
    "{\"type\":\"item.completed\",\"item\":{\"id\":\"c\",\"type\":\"command_execution\",\"command\":\"pwd\",\"aggregated_output\":\"/tmp\",\"exit_code\":0,\"status\":\"completed\"}}\n"
    "{\"type\":\"item.updated\",\"item\":{\"id\":\"m\",\"type\":\"agent_message\",\"text\":\"Hel\"}}\n"
    "{\"type\":\"item.completed\",\"item\":{\"id\":\"m\",\"type\":\"agent_message\",\"text\":\"Hello\"}}\n"
    "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":20,\"cached_input_tokens\":10,\"output_tokens\":3}}\n";

static const gchar *flag_value(gchar **argv, const gchar *flag)
{
    guint i;
    for (i = 0; argv[i] != NULL; i++) if (g_str_equal(argv[i], flag)) return argv[i+1];
    return NULL;
}

static void test_argv(void)
{
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    AiCliClient *cli = AI_CLI_CLIENT(c);
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(cli);
    g_auto(GStrv) argv = klass->build_argv(cli, NULL, NULL, 0, FALSE);
    g_assert_cmpstr(flag_value(argv, "--sandbox"), ==, "read-only");
    g_assert_cmpstr(flag_value(argv, "--model"), ==, "gpt-6-astra");
    g_assert_cmpstr(flag_value(argv, "--ask-for-approval"), ==, "never");
    g_assert_true(g_strv_contains((const gchar *const *)argv, "--json"));
    g_assert_cmpstr(argv[g_strv_length(argv)-1], ==, "-");
    g_clear_pointer(&argv, g_strfreev);
    g_object_set(c, "sandbox", "workspace-write", "additional-directories", "/tmp/a, /tmp/b", NULL);
    ai_cli_client_set_session_id(cli, "thread-test");
    ai_cli_client_set_effort_level(cli, "xhigh");
    argv = klass->build_argv(cli, NULL, NULL, 0, TRUE);
    g_assert_cmpstr(flag_value(argv, "resume"), ==, "thread-test");
    g_assert_cmpstr(flag_value(argv, "--sandbox"), ==, "workspace-write");
    g_assert_cmpstr(flag_value(argv, "-c"), ==, "model_reasoning_effort=\"xhigh\"");
    g_assert_true(g_strv_contains((const gchar *const *)argv, "/tmp/b"));
    g_clear_pointer(&argv, g_strfreev);
    ai_cli_client_set_session_persistence(cli, FALSE);
    argv = klass->build_argv(cli, NULL, NULL, 0, FALSE);
    g_assert_true(g_strv_contains((const gchar *const *)argv, "--ephemeral"));
    g_assert_false(g_strv_contains((const gchar *const *)argv, "resume"));
    g_clear_pointer(&argv, g_strfreev);
    ai_codex_cli_client_set_skip_permissions(c, TRUE);
    g_assert_null(klass->build_argv(cli, NULL, NULL, 0, FALSE));
    ai_codex_cli_client_set_sandbox(c, NULL);
    argv = klass->build_argv(cli, NULL, NULL, 0, FALSE);
    g_assert_true(g_strv_contains((const gchar *const *)argv, "--dangerously-bypass-approvals-and-sandbox"));
    g_assert_null(flag_value(argv, "--sandbox"));
    g_clear_pointer(&argv, g_strfreev);
    ai_codex_cli_client_set_sandbox(c, "typo");
    g_assert_null(klass->build_argv(cli, NULL, NULL, 0, FALSE));
}

static void test_parse(void)
{
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    AiCliClient *cli = AI_CLI_CLIENT(c);
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(cli);
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = klass->parse_json_output(cli, ok_json, &error);
    g_autofree gchar *text = ai_response_get_text(response);
    g_assert_no_error(error);
    g_assert_cmpstr(text, ==, "Hello");
    g_assert_cmpstr(ai_cli_client_get_session_id(cli), ==, "thread-test");
    g_assert_cmpint(ai_usage_get_input_tokens(ai_response_get_usage(response)), ==, 20);
    g_assert_cmpint(ai_usage_get_output_tokens(ai_response_get_usage(response)), ==, 3);
    /* gpt-6-astra is $10/M input, $50/M output: 20*10 + 3*50 micro-dollars. */
    g_assert_cmpint(ai_response_get_cost_micros(response), ==, 350);
}

static void test_astra_usage_event_cost(void)
{
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    g_autoptr(AiResponse) response = ai_response_new("", AI_CODEX_CLI_MODEL_GPT_6_ASTRA);
    g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
    g_autoptr(GError) error = NULL;
    AiEvent *event;

    g_assert_true(AI_CLI_CLIENT_GET_CLASS(c)->parse_stream_events(
        AI_CLI_CLIENT(c),
        "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":1000000,\"output_tokens\":1000000}}",
        response, events, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(events->len, ==, 1);
    event = g_ptr_array_index(events, 0);
    g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_USAGE);
    g_assert_cmpint(ai_event_get_cost_micros(event), ==, 60000000);
    g_assert_cmpint(ai_response_get_cost_micros(response), ==, 60000000);
}

static void test_unpriced_model_omits_cost(void)
{
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
    AiEvent *event;

    ai_cli_client_set_model(AI_CLI_CLIENT(c), AI_CODEX_CLI_MODEL_GPT_5_5);
    response = AI_CLI_CLIENT_GET_CLASS(c)->parse_json_output(AI_CLI_CLIENT(c), ok_json, &error);
    g_assert_no_error(error);
    g_assert_cmpint(ai_response_get_cost_micros(response), ==, -1);

    g_assert_true(AI_CLI_CLIENT_GET_CLASS(c)->parse_stream_events(
        AI_CLI_CLIENT(c),
        "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":20,\"output_tokens\":3}}",
        response, events, &error));
    g_assert_no_error(error);
    event = g_ptr_array_index(events, 0);
    g_assert_cmpint(ai_event_get_kind(event), ==, AI_EVENT_USAGE);
    g_assert_cmpint(ai_event_get_cost_micros(event), ==, -1);
}

static void test_malformed(void)
{
    const gchar *cases[] = { "", "null", "[]", "{}", "{\"type\":1}", "bad json",
        "{\"type\":\"item.completed\",\"item\":null}",
        "{\"type\":\"item.completed\",\"item\":{\"type\":\"agent_message\",\"text\":4}}",
        "{\"type\":\"thread.started\",\"thread_id\":{}}",
        "{\"type\":\"turn.started\"}",
        "{\"type\":\"turn.failed\",\"error\":{\"message\":\"denied\"}}",
        "{\"type\":\"error\",\"message\":\"denied\"}" };
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    guint i;
    for (i = 0; i < G_N_ELEMENTS(cases); i++)
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(AiResponse) response = AI_CLI_CLIENT_GET_CLASS(c)->parse_json_output(AI_CLI_CLIENT(c), cases[i], &error);
        g_assert_null(response);
        g_assert_nonnull(error);
    }
}

typedef struct { GMainLoop *loop; AiResponse *response; GError *error; gboolean streaming; } Result;
static void done(GObject *source, GAsyncResult *result, gpointer data)
{
    Result *r = data;
    r->response = r->streaming ? ai_streamable_chat_stream_finish(AI_STREAMABLE(source), result, &r->error)
                              : ai_provider_chat_finish(AI_PROVIDER(source), result, &r->error);
    g_main_loop_quit(r->loop);
}
static void delta(AiStreamable *s, const gchar *text, gpointer data)
{ (void)s; g_string_append(data, text); }

static gboolean cancel_request(gpointer data)
{
    g_cancellable_cancel(G_CANCELLABLE(data));
    return G_SOURCE_REMOVE;
}

static void test_subprocess(gconstpointer data)
{
    gint mode = GPOINTER_TO_INT(data);
    g_autofree gchar *dir = g_dir_make_tmp("ai-codex-test-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "codex", NULL);
    g_autofree gchar *output = g_build_filename(dir, "output", NULL);
    g_autofree gchar *input = g_build_filename(dir, "input", NULL);
    g_autofree gchar *args = g_build_filename(dir, "args", NULL);
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    g_autofree gchar *padding = g_strnfill(mode == 7 || mode == 8 ? 524288 : 0, 'x');
    g_autofree gchar *prompt = g_strconcat("Private prompt", padding, NULL);
    g_autoptr(AiMessage) message = ai_message_new_user(prompt);
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    GList *messages = g_list_append(NULL, message);
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL, *seen = NULL, *argv = NULL;
    GString *deltas = g_string_new(NULL);
    const gchar *script = "#!/bin/sh\nprintf '%s\\n' \"$@\" > args\ncat > input\ncat output\n";
    if (mode == 4)
        script = "#!/bin/sh\nprintf '%s\\n' \"$@\" > args\ncat > input\ncat output\necho failure >&2\nexit 7\n";
    if (mode == 5 || mode == 7)
        script = "#!/bin/sh\nprintf '%s\\n' \"$@\" > args\nhead -c 262144 /dev/zero >&2\ncat > input\ncat output\n";
    if (mode == 6)
        script = "#!/bin/sh\nprintf '%s\\n' \"$@\" > args\ncat > input\nexec sleep 5\n";
    if (mode == 8)
        script = "#!/bin/sh\nprintf '%s\\n' \"$@\" > args\nexec sleep 5\n";
    if (mode == 9)
        script = "#!/bin/sh\nprintf '%s\\n' \"$@\" > args\ncat > input\necho authentication-failed >&2\nexit 1\n";
    g_assert_true(g_file_set_contents(path, script, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0700), ==, 0);
    g_assert_true(g_file_set_contents(output, mode == 3 ? "{\"type\":\"turn.started\"}\n" : mode == 10 ? "{\"type\":\"turn.failed\",\"error\":{\"message\":\"denied\"}}\n" : ok_json, -1, NULL));
    ai_cli_client_set_executable_path(AI_CLI_CLIENT(c), path);
    ai_cli_client_set_working_directory(AI_CLI_CLIENT(c), dir);
    ai_cli_client_set_system_prompt(AI_CLI_CLIENT(c), "System instructions");
    ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(c), mode == 6 ? 100 : 2000);
    g_signal_connect(c, "delta", G_CALLBACK(delta), deltas);
    if (mode == 0)
        response = ai_cli_client_chat_sync(AI_CLI_CLIENT(c), messages, NULL, &error);
    else
    {
        Result r = { g_main_loop_new(NULL, FALSE), NULL, NULL, mode != 1 };
        if (mode == 8) g_timeout_add(100, cancel_request, cancel);
        if (r.streaming)
            ai_streamable_chat_stream_async(AI_STREAMABLE(c), messages, "System instructions", 0, NULL, cancel, done, &r);
        else
            ai_provider_chat_async(AI_PROVIDER(c), messages, "System instructions", 0, NULL, NULL, done, &r);
        g_main_loop_run(r.loop);
        g_main_loop_unref(r.loop);
        response = r.response; error = r.error;
    }
    if (mode == 3 || mode == 4 || mode == 6 || mode >= 8)
    {
        gint expected = mode == 3 ? AI_ERROR_CLI_PARSE_ERROR : mode == 6 ? AI_ERROR_TIMEOUT : AI_ERROR_CLI_EXECUTION;
        g_assert_null(response);
        if (mode == 8) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
        else g_assert_error(error, AI_ERROR, expected);
        if (mode == 9) g_assert_nonnull(strstr(error->message, "authentication-failed"));
    }
    else
    {
        g_assert_no_error(error);
        text = ai_response_get_text(response);
        g_assert_cmpstr(text, ==, "Hello");
        if (mode != 0) g_assert_cmpstr(deltas->str, ==, "Hello");
    }
    if (mode != 8)
    {
        g_assert_true(g_file_get_contents(input, &seen, NULL, NULL));
        g_assert_nonnull(strstr(seen, prompt));
        g_assert_nonnull(strstr(seen, "System instructions"));
    }
    g_assert_true(g_file_get_contents(args, &argv, NULL, NULL));
    g_assert_null(strstr(argv, "Private prompt"));
    g_assert_nonnull(strstr(argv, "--sandbox\nread-only"));
    g_string_free(deltas, TRUE);
    g_list_free(messages);
    g_unlink(path); g_unlink(input); g_unlink(output); g_unlink(args); g_rmdir(dir);
}

static void models_done(GObject *source, GAsyncResult *result, gpointer data)
{
    const gchar *expected[] = { "gpt-6-astra", "gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna",
        "gpt-5.5", "gpt-5.4-mini", "gpt-5.3-codex-spark" };
    g_autoptr(GError) error = NULL;
    GList *models = ai_provider_list_models_finish(AI_PROVIDER(source), result, &error), *l;
    guint i = 0;
    g_assert_no_error(error);
    g_assert_cmpuint(g_list_length(models), ==, G_N_ELEMENTS(expected));
    for (l = models; l != NULL; l = l->next) g_assert_cmpstr(l->data, ==, expected[i++]);
    g_list_free_full(models, g_free);
    g_main_loop_quit(data);
}
static void test_registration(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GObject) c = ai_provider_factory_new_from_string("codex-cli", NULL, &error);
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    g_assert_no_error(error);
    g_assert_true(AI_IS_CODEX_CLI_CLIENT(c));
    g_assert_cmpint(ai_provider_type_from_string("codex"), ==, AI_PROVIDER_CODEX_CLI);
    g_assert_cmpint(ai_provider_get_provider_type(AI_PROVIDER(c)), ==, AI_PROVIDER_CODEX_CLI);
    g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_CODEX_CLI), ==, "codex-cli");
    ai_provider_list_models_async(AI_PROVIDER(c), NULL, models_done, loop);
    g_main_loop_run(loop);
}

static void test_claude_sandbox(void)
{
    g_autoptr(AiClaudeCodeClient) c = ai_claude_code_client_new();
    g_autoptr(AiClaudeTmuxClient) tmux = ai_claude_tmux_client_new();
    g_auto(GStrv) argv = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *merged = NULL;
    JsonObject *root, *sandbox;
    g_object_set(c, "sandbox", "enabled", "settings", "{\"hooks\":{},\"sandbox\":{\"network\":{\"allowedDomains\":[\"example.org\"]}}}", NULL);
    argv = AI_CLI_CLIENT_GET_CLASS(c)->build_argv(AI_CLI_CLIENT(c), NULL, NULL, 0, FALSE);
    g_assert_nonnull(argv);
    g_assert_true(json_parser_load_from_data(parser, flag_value(argv, "--settings"), -1, &error));
    root = ai_json_root_object(parser);
    sandbox = ai_json_get_object(root, "sandbox");
    g_assert_true(ai_json_get_boolean(sandbox, "enabled", FALSE));
    g_assert_false(ai_json_get_boolean(sandbox, "allowUnsandboxedCommands", TRUE));
    g_assert_true(ai_json_get_boolean(sandbox, "failIfUnavailable", FALSE));
    g_assert_nonnull(ai_json_get_object(root, "hooks"));
    g_assert_nonnull(ai_json_get_object(sandbox, "network"));
    g_object_set(tmux, "sandbox", "disabled", NULL);
    g_object_get(tmux, "sandbox", &merged, NULL);
    g_assert_cmpstr(merged, ==, "disabled");
    g_clear_pointer(&merged, g_free);
    merged = ai_claude_sandbox_settings("{\"hooks\":{\"Stop\":[]}}", "enabled", &error);
    g_assert_no_error(error);
    g_assert_nonnull(strstr(merged, "Stop"));
    g_clear_pointer(&merged, g_free);
    merged = ai_claude_sandbox_settings("{}", "invalid", &error);
    g_assert_null(merged);
    g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
}

/* Exercise option interactions without invoking a model. */
static void test_option_matrix(void)
{
    const gchar *policies[] = { NULL, "read-only", "workspace-write", "danger-full-access", "", "invalid" };
    guint i, bypass;
    for (i = 0; i < G_N_ELEMENTS(policies); i++)
    for (bypass = 0; bypass < 2; bypass++)
    {
        g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
        g_auto(GStrv) argv = NULL;
        gboolean valid = i < 4 && (!bypass || i == 0 || i == 3);
        g_object_set(c, "sandbox", policies[i], "skip-permissions", bypass != 0, NULL);
        argv = AI_CLI_CLIENT_GET_CLASS(c)->build_argv(AI_CLI_CLIENT(c), NULL, NULL, 0, TRUE);
        if (!valid) { g_assert_null(argv); continue; }
        g_assert_nonnull(argv);
        g_assert_cmpint(g_strv_contains((const gchar *const *)argv,
            "--dangerously-bypass-approvals-and-sandbox"), ==, bypass);
        g_assert_cmpstr(flag_value(argv, "--sandbox"), ==,
                       bypass ? NULL : policies[i] != NULL ? policies[i] : "read-only");
    }
}

static void test_session_context(void)
{
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    AiCliClient *cli = AI_CLI_CLIENT(c);
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(c);
    g_autoptr(AiMessage) first = ai_message_new_user("Earlier question");
    g_autoptr(AiMessage) answer = ai_message_new_assistant("Earlier answer");
    g_autoptr(AiMessage) last = ai_message_new_user("Latest question");
    GList *messages = g_list_append(NULL, first);
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *stdin_data = NULL;
    messages = g_list_append(messages, answer);
    messages = g_list_append(messages, last);
    ai_cli_client_mark_portable_context(cli);
    argv = klass->build_argv(cli, messages, "Initial instructions", 0, FALSE);
    stdin_data = klass->build_stdin(cli, messages);
    g_assert_nonnull(strstr(stdin_data, "Earlier question"));
    g_assert_nonnull(strstr(stdin_data, "Earlier answer"));
    g_assert_nonnull(strstr(stdin_data, "Initial instructions"));
    g_clear_pointer(&argv, g_strfreev);
    g_clear_pointer(&stdin_data, g_free);
    ai_cli_client_set_session_id(cli, "thread-explicit");
    g_object_set(c, "continue-session", TRUE, NULL);
    argv = klass->build_argv(cli, messages, "Initial instructions", 0, TRUE);
    g_assert_cmpstr(flag_value(argv, "resume"), ==, "thread-explicit");
    g_assert_false(g_strv_contains((const gchar *const *)argv, "--last"));
    stdin_data = klass->build_stdin(cli, messages);
    g_assert_null(strstr(stdin_data, "Earlier"));
    g_assert_null(strstr(stdin_data, "Initial instructions"));
    g_assert_nonnull(strstr(stdin_data, "Latest question"));
    g_clear_pointer(&argv, g_strfreev);
    g_clear_pointer(&stdin_data, g_free);
    ai_cli_client_set_session_persistence(cli, FALSE);
    argv = klass->build_argv(cli, messages, "Stateless instructions", 0, FALSE);
    stdin_data = klass->build_stdin(cli, messages);
    g_assert_false(g_strv_contains((const gchar *const *)argv, "resume"));
    g_assert_nonnull(strstr(stdin_data, "Earlier question"));
    g_assert_nonnull(strstr(stdin_data, "Stateless instructions"));
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(AiResponse) response = klass->parse_json_output(cli, ok_json, &error);
        g_assert_no_error(error);
        g_assert_cmpstr(ai_cli_client_get_session_id(cli), ==, "thread-explicit");
    }
    g_list_free(messages);
}

static void test_execution_options(void)
{
    const gchar *efforts[] = { "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra", NULL, "", "bogus" };
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    guint i;
    g_object_set(c, "profile", "profile with spaces", "search", TRUE,
        "additional-directories", " , /tmp/a b, , /tmp/$(literal),", NULL);
    for (i = 0; i < G_N_ELEMENTS(efforts); i++)
    {
        g_auto(GStrv) argv = NULL;
        ai_cli_client_set_effort_level(AI_CLI_CLIENT(c), efforts[i]);
        argv = AI_CLI_CLIENT_GET_CLASS(c)->build_argv(AI_CLI_CLIENT(c), NULL, NULL, 0, FALSE);
        if (i == 10) { g_assert_null(argv); continue; }
        g_assert_nonnull(argv);
        g_assert_true(g_strv_contains((const gchar *const *)argv, "--search"));
        g_assert_cmpstr(flag_value(argv, "--profile"), ==, "profile with spaces");
        g_assert_cmpstr(flag_value(argv, "--add-dir"), ==, "/tmp/a b");
        g_assert_true(g_strv_contains((const gchar *const *)argv, "/tmp/$(literal)"));
        if (i < 8)
        {
            g_autofree gchar *expected = g_strdup_printf("model_reasoning_effort=\"%s\"", efforts[i]);
            g_assert_cmpstr(flag_value(argv, "-c"), ==, expected);
        }
        else if (i == 8) g_assert_cmpstr(flag_value(argv, "-c"), ==, "model_reasoning_effort=\"medium\"");
        else g_assert_null(flag_value(argv, "-c"));
    }
}

static void test_event_translation(void)
{
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    g_autoptr(AiResponse) response = ai_response_new("", "test");
    g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
    g_auto(GStrv) lines = g_strsplit(ok_json, "\n", -1);
    const AiEventKind expected[] = { AI_EVENT_STREAM_START, AI_EVENT_THINKING_DELTA,
        AI_EVENT_TOOL_STARTED, AI_EVENT_TOOL_FINISHED, AI_EVENT_TEXT_DELTA, AI_EVENT_USAGE };
    AiCliClientClass *klass = AI_CLI_CLIENT_GET_CLASS(c);
    guint i;
    for (i = 0; lines[i] != NULL; i++)
    {
        g_autoptr(GError) error = NULL;
        g_assert_true(klass->parse_stream_events(AI_CLI_CLIENT(c), lines[i], response, events, &error));
        g_assert_no_error(error);
    }
    g_assert_cmpuint(events->len, ==, G_N_ELEMENTS(expected));
    for (i = 0; i < events->len; i++)
        g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(events, i)), ==, expected[i]);
    g_assert_cmpstr(ai_event_get_text(g_ptr_array_index(events, 1)), ==, "Thinking");
    g_assert_cmpstr(ai_tool_use_get_id(ai_event_get_tool_use(g_ptr_array_index(events, 2))), ==, "c");
    g_assert_cmpstr(ai_tool_result_get_content(ai_event_get_tool_result(g_ptr_array_index(events, 3))), ==, "/tmp");
    g_assert_false(ai_tool_result_get_is_error(ai_event_get_tool_result(g_ptr_array_index(events, 3))));
    g_assert_cmpint(ai_event_get_cost_micros(g_ptr_array_index(events, 5)), ==, 350);
    g_assert_cmpint(ai_response_get_cost_micros(response), ==, 350);
}

static void test_tool_variants(void)
{
    const gchar *kinds[] = { "command_execution", "file_change", "mcp_tool_call", "web_search" };
    guint i;
    for (i = 0; i < G_N_ELEMENTS(kinds); i++)
    {
        g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
        g_autoptr(AiResponse) response = ai_response_new("", "test");
        g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
        g_autoptr(GError) error = NULL;
        g_autofree gchar *line = g_strdup_printf(
            "{\"type\":\"item.completed\",\"item\":{\"id\":\"t\",\"type\":\"%s\",\"status\":\"failed\",\"error\":{\"message\":\"denied\"}}}", kinds[i]);
        g_assert_true(AI_CLI_CLIENT_GET_CLASS(c)->parse_stream_events(AI_CLI_CLIENT(c), line, response, events, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(events->len, ==, 1);
        g_assert_cmpint(ai_event_get_kind(g_ptr_array_index(events, 0)), ==, AI_EVENT_TOOL_FINISHED);
        g_assert_true(ai_tool_result_get_is_error(ai_event_get_tool_result(g_ptr_array_index(events, 0))));
        g_assert_null(ai_response_get_content_blocks(response));
    }
}

static void test_protocol_edges(void)
{
    const gchar *payloads[] = {
        " \t\r\n{\"type\":\"future.event\"}\n{\"type\":\"turn.completed\"}",
        "{\"type\":\"turn.completed\",\"usage\":null}",
        "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":1e100,\"output_tokens\":-1}}",
        "{\"type\":\"turn.completed\",\"usage\":{\"input_tokens\":\"oops\",\"output_tokens\":3.0}}",
        "{\"type\":\"turn.completed\"}\n{\"type\":\"turn.started\"}"
    };
    guint i;
    for (i = 0; i < G_N_ELEMENTS(payloads); i++)
    {
        g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
        g_autoptr(GError) error = NULL;
        g_autoptr(AiResponse) response = AI_CLI_CLIENT_GET_CLASS(c)->parse_json_output(AI_CLI_CLIENT(c), payloads[i], &error);
        if (i == 4)
        {
            g_assert_null(response);
            g_assert_error(error, AI_ERROR, AI_ERROR_CLI_PARSE_ERROR);
            continue;
        }
        g_assert_no_error(error);
        g_assert_nonnull(response);
        if (i == 2)
        {
            g_assert_cmpint(ai_usage_get_input_tokens(ai_response_get_usage(response)), ==, G_MAXINT);
            g_assert_cmpint(ai_usage_get_output_tokens(ai_response_get_usage(response)), ==, 0);
        }
        if (i == 3)
        {
            g_assert_cmpint(ai_usage_get_input_tokens(ai_response_get_usage(response)), ==, 0);
            g_assert_cmpint(ai_usage_get_output_tokens(ai_response_get_usage(response)), ==, 3);
        }
    }
}

static void test_claude_settings_file(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("ai-sandbox-settings-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "settings.json", NULL);
    const gchar *original = "{\"hooks\":{\"Stop\":[]},\"sandbox\":{\"excludedCommands\":[\"docker\"],\"filesystem\":{\"denyRead\":[\"secret\"]}}}";
    g_autofree gchar *merged = NULL, *unchanged = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_assert_true(g_file_set_contents(path, original, -1, &error));
    g_assert_no_error(error);
    merged = ai_claude_sandbox_settings(path, "enabled", &error);
    g_assert_no_error(error);
    g_assert_true(json_parser_load_from_data(parser, merged, -1, &error));
    g_assert_nonnull(ai_json_get_object(ai_json_root_object(parser), "hooks"));
    g_assert_nonnull(strstr(merged, "denyRead"));
    g_assert_nonnull(strstr(merged, "docker"));
    g_assert_true(g_file_get_contents(path, &unchanged, NULL, &error));
    g_assert_cmpstr(unchanged, ==, original);
    g_clear_pointer(&merged, g_free);
    merged = ai_claude_sandbox_settings(" \n {\"sandbox\":{\"enabled\":true}}", "disabled", &error);
    g_assert_no_error(error);
    g_assert_nonnull(strstr(merged, "false"));
    g_clear_pointer(&merged, g_free);
    merged = ai_claude_sandbox_settings("{\"sandbox\":[]}", "enabled", &error);
    g_assert_null(merged);
    g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
    g_clear_error(&error);
    g_assert_true(g_file_set_contents(path, "[]", -1, NULL));
    merged = ai_claude_sandbox_settings(path, "enabled", &error);
    g_assert_null(merged);
    g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
    g_clear_error(&error);
    g_unlink(path);
    merged = ai_claude_sandbox_settings(path, "enabled", &error);
    g_assert_null(merged);
    g_assert_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT);
    g_rmdir(dir);
}

static void test_precancelled(void)
{
    g_autoptr(AiCodexCliClient) c = ai_codex_cli_client_new();
    g_autoptr(GCancellable) cancel = g_cancellable_new();
    Result r = { g_main_loop_new(NULL, FALSE), NULL, NULL, TRUE };
    /* Cancellation must win even before executable resolution. */
    ai_cli_client_set_executable_path(AI_CLI_CLIENT(c), "/does-not-exist/codex");
    g_cancellable_cancel(cancel);
    ai_streamable_chat_stream_async(AI_STREAMABLE(c), NULL, NULL, 0, NULL, cancel, done, &r);
    g_main_loop_run(r.loop);
    g_assert_null(r.response);
    g_assert_error(r.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&r.error);
    g_main_loop_unref(r.loop);
}

/* Build isolated Git fixtures without relying on the developer's identity,
 * hooks, signing setup, or main checkout. */
static void
fixture_git(const gchar *dir, const gchar *const *arguments)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	guint i;

	g_subprocess_launcher_set_cwd(launcher, dir);
	g_ptr_array_add(argv, (gpointer)"git");
	for (i = 0; arguments[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)arguments[i]);
	g_ptr_array_add(argv, NULL);
	process = g_subprocess_launcher_spawnv(launcher,
		(const gchar *const *)argv->pdata, NULL);
	g_assert_nonnull(process);
	g_assert_true(g_subprocess_wait_check(process, NULL, NULL));
}

/* Real filename-only Codex events must acquire a stable, labeled preview. */
static void
test_file_change_workspace_preview(void)
{
	const gchar *init[] = { "init", "-q", NULL };
	const gchar *add[] = { "add", "--", "file[1].c", "deleted.c", NULL };
	const gchar *commit[] = { "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
		"-c", "core.hooksPath=/dev/null", "commit", "--no-gpg-sign", "-qm", "baseline", NULL };
	g_autofree gchar *dir = g_dir_make_tmp("ai-codex-preview-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "file[1].c", NULL);
	g_autofree gchar *deleted = g_build_filename(dir, "deleted.c", NULL);
	g_autofree gchar *added = g_build_filename(dir, "new file.c", NULL);
	g_autoptr(AiCodexCliClient) client = ai_codex_cli_client_new();
	g_autoptr(AiResponse) response = ai_response_new("", "test");
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	AiEvent *event;
	AiToolCall *call;

	g_assert_nonnull(dir);
	fixture_git(dir, init);
	g_assert_true(g_file_set_contents(path, "int before;\n", -1, NULL));
	g_assert_true(g_file_set_contents(deleted, "int removed;\n", -1, NULL));
	fixture_git(dir, add);
	fixture_git(dir, commit);
	g_assert_true(g_file_set_contents(path, "int after;\n", -1, NULL));
	g_assert_cmpint(g_remove(deleted), ==, 0);
	g_assert_true(g_file_set_contents(added, "int created;\n", -1, NULL));
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), dir);
	g_assert_true(AI_CLI_CLIENT_GET_CLASS(client)->parse_stream_events(
		AI_CLI_CLIENT(client),
		"{\"type\":\"item.completed\",\"item\":{\"id\":\"patch\",\"type\":\"file_change\","
		"\"status\":\"completed\",\"changes\":[{\"path\":\"file[1].c\",\"kind\":\"update\"},"
		"{\"path\":\"deleted.c\",\"kind\":\"delete\"},{\"path\":\"new file.c\",\"kind\":\"add\"}]}}",
		response, events, NULL));
	g_assert_cmpuint(events->len, ==, 1);
	event = g_ptr_array_index(events, 0);
	call = ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), ai_event_get_tool_use(event));
	ai_tool_call_finish(call, ai_event_get_tool_result(event));
	g_object_set(block, "show-previews", TRUE, NULL);
	ai_view_block_set_expanded(block, TRUE);
	before = ai_view_block_render_text(block, 0);
	g_assert_nonnull(strstr(before, "Changed 3 files"));
	g_assert_nonnull(strstr(before, "Working-tree diff against HEAD (may include earlier changes)"));
	g_assert_nonnull(strstr(before, "Current file content (no tracked baseline)"));
	g_assert_nonnull(strstr(before, "int before;"));
	g_assert_nonnull(strstr(before, "int after;"));
	g_assert_nonnull(strstr(before, "int removed;"));
	g_assert_nonnull(strstr(before, "int created;"));
	g_assert_null(strstr(before, "edit text unavailable"));

	/* Re-render after another edit: history must use the captured patch. */
	g_assert_true(g_file_set_contents(path, "int later;\n", -1, NULL));
	ai_view_tool_block_call_changed(AI_VIEW_TOOL_BLOCK(block));
	after = ai_view_block_render_text(block, 0);
	g_assert_cmpstr(before, ==, after);

	{
		g_autoptr(GSubprocess) cleanup = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE,
			NULL, "rm", "-rf", "--", dir, NULL);
		g_assert_true(g_subprocess_wait_check(cleanup, NULL, NULL));
	}
}

/* Exact provider text wins; failed edits and missing repositories stay honest. */
static void
test_file_change_preview_fallbacks(void)
{
	const gchar *lines[] = {
		"{\"type\":\"item.completed\",\"item\":{\"id\":\"p\",\"type\":\"file_change\",\"status\":\"completed\","
		"\"changes\":[{\"path\":\"file.c\",\"diff\":\"@@ -1 +1 @@\\n-before\\n+after\"}]}}",
		"{\"type\":\"item.completed\",\"item\":{\"id\":\"p\",\"type\":\"file_change\",\"status\":\"failed\","
		"\"changes\":[{\"path\":\"file.c\",\"kind\":\"update\"}]}}",
		"{\"type\":\"item.completed\",\"item\":{\"id\":\"p\",\"type\":\"file_change\",\"status\":\"completed\","
		"\"changes\":[null,{\"path\":4},{\"path\":\"file.c\",\"kind\":\"update\"}]}}"
	};
	g_autofree gchar *dir = g_dir_make_tmp("ai-codex-no-git-XXXXXX", NULL);
	guint i;

	for (i = 0; i < G_N_ELEMENTS(lines); i++)
	{
		g_autoptr(AiCodexCliClient) client = ai_codex_cli_client_new();
		g_autoptr(AiResponse) response = ai_response_new("", "test");
		g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
		g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
		g_autofree gchar *text = NULL;
		AiEvent *event;
		AiToolCall *call;

		ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), dir);
		g_assert_true(AI_CLI_CLIENT_GET_CLASS(client)->parse_stream_events(
			AI_CLI_CLIENT(client), lines[i], response, events, NULL));
		event = g_ptr_array_index(events, 0);
		call = ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), ai_event_get_tool_use(event));
		ai_tool_call_finish(call, ai_event_get_tool_result(event));
		g_object_set(block, "show-previews", TRUE, NULL);
		text = ai_view_block_render_text(block, 0);
		g_assert_null(strstr(text, "Working-tree diff"));
		g_assert_nonnull(strstr(text, i == 0 ? "after" : "edit text unavailable"));
	}
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* A broken or endlessly verbose Git must not hang or exhaust the frontend. */
static void
test_file_change_capture_limits(gconstpointer data)
{
	gboolean verbose = GPOINTER_TO_INT(data) != 0;
	g_autofree gchar *dir = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *search_path = NULL;
	g_autoptr(AiCodexCliClient) client = NULL;
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GPtrArray) events = NULL;
	JsonObject *input;
	JsonObject *change;
	AiEvent *event;
	gint64 started;

	/* Isolate PATH changes and put the watchdog outside the blocked loop. */
	if (!g_test_subprocess())
	{
		g_test_trap_subprocess(NULL, 5 * G_USEC_PER_SEC, G_TEST_SUBPROCESS_DEFAULT);
		g_test_trap_assert_passed();
		return;
	}
	dir = g_dir_make_tmp("ai-codex-diff-limit-XXXXXX", NULL);
	script = g_build_filename(dir, "git", NULL);
	search_path = g_strconcat(dir, G_SEARCHPATH_SEPARATOR_S, g_getenv("PATH"), NULL);
	g_assert_true(g_file_set_contents(script, verbose
		? "#!/bin/sh\nwhile :; do printf '+int changed;\\n'; done\n"
		: "#!/bin/sh\nexec sleep 10\n", -1, NULL));
	g_assert_cmpint(g_chmod(script, 0700), ==, 0);
	g_setenv("PATH", search_path, TRUE);
	client = ai_codex_cli_client_new();
	response = ai_response_new("", "test");
	events = g_ptr_array_new_with_free_func((GDestroyNotify)ai_event_unref);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(client), dir);
	started = g_get_monotonic_time();
	g_assert_true(AI_CLI_CLIENT_GET_CLASS(client)->parse_stream_events(
		AI_CLI_CLIENT(client),
		"{\"type\":\"item.completed\",\"item\":{\"id\":\"p\",\"type\":\"file_change\",\"status\":\"completed\","
		"\"changes\":[{\"path\":\"file.c\",\"kind\":\"update\"}]}}",
		response, events, NULL));
	g_assert_cmpint(g_get_monotonic_time() - started, <, 3 * G_USEC_PER_SEC);
	event = g_ptr_array_index(events, 0);
	input = json_node_get_object(ai_tool_use_get_input(ai_event_get_tool_use(event)));
	change = json_array_get_object_element(json_object_get_array_member(input, "changes"), 0);
	if (verbose)
	{
		g_assert_cmpuint(strlen(json_object_get_string_member(change, "diff")), <=, 65536);
		g_assert_true(json_object_get_boolean_member(change, "diff_truncated"));
	}
	else g_assert_false(json_object_has_member(change, "diff"));
	g_assert_cmpint(g_remove(script), ==, 0);
	g_assert_cmpint(g_rmdir(dir), ==, 0);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    /* Independent watchdog: a blocked main context cannot fire its own timer. */
    alarm(30);
    g_test_add_func("/codex/registration", test_registration);
    g_test_add_func("/codex/argv", test_argv);
    g_test_add_func("/codex/parse", test_parse);
    g_test_add_func("/codex/astra-cost", test_astra_usage_event_cost);
    g_test_add_func("/codex/unpriced", test_unpriced_model_omits_cost);
    g_test_add_func("/codex/malformed", test_malformed);
    g_test_add_data_func("/codex/process/sync", GINT_TO_POINTER(0), test_subprocess);
    g_test_add_data_func("/codex/process/async", GINT_TO_POINTER(1), test_subprocess);
    g_test_add_data_func("/codex/process/stream", GINT_TO_POINTER(2), test_subprocess);
    g_test_add_data_func("/codex/process/truncated", GINT_TO_POINTER(3), test_subprocess);
    g_test_add_data_func("/codex/process/nonzero", GINT_TO_POINTER(4), test_subprocess);
    g_test_add_data_func("/codex/process/large-stderr", GINT_TO_POINTER(5), test_subprocess);
    g_test_add_data_func("/codex/process/timeout", GINT_TO_POINTER(6), test_subprocess);
    g_test_add_data_func("/codex/process/large-duplex", GINT_TO_POINTER(7), test_subprocess);
    g_test_add_data_func("/codex/process/cancel-blocked-stdin", GINT_TO_POINTER(8), test_subprocess);
    g_test_add_data_func("/codex/process/stderr-only", GINT_TO_POINTER(9), test_subprocess);
    g_test_add_data_func("/codex/process/error-zero-exit", GINT_TO_POINTER(10), test_subprocess);
    g_test_add_func("/sandbox/claude", test_claude_sandbox);
    g_test_add_func("/codex/options/policy-matrix", test_option_matrix);
    g_test_add_func("/codex/options/execution", test_execution_options);
    g_test_add_func("/codex/session-context", test_session_context);
    g_test_add_func("/codex/events/order", test_event_translation);
    g_test_add_func("/codex/events/tool-variants", test_tool_variants);
	g_test_add_func("/codex/events/file-change-preview", test_file_change_workspace_preview);
	g_test_add_func("/codex/events/file-change-fallbacks", test_file_change_preview_fallbacks);
	g_test_add_data_func("/codex/events/file-change-timeout", GINT_TO_POINTER(0), test_file_change_capture_limits);
	g_test_add_data_func("/codex/events/file-change-output-limit", GINT_TO_POINTER(1), test_file_change_capture_limits);
    g_test_add_func("/codex/protocol-edges", test_protocol_edges);
    g_test_add_func("/codex/process/precancelled", test_precancelled);
    g_test_add_func("/sandbox/claude-settings-file", test_claude_settings_file);
    return g_test_run();
}
