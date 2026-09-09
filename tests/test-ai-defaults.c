/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Spawned frontend defaults/setup tests. Only disposable files and recording
 * CLI stubs are used; no inherited credentials, CLI installations or servers.
 */
#include <ai-glib.h>
#include <glib/gstdio.h>
#include <yaml.h>
#include <string.h>
#include <utime.h>
#include "../bin/ai-tui-history.h"

#define CONFIG_FILE "config/ai-glib/config.yaml"
#define SAVED_CONFIG \
	"default_provider: opencode\ndefault_model: library-only\n" \
	"timeout: 77\nmax_retries: 4\nextra: {items: [one, two]}\n" \
	"providers: {openai: {api_key: fake-preserved-key, base_url: http://127.0.0.1:0}}\n" \
	"apps:\n" \
	"  ai: {default_provider: grok-build, default_model: ai-saved, extra: retained}\n" \
	"  ai-tui: {default_provider: cursor, default_model: tui-saved}\n" \
	"  other: {custom: kept}\n"

typedef struct {
	gchar *dir;
	gchar *out;
	gchar *err;
} Box;

static gchar *ai_binary;
static gchar *tui_binary;
static gchar *library_dir;

/* All writes, including stub logs, remain beneath the fixture directory. */
static void
box_write(Box *box, const gchar *name, const gchar *text)
{
	g_autofree gchar *path = g_build_filename(box->dir, name, NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, text, -1, &error));
	g_assert_no_error(error);
}

/* Read exact bytes for cancellation checks, not a reserialized approximation. */
static gchar *
box_read(Box *box, const gchar *name)
{
	g_autofree gchar *path = g_build_filename(box->dir, name, NULL);
	g_autoptr(GError) error = NULL;
	gchar *text = NULL;

	g_assert_true(g_file_get_contents(path, &text, NULL, &error));
	g_assert_no_error(error);
	return text;
}

/* Remove only this fixture tree; never follow a symlink out of it. */
static void
remove_tree(const gchar *path)
{
	if (g_file_test(path, G_FILE_TEST_IS_DIR) &&
	    !g_file_test(path, G_FILE_TEST_IS_SYMLINK))
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
		g_assert_cmpint(g_unlink(path), ==, 0);
}

/* Separate stub filenames prove which provider actually executed. Both JSON
 * and streaming result fields are supplied, so the same stub serves ai/TUI. */
static void
box_setup(Box *box, gconstpointer data)
{
	const gchar *names[] = { "grok", "cursor" };
	g_autoptr(GError) error = NULL;
	guint i;

	(void)data;
	box->dir = g_dir_make_tmp("ai-defaults-XXXXXX", &error);
	g_assert_no_error(error);
	box_write(box, "codex",
		"#!/bin/bash\nset -eu\n"
		"printf '%s\\n' \"$@\" > \"$HOME/codex.argv\"\n"
		"pwd > \"$HOME/codex.cwd\"\n"
		"/usr/bin/cat > \"$HOME/codex.stdin\"\n"
		"printf '%s\\n' '{\"type\":\"thread.started\",\"thread_id\":\"new-thread\"}'\n"
		"printf '%s\\n' '{\"type\":\"turn.started\"}'\n"
		"printf '%s\\n' '{\"type\":\"item.completed\",\"item\":{\"id\":\"m\","
		"\"type\":\"agent_message\",\"text\":\"codex reply\"}}'\n"
		"printf '%s\\n' '{\"type\":\"turn.completed\",\"usage\":"
		"{\"input_tokens\":1,\"output_tokens\":1}}'\n");
	{
		g_autofree gchar *codex = g_build_filename(box->dir, "codex", NULL);
		g_assert_cmpint(g_chmod(codex, 0700), ==, 0);
	}
	box_write(box, "claude",
		"#!/bin/bash\nset -eu\n"
		"printf '%s\\n' \"$@\" > \"$HOME/claude.argv\"\n"
		"pwd > \"$HOME/claude.cwd\"\n"
		"/usr/bin/cat > \"$HOME/claude.stdin\"\n"
		"printf '%s\\n' '{\"type\":\"result\",\"result\":\"claude reply\","
		"\"session_id\":\"new\",\"is_error\":false}'\n");
	box_write(box, "opencode-bin",
		"#!/bin/bash\nset -eu\n"
		"printf '%s\\n' \"$@\" > \"$HOME/opencode.argv\"\n"
		"pwd > \"$HOME/opencode.cwd\"\n"
		"/usr/bin/cat > \"$HOME/opencode.stdin\"\n"
		"printf '%s\\n' '{\"type\":\"step_start\",\"sessionID\":\"new\"}'\n"
		"printf '%s\\n' '{\"type\":\"text\",\"part\":{\"text\":\"opencode reply\"}}'\n"
		"printf '%s\\n' '{\"type\":\"step_finish\",\"part\":{\"tokens\":{\"input\":1,\"output\":1}}}'\n");
	box_write(box, "agy",
		"#!/bin/bash\nset -eu\n"
		"printf '%s\\n' \"$@\" > \"$HOME/agy.argv\"\n"
		"pwd > \"$HOME/agy.cwd\"\n"
		"/usr/bin/cat > \"$HOME/agy.stdin\"\n"
		"printf '%s\\n' '{\"conversation_id\":\"new\",\"status\":\"SUCCESS\","
		"\"response\":\"agy reply\"}'\n");
	{
		g_autofree gchar *claude = g_build_filename(box->dir, "claude", NULL);
		g_autofree gchar *opencode = g_build_filename(box->dir, "opencode-bin", NULL);
		g_autofree gchar *agy = g_build_filename(box->dir, "agy", NULL);
		g_assert_cmpint(g_chmod(claude, 0700), ==, 0);
		g_assert_cmpint(g_chmod(opencode, 0700), ==, 0);
		g_assert_cmpint(g_chmod(agy, 0700), ==, 0);
	}
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_autofree gchar *path = g_build_filename(box->dir, names[i], NULL);
		g_autofree gchar *script = g_strdup_printf(
			"#!/bin/bash\nset -eu\n"
			"printf '%%s\\n' \"$@\" > \"$HOME/%s.argv\"\n"
			"pwd > \"$HOME/%s.cwd\"\n"
			"/usr/bin/cat > \"$HOME/%s.stdin\"\n"
			"for arg in \"$@\"; do\n"
			"  case \"$arg\" in\n"
			"    stream-json) printf '%%s\\n' '{\"type\":\"assistant\","
			"\"timestamp_ms\":1,\"message\":{\"content\":[{\"type\":\"text\","
			"\"text\":\"cursor reply\"}]}}' ;;\n"
			"    streaming-messages-json) printf '%%s\\n' '{\"type\":\"stream_event\","
			"\"event\":{\"type\":\"content_block_delta\",\"delta\":{"
			"\"type\":\"text_delta\",\"text\":\"grok reply\"}}}' ;;\n"
			"  esac\ndone\n"
			"printf '%%s\\n' '{\"type\":\"result\",\"text\":\"%s reply\","
			"\"result\":\"%s reply\",\"stopReason\":\"end_turn\","
			"\"is_error\":false}'\n",
			names[i], names[i], names[i], names[i], names[i]);
		box_write(box, names[i], script);
		g_assert_cmpint(g_chmod(path, 0700), ==, 0);
	}
}

/* Fixture teardown also covers directories the frontend creates itself. */
static void
box_teardown(Box *box, gconstpointer data)
{
	(void)data;
	remove_tree(box->dir);
	g_free(box->dir);
	g_free(box->out);
	g_free(box->err);
}

/* Start with an empty environment rather than guessing every credential and
 * config override name. PATH contains only our stubs; absolute utility paths
 * in the scripts prevent accidentally executing a real provider. */
