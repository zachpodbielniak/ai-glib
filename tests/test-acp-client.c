/*
 * test-acp-client.c - AiAcpClient against a scripted ACP agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Driven against tests/fixtures/fake-acp-agent.bash: a scripted stand-in
 * that records every line the client writes and answers exactly what each
 * test tells it to, in the shapes of the Agent Client Protocol v1 pages
 * (initialization, session-setup, prompt-turn, tool-calls, file-system).
 * The client's request ids are fixed -- initialize 1, the session request
 * 2, session/prompt 3, a session/new after a refused load 4 -- which is
 * what lets a static script answer them.
 *
 * Covered: the handshake and its requests, streamed updates (text,
 * thought, tool calls, plan, usage), permission requests under both
 * settings, session/cancel and its grace, the kill after an answered
 * prompt, stop reasons, errors classified through the shared limit table,
 * the fs/ methods and their confinement, MCP passthrough, session/load and
 * its fallback, the synchronous path, and the registration (provider
 * type, factory, can_steer FALSE, searches_tools FALSE).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ai-glib.h"
#include "core/ai-steerable.h"

/* ================================================================
 * Script fragments
 *
 * Written with ' for " so they stay readable; fixture_write_script()
 * turns them back.
 * ================================================================ */

#define INIT_CAPS(load, image) \
    "read\n" \
    "send {'jsonrpc':'2.0','id':1,'result':{'protocolVersion':1,'agentCapabilities':{'loadSession':" load ",'promptCapabilities':{'image':" image ",'embeddedContext':false},'mcpCapabilities':{'http':false,'sse':false}},'agentInfo':{'name':'fake','version':'0'},'authMethods':[]}}\n"

#define INIT INIT_CAPS("false", "false")

#define SESSION \
    "read\n" \
    "send {'jsonrpc':'2.0','id':2,'result':{'sessionId':'sess-1'}}\n"

/* initialize, session/new, and session/prompt read. */
#define HANDSHAKE INIT SESSION "read\n"

#define UPDATE(body) \
    "send {'jsonrpc':'2.0','method':'session/update','params':{'sessionId':'sess-1','update':" body "}}\n"

#define ANSWER \
    UPDATE("{'sessionUpdate':'agent_thought_chunk','content':{'type':'text','text':'Thinking'}}") \
    UPDATE("{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'Hel'}}") \
    UPDATE("{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'lo'}}") \
    UPDATE("{'sessionUpdate':'plan','entries':[{'content':'look','priority':'high','status':'completed'},{'content':'answer','priority':'low','status':'in_progress'}]}") \
    UPDATE("{'sessionUpdate':'tool_call','toolCallId':'call-1','title':'List files','kind':'execute','status':'pending','rawInput':{'command':'ls'}}") \
    UPDATE("{'sessionUpdate':'tool_call_update','toolCallId':'call-1','status':'completed','content':[{'type':'content','content':{'type':'text','text':'a.txt'}}]}") \
    UPDATE("{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'Done.'}}") \
    UPDATE("{'sessionUpdate':'usage_update','used':100,'size':1000,'cost':{'amount':0.25,'currency':'USD'}}")

#define END_TURN(reason) \
    "send {'jsonrpc':'2.0','id':3,'result':{'stopReason':'" reason "','usage':{'inputTokens':5,'outputTokens':7}}}\n" \
    "eof\n"

#define PLAIN_TURN HANDSHAKE ANSWER END_TURN("end_turn")

/* ================================================================
 * Fixture
 * ================================================================ */

typedef struct
{
    gchar        *dir;
    gchar        *work;
    AiAcpClient  *client;
    GCancellable *cancellable;

    gboolean      done;
    AiResponse   *response;
    GError       *error;

    GPtrArray    *texts;          /* every TEXT_DELTA, in order */
    GPtrArray    *statuses;       /* every STATUS text */
    GString      *thinking;
    guint         tools_started;
    guint         tools_finished;
    gchar        *tool_output;    /* the last finished tool's result */
    gchar        *tool_name;
    gboolean      tool_failed;
    gboolean      cancel_on_text; /* cancel on the first text delta */
} Fixture;

static gboolean
on_deadline(gpointer user_data)
{
    gboolean *expired = user_data;

    *expired = TRUE;
    return G_SOURCE_REMOVE;
}

/* Iterate the default context until @flag is set, failing after 20 s. */
static void
iterate_until(gboolean *flag)
{
    gboolean expired = FALSE;
    guint id = g_timeout_add(20000, on_deadline, &expired);

    while (!*flag && !expired)
        g_main_context_iteration(NULL, TRUE);

    if (!expired)
        g_source_remove(id);
    g_assert_false(expired);
}

static void
fixture_write_script(Fixture *fx, const gchar *script)
{
    g_autofree gchar *path = g_build_filename(fx->dir, "script", NULL);
    g_autofree gchar *text = g_strdup(script);
    g_autoptr(GError) error = NULL;
    gchar *p;

    for (p = text; *p != '\0'; p++)
        if (*p == '\'')
            *p = '"';
    g_file_set_contents(path, text, -1, &error);
    g_assert_no_error(error);
}

static void
fixture_setup(Fixture *fx, const gchar *script)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *fake = NULL;
    g_autofree gchar *agent = NULL;
    g_autofree gchar *made = NULL;
    const gchar *argv[] = { NULL, "--acp", "two words", NULL };
    gsize length = 0;

    memset(fx, 0, sizeof(*fx));
    fx->texts = g_ptr_array_new_with_free_func(g_free);
    fx->statuses = g_ptr_array_new_with_free_func(g_free);
    fx->thinking = g_string_new(NULL);
    made = g_dir_make_tmp("ai-glib-acp-XXXXXX", &error);
    g_assert_no_error(error);
    /* Resolved, so paths the client reports compare equal. */
    fx->dir = realpath(made, NULL);
    g_assert_nonnull(fx->dir);
    fx->work = g_build_filename(fx->dir, "work", NULL);
    g_assert_cmpint(g_mkdir(fx->work, 0700), ==, 0);

    /* A private, executable copy: nothing depends on the checkout's mode bits. */
    g_assert_true(g_file_get_contents("tests/fixtures/fake-acp-agent.bash",
                                      &fake, &length, &error));
    g_assert_no_error(error);
    agent = g_build_filename(fx->dir, "agent", NULL);
    g_file_set_contents(agent, fake, (gssize)length, &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(agent, 0700), ==, 0);
    if (script != NULL)
        fixture_write_script(fx, script);

    argv[0] = agent;
    fx->client = ai_acp_client_new_for_command(argv);
    ai_cli_client_set_env(AI_CLI_CLIENT(fx->client), "FAKE_ACP_DIR", fx->dir);
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx->client), FALSE);
    ai_cli_client_set_working_directory(AI_CLI_CLIENT(fx->client), fx->work);
    /* A run that never closes stdin fails here instead of hanging. */
    ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(fx->client), 15000);
    ai_acp_client_set_shutdown_timeout_ms(fx->client, 2000);
}

