/*
 * test-cli-limit.c - Every wrapped CLI's usage limit, rate limit and
 * sign-out, told apart from an ordinary failed run
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Each CLI reports a usage limit, a 429 and a lapsed login as the same
 * failed run, so the only signal is its wording.  The samples below are
 * that wording, captured from the CLIs' own source or binaries (the
 * provider files cite where each came from), and every one is run
 * through the client the way a real failure arrives: as the error a
 * provider's finish returns, and -- for one per CLI -- as the output of
 * a real subprocess standing in for the CLI.
 */

#include <glib.h>
#include <glib/gstdio.h>

#include "ai-glib.h"
#include "core/ai-cli-limit-private.h"
#include "core/ai-session-limit.h"

typedef struct
{
	const gchar *cli;
	const gchar *text;
	gint         code;
	gint64       reset_in;   /* seconds from now, 0 if none, -1 if any */
} Sample;

/* Text as each CLI prints it; see the tables in src/providers/. */
static const Sample samples[] = {
	/* codex-cli 0.163.0 (UsageLimitReachedError, RetryLimit, ...) */
	{ "codex", "You've hit your usage limit. Upgrade to Pro "
	  "(https://chatgpt.com/explore/pro), visit "
	  "https://chatgpt.com/codex/settings/usage to purchase more credits "
	  "or try again in 3h 20m.", AI_ERROR_SESSION_LIMIT, 12000 },
	{ "codex", "You've hit your usage limit. Try again at 3:45 PM.",
	  AI_ERROR_SESSION_LIMIT, -1 },
	{ "codex", "Quota exceeded. Check your plan and billing details.",
	  AI_ERROR_INSUFFICIENT_QUOTA, 0 },
	{ "codex", "exceeded retry limit, last status: 429 Too Many Requests, "
	  "request id: abc", AI_ERROR_RATE_LIMITED, 0 },
	{ "codex", "Your access token could not be refreshed because your "
	  "refresh token has expired. Please log out and sign in again.",
	  AI_ERROR_INVALID_API_KEY, 0 },
	{ "codex", "Error: no Codex credentials were found",
	  AI_ERROR_INVALID_API_KEY, 0 },

	/* grok 1.0.77 */
	{ "grok", "CLI error: You hit your weekly limit.",
	  AI_ERROR_SESSION_LIMIT, 0 },
	{ "grok", "CLI error: You've hit the rate limit for your plan. "
	  "(Retry-After: 30)", AI_ERROR_RATE_LIMITED, 30 },
	{ "grok", "CLI error: Not logged in. Run `grok login`.",
	  AI_ERROR_INVALID_API_KEY, 0 },
	{ "grok", "CLI error: run out of credits",
	  AI_ERROR_INSUFFICIENT_QUOTA, 0 },

	/* opencode v1.19.5 retry.ts / provider/error.ts, v2 to-session-error.ts */
	{ "opencode", "CLI error: APIError: Usage limit reached. It will reset "
	  "in 2 hours 5 minutes. To continue using this model now, enable "
	  "usage from your available balance", AI_ERROR_SESSION_LIMIT, 7500 },
	{ "opencode", "CLI error: APIError: Too Many Requests",
	  AI_ERROR_RATE_LIMITED, 0 },
	{ "opencode", "CLI error: ProviderAuthError: API key is missing",
	  AI_ERROR_INVALID_API_KEY, 0 },
	{ "opencode", "CLI error: {\"type\":\"provider.quota\",\"message\":"
	  "\"insufficient balance\"}", AI_ERROR_INSUFFICIENT_QUOTA, 0 },

	/* cursor agent 2026.11.13 bundle; usage limit from forum.cursor.com */
	{ "cursor", "CLI exited with status 1: Error: You\xe2\x80\x99ve hit your "
	  "usage limit", AI_ERROR_SESSION_LIMIT, 0 },
	{ "cursor", "CLI exited with status 1: RetriableError: "
	  "[resource_exhausted] Error", AI_ERROR_RATE_LIMITED, 0 },
	{ "cursor", "CLI exited with status 1: Error: Authentication required. "
	  "Please run 'agent login' first, or set CURSOR_API_KEY environment "
	  "variable.", AI_ERROR_INVALID_API_KEY, 0 },

	/* agy 1.5.9; quota text from discuss.ai.google.dev */
	{ "agy", "CLI error: Contact your administrator to enable overages. "
	  "Resets in 53h16m34s.: RESOURCE_EXHAUSTED (code 429): Individual "
	  "quota reached.", AI_ERROR_SESSION_LIMIT, 191794 },
	{ "agy", "CLI error: RESOURCE_EXHAUSTED (code 429): Too Many Requests",
	  AI_ERROR_RATE_LIMITED, 0 },
	{ "agy", "CLI error: Error: authentication required. Run 'agy' to log "
	  "in, then retry.", AI_ERROR_INVALID_API_KEY, 0 },
	{ "agy", "CLI error: Your AI credits balance is too low to continue.",
	  AI_ERROR_INSUFFICIENT_QUOTA, 0 }
};