static gint
run_box(Box *box, gboolean tui, const gchar * const *args,
        const gchar *input, const gchar * const *overrides)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) proc = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *config = g_build_filename(box->dir, "config", NULL);
	g_autofree gchar *grok = g_build_filename(box->dir, "grok", NULL);
	g_autofree gchar *cursor = g_build_filename(box->dir, "cursor", NULL);
	g_autofree gchar *codex = g_build_filename(box->dir, "codex", NULL);
	g_autofree gchar *claude = g_build_filename(box->dir, "claude", NULL);
	g_autofree gchar *opencode = g_build_filename(box->dir, "opencode-bin", NULL);
	g_autofree gchar *agy = g_build_filename(box->dir, "agy", NULL);
	gchar *empty[] = { NULL };
	guint i;
	gint status;

	g_clear_pointer(&box->out, g_free);
	g_clear_pointer(&box->err, g_free);
	g_subprocess_launcher_set_environ(launcher, empty);
	g_subprocess_launcher_set_cwd(launcher, box->dir);
	g_subprocess_launcher_setenv(launcher, "HOME", box->dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", config, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_DATA_HOME", box->dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "XDG_CACHE_HOME", box->dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "PATH", box->dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "LD_LIBRARY_PATH", library_dir, TRUE);
	g_subprocess_launcher_setenv(launcher, "GROK_PATH", grok, TRUE);
	g_subprocess_launcher_setenv(launcher, "CURSOR_AGENT_PATH", cursor, TRUE);
	g_subprocess_launcher_setenv(launcher, "CODEX_PATH", codex, TRUE);
	g_subprocess_launcher_setenv(launcher, "CLAUDE_CODE_PATH", claude, TRUE);
	g_subprocess_launcher_setenv(launcher, "OPENCODE_PATH", opencode, TRUE);
	g_subprocess_launcher_setenv(launcher, "AGY_PATH", agy, TRUE);
	g_subprocess_launcher_setenv(launcher, "G_DEBUG", "fatal-warnings", TRUE);
	for (i = 0; overrides != NULL && overrides[i] != NULL; i += 2)
		g_subprocess_launcher_setenv(launcher, overrides[i], overrides[i + 1], TRUE);
	g_ptr_array_add(argv, tui ? tui_binary : ai_binary);
	for (i = 0; args[i] != NULL; i++)
		g_ptr_array_add(argv, (gpointer)args[i]);
	g_ptr_array_add(argv, NULL);
	proc = g_subprocess_launcher_spawnv(launcher,
		(const gchar * const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_assert_true(g_subprocess_communicate_utf8(proc, input != NULL ? input : "",
		NULL, &box->out, &box->err, &error));
	g_assert_no_error(error);
	if (!g_subprocess_get_if_exited(proc))
		g_error("frontend terminated by signal: %s", box->err);
	status = g_subprocess_get_exit_status(proc);
	if (status != 0)
		g_test_message("exit %d: %s", status, box->err);
	return status;
}

/* Derive provider menu numbers from the public enum, not fragile literals. */
static guint
provider_choice(AiProviderType provider)
{
	g_autoptr(GEnumClass) klass = g_type_class_ref(AI_TYPE_PROVIDER_TYPE);
	guint i;

	for (i = 0; i < klass->n_values; i++)
		if (klass->values[i].value == (gint)provider)
			return i + 1;
	g_assert_not_reached();
}

/* Drive only the public stdin protocol; no direct save API calls in tests. */
static gint
setup_scope(Box *box, guint scope, AiProviderType provider, const gchar *tail)
{
	const gchar *args[] = { "--setup", NULL };
	g_autofree gchar *input = g_strdup_printf("%u\n%u\n%s", scope,
		provider_choice(provider), tail);

	return run_box(box, FALSE, args, input, NULL);
}

/* Inspect arbitrary saved YAML scalars, including explicit empty model IDs.
 * libyaml preserves empty strings and does not consult process config/env. */
static void
assert_saved(Box *box, const gchar *path, const gchar *expected)
{
	g_autofree gchar *text = box_read(box, CONFIG_FILE);
	g_auto(GStrv) parts = g_strsplit(path, "/", -1);
	yaml_parser_t parser;
	yaml_document_t document;
	yaml_node_t *node;
	guint i;

	g_assert_true(yaml_parser_initialize(&parser));
	yaml_parser_set_input_string(&parser, (const unsigned char *)text, strlen(text));
	g_assert_true(yaml_parser_load(&parser, &document));
	node = yaml_document_get_root_node(&document);
	for (i = 0; parts[i] != NULL && node != NULL; i++)
	{
		yaml_node_pair_t *pair;
		yaml_node_t *next = NULL;

		if (node->type == YAML_SEQUENCE_NODE)
		{
			guint64 index;
			guint count = (guint)(node->data.sequence.items.top - node->data.sequence.items.start);

			g_assert_cmpuint(count, >, 0);
			g_assert_true(g_ascii_string_to_unsigned(parts[i], 10, 0, count - 1, &index, NULL));
			node = yaml_document_get_node(&document, node->data.sequence.items.start[index]);
			continue;
		}
		g_assert_cmpint(node->type, ==, YAML_MAPPING_NODE);
		for (pair = node->data.mapping.pairs.start; pair < node->data.mapping.pairs.top; pair++)
		{
			yaml_node_t *key = yaml_document_get_node(&document, pair->key);
			if (key->type == YAML_SCALAR_NODE &&
			    g_str_equal((const gchar *)key->data.scalar.value, parts[i]))
				next = yaml_document_get_node(&document, pair->value);
		}
		node = next;
	}
	if (expected == NULL)
		g_assert_null(node);
	else
	{
		g_assert_nonnull(node);
		g_assert_cmpint(node->type, ==, YAML_SCALAR_NODE);
		g_assert_cmpstr((const gchar *)node->data.scalar.value, ==, expected);
	}
	yaml_document_delete(&document);
	yaml_parser_delete(&parser);
}

/* Creation must not synthesize library or sibling defaults. */
static void
test_create(Box *box, gconstpointer data)
{
	GStatBuf st;
	g_autofree gchar *path = g_build_filename(box->dir, CONFIG_FILE, NULL);

	(void)data;
	g_assert_cmpint(setup_scope(box, 1, AI_PROVIDER_GROK_BUILD, "2\nmanual-model\ny\n"), ==, 0);
	g_assert_nonnull(strstr(box->out, "Defaults saved"));
	assert_saved(box, "apps/ai/default_provider", "grok-build");
	assert_saved(box, "apps/ai/default_model", "manual-model");
	assert_saved(box, "default_provider", NULL);
	assert_saved(box, "default_model", NULL);
	assert_saved(box, "apps/ai-tui", NULL);
	g_assert_cmpint(g_stat(path, &st), ==, 0);
	g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
}

/* Updating each scope leaves the other two and unknown data intact. Discovery
 * temporarily clamps timeout/retries, which must not leak into saved config. */
static void
test_update(Box *box, gconstpointer data)
{
	(void)data;
	box_write(box, CONFIG_FILE, SAVED_CONFIG);
	g_assert_cmpint(setup_scope(box, 1, AI_PROVIDER_GROK_BUILD, "4\ny\n"), ==, 0);
	g_assert_nonnull(strstr(box->out, "4. " AI_GROK_BUILD_MODEL_GROK_4_5));
	assert_saved(box, "apps/ai/default_model", AI_GROK_BUILD_MODEL_GROK_4_5);
	assert_saved(box, "apps/ai-tui/default_model", "tui-saved");
	assert_saved(box, "default_model", "library-only");
	g_assert_cmpint(setup_scope(box, 2, AI_PROVIDER_CURSOR, "1\nyes\n"), ==, 0);
	assert_saved(box, "apps/ai-tui/default_model", "");
	assert_saved(box, "apps/ai/default_model", AI_GROK_BUILD_MODEL_GROK_4_5);
	g_assert_cmpint(setup_scope(box, 3, AI_PROVIDER_GROK_BUILD, "2\nlibrary-new\ny\n"), ==, 0);
	assert_saved(box, "default_provider", "grok-build");
	assert_saved(box, "default_model", "library-new");
	assert_saved(box, "apps/ai/default_provider", "grok-build");
	assert_saved(box, "apps/ai/default_model", AI_GROK_BUILD_MODEL_GROK_4_5);
	assert_saved(box, "apps/ai-tui/default_provider", "cursor");
	assert_saved(box, "apps/ai-tui/default_model", "");
	assert_saved(box, "apps/ai/extra", "retained");
	assert_saved(box, "apps/other/custom", "kept");
	assert_saved(box, "timeout", "77");
	assert_saved(box, "max_retries", "4");
	assert_saved(box, "extra/items/0", "one");
	assert_saved(box, "extra/items/1", "two");
	assert_saved(box, "providers/openai/api_key", "fake-preserved-key");
	assert_saved(box, "providers/openai/base_url", "http://127.0.0.1:0");
	g_assert_cmpint(setup_scope(box, 3, AI_PROVIDER_GROK_BUILD, "1\ny\n"), ==, 0);
	assert_saved(box, "default_model", "");
}

/* EOF at every stage and negative confirmation are byte-preserving, both
 * with a preexisting config and with no config directory at all. */
static void
test_cancel(Box *box, gconstpointer data)
{
	const gchar *args[] = { "--setup", NULL };
	const gchar *tails[] = { "", "q\n", "cancel\n", "2\n", "2\nq\n",
		"1\n", "1\nn\n", "1\n\n", "2\nmanual\n", "2\nmanual\ny",
		"1\nmaybe\ncancel\n", "0\n-1\n99999\nnope\nq\n" };
	guint i, existing;
	g_autofree gchar *path = g_build_filename(box->dir, CONFIG_FILE, NULL);

	(void)data;
	for (existing = 0; existing < 2; existing++)
	{
		if (existing)
			box_write(box, CONFIG_FILE, SAVED_CONFIG);
		for (i = 0; i < G_N_ELEMENTS(tails) + 3; i++)
		{
			g_autofree gchar *input = i < 3 ? g_strdup(i == 0 ? "" : i == 1 ? "q\n" : "1\n") :
				g_strdup_printf("1\n%u\n%s", provider_choice(AI_PROVIDER_GROK_BUILD), tails[i - 3]);
			g_assert_cmpint(run_box(box, FALSE, args, input, NULL), ==, 0);
			g_assert_nonnull(strstr(box->out, "Setup cancelled"));
			if (existing)
			{
				g_autofree gchar *after = box_read(box, CONFIG_FILE);
				g_assert_cmpstr(after, ==, SAVED_CONFIG);
			}
			else
				g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
		}
	}
}

/* Invalid input is retried, not silently coerced to a menu index or model. */
static void
test_invalid_retry(Box *box, gconstpointer data)
{
	const gchar *args[] = { "--setup", NULL };
	g_autofree gchar *input = g_strdup_printf(
		"bad\n0\n4\n1\n-1\n99999\n%u\n0\n99999\n2\n\ndefault\nmanual\nmaybe\ny\n",
		provider_choice(AI_PROVIDER_GROK_BUILD));

	(void)data;
	g_assert_cmpint(run_box(box, FALSE, args, input, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "Choose a number"));
	g_assert_nonnull(strstr(box->out, "default is reserved"));
	g_assert_nonnull(strstr(box->out, "Enter y to save"));
	assert_saved(box, "apps/ai/default_model", "manual");
}

/* A confirmed wizard cannot repair malformed/ambiguous YAML by overwriting it. */
static void
test_malformed(Box *box, gconstpointer data)
{
	const gchar *bad[] = { "apps: [unclosed\n", "apps: []\n",
		"apps: {ai: {default_provider: typo}}\n",
		"apps: {ai: {default_model: [wrong]}}\n",
		"timeout: 1\ntimeout: 2\n", "---\n{}\n---\n{}\n" };
	guint i;

	(void)data;
	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{
		g_autofree gchar *after = NULL;
		box_write(box, CONFIG_FILE, bad[i]);
		g_assert_cmpint(setup_scope(box, 1, AI_PROVIDER_GROK_BUILD, "1\ny\n"), !=, 0);
		g_assert_nonnull(strstr(box->err, "could not save defaults"));
		after = box_read(box, CONFIG_FILE);
		g_assert_cmpstr(after, ==, bad[i]);
	}
}

/* Assert on the child's recorded argv/stdin/cwd, not merely dry-run text. */
static void
assert_child(Box *box, const gchar *provider, const gchar *model, const gchar *prompt)
{
	g_autofree gchar *name = g_strconcat(provider, ".argv", NULL);
	g_autofree gchar *args = box_read(box, name);
	g_autofree gchar *needle = g_strdup_printf("--model\n%s\n", model);
	g_autofree gchar *stdin_name = g_strconcat(provider, ".stdin", NULL);
	g_autofree gchar *input = box_read(box, stdin_name);
	g_autofree gchar *cwd_name = g_strconcat(provider, ".cwd", NULL);
	g_autofree gchar *cwd = box_read(box, cwd_name);

	g_assert_nonnull(strstr(args, needle));
	g_assert_null(strstr(args, prompt));
	g_assert_nonnull(strstr(input, prompt));
	g_assert_cmpstr(g_strchomp(cwd), ==, box->dir);
	g_assert_null(strstr(args, "library-only"));
	g_assert_null(strstr(args, "env-library"));
}

/* Defaults, concrete overrides, ignored legacy env and --set reach a real
 * spawned provider. Cross-provider requests must start with its native model. */
static void
test_ai_resolution(Box *box, gconstpointer data)
{
	const gchar *env[] = { "AI_PROVIDER", "cursor", "AI_GLIB_DEFAULT_PROVIDER", "opencode",
		"AI_GLIB_DEFAULT_MODEL", "env-library", NULL };
	const gchar *library_env[] = { "AI_GLIB_DEFAULT_PROVIDER", "opencode",
		"AI_GLIB_DEFAULT_MODEL", "env-library", NULL };
	struct {
		const gchar *args[12];
		const gchar *provider;
		const gchar *model;
		const gchar * const *env;
	} cases[] = {
		{ { NULL }, "grok", "ai-saved", NULL },
		{ { NULL }, "grok", "ai-saved", library_env },
		{ { "-p", "default", "-m", "default", "--skip-permissions", NULL }, "grok", "ai-saved", env },
		{ { "-p", "grok-build", NULL }, "grok", "ai-saved", env },
		{ { "-m", "concrete", NULL }, "grok", "concrete", NULL },
		{ { "-p", "cursor", NULL }, "cursor", AI_CURSOR_MODEL_AUTO, NULL },
		{ { "-p", "cursor", "-m", "default", NULL }, "cursor", AI_CURSOR_MODEL_AUTO, NULL },
		{ { NULL }, "grok", "ai-saved", env },
		{ { "-p", "cursor", "-m", "concrete", NULL }, "cursor", "concrete", NULL },
		{ { "--set", "model=last", "-m", "concrete", NULL }, "grok", "last", NULL },
		{ { "-m", "default", NULL }, "grok", "ai-saved", env },
		{ { "-p", "default", NULL }, "grok", "ai-saved", env },
		{ { "--model", "default", "--provider", "default", NULL }, "grok", "ai-saved", env },
		{ { "--model", "concrete", NULL }, "grok", "concrete", env },
		{ { "--provider", "cursor", NULL }, "cursor", AI_CURSOR_MODEL_AUTO, env }
	};
	guint i;

	(void)data;
	box_write(box, CONFIG_FILE, SAVED_CONFIG);
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *reply = g_strconcat(cases[i].provider, " reply", NULL);
		g_test_message("ai resolution case %u", i);
		g_assert_cmpint(run_box(box, FALSE, cases[i].args, "piped defaults prompt\n", cases[i].env), ==, 0);
		g_assert_nonnull(strstr(box->out, reply));
		assert_child(box, cases[i].provider, cases[i].model, "piped defaults prompt");
		if (i == 2)
		{
			g_autofree gchar *args = box_read(box, "grok.argv");
			g_assert_nonnull(strstr(args, "--permission-mode\nbypassPermissions\n"));
			g_assert_nonnull(strstr(args, "--prompt-file\n/dev/stdin\n"));
		}
	}
	{
		g_autofree gchar *after = box_read(box, CONFIG_FILE);
		g_assert_cmpstr(after, ==, SAVED_CONFIG);
	}
}

