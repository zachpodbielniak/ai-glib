/*
 * test-steerable.c - Words into a running turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The contract under test is the tri-state.  REFUSED must mean provably
 * not delivered, because the caller will queue the words again; anything
 * that may have reached the engine is DELIVERED or INDETERMINATE, which
 * the caller must never replay.  Each outcome is driven here against
 * something that can produce it on purpose:
 *
 *   - claude-code against a fake `claude` that records its stdin and
 *     either echoes a steer back (--replay-user-messages), swallows it,
 *     or closes its stdin before it arrives;
 *   - the executor's own tool loop against a scripted provider, to pin
 *     the splice point: the top of a round, after every tool result of
 *     the batch, never between two of them;
 *   - claude-tmux against a fake tmux and a transcript the test writes,
 *     to pin how a typed line is classified.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "ai-glib.h"
#include "core/ai-steerable.h"
#include "providers/ai-claude-tmux-client-internal.h"

/* ================================================================
 * Shared: wait for a steer's answer
 * ================================================================ */

typedef struct
{
    gboolean      done;
    AiSteerResult result;
    GError       *error;
} SteerOutcome;

static void
on_steered(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SteerOutcome *outcome = user_data;

    outcome->result = ai_steerable_steer_finish(AI_STEERABLE(source), result,
                                                &outcome->error);
    outcome->done = TRUE;
}

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

/* Whether a turn is running, without leaking the id it names. */
static void
assert_turn_running(AiSteerable *steerable, gboolean expected)
{
    g_autofree gchar *turn_id = ai_steerable_dup_turn_id(steerable);

    g_assert_cmpint(turn_id != NULL, ==, expected);
}

static void
steer_now(AiSteerable *steerable, const gchar *turn_id, const gchar *text,
          SteerOutcome *outcome)
{
    ai_steerable_steer_async(steerable, turn_id, text, NULL, on_steered,
                             outcome);
    iterate_until(&outcome->done);
}

/* ================================================================
 * The interface itself
 * ================================================================ */

static void
assert_can_steer(gpointer object, gboolean expected)
{
    g_assert_true(AI_IS_STEERABLE(object));
    g_assert_cmpint(ai_steerable_can_steer(AI_STEERABLE(object)), ==, expected);
}

/* Callers ask a probe client, so every client must answer. */
static void
test_can_steer_is_a_class_datum(void)
{
    g_autoptr(AiClaudeCodeClient) claude_code = ai_claude_code_client_new();
    g_autoptr(AiClaudeTmuxClient) claude_tmux = ai_claude_tmux_client_new();
    g_autoptr(AiToolExecutor) executor = ai_tool_executor_new_empty();
    g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
    g_autoptr(AiGrokBuildClient) grok_build = ai_grok_build_client_new();
    g_autoptr(AiAntigravityClient) antigravity = ai_antigravity_client_new();
    g_autoptr(AiCursorClient) cursor = ai_cursor_client_new();
    g_autoptr(GObject) codex = g_object_new(AI_TYPE_CODEX_CLI_CLIENT, NULL);
    g_autoptr(AiClaudeClient) claude_http = ai_claude_client_new();
    g_autoptr(AiOpenAIClient) openai = ai_openai_client_new();

    assert_can_steer(claude_code, TRUE);
    assert_can_steer(claude_tmux, TRUE);
    assert_can_steer(executor, TRUE);

    assert_can_steer(opencode, FALSE);
    assert_can_steer(grok_build, FALSE);
    assert_can_steer(antigravity, FALSE);
    assert_can_steer(cursor, FALSE);
    assert_can_steer(codex, FALSE);
    assert_can_steer(claude_http, FALSE);
    assert_can_steer(openai, FALSE);
}

static void
test_cannot_steer_refuses(void)
{
    g_autoptr(AiOpenCodeClient) opencode = ai_opencode_client_new();
    SteerOutcome outcome = { 0 };

    g_assert_null(ai_steerable_dup_turn_id(AI_STEERABLE(opencode)));

    steer_now(AI_STEERABLE(opencode), NULL, "words", &outcome);
    g_assert_cmpint(outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NOT_SUPPORTED);
    g_clear_error(&outcome.error);
}

static void
test_result_names(void)
{
    g_assert_cmpstr(ai_steer_result_to_string(AI_STEER_REFUSED), ==, "refused");
    g_assert_cmpstr(ai_steer_result_to_string(AI_STEER_DELIVERED), ==, "delivered");
    g_assert_cmpstr(ai_steer_result_to_string(AI_STEER_INDETERMINATE), ==,
                    "indeterminate");
    /* Zero must never read as a delivery. */
    g_assert_cmpint(AI_STEER_REFUSED, ==, 0);
}

/* ================================================================
 * claude-code: a fake CLI that records its stdin
 * ================================================================ */

/*
 * The fake speaks just enough of claude's stream-json to drive the
 * reader: it reads the prompt, echoes it (as --replay-user-messages
 * does), announces a tool call and then, by mode:
 *
 *   deliver        reads the steer, echoes it with its uuid, answers
 *   swallow        reads the steer, never echoes it, answers and exits
 *   closed         closes its stdin before the steer can arrive
 *   late           answers at once; a steer then meets a settled turn
 *
 * Every mode but swallow then waits for EOF on stdin before exiting, so
 * a run that forgot to close stdin hangs into the process deadline
 * instead of passing.
 */
