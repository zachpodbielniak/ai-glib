/* SPDX-License-Identifier: AGPL-3.0-or-later
 * OpenCode's actual pipe/launcher contract, without credentials or network.
 * A private context catches global-default deadline/cancellation mistakes. */
#include <glib.h>
#include <glib/gstdio.h>
#include "ai-glib.h"

#define ANSWER "{\"type\":\"text\",\"sessionID\":\"ses_test\",\"part\":{\"text\":\"answer\"}}\n"
#define TOOL "{\"type\":\"tool_use\",\"sessionID\":\"ses_test\",\"part\":{\"id\":\"prt_1\",\"callID\":\"call_1\",\"tool\":\"bash\",\"state\":{\"status\":\"completed\",\"output\":\"done\"}}}\n"
#define FAILURE "{\"type\":\"error\",\"error\":{\"name\":\"APIError\",\"data\":{\"message\":\"test failure\"}}}\n"

typedef struct
{
	gchar *dir;
	AiOpenCodeClient *client;
	GMainContext *context;
	GAsyncResult *result;
} Fixture;

/* Each file controls one observable part of the fake CLI's behavior. */
static void
stage(Fixture *f, const gchar *name, const gchar *value)
{
	g_autofree gchar *path = g_build_filename(f->dir, name, NULL);
	g_assert_true(g_file_set_contents(path, value, -1, NULL));
}

static gchar *
read_record(Fixture *f, const gchar *name)
{
	g_autofree gchar *path = g_build_filename(f->dir, name, NULL);
	gchar *value = NULL;
	g_assert_true(g_file_get_contents(path, &value, NULL, NULL));
	return value;
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autofree gchar *script = NULL;
	g_autofree gchar *path = NULL;
	(void)unused;
	f->dir = g_dir_make_tmp("ai-opencode-XXXXXX", NULL);
	g_assert_nonnull(f->dir);
	path = g_build_filename(f->dir, "opencode", NULL);
	script = g_strdup_printf(
		"#!/bin/bash\n"
		"set -eu\n"
		"d='%s'\n"
		"n=0; if [[ -f $d/count ]]; then read -r n < \"$d/count\"; fi\n"
		"n=$((n+1)); echo \"$n\" > \"$d/count\"\n"
		"printf '%%s\\n' \"$@\" > \"$d/argv.$n\"\n"
		"pwd -P > \"$d/cwd.$n\"\n"
		"printf '%%s' \"${OPENCODE_PERMISSION-}\" > \"$d/permission.$n\"\n"
		"printf '%%s' \"${OPENCODE_SERVER_PASSWORD-}\" > \"$d/password.$n\"\n"
		"printf '%%s' \"${OPENCODE_SERVER_USERNAME-}\" > \"$d/username.$n\"\n"
		"cat > \"$d/stdin.$n\"\n"
		"if [[ -f $d/sleep.$n ]]; then exec sleep 30; fi\n"
		"if [[ -f $d/signal ]]; then kill -TERM $$; fi\n"
		"if [[ -f $d/stdout.$n ]]; then cat \"$d/stdout.$n\"; fi\n"
		"if [[ -f $d/stderr ]]; then cat \"$d/stderr\" >&2; fi\n"
		"status=0; if [[ -f $d/exit ]]; then read -r status < \"$d/exit\"; fi\n"
		"exit \"$status\"\n", f->dir);
	stage(f, "opencode", script);
	g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	f->client = ai_opencode_client_new();
	g_object_set(f->client, "executable-path", path, "working-directory", f->dir,
		"process-timeout-ms", 2000, "pure", TRUE, NULL);
	f->context = g_main_context_new();
	g_main_context_push_thread_default(f->context);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_autoptr(GDir) dir = NULL;
	const gchar *name;
	gpointer weak_client = f->client;
	gint64 deadline = g_get_monotonic_time() + 5000000;
	(void)unused;
	g_object_add_weak_pointer(G_OBJECT(f->client), &weak_client);
	g_clear_object(&f->result);
	g_clear_object(&f->client);
	/* Streaming cancellation returns promptly while cancelled pipe reads
	 * settle asynchronously. Keep their context alive and prove that every
	 * operation releases the client before destroying that context. */
	while (weak_client != NULL)
	{
		g_assert_cmpint(g_get_monotonic_time(), <, deadline);
		g_main_context_iteration(f->context, FALSE);
		g_usleep(1000);
	}
	g_main_context_pop_thread_default(f->context);
	g_main_context_unref(f->context);
	dir = g_dir_open(f->dir, 0, NULL);
	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *path = g_build_filename(f->dir, name, NULL);
		g_assert_cmpint(g_remove(path), ==, 0);
	}
	g_clear_pointer(&dir, g_dir_close);
	g_assert_cmpint(g_rmdir(f->dir), ==, 0);
	g_free(f->dir);
}

/* Hold the async result until its operation has settled. */
static void
done(GObject *source, GAsyncResult *result, gpointer data)
{
	Fixture *f = data;
	(void)source;
	f->result = g_object_ref(result);
}