/* TUI startup has its own scope; --dump drives the streaming parser without a
 * terminal. Missing optional ncurses skips only this test, not wizard/ai tests. */
static void
test_tui_resolution(Box *box, gconstpointer data)
{
	const gchar *env[] = { "AI_PROVIDER", "grok-build", "AI_GLIB_DEFAULT_PROVIDER", "opencode",
		"AI_GLIB_DEFAULT_MODEL", "env-library", NULL };
	const gchar *library_env[] = { "AI_GLIB_DEFAULT_PROVIDER", "opencode",
		"AI_GLIB_DEFAULT_MODEL", "env-library", NULL };
	struct {
		const gchar *args[12];
		const gchar *provider;
		const gchar *model;
		const gchar * const *env;
	} cases[] = {
		{ { "--dump", "tui prompt", NULL }, "cursor", "tui-saved", NULL },
		{ { "-p", "default", "-m", "default", "--dump", "tui prompt", NULL }, "cursor", "tui-saved", env },
		{ { "-p", "grok-build", "-m", "default", "--dump", "tui prompt", NULL }, "grok", AI_GROK_BUILD_DEFAULT_MODEL, NULL },
		{ { "--dump", "tui prompt", NULL }, "cursor", "tui-saved", env },
		{ { "-m", "concrete", "--dump", "tui prompt", NULL }, "cursor", "concrete", NULL },
		{ { "--set", "model=last", "-m", "concrete", "--dump", "tui prompt", NULL }, "cursor", "last", NULL },
		{ { "--dump", "tui prompt", NULL }, "cursor", "tui-saved", library_env },
		{ { "-m", "default", "--dump", "tui prompt", NULL }, "cursor", "tui-saved", env },
		{ { "-p", "default", "--dump", "tui prompt", NULL }, "cursor", "tui-saved", env },
		{ { "--model", "default", "--provider", "default", "--dump", "tui prompt", NULL }, "cursor", "tui-saved", env },
		{ { "--model", "concrete", "--dump", "tui prompt", NULL }, "cursor", "concrete", env },
		{ { "--provider", "grok-build", "--dump", "tui prompt", NULL }, "grok", AI_GROK_BUILD_DEFAULT_MODEL, env }
	};
	guint i;

	(void)data;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai-tui not built (ncurses-devel required)");
		return;
	}
	box_write(box, CONFIG_FILE, SAVED_CONFIG);
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *reply = g_strconcat(cases[i].provider, " reply", NULL);
		g_test_message("tui resolution case %u", i);
		g_assert_cmpint(run_box(box, TRUE, cases[i].args, NULL, cases[i].env), ==, 0);
		g_assert_nonnull(strstr(box->out, reply));
		assert_child(box, cases[i].provider, cases[i].model, "tui prompt");
	}
	/* Clear a previously saved model through the wizard, then start afresh. */
	g_assert_cmpint(setup_scope(box, 2, AI_PROVIDER_CURSOR, "1\ny\n"), ==, 0);
	g_assert_cmpint(run_box(box, TRUE, cases[0].args, NULL, NULL), ==, 0);
	assert_child(box, "cursor", AI_CURSOR_MODEL_AUTO, "tui prompt");
	assert_saved(box, "default_model", "library-only");
	assert_saved(box, "apps/ai/default_model", "ai-saved");
}

