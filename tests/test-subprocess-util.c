/*
 * test-subprocess-util.c - Unit tests for the bounded subprocess helper
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * ai_subprocess_communicate_utf8_bounded() exists so that no wedged
 * child (or fd-inheriting grandchild) can ever block a caller
 * forever — the regression these tests lock in is the libreclaw
 * incident where one hung tmux invocation froze a session for hours.
 * Each hang test asserts both the error code AND that the call
 * returned promptly.
 */

#include <glib.h>
#include <gio/gio.h>

#include "core/ai-subprocess-util.h"
#include "core/ai-error.h"

/*
 * Generous wall-clock ceiling for "returned promptly": far above the
 * deadlines used below (with their 2 s pipe-drain grace), far below
 * the sleeps the stuck children would take.
 */
#define PROMPT_RETURN_US (10 * G_USEC_PER_SEC)

static GSubprocess *
spawn_argv(const gchar * const *argv)
{
	g_autoptr(GError) error = NULL;
	GSubprocess *sub;

	sub = g_subprocess_newv(argv,
	                        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                        G_SUBPROCESS_FLAGS_STDERR_PIPE,
	                        &error);
	g_assert_no_error(error);
	g_assert_nonnull(sub);
	return sub;
}

/*
 * Happy path: a fast command completes well inside the deadline and
 * its output comes through untouched.
 */
static void
test_bounded_fast_command(void)
{
	const gchar *argv[] = { "sh", "-c", "echo out; echo err >&2", NULL };
	g_autoptr(GSubprocess) sub = spawn_argv(argv);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	gboolean ok;

	ok = ai_subprocess_communicate_utf8_bounded(sub, NULL, 30000, NULL,
	                                            &out, &err, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	g_assert_true(g_subprocess_get_successful(sub));
	g_assert_cmpstr(out, ==, "out\n");
	g_assert_cmpstr(err, ==, "err\n");
}

/*
 * stdin is piped through to the child.
 */
static void
test_bounded_stdin_roundtrip(void)
{
	const gchar *argv[] = { "cat", NULL };
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	GSubprocess *sub;
	gboolean ok;

	sub = g_subprocess_newv(argv,
	                        G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                        G_SUBPROCESS_FLAGS_STDOUT_PIPE,
	                        &error);
	g_assert_no_error(error);

	ok = ai_subprocess_communicate_utf8_bounded(sub, "hello bound",
	                                            30000, NULL,
	                                            &out, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	g_assert_cmpstr(out, ==, "hello bound");
	g_object_unref(sub);
}

/*
 * The deadline fires: the child is killed, AI_ERROR_TIMEOUT comes
 * back, and the call returns promptly instead of after the child's
 * 100 s sleep.
 */
static void
test_bounded_timeout_kills_child(void)
{
	const gchar *argv[] = { "sleep", "100", NULL };
	g_autoptr(GSubprocess) sub = spawn_argv(argv);
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();
	gboolean ok;

	ok = ai_subprocess_communicate_utf8_bounded(sub, NULL, 200, NULL,
	                                            NULL, NULL, &error);
	g_assert_false(ok);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - start, <, PROMPT_RETURN_US);

	/* The child must actually be dead — killed, not exited. */
	g_assert_true(g_subprocess_get_if_signaled(sub));
}

/*
 * An already-cancelled cancellable aborts immediately with
 * AI_ERROR_CANCELLED.
 */
static void
test_bounded_precancelled(void)
{
	const gchar *argv[] = { "sleep", "100", NULL };
	g_autoptr(GSubprocess) sub = spawn_argv(argv);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();
	gboolean ok;

	g_cancellable_cancel(cancellable);

	ok = ai_subprocess_communicate_utf8_bounded(sub, NULL, 0,
	                                            cancellable,
	                                            NULL, NULL, &error);
	g_assert_false(ok);
	g_assert_error(error, AI_ERROR, AI_ERROR_CANCELLED);
	g_assert_cmpint(g_get_monotonic_time() - start, <, PROMPT_RETURN_US);
}

static gpointer
cancel_after_100ms(gpointer data)
{
	g_usleep(100 * 1000);
	g_cancellable_cancel(G_CANCELLABLE(data));
	return NULL;
}

/*
 * Cancellation from another thread mid-wait unblocks the call and
 * kills the child.  This is the !stop / watchdog path.
 */
static void
test_bounded_cancel_unblocks(void)
{
	const gchar *argv[] = { "sleep", "100", NULL };
	g_autoptr(GSubprocess) sub = spawn_argv(argv);
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();
	GThread *thread;
	gboolean ok;

	thread = g_thread_new("canceller", cancel_after_100ms, cancellable);

	ok = ai_subprocess_communicate_utf8_bounded(sub, NULL, 0,
	                                            cancellable,
	                                            NULL, NULL, &error);
	g_thread_join(thread);

	g_assert_false(ok);
	g_assert_error(error, AI_ERROR, AI_ERROR_CANCELLED);
	g_assert_cmpint(g_get_monotonic_time() - start, <, PROMPT_RETURN_US);
	g_assert_true(g_subprocess_get_if_signaled(sub));
}

/*
 * The nasty case that plain kill-on-timeout does NOT cure: a
 * grandchild inherited the pipes and keeps them open after the
 * direct child dies, so EOF never arrives.  The grace escalation
 * must abandon the pipes and return anyway.
 */
static void
test_bounded_grandchild_holds_pipe(void)
{
	const gchar *argv[] = {
		"sh", "-c", "sleep 30 & exec sleep 30", NULL
	};
	g_autoptr(GSubprocess) sub = spawn_argv(argv);
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();
	gboolean ok;

	ok = ai_subprocess_communicate_utf8_bounded(sub, NULL, 200, NULL,
	                                            NULL, NULL, &error);
	g_assert_false(ok);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - start, <, PROMPT_RETURN_US);
}

/*
 * A failing-but-fast command is NOT the helper's business: it
 * returns TRUE (communication completed) and the caller reads the
 * exit status, exactly like the GLib original.
 */
static void
test_bounded_nonzero_exit_is_not_an_error(void)
{
	const gchar *argv[] = { "sh", "-c", "exit 7", NULL };
	g_autoptr(GSubprocess) sub = spawn_argv(argv);
	g_autoptr(GError) error = NULL;
	gboolean ok;

	ok = ai_subprocess_communicate_utf8_bounded(sub, NULL, 30000, NULL,
	                                            NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	g_assert_false(g_subprocess_get_successful(sub));
	g_assert_cmpint(g_subprocess_get_exit_status(sub), ==, 7);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/bounded-communicate/fast-command",
	                test_bounded_fast_command);
	g_test_add_func("/ai-glib/bounded-communicate/stdin-roundtrip",
	                test_bounded_stdin_roundtrip);
	g_test_add_func("/ai-glib/bounded-communicate/timeout-kills-child",
	                test_bounded_timeout_kills_child);
	g_test_add_func("/ai-glib/bounded-communicate/precancelled",
	                test_bounded_precancelled);
	g_test_add_func("/ai-glib/bounded-communicate/cancel-unblocks",
	                test_bounded_cancel_unblocks);
	g_test_add_func("/ai-glib/bounded-communicate/grandchild-holds-pipe",
	                test_bounded_grandchild_holds_pipe);
	g_test_add_func("/ai-glib/bounded-communicate/nonzero-exit-not-error",
	                test_bounded_nonzero_exit_is_not_an_error);

	return g_test_run();
}
