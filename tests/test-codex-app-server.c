/*
 * test-codex-app-server.c - AiCodexCliClient over `codex app-server`
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * No codex is installed where this was written, so the transport is
 * driven against tests/fixtures/fake-codex-app-server.bash: a scripted
 * stand-in that records every line the client writes and answers
 * exactly what each test tells it to, in the shapes of the published
 * protocol (openai/codex rust-v0.169.0, codex-rs/app-server-protocol).
 * The client's request ids are deterministic -- initialize 1, the thread
 * request 2, turn/start 3, steers from 4 -- which is what lets a static
 * script answer them.
 *
 * Covered: the handshake and its requests, resume, deltas and usage,
 * approvals answered as exec's flags would, failures classified as
 * exec's are, the synchronous path, the fail-closed settings, and
 * turn/steer's tri-state: a result is DELIVERED, a refusal is REFUSED
 * with the reason, an internal error or silence past the timeout is
 * INDETERMINATE, and a steer before or after the turn never leaves.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "ai-glib.h"
#include "core/ai-steerable.h"

/* ================================================================
 * Script fragments
 *
 * Written with ' for " so they stay readable; fixture_write_script()
 * turns them back.  An apostrophe inside a JSON string is '.
 * ================================================================ */

#define HANDSHAKE \
    "read\n" \
    "send {'id':1,'result':{'userAgent':'fake/0','codexHome':'/tmp','platformFamily':'unix','platformOs':'linux'}}\n" \
    "read\n" \
    "read\n" \
    "send {'id':2,'result':{'thread':{'id':'thr-1','items':[]},'model':'gpt-6-astra','modelProvider':'openai','cwd':'/tmp','approvalPolicy':'never','approvalsReviewer':'user','sandbox':{'type':'readOnly'}}}\n" \
    "read\n" \
    "send {'id':3,'result':{'turn':{'id':'turn-1','items':[],'status':'inProgress'}}}\n" \
    "send {'method':'turn/started','params':{'threadId':'thr-1','turn':{'id':'turn-1','items':[],'status':'inProgress'}}}\n"

#define TOOL_STARTED \
    "send {'method':'item/started','params':{'threadId':'thr-1','turnId':'turn-1','startedAtMs':1,'item':{'type':'commandExecution','id':'cmd-1','command':'sleep 5','cwd':'/tmp','status':'inProgress','commandActions':[]}}}\n"

#define TOOL_COMPLETED \
    "send {'method':'item/completed','params':{'threadId':'thr-1','turnId':'turn-1','completedAtMs':2,'item':{'type':'commandExecution','id':'cmd-1','command':'sleep 5','cwd':'/tmp','status':'completed','aggregatedOutput':'slept','exitCode':0,'commandActions':[]}}}\n"

#define ANSWER \
    "send {'method':'item/agentMessage/delta','params':{'threadId':'thr-1','turnId':'turn-1','itemId':'msg-1','delta':'Hel'}}\n" \
    "send {'method':'item/agentMessage/delta','params':{'threadId':'thr-1','turnId':'turn-1','itemId':'msg-1','delta':'lo'}}\n" \
    "send {'method':'item/completed','params':{'threadId':'thr-1','turnId':'turn-1','completedAtMs':3,'item':{'type':'agentMessage','id':'msg-1','text':'Hello'}}}\n" \
    "send {'method':'thread/tokenUsage/updated','params':{'threadId':'thr-1','turnId':'turn-1','tokenUsage':{'last':{'inputTokens':5,'outputTokens':7,'cachedInputTokens':0,'reasoningOutputTokens':0,'totalTokens':12},'total':{'inputTokens':5,'outputTokens':7,'cachedInputTokens':0,'reasoningOutputTokens':0,'totalTokens':12}}}}\n"

#define COMPLETED \
    "send {'method':'turn/completed','params':{'threadId':'thr-1','turn':{'id':'turn-1','items':[],'status':'completed'}}}\n" \
    "eof\n"