/* A library-only wizard save must not become either frontend's fallback.
 * Dry-run observes the built-in HTTP choice without sending a request. */
static void
test_library_independent(Box *box, gconstpointer data)
{
	const gchar *args[] = { "--dry-run", "hello", NULL };
	const gchar *tui_args[] = { "--dry-run", NULL };
	const gchar *env[] = { "AI_GLIB_DEFAULT_PROVIDER", "grok-build",
		"AI_GLIB_DEFAULT_MODEL", "env-library", "AI_PROVIDER", "not-a-provider", NULL };

	(void)data;
	g_assert_cmpint(setup_scope(box, 3, AI_PROVIDER_CURSOR, "2\nlibrary-only\ny\n"), ==, 0);
	assert_saved(box, "default_provider", "cursor");
	assert_saved(box, "default_model", "library-only");
	assert_saved(box, "apps", NULL);
	g_assert_cmpint(run_box(box, FALSE, args, NULL, env), ==, 0);
	g_assert_nonnull(strstr(box->out, "claude"));
	g_assert_null(strstr(box->out, "library-only"));
	g_assert_null(strstr(box->out, "env-library"));
	if (g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_assert_cmpint(run_box(box, TRUE, tui_args, NULL, env), ==, 0);
		g_assert_nonnull(strstr(box->out, "HTTP provider"));
		g_assert_null(strstr(box->out, "library-only"));
		g_assert_null(strstr(box->out, "env-library"));
	}
}

/* Bind, but do not listen, on a private loopback port. Discovery must fail
 * locally; reserving the port avoids ever connecting to somebody's server. */
static void
test_http_fallback(Box *box, gconstpointer data)
{
	g_autoptr(GSocket) socket = NULL;
	g_autoptr(GInetAddress) loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
	g_autoptr(GSocketAddress) address = g_inet_socket_address_new(loopback, 0);
	g_autoptr(GSocketAddress) bound = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *config = NULL;

	(void)data;
	socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error);
	g_assert_no_error(error);
	g_assert_true(g_socket_bind(socket, address, FALSE, &error));
	g_assert_no_error(error);
	bound = g_socket_get_local_address(socket, &error);
	g_assert_no_error(error);
	config = g_strdup_printf("timeout: 1\nproviders:\n  openai:\n"
		"    api_key: offline-test-only\n    base_url: http://127.0.0.1:%u\n",
		g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound)));
	box_write(box, CONFIG_FILE, config);
	g_assert_cmpint(setup_scope(box, 1, AI_PROVIDER_OPENAI, "2\noffline-manual\ny\n"), ==, 0);
	g_assert_nonnull(strstr(box->out, "Discovery unavailable"));
	assert_saved(box, "apps/ai/default_provider", "openai");
	assert_saved(box, "apps/ai/default_model", "offline-manual");
	g_assert_cmpint(setup_scope(box, 1, AI_PROVIDER_OPENAI, "1\ny\n"), ==, 0);
	g_assert_nonnull(strstr(box->out, "Discovery unavailable"));
	assert_saved(box, "apps/ai/default_model", "");
}

/* Antigravity exposes the effective inherited deadline in its argv, letting
 * both real frontends prove their default without waiting thirty minutes. */
