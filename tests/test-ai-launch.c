/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Offline integration coverage: native launch must bypass the headless harness.
 * Only the built frontends are exercised, never ai-launch.h directly.
 */
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
	const gchar *name;
	const gchar *variable;
	const gchar *executable;
	const gchar *skip;
	const gchar *session;
} Provider;

static const Provider providers[] = {
	{ "claude-code", "CLAUDE_CODE_PATH", "claude", "--dangerously-skip-permissions", "--resume" },
	{ "claude-tmux", "CLAUDE_CODE_PATH", "claude", "--dangerously-skip-permissions", "--resume" },
	{ "opencode", "OPENCODE_PATH", "opencode", "--auto", "--session" },
	{ "grok-build", "GROK_PATH", "grok", "--permission-mode", "--resume" },
	{ "antigravity", "AGY_PATH", "agy", "--dangerously-skip-permissions", "--conversation" },
	{ "cursor", "CURSOR_AGENT_PATH", "cursor", "--force", "--resume" },
	{ "codex-cli", "CODEX_PATH", "codex", "--dangerously-bypass-approvals-and-sandbox", "resume" },
	{ "claude", "CLAUDE_CODE_PATH", "claude", "--dangerously-skip-permissions", "--resume" }
};
static const gchar *modes[] = { "--launch", "--launch-cmd", "--launch-cmd-print" };
static gchar *binaries[2];

typedef struct
{
	guint frontend;
	guint provider;
	guint mode;
	gboolean leading_dash;
} Case;

typedef struct
{
	gchar *dir;
	gchar *config;
	gchar *tools;
	gchar *record;
	gchar *input;
	GSubprocessLauncher *launcher;
} Fixture;

typedef struct
{
	gchar *out;
	gchar *err;
	gchar *pid;
	gint status;
	goffset consumed;
} Run;

/* Record argv without escaping or line splitting, including argv[0]. */
static const gchar stub_script[] =
	"#!/bin/bash\n"
	"set -eu\n"
	"printf '%s\\0' \"$0\" \"$@\" > \"$RECORD/argv\"\n"
	"printf '%s' \"$$\" > \"$RECORD/pid\"\n"
	"printf '%s' \"$PWD\" > \"$RECORD/cwd\"\n"
	"printf '%s' \"${OPENCODE_PERMISSION-}\" > \"$RECORD/permission\"\n"
	"/bin/cat > \"$RECORD/stdin\"\n"
	"printf 'stub stdout\\n'\n"
	"printf 'stub stderr\\n' >&2\n"
	"exit \"${STUB_EXIT:-0}\"\n";

/* Delete only the private fixture tree; do not follow symlinks to helpers. */
static void
remove_tree(const gchar *path)
{
	if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
	{
		g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
		const gchar *name;

		g_assert_nonnull(dir);
		while ((name = g_dir_read_name(dir)) != NULL)
		{
			g_autofree gchar *child = g_build_filename(path, name, NULL);
			remove_tree(child);
		}
		g_assert_cmpint(g_rmdir(path), ==, 0);
	}
	else
		g_assert_cmpint(g_remove(path), ==, 0);
}

/* All children, including bash, start from an empty environment. PATH contains
 * only our stubs and two explicit coreutils helpers needed by printed commands.
 */