static AiCliClient *
client_for(const gchar *cli)
{
	if (g_str_equal(cli, "codex"))
		return AI_CLI_CLIENT(ai_codex_cli_client_new());
	if (g_str_equal(cli, "grok"))
		return AI_CLI_CLIENT(ai_grok_build_client_new());
	if (g_str_equal(cli, "opencode"))
		return AI_CLI_CLIENT(ai_opencode_client_new());
	if (g_str_equal(cli, "cursor"))
		return AI_CLI_CLIENT(ai_cursor_client_new());
	if (g_str_equal(cli, "agy"))
		return AI_CLI_CLIENT(ai_antigravity_client_new());
	g_assert_not_reached();
	return NULL;
}

/* Checks a reclassified error against what its sample means. */
static void
assert_sample(const Sample *s, const GError *error, gint64 before, gint64 after)
{
	gint64 reset = 0;
	gboolean notice;

	g_assert_nonnull(error);
	g_assert_true(error->domain == AI_ERROR);
	g_assert_cmpint(error->code, ==, s->code);

	/* The CLI's own words are kept, and the CLI is named. */
	g_assert_nonnull(strstr(error->message, s->cli));

	notice = ai_session_limit_notice_parse(error->message, &reset);

	if (s->code == AI_ERROR_SESSION_LIMIT)
		g_assert_true(notice);

	if (s->reset_in > 0) {
		g_assert_true(notice);
		g_assert_cmpint(reset, >=, before + s->reset_in);
		g_assert_cmpint(reset, <=, after + s->reset_in + 1);
	} else if (s->reset_in < 0) {
		g_assert_true(notice);
		g_assert_cmpint(reset, >, before);
		g_assert_cmpint(reset, <=, after + 24 * 3600);
	} else if (notice) {
		g_assert_cmpint(reset, ==, 0);
	}
}

/*
 * Every sample, through the provider's own registration: the error a
 * finish would return, reclassified the way the finish does it.
 */
static void
test_samples_are_classified(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(samples); i++) {
		g_autoptr(AiCliClient) client = client_for(samples[i].cli);
		g_autoptr(GError) error = g_error_new_literal(
			AI_ERROR, AI_ERROR_CLI_EXECUTION, samples[i].text);
		gint64 before = g_get_real_time() / G_USEC_PER_SEC;
		gint64 after;

		ai_cli_limit_reclassify(client, &error);
		after = g_get_real_time() / G_USEC_PER_SEC;
		g_test_message("%s: %s", samples[i].cli, error->message);
		assert_sample(&samples[i], error, before, after);
	}
}

/* An ordinary failure is left exactly as it was. */
static void
test_ordinary_failure_is_untouched(void)
{
	const gchar *clis[] = { "codex", "grok", "opencode", "cursor", "agy" };
	guint i;

	for (i = 0; i < G_N_ELEMENTS(clis); i++) {
		g_autoptr(AiCliClient) client = client_for(clis[i]);
		g_autoptr(GError) error = g_error_new_literal(
			AI_ERROR, AI_ERROR_CLI_EXECUTION,
			"CLI error: The 'gpt-5.5' model is not supported");

		ai_cli_limit_reclassify(client, &error);
		g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
		g_assert_cmpstr(error->message, ==,
		                "CLI error: The 'gpt-5.5' model is not supported");
	}
}

/*
 * Only the generic CLI failure codes are read.  A timeout whose message
 * happens to quote "rate limit" is still a timeout.
 */
