/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-glib.h"
#include <glib/gstdio.h>
#include <stdarg.h>
#include <unistd.h>

static gchar *binary;
static gchar *socket_name;
static gchar *sandbox;

static gchar *
tmux(const gchar *first, ...)
{
	g_autoptr(GPtrArray) args = g_ptr_array_new();
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	gchar *output = NULL;
	const gchar *word;
	va_list ap;
	g_ptr_array_add(args, "tmux"); g_ptr_array_add(args, "-L"); g_ptr_array_add(args, socket_name);
	g_ptr_array_add(args, "-f"); g_ptr_array_add(args, "/dev/null");
	va_start(ap, first);
	for (word = first; word != NULL; word = va_arg(ap, const gchar *)) g_ptr_array_add(args, (gpointer)word);
	va_end(ap);
	g_ptr_array_add(args, NULL);
	child = g_subprocess_newv((const gchar * const *)args->pdata,
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, &error);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(child, NULL, NULL, &output, NULL, &error));
	g_assert_no_error(error);
	return output;
}

static void
write_file(const gchar *name, const gchar *data)
{
	g_autofree gchar *path = g_build_filename(sandbox, name, NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_mkdir_with_parents(parent, 0700);
	g_assert_true(g_file_set_contents(path, data, -1, NULL));
}

static void
start_at(const gchar *name, const gchar *option, const gchar *cwd)
{
	g_autofree gchar *home = g_strconcat("HOME=", sandbox, NULL);
	g_autofree gchar *state = g_strconcat("XDG_STATE_HOME=", sandbox, "/state", NULL);
	g_autofree gchar *config = g_strconcat("XDG_CONFIG_HOME=", sandbox, "/config", NULL);
	g_autofree gchar *grok = g_strconcat("GROK_PATH=", sandbox, "/grok", NULL);
	g_autofree gchar *path = g_strconcat("PATH=", sandbox, ":", g_getenv("PATH"), NULL);
	g_autofree gchar *output = tmux("new-session", "-d", "-s", name, "-x", "140", "-y", "35", "-c", cwd,
		"env", home, state, config, grok, path, "HERDR_ENV=0", "TERM=xterm-256color", "LC_ALL=C.UTF-8",
		binary, "-p", "grok-build", "--no-animation", option, NULL);
}

static void
start(const gchar *name, const gchar *option)
{
	start_at(name, option, sandbox);
}

/* Opt-in verbatim evidence from the running binary, suitable for PR demos. */
static void
demonstrate(const gchar *session, const gchar *caption)
{
	g_autofree gchar *capture = NULL;
	g_autofree gchar *title = NULL;
	if (g_getenv("AI_TUI_DEMONSTRATE") == NULL) return;
	capture = tmux("capture-pane", "-p", "-t", session, NULL);
	title = tmux("display-message", "-p", "-t", session, "window=#{window_name} pane=#{pane_title}", NULL);
	g_test_message("DEMONSTRATION: %s\n%s%s", caption, title, capture);
}

static gboolean
wait_text(const gchar *session, const gchar *needle)
{
	gint64 deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
	do
	{
		g_autofree gchar *capture = tmux("capture-pane", "-p", "-t", session, NULL);
		if (strstr(capture, needle) != NULL) return TRUE;
		g_usleep(50000);
	} while (g_get_monotonic_time() < deadline);
	{
		g_autofree gchar *capture = tmux("capture-pane", "-p", "-t", session, NULL);
		g_test_message("Missing %s in:\n%s", needle, capture);
	}
	return FALSE;
}

static void
send(const gchar *session, const gchar *text)
{
	g_autofree gchar *out = tmux("send-keys", "-t", session, "-l", text, NULL);
}

static void
key(const gchar *session, const gchar *name)
{
	g_autofree gchar *out = tmux("send-keys", "-t", session, name, NULL);
}

static void
setup(void)
{
	g_autofree gchar *stub = NULL;
	g_autofree gchar *browser = NULL;
	sandbox = g_dir_make_tmp("ai-dashboard-pty-XXXXXX", NULL);
	socket_name = g_strdup_printf("ai-dashboard-%d-%u", (int)getpid(), g_random_int());
	write_file("grok", "#!/bin/sh\ncat > input\nif test -f delay; then sleep 2; fi\ncat reply\n");
	write_file("reply", "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"Dashboard test complete\"}}}\n{\"type\":\"result\",\"text\":\"Dashboard test complete\",\"sessionId\":\"test-session\"}\n");
	write_file("xdg-open", "#!/bin/sh\nprintf '%s' \"$1\" > opened\n");
	stub = g_build_filename(sandbox, "grok", NULL);
	browser = g_build_filename(sandbox, "xdg-open", NULL);
	g_chmod(stub, 0700); g_chmod(browser, 0700);
}

static void
remove_tree(const gchar *path)
{
	g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
	const gchar *name;
	if (dir != NULL) while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *child = g_build_filename(path, name, NULL);
		if (g_file_test(child, G_FILE_TEST_IS_DIR)) remove_tree(child); else g_unlink(child);
	}
	g_rmdir(path);
}