static void
fixture_setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	gchar *empty[] = { NULL };
	guint i;
	(void)data;

	f->dir = g_dir_make_tmp("ai-launch-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = g_build_filename(f->dir, "config", NULL);
	f->tools = g_build_filename(f->dir, "stub tools", NULL);
	f->record = g_build_filename(f->dir, "record", NULL);
	f->input = g_build_filename(f->dir, "input", NULL);
	g_assert_cmpint(g_mkdir(f->config, 0700), ==, 0);
	g_assert_cmpint(g_mkdir(f->tools, 0700), ==, 0);
	g_assert_cmpint(g_mkdir(f->record, 0700), ==, 0);
	f->launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_set_environ(f->launcher, empty);
	g_subprocess_launcher_set_cwd(f->launcher, f->dir);
	g_subprocess_launcher_setenv(f->launcher, "HOME", f->dir, TRUE);
	g_subprocess_launcher_setenv(f->launcher, "XDG_CONFIG_HOME", f->config, TRUE);
	g_subprocess_launcher_setenv(f->launcher, "PATH", f->tools, TRUE);
	g_subprocess_launcher_setenv(f->launcher, "RECORD", f->record, TRUE);
	g_subprocess_launcher_setenv(f->launcher, "G_DEBUG", "fatal-warnings", TRUE);
	for (i = 0; i <= G_N_ELEMENTS(providers); i++)
	{
		const gchar *name = i == G_N_ELEMENTS(providers) ? "ollama" : providers[i].executable;
		const gchar *variable = i == G_N_ELEMENTS(providers) ? "OLLAMA_PATH" : providers[i].variable;
		g_autofree gchar *path = g_build_filename(f->tools, name, NULL);

		g_assert_true(g_file_set_contents(path, stub_script, -1, &error));
		g_assert_no_error(error);
		g_assert_cmpint(g_chmod(path, 0700), ==, 0);
		g_subprocess_launcher_setenv(f->launcher, variable, path, TRUE);
	}
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *path = g_build_filename(f->tools, i == 0 ? "cat" : "env", NULL);
		g_assert_cmpint(symlink(i == 0 ? "/bin/cat" : "/usr/bin/env", path), ==, 0);
	}
}

/* Each registered test has independent configuration and recordings. */
static void
fixture_teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->launcher);
	remove_tree(f->dir);
	g_free(f->dir);
	g_free(f->config);
	g_free(f->tools);
	g_free(f->record);
	g_free(f->input);
}

/* Runs own captured strings, even when a child exits unsuccessfully. */
static void
run_free(Run *run)
{
	g_free(run->out);
	g_free(run->err);
	g_free(run->pid);
	g_free(run);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(Run, run_free)

/* A dup shares the input offset: command-only must leave it at zero, while
 * exec must give the identical bytes to the native process. No pipe deadlock.
 */
static Run *
run_program(Fixture *f, const gchar *binary, const gchar * const *args,
	const gchar *input, gsize length)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(Run) run = g_new0(Run, 1);
	gint fd, child_fd;
	guint i;

	g_assert_true(g_file_set_contents(f->input, input, (gssize)length, &error));
	g_assert_no_error(error);
	fd = g_open(f->input, O_RDONLY, 0);
	g_assert_cmpint(fd, >=, 0);
	child_fd = dup(fd);
	g_assert_cmpint(child_fd, >=, 0);
	g_subprocess_launcher_take_stdin_fd(f->launcher, child_fd);
	g_ptr_array_add(argv, (gpointer)binary);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	process = g_subprocess_launcher_spawnv(f->launcher,
		(const gchar * const *)argv->pdata, &error);
	g_assert_no_error(error);
	run->pid = g_strdup(g_subprocess_get_identifier(process));
	g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL,
		&run->out, &run->err, &error));
	g_assert_no_error(error);
	g_assert_true(g_subprocess_get_if_exited(process));
	run->status = g_subprocess_get_exit_status(process);
	run->consumed = lseek(fd, 0, SEEK_CUR);
	g_assert_cmpint(close(fd), ==, 0);
	if (run->status != 0)
		g_test_message("child exit %d: %s", run->status, run->err);
	return (Run *)g_steal_pointer(&run);
}

/* Read a recording, retaining its length when it contains arbitrary bytes. */
static gchar *
record_read(Fixture *f, const gchar *name, gsize *length)
{
	g_autofree gchar *path = g_build_filename(f->record, name, NULL);
	g_autofree gchar *text = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(g_file_get_contents(path, &text, length, &error));
	g_assert_no_error(error);
	return (gchar *)g_steal_pointer(&text);
}

/* Convert the NUL-delimited recording into individually assertable arguments. */
static gchar **
record_argv(Fixture *f)
{
	g_autoptr(GPtrArray) args = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *bytes = NULL;
	gsize length, offset;

	bytes = record_read(f, "argv", &length);
	for (offset = 0; offset < length; offset += strlen(bytes + offset) + 1)
		g_ptr_array_add(args, g_strdup(bytes + offset));
	g_ptr_array_add(args, NULL);
	return (gchar **)g_ptr_array_free(g_steal_pointer(&args), FALSE);
}

