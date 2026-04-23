/*
 * test-cli-client.c - Unit tests for AiCliClient base class
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#include "core/ai-cli-client.h"
#include "core/ai-config.h"

/*
 * Test that AiCliClient is a valid GType.
 */
static void
test_cli_client_gtype(void)
{
	GType type;

	type = ai_cli_client_get_type();
	g_assert_true(G_TYPE_IS_OBJECT(type));
	g_assert_cmpstr(g_type_name(type), ==, "AiCliClient");
}

/*
 * Test that AiCliClient is derivable (cannot be instantiated directly).
 */
static void
test_cli_client_derivable(void)
{
	GType type;

	type = ai_cli_client_get_type();
	/* AiCliClient is derivable, not instantiable directly */
	g_assert_true(G_TYPE_IS_DERIVABLE(type));
}

/* ================================================================== */
/* ai_cli_client_format_exit_error                                     */
/*                                                                     */
/* Covers every combination of NULL / empty / non-empty stderr +      */
/* stdout that a misbehaving CLI can produce.  Regression guard for   */
/* the bug where "CLI exited with status 1: " (trailing empty) hid    */
/* the real cause because only NULL was checked, not empty string.   */
/* ================================================================== */

static void
test_format_exit_error_stderr_wins(void)
{
	g_autofree gchar *msg = NULL;

	/* Both present — stderr takes priority (conventional error stream). */
	msg = ai_cli_client_format_exit_error(1, "stderr msg", "stdout msg");
	g_assert_cmpstr(msg, ==, "CLI exited with status 1: stderr msg");
}

static void
test_format_exit_error_stderr_only(void)
{
	g_autofree gchar *msg = NULL;

	msg = ai_cli_client_format_exit_error(2, "auth failure", NULL);
	g_assert_cmpstr(msg, ==, "CLI exited with status 2: auth failure");
}

static void
test_format_exit_error_stdout_fallback_when_stderr_null(void)
{
	g_autofree gchar *msg = NULL;

	/* stderr NULL → fall back to stdout. */
	msg = ai_cli_client_format_exit_error(1, NULL, "usage: cli ...");
	g_assert_cmpstr(msg, ==, "CLI exited with status 1: usage: cli ...");
}

static void
test_format_exit_error_stdout_fallback_when_stderr_empty(void)
{
	g_autofree gchar *msg = NULL;

	/* REGRESSION: the original bug.  Empty-string stderr must not be
	 * treated as "stderr has content" — fall through to stdout. */
	msg = ai_cli_client_format_exit_error(1, "", "error: bad flag");
	g_assert_cmpstr(msg, ==, "CLI exited with status 1: error: bad flag");
}

static void
test_format_exit_error_both_null(void)
{
	g_autofree gchar *msg = NULL;

	/* No diagnostic anywhere — sentinel so the user doesn't see a
	 * trailing-colon message. */
	msg = ai_cli_client_format_exit_error(1, NULL, NULL);
	g_assert_cmpstr(msg, ==,
		"CLI exited with status 1: (no output on stderr or stdout)");
}

static void
test_format_exit_error_both_empty(void)
{
	g_autofree gchar *msg = NULL;

	/* Both empty-string → still the sentinel. */
	msg = ai_cli_client_format_exit_error(1, "", "");
	g_assert_cmpstr(msg, ==,
		"CLI exited with status 1: (no output on stderr or stdout)");
}

static void
test_format_exit_error_stderr_null_stdout_empty(void)
{
	g_autofree gchar *msg = NULL;

	/* stderr missing, stdout empty → sentinel. */
	msg = ai_cli_client_format_exit_error(1, NULL, "");
	g_assert_cmpstr(msg, ==,
		"CLI exited with status 1: (no output on stderr or stdout)");
}

static void
test_format_exit_error_stderr_empty_stdout_null(void)
{
	g_autofree gchar *msg = NULL;

	/* Inverse of above — still sentinel. */
	msg = ai_cli_client_format_exit_error(1, "", NULL);
	g_assert_cmpstr(msg, ==,
		"CLI exited with status 1: (no output on stderr or stdout)");
}

static void
test_format_exit_error_non_zero_exit_statuses(void)
{
	g_autofree gchar *msg1 = NULL;
	g_autofree gchar *msg127 = NULL;
	g_autofree gchar *msg_neg = NULL;

	/* Different exit codes format correctly — no magic-number
	 * assumptions about status being 1. */
	msg1 = ai_cli_client_format_exit_error(1, "e", NULL);
	g_assert_cmpstr(msg1, ==, "CLI exited with status 1: e");

	/* 127 = "command not found" in POSIX shells. */
	msg127 = ai_cli_client_format_exit_error(127, "not found", NULL);
	g_assert_cmpstr(msg127, ==, "CLI exited with status 127: not found");

	/* Negative values (e.g. if caller misinterprets signal death). */
	msg_neg = ai_cli_client_format_exit_error(-1, "signaled", NULL);
	g_assert_cmpstr(msg_neg, ==, "CLI exited with status -1: signaled");
}