static void
test_other_codes_are_untouched(void)
{
	g_autoptr(AiCliClient) client = client_for("codex");
	g_autoptr(GError) error = g_error_new_literal(
		AI_ERROR, AI_ERROR_TIMEOUT, "rate limit while waiting");

	ai_cli_limit_reclassify(client, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
}

/*
 * claude-code registers nothing: its limit is recognised from the
 * synthetic message, and its stderr is not guessed at.
 */
static void
test_unregistered_cli_is_untouched(void)
{
	g_autoptr(AiCliClient) client =
		AI_CLI_CLIENT(ai_claude_code_client_new());
	g_autoptr(GError) error = g_error_new_literal(
		AI_ERROR, AI_ERROR_CLI_EXECUTION, "You've hit your usage limit");

	ai_cli_limit_reclassify(client, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
}

/*
 * The message shape, with a fixed clock: claude-code's sentence and
 * notice first, then the CLI's words on one line.
 */
static void
test_session_limit_shape(void)
{
	static const AiCliLimitPattern table[] = {
		{ AI_ERROR_SESSION_LIMIT, "usage limit" },
		{ 0, NULL }
	};
	g_autoptr(GError) error = NULL;
	g_autofree gchar *why = NULL;
	g_autofree gchar *notice = NULL;
	g_autofree gchar *expected = NULL;
	const gint64 now = G_GINT64_CONSTANT(1787659200);

	g_assert_true(ai_cli_limit_classify(table, "codex",
		"You've hit your usage limit.\nTry again in 90 minutes.",
		now, &error));
	why = ai_session_limit_format(now + 5400);
	notice = ai_session_limit_notice_new(now + 5400);
	expected = g_strdup_printf("%s %s (codex: You've hit your usage limit. "
	                           "Try again in 90 minutes.)", why, notice);
	g_assert_cmpstr(error->message, ==, expected);

	g_clear_error(&error);
	g_assert_false(ai_cli_limit_classify(table, "codex", "all good", now,
	                                     &error));
	g_assert_no_error(error);
}

/* ------------------------------------------------------------------ */
/* Through a real subprocess                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
	const gchar *cli;
	const gchar *stdout_text;
	const gchar *stream_stdout;  /* when streaming prints another shape */
	const gchar *stderr_text;
	gint         exit_status;
	gint         code;
} Run;

static const Run runs[] = {
	{ "codex",
	  "{\"type\":\"thread.started\",\"thread_id\":\"t-1\"}\n"
	  "{\"type\":\"turn.started\"}\n"
	  "{\"type\":\"error\",\"message\":\"You've hit your usage limit. "
	  "Try again at 3:45 PM.\"}\n"
	  "{\"type\":\"turn.failed\",\"error\":{\"message\":\"You've hit your "
	  "usage limit. Try again at 3:45 PM.\"}}\n",
	  NULL, "", 1, AI_ERROR_SESSION_LIMIT },
	{ "grok",
	  "{\"type\":\"error\",\"message\":\"You've hit the rate limit for "
	  "your plan.\"}\n", NULL, "", 0, AI_ERROR_RATE_LIMITED },
	{ "opencode",
	  "{\"type\":\"error\",\"timestamp\":1,\"sessionID\":\"s-1\","
	  "\"error\":{\"name\":\"APIError\",\"data\":{\"message\":\"Usage "
	  "limit reached. It will reset in 3 hours 20 minutes.\"}}}\n",
	  NULL, "", 1, AI_ERROR_SESSION_LIMIT },
	{ "cursor", "", NULL,
	  "Error: Authentication required. Please run 'agent login' first, "
	  "or set CURSOR_API_KEY environment variable.\n",
	  1, AI_ERROR_INVALID_API_KEY },
	{ "agy",
	  "{\"conversation_id\":\"c-1\",\"status\":\"ERROR\",\"response\":\"\","
	  "\"error\":\"Contact your administrator to enable overages. Resets in "
	  "53h16m34s.: RESOURCE_EXHAUSTED (code 429): Individual quota "
	  "reached.\"}\n",
	  /* stream-json wraps the same object (antigravity docs, headless) */
	  "{\"event\":\"result\",\"result\":{\"conversation_id\":\"c-1\","
	  "\"status\":\"ERROR\",\"response\":\"\",\"error\":\"Contact your "
	  "administrator to enable overages. Resets in 53h16m34s.: "
	  "RESOURCE_EXHAUSTED (code 429): Individual quota reached.\"}}\n",
	  "", 1, AI_ERROR_SESSION_LIMIT }
};

/* A shell script that prints @run's output, ignoring its prompt. */
static gchar *
write_fake_cli(const gchar *dir, const Run *run, gboolean stream)
{
	g_autofree gchar *out = g_build_filename(dir, "stdout", NULL);
	g_autofree gchar *err = g_build_filename(dir, "stderr", NULL);
	g_autofree gchar *script = NULL;
	gchar *path = g_build_filename(dir, "fake-cli", NULL);
	g_autoptr(GError) error = NULL;

	g_assert_true(g_file_set_contents(out,
		stream && run->stream_stdout != NULL ? run->stream_stdout
		                                     : run->stdout_text, -1, &error));
	g_assert_true(g_file_set_contents(err, run->stderr_text, -1, &error));
	script = g_strdup_printf("#!/bin/sh\n"
	                         "cat >/dev/null 2>&1\n"
	                         "cat '%s'\n"
	                         "cat '%s' >&2\n"
	                         "exit %d\n", out, err, run->exit_status);
	g_assert_true(g_file_set_contents(path, script, -1, &error));
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	return path;
}

typedef struct
{
	gboolean    done;
	AiResponse *response;
	GError     *error;
} Result;

static void
chat_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
	Result *result = user_data;

	result->response = ai_provider_chat_finish(AI_PROVIDER(source), res,
	                                           &result->error);
	result->done = TRUE;
}