static gchar *
fixture_read(Fixture *fx, const gchar *name)
{
    g_autofree gchar *path = g_build_filename(fx->dir, name, NULL);
    gchar *contents = NULL;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;
    return contents;
}

/* Line @index (0-based) of requests.log, parsed. */
static JsonObject *
fixture_request(Fixture *fx, guint index, JsonParser *parser)
{
    g_autofree gchar *log = fixture_read(fx, "requests.log");
    g_auto(GStrv) lines = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_nonnull(log);
    lines = g_strsplit(log, "\n", -1);
    g_assert_cmpuint(g_strv_length(lines), >, index);
    json_parser_load_from_data(parser, lines[index], -1, &error);
    g_assert_no_error(error);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
    return json_node_get_object(json_parser_get_root(parser));
}

static guint
fixture_request_count(Fixture *fx)
{
    g_autofree gchar *log = fixture_read(fx, "requests.log");
    g_auto(GStrv) lines = NULL;

    if (log == NULL)
        return 0;
    lines = g_strsplit(log, "\n", -1);
    return g_strv_length(lines) - 1;   /* the log ends with a newline */
}

/* Removes @path and everything under it, never following a link. */
static void
remove_tree(const gchar *path)
{
    GStatBuf st;

    if (g_lstat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode))
    {
        GDir *dir = g_dir_open(path, 0, NULL);
        const gchar *name;

        while (dir != NULL && (name = g_dir_read_name(dir)) != NULL)
        {
            g_autofree gchar *child = g_build_filename(path, name, NULL);

            remove_tree(child);
        }
        if (dir != NULL)
            g_dir_close(dir);
        g_rmdir(path);
    }
    else
    {
        g_unlink(path);
    }
}

static void
fixture_teardown(Fixture *fx)
{
    remove_tree(fx->dir);
    free(fx->dir);
    g_free(fx->work);
    g_clear_object(&fx->client);
    g_clear_object(&fx->cancellable);
    g_clear_object(&fx->response);
    g_clear_error(&fx->error);
    g_ptr_array_unref(fx->texts);
    g_ptr_array_unref(fx->statuses);
    g_string_free(fx->thinking, TRUE);
    g_free(fx->tool_output);
    g_free(fx->tool_name);
}

static void
on_event(AiEventSource *source, AiEvent *event, gpointer user_data)
{
    Fixture *fx = user_data;
    AiEventKind kind = ai_event_get_kind(event);

    (void)source;

    if (kind == AI_EVENT_TEXT_DELTA)
    {
        g_ptr_array_add(fx->texts, g_strdup(ai_event_get_text(event)));
        if (fx->cancel_on_text && fx->cancellable != NULL)
            g_cancellable_cancel(fx->cancellable);
    }
    else if (kind == AI_EVENT_THINKING_DELTA)
        g_string_append(fx->thinking, ai_event_get_text(event));
    else if (kind == AI_EVENT_TOOL_STARTED)
    {
        fx->tools_started++;
        g_free(fx->tool_name);
        fx->tool_name = g_strdup(ai_tool_use_get_name(ai_event_get_tool_use(event)));
    }
    else if (kind == AI_EVENT_TOOL_FINISHED)
    {
        AiToolResult *result = ai_event_get_tool_result(event);

        fx->tools_finished++;
        g_free(fx->tool_output);
        fx->tool_output = g_strdup(ai_tool_result_get_content(result));
        fx->tool_failed = ai_tool_result_get_is_error(result);
    }
    else if (kind == AI_EVENT_STATUS)
        g_ptr_array_add(fx->statuses, g_strdup(ai_event_get_text(event)));
}

static void
on_stream(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Fixture *fx = user_data;

    fx->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source), result,
                                                    &fx->error);
    fx->done = TRUE;
}

/* One streamed turn of @messages (a single "hi" when NULL). */
static void
fixture_run_messages(Fixture *fx, GList *messages, const gchar *system)
{
    g_autoptr(AiMessage) message = ai_message_new_user("hi");
    GList *own = NULL;
    gulong handler;

    if (messages == NULL)
        messages = own = g_list_append(NULL, message);
    handler = g_signal_connect(fx->client, "event", G_CALLBACK(on_event), fx);
    ai_streamable_chat_stream_async(AI_STREAMABLE(fx->client), messages, system, 0,
                                    NULL, fx->cancellable, on_stream, fx);
    iterate_until(&fx->done);
    g_signal_handler_disconnect(fx->client, handler);
    g_list_free(own);
}

static void
fixture_run(Fixture *fx)
{
    fixture_run_messages(fx, NULL, NULL);
}

static gchar *
response_text(Fixture *fx)
{
    g_assert_no_error(fx->error);
    g_assert_nonnull(fx->response);
    return ai_response_get_text(fx->response);
}

/* Whether any status event contains @needle. */
static gboolean
fixture_said(Fixture *fx, const gchar *needle)
{
    guint i;

    for (i = 0; i < fx->statuses->len; i++)
        if (strstr(g_ptr_array_index(fx->statuses, i), needle) != NULL)
            return TRUE;
    return FALSE;
}

/* ================================================================
 * Registration
 * ================================================================ */