/* Exact token matching prevents --print-timeout from satisfying --print. */
static gint
arg_index(gchar **argv, const gchar *value)
{
	guint i;
	for (i = 0; argv[i] != NULL; i++)
		if (strcmp(argv[i], value) == 0)
			return (gint)i;
	return -1;
}

/* Assert both the spelling and its adjacent value. */
static void
assert_pair(gchar **argv, const gchar *flag, const gchar *value)
{
	gint index = arg_index(argv, flag);
	g_assert_cmpint(index, >=, 0);
	g_assert_cmpstr(argv[index + 1], ==, value);
}

/* A command-only result must neither execute the stub nor read input; then
 * execute that actual output under bash, rather than merely parsing quotes.
 */
static Run *
execute_command(Fixture *f, const Case *c, const gchar * const *args,
	const gchar *input, gsize length)
{
	g_autofree gchar *path = g_build_filename(f->record, "argv", NULL);
	g_autoptr(Run) printed = NULL;
	const gchar *shell_args[] = { "--noprofile", "--norc", "-c", NULL, NULL };

	g_remove(path);
	printed = run_program(f, binaries[c->frontend], args, input, length);
	g_assert_cmpint(printed->status, ==, 0);
	g_assert_cmpint(printed->consumed, ==, 0);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	shell_args[3] = printed->out;
	return run_program(f, "/bin/bash", shell_args, input, length);
}

/* Both apps and all CLI types, plus the HTTP Claude alias, must use native
 * interactive/print syntax. A hostile leading-dash prompt stays one argument.
 */
