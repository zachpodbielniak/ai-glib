/*
 * test-ai-cli.c - Tests for the `ai` command-line front-end
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These spawn the real `ai` binary. Everything the library-level tests
 * cover is reachable from C directly; what only these can check is that
 * the CLI actually wires a provider up -- that `-p grok-build` selects the
 * right client, that --set reaches its properties, that --dry-run prints
 * the command that would really be spawned, and that a full run prints
 * the model's answer.
 *
 * Two things keep them hermetic: --dry-run spawns nothing, and the runs
 * that do spawn point GROK_PATH at a stub script. No network, no real
 * grok, no API key.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

/* Path to the binary under test, resolved in main(). */
static gchar *ai_bin = NULL;

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

typedef struct
{
	gchar *stdout_data;
	gchar *stderr_data;
	gint   exit_status;
} Run;

static void
run_clear(Run *run)
{
	g_clear_pointer(&run->stdout_data, g_free);
	g_clear_pointer(&run->stderr_data, g_free);
}

/*
 * Run `ai` with @args (NULL-terminated, without argv[0]).
 *
 * @grok_path, when given, becomes the child's GROK_PATH so the client
 * resolves the stub instead of a real grok. The variable is explicitly
 * cleared otherwise: a developer machine may well have one exported, and
 * a test that passes only there is worse than no test.
 */
static void
run_ai(const gchar * const *args, const gchar *grok_path, Run *out)
{
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	gsize i;

	g_ptr_array_add(argv, ai_bin);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer) args[i]);
	g_ptr_array_add(argv, NULL);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_unsetenv(launcher, "AI_PROVIDER");
	g_subprocess_launcher_unsetenv(launcher, "AI_GLIB_DEFAULT_PROVIDER");

	if (grok_path != NULL)
		g_subprocess_launcher_setenv(launcher, "GROK_PATH", grok_path, TRUE);
	else
		g_subprocess_launcher_unsetenv(launcher, "GROK_PATH");

	proc = g_subprocess_launcher_spawnv(launcher,
	                                    (const gchar * const *) argv->pdata,
	                                    &error);
	g_assert_no_error(error);

	g_subprocess_communicate_utf8(proc, NULL, NULL,
	                              &out->stdout_data, &out->stderr_data,
	                              &error);
	g_assert_no_error(error);

	out->exit_status = g_subprocess_get_exit_status(proc);
}

/*
 * A stub `grok` that swallows its stdin and prints @payload. Returns the
 * directory holding it; free with stub_dir_free().
 */
static gchar *
stub_dir_new(const gchar *payload, gchar **out_stub_path)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *script = NULL;
	g_autofree gchar *payload_path = NULL;
	gchar *dir;
	gchar *stub;

	dir = g_dir_make_tmp("ai-glib-aicli-XXXXXX", &error);
	g_assert_no_error(error);

	payload_path = g_build_filename(dir, "payload", NULL);
	g_file_set_contents(payload_path, payload, -1, &error);
	g_assert_no_error(error);

	stub = g_build_filename(dir, "grok", NULL);
	script = g_strdup_printf("#!/bin/sh\ncat > /dev/null\ncat '%s'\n",
	                         payload_path);
	g_file_set_contents(stub, script, -1, &error);
	g_assert_no_error(error);
	g_assert_cmpint(g_chmod(stub, 0700), ==, 0);

	*out_stub_path = stub;
	return dir;
}

static void
stub_dir_free(gchar *dir, gchar *stub)
{
	g_autofree gchar *payload_path = g_build_filename(dir, "payload", NULL);

	g_remove(payload_path);
	g_remove(stub);
	g_rmdir(dir);
	g_free(stub);
	g_free(dir);
}

/* The argv --dry-run prints, minus the resolved executable in front. */
static void
assert_dry_run_contains(const gchar *output, const gchar *needle)
{
	g_assert_nonnull(output);
	if (strstr(output, needle) == NULL)
		g_error("expected '%s' in dry-run output:\n%s", needle, output);
}

static void
assert_dry_run_lacks(const gchar *output, const gchar *needle)
{
	g_assert_nonnull(output);
	if (strstr(output, needle) != NULL)
		g_error("did not expect '%s' in dry-run output:\n%s", needle, output);
}

/* ----------------------------------------------------------------
 * Provider selection
 * ---------------------------------------------------------------- */