static void
test_registration(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GObject) made = NULL;
    g_autoptr(AiAcpClient) client = ai_acp_client_new();

    g_assert_cmpint(ai_provider_type_from_string("acp"), ==, AI_PROVIDER_ACP);
    g_assert_cmpint(ai_provider_type_from_string("acp-agent"), ==, AI_PROVIDER_ACP);
    g_assert_cmpstr(ai_provider_type_to_string(AI_PROVIDER_ACP), ==, "acp");

    made = ai_provider_factory_new_from_string("acp", NULL, &error);
    g_assert_no_error(error);
    g_assert_true(AI_IS_ACP_CLIENT(made));
    g_assert_cmpint(ai_provider_get_provider_type(AI_PROVIDER(made)), ==, AI_PROVIDER_ACP);
    g_assert_cmpstr(ai_provider_get_default_model(AI_PROVIDER(made)), ==, AI_ACP_MODEL_DEFAULT);

    /* ACP v1 has no request that puts words into a running turn. */
    g_assert_false(ai_steerable_can_steer(AI_STEERABLE(client)));
    g_assert_false(ai_cli_client_searches_tools(AI_CLI_CLIENT(client)));
    g_assert_false(ai_cli_client_supports_config_dir(AI_CLI_CLIENT(client)));
    g_assert_true(ai_tool_endpoint_consumer_supports_kind(AI_TOOL_ENDPOINT_CONSUMER(client),
                                                          AI_ENDPOINT_KIND_MCP_CONFIG));
}

/* set_argv round-trips through the command line; nothing set is an error. */
static void
test_command(void)
{
    g_autoptr(AiAcpClient) client = ai_acp_client_new();
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;
    const gchar *words[] = { "my agent", "--acp", "it's", "$HOME", NULL };
    Fixture fx;

    g_unsetenv("AI_ACP_COMMAND");
    g_assert_null(ai_acp_client_dup_argv(client, &error));
    g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
    g_clear_error(&error);

    ai_acp_client_set_argv(client, words);
    argv = ai_acp_client_dup_argv(client, &error);
    g_assert_no_error(error);
    g_assert_cmpstrv(argv, words);
    g_clear_pointer(&argv, g_strfreev);

    /* The environment stands in for an unset property. */
    ai_acp_client_set_command(client, NULL);
    g_setenv("AI_ACP_COMMAND", "agent --stdio", TRUE);
    argv = ai_acp_client_dup_argv(client, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(argv[1], ==, "--stdio");
    g_unsetenv("AI_ACP_COMMAND");

    /* With no command a turn fails before anything is started. */
    fixture_setup(&fx, PLAIN_TURN);
    ai_acp_client_set_command(fx.client, NULL);
    fixture_run(&fx);
    g_assert_nonnull(fx.error);
    g_assert_null(fixture_read(&fx, "argv.log"));
    fixture_teardown(&fx);
}

/* ================================================================
 * The protocol
 * ================================================================ */

static void
test_turn(void)
{
    Fixture fx;
    g_autofree gchar *eof = NULL;
    g_autoptr(JsonParser) p0 = json_parser_new();
    g_autoptr(JsonParser) p1 = json_parser_new();
    g_autoptr(JsonParser) p2 = json_parser_new();
    g_autofree gchar *text = NULL;
    g_autofree gchar *argv = NULL;
    g_autofree gchar *cwd = NULL;
    JsonObject *request;
    JsonObject *params;
    JsonObject *caps;
    JsonArray *prompt;
    AiUsage *usage;

    fixture_setup(&fx, PLAIN_TURN);
    fixture_run(&fx);

    /* The answer: text split at the tool call, as two blocks. */
    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "Hello\nDone.");
    g_assert_cmpuint(g_list_length(ai_response_get_content_blocks(fx.response)), ==, 2);
    g_assert_cmpint(ai_response_get_stop_reason(fx.response), ==, AI_STOP_REASON_END_TURN);
    usage = ai_response_get_usage(fx.response);
    g_assert_nonnull(usage);
    g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 5);
    g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 7);
    g_assert_cmpint(ai_response_get_cost_micros(fx.response), ==, 250000);

    /* The narration. */
    g_assert_cmpuint(fx.texts->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(fx.texts, 0), ==, "Hel");
    g_assert_cmpstr(fx.thinking->str, ==, "Thinking");
    g_assert_cmpuint(fx.tools_started, ==, 1);
    g_assert_cmpuint(fx.tools_finished, ==, 1);
    g_assert_cmpstr(fx.tool_name, ==, "acp_execute");
    g_assert_cmpstr(fx.tool_output, ==, "a.txt");
    g_assert_false(fx.tool_failed);
    g_assert_true(fixture_said(&fx, "[x] look"));
    g_assert_true(fixture_said(&fx, "[>] answer"));

    /* The command, unquoted back into its words, in the working directory. */
    argv = fixture_read(&fx, "argv.log");
    g_assert_cmpstr(argv, ==, "--acp two words\n");
    cwd = fixture_read(&fx, "cwd.log");
    g_assert_true(g_str_has_prefix(cwd, fx.work));

    /* initialize */
    request = fixture_request(&fx, 0, p0);
    g_assert_cmpstr(json_object_get_string_member(request, "jsonrpc"), ==, "2.0");
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "initialize");
    g_assert_cmpint(json_object_get_int_member(request, "id"), ==, 1);
    params = json_object_get_object_member(request, "params");
    g_assert_cmpint(json_object_get_int_member(params, "protocolVersion"), ==, 1);
    caps = json_object_get_object_member(params, "clientCapabilities");
    g_assert_true(json_object_get_boolean_member(
        json_object_get_object_member(caps, "fs"), "readTextFile"));
    g_assert_false(json_object_get_boolean_member(
        json_object_get_object_member(caps, "fs"), "writeTextFile"));
    g_assert_false(json_object_get_boolean_member(caps, "terminal"));
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(params, "clientInfo"), "name"), ==, "ai-glib");

    /* session/new */
    request = fixture_request(&fx, 1, p1);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "session/new");
    params = json_object_get_object_member(request, "params");
    g_assert_cmpstr(json_object_get_string_member(params, "cwd"), ==, fx.work);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(params, "mcpServers")), ==, 0);

    /* session/prompt */
    request = fixture_request(&fx, 2, p2);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "session/prompt");
    g_assert_cmpint(json_object_get_int_member(request, "id"), ==, 3);
    params = json_object_get_object_member(request, "params");
    g_assert_cmpstr(json_object_get_string_member(params, "sessionId"), ==, "sess-1");
    prompt = json_object_get_array_member(params, "prompt");
    g_assert_cmpuint(json_array_get_length(prompt), ==, 1);
    g_assert_nonnull(strstr(json_object_get_string_member(
        json_array_get_object_element(prompt, 0), "text"), "hi"));

    /* The client closed stdin; the agent exited on its own. */
    g_assert_cmpuint(fixture_request_count(&fx), ==, 3);
    eof = fixture_read(&fx, "eof");
    g_assert_nonnull(eof);
    fixture_teardown(&fx);
}