/* The whole of a plain turn. */
#define PLAIN_TURN HANDSHAKE ANSWER COMPLETED

/* ================================================================
 * Fixture
 * ================================================================ */

typedef struct
{
    gboolean      done;
    AiSteerResult result;
    GError       *error;
} SteerOutcome;

typedef struct
{
    gchar            *dir;
    AiCodexCliClient *client;

    gboolean          stream_done;
    AiResponse       *response;
    GError           *stream_error;

    GPtrArray        *texts;       /* every TEXT_DELTA, in order */
    guint             tools_started;
    guint             tools_finished;
    guint             statuses;

    AiEventKind       steer_on;    /* steer on the first event of this kind */
    gboolean          steer_now;   /* or as soon as the run starts */
    const gchar      *steer_turn_id;
    gboolean          steered;
    SteerOutcome      outcome;
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
    g_autofree gchar *cli = NULL;
    gsize length = 0;

    memset(fx, 0, sizeof(*fx));
    fx->steer_on = (AiEventKind)-1;
    fx->texts = g_ptr_array_new_with_free_func(g_free);
    fx->dir = g_dir_make_tmp("ai-glib-codex-app-XXXXXX", &error);
    g_assert_no_error(error);

    /* A private, executable copy: nothing depends on the checkout's mode bits. */
    g_assert_true(g_file_get_contents("tests/fixtures/fake-codex-app-server.bash",
                                      &fake, &length, &error));
    g_assert_no_error(error);
    cli = g_build_filename(fx->dir, "codex", NULL);
    g_file_set_contents(cli, fake, (gssize)length, &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(cli, 0700), ==, 0);
    fixture_write_script(fx, script);

    fx->client = ai_codex_cli_client_new();
    ai_codex_cli_client_set_transport(fx->client, AI_CODEX_CLI_TRANSPORT_APP_SERVER);
    ai_cli_client_set_executable_path(AI_CLI_CLIENT(fx->client), cli);
    ai_cli_client_set_env(AI_CLI_CLIENT(fx->client), "FAKE_CODEX_DIR", fx->dir);
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx->client), FALSE);
    /* A run that never closes stdin fails here instead of hanging. */
    ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(fx->client), 15000);
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

static void
assert_file_exists(Fixture *fx, const gchar *name)
{
    g_autofree gchar *contents = fixture_read(fx, name);

    g_assert_nonnull(contents);
}

static void
fixture_teardown(Fixture *fx)
{
    const gchar *names[] = { "codex", "script", "argv.log", "requests.log", "eof", NULL };
    guint i;

    for (i = 0; names[i] != NULL; i++)
    {
        g_autofree gchar *path = g_build_filename(fx->dir, names[i], NULL);

        g_unlink(path);
    }
    g_rmdir(fx->dir);
    g_free(fx->dir);
    g_clear_object(&fx->client);
    g_clear_object(&fx->response);
    g_clear_error(&fx->stream_error);
    g_clear_error(&fx->outcome.error);
    g_ptr_array_unref(fx->texts);
}

static void
on_steered(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SteerOutcome *outcome = user_data;

    outcome->result = ai_steerable_steer_finish(AI_STEERABLE(source), result,
                                                &outcome->error);
    outcome->done = TRUE;
}

static void
fixture_steer(Fixture *fx)
{
    g_autofree gchar *turn_id = ai_steerable_dup_turn_id(AI_STEERABLE(fx->client));

    fx->steered = TRUE;
    ai_steerable_steer_async(AI_STEERABLE(fx->client),
                             fx->steer_turn_id != NULL ? fx->steer_turn_id : turn_id,
                             "STEERED WORDS", NULL, on_steered, &fx->outcome);
}