/* grok-build must be listed, and listed as a CLI provider. */
static void
test_cli_list_providers(void)
{
	const gchar *args[] = { "--list-providers", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	g_assert_nonnull(strstr(run.stdout_data, "grok-build"));
	g_assert_nonnull(strstr(run.stdout_data, "grok-build     CLI"));
	/* And the HTTP one is still distinct. */
	g_assert_nonnull(strstr(run.stdout_data, "grok           HTTP"));

	run_clear(&run);
}

/* The --provider help text has to name it or nobody will find it. */
static void
test_cli_help_mentions_provider(void)
{
	const gchar *args[] = { "--help", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	g_assert_nonnull(strstr(run.stdout_data, "grok-build"));
	g_assert_nonnull(strstr(run.stdout_data, "--set"));

	run_clear(&run);
}

/* Both spellings select the CLI provider, not the HTTP one. */
static void
test_cli_provider_aliases(void)
{
	const gchar *hyphen[] = { "-p", "grok-build", "--dry-run", "hi", NULL };
	const gchar *underscore[] = { "-p", "grok_build", "--dry-run", "hi", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(hyphen, NULL, &run);
	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--prompt-file /dev/stdin");
	run_clear(&run);

	run_ai(underscore, NULL, &run);
	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--prompt-file /dev/stdin");
	run_clear(&run);
}

/* ----------------------------------------------------------------
 * --dry-run
 * ---------------------------------------------------------------- */

static void
test_cli_dry_run_basic(void)
{
	const gchar *args[] = { "-p", "grok-build", "--dry-run", "hello", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--prompt-file /dev/stdin");
	assert_dry_run_contains(run.stdout_data, "--output-format json");
	assert_dry_run_contains(run.stdout_data, "--model grok-4.6");
	assert_dry_run_contains(run.stdout_data, "--verbatim");
	assert_dry_run_contains(run.stdout_data, "--reasoning-effort medium");
	/* The prompt is piped, never an argument. */
	assert_dry_run_lacks(run.stdout_data, "hello");

	run_clear(&run);
}

static void
test_cli_dry_run_model_system_effort(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "-m", "grok-4.5", "-s", "Be terse.",
		"--effort", "high", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--model grok-4.5");
	assert_dry_run_contains(run.stdout_data,
	                        "--system-prompt-override \"Be terse.\"");
	assert_dry_run_contains(run.stdout_data, "--reasoning-effort high");

	run_clear(&run);
}

/* --effort max is folded onto xhigh rather than rejected by grok. */
static void
test_cli_dry_run_effort_max(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--effort", "max", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--reasoning-effort xhigh");

	run_clear(&run);
}

static void
test_cli_dry_run_skip_permissions(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--skip-permissions", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data,
	                        "--permission-mode bypassPermissions");

	run_clear(&run);
}

static void
test_cli_dry_run_streaming(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--stream", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data,
	                        "--output-format streaming-messages-json");
	assert_dry_run_contains(run.stdout_data, "--include-partial-messages");

	run_clear(&run);
}

/* ----------------------------------------------------------------
 * --set
 * ---------------------------------------------------------------- */

/* The knobs that have no dedicated flag are still reachable. */
static void
test_cli_set_properties(void)
{
	const gchar *args[] = {
		"-p", "grok-build",
		"--set", "sandbox=workspace",
		"--set", "max-turns=20",
		"--set", "permission-mode=acceptEdits",
		"--set", "agent=reviewer",
		"--set", "rules=Never touch vendor/.",
		"--set", "allowed-tools=read_file,list_dir",
		"--set", "disallowed-tools=run_terminal_command",
		"--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--sandbox workspace");
	assert_dry_run_contains(run.stdout_data, "--max-turns 20");
	assert_dry_run_contains(run.stdout_data, "--permission-mode acceptEdits");
	assert_dry_run_contains(run.stdout_data, "--agent reviewer");
	assert_dry_run_contains(run.stdout_data, "--rules");
	assert_dry_run_contains(run.stdout_data, "--allow read_file");
	assert_dry_run_contains(run.stdout_data, "--allow list_dir");
	assert_dry_run_contains(run.stdout_data, "--deny run_terminal_command");

	run_clear(&run);
}

/* A bare --set NAME is a boolean switch. */
static void
test_cli_set_bare_boolean(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--set", "disable-web-search",
		"--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--disable-web-search");

	run_clear(&run);
}

/* Turning a default-on boolean off must work too. */
static void
test_cli_set_boolean_false(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--set", "verbatim=false", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_lacks(run.stdout_data, "--verbatim");

	run_clear(&run);
}

/* Inherited AiCliClient properties are reachable by the same route. */
static void
test_cli_set_inherited_property(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--set", "session-id=sess-99", "--dry-run",
		"hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--resume sess-99");

	run_clear(&run);
}