/* The synchronous call runs the same protocol on a private context. */
static void
test_sync(void)
{
    Fixture fx;
    g_autoptr(AiMessage) message = ai_message_new_user("hi");
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;
    g_autofree gchar *text = NULL;
    GList *messages;

    fixture_setup(&fx, PLAIN_TURN);
    messages = g_list_append(NULL, message);
    response = ai_cli_client_chat_sync(AI_CLI_CLIENT(fx.client), messages, NULL, &error);
    g_list_free(messages);
    g_assert_no_error(error);
    text = ai_response_get_text(response);
    g_assert_cmpstr(text, ==, "Hello\nDone.");
    fixture_teardown(&fx);
}

/* The system prompt travels in the prompt text, ahead of the conversation. */
static void
test_system_prompt(void)
{
    Fixture fx;
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *params;
    const gchar *text;

    fixture_setup(&fx, PLAIN_TURN);
    fixture_run_messages(&fx, NULL, "Be terse.");
    g_assert_no_error(fx.error);
    params = json_object_get_object_member(fixture_request(&fx, 2, parser), "params");
    text = json_object_get_string_member(json_array_get_object_element(
        json_object_get_array_member(params, "prompt"), 0), "text");
    g_assert_true(g_str_has_prefix(text, "<system>\nBe terse.\n</system>"));
    fixture_teardown(&fx);
}

/* Updates before the prompt (a load's replay) and for another session are not this turn. */
static void
test_foreign_updates(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx,
        INIT "read\n"
        UPDATE("{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'BEFORE'}}")
        "send {'jsonrpc':'2.0','id':2,'result':{'sessionId':'sess-1'}}\n"
        "read\n"
        "send {'jsonrpc':'2.0','method':'session/update','params':{'sessionId':'other','update':{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'OTHER'}}}}\n"
        "send {'jsonrpc':'2.0','method':'_vendor/notice','params':{}}\n"
        UPDATE("{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'mine'}}")
        END_TURN("end_turn"));
    fixture_run(&fx);
    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "mine");
    fixture_teardown(&fx);
}

/* ================================================================
 * Permissions
 * ================================================================ */

#define PERMISSION \
    "send {'jsonrpc':'2.0','id':'perm-1','method':'session/request_permission','params':{'sessionId':'sess-1','toolCall':{'toolCallId':'call-1','title':'Write notes.txt'},'options':[{'optionId':'always','name':'Always','kind':'allow_always'},{'optionId':'no','name':'Reject','kind':'reject_once'},{'optionId':'yes','name':'Allow','kind':'allow_once'},{'optionId':'never','name':'Never','kind':'reject_always'}]}}\n" \
    "read\n"

static void
check_permission(gboolean skip, const gchar *expected)
{
    Fixture fx;
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *answer;
    JsonObject *outcome;

    fixture_setup(&fx, HANDSHAKE PERMISSION ANSWER END_TURN("end_turn"));
    ai_acp_client_set_skip_permissions(fx.client, skip);
    fixture_run(&fx);
    g_assert_no_error(fx.error);

    /* Chosen by kind, once before always, whatever the order offered. */
    answer = fixture_request(&fx, 3, parser);
    g_assert_cmpstr(json_object_get_string_member(answer, "id"), ==, "perm-1");
    outcome = json_object_get_object_member(json_object_get_object_member(answer, "result"),
                                            "outcome");
    g_assert_cmpstr(json_object_get_string_member(outcome, "outcome"), ==, "selected");
    g_assert_cmpstr(json_object_get_string_member(outcome, "optionId"), ==, expected);
    g_assert_true(fixture_said(&fx, "Write notes.txt"));
    fixture_teardown(&fx);
}

static void
test_permission_reject(void)
{
    check_permission(FALSE, "no");
}

static void
test_permission_allow(void)
{
    check_permission(TRUE, "yes");
}

/* No option of the wanted kind: "cancelled", never a guess. */
static void
test_permission_no_option(void)
{
    Fixture fx;
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *outcome;

    fixture_setup(&fx, HANDSHAKE
        "send {'jsonrpc':'2.0','id':9,'method':'session/request_permission','params':{'sessionId':'sess-1','toolCall':{'toolCallId':'c'},'options':[{'optionId':'ok','name':'OK','kind':'allow_once'}]}}\n"
        "read\n"
        END_TURN("end_turn"));
    fixture_run(&fx);
    g_assert_no_error(fx.error);
    outcome = json_object_get_object_member(json_object_get_object_member(
        fixture_request(&fx, 3, parser), "result"), "outcome");
    g_assert_cmpstr(json_object_get_string_member(outcome, "outcome"), ==, "cancelled");
    g_assert_false(json_object_has_member(outcome, "optionId"));
    fixture_teardown(&fx);
}

/* Terminals were never offered; an unknown request is still answered. */
static void
test_unserved_requests(void)
{
    Fixture fx;
    g_autoptr(JsonParser) p3 = json_parser_new();
    g_autoptr(JsonParser) p4 = json_parser_new();

    fixture_setup(&fx, HANDSHAKE
        "send {'jsonrpc':'2.0','id':20,'method':'terminal/create','params':{'sessionId':'sess-1','command':'ls'}}\n"
        "read\n"
        "send {'jsonrpc':'2.0','id':21,'method':'_vendor/thing','params':{}}\n"
        "read\n"
        END_TURN("end_turn"));
    fixture_run(&fx);
    g_assert_no_error(fx.error);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
        fixture_request(&fx, 3, p3), "error"), "code"), ==, -32601);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
        fixture_request(&fx, 4, p4), "error"), "code"), ==, -32601);
    fixture_teardown(&fx);
}