static void
teardown(void)
{
	g_autofree gchar *out = tmux("kill-server", NULL);
	remove_tree(sandbox);
	g_clear_pointer(&sandbox, g_free); g_clear_pointer(&socket_name, g_free);
}

static void
test_toggle(void)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *opened = NULL;
	setup(); start("toggle", "--no-dashboard");
	g_assert_true(wait_text("toggle", "COMPOSE"));
	send("toggle", "unsent draft");
	key("toggle", "C-\\");
	g_assert_true(wait_text("toggle", "PROJECT DASHBOARD"));
	key("toggle", "C-\\");
	g_assert_true(wait_text("toggle", "unsent draft"));
	demonstrate("toggle", "Ctrl+\\ returns with unsent draft preserved");
	key("toggle", "C-u");
	send("toggle", "/issue link https://example.invalid/owner/repo/issues/42/"); key("toggle", "Enter");
	g_assert_true(wait_text("toggle", "#42"));
	send("toggle", "/pr link https://example.invalid/owner/repo/pulls/81"); key("toggle", "Enter");
	g_assert_true(wait_text("toggle", "#81"));
	demonstrate("toggle", "Linked issue and PR visible in the side panel; Ctrl+] opens the issue URL");
	key("toggle", "C-]");
	path = g_build_filename(sandbox, "opened", NULL);
	{
		gint64 deadline = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
		while (!g_file_get_contents(path, &opened, NULL, NULL) && g_get_monotonic_time() < deadline) g_usleep(20000);
	}
	g_assert_cmpstr(opened, ==, "https://example.invalid/owner/repo/issues/42/");
	write_file("delay", "yes");
	send("toggle", "run tests"); key("toggle", "Enter");
	key("toggle", "C-\\");
	g_assert_true(wait_text("toggle", "WORK"));
	demonstrate("toggle", "Dashboard stays usable while provider work runs");
	g_assert_true(wait_text("toggle", "DONE"));
	{
		gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
		do {
			g_clear_pointer(&name, g_free);
			name = tmux("display-message", "-p", "-t", "toggle", "#{window_name}", NULL);
			if (g_str_has_prefix(name, "DONE:")) break;
			g_usleep(50000);
		} while (g_get_monotonic_time() < deadline);
	}
	g_assert_true(g_str_has_prefix(name, "DONE:"));
	demonstrate("toggle", "Completed turn: dashboard DONE and aggregate window prefix");
	key("toggle", "C-\\");
	g_assert_true(wait_text("toggle", "Dashboard test complete"));
	teardown();
}

static void
test_startup(void)
{
	setup();
	write_file("config/ai-glib/config.yaml", "apps:\n  ai-tui:\n    open-dashboard-on-load: true\n");
	start("configured", NULL);
	g_assert_true(wait_text("configured", "PROJECT DASHBOARD"));
	demonstrate("configured", "Bare launch obeys open-dashboard-on-load config");
	key("configured", "n");
	g_assert_true(wait_text("configured", "COMPOSE"));
	start("override", "--no-dashboard");
	g_assert_true(wait_text("override", "COMPOSE"));
	start("prompt", "explicit prompt");
	g_assert_true(wait_text("prompt", "Dashboard test complete"));
	start("explicit", "--dashboard");
	g_assert_true(wait_text("explicit", "PROJECT DASHBOARD"));
	demonstrate("explicit", "Dashboard aggregates independently launched conversations");
	teardown();
}

static void
test_assignment(void)
{
	g_autofree gchar *gh = NULL;
	g_autofree gchar *input = NULL;
	g_autofree gchar *received = NULL;
	setup();
	write_file("gh", "#!/bin/sh\nsleep 1\nprintf '%s' '{\"title\":\"Fix assigned parser\",\"state\":\"OPEN\",\"body\":\"Preserve UTF-8 and reject truncated input\"}'\n");
	gh = g_build_filename(sandbox, "gh", NULL); g_chmod(gh, 0700);
	start("assignment", "--no-dashboard");
	g_assert_true(wait_text("assignment", "COMPOSE"));
	send("assignment", "/work https://github.com/example/project/issues/12"); key("assignment", "Enter");
	g_assert_true(wait_text("assignment", "Loading assignment"));
	send("assignment", "keep my next draft");
	g_assert_true(wait_text("assignment", "Dashboard test complete"));
	g_assert_true(wait_text("assignment", "keep my next draft"));
	g_assert_true(wait_text("assignment", "#12 OPEN"));
	input = g_build_filename(sandbox, "input", NULL);
	g_assert_true(g_file_get_contents(input, &received, NULL, NULL));
	g_assert_nonnull(strstr(received, "Preserve UTF-8 and reject truncated input"));
	g_assert_null(strstr(received, "keep my next draft"));
	demonstrate("assignment", "Issue assignment reaches provider; metadata and next draft retained");
	if (g_getenv("AI_TUI_DEMONSTRATE") != NULL) g_test_message("PROVIDER STDIN:\n%s", received);
	teardown();
}

