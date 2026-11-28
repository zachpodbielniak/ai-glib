/*
 * test-cli-late-turn.c - A turn the CLI runs by itself after a background
 *                        task finishes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * VENTURE #58.  Claude Code can end a turn -- write its `result` -- with a
 * sub-agent or background shell still running.  It does not exit then: it
 * waits, and when the work finishes it runs a second turn by itself (the
 * `<task-notification>` is enqueued inside the CLI, no prompt arrives) and
 * writes a second `result`.  The reader read all of it to EOF, but the
 * parser kept only the first result's text, so the second turn -- in the
 * case that found this, a security review's full report -- was read and
 * thrown away, and the task waiting on it sat "running" for good.
 *
 * A fake `claude` speaks just enough stream-json for that, by mode:
 *
 *   late     task_started, an interim answer and its result; a second
 *            later, task_notification, the final report and its result
 *   plain    one answer, one result, no background work
 *   awaited  a foreground sub-agent (awaited) that never reports back,
 *            then one answer -- must end exactly like plain
 *   stuck    task_started, an interim answer and its result, then the
 *            work never reports back and the CLI never exits
 *
 * Every mode records its pid, so a test can check the child is gone.
 *
 * The first assertion of test_late_turn_reaches_the_caller() is the one
 * that failed before the fix: the response read "INTERIM" alone.  The rest
 * of this file uses API that came with the fix; it is compiled out when
 * that API is absent, which is how the first assertion is shown failing
 * against the old library (see docs/providers/claude-code.org).
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>

#include "ai-glib.h"

#define FAKE_CLAUDE                                                            \
    "#!/bin/sh\n"                                                              \
    "d='%s'\n"                                                                 \
    "mode=$(cat \"$d/mode\")\n"                                                \
    "echo $$ > \"$d/pid\"\n"                                                   \
    "IFS= read -r first || true\n"                                             \
    "say() { printf '{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"%%s\"}]}}\\n' \"$1\"; }\n" \
    "result() { printf '{\"type\":\"result\",\"subtype\":\"success\",\"result\":\"%%s\",\"session_id\":\"s-late\",\"usage\":{\"input_tokens\":1,\"output_tokens\":1}}\\n' \"$1\"; }\n" \
    "started() { printf '{\"type\":\"system\",\"subtype\":\"task_started\",\"task_id\":\"%%s\",\"tool_use_id\":\"toolu_1\",\"description\":\"%%s\"%%s}\\n' \"$1\" \"$2\" \"$3\"; }\n" \
    "notified() { printf '{\"type\":\"system\",\"subtype\":\"task_notification\",\"task_id\":\"%%s\",\"status\":\"completed\"}\\n' \"$1\"; }\n" \
    "echo '{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"s-late\"}'\n" \
    "case \"$mode\" in\n"                                                      \
    "  late)    started t-bg 'Review the bump' ',\"is_backgrounded\":true'\n"  \
    "           say INTERIM; result INTERIM\n"                                 \
    "           sleep 1\n"                                                     \
    "           notified t-bg\n"                                               \
    "           say 'FINAL REPORT'; result 'FINAL REPORT' ;;\n"                \
    "  plain)   say ONLY; result ONLY ;;\n"                                    \
    "  awaited) started t-fg 'Look it up' ',\"awaited\":true'\n"               \
    "           say ONLY; result ONLY ;;\n"                                    \
    "  stuck)   started t-bg 'Review the bump' ''\n"                           \
    "           say INTERIM; result INTERIM\n"                                 \
    "           exec sleep 60 ;;\n"                                            \
    "esac\n"

typedef struct
{
    gchar              *dir;
    AiClaudeCodeClient *client;

    gboolean            done;
    AiResponse         *response;
    GError             *error;

    guint               continued;
    gchar              *continued_text;
    guint               abandoned;
    gchar              *abandoned_text;
    guint               stream_ends;
} Fixture;

static gboolean
on_deadline(gpointer user_data)
{
    gboolean *expired = user_data;

    *expired = TRUE;
    return G_SOURCE_REMOVE;
}

/* Iterate the default context until @flag is set, failing after 30 s. */
static void
iterate_until(gboolean *flag)
{
    gboolean expired = FALSE;
    guint id = g_timeout_add(30000, on_deadline, &expired);

    while (!*flag && !expired)
        g_main_context_iteration(NULL, TRUE);

    if (!expired)
        g_source_remove(id);
    g_assert_false(expired);
}