/* ================================================================
 * Cancellation and shutdown
 * ================================================================ */

/* Cancel mid-turn: session/cancel, the agent answers "cancelled", exits cleanly. */
static void
test_cancel(void)
{
    Fixture fx;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *eof = NULL;
    JsonObject *cancel;

    fixture_setup(&fx, HANDSHAKE
        UPDATE("{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'Hel'}}")
        "read\n"
        "send {'jsonrpc':'2.0','id':3,'result':{'stopReason':'cancelled'}}\n"
        "eof\n");
    fx.cancellable = g_cancellable_new();
    fx.cancel_on_text = TRUE;
    fixture_run(&fx);
    g_assert_error(fx.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

    cancel = fixture_request(&fx, 3, parser);
    g_assert_cmpstr(json_object_get_string_member(cancel, "method"), ==, "session/cancel");
    g_assert_false(json_object_has_member(cancel, "id"));
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(cancel, "params"), "sessionId"), ==, "sess-1");
    eof = fixture_read(&fx, "eof");
    g_assert_nonnull(eof);
    fixture_teardown(&fx);
}

/* An agent that ignores session/cancel is killed after the grace. */
static void
test_cancel_ignored(void)
{
    Fixture fx;
    gint64 started;

    fixture_setup(&fx, HANDSHAKE
        UPDATE("{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'Hel'}}")
        "read\n"
        "sleep 30\n");
    ai_acp_client_set_shutdown_timeout_ms(fx.client, 200);
    fx.cancellable = g_cancellable_new();
    fx.cancel_on_text = TRUE;
    started = g_get_monotonic_time();
    fixture_run(&fx);
    g_assert_error(fx.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
    fixture_teardown(&fx);
}

static gboolean
cancel_later(gpointer user_data)
{
    g_cancellable_cancel(G_CANCELLABLE(user_data));
    return G_SOURCE_REMOVE;
}

/* Cancelled before the prompt went out: nothing to cancel, killed at once. */
static void
test_cancel_before_prompt(void)
{
    Fixture fx;
    gint64 started;

    fixture_setup(&fx, "read\nsleep 30\n");
    fx.cancellable = g_cancellable_new();
    started = g_get_monotonic_time();
    g_timeout_add(100, cancel_later, fx.cancellable);
    fixture_run(&fx);
    g_assert_error(fx.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpint(g_get_monotonic_time() - started, <, 10 * G_USEC_PER_SEC);
    g_assert_cmpuint(fixture_request_count(&fx), <=, 1);
    fixture_teardown(&fx);
}

/* An agent that answers and then lingers keeps its answer; it is killed. */
static void
test_linger_after_answer(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, HANDSHAKE ANSWER
        "send {'jsonrpc':'2.0','id':3,'result':{'stopReason':'end_turn'}}\n"
        "sleep 30\n");
    ai_acp_client_set_shutdown_timeout_ms(fx.client, 200);
    fixture_run(&fx);
    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "Hello\nDone.");
    fixture_teardown(&fx);
}

/* ================================================================
 * Stop reasons and errors
 * ================================================================ */

static void
test_stop_reasons(void)
{
    static const struct
    {
        const gchar   *reason;
        AiStopReason   expected;
    } cases[] = {
        { "max_tokens", AI_STOP_REASON_MAX_TOKENS },
        { "max_turn_requests", AI_STOP_REASON_MAX_TOKENS },
        { "refusal", AI_STOP_REASON_CONTENT_FILTER },
        { "something_new", AI_STOP_REASON_END_TURN },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++)
    {
        Fixture fx;
        g_autofree gchar *script = g_strdup_printf("%s%s"
            "send {'jsonrpc':'2.0','id':3,'result':{'stopReason':'%s'}}\neof\n",
            HANDSHAKE, ANSWER, cases[i].reason);

        fixture_setup(&fx, script);
        fixture_run(&fx);
        g_assert_no_error(fx.error);
        g_assert_cmpint(ai_response_get_stop_reason(fx.response), ==, cases[i].expected);
        fixture_teardown(&fx);
    }
}

/* The agent cancelling on its own is an error, not the caller's cancel. */
static void
test_agent_cancelled(void)
{
    Fixture fx;

    fixture_setup(&fx, HANDSHAKE END_TURN("cancelled"));
    fixture_run(&fx);
    g_assert_error(fx.error, AI_ERROR, AI_ERROR_CANCELLED);
    fixture_teardown(&fx);
}

/* Failures go through the shared limit classification. */
static void
test_errors(void)
{
    static const struct
    {
        const gchar *script;
        gint         code;
    } cases[] = {
        /* A 429 in data under a generic message. */
        { HANDSHAKE "send {'jsonrpc':'2.0','id':3,'error':{'code':-32603,'message':'Internal error','data':{'message':'429 Too Many Requests'}}}\neof\n",
          AI_ERROR_RATE_LIMITED },
        /* A usage limit. */
        { HANDSHAKE "send {'jsonrpc':'2.0','id':3,'error':{'code':-32603,'message':'You have reached your usage limit. Try again in 2h.'}}\neof\n",
          AI_ERROR_SESSION_LIMIT },
        /* ACP's auth_required, whatever its words. */
        { INIT "read\nsend {'jsonrpc':'2.0','id':2,'error':{'code':-32000,'message':'Please run /login'}}\neof\n",
          AI_ERROR_INVALID_API_KEY },
        /* A plain refusal stays a CLI failure. */
        { HANDSHAKE "send {'jsonrpc':'2.0','id':3,'error':{'code':-32602,'message':'bad prompt'}}\neof\n",
          AI_ERROR_CLI_EXECUTION },
        /* Another protocol version. */
        { "read\nsend {'jsonrpc':'2.0','id':1,'result':{'protocolVersion':2}}\neof\n",
          AI_ERROR_CLI_EXECUTION },
        /* No sessionId. */
        { INIT "read\nsend {'jsonrpc':'2.0','id':2,'result':{}}\neof\n",
          AI_ERROR_CLI_PARSE_ERROR },
        /* Not JSON at all. */
        { "read\nsend this is not json\neof\n",
          AI_ERROR_CLI_PARSE_ERROR },
        /* No stopReason. */
        { HANDSHAKE "send {'jsonrpc':'2.0','id':3,'result':{}}\neof\n",
          AI_ERROR_CLI_PARSE_ERROR },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++)
    {
        Fixture fx;

        fixture_setup(&fx, cases[i].script);
        fixture_run(&fx);
        g_assert_error(fx.error, AI_ERROR, cases[i].code);
        fixture_teardown(&fx);
    }
}