/* An unknown property fails loudly and says what the provider does take. */
static void
test_cli_set_unknown_property(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--set", "nope=1", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 2);
	g_assert_nonnull(strstr(run.stderr_data, "no property 'nope'"));
	/* The list is the useful part of the error. */
	g_assert_nonnull(strstr(run.stderr_data, "sandbox"));
	g_assert_nonnull(strstr(run.stderr_data, "max-turns"));

	run_clear(&run);
}

/* A read-only property is refused rather than silently ignored. */
static void
test_cli_set_read_only_property(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--set", "total-cost=1.5", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 2);
	g_assert_nonnull(strstr(run.stderr_data, "read-only"));

	run_clear(&run);
}

/*
 * A value that does not parse is an error, not a silent 0 -- otherwise the
 * run happens without the bound the user asked for.
 */
static void
test_cli_set_bad_value(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "--set", "max-turns=abc", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 2);
	g_assert_nonnull(strstr(run.stderr_data, "cannot read 'abc'"));

	run_clear(&run);
}

/* --set wins over the dedicated flag for the same knob. */
static void
test_cli_set_overrides_flag(void)
{
	const gchar *args[] = {
		"-p", "grok-build", "-m", "grok-4.6", "--set", "model=grok-4.5",
		"--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--model grok-4.5");
	assert_dry_run_lacks(run.stdout_data, "--model grok-4.6");

	run_clear(&run);
}

/* ----------------------------------------------------------------
 * Other CLI providers
 * ---------------------------------------------------------------- */

/*
 * --dry-run goes through the AiCliClient vtable rather than a branch per
 * provider, so every CLI provider gets it. opencode had none before that
 * change and fell through to the one-line summary.
 */
static void
test_cli_dry_run_opencode(void)
{
	const gchar *args[] = { "-p", "opencode", "--dry-run", "hi", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "run");
	assert_dry_run_contains(run.stdout_data, "--format json");
	assert_dry_run_contains(run.stdout_data, "--model");
	/* The summary line means the vtable branch did not fire. */
	assert_dry_run_lacks(run.stdout_data, "no CLI subprocess");

	run_clear(&run);
}