static gchar *
fixture_path(Fixture *fx, const gchar *name)
{
    return g_build_filename(fx->dir, name, NULL);
}

static void
fixture_setup(Fixture *fx, const gchar *mode, gboolean steerable)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *script = NULL;
    g_autofree gchar *cli = NULL;
    g_autofree gchar *mode_path = NULL;

    memset(fx, 0, sizeof(*fx));
    fx->dir = g_dir_make_tmp("ai-glib-late-turn-XXXXXX", &error);
    g_assert_no_error(error);

    cli = fixture_path(fx, "claude");
    script = g_strdup_printf(FAKE_CLAUDE, fx->dir);
    g_file_set_contents(cli, script, -1, &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_chmod(cli, 0700), ==, 0);

    mode_path = fixture_path(fx, "mode");
    g_file_set_contents(mode_path, mode, -1, &error);
    g_assert_no_error(error);

    fx->client = ai_claude_code_client_new();
    g_object_set(fx->client, "steerable", steerable, NULL);
    ai_cli_client_set_executable_path(AI_CLI_CLIENT(fx->client), cli);
    ai_cli_client_set_session_persistence(AI_CLI_CLIENT(fx->client), FALSE);
    /* A run that waits for ever fails here instead of hanging the suite. */
    ai_cli_client_set_process_timeout_ms(AI_CLI_CLIENT(fx->client), 20000);
}

static void
fixture_teardown(Fixture *fx)
{
    const gchar *names[] = { "claude", "mode", "pid", NULL };
    guint i;

    for (i = 0; names[i] != NULL; i++)
    {
        g_autofree gchar *path = fixture_path(fx, names[i]);
        g_unlink(path);
    }
    g_rmdir(fx->dir);
    g_free(fx->dir);
    g_clear_object(&fx->client);
    g_clear_object(&fx->response);
    g_clear_error(&fx->error);
    g_free(fx->continued_text);
    g_free(fx->abandoned_text);
}

static void
on_event(AiEventSource *source, AiEvent *event, gpointer user_data)
{
    Fixture *fx = user_data;

    (void)source;

    switch ((gint)ai_event_get_kind(event))
    {
#ifdef AI_CLI_CLIENT_DEFAULT_BACKGROUND_WAIT_MS
        case AI_EVENT_TURN_CONTINUED:
            fx->continued++;
            g_free(fx->continued_text);
            fx->continued_text = g_strdup(ai_event_get_text(event));
            break;
        case AI_EVENT_BACKGROUND_ABANDONED:
            fx->abandoned++;
            g_free(fx->abandoned_text);
            fx->abandoned_text = g_strdup(ai_event_get_text(event));
            break;
#endif
        case AI_EVENT_STREAM_END:
            fx->stream_ends++;
            break;
        default:
            break;
    }
}

static void
on_stream(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Fixture *fx = user_data;

    fx->response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
                                                    result, &fx->error);
    fx->done = TRUE;
}

/* One streamed turn, as libreclaw runs it. */
static gchar *
fixture_run(Fixture *fx)
{
    g_autoptr(AiMessage) message = ai_message_new_user("review it");
    GList *messages = g_list_append(NULL, message);
    gulong handler;

    handler = g_signal_connect(fx->client, "event", G_CALLBACK(on_event), fx);

    ai_streamable_chat_stream_async(AI_STREAMABLE(fx->client), messages, NULL,
                                    0, NULL, NULL, on_stream, fx);
    iterate_until(&fx->done);

    g_signal_handler_disconnect(fx->client, handler);
    g_list_free(messages);

    g_assert_no_error(fx->error);
    g_assert_nonnull(fx->response);

    return ai_response_get_text(fx->response);
}

/* Whether the fake's process is gone (reaped, or never existed). */
static gboolean
fixture_child_gone(Fixture *fx)
{
    g_autofree gchar *path = fixture_path(fx, "pid");
    g_autofree gchar *contents = NULL;
    gint64 pid;
    gint i;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return TRUE;

    pid = g_ascii_strtoll(contents, NULL, 10);
    g_assert_cmpint(pid, >, 1);

    /* The kill lands at once; the reap is the child watch's, a moment on. */
    for (i = 0; i < 100; i++)
    {
        if (kill((pid_t)pid, 0) != 0)
            return TRUE;
        g_main_context_iteration(NULL, FALSE);
        g_usleep(20000);
    }

    return FALSE;
}