/* An agent that dies before answering says why on stderr. */
static void
test_exit_before_answer(void)
{
    Fixture fx;

    fixture_setup(&fx, HANDSHAKE "stderr agent crashed: out of cheese\nexit 1\n");
    fixture_run(&fx);
    g_assert_nonnull(fx.error);
    g_assert_nonnull(strstr(fx.error->message, "out of cheese"));
    fixture_teardown(&fx);

    /* Exit 0 with no answer is still a failure. */
    fixture_setup(&fx, HANDSHAKE "exit 0\n");
    fixture_run(&fx);
    g_assert_error(fx.error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
    fixture_teardown(&fx);
}

/* ================================================================
 * fs/
 * ================================================================ */

static void
test_fs_read(void)
{
    Fixture fx;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *file = NULL;
    g_autofree gchar *link = NULL;
    g_autofree gchar *script = NULL;
    g_autoptr(JsonParser) p3 = json_parser_new();
    g_autoptr(JsonParser) p4 = json_parser_new();
    g_autoptr(JsonParser) p5 = json_parser_new();
    g_autoptr(JsonParser) p6 = json_parser_new();
    g_autoptr(JsonParser) p7 = json_parser_new();
    g_autofree gchar *secret = NULL;
    JsonObject *answer;

    fixture_setup(&fx, NULL);
    file = g_build_filename(fx.work, "a.txt", NULL);
    g_file_set_contents(file, "one\ntwo\nthree\n", -1, &error);
    g_assert_no_error(error);
    secret = g_build_filename(fx.dir, "secret.txt", NULL);
    g_file_set_contents(secret, "SECRET", -1, &error);
    g_assert_no_error(error);
    link = g_build_filename(fx.work, "escape.txt", NULL);
    g_assert_cmpint(symlink(secret, link), ==, 0);

    script = g_strdup_printf(HANDSHAKE
        "send {'jsonrpc':'2.0','id':30,'method':'fs/read_text_file','params':{'sessionId':'sess-1','path':'%s'}}\nread\n"
        "send {'jsonrpc':'2.0','id':31,'method':'fs/read_text_file','params':{'sessionId':'sess-1','path':'%s','line':2,'limit':1}}\nread\n"
        "send {'jsonrpc':'2.0','id':32,'method':'fs/read_text_file','params':{'sessionId':'sess-1','path':'%s/../secret.txt'}}\nread\n"
        "send {'jsonrpc':'2.0','id':33,'method':'fs/read_text_file','params':{'sessionId':'sess-1','path':'%s'}}\nread\n"
        "send {'jsonrpc':'2.0','id':34,'method':'fs/write_text_file','params':{'sessionId':'sess-1','path':'%s','content':'x'}}\nread\n"
        END_TURN("end_turn"),
        file, file, fx.work, link, file);
    fixture_write_script(&fx, script);
    fixture_run(&fx);
    g_assert_no_error(fx.error);

    /* Whole file, then line 2 alone. */
    answer = json_object_get_object_member(fixture_request(&fx, 3, p3), "result");
    g_assert_cmpstr(json_object_get_string_member(answer, "content"), ==, "one\ntwo\nthree\n");
    answer = json_object_get_object_member(fixture_request(&fx, 4, p4), "result");
    g_assert_cmpstr(json_object_get_string_member(answer, "content"), ==, "two\n");
    /* Outside the working directory, by .. or by a link: refused. */
    g_assert_true(json_object_has_member(fixture_request(&fx, 5, p5), "error"));
    g_assert_true(json_object_has_member(fixture_request(&fx, 6, p6), "error"));
    /* Writes are not offered without skip-permissions. */
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
        fixture_request(&fx, 7, p7), "error"), "code"), ==, -32601);
    fixture_teardown(&fx);
}

static void
test_fs_write(void)
{
    Fixture fx;
    g_autofree gchar *target = NULL;
    g_autofree gchar *outside = NULL;
    g_autofree gchar *script = NULL;
    g_autofree gchar *written = NULL;
    g_autoptr(JsonParser) p0 = json_parser_new();
    g_autoptr(JsonParser) p3 = json_parser_new();
    g_autoptr(JsonParser) p4 = json_parser_new();
    JsonObject *caps;

    fixture_setup(&fx, NULL);
    ai_acp_client_set_skip_permissions(fx.client, TRUE);
    target = g_build_filename(fx.work, "sub", "new.txt", NULL);
    outside = g_build_filename(fx.dir, "outside.txt", NULL);
    script = g_strdup_printf(HANDSHAKE
        "send {'jsonrpc':'2.0','id':40,'method':'fs/write_text_file','params':{'sessionId':'sess-1','path':'%s','content':'hello'}}\nread\n"
        "send {'jsonrpc':'2.0','id':41,'method':'fs/write_text_file','params':{'sessionId':'sess-1','path':'%s','content':'no'}}\nread\n"
        END_TURN("end_turn"),
        target, outside);
    fixture_write_script(&fx, script);
    fixture_run(&fx);
    g_assert_no_error(fx.error);

    caps = json_object_get_object_member(json_object_get_object_member(
        fixture_request(&fx, 0, p0), "params"), "clientCapabilities");
    g_assert_true(json_object_get_boolean_member(
        json_object_get_object_member(caps, "fs"), "writeTextFile"));
    g_assert_true(json_object_has_member(fixture_request(&fx, 3, p3), "result"));
    g_assert_true(g_file_get_contents(target, &written, NULL, NULL));
    g_assert_cmpstr(written, ==, "hello");
    g_assert_true(json_object_has_member(fixture_request(&fx, 4, p4), "error"));
    g_assert_false(g_file_test(outside, G_FILE_TEST_EXISTS));
    fixture_teardown(&fx);
}