#define FAKE_CLAUDE                                                            \
    "#!/bin/sh\n"                                                              \
    "d='%s'\n"                                                                 \
    "mode=$(cat \"$d/mode\")\n"                                                \
    "printf '%%s\\n' \"$*\" > \"$d/argv.log\"\n"                               \
    "uuid_of() { printf '%%s' \"$1\" | sed -n 's/.*\"uuid\":\"\\([^\"]*\\)\".*/\\1/p'; }\n" \
    "echo_user() { printf '{\"type\":\"user\",\"isReplay\":true,\"uuid\":\"%%s\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":\"ECHOED-PROMPT\"}]}}\\n' \"$1\"; }\n" \
    "IFS= read -r first\n"                                                     \
    "printf '%%s\\n' \"$first\" >> \"$d/stdin.log\"\n"                         \
    "if [ \"$mode\" = closed ]; then exec 0<&-; fi\n"                         \
    "echo '{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"s-1\"}'\n" \
    "echo_user \"$(uuid_of \"$first\")\"\n"                                    \
    "echo '{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{}}]}}'\n" \
    "answer() { echo '{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"done\"}]}}'; echo '{\"type\":\"result\",\"subtype\":\"success\",\"result\":\"done\",\"session_id\":\"s-1\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}'; }\n" \
    "case \"$mode\" in\n"                                                      \
    "  deliver) IFS= read -r steer; printf '%%s\\n' \"$steer\" >> \"$d/stdin.log\";\n" \
    "           echo_user \"$(uuid_of \"$steer\")\"; answer ;;\n"              \
    "  swallow) IFS= read -r steer; printf '%%s\\n' \"$steer\" >> \"$d/stdin.log\";\n" \
    "           answer; exit 0 ;;\n"                                           \
    "  closed)  sleep 1; answer; exit 0 ;;\n"                                  \
    "  late)    answer ;;\n"                                                   \
    "esac\n"                                                                   \
    "cat > /dev/null\n"                                                        \
    "echo eof > \"$d/eof\"\n"

typedef struct
{
    gchar              *dir;
    AiClaudeCodeClient *client;
    const gchar        *mode;

    gboolean            stream_done;
    AiResponse         *response;
    GError             *stream_error;

    /* What to steer, and when: on the first event of this kind. */
    AiEventKind         steer_on;
    const gchar        *steer_turn_id;   /* NULL, or "bogus" */
    gboolean            steered;
    gchar              *turn_id_seen;
    SteerOutcome        outcome;
} ClaudeFixture;

static gchar *
fixture_read(ClaudeFixture *fx, const gchar *name)
{
    g_autofree gchar *path = g_build_filename(fx->dir, name, NULL);
    gchar *contents = NULL;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;
    return contents;
}

/* Whether the fake left @name behind; freed, unlike a bare assert on it. */
static void
assert_file_exists(ClaudeFixture *fx, const gchar *name)
{
    g_autofree gchar *contents = fixture_read(fx, name);

    g_assert_nonnull(contents);
}

static void
fixture_setup_full(ClaudeFixture *fx, const gchar *mode, gboolean steerable)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *script = NULL;
    g_autofree gchar *cli = NULL;
    g_autofree gchar *mode_path = NULL;

    memset(fx, 0, sizeof(*fx));
    fx->mode = mode;
    fx->dir = g_dir_make_tmp("ai-glib-steer-XXXXXX", &error);
    g_assert_no_error(error);

    cli = g_build_filename(fx->dir, "claude", NULL);
    script = g_strdup_printf(FAKE_CLAUDE, fx->dir);
    g_file_set_contents(cli, script, -1, &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(cli, 0700), ==, 0);

    mode_path = g_build_filename(fx->dir, "mode", NULL);
    g_file_set_contents(mode_path, mode, -1, &error);
    g_assert_no_error(error);

    fx->client = ai_claude_code_client_new();
    g_object_set(fx->client, "steerable", steerable, NULL);
    ai_cli_client_set_executable_path(AI_CLI_CLIENT(fx->client), cli);
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx->client), FALSE);
    /* A run that never closes stdin fails here instead of hanging. */
    ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(fx->client), 15000);
}

static void
fixture_setup(ClaudeFixture *fx, const gchar *mode)
{
    fixture_setup_full(fx, mode, TRUE);
}

static void
fixture_teardown(ClaudeFixture *fx)
{
    const gchar *names[] = { "claude", "mode", "argv.log", "stdin.log", "eof", NULL };
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
    g_free(fx->turn_id_seen);
}

static void
on_fixture_event(AiEventSource *source, AiEvent *event, gpointer user_data)
{
    ClaudeFixture *fx = user_data;

    (void)source;

    if (fx->steered || ai_event_get_kind(event) != fx->steer_on)
        return;

    fx->steered = TRUE;
    fx->turn_id_seen = ai_steerable_dup_turn_id(AI_STEERABLE(fx->client));
    ai_steerable_steer_async(AI_STEERABLE(fx->client),
                             fx->steer_turn_id != NULL ? fx->steer_turn_id
                                                       : fx->turn_id_seen,
                             "STEERED WORDS", NULL, on_steered, &fx->outcome);
}

static void
on_fixture_stream(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClaudeFixture *fx = user_data;

    fx->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
                                                    result, &fx->stream_error);
    fx->stream_done = TRUE;
}

/* Run one streamed turn, steering on the first event of @steer_on. */
static void
fixture_run(ClaudeFixture *fx, AiEventKind steer_on)
{
    g_autoptr(AiMessage) message = ai_message_new_user("do the thing");
    GList *messages = g_list_append(NULL, message);
    gulong handler;

    fx->steer_on = steer_on;
    handler = g_signal_connect(fx->client, "event",
                               G_CALLBACK(on_fixture_event), fx);

    ai_streamable_chat_stream_async(AI_STREAMABLE(fx->client), messages, NULL,
                                    0, NULL, NULL, on_fixture_stream, fx);
    iterate_until(&fx->stream_done);
    if (fx->steered)
        iterate_until(&fx->outcome.done);

    g_signal_handler_disconnect(fx->client, handler);
    g_list_free(messages);
}