static void
test_process_timeout_default(Box *box, gconstpointer data)
{
	guint tui;
	guint i;
	const gchar *values[] = { NULL, "process-timeout-ms=60000", "process-timeout-ms=0" };
	const gchar *expected[] = { "--print-timeout 168h", "--print-timeout 1m", "--print-timeout 168h" };

	(void)data;
	for (tui = 0; tui < 2; tui++)
	{
		if (tui && !g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
			continue;
		for (i = 0; i < G_N_ELEMENTS(values); i++)
		{
			const gchar *args[] = { "-p", "antigravity", "--dry-run", "prompt",
				values[i] != NULL ? "--set" : NULL, values[i], NULL };

			g_assert_cmpint(run_box(box, tui, args, NULL, NULL), ==, 0);
			g_assert_nonnull(strstr(box->out, expected[i]));
		}
	}
}

/* A finite override must still terminate a real streaming subprocess; zero
 * and the frontend default must let the same delayed response complete. */
static void
test_process_timeout_stream(Box *box, gconstpointer data)
{
	g_autofree gchar *script = box_read(box, "grok");
	g_autofree gchar *delayed = g_strconcat("#!/bin/bash\n/usr/bin/sleep 1\n", script, NULL);
	const gchar *values[] = { NULL, "process-timeout-ms=100", "process-timeout-ms=0" };
	guint tui;
	guint i;

	(void)data;
	box_write(box, "grok", delayed);
	for (tui = 0; tui < 2; tui++)
	{
		if (tui && !g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
			continue;
		for (i = 0; i < G_N_ELEMENTS(values); i++)
		{
			const gchar *args[] = { "-p", "grok-build", tui ? "--dump" : "--stream", "prompt",
				values[i] != NULL ? "--set" : NULL, values[i], NULL };
			gint status = run_box(box, tui, args, NULL, NULL);

			if (i == 1)
			{
				/* ai's streaming path currently reports errors on stderr
				 * without propagating them to its exit status. */
				g_assert_true(strstr(box->out, "100 ms deadline") != NULL ||
					strstr(box->err, "100 ms deadline") != NULL);
				g_assert_null(strstr(box->out, "grok reply"));
			}
			else
			{
				g_assert_cmpint(status, ==, 0);
				g_assert_nonnull(strstr(box->out, "grok reply"));
			}
		}
	}
}

/* Native session fixtures are isolated from the developer's real Grok home. */
static void
write_history(Box *box, const gchar *id, const gchar *date, const gchar *log)
{
	g_autofree gchar *encoded = g_uri_escape_string(box->dir, NULL, FALSE);
	g_autofree gchar *directory = g_build_filename(".grok", "sessions", encoded, id, NULL);
	g_autofree gchar *summary_path = g_build_filename(directory, "summary.json", NULL);
	g_autofree gchar *log_path = g_build_filename(directory, "updates.jsonl", NULL);
	g_autofree gchar *summary = g_strdup_printf(
		"{\"info\":{\"id\":\"%s\",\"cwd\":\"%s\"},\"updated_at\":\"%s\"}", id, box->dir, date);

	box_write(box, summary_path, summary);
	box_write(box, log_path, log);
}

#define HISTORY_EVENT(id, update) "{\"params\":{\"sessionId\":\"" id "\",\"update\":" update "}}\n"
#define HISTORY_TEXT(id, kind, text) HISTORY_EVENT(id, "{\"sessionUpdate\":\"" kind "\",\"content\":{\"type\":\"text\",\"text\":\"" text "\"}}")

/* Verify selection and no replay through the real binary, then inspect startup
 * directly to prove restoration needs neither a submitted prompt nor a CLI. */
static void
test_native_history(Box *box, gconstpointer data)
{
	const gchar *args[] = { "-p", "grok-build", "-c", "--dump", "new prompt", NULL };
	const gchar *fresh[] = { "-p", "grok-build", "--dump", "fresh prompt", NULL };
	const gchar *explicit_id[] = { "-p", "grok-build", "-c", "--set", "session-id=older", "--dump", "new prompt", NULL };
	const gchar *disabled[] = { "-p", "grok-build", "-c", "--set", "session-persistence=false", "--dump", "new prompt", NULL };
	g_autoptr(AiGrokBuildClient) provider = ai_grok_build_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(GPtrArray) history = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *sent = NULL;
	g_autofree gchar *argv = NULL;
	AiTranscript *transcript = ai_conversation_get_transcript(conversation);

	(void)data;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai-tui unavailable");
		return;
	}
	write_history(box, "older", "2026-09-06T12:00:00Z",
		HISTORY_TEXT("older", "user_message_chunk", "older question"));
	write_history(box, "latest", "2026-09-07T12:00:00Z",
		HISTORY_TEXT("latest", "user_message_chunk", "previous question")
		HISTORY_TEXT("latest", "agent_thought_chunk", "previous reasoning")
		HISTORY_TEXT("latest", "agent_message_chunk", "previous ")
		HISTORY_TEXT("latest", "agent_message_chunk", "answer")
		HISTORY_EVENT("latest", "{\"sessionUpdate\":\"tool_call\",\"toolCallId\":\"tool-1\",\"title\":\"Read `/tmp/file.c`\",\"kind\":\"read\",\"rawInput\":{\"variant\":\"ReadFile\",\"target_file\":\"file.c\"}}")
		HISTORY_EVENT("latest", "{\"sessionUpdate\":\"tool_call_update\",\"toolCallId\":\"tool-1\",\"status\":\"completed\",\"content\":[{\"content\":{\"text\":\"saved output\"}}]}")
		HISTORY_TEXT("different-session", "agent_message_chunk", "must not appear"));
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "previous question"));
	g_assert_nonnull(strstr(box->out, "previous answer"));
	g_assert_null(strstr(box->out, "older question"));
	g_assert_null(strstr(box->out, "must not appear"));
	sent = box_read(box, "grok.stdin");
	argv = box_read(box, "grok.argv");
	g_assert_null(strstr(sent, "previous question"));
	g_assert_nonnull(strstr(sent, "new prompt"));
	g_assert_nonnull(strstr(argv, "--resume\nlatest\n"));
	g_assert_cmpint(run_box(box, TRUE, explicit_id, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "older question"));
	g_assert_null(strstr(box->out, "previous question"));
	g_assert_cmpint(run_box(box, TRUE, fresh, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));
	g_assert_cmpint(run_box(box, TRUE, disabled, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));

	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "HOME", box->dir);
	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "GROK_HOME", "");
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), box->dir);
	g_object_set(provider, "continue-session", TRUE, NULL);
	ai_tui_history_restore(conversation, history);
	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 4);
	g_assert_cmpuint(history->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(history, 0), ==, "previous question");
	g_assert_null(ai_conversation_get_messages(conversation));
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(provider)), ==, "latest");
	g_assert_cmpstr(ai_tool_call_get_name(ai_view_tool_block_get_call(
		AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(transcript, 3)), 0)), ==, "read_file");
	/* get_summary() is (transfer full); asserting on it inline leaked
	 * the string on every run of the ASAN build. */
	{
		g_autofree gchar *summary = ai_view_tool_block_get_summary(
			AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(transcript, 3)));

		g_assert_cmpstr(summary, ==, "Read file.c");
	}
	g_assert_cmpstr(ai_tool_call_get_result(ai_view_tool_block_get_call(
		AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(transcript, 3)), 0)), ==, "saved output");

	/* A session in another project must never leak into this display. */
	{
		g_autofree gchar *other = g_build_filename(box->dir, "other-project", NULL);
		g_autofree gchar *selected_id = NULL;
		g_autofree gchar *selected_path = NULL;

		g_assert_cmpint(g_mkdir(other, 0700), ==, 0);
		ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), other);
		selected_path = ai_tui_history_find(AI_CLI_CLIENT(provider), &selected_id);
		g_assert_null(selected_path);
		g_assert_null(selected_id);
		ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), box->dir);
	}

	/* Corruption is reported without publishing partial historical blocks. */
	write_history(box, "latest", "2026-09-07T12:00:00Z",
		HISTORY_TEXT("latest", "user_message_chunk", "partial history must not appear") "{broken\n");
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "Could not load native session history"));
	g_assert_null(strstr(box->out, "previous question"));
	g_assert_null(strstr(box->out, "partial history must not appear"));

	/* The alternate home and hashed bucket follow the same native lookup. */
	{
		g_autofree gchar *encoded = g_uri_escape_string(box->dir, NULL, FALSE);
		g_autofree gchar *old_home = g_build_filename(box->dir, ".grok", NULL);
		g_autofree gchar *new_home = g_build_filename(box->dir, "native-home", NULL);
		g_autofree gchar *old_bucket = g_build_filename(new_home, "sessions", encoded, NULL);
		g_autofree gchar *new_bucket = g_build_filename(new_home, "sessions", "project-hash", NULL);
		g_autofree gchar *selected_id = NULL;
		g_autofree gchar *selected_path = NULL;

		g_assert_cmpint(g_rename(old_home, new_home), ==, 0);
		g_assert_cmpint(g_rename(old_bucket, new_bucket), ==, 0);
		box_write(box, "native-home/sessions/project-hash/.cwd", box->dir);
		ai_cli_client_set_env(AI_CLI_CLIENT(provider), "GROK_HOME", new_home);
		selected_path = ai_tui_history_find(AI_CLI_CLIENT(provider), &selected_id);
		g_assert_nonnull(selected_path);
		g_assert_cmpstr(selected_id, ==, "latest");
	}
}