/* client-fs off: nothing is offered and nothing is served. */
static void
test_fs_off(void)
{
    Fixture fx;
    g_autoptr(JsonParser) p0 = json_parser_new();
    g_autoptr(JsonParser) p3 = json_parser_new();
    JsonObject *caps;

    fixture_setup(&fx, HANDSHAKE
        "send {'jsonrpc':'2.0','id':30,'method':'fs/read_text_file','params':{'sessionId':'sess-1','path':'/etc/hostname'}}\nread\n"
        END_TURN("end_turn"));
    ai_acp_client_set_client_fs(fx.client, FALSE);
    fixture_run(&fx);
    g_assert_no_error(fx.error);
    caps = json_object_get_object_member(json_object_get_object_member(
        fixture_request(&fx, 0, p0), "params"), "clientCapabilities");
    g_assert_false(json_object_get_boolean_member(
        json_object_get_object_member(caps, "fs"), "readTextFile"));
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(
        fixture_request(&fx, 3, p3), "error"), "code"), ==, -32601);
    fixture_teardown(&fx);
}

/* ================================================================
 * MCP passthrough
 * ================================================================ */

static void
test_mcp_servers(void)
{
    Fixture fx;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(AiAgentEndpoint) endpoint = NULL;
    g_autofree gchar *config = NULL;
    JsonArray *servers;
    JsonObject *server;
    JsonObject *env;

    fixture_setup(&fx, PLAIN_TURN);
    config = g_build_filename(fx.dir, "mcp.json", NULL);
    g_file_set_contents(config,
        "{\"mcpServers\":{"
        "\"tools\":{\"command\":\"/bin/sh\",\"args\":[\"-c\",\"exit 0\"],\"env\":{\"TOKEN\":\"t0k\"}},"
        "\"web\":{\"type\":\"http\",\"url\":\"https://example.invalid/mcp\"},"
        "\"ghost\":{\"command\":\"no-such-mcp-server-anywhere\"}}}", -1, &error);
    g_assert_no_error(error);
    endpoint = ai_agent_endpoint_new(AI_ENDPOINT_KIND_MCP_CONFIG, config);
    g_assert_true(ai_tool_endpoint_consumer_apply(AI_TOOL_ENDPOINT_CONSUMER(fx.client),
                                                  endpoint, &error));
    g_assert_no_error(error);
    fixture_run(&fx);
    g_assert_no_error(fx.error);

    /* stdio carried with its env as {name, value}; http left out (not advertised). */
    servers = json_object_get_array_member(json_object_get_object_member(
        fixture_request(&fx, 1, parser), "params"), "mcpServers");
    g_assert_cmpuint(json_array_get_length(servers), ==, 1);
    server = json_array_get_object_element(servers, 0);
    g_assert_cmpstr(json_object_get_string_member(server, "name"), ==, "tools");
    g_assert_cmpstr(json_object_get_string_member(server, "command"), ==, "/bin/sh");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(server, "args")), ==, 2);
    env = json_array_get_object_element(json_object_get_array_member(server, "env"), 0);
    g_assert_cmpstr(json_object_get_string_member(env, "name"), ==, "TOKEN");
    g_assert_cmpstr(json_object_get_string_member(env, "value"), ==, "t0k");
    g_assert_true(fixture_said(&fx, "web left out"));
    g_assert_true(fixture_said(&fx, "ghost left out"));

    /* Revoked: nothing goes over. */
    g_assert_true(ai_tool_endpoint_consumer_clear(AI_TOOL_ENDPOINT_CONSUMER(fx.client), &error));
    g_assert_no_error(error);
    fixture_teardown(&fx);
}

/* ================================================================
 * Sessions
 * ================================================================ */

static void
test_session_persisted(void)
{
    Fixture fx;

    fixture_setup(&fx, PLAIN_TURN);
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx.client), TRUE);
    fixture_run(&fx);
    g_assert_no_error(fx.error);
    g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(fx.client)), ==, "sess-1");
    fixture_teardown(&fx);
}

/* session/load: the replay is not this turn, and only the newest message goes. */
static void
test_session_load(void)
{
    Fixture fx;
    g_autoptr(JsonParser) p1 = json_parser_new();
    g_autoptr(JsonParser) p2 = json_parser_new();
    g_autoptr(AiMessage) first = ai_message_new_user("FIRST QUESTION");
    g_autoptr(AiMessage) reply = ai_message_new_assistant("FIRST ANSWER");
    g_autoptr(AiMessage) second = ai_message_new_user("SECOND QUESTION");
    g_autofree gchar *text = NULL;
    GList *messages = NULL;
    JsonObject *request;
    const gchar *prompt;

    fixture_setup(&fx, INIT_CAPS("true", "false")
        "read\n"
        "send {'jsonrpc':'2.0','method':'session/update','params':{'sessionId':'old-1','update':{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'REPLAYED'}}}}\n"
        "send {'jsonrpc':'2.0','id':2,'result':{}}\n"
        "read\n"
        "send {'jsonrpc':'2.0','method':'session/update','params':{'sessionId':'old-1','update':{'sessionUpdate':'agent_message_chunk','content':{'type':'text','text':'fresh'}}}}\n"
        "send {'jsonrpc':'2.0','id':3,'result':{'stopReason':'end_turn'}}\n"
        "eof\n");
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx.client), TRUE);
    ai_cli_client_set_session_id(AI_CLI_CLIENT(fx.client), "old-1");
    messages = g_list_append(messages, first);
    messages = g_list_append(messages, reply);
    messages = g_list_append(messages, second);
    fixture_run_messages(&fx, messages, "SYSTEM");
    g_list_free(messages);

    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "fresh");
    request = fixture_request(&fx, 1, p1);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "session/load");
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(request, "params"), "sessionId"), ==, "old-1");
    request = fixture_request(&fx, 2, p2);
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(request, "params"), "sessionId"), ==, "old-1");
    prompt = json_object_get_string_member(json_array_get_object_element(
        json_object_get_array_member(json_object_get_object_member(request, "params"),
                                     "prompt"), 0), "text");
    g_assert_nonnull(strstr(prompt, "SECOND QUESTION"));
    g_assert_null(strstr(prompt, "FIRST QUESTION"));
    g_assert_null(strstr(prompt, "SYSTEM"));
    fixture_teardown(&fx);
}