/*
 * The case that lost the report.  The later turn's text reaches whoever is
 * waiting on the run, after the earlier one, as one answer.
 */
static void
run_late_turn(gboolean steerable)
{
    Fixture fx;
    g_autofree gchar *text = NULL;
    gint64 started;

    fixture_setup(&fx, "late", steerable);

    started = g_get_monotonic_time();
    text = fixture_run(&fx);

    /* Failed before the fix: the response read "INTERIM" alone. */
    g_assert_cmpstr(text, ==, "INTERIM\n\nFINAL REPORT");

    /* It waited for the second turn rather than ending at the first. */
    g_assert_cmpint(g_get_monotonic_time() - started, >=, G_USEC_PER_SEC);
    g_assert_cmpuint(fx.stream_ends, ==, 1);

#ifdef AI_CLI_CLIENT_DEFAULT_BACKGROUND_WAIT_MS
    /* The boundary was announced once, naming the work that finished. */
    g_assert_cmpuint(fx.continued, ==, 1);
    g_assert_nonnull(strstr(fx.continued_text, "t-bg"));
    g_assert_cmpuint(fx.abandoned, ==, 0);
#endif

    g_assert_true(fixture_child_gone(&fx));
    fixture_teardown(&fx);
}

static void
test_late_turn_reaches_the_caller(void)
{
    run_late_turn(FALSE);
}

/* The same through a steerable turn, whose stdin closes at the result. */
static void
test_late_turn_reaches_the_caller_steerable(void)
{
    run_late_turn(TRUE);
}

#ifdef AI_CLI_CLIENT_DEFAULT_BACKGROUND_WAIT_MS

/* No background work: exactly the turn there always was. */
static void
test_plain_turn_is_unchanged(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;
    gint64 started;

    fixture_setup(&fx, "plain", TRUE);

    started = g_get_monotonic_time();
    text = fixture_run(&fx);

    g_assert_cmpstr(text, ==, "ONLY");
    g_assert_cmpuint(fx.continued, ==, 0);
    g_assert_cmpuint(fx.abandoned, ==, 0);
    g_assert_cmpuint(fx.stream_ends, ==, 1);
    g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
    fixture_teardown(&fx);
}

/*
 * A foreground sub-agent that the CLI says it awaited is not background
 * work, even when no notification for it arrives: nothing is waited for.
 */
static void
test_awaited_task_is_not_waited_for(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, "awaited", FALSE);
    /* Were it tracked, this wait would trip and say so. */
    ai_cli_client_set_background_wait_ms(AI_CLI_CLIENT(fx.client), 1);

    text = fixture_run(&fx);

    g_assert_cmpstr(text, ==, "ONLY");
    g_assert_cmpuint(fx.continued, ==, 0);
    g_assert_cmpuint(fx.abandoned, ==, 0);
    fixture_teardown(&fx);
}

/*
 * The bound.  Background work that never reports back does not hold the
 * run: at background-wait-ms the CLI is stopped and the run ends with the
 * turn it had, plus a note naming the work -- not an error, which would
 * throw that turn away, and not a wait for ever.
 */
static void
test_background_wait_trips(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;
    gint64 started;
    gint64 elapsed;

    fixture_setup(&fx, "stuck", TRUE);
    ai_cli_client_set_background_wait_ms(AI_CLI_CLIENT(fx.client), 500);

    started = g_get_monotonic_time();
    text = fixture_run(&fx);
    elapsed = g_get_monotonic_time() - started;

    /* Well short of the fake's 60 s and the 20 s process deadline. */
    g_assert_cmpint(elapsed, >=, 500 * 1000);
    g_assert_cmpint(elapsed, <, 10 * G_USEC_PER_SEC);

    g_assert_true(g_str_has_prefix(text, "INTERIM\n\n[ai-glib] "));
    g_assert_nonnull(strstr(text, "1 background task still running"));
    g_assert_nonnull(strstr(text, "t-bg (Review the bump)"));

    g_assert_cmpuint(fx.abandoned, ==, 1);
    g_assert_nonnull(strstr(fx.abandoned_text, "t-bg (Review the bump)"));
    g_assert_cmpuint(fx.continued, ==, 0);
    g_assert_cmpuint(fx.stream_ends, ==, 1);

    g_assert_true(fixture_child_gone(&fx));
    fixture_teardown(&fx);
}