static void
on_event(AiEventSource *source, AiEvent *event, gpointer user_data)
{
    Fixture *fx = user_data;
    AiEventKind kind = ai_event_get_kind(event);

    (void)source;

    if (kind == AI_EVENT_TEXT_DELTA)
        g_ptr_array_add(fx->texts, g_strdup(ai_event_get_text(event)));
    else if (kind == AI_EVENT_TOOL_STARTED)
        fx->tools_started++;
    else if (kind == AI_EVENT_TOOL_FINISHED)
        fx->tools_finished++;
    else if (kind == AI_EVENT_STATUS)
        fx->statuses++;

    if (!fx->steered && kind == fx->steer_on)
        fixture_steer(fx);
}

static void
on_stream(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Fixture *fx = user_data;

    fx->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source), result,
                                                    &fx->stream_error);
    fx->stream_done = TRUE;
}

/* One streamed turn; steers as the fixture says. */
static void
fixture_run(Fixture *fx)
{
    g_autoptr(AiMessage) message = ai_message_new_user("do the thing");
    GList *messages = g_list_append(NULL, message);
    gulong handler;

    handler = g_signal_connect(fx->client, "event", G_CALLBACK(on_event), fx);
    ai_streamable_chat_stream_async(AI_STREAMABLE(fx->client), messages, NULL, 0,
                                    NULL, NULL, on_stream, fx);
    if (fx->steer_now)
        fixture_steer(fx);
    iterate_until(&fx->stream_done);
    if (fx->steered)
        iterate_until(&fx->outcome.done);
    g_signal_handler_disconnect(fx->client, handler);
    g_list_free(messages);
}

static gchar *
response_text(Fixture *fx)
{
    g_assert_no_error(fx->stream_error);
    g_assert_nonnull(fx->response);
    return ai_response_get_text(fx->response);
}

/* ================================================================
 * Transport selection
 * ================================================================ */

/* exec stays the default, unchanged, and cannot steer. */
static void
test_exec_is_the_default(void)
{
    g_autoptr(AiCodexCliClient) codex = ai_codex_cli_client_new();
    g_autoptr(AiCodexCliClient) app = ai_codex_cli_client_new();
    g_autofree gchar *transport = NULL;
    SteerOutcome outcome = { 0 };

    g_assert_cmpstr(ai_codex_cli_client_get_transport(codex), ==, AI_CODEX_CLI_TRANSPORT_EXEC);
    g_object_get(codex, "transport", &transport, NULL);
    g_assert_cmpstr(transport, ==, "exec");
    g_assert_false(ai_steerable_can_steer(AI_STEERABLE(codex)));
    g_assert_null(ai_steerable_dup_turn_id(AI_STEERABLE(codex)));

    ai_steerable_steer_async(AI_STEERABLE(codex), NULL, "words", NULL, on_steered, &outcome);
    iterate_until(&outcome.done);
    g_assert_cmpint(outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NOT_SUPPORTED);
    g_clear_error(&outcome.error);

    /* The answer is per instance: the transport decides it. */
    g_object_set(app, "transport", "app-server", NULL);
    g_assert_true(ai_steerable_can_steer(AI_STEERABLE(app)));
    ai_codex_cli_client_set_transport(app, NULL);
    g_assert_cmpstr(ai_codex_cli_client_get_transport(app), ==, "exec");
    g_assert_false(ai_steerable_can_steer(AI_STEERABLE(app)));
}