static AiResponse *
run(Fixture *f, gint mode, const gchar *prompt, GCancellable *cancel, GError **error)
{
	g_autoptr(AiMessage) message = ai_message_new_user(prompt);
	GList messages = { message, NULL, NULL };
	g_clear_object(&f->result);
	if (mode == 0)
		return ai_cli_client_chat_sync(AI_CLI_CLIENT(f->client), &messages, cancel, error);
	if (mode == 1)
		ai_provider_chat_async(AI_PROVIDER(f->client), &messages, NULL, 4096, NULL, cancel, done, f);
	else
		ai_streamable_chat_stream_async(AI_STREAMABLE(f->client), &messages, NULL, 4096, NULL, cancel, done, f);
	while (f->result == NULL)
		g_main_context_iteration(f->context, TRUE);
	if (mode == 1)
		return ai_provider_chat_finish(AI_PROVIDER(f->client), f->result, error);
	return ai_streamable_chat_stream_finish(AI_STREAMABLE(f->client), f->result, error);
}

static void
success(Fixture *f, gconstpointer data)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *prompt = g_strnfill(200000, 'x');
	g_autofree gchar *seen = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *args = NULL;
	const gchar *paths[] = { "a,b.txt", "two words.txt", NULL };
	stage(f, "stdout.1", ANSWER);
	g_object_set(f->client, "skip-permissions", TRUE, "username", "test-user",
		"password", "test-secret", "file-paths", paths,
		"attach", "http://127.0.0.1:1", "directory", "/remote/only", NULL);
	ai_cli_client_set_env(AI_CLI_CLIENT(f->client), "OPENCODE_PERMISSION", "{\"bash\":\"deny\"}");
	response = run(f, GPOINTER_TO_INT(data), prompt, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(response);
	text = ai_response_get_text(response);
	g_assert_cmpstr(text, ==, "answer");
	seen = read_record(f, "stdin.1");
	g_assert_true(g_str_has_prefix(seen, prompt));
	g_clear_pointer(&seen, g_free);
	seen = read_record(f, "permission.1");
	g_assert_cmpstr(seen, ==, "{\"bash\":\"deny\"}");
	g_clear_pointer(&seen, g_free);
	seen = read_record(f, "password.1");
	g_assert_cmpstr(seen, ==, "test-secret");
	g_clear_pointer(&seen, g_free);
	seen = read_record(f, "cwd.1");
	g_strchomp(seen);
	g_assert_cmpstr(seen, ==, f->dir);
	args = read_record(f, "argv.1");
	g_assert_nonnull(strstr(args, "--auto\n"));
	g_assert_nonnull(strstr(args, "--dir\n/remote/only\n"));
	g_assert_nonnull(strstr(args, "--file\na,b.txt\n--file\ntwo words.txt\n"));
	g_assert_null(strstr(args, "test-secret"));
	g_assert_null(strstr(args, prompt));
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(f->client)), ==, "ses_test");
}

static void
exit_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	stage(f, "stdout.1", ANSWER);
	stage(f, "stderr", "diagnostic");
	stage(f, "exit", "7\n");
	response = run(f, GPOINTER_TO_INT(data), "hi", NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "diagnostic"));
}

static void
structured_failure(Fixture *f, gconstpointer data)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	stage(f, "stdout.1", FAILURE);
	response = run(f, GPOINTER_TO_INT(data), "hi", NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
	g_assert_nonnull(strstr(error->message, "test failure"));
}

/* A child signal must be an ordinary error, never a host GLib critical. */
static void
signaled(Fixture *f, gconstpointer data)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	stage(f, "signal", "");
	response = run(f, GPOINTER_TO_INT(data), "hi", NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_CLI_EXECUTION);
}

static void
timeout(Fixture *f, gconstpointer data)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	gint64 start = g_get_monotonic_time();
	stage(f, "sleep.1", "");
	g_object_set(f->client, "process-timeout-ms", 100, NULL);
	response = run(f, GPOINTER_TO_INT(data), "hi", NULL, &error);
	g_assert_null(response);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	g_assert_cmpint(g_get_monotonic_time() - start, <, 5000000);
}

static gboolean
cancel_now(gpointer data)
{
	g_cancellable_cancel(data);
	return G_SOURCE_REMOVE;
}

static void
cancelled(Fixture *f, gconstpointer data)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GSource) source = g_timeout_source_new(100);
	stage(f, "sleep.1", "");
	g_source_set_callback(source, cancel_now, cancel, NULL);
	g_source_attach(source, f->context);
	response = run(f, GPOINTER_TO_INT(data), "hi", cancel, &error);
	g_source_destroy(source);
	g_assert_null(response);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static void