/* 0 is "as long as the CLI runs": the late turn still arrives. */
static void
test_background_wait_zero_waits(void)
{
    Fixture fx;
    g_autofree gchar *text = NULL;

    fixture_setup(&fx, "late", FALSE);
    ai_cli_client_set_background_wait_ms(AI_CLI_CLIENT(fx.client), 0);

    text = fixture_run(&fx);

    g_assert_cmpstr(text, ==, "INTERIM\n\nFINAL REPORT");
    g_assert_cmpuint(fx.abandoned, ==, 0);
    fixture_teardown(&fx);
}

/* The property: its default, and that it is a property. */
static void
test_background_wait_property(void)
{
    g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
    gint wait_ms = -1;

    g_assert_cmpint(ai_cli_client_get_background_wait_ms(AI_CLI_CLIENT(client)),
                    ==, AI_CLI_CLIENT_DEFAULT_BACKGROUND_WAIT_MS);
    g_assert_cmpint(AI_CLI_CLIENT_DEFAULT_BACKGROUND_WAIT_MS, ==, 1800000);

    g_object_set(client, "background-wait-ms", 1234, NULL);
    g_object_get(client, "background-wait-ms", &wait_ms, NULL);
    g_assert_cmpint(wait_ms, ==, 1234);
}

/*
 * Cancelling while the run waits for background work ends it at once and
 * kills the child; nothing fires into the finished run afterwards.
 */
static gboolean
cancel_soon(gpointer user_data)
{
    g_cancellable_cancel(G_CANCELLABLE(user_data));
    return G_SOURCE_REMOVE;
}

static void
test_cancel_while_waiting(void)
{
    Fixture fx;
    g_autoptr(AiMessage) message = ai_message_new_user("review it");
    g_autoptr(GCancellable) cancellable = g_cancellable_new();
    GList *messages = g_list_append(NULL, message);
    gboolean settled = FALSE;
    guint id;

    fixture_setup(&fx, "stuck", TRUE);
    ai_cli_client_set_background_wait_ms(AI_CLI_CLIENT(fx.client), 2000);

    g_timeout_add(500, cancel_soon, cancellable);
    ai_streamable_chat_stream_async(AI_STREAMABLE(fx.client), messages, NULL,
                                    0, NULL, cancellable, on_stream, &fx);
    iterate_until(&fx.done);
    g_list_free(messages);

    g_assert_null(fx.response);
    g_assert_nonnull(fx.error);
    g_assert_true(fixture_child_gone(&fx));

    /* Past the wait: the destroyed source must not fire. */
    id = g_timeout_add(2500, on_deadline, &settled);
    iterate_until(&settled);
    (void)id;
    g_assert_cmpuint(fx.abandoned, ==, 0);

    fixture_teardown(&fx);
}

#endif

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ai-glib/cli-late-turn/reaches-the-caller",
                    test_late_turn_reaches_the_caller);
    g_test_add_func("/ai-glib/cli-late-turn/reaches-the-caller-steerable",
                    test_late_turn_reaches_the_caller_steerable);
#ifdef AI_CLI_CLIENT_DEFAULT_BACKGROUND_WAIT_MS
    g_test_add_func("/ai-glib/cli-late-turn/plain-turn-unchanged",
                    test_plain_turn_is_unchanged);
    g_test_add_func("/ai-glib/cli-late-turn/awaited-task-not-waited-for",
                    test_awaited_task_is_not_waited_for);
    g_test_add_func("/ai-glib/cli-late-turn/background-wait-trips",
                    test_background_wait_trips);
    g_test_add_func("/ai-glib/cli-late-turn/background-wait-zero-waits",
                    test_background_wait_zero_waits);
    g_test_add_func("/ai-glib/cli-late-turn/background-wait-property",
                    test_background_wait_property);
    g_test_add_func("/ai-glib/cli-late-turn/cancel-while-waiting",
                    test_cancel_while_waiting);
#endif

    return g_test_run();
}