/* Settings that cannot work fail before codex is started. */
static void
test_fails_closed(void)
{
    const gchar *cases[] = { "profile", "transport", "effort", "directory", NULL };
    guint i;

    for (i = 0; cases[i] != NULL; i++)
    {
        Fixture fx;
        g_autofree gchar *argv = NULL;

        fixture_setup(&fx, PLAIN_TURN);
        if (g_str_equal(cases[i], "profile"))
            g_object_set(fx.client, "profile", "work", NULL);
        else if (g_str_equal(cases[i], "transport"))
            g_object_set(fx.client, "transport", "websocket", NULL);
        else if (g_str_equal(cases[i], "effort"))
            ai_cli_client_set_effort_level(AI_CLI_CLIENT(fx.client), "extreme");
        else
            g_object_set(fx.client, "additional-directories", "/tmp/a\"b", NULL);
        fixture_run(&fx);
        g_assert_error(fx.stream_error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
        argv = fixture_read(&fx, "argv.log");
        g_assert_null(argv);
        fixture_teardown(&fx);
    }
}

/* ================================================================
 * The protocol
 * ================================================================ */

static void
test_handshake_and_turn(void)
{
    Fixture fx;
    g_autoptr(JsonParser) p0 = json_parser_new();
    g_autoptr(JsonParser) p1 = json_parser_new();
    g_autoptr(JsonParser) p2 = json_parser_new();
    g_autoptr(JsonParser) p3 = json_parser_new();
    g_autofree gchar *argv = NULL;
    g_autofree gchar *text = NULL;
    JsonObject *request;
    JsonObject *params;
    JsonArray *input;
    AiUsage *usage;

    fixture_setup(&fx, PLAIN_TURN);
    ai_cli_client_set_working_directory(AI_CLI_CLIENT(fx.client), "/tmp");
    ai_cli_client_set_effort_level(AI_CLI_CLIENT(fx.client), "high");
    fixture_run(&fx);

    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "Hello");
    /* Real deltas, and the completed item does not repeat them. */
    g_assert_cmpuint(fx.texts->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(fx.texts, 0), ==, "Hel");
    g_assert_cmpstr(g_ptr_array_index(fx.texts, 1), ==, "lo");
    usage = ai_response_get_usage(fx.response);
    g_assert_nonnull(usage);
    g_assert_cmpint(ai_usage_get_input_tokens(usage), ==, 5);
    g_assert_cmpint(ai_usage_get_output_tokens(usage), ==, 7);
    g_assert_cmpint(ai_response_get_stop_reason(fx.response), ==, AI_STOP_REASON_END_TURN);

    argv = fixture_read(&fx, "argv.log");
    g_assert_cmpstr(argv, ==, "app-server\n");

    g_assert_cmpuint(fixture_request_count(&fx), ==, 4);
    request = fixture_request(&fx, 0, p0);
    g_assert_cmpint(json_object_get_int_member(request, "id"), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "initialize");
    g_assert_false(json_object_has_member(request, "jsonrpc"));
    params = json_object_get_object_member(request, "params");
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(params, "clientInfo"), "name"), ==, "ai-glib");

    request = fixture_request(&fx, 1, p1);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "initialized");
    g_assert_false(json_object_has_member(request, "id"));

    request = fixture_request(&fx, 2, p2);
    g_assert_cmpint(json_object_get_int_member(request, "id"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "thread/start");
    params = json_object_get_object_member(request, "params");
    g_assert_cmpstr(json_object_get_string_member(params, "model"), ==, AI_CODEX_CLI_DEFAULT_MODEL);
    g_assert_cmpstr(json_object_get_string_member(params, "cwd"), ==, "/tmp");
    g_assert_cmpstr(json_object_get_string_member(params, "approvalPolicy"), ==, "never");
    g_assert_cmpstr(json_object_get_string_member(params, "sandbox"), ==, "read-only");
    g_assert_true(json_object_get_boolean_member(params, "ephemeral"));

    request = fixture_request(&fx, 3, p3);
    g_assert_cmpint(json_object_get_int_member(request, "id"), ==, 3);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "turn/start");
    params = json_object_get_object_member(request, "params");
    g_assert_cmpstr(json_object_get_string_member(params, "threadId"), ==, "thr-1");
    g_assert_cmpstr(json_object_get_string_member(params, "effort"), ==, "high");
    input = json_object_get_array_member(params, "input");
    g_assert_cmpuint(json_array_get_length(input), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(
        json_array_get_object_element(input, 0), "type"), ==, "text");
    g_assert_nonnull(strstr(json_object_get_string_member(
        json_array_get_object_element(input, 0), "text"), "do the thing"));

    /* turn/completed closed stdin, which is what ends app-server. */
    assert_file_exists(&fx, "eof");
    fixture_teardown(&fx);
}