static void
test_window_aggregation(void)
{
	g_autofree gchar *out = NULL;
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;
	g_autofree gchar *name = NULL;
	gint64 deadline;
	setup(); start("one", "--no-dashboard"); start("two", "--no-dashboard");
	g_assert_true(wait_text("one", "COMPOSE")); g_assert_true(wait_text("two", "COMPOSE"));
	first = tmux("display-message", "-p", "-t", "one", "#{pane_id}", NULL); g_strchomp(first);
	second = tmux("display-message", "-p", "-t", "two", "#{pane_id}", NULL); g_strchomp(second);
	out = tmux("join-pane", "-s", second, "-t", first, NULL); g_clear_pointer(&out, g_free);
	write_file("reply", "{\"type\":\"error\",\"message\":\"Deliberate provider failure\"}\n");
	send(first, "fail"); key(first, "Enter");
	g_assert_true(wait_text(first, "Deliberate provider failure"));
	deadline = g_get_monotonic_time() + 6 * G_USEC_PER_SEC;
	do {
		g_clear_pointer(&name, g_free);
		name = tmux("display-message", "-p", "-t", second, "#{window_name}", NULL);
		if (g_str_has_prefix(name, "ERROR:")) break;
		g_usleep(50000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_true(g_str_has_prefix(name, "ERROR:"));
	demonstrate(first, "Two panes: ERROR takes precedence over another idle reporter");
	/* Another reporter cannot erase the error with its idle state. */
	g_usleep(3200000);
	g_clear_pointer(&name, g_free);
	name = tmux("display-message", "-p", "-t", second, "#{window_name}", NULL);
	g_assert_true(g_str_has_prefix(name, "ERROR:"));
	out = tmux("rename-window", "-t", second, "my manual name", NULL); g_clear_pointer(&out, g_free);
	g_usleep(3200000);
	g_clear_pointer(&name, g_free);
	name = tmux("display-message", "-p", "-t", second, "#{window_name}", NULL);
	g_assert_cmpstr(g_strchomp(name), ==, "my manual name");
	demonstrate(second, "User window rename survives reporter heartbeats");
	teardown();
}

static void
test_resume(void)
{
	g_autofree gchar *registry = NULL;
	g_autolist(AiWorkSession) rows = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *option = NULL;
	g_autofree gchar *out = NULL;
	gint64 deadline;
	setup(); start("original", "--no-dashboard");
	g_assert_true(wait_text("original", "COMPOSE"));
	send("original", "/issue link https://example.invalid/team/repo/issues/18"); key("original", "Enter");
	g_assert_true(wait_text("original", "#18"));
	send("original", "finish"); key("original", "Enter");
	g_assert_true(wait_text("original", "Dashboard test complete"));
	key("original", "C-\\"); g_assert_true(wait_text("original", "PROJECT DASHBOARD"));
	key("original", "q");
	registry = g_build_filename(sandbox, "state", "ai-glib", "sessions", NULL);
	deadline = g_get_monotonic_time() + 6 * G_USEC_PER_SEC;
	do {
		g_clear_list(&rows, g_object_unref); rows = ai_work_session_list(registry, NULL);
		if (rows != NULL && g_list_length(rows) == 1 && g_str_equal(ai_work_session_get_field(g_list_nth_data(rows, 0), "status"), "DISCONNECTED")) break;
		g_usleep(50000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_nonnull(rows); g_assert_cmpuint(g_list_length(rows), ==, 1);
	g_assert_cmpstr(ai_work_session_get_field(g_list_nth_data(rows, 0), "status"), ==, "DISCONNECTED");
	id = g_strdup(ai_work_session_get_id(g_list_nth_data(rows, 0)));
	option = g_strconcat("--workspace-session=", id, NULL);
	start("resumed", option);
	g_assert_true(wait_text("resumed", "#18"));
	g_assert_true(wait_text("resumed", "COMPOSE"));
	g_clear_list(&rows, g_object_unref); rows = ai_work_session_list(registry, NULL);
	g_assert_cmpuint(g_list_length(rows), ==, 1);
	g_assert_cmpstr(ai_work_session_get_id(g_list_nth_data(rows, 0)), ==, id);
	demonstrate("resumed", "Native recovery preserves the stable session and linked issue");
	teardown();
}

static void
test_title_optout(void)
{
	g_autofree gchar *out = NULL;
	g_autofree gchar *name = NULL;
	setup(); start("optout", "--no-tmux-titles");
	g_assert_true(wait_text("optout", "COMPOSE"));
	out = tmux("rename-window", "-t", "optout", "preserve this", NULL);
	send("optout", "test"); key("optout", "Enter");
	g_assert_true(wait_text("optout", "Dashboard test complete"));
	name = tmux("display-message", "-p", "-t", "optout", "#{window_name}", NULL);
	g_assert_cmpstr(g_strchomp(name), ==, "preserve this");
	key("optout", "C-\\"); g_assert_true(wait_text("optout", "DONE"));
	teardown();
}

static void
test_worktree_launch(void)
{
	g_autofree gchar *repo = NULL;
	g_autofree gchar *registry = NULL;
	g_autoptr(GSubprocess) child = NULL;
	g_autolist(AiWorkSession) rows = NULL;
	gint64 deadline;
	guint i;
	gboolean found = FALSE;
	setup();
	repo = g_build_filename(sandbox, "repo", NULL);
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		NULL, "git", "init", "--template=", "-q", repo, NULL);
	g_assert_nonnull(child); g_assert_true(g_subprocess_wait_check(child, NULL, NULL)); g_clear_object(&child);
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		NULL, "git", "-C", repo, "-c", "user.name=Test", "-c", "user.email=test@example.invalid",
		"-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false", "commit", "--allow-empty", "-qm", "seed", NULL);
	g_assert_nonnull(child); g_assert_true(g_subprocess_wait_check(child, NULL, NULL)); g_clear_object(&child);
	child = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
		NULL, "git", "-C", repo, "config", "core.hooksPath", "/dev/null", NULL);
	g_assert_nonnull(child); g_assert_true(g_subprocess_wait_check(child, NULL, NULL));
	start_at("project", "--no-dashboard", repo);
	g_assert_true(wait_text("project", "COMPOSE"));
	key("project", "C-\\"); g_assert_true(wait_text("project", "PROJECT DASHBOARD")); key("project", "w");
	registry = g_build_filename(sandbox, "state", "ai-glib", "sessions", NULL);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	do {
		g_clear_list(&rows, g_object_unref); rows = ai_work_session_list(registry, NULL);
		if (rows != NULL && g_list_length(rows) == 2) break;
		g_usleep(50000);
	} while (g_get_monotonic_time() < deadline);
	g_assert_nonnull(rows); g_assert_cmpuint(g_list_length(rows), ==, 2);
	g_assert_cmpstr(ai_work_session_get_field(g_list_nth_data(rows, 0), "project"), ==,
		ai_work_session_get_field(g_list_nth_data(rows, 1), "project"));
	for (i = 0; i < g_list_length(rows); i++)
	{
		AiWorkSession *row = g_list_nth_data(rows, i);
		if (g_str_has_prefix(ai_work_session_get_field(row, "branch"), "ai/"))
		{
			g_assert_true(g_file_test(ai_work_session_get_field(row, "directory"), G_FILE_TEST_IS_DIR));
			found = TRUE;
		}
	}
	g_assert_true(found);
	g_assert_true(wait_text("project:0", "ai/"));
	demonstrate("project:0", "Created Git worktree registers under the same project");
	teardown();
}

static void
test_skipped(void)
{
	g_test_skip("tmux or ai-tui unavailable");
}

int
main(int argc, char **argv)
{
	g_autofree gchar *test_dir = NULL;
	g_autofree gchar *out_dir = NULL;
	g_autofree gchar *tmux_path = NULL;
	g_test_init(&argc, &argv, NULL);
	test_dir = g_path_get_dirname(argv[0]); out_dir = g_path_get_dirname(test_dir);
	{ g_autofree gchar *relative = g_build_filename(out_dir, "bin", "ai-tui", NULL);
	binary = g_canonicalize_filename(relative, NULL); }
	tmux_path = g_find_program_in_path("tmux");
	if (tmux_path == NULL || !g_file_test(binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_add_func("/dashboard/unavailable", test_skipped); g_free(binary); return g_test_run();
	}
	g_test_add_func("/dashboard/toggle-links-running", test_toggle);
	g_test_add_func("/dashboard/startup", test_startup);
	g_test_add_func("/dashboard/assignment", test_assignment);
	g_test_add_func("/dashboard/window-aggregation", test_window_aggregation);
	g_test_add_func("/dashboard/resume", test_resume);
	g_test_add_func("/dashboard/title-optout", test_title_optout);
	g_test_add_func("/dashboard/worktree-launch", test_worktree_launch);
	{
		gint result = g_test_run(); g_free(binary); return result;
	}
}