/* A refused load starts a new session with the whole conversation. */
static void
test_session_load_fallback(void)
{
    Fixture fx;
    g_autoptr(JsonParser) p2 = json_parser_new();
    g_autoptr(JsonParser) p3 = json_parser_new();
    JsonObject *request;

    fixture_setup(&fx, INIT_CAPS("true", "false")
        "read\n"
        "send {'jsonrpc':'2.0','id':2,'error':{'code':-32002,'message':'Resource not found'}}\n"
        "read\n"
        "send {'jsonrpc':'2.0','id':4,'result':{'sessionId':'sess-2'}}\n"
        "read\n"
        "send {'jsonrpc':'2.0','id':3,'result':{'stopReason':'end_turn'}}\n"
        "eof\n");
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx.client), TRUE);
    ai_cli_client_set_session_id(AI_CLI_CLIENT(fx.client), "gone");
    fixture_run(&fx);
    g_assert_no_error(fx.error);
    request = fixture_request(&fx, 2, p2);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "session/new");
    g_assert_cmpint(json_object_get_int_member(request, "id"), ==, 4);
    request = fixture_request(&fx, 3, p3);
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(request, "params"), "sessionId"), ==, "sess-2");
    g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(fx.client)), ==, "sess-2");
    g_assert_true(fixture_said(&fx, "could not load session gone"));
    fixture_teardown(&fx);
}

/* An agent without loadSession gets a new session and the whole conversation. */
static void
test_session_no_load(void)
{
    Fixture fx;
    g_autoptr(JsonParser) p1 = json_parser_new();

    fixture_setup(&fx, PLAIN_TURN);
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx.client), TRUE);
    ai_cli_client_set_session_id(AI_CLI_CLIENT(fx.client), "old-1");
    fixture_run(&fx);
    g_assert_no_error(fx.error);
    g_assert_cmpstr(json_object_get_string_member(fixture_request(&fx, 1, p1), "method"),
                    ==, "session/new");
    g_assert_true(fixture_said(&fx, "cannot load sessions"));
    fixture_teardown(&fx);
}

/* ================================================================
 * Images
 * ================================================================ */

static void
check_image(gboolean accepted)
{
    Fixture fx;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(AiMessage) message = ai_message_new_user("look");
    g_autoptr(GBytes) bytes = g_bytes_new_static("PNGDATA", 7);
    g_autoptr(AiImageContent) image = ai_image_content_new_from_bytes(bytes, "image/png");
    g_autofree gchar *script = g_strdup_printf("%s%s%s",
        accepted ? INIT_CAPS("false", "true") : INIT, SESSION "read\n", END_TURN("end_turn"));
    GList *messages;
    JsonArray *prompt;

    ai_message_add_content_block(message, AI_CONTENT_BLOCK(g_steal_pointer(&image)));
    fixture_setup(&fx, script);
    messages = g_list_append(NULL, message);
    fixture_run_messages(&fx, messages, NULL);
    g_list_free(messages);
    g_assert_no_error(fx.error);

    prompt = json_object_get_array_member(json_object_get_object_member(
        fixture_request(&fx, 2, parser), "params"), "prompt");
    if (accepted)
    {
        JsonObject *block;

        g_assert_cmpuint(json_array_get_length(prompt), ==, 2);
        block = json_array_get_object_element(prompt, 1);
        g_assert_cmpstr(json_object_get_string_member(block, "type"), ==, "image");
        g_assert_cmpstr(json_object_get_string_member(block, "mimeType"), ==, "image/png");
        g_assert_cmpstr(json_object_get_string_member(block, "data"), ==, "UE5HREFUQQ==");
    }
    else
    {
        g_assert_cmpuint(json_array_get_length(prompt), ==, 1);
        g_assert_true(fixture_said(&fx, "image(s) left out"));
    }
    fixture_teardown(&fx);
}

static void
test_image_accepted(void)
{
    check_image(TRUE);
}

static void
test_image_refused(void)
{
    check_image(FALSE);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/acp/registration", test_registration);
    g_test_add_func("/acp/command", test_command);
    g_test_add_func("/acp/turn", test_turn);
    g_test_add_func("/acp/sync", test_sync);
    g_test_add_func("/acp/system-prompt", test_system_prompt);
    g_test_add_func("/acp/foreign-updates", test_foreign_updates);
    g_test_add_func("/acp/permission/reject", test_permission_reject);
    g_test_add_func("/acp/permission/allow", test_permission_allow);
    g_test_add_func("/acp/permission/no-option", test_permission_no_option);
    g_test_add_func("/acp/unserved-requests", test_unserved_requests);
    g_test_add_func("/acp/cancel", test_cancel);
    g_test_add_func("/acp/cancel/ignored", test_cancel_ignored);
    g_test_add_func("/acp/cancel/before-prompt", test_cancel_before_prompt);
    g_test_add_func("/acp/linger-after-answer", test_linger_after_answer);
    g_test_add_func("/acp/stop-reasons", test_stop_reasons);
    g_test_add_func("/acp/agent-cancelled", test_agent_cancelled);
    g_test_add_func("/acp/errors", test_errors);
    g_test_add_func("/acp/exit-before-answer", test_exit_before_answer);
    g_test_add_func("/acp/fs/read", test_fs_read);
    g_test_add_func("/acp/fs/write", test_fs_write);
    g_test_add_func("/acp/fs/off", test_fs_off);
    g_test_add_func("/acp/mcp", test_mcp_servers);
    g_test_add_func("/acp/session/persisted", test_session_persisted);
    g_test_add_func("/acp/session/load", test_session_load);
    g_test_add_func("/acp/session/load-fallback", test_session_load_fallback);
    g_test_add_func("/acp/session/no-load", test_session_no_load);
    g_test_add_func("/acp/image/accepted", test_image_accepted);
    g_test_add_func("/acp/image/refused", test_image_refused);

    return g_test_run();
}