/* A session id resumes that thread; the thread codex names is kept. */
static void
test_resume_thread(void)
{
    Fixture fx;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = NULL;
    JsonObject *params;

    fixture_setup(&fx, PLAIN_TURN);
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx.client), TRUE);
    ai_cli_client_set_session_id(AI_CLI_CLIENT(fx.client), "thr-old");
    fixture_run(&fx);
    text = response_text(&fx);

    g_assert_cmpstr(json_object_get_string_member(fixture_request(&fx, 2, parser), "method"), ==,
                    "thread/resume");
    params = json_object_get_object_member(fixture_request(&fx, 2, parser), "params");
    g_assert_cmpstr(json_object_get_string_member(params, "threadId"), ==, "thr-old");
    g_assert_false(json_object_has_member(params, "ephemeral"));
    g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(fx.client)), ==, "thr-1");
    fixture_teardown(&fx);
}

/* Global settings go in -c overrides ahead of the subcommand. */
static void
test_config_overrides(void)
{
    Fixture fx;
    g_autofree gchar *argv = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *expected = NULL;

    fixture_setup(&fx, PLAIN_TURN);
    g_object_set(fx.client, "search", TRUE,
                 "additional-directories", "/srv/one, rel", NULL);
    /* A relative root is taken from the working directory. */
    ai_cli_client_set_working_directory(AI_CLI_CLIENT(fx.client), fx.dir);
    fixture_run(&fx);
    text = response_text(&fx);
    argv = fixture_read(&fx, "argv.log");
    expected = g_strdup_printf("-c web_search=\"live\" -c sandbox_workspace_write.writable_roots="
                               "[\"/srv/one\",\"%s/rel\"] app-server\n", fx.dir);
    g_assert_cmpstr(argv, ==, expected);
    fixture_teardown(&fx);
}

/* Tool items keep exec's names, so the view styles them alike. */
static void
test_tool_items(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, HANDSHAKE TOOL_STARTED TOOL_COMPLETED ANSWER COMPLETED);
    fixture_run(&fx);
    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "Hello");
    g_assert_cmpuint(fx.tools_started, ==, 1);
    g_assert_cmpuint(fx.tools_finished, ==, 1);
    fixture_teardown(&fx);
}

/* The synchronous call runs the same protocol on a private context. */
static void
test_sync_path(void)
{
    Fixture fx;
    g_autoptr(AiMessage) message = ai_message_new_user("do the thing");
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *text = NULL;
    GList *messages = g_list_append(NULL, message);

    fixture_setup(&fx, PLAIN_TURN);
    response = ai_cli_client_chat_sync(AI_CLI_CLIENT(fx.client), messages, NULL, &error);
    g_list_free(messages);
    g_assert_no_error(error);
    g_assert_nonnull(response);
    text = ai_response_get_text(response);
    g_assert_cmpstr(text, ==, "Hello");
    assert_file_exists(&fx, "eof");
    fixture_teardown(&fx);
}

/* ================================================================
 * Approvals and other server requests
 * ================================================================ */

#define REQUESTS \
    "send {'id':'srv-1','method':'item/commandExecution/requestApproval','params':{'threadId':'thr-1','turnId':'turn-1','itemId':'cmd-1','command':'rm -rf /tmp/x'}}\n" \
    "read\n" \
    "send {'id':7,'method':'execCommandApproval','params':{}}\n" \
    "read\n" \
    "send {'id':8,'method':'item/permissions/requestApproval','params':{}}\n" \
    "read\n" \
    "send {'id':9,'method':'item/tool/requestUserInput','params':{}}\n" \
    "read\n"