#define CODEX_ITEM(item) "{\"type\":\"event_msg\",\"payload\":{\"type\":\"item_completed\",\"item\":" item "}}\n"
#define CODEX_USER(text) CODEX_ITEM("{\"type\":\"UserMessage\",\"id\":\"u\",\"content\":[{\"type\":\"text\",\"text\":\"" text "\"}]}")
#define CODEX_AGENT(text) CODEX_ITEM("{\"type\":\"AgentMessage\",\"id\":\"a\",\"content\":[{\"type\":\"Text\",\"text\":\"" text "\"}]}")
#define CODEX_THINK(text) CODEX_ITEM("{\"type\":\"Reasoning\",\"id\":\"r\",\"summary_text\":[\"" text "\"]}")

/* Native Codex rollouts live under $CODEX_HOME/sessions/YYYY/MM/DD/. */
static void
write_codex_history(Box *box, const gchar *id, const gchar *stamp, const gchar *cwd, const gchar *log)
{
	g_autofree gchar *rel = g_strdup_printf(".codex/sessions/2026/09/08/rollout-%s-%s.jsonl", stamp, id);
	g_autofree gchar *path = g_build_filename(box->dir, rel, NULL);
	g_autofree gchar *meta = g_strdup_printf(
		"{\"timestamp\":\"%s\",\"type\":\"session_meta\",\"payload\":"
		"{\"session_id\":\"%s\",\"id\":\"%s\",\"cwd\":\"%s\"}}\n", stamp, id, id, cwd);
	g_autofree gchar *contents = g_strconcat(meta, log, NULL);
	g_autoptr(GDateTime) at = g_date_time_new_from_iso8601(stamp, NULL);
	struct utimbuf times;

	box_write(box, rel, contents);
	g_assert_nonnull(at);
	times.actime = times.modtime = (time_t)g_date_time_to_unix(at);
	g_assert_cmpint(g_utime(path, &times), ==, 0);
}

static void
test_codex_native_history(Box *box, gconstpointer data)
{
	const gchar *args[] = { "-p", "codex", "-c", "--dump", "new prompt", NULL };
	const gchar *fresh[] = { "-p", "codex", "--dump", "fresh prompt", NULL };
	const gchar *explicit_id[] = { "-p", "codex", "-c", "--set", "session-id=older", "--dump", "new prompt", NULL };
	const gchar *disabled[] = { "-p", "codex", "-c", "--set", "session-persistence=false", "--dump", "new prompt", NULL };
	g_autoptr(AiCodexCliClient) provider = ai_codex_cli_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(GPtrArray) history = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *sent = NULL;
	g_autofree gchar *argv = NULL;
	AiTranscript *transcript = ai_conversation_get_transcript(conversation);

	(void)data;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai-tui unavailable");
		return;
	}
	write_codex_history(box, "older", "2026-09-06T12:00:00Z", box->dir,
		CODEX_USER("older question"));
	write_codex_history(box, "latest", "2026-09-07T12:00:00Z", box->dir,
		CODEX_USER("previous question")
		CODEX_THINK("previous reasoning")
		CODEX_AGENT("previous answer")
		CODEX_ITEM("{\"type\":\"CommandExecution\",\"id\":\"cmd-1\",\"command\":[\"/bin/bash\",\"-lc\",\"make\"],\"status\":\"completed\",\"aggregated_output\":\"ok\",\"exit_code\":0}")
		CODEX_ITEM("{\"type\":\"FileChange\",\"id\":\"file-1\",\"status\":\"completed\",\"changes\":{\"file.c\":{\"type\":\"update\"}}}"));
	write_codex_history(box, "other", "2026-09-08T12:00:00Z", "/tmp/not-this-project",
		CODEX_USER("must not appear"));
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "previous question"));
	g_assert_nonnull(strstr(box->out, "previous answer"));
	g_assert_null(strstr(box->out, "older question"));
	g_assert_null(strstr(box->out, "must not appear"));
	sent = box_read(box, "codex.stdin");
	argv = box_read(box, "codex.argv");
	g_assert_null(strstr(sent, "previous question"));
	g_assert_nonnull(strstr(sent, "new prompt"));
	g_assert_nonnull(strstr(argv, "resume\nlatest\n"));
	g_assert_null(strstr(argv, "--last"));
	g_assert_cmpint(run_box(box, TRUE, explicit_id, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "older question"));
	g_assert_null(strstr(box->out, "previous question"));
	g_assert_cmpint(run_box(box, TRUE, fresh, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));
	g_assert_cmpint(run_box(box, TRUE, disabled, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));

	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "HOME", box->dir);
	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "CODEX_HOME", "");
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), box->dir);
	g_object_set(provider, "continue-session", TRUE, NULL);
	ai_tui_history_restore(conversation, history);
	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 4);
	g_assert_cmpuint(history->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(history, 0), ==, "previous question");
	g_assert_null(ai_conversation_get_messages(conversation));
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(provider)), ==, "latest");
	g_assert_cmpstr(ai_tool_call_get_name(ai_view_tool_block_get_call(
		AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(transcript, 3)), 0)), ==, "command_execution");
	g_assert_cmpstr(ai_tool_call_get_name(ai_view_tool_block_get_call(
		AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(transcript, 3)), 1)), ==, "file_change");
	{
		g_autofree gchar *summary = ai_view_tool_block_get_summary(
			AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(transcript, 3)));

		g_assert_cmpstr(summary, ==, "Ran make, changed file.c");
	}
	g_assert_cmpstr(ai_tool_call_get_result(ai_view_tool_block_get_call(
		AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(transcript, 3)), 0)), ==, "ok");

	{
		g_autofree gchar *other = g_build_filename(box->dir, "other-project", NULL);
		g_autofree gchar *selected_id = NULL;
		g_autofree gchar *selected_path = NULL;

		g_assert_cmpint(g_mkdir(other, 0700), ==, 0);
		ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), other);
		selected_path = ai_tui_codex_history_find(AI_CLI_CLIENT(provider), &selected_id);
		g_assert_null(selected_path);
		g_assert_null(selected_id);
		ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), box->dir);
	}

	write_codex_history(box, "latest", "2026-09-07T12:00:00Z", box->dir,
		CODEX_USER("partial history must not appear") "{broken\n");
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "Could not load native session history"));
	g_assert_null(strstr(box->out, "previous question"));
	g_assert_null(strstr(box->out, "partial history must not appear"));

	{
		g_autofree gchar *old_home = g_build_filename(box->dir, ".codex", NULL);
		g_autofree gchar *new_home = g_build_filename(box->dir, "native-home", NULL);
		g_autofree gchar *selected_id = NULL;
		g_autofree gchar *selected_path = NULL;

		g_assert_cmpint(g_rename(old_home, new_home), ==, 0);
		ai_cli_client_set_env(AI_CLI_CLIENT(provider), "CODEX_HOME", new_home);
		selected_path = ai_tui_codex_history_find(AI_CLI_CLIENT(provider), &selected_id);
		g_assert_nonnull(selected_path);
		g_assert_cmpstr(selected_id, ==, "latest");
	}
}

static void
touch_stamp(const gchar *path, const gchar *stamp)
{
	g_autoptr(GDateTime) at = g_date_time_new_from_iso8601(stamp, NULL);
	struct utimbuf times;

	g_assert_nonnull(at);
	times.actime = times.modtime = (time_t)g_date_time_to_unix(at);
	g_assert_cmpint(g_utime(path, &times), ==, 0);
}