static void
test_format_exit_error_exit_status_zero(void)
{
	g_autofree gchar *msg = NULL;

	/* In practice the caller only invokes this for non-zero exits,
	 * but the helper must still format 0 without crashing — it's
	 * a pure formatter, not a decision function. */
	msg = ai_cli_client_format_exit_error(0, "weird but ok", NULL);
	g_assert_cmpstr(msg, ==, "CLI exited with status 0: weird but ok");
}

static void
test_format_exit_error_trailing_newline_preserved(void)
{
	g_autofree gchar *msg = NULL;

	/* CLIs typically emit stderr with a trailing '\n'.  We don't
	 * strip it — callers log these with g_warning/g_set_error which
	 * tolerates embedded newlines.  Test locks in the behavior so a
	 * future refactor doesn't quietly change it. */
	msg = ai_cli_client_format_exit_error(1, "boom\n", NULL);
	g_assert_cmpstr(msg, ==, "CLI exited with status 1: boom\n");
}

static void
test_format_exit_error_multiline_stderr(void)
{
	g_autofree gchar *msg = NULL;

	msg = ai_cli_client_format_exit_error(1,
		"Error: something failed\n"
		"  at path/to/file:42\n"
		"  called from other:10\n",
		NULL);

	g_assert_cmpstr(msg, ==,
		"CLI exited with status 1: Error: something failed\n"
		"  at path/to/file:42\n"
		"  called from other:10\n");
}

static void
test_format_exit_error_utf8_content(void)
{
	g_autofree gchar *msg = NULL;

	/* Non-ASCII detail must round-trip unchanged.  g_strdup_printf
	 * is byte-based for %s so UTF-8 sequences pass through. */
	msg = ai_cli_client_format_exit_error(1,
		"\xE2\x9D\x8C failed\xE2\x80\xA6", NULL);
	g_assert_cmpstr(msg, ==,
		"CLI exited with status 1: \xE2\x9D\x8C failed\xE2\x80\xA6");
}

static void
test_format_exit_error_single_char_stderr(void)
{
	g_autofree gchar *msg = NULL;

	/* Boundary: one-character stderr is non-empty and should be
	 * used as the detail, not fall through to stdout. */
	msg = ai_cli_client_format_exit_error(1, "x", "stdout msg");
	g_assert_cmpstr(msg, ==, "CLI exited with status 1: x");
}

static void
test_format_exit_error_idempotent(void)
{
	g_autofree gchar *a = NULL;
	g_autofree gchar *b = NULL;

	/* Pure function — identical inputs must produce identical output. */
	a = ai_cli_client_format_exit_error(5, "x", "y");
	b = ai_cli_client_format_exit_error(5, "x", "y");
	g_assert_cmpstr(a, ==, b);
}

static void
test_format_exit_error_returns_new_allocation(void)
{
	gchar *a;
	gchar *b;

	/* Not idempotent on pointer identity — each call returns a
	 * distinct allocation that the caller owns. */
	a = ai_cli_client_format_exit_error(1, "x", NULL);
	b = ai_cli_client_format_exit_error(1, "x", NULL);

	g_assert_nonnull(a);
	g_assert_nonnull(b);
	g_assert_true(a != b);

	g_free(a);
	g_free(b);
}

int
main(
	int   argc,
	char *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/cli-client/gtype", test_cli_client_gtype);
	g_test_add_func("/ai-glib/cli-client/derivable", test_cli_client_derivable);

	/* ai_cli_client_format_exit_error coverage */
	g_test_add_func("/ai-glib/cli-client/format-exit-error/stderr-wins",
	                test_format_exit_error_stderr_wins);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/stderr-only",
	                test_format_exit_error_stderr_only);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/stdout-fallback-when-stderr-null",
	                test_format_exit_error_stdout_fallback_when_stderr_null);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/stdout-fallback-when-stderr-empty",
	                test_format_exit_error_stdout_fallback_when_stderr_empty);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/both-null",
	                test_format_exit_error_both_null);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/both-empty",
	                test_format_exit_error_both_empty);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/stderr-null-stdout-empty",
	                test_format_exit_error_stderr_null_stdout_empty);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/stderr-empty-stdout-null",
	                test_format_exit_error_stderr_empty_stdout_null);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/non-zero-exit-statuses",
	                test_format_exit_error_non_zero_exit_statuses);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/exit-status-zero",
	                test_format_exit_error_exit_status_zero);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/trailing-newline-preserved",
	                test_format_exit_error_trailing_newline_preserved);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/multiline-stderr",
	                test_format_exit_error_multiline_stderr);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/utf8-content",
	                test_format_exit_error_utf8_content);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/single-char-stderr",
	                test_format_exit_error_single_char_stderr);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/idempotent",
	                test_format_exit_error_idempotent);
	g_test_add_func("/ai-glib/cli-client/format-exit-error/returns-new-allocation",
	                test_format_exit_error_returns_new_allocation);

	return g_test_run();
}