static void
check_requests(gboolean skip_permissions)
{
    Fixture fx;
    g_autoptr(JsonParser) p2 = json_parser_new();
    g_autoptr(JsonParser) p4 = json_parser_new();
    g_autoptr(JsonParser) p5 = json_parser_new();
    g_autoptr(JsonParser) p6 = json_parser_new();
    g_autoptr(JsonParser) p7 = json_parser_new();
    g_autofree gchar *text = NULL;
    JsonObject *answer;

    fixture_setup(&fx, HANDSHAKE REQUESTS ANSWER COMPLETED);
    ai_codex_cli_client_set_skip_permissions(fx.client, skip_permissions);
    fixture_run(&fx);
    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "Hello");

    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(fixture_request(&fx, 2, p2), "params"), "sandbox"),
        ==, skip_permissions ? "danger-full-access" : "read-only");

    /* Answered under the id codex chose, string or number. */
    answer = fixture_request(&fx, 4, p4);
    g_assert_cmpstr(json_object_get_string_member(answer, "id"), ==, "srv-1");
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(answer, "result"), "decision"),
        ==, skip_permissions ? "accept" : "decline");

    answer = fixture_request(&fx, 5, p5);
    g_assert_cmpint(json_object_get_int_member(answer, "id"), ==, 7);
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(answer, "result"), "decision"),
        ==, skip_permissions ? "approved" : "denied");

    /* Never more permissions than the run was started with. */
    answer = fixture_request(&fx, 6, p6);
    g_assert_cmpint(json_object_get_size(json_object_get_object_member(
        json_object_get_object_member(answer, "result"), "permissions")), ==, 0);

    /* Nobody is there to answer a question: an error, not a hang. */
    answer = fixture_request(&fx, 7, p7);
    g_assert_cmpint(json_object_get_int_member(answer, "id"), ==, 9);
    g_assert_true(json_object_has_member(answer, "error"));
    g_assert_false(json_object_has_member(answer, "result"));
    g_assert_cmpuint(fx.statuses, >=, 3);
    fixture_teardown(&fx);
}

static void
test_approvals_declined(void)
{
    check_requests(FALSE);
}

static void
test_approvals_accepted_when_skipping(void)
{
    check_requests(TRUE);
}

/* ================================================================
 * Failures
 * ================================================================ */

#define FAILED_TURN(error_json) \
    HANDSHAKE \
    "send {'method':'turn/completed','params':{'threadId':'thr-1','turn':{'id':'turn-1','items':[],'status':'failed','error':" error_json "}}}\n" \
    "eof\n"

static void
check_failure(const gchar *script, gint code, const gchar *needle)
{
    Fixture fx;

    fixture_setup(&fx, script);
    fixture_run(&fx);
    g_assert_null(fx.response);
    g_assert_error(fx.stream_error, AI_ERROR, code);
    g_assert_nonnull(strstr(fx.stream_error->message, needle));
    fixture_teardown(&fx);
}

static void
test_failures_are_classified(void)
{
    /* The words, through the same phrase table as exec. */
    check_failure(FAILED_TURN("{'message':'You\\u0027ve hit your usage limit. Try again at 3:45 PM.'}"),
                  AI_ERROR_SESSION_LIMIT, "usage limit");
    /* Words that match nothing, but codex said what kind it was. */
    check_failure(FAILED_TURN("{'message':'slow down','codexErrorInfo':'rateLimitExceeded'}"),
                  AI_ERROR_RATE_LIMITED, "slow down");
    check_failure(FAILED_TURN("{'message':'token gone','codexErrorInfo':'unauthorized'}"),
                  AI_ERROR_INVALID_API_KEY, "token gone");
    /* Anything else is a plain execution error with codex's sentence. */
    check_failure(FAILED_TURN("{'message':'the model broke','codexErrorInfo':'other'}"),
                  AI_ERROR_CLI_EXECUTION, "the model broke");
    /* A thread refusal fails the run with codex's words. */
    check_failure("read\n"
                  "send {'id':1,'result':{}}\n"
                  "read\n"
                  "read\n"
                  "send {'id':2,'error':{'code':-32600,'message':'no rollout found for thread id thr-old'}}\n"
                  "sleep 1\n",
                  AI_ERROR_CLI_EXECUTION, "no rollout found");
}