/* The streamed command line is the steerable one; the one-shot is not. */
static void
test_claude_code_streams_with_open_input(void)
{
    ClaudeFixture fx;
    g_autofree gchar *argv = NULL;
    g_autofree gchar *stdin_log = NULL;
    g_auto(GStrv) sync_argv = NULL;
    g_autoptr(AiMessage) message = ai_message_new_user("x");
    GList *messages;
    AiCliClientClass *klass;

    fixture_setup(&fx, "late");
    fixture_run(&fx, AI_EVENT_STREAM_END);   /* never steers */

    g_assert_no_error(fx.stream_error);
    argv = fixture_read(&fx, "argv.log");
    g_assert_nonnull(strstr(argv, "--input-format stream-json"));
    g_assert_nonnull(strstr(argv, "--output-format stream-json"));
    g_assert_nonnull(strstr(argv, "--replay-user-messages"));

    /* The prompt went in as a JSON user event carrying a uuid. */
    stdin_log = fixture_read(&fx, "stdin.log");
    g_assert_true(g_str_has_prefix(stdin_log, "{\"type\":\"user\""));
    g_assert_nonnull(strstr(stdin_log, "\"uuid\":\""));
    g_assert_nonnull(strstr(stdin_log, "do the thing"));

    /* stdin was closed at the result: the fake saw EOF and exited. */
    assert_file_exists(&fx, "eof");

    /* The echo of our own prompt is not part of the reply. */
    {
        g_autofree gchar *text = ai_response_get_text(fx.response);
        g_assert_cmpstr(text, ==, "done");
    }

    /* No turn is running once the stream is over. */
    g_assert_null(ai_steerable_dup_turn_id(AI_STEERABLE(fx.client)));

    /* The one-shot path keeps its plain shape. */
    messages = g_list_append(NULL, message);
    klass = AI_CLI_CLIENT_GET_CLASS(fx.client);
    sync_argv = klass->build_argv(AI_CLI_CLIENT(fx.client), messages, NULL, 0,
                                  FALSE);
    g_assert_false(g_strv_contains((const gchar * const *)sync_argv,
                                   "--replay-user-messages"));
    g_assert_false(g_strv_contains((const gchar * const *)sync_argv,
                                   "--input-format"));
    g_list_free(messages);

    fixture_teardown(&fx);
}

/*
 * Off by default: a streamed turn keeps its old shape (text on stdin,
 * closed at once), which a stand-in that reads to EOF depends on, and a
 * steer is refused as NOT_STEERABLE rather than finding no turn.
 */
static void
test_claude_code_steering_is_opt_in(void)
{
    ClaudeFixture fx;
    g_autofree gchar *argv = NULL;
    g_autofree gchar *stdin_log = NULL;
    gboolean steerable = TRUE;

    fixture_setup_full(&fx, "late", FALSE);
    g_object_get(fx.client, "steerable", &steerable, NULL);
    g_assert_false(steerable);
    fixture_run(&fx, AI_EVENT_TOOL_STARTED);

    g_assert_no_error(fx.stream_error);
    argv = fixture_read(&fx, "argv.log");
    g_assert_null(strstr(argv, "--input-format"));
    g_assert_null(strstr(argv, "--replay-user-messages"));
    stdin_log = fixture_read(&fx, "stdin.log");
    g_assert_false(g_str_has_prefix(stdin_log, "{"));

    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR,
                   AI_STEER_ERROR_NOT_STEERABLE);

    fixture_teardown(&fx);
}

/* Written, echoed back by uuid: DELIVERED, and the CLI got the words. */
static void
test_claude_code_steer_delivered(void)
{
    ClaudeFixture fx;
    g_autofree gchar *stdin_log = NULL;
    g_auto(GStrv) lines = NULL;

    fixture_setup(&fx, "deliver");
    fixture_run(&fx, AI_EVENT_TOOL_STARTED);

    g_assert_no_error(fx.stream_error);
    g_assert_true(fx.steered);
    g_assert_nonnull(fx.turn_id_seen);
    g_assert_no_error(fx.outcome.error);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_DELIVERED);

    stdin_log = fixture_read(&fx, "stdin.log");
    lines = g_strsplit(stdin_log, "\n", -1);
    g_assert_cmpuint(g_strv_length(lines), ==, 3);   /* two lines + "" */
    g_assert_nonnull(strstr(lines[1], "STEERED WORDS"));
    g_assert_nonnull(strstr(lines[1], "\"uuid\":\""));
    assert_file_exists(&fx, "eof");

    fixture_teardown(&fx);
}

/* Written, never echoed, run over: INDETERMINATE -- never REFUSED. */
static void
test_claude_code_steer_indeterminate(void)
{
    ClaudeFixture fx;
    g_autofree gchar *stdin_log = NULL;

    fixture_setup(&fx, "swallow");
    fixture_run(&fx, AI_EVENT_TOOL_STARTED);

    g_assert_no_error(fx.stream_error);
    g_assert_no_error(fx.outcome.error);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_INDETERMINATE);

    /* The words did reach the CLI, which is exactly why this is not a
     * refusal: queueing them again would run them twice. */
    stdin_log = fixture_read(&fx, "stdin.log");
    g_assert_nonnull(strstr(stdin_log, "STEERED WORDS"));

    fixture_teardown(&fx);
}