static void
test_provider_modes(Fixture *f, gconstpointer data)
{
	const Case *c = (const Case *)data;
	const Provider *p = &providers[c->provider];
	const gchar *prompt = c->leading_dash ? "-dash 'single' \"double\" $HOME; $(: > INJECTED)\nnext" :
		"words 'single' \"double\" $HOME; $(: > INJECTED)\nnext";
	const gchar *args[] = { modes[c->mode], "-p", p->name,
		c->leading_dash ? "--" : prompt, c->leading_dash ? prompt : NULL, NULL };
	static const gchar input[] = "untouched\0stdin\n\n";
	g_autoptr(Run) run = NULL;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *exe = g_build_filename(f->tools, p->executable, NULL);
	g_autofree gchar *injected = g_build_filename(f->dir, "INJECTED", NULL);
	g_autofree gchar *received = NULL;
	g_autofree gchar *pid = NULL;
	g_autofree gchar *cwd = NULL;
	g_autofree gchar *attached = NULL;
	gsize length;
	gboolean print = c->mode == 2;

	if (!g_file_test(binaries[c->frontend], G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("frontend not built");
		return;
	}
	g_subprocess_launcher_setenv(f->launcher, "STUB_EXIT", "37", TRUE);
	if (c->mode == 0)
		run = run_program(f, binaries[c->frontend], args, input, sizeof input - 1);
	else
		run = execute_command(f, c, args, input, sizeof input - 1);
	g_assert_cmpint(run->status, ==, 37);
	g_assert_cmpstr(run->out, ==, "stub stdout\n");
	g_assert_cmpstr(run->err, ==, "stub stderr\n");
	argv = record_argv(f);
	g_assert_cmpstr(argv[0], ==, exe);
	pid = record_read(f, "pid", NULL);
	g_assert_cmpstr(pid, ==, run->pid);
	cwd = record_read(f, "cwd", NULL);
	g_assert_cmpstr(cwd, ==, f->dir);
	received = record_read(f, "stdin", &length);
	g_assert_cmpmem(received, length, input, sizeof input - 1);
	g_assert_false(g_file_test(injected, G_FILE_TEST_EXISTS));
	g_assert_cmpint(arg_index(argv, "--json"), ==, -1);
	g_assert_cmpint(arg_index(argv, "json"), ==, -1);
	g_assert_cmpint(arg_index(argv, "stream-json"), ==, -1);
	g_assert_cmpint(arg_index(argv, "streaming-messages-json"), ==, -1);
	g_assert_cmpint(arg_index(argv, "--input-format"), ==, -1);
	g_assert_cmpint(arg_index(argv, "--include-partial-messages"), ==, -1);
	g_assert_cmpint(arg_index(argv, "--stream-partial-output"), ==, -1);
	g_assert_cmpint(arg_index(argv, p->skip), ==, -1);
	if (strcmp(p->executable, "agy") == 0)
	{
		attached = g_strconcat(print ? "--print=" : "--prompt-interactive=", prompt, NULL);
		if (c->leading_dash)
			g_assert_cmpint(arg_index(argv, attached), >=, 0);
		else
			assert_pair(argv, print ? "--print" : "--prompt-interactive", prompt);
	}
	else if (strcmp(p->executable, "opencode") == 0 && !print)
	{
		attached = g_strconcat("--prompt=", prompt, NULL);
		if (c->leading_dash)
			g_assert_cmpint(arg_index(argv, attached), >=, 0);
		else
			assert_pair(argv, "--prompt", prompt);
	}
	else if (strcmp(p->executable, "grok") == 0 && print)
	{
		attached = g_strconcat("--single=", prompt, NULL);
		if (c->leading_dash)
			g_assert_cmpint(arg_index(argv, attached), >=, 0);
		else
			assert_pair(argv, "--single", prompt);
	}
	else
		assert_pair(argv, "--", prompt);
	if (!print)
	{
		g_assert_cmpint(arg_index(argv, "--print"), ==, -1);
		g_assert_cmpint(arg_index(argv, "--output-format"), ==, -1);
		g_assert_cmpint(arg_index(argv, "--format"), ==, -1);
		g_assert_cmpint(arg_index(argv, "run"), ==, -1);
		g_assert_cmpint(arg_index(argv, "exec"), ==, -1);
		g_assert_cmpint(arg_index(argv, "--prompt-file"), ==, -1);
	}
	else if (strcmp(p->executable, "claude") == 0 || strcmp(p->executable, "cursor") == 0)
		g_assert_cmpint(arg_index(argv, "--print"), >=, 0);
	else if (strcmp(p->executable, "opencode") == 0)
	{
		g_assert_cmpstr(argv[1], ==, "run");
		assert_pair(argv, "--format", "default");
	}
	else if (strcmp(p->executable, "codex") == 0)
		g_assert_cmpint(arg_index(argv, "exec"), >=, 0);
	else if (strcmp(p->executable, "grok") == 0)
		assert_pair(argv, "--output-format", "plain");
}

/* No positional prompt leaves stdin native; only agy's generated print command
 * consumes it into a flag. It must not do so while merely printing the command.
 */
static void
test_no_prompt(Fixture *f, gconstpointer data)
{
	const Case *c = (const Case *)data;
	const Provider *p = &providers[c->provider];
	const gchar *args[] = { modes[c->mode], "-p", p->name, NULL };
	const gchar *input = "-stdin 'quoted' $HOME; $(: > INJECTED)";
	g_autoptr(Run) run = NULL;
	g_auto(GStrv) argv = NULL;
	g_autofree gchar *received = NULL;
	g_autofree gchar *attached = NULL;
	g_autofree gchar *injected = g_build_filename(f->dir, "INJECTED", NULL);

	if (!g_file_test(binaries[c->frontend], G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("frontend not built");
		return;
	}
	if (c->mode == 0)
		run = run_program(f, binaries[c->frontend], args, input, strlen(input));
	else
		run = execute_command(f, c, args, input, strlen(input));
	g_assert_cmpint(run->status, ==, 0);
	argv = record_argv(f);
	g_assert_false(g_file_test(injected, G_FILE_TEST_EXISTS));
	received = record_read(f, "stdin", NULL);
	if (c->mode == 2 && strcmp(p->executable, "agy") == 0)
	{
		attached = g_strconcat("--print=", input, NULL);
		g_assert_cmpint(arg_index(argv, attached), >=, 0);
		g_assert_cmpstr(received, ==, "");
	}
	else
		g_assert_cmpstr(received, ==, input);
	if (c->mode == 2 && strcmp(p->executable, "grok") == 0)
		assert_pair(argv, "--prompt-file", "/dev/stdin");
	if (c->mode == 2 && strcmp(p->executable, "codex") == 0)
		assert_pair(argv, "--", "-");
}

/* --set overrides dedicated model flags; permission and resume mappings are
 * checked on executed commands, including OpenCode's environment assignment.
 */
static void
test_properties(Fixture *f, gconstpointer data)
{
	const Case *c = (const Case *)data;
	const Provider *p = &providers[c->provider];
	guint resume;

	if (!g_file_test(binaries[c->frontend], G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("frontend not built");
		return;
	}
	for (resume = 0; resume < 2; resume++)
	{
		const gchar *args[] = { modes[c->mode], "-p", p->name,
			"-m", "flag-model", "--set", "model=override-model",
			"--skip-permissions", "--continue",
			resume ? "--set" : NULL, "session-id=session-123", NULL };
		g_autoptr(Run) run = execute_command(f, c, args, "", 0);
		g_auto(GStrv) argv = record_argv(f);

		g_assert_cmpint(run->status, ==, 0);
		assert_pair(argv, "--model", "override-model");
		g_assert_cmpint(arg_index(argv, "flag-model"), ==, -1);
		g_assert_cmpint(arg_index(argv, p->skip), >=, 0);
		if (strcmp(p->executable, "grok") == 0)
			assert_pair(argv, "--permission-mode", "bypassPermissions");
		if (strcmp(p->executable, "opencode") == 0)
		{
			g_autofree gchar *permission = record_read(f, "permission", NULL);
			g_assert_cmpstr(permission, ==, "{\"*\":\"allow\"}");
		}
		if (strcmp(p->executable, "codex") == 0)
		{
			g_assert_cmpint(arg_index(argv, "resume"), >=, 0);
			if (resume)
				assert_pair(argv, "--", "session-123");
			else
				g_assert_cmpint(arg_index(argv, "--last"), >=, 0);
		}
		else if (resume)
		{
			assert_pair(argv, p->session, "session-123");
			g_assert_cmpint(arg_index(argv, "--continue"), ==, -1);
		}
		else
			g_assert_cmpint(arg_index(argv, "--continue"), >=, 0);
	}
	if (strcmp(p->executable, "claude") == 0)
	{
		const gchar *args[] = { modes[c->mode], "-p", p->name, "--set", "tools=", NULL };
		g_autoptr(Run) run = execute_command(f, c, args, "", 0);
		g_auto(GStrv) argv = record_argv(f);

		g_assert_cmpint(run->status, ==, 0);
		assert_pair(argv, "--tools", "");
	}
}

/* App scopes must beat library defaults, while explicit default bypasses the
 * environment. An environment-selected provider must not inherit another model.
 */
static void
test_defaults(Fixture *f, gconstpointer data)
{
	const Case *c = (const Case *)data;
	g_autofree gchar *dir = g_build_filename(f->config, "ai-glib", NULL);
	g_autofree gchar *path = g_build_filename(dir, "config.yaml", NULL);
	guint variant;

	if (!g_file_test(binaries[c->frontend], G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("frontend not built");
		return;
	}
	g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path,
		"default_provider: opencode\ndefault_model: library-model\n"
		"apps:\n  ai:\n    default_provider: claude\n    default_model: opus\n"
		"  ai-tui:\n    default_provider: cursor\n    default_model: cursor-saved\n", -1, NULL));
	for (variant = 0; variant < 3; variant++)
	{
		const gchar *args[] = { "--launch-cmd", variant == 2 ? "-p" : NULL,
			"default", "-m", "default", NULL };
		g_autoptr(Run) run = NULL;
		g_auto(GStrv) argv = NULL;
		g_autofree gchar *exe = NULL;

		if (variant != 0)
			g_subprocess_launcher_setenv(f->launcher, "AI_PROVIDER", "grok-build", TRUE);
		run = execute_command(f, c, args, "", 0);
		g_assert_cmpint(run->status, ==, 0);
		argv = record_argv(f);
		exe = g_build_filename(f->tools, variant == 1 ? "grok" :
			c->frontend == 0 ? "claude" : "cursor", NULL);
		g_assert_cmpstr(argv[0], ==, exe);
		g_assert_cmpint(arg_index(argv, "library-model"), ==, -1);
		if (variant != 1)
			assert_pair(argv, "--model", c->frontend == 0 ? "opus" : "cursor-saved");
		else
		{
			g_assert_cmpint(arg_index(argv, "opus"), ==, -1);
			g_assert_cmpint(arg_index(argv, "cursor-saved"), ==, -1);
		}
	}
}

/* Executable paths and working directories containing shell metacharacters
 * must work both through execve and through a printed command's cd subshell.
 */
static void
test_paths_and_ollama(Fixture *f, gconstpointer data)
{
	const Case *c = (const Case *)data;
	g_autofree gchar *work = g_build_filename(f->dir, "work ' $; space", NULL);
	g_autofree gchar *stub = g_build_filename(f->tools, "custom ' $; cli", NULL);
	g_autofree gchar *cwd_property = g_strconcat("working-directory=", work, NULL);
	g_autofree gchar *exe_property = g_strconcat("executable-path=", stub, NULL);
	guint mode;

	if (!g_file_test(binaries[c->frontend], G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("frontend not built");
		return;
	}
	g_assert_cmpint(g_mkdir(work, 0700), ==, 0);
	g_assert_true(g_file_set_contents(stub, stub_script, -1, NULL));
	g_assert_cmpint(g_chmod(stub, 0700), ==, 0);
	for (mode = 0; mode < G_N_ELEMENTS(modes); mode++)
	{
		const gchar *args[] = { modes[mode], "-p", "cursor", "--set", exe_property,
			"--set", cwd_property, "hello", NULL };
		g_autoptr(Run) run = mode == 0 ? run_program(f, binaries[c->frontend], args, "raw\n", 4) :
			execute_command(f, c, args, "raw\n", 4);
		g_auto(GStrv) argv = record_argv(f);
		g_autofree gchar *cwd = record_read(f, "cwd", NULL);

		g_assert_cmpint(run->status, ==, 0);
		g_assert_cmpstr(argv[0], ==, stub);
		g_assert_cmpstr(cwd, ==, work);
	}
	for (mode = 0; mode < G_N_ELEMENTS(modes); mode++)
	{
		guint alias;
		for (alias = 0; alias < 2; alias++)
		{
			const gchar *args[] = { modes[mode], "-p", alias ? "claude-tmux" : "claude",
				"-m", "ollama/local-model", "hello", NULL };
			g_autoptr(Run) run = mode == 0 ? run_program(f, binaries[c->frontend], args, "", 0) :
				execute_command(f, c, args, "", 0);
			g_auto(GStrv) argv = record_argv(f);
			g_autofree gchar *ollama = g_build_filename(f->tools, "ollama", NULL);
			guint i, models = 0;

			g_assert_cmpint(run->status, ==, 0);
			g_assert_cmpstr(argv[0], ==, ollama);
			g_assert_cmpstr(argv[1], ==, "launch");
			g_assert_cmpstr(argv[2], ==, "claude");
			assert_pair(argv, "--model", "local-model");
			g_assert_cmpstr(argv[5], ==, "--");
			for (i = 0; argv[i] != NULL; i++)
				if (strcmp(argv[i], "--model") == 0) models++;
			g_assert_cmpuint(models, ==, 1);
			if (mode == 2)
				g_assert_cmpint(arg_index(argv, "--print"), >=, 0);
			else
				g_assert_cmpint(arg_index(argv, "--print"), ==, -1);
		}
	}
}

/* Refused combinations and secret-valued properties must fail before running
 * anything or consuming stdin. Check diagnostics, not merely exit status.
 */
static void
test_errors(Fixture *f, gconstpointer data)
{
	const Case *c = (const Case *)data;
	const gchar *http[] = { "openai", "gemini", "grok", "ollama" };
	const gchar *conflicts[2][5] = {
		{ "--dry-run", "--interactive", "--setup", "--image-gen", "--list-image-models" },
		{ "--dry-run", "--local-tools", "--yes", "--dump=hello", NULL }
	};
	g_autofree gchar *record = g_build_filename(f->record, "argv", NULL);
	guint i, mode;

	if (!g_file_test(binaries[c->frontend], G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("frontend not built");
		return;
	}
	for (mode = 0; mode < G_N_ELEMENTS(modes); mode++)
	{
		for (i = 0; i < G_N_ELEMENTS(http); i++)
		{
			const gchar *args[] = { modes[mode], "-p", http[i], NULL };
			g_autoptr(Run) run = run_program(f, binaries[c->frontend], args, "unread", 6);
			g_assert_cmpint(run->status, ==, 2);
			g_assert_nonnull(strstr(run->err, "CLI provider"));
			g_assert_cmpint(run->consumed, ==, 0);
		}
		for (i = mode + 1; i < G_N_ELEMENTS(modes); i++)
		{
			const gchar *args[] = { modes[mode], modes[i], NULL };
			g_autoptr(Run) run = run_program(f, binaries[c->frontend], args, "unread", 6);
			g_assert_cmpint(run->status, ==, 2);
			g_assert_nonnull(strstr(run->err, "choose one launch mode"));
			g_assert_cmpint(run->consumed, ==, 0);
		}
		for (i = 0; i < G_N_ELEMENTS(conflicts[0]) && conflicts[c->frontend][i] != NULL; i++)
		{
			const gchar *args[] = { modes[mode], conflicts[c->frontend][i], NULL };
			g_autoptr(Run) run = run_program(f, binaries[c->frontend], args, "unread", 6);
			g_assert_cmpint(run->status, ==, 2);
			g_assert_nonnull(strstr(run->err, "cannot be combined"));
			g_assert_cmpint(run->consumed, ==, 0);
		}
	}
	for (mode = 1; mode < G_N_ELEMENTS(modes); mode++)
	{
		const gchar *args[] = { modes[mode], "-p", "cursor", "--set",
			"api-key=TEST-SECRET-DO-NOT-PRINT", NULL };
		g_autoptr(Run) run = run_program(f, binaries[c->frontend], args, "unread", 6);
		g_assert_cmpint(run->status, ==, 2);
		g_assert_nonnull(strstr(run->err, "CURSOR_API_KEY"));
		g_assert_null(strstr(run->out, "TEST-SECRET-DO-NOT-PRINT"));
		g_assert_null(strstr(run->err, "TEST-SECRET-DO-NOT-PRINT"));
		g_assert_cmpint(run->consumed, ==, 0);
	}
	g_assert_false(g_file_test(record, G_FILE_TEST_EXISTS));
	/* Matching inherited credentials are safe to rely on, but never to echo. */
	g_subprocess_launcher_setenv(f->launcher, "CURSOR_API_KEY", "TEST-SECRET-DO-NOT-PRINT", TRUE);
	for (mode = 1; mode < G_N_ELEMENTS(modes); mode++)
	{
		const gchar *args[] = { modes[mode], "-p", "cursor", "--set",
			"api-key=TEST-SECRET-DO-NOT-PRINT", NULL };
		g_autoptr(Run) run = run_program(f, binaries[c->frontend], args, "unread", 6);
		g_assert_cmpint(run->status, ==, 0);
		g_assert_null(strstr(run->out, "TEST-SECRET-DO-NOT-PRINT"));
		g_assert_null(strstr(run->err, "TEST-SECRET-DO-NOT-PRINT"));
		g_assert_cmpint(run->consumed, ==, 0);
		g_assert_false(g_file_test(record, G_FILE_TEST_EXISTS));
	}
	{
		const gchar *missing[] = { "--launch", "-p", "cursor", "--set",
			"executable-path=/nonexistent-ai-launch-test-binary", NULL };
		const gchar *bad_cwd[] = { "--launch", "-p", "cursor", "--set",
			"working-directory=/nonexistent-ai-launch-test-directory", NULL };
		const gchar *unsupported[] = { "--launch-cmd", "-p", "claude", "--set",
			"fallback-model=sonnet", NULL };
		g_autoptr(Run) run = run_program(f, binaries[c->frontend], missing, "unread", 6);

		g_assert_cmpint(run->status, ==, 127);
		g_assert_cmpint(run->consumed, ==, 0);
		g_clear_pointer(&run, run_free);
		run = run_program(f, binaries[c->frontend], bad_cwd, "unread", 6);
		g_assert_cmpint(run->status, ==, 126);
		g_assert_cmpint(run->consumed, ==, 0);
		g_clear_pointer(&run, run_free);
		run = run_program(f, binaries[c->frontend], unsupported, "unread", 6);
		g_assert_cmpint(run->status, ==, 2);
		g_assert_nonnull(strstr(run->err, "fallback-model"));
		g_assert_false(g_file_test(record, G_FILE_TEST_EXISTS));
	}
}

/* Locate sibling binaries before any sandbox cwd is selected. Cases stay alive
 * until g_test_run finishes; absent optional ai-tui is reported as a real skip.
 */
int
main(int argc, char **argv)
{
	g_autofree gchar *self = g_file_read_link("/proc/self/exe", NULL);
	g_autofree gchar *dir = NULL;
	g_autoptr(GPtrArray) cases = g_ptr_array_new_with_free_func(g_free);
	guint frontend, provider, mode;
	gint status;

	g_test_init(&argc, &argv, NULL);
	if (self == NULL) self = g_canonicalize_filename(argv[0], NULL);
	dir = g_path_get_dirname(self);
	for (frontend = 0; frontend < 2; frontend++)
	{
		const gchar *name = frontend == 0 ? "ai" : "ai-tui";
		Case *app = g_new0(Case, 1);
		g_autofree gchar *defaults_path = g_strdup_printf("/ai-glib/launch/%s/defaults", name);
		g_autofree gchar *paths_path = g_strdup_printf("/ai-glib/launch/%s/paths-ollama", name);
		g_autofree gchar *errors_path = g_strdup_printf("/ai-glib/launch/%s/errors", name);

		binaries[frontend] = g_build_filename(dir, "..", "bin", name, NULL);
		app->frontend = frontend;
		g_ptr_array_add(cases, app);
		g_test_add(defaults_path, Fixture, app, fixture_setup, test_defaults, fixture_teardown);
		g_test_add(paths_path, Fixture, app, fixture_setup, test_paths_and_ollama, fixture_teardown);
		g_test_add(errors_path, Fixture, app, fixture_setup, test_errors, fixture_teardown);
		for (provider = 0; provider < G_N_ELEMENTS(providers); provider++)
			for (mode = 0; mode < G_N_ELEMENTS(modes); mode++)
			{
				Case *c = g_new0(Case, 1);
				Case *leading = g_new0(Case, 1);
				g_autofree gchar *path = g_strdup_printf("/ai-glib/launch/%s/%s/%s",
					name, providers[provider].name, modes[mode] + 2);
				g_autofree gchar *stdin_path = g_strconcat(path, "-no-prompt", NULL);
				g_autofree gchar *leading_path = g_strconcat(path, "-leading-dash", NULL);

				c->frontend = frontend;
				c->provider = provider;
				c->mode = mode;
				g_ptr_array_add(cases, c);
				*leading = *c;
				leading->leading_dash = TRUE;
				g_ptr_array_add(cases, leading);
				g_test_add(path, Fixture, c, fixture_setup, test_provider_modes, fixture_teardown);
				g_test_add(leading_path, Fixture, leading, fixture_setup, test_provider_modes, fixture_teardown);
				g_test_add(stdin_path, Fixture, c, fixture_setup, test_no_prompt, fixture_teardown);
				if (mode != 0)
				{
					g_autofree gchar *props = g_strconcat(path, "-properties", NULL);
					g_test_add(props, Fixture, c, fixture_setup, test_properties, fixture_teardown);
				}
			}
	}
	/* Bound regressions that accidentally enter an interactive harness. */
	alarm(120);
	status = g_test_run();
	alarm(0);
	g_free(binaries[0]);
	g_free(binaries[1]);
	return status;
}