retry(Fixture *f, gconstpointer data)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *args = NULL;
	g_autofree gchar *text = NULL;
	gint mode = GPOINTER_TO_INT(data);
	g_autoptr(GCancellable) cancel = g_cancellable_new();
	g_autoptr(GSource) source = g_timeout_source_new(100);
	stage(f, "stdout.1", TOOL);
	stage(f, "stdout.2", ANSWER);
	g_object_set(f->client, "effort-level", "minimal", "skip-permissions", TRUE, NULL);
	if (mode != 0)
	{
		stage(f, "sleep.2", "");
		if (mode == 1)
			g_object_set(f->client, "process-timeout-ms", 100, NULL);
		else
		{
			g_source_set_callback(source, cancel_now, cancel, NULL);
			g_source_attach(source, f->context);
		}
	}
	response = run(f, 1, "hi", cancel, &error);
	g_source_destroy(source);
	if (mode == 1)
	{
		g_assert_null(response);
		g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	}
	else if (mode == 2)
	{
		g_assert_null(response);
		g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	}
	else
	{
		g_assert_no_error(error);
		text = ai_response_get_text(response);
		g_assert_cmpstr(text, ==, "answer");
	}
	args = read_record(f, "argv.2");
	g_assert_nonnull(strstr(args, "--session\nses_test\n"));
	g_assert_nonnull(strstr(args, "--variant\nminimal\n"));
	g_assert_nonnull(strstr(args, "--auto\n"));
	g_assert_null(strstr(args, "--fork"));
	g_assert_null(strstr(args, "--command"));
}

static void
command(Fixture *f, gconstpointer unused)
{
	g_autoptr(AiResponse) response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seen = NULL;
	(void)unused;
	g_object_set(f->client, "command", "test", "system-prompt", "ignored", NULL);
	stage(f, "stdout.1", ANSWER);
	response = run(f, 1, "src/a.c --verbose", NULL, &error);
	g_assert_no_error(error);
	seen = read_record(f, "stdin.1");
	g_assert_cmpstr(seen, ==, "src/a.c --verbose");
}

static void
models(Fixture *f, gconstpointer unused)
{
	GList *result;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *args = NULL;
	(void)unused;
	stage(f, "stdout.1", "plugin banner\ncustom/local-model\nopenai/new-model\ncustom/local-model\n");
	ai_provider_list_models_async(AI_PROVIDER(f->client), NULL, done, f);
	while (f->result == NULL)
		g_main_context_iteration(f->context, TRUE);
	result = ai_provider_list_models_finish(AI_PROVIDER(f->client), f->result, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_list_length(result), ==, 2);
	g_assert_cmpstr(result->data, ==, "custom/local-model");
	g_assert_cmpstr(result->next->data, ==, "openai/new-model");
	g_list_free_full(result, g_free);
	args = read_record(f, "argv.1");
	g_assert_cmpstr(args, ==, "models\n--pure\n");
}

int
main(int argc, char **argv)
{
	gint mode;
	g_test_init(&argc, &argv, NULL);
	for (mode = 0; mode < 3; mode++)
	{
		g_autofree gchar *base = g_strdup_printf("/ai-glib/opencode-spawn/%d", mode);
		g_autofree gchar *a = g_strconcat(base, "/success", NULL);
		g_autofree gchar *b = g_strconcat(base, "/exit-failure", NULL);
		g_autofree gchar *c = g_strconcat(base, "/structured-failure", NULL);
		g_autofree gchar *d = g_strconcat(base, "/timeout", NULL);
		g_autofree gchar *signal_path = g_strconcat(base, "/signal", NULL);
		g_test_add(a, Fixture, GINT_TO_POINTER(mode), setup, success, teardown);
		g_test_add(b, Fixture, GINT_TO_POINTER(mode), setup, exit_failure, teardown);
		g_test_add(c, Fixture, GINT_TO_POINTER(mode), setup, structured_failure, teardown);
		g_test_add(d, Fixture, GINT_TO_POINTER(mode), setup, timeout, teardown);
		g_test_add(signal_path, Fixture, GINT_TO_POINTER(mode), setup, signaled, teardown);
		/* Sync uses its own private context, so cancellation is tested via
		 * async/stream here and by the shared subprocess utility for sync. */
		if (mode != 0)
		{
			g_autofree gchar *e = g_strconcat(base, "/cancelled", NULL);
			g_test_add(e, Fixture, GINT_TO_POINTER(mode), setup, cancelled, teardown);
		}
	}
	g_test_add("/ai-glib/opencode-spawn/retry", Fixture, NULL, setup, retry, teardown);
	g_test_add("/ai-glib/opencode-spawn/retry-timeout", Fixture, GINT_TO_POINTER(1), setup, retry, teardown);
	g_test_add("/ai-glib/opencode-spawn/retry-cancel", Fixture, GINT_TO_POINTER(2), setup, retry, teardown);
	g_test_add("/ai-glib/opencode-spawn/command", Fixture, NULL, setup, command, teardown);
	g_test_add("/ai-glib/opencode-spawn/models", Fixture, NULL, setup, models, teardown);
	return g_test_run();
}