/* ================================================================
 * Steering
 * ================================================================ */

#define STEERED_TURN(steer_answer) \
    HANDSHAKE TOOL_STARTED \
    "read\n" \
    steer_answer \
    TOOL_COMPLETED ANSWER COMPLETED

static void
run_steer(Fixture *fx, const gchar *script)
{
    fixture_setup(fx, script);
    fx->steer_on = AI_EVENT_TOOL_STARTED;
    fixture_run(fx);
}

/* The request names the thread and the running turn, under id 4. */
static void
assert_steer_request(Fixture *fx)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *request = fixture_request(fx, 4, parser);
    JsonObject *params = json_object_get_object_member(request, "params");
    JsonObject *input;

    g_assert_cmpint(json_object_get_int_member(request, "id"), ==, 4);
    g_assert_cmpstr(json_object_get_string_member(request, "method"), ==, "turn/steer");
    g_assert_cmpstr(json_object_get_string_member(params, "threadId"), ==, "thr-1");
    g_assert_cmpstr(json_object_get_string_member(params, "expectedTurnId"), ==, "turn-1");
    input = json_array_get_object_element(json_object_get_array_member(params, "input"), 0);
    g_assert_cmpstr(json_object_get_string_member(input, "type"), ==, "text");
    g_assert_cmpstr(json_object_get_string_member(input, "text"), ==, "STEERED WORDS");
}

static void
test_steer_delivered(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    run_steer(&fx, STEERED_TURN("send {'id':4,'result':{'turnId':'turn-1'}}\n"));
    text = response_text(&fx);
    g_assert_cmpstr(text, ==, "Hello");
    assert_steer_request(&fx);
    g_assert_no_error(fx.outcome.error);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_DELIVERED);
    assert_file_exists(&fx, "eof");
    fixture_teardown(&fx);
}

static void
test_steer_turn_mismatch(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    run_steer(&fx, STEERED_TURN(
        "send {'id':4,'error':{'code':-32600,'message':'expected active turn id `turn-1` but found `turn-2`'}}\n"));
    text = response_text(&fx);
    assert_steer_request(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_TURN_MISMATCH);
    fixture_teardown(&fx);
}

static void
test_steer_no_active_turn(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    run_steer(&fx, STEERED_TURN(
        "send {'id':4,'error':{'code':-32600,'message':'no active turn to steer'}}\n"));
    text = response_text(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN);
    fixture_teardown(&fx);
}

static void
test_steer_not_steerable(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    run_steer(&fx, STEERED_TURN(
        "send {'id':4,'error':{'code':-32600,'message':'cannot steer a review turn','data':{'message':'cannot steer a review turn','codexErrorInfo':{'activeTurnNotSteerable':{'turnKind':'review'}}}}}\n"));
    text = response_text(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NOT_STEERABLE);
    fixture_teardown(&fx);
}

/* The core call itself failed: it cannot be shown not to have run. */
static void
test_steer_internal_error_is_indeterminate(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    run_steer(&fx, STEERED_TURN(
        "send {'id':4,'error':{'code':-32603,'message':'failed to steer turn: channel closed'}}\n"));
    text = response_text(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_INDETERMINATE);
    g_assert_no_error(fx.outcome.error);
    fixture_teardown(&fx);
}