static void
write_claude_history(Box *box, const gchar *id, const gchar *stamp, const gchar *log)
{
	g_autofree gchar *encoded = ai_tui_history_dash_path(box->dir, TRUE);
	g_autofree gchar *rel = g_strdup_printf(".claude/projects/%s/%s.jsonl", encoded, id);
	g_autofree gchar *path = g_build_filename(box->dir, rel, NULL);

	box_write(box, rel, log);
	touch_stamp(path, stamp);
}

static void
write_cursor_history(Box *box, const gchar *id, const gchar *stamp, const gchar *log)
{
	g_autofree gchar *encoded = ai_tui_history_dash_path(box->dir, FALSE);
	g_autofree gchar *rel = g_strdup_printf(".cursor/projects/%s/agent-transcripts/%s/%s.jsonl", encoded, id, id);
	g_autofree gchar *path = g_build_filename(box->dir, rel, NULL);

	box_write(box, rel, log);
	touch_stamp(path, stamp);
}

static void
write_agy_history(Box *box, const gchar *id, const gchar *cwd, const gchar *stamp, const gchar *log)
{
	g_autofree gchar *rel = g_strdup_printf(".gemini/antigravity-cli/brain/%s/.system_generated/logs/transcript.jsonl", id);
	g_autofree gchar *path = g_build_filename(box->dir, rel, NULL);
	g_autofree gchar *index = g_strdup_printf("{\"%s\":\"%s\"}", cwd, id);
	g_autofree gchar *existing = NULL;
	g_autofree gchar *index_path = g_build_filename(box->dir, ".gemini/antigravity-cli/cache/last_conversations.json", NULL);

	if (g_file_get_contents(index_path, &existing, NULL, NULL) && existing != NULL && *existing == '{')
	{
		g_autofree gchar *merged = NULL;
		existing[strlen(existing) - 1] = '\0';
		merged = g_strdup_printf("%s,\"%s\":\"%s\"}", existing, cwd, id);
		box_write(box, ".gemini/antigravity-cli/cache/last_conversations.json", merged);
	}
	else
		box_write(box, ".gemini/antigravity-cli/cache/last_conversations.json", index);
	box_write(box, rel, log);
	touch_stamp(path, stamp);
}

static void
write_opencode_history(Box *box, const gchar *id, const gchar *cwd, gint64 updated,
                       const gchar *user, const gchar *reply)
{
	g_autofree gchar *path = g_build_filename(box->dir, "opencode", "opencode.db", NULL);
	g_autofree gchar *parent = g_path_get_dirname(path);
	g_autofree gchar *sql = NULL;
	sqlite3 *db = NULL;
	char *errmsg = NULL;

	g_assert_cmpint(g_mkdir_with_parents(parent, 0700), ==, 0);
	g_assert_cmpint(sqlite3_open(path, &db), ==, SQLITE_OK);
	sql = g_strdup_printf(
		"CREATE TABLE IF NOT EXISTS session (id TEXT PRIMARY KEY, directory TEXT, title TEXT, time_updated INTEGER, parent_id TEXT);"
		"CREATE TABLE IF NOT EXISTS message (id TEXT PRIMARY KEY, session_id TEXT, time_created INTEGER, data TEXT);"
		"CREATE TABLE IF NOT EXISTS part (id TEXT PRIMARY KEY, message_id TEXT, session_id TEXT, time_created INTEGER, data TEXT);"
		"INSERT OR REPLACE INTO session(id, directory, title, time_updated, parent_id) VALUES ('%s', '%s', 't', %lld, NULL);"
		"INSERT OR REPLACE INTO message VALUES ('mu-%s', '%s', 1, '{\"role\":\"user\"}');"
		"INSERT OR REPLACE INTO message VALUES ('ma-%s', '%s', 2, '{\"role\":\"assistant\"}');"
		"INSERT OR REPLACE INTO part VALUES ('pu-%s', 'mu-%s', '%s', 1, '{\"type\":\"text\",\"text\":\"%s\"}');"
		"INSERT OR REPLACE INTO part VALUES ('pr-%s', 'ma-%s', '%s', 2, '{\"type\":\"reasoning\",\"text\":\"previous reasoning\"}');"
		"INSERT OR REPLACE INTO part VALUES ('pa-%s', 'ma-%s', '%s', 3, '{\"type\":\"text\",\"text\":\"%s\"}');"
		"INSERT OR REPLACE INTO part VALUES ('pt-%s', 'ma-%s', '%s', 4, '{\"type\":\"tool\",\"tool\":\"bash\",\"callID\":\"c1\",\"state\":{\"status\":\"completed\",\"input\":{\"command\":\"make\"},\"output\":\"ok\"}}');",
		id, cwd, (long long)updated, id, id, id, id, id, id, id, user, id, id, id, id, id, id, reply, id, id, id);
	g_assert_cmpint(sqlite3_exec(db, sql, NULL, NULL, &errmsg), ==, SQLITE_OK);
	sqlite3_close(db);
}

#define CLAUDE_USER(text) "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"" text "\"}}\n"
#define CLAUDE_ASSISTANT "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[" \
	"{\"type\":\"thinking\",\"thinking\":\"previous reasoning\"}," \
	"{\"type\":\"text\",\"text\":\"previous answer\"}," \
	"{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Read\",\"input\":{\"file_path\":\"file.c\"}}]}}\n" \
	"{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"saved output\"}]}}\n"
#define CURSOR_USER(text) "{\"role\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<user_query>" text "</user_query>\"}]}}\n"
#define CURSOR_ASSISTANT "{\"role\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"previous answer\"}]}}\n"
#define AGY_USER(text) "{\"type\":\"USER_INPUT\",\"content\":\"<USER_REQUEST>\\n" text "\\n</USER_REQUEST>\"}\n"
#define AGY_ASSISTANT "{\"type\":\"PLANNER_RESPONSE\",\"content\":\"previous answer\",\"tool_calls\":[{\"name\":\"view_file\",\"args\":{\"AbsolutePath\":\"file.c\"}}]}\n"

static void
test_claude_native_history(Box *box, gconstpointer data)
{
	const gchar *args[] = { "-p", "claude-code", "-c", "--dump", "new prompt", NULL };
	const gchar *fresh[] = { "-p", "claude-code", "--dump", "fresh prompt", NULL };
	g_autoptr(AiClaudeCodeClient) code = ai_claude_code_client_new();
	g_autoptr(AiClaudeTmuxClient) tmux = ai_claude_tmux_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(code));
	g_autoptr(GPtrArray) history = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *argv = NULL;

	(void)data;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai-tui unavailable");
		return;
	}
	write_claude_history(box, "older", "2026-09-06T12:00:00Z", CLAUDE_USER("older question"));
	write_claude_history(box, "latest", "2026-09-07T12:00:00Z",
		CLAUDE_USER("previous question") CLAUDE_ASSISTANT);
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "previous question"));
	g_assert_nonnull(strstr(box->out, "previous answer"));
	g_assert_null(strstr(box->out, "older question"));
	argv = box_read(box, "claude.argv");
	g_assert_nonnull(strstr(argv, "--resume\nlatest\n"));
	g_assert_cmpint(run_box(box, TRUE, fresh, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));

	ai_cli_client_set_env(AI_CLI_CLIENT(code), "HOME", box->dir);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(code), box->dir);
	g_object_set(code, "continue-session", TRUE, NULL);
	ai_tui_history_restore(conversation, history);
	g_assert_cmpuint(ai_transcript_get_n_blocks(ai_conversation_get_transcript(conversation)), ==, 4);
	g_assert_cmpstr(g_ptr_array_index(history, 0), ==, "previous question");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(code)), ==, "latest");
	{
		g_autofree gchar *summary = ai_view_tool_block_get_summary(
			AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(
				ai_conversation_get_transcript(conversation), 3)));

		g_assert_cmpstr(summary, ==, "Read file.c");
	}

	ai_cli_client_set_env(AI_CLI_CLIENT(tmux), "HOME", box->dir);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(tmux), box->dir);
	g_object_set(tmux, "continue-session", TRUE, NULL);
	{
		g_autoptr(AiConversation) tmux_conversation = ai_conversation_new(G_OBJECT(tmux));
		g_autoptr(GPtrArray) tmux_history = g_ptr_array_new_with_free_func(g_free);
		ai_tui_history_restore(tmux_conversation, tmux_history);
		g_assert_cmpuint(ai_transcript_get_n_blocks(ai_conversation_get_transcript(tmux_conversation)), >, 0);
		g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(tmux)), ==, "latest");
	}
}