static void
stream_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
	Result *result = user_data;

	result->response = ai_streamable_chat_stream_finish(
		AI_STREAMABLE(source), res, &result->error);
	result->done = TRUE;
}

enum { RUN_CHAT, RUN_STREAM, RUN_SYNC };

static void
run_one(gconstpointer data, gint mode)
{
	gboolean stream = mode == RUN_STREAM;
	const Run *run = data;
	g_autofree gchar *dir = g_dir_make_tmp("ai-cli-limit-XXXXXX", NULL);
	g_autofree gchar *fake = NULL;
	g_autoptr(AiCliClient) client = client_for(run->cli);
	g_autoptr(AiMessage) message = ai_message_new_user("hello");
	GList messages = { message, NULL, NULL };
	Result result = { FALSE, NULL, NULL };

	g_assert_nonnull(dir);
	fake = write_fake_cli(dir, run, stream);
	ai_cli_client_set_executable_path(client, fake);
	ai_cli_client_set_working_directory(client, dir);

	if (mode == RUN_SYNC) {
		/* ai_cli_client_chat_sync(): ai-simple and `ai` use this path. */
		result.response = ai_cli_client_chat_sync(client, &messages, NULL,
		                                          &result.error);
		result.done = TRUE;
	} else if (stream)
		ai_streamable_chat_stream_async(AI_STREAMABLE(client), &messages,
		                                NULL, 64, NULL, NULL,
		                                stream_done, &result);
	else
		ai_provider_chat_async(AI_PROVIDER(client), &messages, NULL, 64,
		                       NULL, NULL, chat_done, &result);

	while (!result.done)
		g_main_context_iteration(NULL, TRUE);

	g_test_message("%s%s: %s", run->cli,
	               mode == RUN_SYNC ? " (sync)" : stream ? " (stream)" : "",
	               result.error != NULL ? result.error->message : "(no error)");
	g_assert_null(result.response);
	g_assert_error(result.error, AI_ERROR, run->code);
	g_clear_error(&result.error);

	{
		g_autofree gchar *out = g_build_filename(dir, "stdout", NULL);
		g_autofree gchar *err = g_build_filename(dir, "stderr", NULL);

		g_unlink(out);
		g_unlink(err);
		g_unlink(fake);
		g_rmdir(dir);
	}
}

static void test_run_chat(gconstpointer data) { run_one(data, RUN_CHAT); }
static void test_run_stream(gconstpointer data) { run_one(data, RUN_STREAM); }
static void test_run_sync(gconstpointer data) { run_one(data, RUN_SYNC); }

int
main(int argc, char *argv[])
{
	guint i;

	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/cli-limit/samples", test_samples_are_classified);
	g_test_add_func("/cli-limit/ordinary-failure",
	                test_ordinary_failure_is_untouched);
	g_test_add_func("/cli-limit/other-codes", test_other_codes_are_untouched);
	g_test_add_func("/cli-limit/unregistered", test_unregistered_cli_is_untouched);
	g_test_add_func("/cli-limit/session-limit-shape", test_session_limit_shape);

	for (i = 0; i < G_N_ELEMENTS(runs); i++) {
		g_autofree gchar *chat = g_strdup_printf("/cli-limit/run/%s/chat",
		                                         runs[i].cli);
		g_autofree gchar *stream = g_strdup_printf("/cli-limit/run/%s/stream",
		                                           runs[i].cli);

		g_test_add_data_func(chat, &runs[i], test_run_chat);
		g_autofree gchar *sync = g_strdup_printf("/cli-limit/run/%s/sync",
		                                         runs[i].cli);

		g_test_add_data_func(stream, &runs[i], test_run_stream);
		g_test_add_data_func(sync, &runs[i], test_run_sync);
	}

	return g_test_run();
}