/* Nothing could be written: REFUSED, and nothing reached the CLI. */
static void
test_claude_code_steer_refused_when_input_closed(void)
{
    ClaudeFixture fx;
    g_autofree gchar *stdin_log = NULL;

    fixture_setup(&fx, "closed");
    fixture_run(&fx, AI_EVENT_TOOL_STARTED);

    g_assert_no_error(fx.stream_error);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_SEND_FAILED);

    stdin_log = fixture_read(&fx, "stdin.log");
    g_assert_null(strstr(stdin_log, "STEERED WORDS"));

    fixture_teardown(&fx);
}

/* After the result the turn is settled: REFUSED, and stdin still closes. */
static void
test_claude_code_steer_refused_after_result(void)
{
    ClaudeFixture fx;
    g_autofree gchar *stdin_log = NULL;

    fixture_setup(&fx, "late");
    fixture_run(&fx, AI_EVENT_USAGE);

    g_assert_no_error(fx.stream_error);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR,
                   AI_STEER_ERROR_NO_ACTIVE_TURN);

    stdin_log = fixture_read(&fx, "stdin.log");
    g_assert_null(strstr(stdin_log, "STEERED WORDS"));
    assert_file_exists(&fx, "eof");

    fixture_teardown(&fx);
}

/* A held turn id that is not the running turn's: REFUSED, not misrouted. */
static void
test_claude_code_steer_turn_mismatch(void)
{
    ClaudeFixture fx;

    fixture_setup(&fx, "late");
    fx.steer_turn_id = "not-this-turn";
    fixture_run(&fx, AI_EVENT_TOOL_STARTED);

    g_assert_no_error(fx.stream_error);
    g_assert_cmpint(fx.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(fx.outcome.error, AI_STEER_ERROR,
                   AI_STEER_ERROR_TURN_MISMATCH);

    fixture_teardown(&fx);
}

static void
test_claude_code_steer_without_a_turn(void)
{
    g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
    SteerOutcome outcome = { 0 };

    g_object_set(client, "steerable", TRUE, NULL);

    g_assert_null(ai_steerable_dup_turn_id(AI_STEERABLE(client)));
    steer_now(AI_STEERABLE(client), NULL, "words", &outcome);
    g_assert_cmpint(outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN);
    g_clear_error(&outcome.error);
}

/* ================================================================
 * The executor's own loop: where the words go in
 * ================================================================ */

/*
 * A provider whose script is a list of turns, each either a batch of
 * tool calls or a final text.  AiMockProvider answers one tool call per
 * turn, and the splice point only means something with two.
 */
#define TEST_TYPE_SCRIPT_PROVIDER (test_script_provider_get_type())
G_DECLARE_FINAL_TYPE(TestScriptProvider, test_script_provider, TEST,
                     SCRIPT_PROVIDER, GObject)

struct _TestScriptProvider
{
    GObject  parent_instance;
    GQueue   turns;        /* gchar*: "tools:N" or the final text */
    guint    calls;
    guint    delay_ms;
    GList   *last_messages;
    void   (*on_call)(TestScriptProvider *self, gpointer data);
    gpointer on_call_data;
};

static void test_script_provider_iface_init(AiProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(TestScriptProvider, test_script_provider, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_PROVIDER,
                                              test_script_provider_iface_init))

static void
test_script_provider_finalize(GObject *object)
{
    TestScriptProvider *self = TEST_SCRIPT_PROVIDER(object);

    g_queue_clear_full(&self->turns, g_free);
    g_list_free_full(self->last_messages, g_object_unref);
    G_OBJECT_CLASS(test_script_provider_parent_class)->finalize(object);
}

static void
test_script_provider_class_init(TestScriptProviderClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = test_script_provider_finalize;
}

static void
test_script_provider_init(TestScriptProvider *self)
{
    g_queue_init(&self->turns);
}

static AiResponse *
script_next_response(TestScriptProvider *self)
{
    g_autofree gchar *turn = g_queue_pop_head(&self->turns);
    AiResponse *response = ai_response_new("r", "script");

    if (turn != NULL && g_str_has_prefix(turn, "tools:"))
    {
        guint n = (guint)g_ascii_strtoull(turn + 6, NULL, 10);
        guint i;

        for (i = 0; i < n; i++)
        {
            g_autofree gchar *id = g_strdup_printf("call-%u-%u", self->calls, i);
            AiToolUse *use = ai_tool_use_new_from_json_string(id, "probe", "{}");

            ai_response_add_content_block(response, AI_CONTENT_BLOCK(use));
        }
        ai_response_set_stop_reason(response, AI_STOP_REASON_TOOL_USE);
    }
    else
    {
        AiTextContent *text = ai_text_content_new(turn != NULL ? turn : "fallback");

        ai_response_add_content_block(response, AI_CONTENT_BLOCK(text));
        ai_response_set_stop_reason(response, AI_STOP_REASON_END_TURN);
    }

    return response;
}

static gboolean
script_deliver(gpointer user_data)
{
    GTask *task = user_data;
    TestScriptProvider *self = g_task_get_source_object(task);

    g_task_return_pointer(task, script_next_response(self), g_object_unref);
    g_object_unref(task);
    return G_SOURCE_REMOVE;
}

static gpointer
copy_ref(gconstpointer src, gpointer data)
{
    (void)data;
    return g_object_ref((gpointer)src);
}

static void
script_chat_async(AiProvider *provider, GList *messages,
                  const gchar *system_prompt, gint max_tokens, GList *tools,
                  GCancellable *cancellable, GAsyncReadyCallback callback,
                  gpointer user_data)
{
    TestScriptProvider *self = TEST_SCRIPT_PROVIDER(provider);
    GTask *task = g_task_new(self, cancellable, callback, user_data);

    (void)system_prompt;
    (void)max_tokens;
    (void)tools;

    self->calls++;
    g_list_free_full(self->last_messages, g_object_unref);
    self->last_messages = g_list_copy_deep(messages, copy_ref, NULL);
    if (self->on_call != NULL)
        self->on_call(self, self->on_call_data);

    g_timeout_add(self->delay_ms, script_deliver, task);
}

static AiResponse *
script_chat_finish(AiProvider *provider, GAsyncResult *result, GError **error)
{
    (void)provider;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static AiProviderType
script_type(AiProvider *provider)
{
    (void)provider;
    return AI_PROVIDER_CLAUDE;
}

static const gchar *
script_name(AiProvider *provider)
{
    (void)provider;
    return "script";
}

static void
test_script_provider_iface_init(AiProviderInterface *iface)
{
    iface->get_provider_type = script_type;
    iface->get_name = script_name;
    iface->get_default_model = script_name;
    iface->chat_async = script_chat_async;
    iface->chat_finish = script_chat_finish;
}

static TestScriptProvider *
script_new(guint delay_ms, const gchar *first, ...)
{
    TestScriptProvider *self = g_object_new(TEST_TYPE_SCRIPT_PROVIDER, NULL);
    const gchar *turn;
    va_list ap;

    self->delay_ms = delay_ms;
    va_start(ap, first);
    for (turn = first; turn != NULL; turn = va_arg(ap, const gchar *))
        g_queue_push_tail(&self->turns, g_strdup(turn));
    va_end(ap);

    return self;
}

typedef struct
{
    AiToolExecutor *executor;
    guint           tool_calls;
    guint           steer_at_tool;   /* 1-based; 0 = never */
    SteerOutcome    outcome;
} ProbeState;

/* The probe tool; optionally steers from inside the batch. */
static gchar *
probe_tool(AiToolUse *tool_use, GCancellable *cancellable, GError **error,
           gpointer user_data)
{
    ProbeState *state = user_data;

    (void)tool_use;
    (void)cancellable;
    (void)error;

    state->tool_calls++;
    if (state->tool_calls == state->steer_at_tool)
        ai_steerable_steer_async(AI_STEERABLE(state->executor), NULL,
                                 "STEERED MID-BATCH", NULL, on_steered,
                                 &state->outcome);

    return g_strdup_printf("result %u", state->tool_calls);
}

static AiToolExecutor *
probe_executor(ProbeState *state)
{
    AiToolExecutor *executor = ai_tool_executor_new_empty();
    g_autoptr(AiTool) tool = ai_tool_new("probe", "a probe");

    state->executor = executor;
    ai_tool_executor_register_callback(executor, tool, probe_tool, state, NULL);
    return executor;
}

typedef struct
{
    gboolean  done;
    gchar    *text;
    GList    *messages;
    GError   *error;
} RunOutcome;

static void
on_run(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RunOutcome *run = user_data;

    run->text = ai_tool_executor_run_full_finish(AI_TOOL_EXECUTOR(source),
                                                 result, &run->messages,
                                                 &run->error);
    run->done = TRUE;
}

static void
on_run_plain(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RunOutcome *run = user_data;

    run->text = ai_tool_executor_run_finish(AI_TOOL_EXECUTOR(source), result,
                                            &run->error);
    run->done = TRUE;
}

static void
run_outcome_clear(RunOutcome *run)
{
    g_free(run->text);
    g_list_free_full(run->messages, g_object_unref);
    g_clear_error(&run->error);
}

/* Index of the first message whose text contains @needle, or -1. */
static gint
message_index(GList *messages, const gchar *needle)
{
    GList *l;
    gint i = 0;

    for (l = messages; l != NULL; l = l->next, i++)
    {
        g_autofree gchar *text = ai_message_get_text(l->data);

        if (text != NULL && strstr(text, needle) != NULL)
            return i;
    }
    return -1;
}

/* Index of the last message carrying a tool result. */
static gint
last_tool_result_index(GList *messages)
{
    GList *l;
    gint i = 0;
    gint last = -1;

    for (l = messages; l != NULL; l = l->next, i++)
    {
        GList *b;

        for (b = ai_message_get_content_blocks(l->data); b != NULL; b = b->next)
            if (AI_IS_TOOL_RESULT(b->data))
                last = i;
    }
    return last;
}

/*
 * Steered from inside the first of two tool calls: the words wait until
 * both results are in, and then reach the model at the top of the next
 * round -- never between the two results.
 */
static void
test_executor_splices_after_the_batch(void)
{
    ProbeState state = { 0 };
    g_autoptr(AiToolExecutor) executor = probe_executor(&state);
    g_autoptr(TestScriptProvider) provider =
        script_new(10, "tools:2", "final", NULL);
    g_autoptr(AiMessage) user = ai_message_new_user("go");
    GList *messages = g_list_append(NULL, user);
    RunOutcome run = { 0 };
    gint steer_at;

    state.steer_at_tool = 1;
    ai_tool_executor_run_full_async(executor, AI_PROVIDER(provider), messages,
                                    NULL, 0, 0, NULL, on_run, &run);
    iterate_until(&run.done);
    iterate_until(&state.outcome.done);

    g_assert_no_error(run.error);
    g_assert_cmpstr(run.text, ==, "final");
    g_assert_no_error(state.outcome.error);
    g_assert_cmpint(state.outcome.result, ==, AI_STEER_DELIVERED);
    g_assert_cmpuint(state.tool_calls, ==, 2);

    /* The second request already carried the words, after both results. */
    steer_at = message_index(provider->last_messages, "STEERED MID-BATCH");
    g_assert_cmpint(steer_at, >, last_tool_result_index(provider->last_messages));
    g_assert_cmpint(steer_at, ==,
                    (gint)g_list_length(provider->last_messages) - 1);
    g_assert_cmpuint(provider->calls, ==, 2);

    run_outcome_clear(&run);
    g_list_free(messages);
}

typedef struct
{
    ProbeState *state;
    guint       on_call;
    gboolean    fired;
} CallSteer;

/* Steer while request number @on_call is in flight. */
static void
steer_during_call(TestScriptProvider *self, gpointer data)
{
    CallSteer *call = data;

    if (self->calls != call->on_call || call->fired)
        return;
    call->fired = TRUE;
    ai_steerable_steer_async(AI_STEERABLE(call->state->executor), NULL,
                             "STEERED DURING ANSWER", NULL, on_steered,
                             &call->state->outcome);
}

/*
 * Steered while the model writes what would have been its final answer:
 * that answer is not final -- the run goes round once more with the words.
 */
static void
test_executor_steer_extends_a_final_answer(void)
{
    ProbeState state = { 0 };
    g_autoptr(AiToolExecutor) executor = probe_executor(&state);
    g_autoptr(TestScriptProvider) provider =
        script_new(50, "first answer", "second answer", NULL);
    g_autoptr(AiMessage) user = ai_message_new_user("go");
    GList *messages = g_list_append(NULL, user);
    CallSteer call = { &state, 1, FALSE };
    RunOutcome run = { 0 };

    provider->on_call = steer_during_call;
    provider->on_call_data = &call;

    ai_tool_executor_run_full_async(executor, AI_PROVIDER(provider), messages,
                                    NULL, 0, 0, NULL, on_run, &run);
    iterate_until(&run.done);

    g_assert_no_error(run.error);
    g_assert_true(state.outcome.done);
    g_assert_cmpint(state.outcome.result, ==, AI_STEER_DELIVERED);
    g_assert_cmpuint(provider->calls, ==, 2);
    g_assert_cmpstr(run.text, ==, "second answer");
    g_assert_cmpint(message_index(provider->last_messages,
                                  "STEERED DURING ANSWER"), >=, 0);

    run_outcome_clear(&run);
    g_list_free(messages);
}

/* Parked but the run stops at its turn limit: never spliced, so REFUSED. */
static void
test_executor_unspliced_words_are_refused(void)
{
    ProbeState state = { 0 };
    g_autoptr(AiToolExecutor) executor = probe_executor(&state);
    g_autoptr(TestScriptProvider) provider =
        script_new(50, "only answer", NULL);
    g_autoptr(AiMessage) user = ai_message_new_user("go");
    GList *messages = g_list_append(NULL, user);
    CallSteer call = { &state, 1, FALSE };
    RunOutcome run = { 0 };

    provider->on_call = steer_during_call;
    provider->on_call_data = &call;

    /* max_turns 1: the answer to the first request ends the run. */
    ai_tool_executor_run_async(executor, AI_PROVIDER(provider), messages,
                               NULL, 0, 1, NULL, on_run_plain, &run);
    iterate_until(&run.done);

    g_assert_true(state.outcome.done);
    g_assert_cmpint(state.outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(state.outcome.error, AI_STEER_ERROR,
                   AI_STEER_ERROR_NO_ACTIVE_TURN);
    g_assert_cmpuint(provider->calls, ==, 1);
    g_assert_null(ai_steerable_dup_turn_id(AI_STEERABLE(executor)));

    g_clear_error(&state.outcome.error);
    run_outcome_clear(&run);
    g_list_free(messages);
}

static void
test_executor_steer_without_a_run(void)
{
    g_autoptr(AiToolExecutor) executor = ai_tool_executor_new_empty();
    SteerOutcome outcome = { 0 };

    steer_now(AI_STEERABLE(executor), NULL, "words", &outcome);
    g_assert_cmpint(outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN);
    g_clear_error(&outcome.error);
}

/* ================================================================
 * claude-tmux: reading the transcript and the pane
 * ================================================================ */

static void
test_tmux_queue_operations(void)
{
    const gchar *enqueued =
        "{\"type\":\"queue-operation\",\"operation\":\"enqueue\",\"content\":\"a\"}\n";
    const gchar *absorbed =
        "{\"type\":\"queue-operation\",\"operation\":\"enqueue\",\"content\":\"a\"}\n"
        "{\"type\":\"user\",\"message\":{\"content\":[]}}\n"
        "{\"type\":\"queue-operation\",\"operation\":\"remove\","
        "\"reason\":\"absorbed_mid_turn\",\"content\":\"a\"}\n";
    const gchar *two_one_left =
        "{\"type\":\"queue-operation\",\"operation\":\"enqueue\"}\n"
        "{\"type\":\"queue-operation\",\"operation\":\"enqueue\"}\n"
        "{\"type\":\"queue-operation\",\"operation\":\"dequeue\"}\n"
        "{\"type\":\"queue-operation\",\"opera";   /* half-written */

    g_assert_false(ai_claude_tmux_client_jsonl_has_enqueue(""));
    g_assert_true(ai_claude_tmux_client_jsonl_has_enqueue(enqueued));
    g_assert_cmpuint(ai_claude_tmux_client_jsonl_pending_queued(enqueued), ==, 1);
    g_assert_cmpuint(ai_claude_tmux_client_jsonl_pending_queued(absorbed), ==, 0);
    g_assert_cmpuint(ai_claude_tmux_client_jsonl_pending_queued(two_one_left), ==, 1);
    /* A tool result is a "user" entry too; it is not a queued steer. */
    g_assert_false(ai_claude_tmux_client_jsonl_has_enqueue(
        "{\"type\":\"user\",\"message\":{\"content\":[]}}\n"));
}

/*
 * A fake tmux.  load-buffer/paste-buffer succeed unless "paste-fails"
 * exists; an Enter appends an enqueue to the transcript when "accept"
 * exists; capture-pane prints the file "pane".  Every call is logged.
 */
#define FAKE_TMUX                                                              \
    "#!/bin/sh\n"                                                              \
    "d='%s'\n"                                                                 \
    "printf '%%s\\n' \"$*\" >> \"$d/tmux.log\"\n"                              \
    "case \"$*\" in\n"                                                         \
    "  *load-buffer*) [ -f \"$d/paste-fails\" ] && exit 1; exit 0 ;;\n"        \
    "  *send-keys*Enter*) if [ -f \"$d/accept\" ]; then\n"                     \
    "      echo '{\"type\":\"queue-operation\",\"operation\":\"enqueue\"}' >> \"$d/t.jsonl\"; fi; exit 0 ;;\n" \
    "  *capture-pane*) cat \"$d/pane\" 2>/dev/null; exit 0 ;;\n"              \
    "esac\n"                                                                   \
    "exit 0\n"

typedef struct
{
    gchar              *dir;
    gchar              *jsonl;
    AiClaudeTmuxClient *client;
} TmuxFixture;

static void
tmux_touch(TmuxFixture *fx, const gchar *name, const gchar *contents)
{
    g_autofree gchar *path = g_build_filename(fx->dir, name, NULL);
    g_autoptr(GError) error = NULL;

    g_file_set_contents(path, contents, -1, &error);
    g_assert_no_error(error);
}

static void
tmux_setup(TmuxFixture *fx)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *script = NULL;
    g_autofree gchar *tmux = NULL;

    fx->dir = g_dir_make_tmp("ai-glib-steer-tmux-XXXXXX", &error);
    g_assert_no_error(error);
    tmux = g_build_filename(fx->dir, "tmux", NULL);
    script = g_strdup_printf(FAKE_TMUX, fx->dir);
    g_file_set_contents(tmux, script, -1, &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(tmux, 0700), ==, 0);

    fx->jsonl = g_build_filename(fx->dir, "t.jsonl", NULL);
    tmux_touch(fx, "t.jsonl", "{\"type\":\"user\",\"message\":{}}\n");

    fx->client = ai_claude_tmux_client_new();
    g_object_set(fx->client,
                 "prompt-resend-interval-ms", 100,
                 "max-prompt-send-attempts", 2,
                 "prompt-send-exponential-backoff", FALSE,
                 "command-timeout-ms", 5000,
                 NULL);
    ai_claude_tmux_client_open_live_turn(fx->client, tmux, "sock", "sess",
                                         "sid", fx->jsonl, fx->dir, 0);
}

static void
tmux_teardown(TmuxFixture *fx)
{
    const gchar *names[] = { "tmux", "tmux.log", "t.jsonl", "accept", "pane",
                             "paste-fails", NULL };
    guint i;

    for (i = 0; names[i] != NULL; i++)
    {
        g_autofree gchar *path = g_build_filename(fx->dir, names[i], NULL);
        g_unlink(path);
    }
    g_rmdir(fx->dir);
    g_free(fx->dir);
    g_free(fx->jsonl);
    g_clear_object(&fx->client);
}

static void
test_tmux_steer_delivered(void)
{
    TmuxFixture fx;
    SteerOutcome outcome = { 0 };
    g_autofree gchar *turn_id = NULL;
    g_autofree gchar *log = NULL;
    g_autofree gchar *log_path = NULL;

    tmux_setup(&fx);
    tmux_touch(&fx, "accept", "");

    turn_id = ai_steerable_dup_turn_id(AI_STEERABLE(fx.client));
    g_assert_nonnull(turn_id);
    steer_now(AI_STEERABLE(fx.client), turn_id, "steer me", &outcome);
    g_assert_no_error(outcome.error);
    g_assert_cmpint(outcome.result, ==, AI_STEER_DELIVERED);

    /* Typed as a bracketed paste, then submitted, on our own socket. */
    log_path = g_build_filename(fx.dir, "tmux.log", NULL);
    g_file_get_contents(log_path, &log, NULL, NULL);
    g_assert_nonnull(strstr(log, "-L sock load-buffer"));
    g_assert_nonnull(strstr(log, "paste-buffer"));
    g_assert_nonnull(strstr(log, "-p"));
    g_assert_nonnull(strstr(log, "send-keys -t sess Enter"));

    /* The enqueue is still pending: the turn is not over at Stop. */
    g_assert_false(ai_claude_tmux_client_settle_live_turn(fx.client, fx.jsonl, 0));
    assert_turn_running(AI_STEERABLE(fx.client), TRUE);

    tmux_teardown(&fx);
}

/* Still in the draft box after every Enter: cleared, so REFUSED. */
static void
test_tmux_steer_refused_when_still_in_draft(void)
{
    TmuxFixture fx;
    SteerOutcome outcome = { 0 };
    g_autofree gchar *log = NULL;
    g_autofree gchar *log_path = NULL;

    tmux_setup(&fx);
    tmux_touch(&fx, "pane", "  earlier output\n> steer me\n  status line\n");

    steer_now(AI_STEERABLE(fx.client), NULL, "steer me", &outcome);
    g_assert_cmpint(outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_SEND_FAILED);
    g_clear_error(&outcome.error);

    log_path = g_build_filename(fx.dir, "tmux.log", NULL);
    g_file_get_contents(log_path, &log, NULL, NULL);
    g_assert_nonnull(strstr(log, "C-u"));

    tmux_teardown(&fx);
}

/* Gone from the box, never queued: nothing can decide -- INDETERMINATE. */
static void
test_tmux_steer_indeterminate(void)
{
    TmuxFixture fx;
    SteerOutcome outcome = { 0 };

    tmux_setup(&fx);
    tmux_touch(&fx, "pane", "  earlier output\n>\n  status line\n");

    steer_now(AI_STEERABLE(fx.client), NULL, "steer me", &outcome);
    g_assert_no_error(outcome.error);
    g_assert_cmpint(outcome.result, ==, AI_STEER_INDETERMINATE);

    tmux_teardown(&fx);
}

/* The paste itself failed: REFUSED, and no Enter was pressed. */
static void
test_tmux_steer_refused_when_paste_fails(void)
{
    TmuxFixture fx;
    SteerOutcome outcome = { 0 };
    g_autofree gchar *log = NULL;
    g_autofree gchar *log_path = NULL;

    tmux_setup(&fx);
    tmux_touch(&fx, "paste-fails", "");
    tmux_touch(&fx, "accept", "");

    steer_now(AI_STEERABLE(fx.client), NULL, "steer me", &outcome);
    g_assert_cmpint(outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_SEND_FAILED);
    g_clear_error(&outcome.error);

    log_path = g_build_filename(fx.dir, "tmux.log", NULL);
    g_file_get_contents(log_path, &log, NULL, NULL);
    g_assert_null(strstr(log, "Enter"));

    tmux_teardown(&fx);
}

/* Settled with nothing queued: the live turn closes and steers refuse. */
static void
test_tmux_steer_refused_after_settle(void)
{
    TmuxFixture fx;
    SteerOutcome outcome = { 0 };

    tmux_setup(&fx);
    g_assert_true(ai_claude_tmux_client_settle_live_turn(fx.client, fx.jsonl, 0));
    g_assert_null(ai_steerable_dup_turn_id(AI_STEERABLE(fx.client)));

    steer_now(AI_STEERABLE(fx.client), NULL, "too late", &outcome);
    g_assert_cmpint(outcome.result, ==, AI_STEER_REFUSED);
    g_assert_error(outcome.error, AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN);
    g_clear_error(&outcome.error);

    tmux_teardown(&fx);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ai-glib/steerable/can-steer-is-a-class-datum",
                    test_can_steer_is_a_class_datum);
    g_test_add_func("/ai-glib/steerable/cannot-steer-refuses",
                    test_cannot_steer_refuses);
    g_test_add_func("/ai-glib/steerable/result-names", test_result_names);

    g_test_add_func("/ai-glib/steerable/claude-code/streams-with-open-input",
                    test_claude_code_streams_with_open_input);
    g_test_add_func("/ai-glib/steerable/claude-code/steering-is-opt-in",
                    test_claude_code_steering_is_opt_in);
    g_test_add_func("/ai-glib/steerable/claude-code/delivered",
                    test_claude_code_steer_delivered);
    g_test_add_func("/ai-glib/steerable/claude-code/indeterminate",
                    test_claude_code_steer_indeterminate);
    g_test_add_func("/ai-glib/steerable/claude-code/refused-when-input-closed",
                    test_claude_code_steer_refused_when_input_closed);
    g_test_add_func("/ai-glib/steerable/claude-code/refused-after-result",
                    test_claude_code_steer_refused_after_result);
    g_test_add_func("/ai-glib/steerable/claude-code/turn-mismatch",
                    test_claude_code_steer_turn_mismatch);
    g_test_add_func("/ai-glib/steerable/claude-code/without-a-turn",
                    test_claude_code_steer_without_a_turn);

    g_test_add_func("/ai-glib/steerable/executor/splices-after-the-batch",
                    test_executor_splices_after_the_batch);
    g_test_add_func("/ai-glib/steerable/executor/extends-a-final-answer",
                    test_executor_steer_extends_a_final_answer);
    g_test_add_func("/ai-glib/steerable/executor/unspliced-words-are-refused",
                    test_executor_unspliced_words_are_refused);
    g_test_add_func("/ai-glib/steerable/executor/without-a-run",
                    test_executor_steer_without_a_run);

    g_test_add_func("/ai-glib/steerable/claude-tmux/queue-operations",
                    test_tmux_queue_operations);
    g_test_add_func("/ai-glib/steerable/claude-tmux/delivered",
                    test_tmux_steer_delivered);
    g_test_add_func("/ai-glib/steerable/claude-tmux/refused-when-still-in-draft",
                    test_tmux_steer_refused_when_still_in_draft);
    g_test_add_func("/ai-glib/steerable/claude-tmux/indeterminate",
                    test_tmux_steer_indeterminate);
    g_test_add_func("/ai-glib/steerable/claude-tmux/refused-when-paste-fails",
                    test_tmux_steer_refused_when_paste_fails);
    g_test_add_func("/ai-glib/steerable/claude-tmux/refused-after-settle",
                    test_tmux_steer_refused_after_settle);

    return g_test_run();
}
