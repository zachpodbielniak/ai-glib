/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include "core/ai-subprocess-util.h"

static gchar *tui;
static void
test_resumed_provider_request(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("ai-links-tui-XXXXXX", NULL);
	g_autofree gchar *registry = g_build_filename(dir, "ai-glib", "sessions", NULL);
	g_autofree gchar *gh = g_build_filename(dir, "gh", NULL);
	g_autofree gchar *grok = g_build_filename(dir, "grok", NULL);
	g_autofree gchar *capture = g_build_filename(dir, "prompt.txt", NULL);
	g_autofree gchar *output = NULL, *error_output = NULL, *request = NULL;
	g_autoptr(AiWorkSession) work = g_object_new(AI_TYPE_WORK_SESSION,
		"directory", dir, "project", dir, "provider", "grok-build", "model", "grok-4.7", "provider-session", "native-test", NULL);
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) child = NULL;
	g_autoptr(GError) error = NULL;
	if (!g_file_test(tui, G_FILE_TEST_IS_EXECUTABLE)) { g_test_skip("TUI not built"); return; }
	g_assert_true(ai_work_session_add_link(work, "https://github.com/team/project/issues/9", NULL));
	g_assert_true(ai_work_session_save(work, registry, FALSE, &error));
	g_assert_no_error(error);
	g_assert_true(g_file_set_contents(gh, "#!/bin/sh\nprintf '%s' '{\"title\":\"TUI_LINK_DISCOVERED\",\"state\":\"open\",\"body\":\"Stored work description\",\"comments\":[{\"body\":\"TUI_LINK_COMMENT\"}]}'\n", -1, NULL));
	g_assert_true(g_file_set_contents(grok, "#!/bin/sh\n/bin/cat > \"$LINK_CAPTURE\"\nprintf '%s\\n' '{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"Linked work received\"}}}' '{\"type\":\"result\",\"result\":\"Linked work received\",\"session_id\":\"test\"}'\n", -1, NULL));
	g_chmod(gh, 0700); g_chmod(grok, 0700);
	g_subprocess_launcher_set_cwd(launcher, dir);
	g_subprocess_launcher_setenv(launcher, "HOME", dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_STATE_HOME", dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "PATH", dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "GROK_PATH", grok, TRUE);
	g_subprocess_launcher_setenv(launcher, "LINK_CAPTURE", capture, TRUE);
	g_subprocess_launcher_unsetenv(launcher, "TMUX");
	child = g_subprocess_launcher_spawn(launcher, &error, tui, "--no-herdr",
		"--workspace-session", ai_work_session_get_id(work), "--dump", "Read the linked work", NULL);
	g_assert_no_error(error);
	g_assert_true(ai_subprocess_communicate_utf8_bounded(child, NULL, 20000, NULL, &output, &error_output, &error));
	g_assert_no_error(error);
	g_test_message("%s", error_output != NULL ? error_output : "");
	g_assert_true(g_subprocess_get_successful(child));
	g_assert_true(g_file_get_contents(capture, &request, NULL, &error));
	g_assert_no_error(error);
	g_assert_nonnull(strstr(request, "Read the linked work"));
	g_assert_nonnull(strstr(request, "https://github.com/team/project/issues/9"));
	g_assert_nonnull(strstr(request, "TUI_LINK_DISCOVERED"));
	g_assert_nonnull(strstr(request, "TUI_LINK_COMMENT"));
	g_assert_nonnull(strstr(output, "Linked work received"));
}
int
main(int argc, char **argv)
{
	g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = g_path_get_dirname(exe);
	g_test_init(&argc, &argv, NULL);
	tui = g_build_filename(dir, "..", "bin", "ai-tui", NULL);
	g_test_add_func("/linked-work/tui/resumed-provider-request", test_resumed_provider_request);
	return g_test_run();
}