/* Sent, and no answer within steer-timeout-ms. */
static void
test_steer_timeout_is_indeterminate(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, STEERED_TURN("sleep 1\n"));
    g_object_set(fx.client, "steer-timeout-ms", 200, NULL);
    fx.steer_on = AI_EVENT_TOOL_STARTED;
    fixture_run(&fx);
    text = response_text(&fx);
    assert_steer_request(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_INDETERMINATE);
    g_assert_no_error(fx.outcome.error);
    fixture_teardown(&fx);
}

/* Before codex names its turn, nothing is written. */
static void
test_steer_before_the_turn_is_refused(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, PLAIN_TURN);
    fx.steer_now = TRUE;
    fixture_run(&fx);
    text = response_text(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NOT_STEERABLE);
    g_assert_cmpuint(fixture_request_count(&fx), ==, 4);
    fixture_teardown(&fx);
}

/* After turn/completed the channel is settled: refused, never written. */
static void
test_steer_after_the_turn_is_refused(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, PLAIN_TURN);
    fx.steer_on = AI_EVENT_USAGE;
    fixture_run(&fx);
    text = response_text(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN);
    g_assert_cmpuint(fixture_request_count(&fx), ==, 4);
    fixture_teardown(&fx);
}

/* A turn id from another run never reaches codex. */
static void
test_steer_wrong_local_turn(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, HANDSHAKE TOOL_STARTED TOOL_COMPLETED ANSWER COMPLETED);
    fx.steer_on = AI_EVENT_TOOL_STARTED;
    fx.steer_turn_id = "bogus";
    fixture_run(&fx);
    text = response_text(&fx);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_TURN_MISMATCH);
    g_assert_cmpuint(fixture_request_count(&fx), ==, 4);
    fixture_teardown(&fx);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ai-glib/codex-app-server/exec-is-the-default", test_exec_is_the_default);
    g_test_add_func("/ai-glib/codex-app-server/fails-closed", test_fails_closed);
    g_test_add_func("/ai-glib/codex-app-server/handshake-and-turn", test_handshake_and_turn);
    g_test_add_func("/ai-glib/codex-app-server/resume-thread", test_resume_thread);
    g_test_add_func("/ai-glib/codex-app-server/config-overrides", test_config_overrides);
    g_test_add_func("/ai-glib/codex-app-server/tool-items", test_tool_items);
    g_test_add_func("/ai-glib/codex-app-server/sync-path", test_sync_path);
    g_test_add_func("/ai-glib/codex-app-server/approvals-declined", test_approvals_declined);
    g_test_add_func("/ai-glib/codex-app-server/approvals-accepted-when-skipping",
                    test_approvals_accepted_when_skipping);
    g_test_add_func("/ai-glib/codex-app-server/failures-are-classified", test_failures_are_classified);
    g_test_add_func("/ai-glib/codex-app-server/steer/delivered", test_steer_delivered);
    g_test_add_func("/ai-glib/codex-app-server/steer/turn-mismatch", test_steer_turn_mismatch);
    g_test_add_func("/ai-glib/codex-app-server/steer/no-active-turn", test_steer_no_active_turn);
    g_test_add_func("/ai-glib/codex-app-server/steer/not-steerable", test_steer_not_steerable);
    g_test_add_func("/ai-glib/codex-app-server/steer/internal-error-is-indeterminate",
                    test_steer_internal_error_is_indeterminate);
    g_test_add_func("/ai-glib/codex-app-server/steer/timeout-is-indeterminate",
                    test_steer_timeout_is_indeterminate);
    g_test_add_func("/ai-glib/codex-app-server/steer/before-the-turn-is-refused",
                    test_steer_before_the_turn_is_refused);
    g_test_add_func("/ai-glib/codex-app-server/steer/after-the-turn-is-refused",
                    test_steer_after_the_turn_is_refused);
    g_test_add_func("/ai-glib/codex-app-server/steer/wrong-local-turn",
                    test_steer_wrong_local_turn);

    return g_test_run();
}