static void
test_cli_dry_run_claude_code(void)
{
	const gchar *args[] = { "-p", "claude-code", "--dry-run", "hi", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--print");
	assert_dry_run_contains(run.stdout_data, "--output-format json");

	run_clear(&run);
}

/*
 * --skip-permissions reaches opencode as --auto. The flag was accepted
 * and silently ignored for this provider before: `ai` only applied it to
 * the claude providers.
 */
static void
test_cli_skip_permissions_opencode(void)
{
	const gchar *off[] = { "-p", "opencode", "--dry-run", "hi", NULL };
	const gchar *on[] = {
		"-p", "opencode", "--skip-permissions", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(off, NULL, &run);
	assert_dry_run_lacks(run.stdout_data, "--auto");
	run_clear(&run);

	run_ai(on, NULL, &run);
	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--auto");
	run_clear(&run);
}

/* The opencode knobs with no dedicated flag are reachable through --set. */
static void
test_cli_set_opencode_properties(void)
{
	const gchar *args[] = {
		"-p", "opencode",
		"--set", "agent=build",
		"--set", "title=nightly",
		"--set", "port=4096",
		"--set", "thinking",
		"--set", "pure",
		"--set", "log-level=WARN",
		"--set", "files=a.txt,b.txt",
		"--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--agent build");
	assert_dry_run_contains(run.stdout_data, "--title nightly");
	assert_dry_run_contains(run.stdout_data, "--port 4096");
	assert_dry_run_contains(run.stdout_data, "--thinking");
	assert_dry_run_contains(run.stdout_data, "--pure");
	assert_dry_run_contains(run.stdout_data, "--log-level WARN");
	assert_dry_run_contains(run.stdout_data, "--file a.txt");
	assert_dry_run_contains(run.stdout_data, "--file b.txt");

	run_clear(&run);
}

/* Same for the claude-code knobs. */
static void
test_cli_set_claude_code_properties(void)
{
	const gchar *args[] = {
		"-p", "claude-code",
		"--set", "agent=reviewer",
		"--set", "max-budget-usd=1.25",
		"--set", "tools=Bash,Read",
		"--set", "bare",
		"--set", "strict-mcp-config",
		"--set", "fallback-model=haiku",
		"--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	assert_dry_run_contains(run.stdout_data, "--agent reviewer");
	assert_dry_run_contains(run.stdout_data, "--max-budget-usd 1.25");
	assert_dry_run_contains(run.stdout_data, "--tools Bash Read");
	assert_dry_run_contains(run.stdout_data, "--bare");
	assert_dry_run_contains(run.stdout_data, "--strict-mcp-config");
	assert_dry_run_contains(run.stdout_data, "--fallback-model haiku");

	run_clear(&run);
}

/* The property list in the error is the provider's own, not a fixed one. */
static void
test_cli_set_unknown_property_lists_provider(void)
{
	const gchar *args[] = {
		"-p", "opencode", "--set", "sandbox=workspace", "--dry-run", "hi", NULL
	};
	Run run = { NULL, NULL, 0 };

	run_ai(args, NULL, &run);

	/* sandbox is a grok-build knob; opencode has no such property. */
	g_assert_cmpint(run.exit_status, ==, 2);
	g_assert_nonnull(strstr(run.stderr_data, "AiOpenCodeClient"));
	g_assert_nonnull(strstr(run.stderr_data, "thinking"));

	run_clear(&run);
}

/* ----------------------------------------------------------------
 * Real runs, against a stub grok
 * ---------------------------------------------------------------- */

static void
test_cli_run_prints_answer(void)
{
	const gchar *args[] = { "-p", "grok-build", "What is 2+2?", NULL };
	g_autofree gchar *stub = NULL;
	gchar *dir;
	Run run = { NULL, NULL, 0 };

	dir = stub_dir_new("{\"text\":\"4\",\"stopReason\":\"end_turn\","
	                   "\"sessionId\":\"s\"}", &stub);

	run_ai(args, stub, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	g_assert_cmpstr(g_strstrip(run.stdout_data), ==, "4");

	run_clear(&run);
	stub_dir_free(dir, g_steal_pointer(&stub));
}

/* The zero-exit error payload must fail the CLI, not print nothing. */
static void
test_cli_run_reports_error(void)
{
	const gchar *args[] = { "-p", "grok-build", "hi", NULL };
	g_autofree gchar *stub = NULL;
	gchar *dir;
	Run run = { NULL, NULL, 0 };

	dir = stub_dir_new("{\"type\":\"error\",\"message\":\"not logged in\"}",
	                   &stub);

	run_ai(args, stub, &run);

	g_assert_cmpint(run.exit_status, !=, 0);
	g_assert_nonnull(strstr(run.stderr_data, "not logged in"));

	run_clear(&run);
	stub_dir_free(dir, g_steal_pointer(&stub));
}

/* --stream prints the deltas as they arrive. */
static void
test_cli_run_streaming(void)
{
	const gchar *args[] = { "-p", "grok-build", "--stream", "hi", NULL };
	g_autofree gchar *stub = NULL;
	gchar *dir;
	Run run = { NULL, NULL, 0 };

	dir = stub_dir_new(
		"{\"type\":\"system\",\"subtype\":\"init\"}\n"
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"HEL\"}}}\n"
		"{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\","
		"\"delta\":{\"type\":\"text_delta\",\"text\":\"LO\"}}}\n"
		"{\"type\":\"result\",\"result\":\"HELLO\",\"session_id\":\"s\"}\n",
		&stub);

	run_ai(args, stub, &run);

	g_assert_cmpint(run.exit_status, ==, 0);
	g_assert_nonnull(strstr(run.stdout_data, "HELLO"));

	run_clear(&run);
	stub_dir_free(dir, g_steal_pointer(&stub));
}

/* A prompt on stdin reaches the provider just as a positional one does. */
static void
test_cli_run_prompt_from_stdin(void)
{
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *out = NULL;
	g_autofree gchar *stub = NULL;
	gchar *dir;

	dir = stub_dir_new("{\"text\":\"piped\"}", &stub);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_setenv(launcher, "GROK_PATH", stub, TRUE);

	proc = g_subprocess_launcher_spawn(launcher, &error, ai_bin,
	                                   "-p", "grok-build", NULL);
	g_assert_no_error(error);

	g_subprocess_communicate_utf8(proc, "hello from stdin\n", NULL,
	                              &out, NULL, &error);
	g_assert_no_error(error);

	g_assert_cmpint(g_subprocess_get_exit_status(proc), ==, 0);
	g_assert_cmpstr(g_strstrip(out), ==, "piped");

	stub_dir_free(dir, g_steal_pointer(&stub));
}

/* A missing binary is reported, not retried into a hang. */
static void
test_cli_run_missing_binary(void)
{
	const gchar *args[] = { "-p", "grok-build", "hi", NULL };
	Run run = { NULL, NULL, 0 };

	run_ai(args, "/nonexistent/grok", &run);

	g_assert_cmpint(run.exit_status, !=, 0);
	g_assert_nonnull(strstr(run.stderr_data, "not found"));

	run_clear(&run);
}

/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */

/*
 * Find the `ai` binary next to this test: tests land in
 * build/<type>/tests/ and binaries in build/<type>/bin/, so it is one
 * directory over regardless of which build type is running or what the
 * working directory happens to be.
 */
static gchar *
find_ai_binary(const gchar *argv0)
{
	g_autofree gchar *self = NULL;
	g_autofree gchar *dir = NULL;
	gchar *candidate;

	self = g_file_read_link("/proc/self/exe", NULL);
	if (self == NULL)
		self = g_strdup(argv0);

	dir = g_path_get_dirname(self);
	candidate = g_build_filename(dir, "..", "bin", "ai", NULL);

	if (g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
		return candidate;

	g_free(candidate);
	return NULL;
}

int
main(
	int   argc,
	char *argv[]
){
	int status;

	g_test_init(&argc, &argv, NULL);

	ai_bin = find_ai_binary(argv[0]);
	if (ai_bin == NULL)
	{
		g_printerr("SKIP: the `ai` binary was not found next to this test "
		           "(run `make` first)\n");
		return 0;
	}

	g_test_add_func("/ai-glib/ai-cli/list-providers",
	                test_cli_list_providers);
	g_test_add_func("/ai-glib/ai-cli/help-mentions-provider",
	                test_cli_help_mentions_provider);
	g_test_add_func("/ai-glib/ai-cli/provider-aliases",
	                test_cli_provider_aliases);

	g_test_add_func("/ai-glib/ai-cli/dry-run/basic",
	                test_cli_dry_run_basic);
	g_test_add_func("/ai-glib/ai-cli/dry-run/model-system-effort",
	                test_cli_dry_run_model_system_effort);
	g_test_add_func("/ai-glib/ai-cli/dry-run/effort-max",
	                test_cli_dry_run_effort_max);
	g_test_add_func("/ai-glib/ai-cli/dry-run/skip-permissions",
	                test_cli_dry_run_skip_permissions);
	g_test_add_func("/ai-glib/ai-cli/dry-run/streaming",
	                test_cli_dry_run_streaming);

	g_test_add_func("/ai-glib/ai-cli/set/properties",
	                test_cli_set_properties);
	g_test_add_func("/ai-glib/ai-cli/set/bare-boolean",
	                test_cli_set_bare_boolean);
	g_test_add_func("/ai-glib/ai-cli/set/boolean-false",
	                test_cli_set_boolean_false);
	g_test_add_func("/ai-glib/ai-cli/set/inherited-property",
	                test_cli_set_inherited_property);
	g_test_add_func("/ai-glib/ai-cli/set/unknown-property",
	                test_cli_set_unknown_property);
	g_test_add_func("/ai-glib/ai-cli/set/read-only-property",
	                test_cli_set_read_only_property);
	g_test_add_func("/ai-glib/ai-cli/set/bad-value",
	                test_cli_set_bad_value);
	g_test_add_func("/ai-glib/ai-cli/set/overrides-flag",
	                test_cli_set_overrides_flag);

	g_test_add_func("/ai-glib/ai-cli/dry-run/opencode",
	                test_cli_dry_run_opencode);
	g_test_add_func("/ai-glib/ai-cli/dry-run/claude-code",
	                test_cli_dry_run_claude_code);
	g_test_add_func("/ai-glib/ai-cli/skip-permissions/opencode",
	                test_cli_skip_permissions_opencode);
	g_test_add_func("/ai-glib/ai-cli/set/opencode-properties",
	                test_cli_set_opencode_properties);
	g_test_add_func("/ai-glib/ai-cli/set/claude-code-properties",
	                test_cli_set_claude_code_properties);
	g_test_add_func("/ai-glib/ai-cli/set/unknown-lists-provider",
	                test_cli_set_unknown_property_lists_provider);

	g_test_add_func("/ai-glib/ai-cli/run/prints-answer",
	                test_cli_run_prints_answer);
	g_test_add_func("/ai-glib/ai-cli/run/reports-error",
	                test_cli_run_reports_error);
	g_test_add_func("/ai-glib/ai-cli/run/streaming",
	                test_cli_run_streaming);
	g_test_add_func("/ai-glib/ai-cli/run/prompt-from-stdin",
	                test_cli_run_prompt_from_stdin);
	g_test_add_func("/ai-glib/ai-cli/run/missing-binary",
	                test_cli_run_missing_binary);

	status = g_test_run();

	g_free(ai_bin);
	return status;
}