static void
test_cursor_native_history(Box *box, gconstpointer data)
{
	const gchar *args[] = { "-p", "cursor", "-c", "--dump", "new prompt", NULL };
	const gchar *fresh[] = { "-p", "cursor", "--dump", "fresh prompt", NULL };
	g_autoptr(AiCursorClient) provider = ai_cursor_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(GPtrArray) history = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *argv = NULL;

	(void)data;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai-tui unavailable");
		return;
	}
	write_cursor_history(box, "older", "2026-09-06T12:00:00Z", CURSOR_USER("older question"));
	write_cursor_history(box, "latest", "2026-09-07T12:00:00Z",
		CURSOR_USER("previous question") CURSOR_ASSISTANT);
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "previous question"));
	g_assert_nonnull(strstr(box->out, "previous answer"));
	g_assert_null(strstr(box->out, "older question"));
	argv = box_read(box, "cursor.argv");
	g_assert_nonnull(strstr(argv, "--resume\nlatest\n"));
	g_assert_cmpint(run_box(box, TRUE, fresh, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));

	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "HOME", box->dir);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), box->dir);
	g_object_set(provider, "continue-session", TRUE, NULL);
	ai_tui_history_restore(conversation, history);
	g_assert_cmpuint(history->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(history, 0), ==, "previous question");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(provider)), ==, "latest");
}

static void
test_opencode_native_history(Box *box, gconstpointer data)
{
	const gchar *args[] = { "-p", "opencode", "-c", "--dump", "new prompt", NULL };
	const gchar *fresh[] = { "-p", "opencode", "--dump", "fresh prompt", NULL };
	g_autoptr(AiOpenCodeClient) provider = ai_opencode_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(GPtrArray) history = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *argv = NULL;

	(void)data;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai-tui unavailable");
		return;
	}
	write_opencode_history(box, "older", box->dir, 100, "older question", "older answer");
	write_opencode_history(box, "latest", box->dir, 200, "previous question", "previous answer");
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "previous question"));
	g_assert_nonnull(strstr(box->out, "previous answer"));
	g_assert_null(strstr(box->out, "older question"));
	argv = box_read(box, "opencode.argv");
	g_assert_nonnull(strstr(argv, "--session\nlatest\n"));
	g_assert_cmpint(run_box(box, TRUE, fresh, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));

	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "HOME", box->dir);
	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "XDG_DATA_HOME", box->dir);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), box->dir);
	g_object_set(provider, "continue-session", TRUE, NULL);
	ai_tui_history_restore(conversation, history);
	g_assert_cmpuint(ai_transcript_get_n_blocks(ai_conversation_get_transcript(conversation)), ==, 4);
	g_assert_cmpstr(g_ptr_array_index(history, 0), ==, "previous question");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(provider)), ==, "latest");
	{
		g_autofree gchar *summary = ai_view_tool_block_get_summary(
			AI_VIEW_TOOL_BLOCK(ai_transcript_get_block(
				ai_conversation_get_transcript(conversation), 3)));

		g_assert_cmpstr(summary, ==, "Ran make");
	}
}

static void
test_agy_native_history(Box *box, gconstpointer data)
{
	const gchar *args[] = { "-p", "agy", "-c", "--dump", "new prompt", NULL };
	const gchar *fresh[] = { "-p", "agy", "--dump", "fresh prompt", NULL };
	g_autoptr(AiAntigravityClient) provider = ai_antigravity_client_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(GPtrArray) history = g_ptr_array_new_with_free_func(g_free);
	g_autofree gchar *argv = NULL;

	(void)data;
	if (!g_file_test(tui_binary, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("ai-tui unavailable");
		return;
	}
	write_agy_history(box, "older", "/tmp/not-this-project", "2026-09-06T12:00:00Z",
		AGY_USER("older question") AGY_ASSISTANT);
	write_agy_history(box, "latest", box->dir, "2026-09-07T12:00:00Z",
		AGY_USER("previous question") AGY_ASSISTANT);
	g_assert_cmpint(run_box(box, TRUE, args, NULL, NULL), ==, 0);
	g_assert_nonnull(strstr(box->out, "previous question"));
	g_assert_nonnull(strstr(box->out, "previous answer"));
	g_assert_null(strstr(box->out, "older question"));
	argv = box_read(box, "agy.argv");
	g_assert_nonnull(strstr(argv, "--conversation\nlatest\n"));
	g_assert_cmpint(run_box(box, TRUE, fresh, NULL, NULL), ==, 0);
	g_assert_null(strstr(box->out, "previous question"));

	ai_cli_client_set_env(AI_CLI_CLIENT(provider), "HOME", box->dir);
	ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider), box->dir);
	g_object_set(provider, "continue-session", TRUE, NULL);
	ai_tui_history_restore(conversation, history);
	g_assert_cmpuint(history->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(history, 0), ==, "previous question");
	g_assert_cmpstr(ai_cli_client_get_session_id(AI_CLI_CLIENT(provider)), ==, "latest");
}

/* Resolve siblings before subprocesses change cwd; release/debug both work. */
int
main(int argc, char *argv[])
{
	g_autofree gchar *self = NULL;
	g_autofree gchar *absolute = NULL;
	g_autofree gchar *dir = NULL;
	gint status;

	/* Spawned fixtures must never register in the developer's real herdr pane. */
	g_unsetenv("HERDR_ENV");
	g_test_init(&argc, &argv, NULL);
	self = g_file_read_link("/proc/self/exe", NULL);
	absolute = g_canonicalize_filename(self != NULL ? self : argv[0], NULL);
	dir = g_path_get_dirname(absolute);
	library_dir = g_canonicalize_filename("..", dir);
	ai_binary = g_build_filename(library_dir, "bin", "ai", NULL);
	tui_binary = g_build_filename(library_dir, "bin", "ai-tui", NULL);
	g_assert_true(g_file_test(ai_binary, G_FILE_TEST_IS_EXECUTABLE));

#define ADD(name, func) g_test_add("/ai-glib/ai-defaults/" name, Box, NULL, box_setup, func, box_teardown)
	ADD("setup/create", test_create);
	ADD("setup/update-scopes", test_update);
	ADD("setup/cancel-eof", test_cancel);
	ADD("setup/invalid-retry", test_invalid_retry);
	ADD("setup/malformed-unchanged", test_malformed);
	ADD("setup/http-fallback", test_http_fallback);
	ADD("setup/library-independent", test_library_independent);
	ADD("ai-resolution", test_ai_resolution);
	ADD("tui-resolution", test_tui_resolution);
	ADD("process-timeout/default", test_process_timeout_default);
	ADD("process-timeout/stream", test_process_timeout_stream);
	ADD("native-history", test_native_history);
	ADD("codex-native-history", test_codex_native_history);
	ADD("claude-native-history", test_claude_native_history);
	ADD("cursor-native-history", test_cursor_native_history);
	ADD("opencode-native-history", test_opencode_native_history);
	ADD("agy-native-history", test_agy_native_history);
#undef ADD
	status = g_test_run();
	g_free(ai_binary);
	g_free(tui_binary);
	g_free(library_dir);
	return status;
}
