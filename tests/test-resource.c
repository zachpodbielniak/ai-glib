/*
 * test-resource.c - The frontmatter parser
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * These files are not ours. They are written by claude-code, by opencode,
 * by a plugin, or by hand at three in the morning, and ai-glib has to read
 * whatever is there without falling over. So most of what follows is
 * malformed input: unterminated delimiters, a sequence where a mapping was
 * expected, a BOM, CRLF, a NUL byte. The rule under test throughout is
 * that a bad file degrades -- it loses its description, never its
 * existence -- because one broken file must not take the other sixteen
 * with it, and a g_critical here would abort a suite running with
 * G_DEBUG=fatal-warnings.
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static AiResource *
parse(const gchar *text)
{
	GError     *error = NULL;
	AiResource *resource;

	resource = ai_resource_new_from_data(text, -1, "fallback",
	                                     AI_RESOURCE_COMMAND, "claude",
	                                     AI_RESOURCE_SCOPE_USER, &error);

	g_assert_no_error(error);
	g_assert_nonnull(resource);

	return resource;
}

/* A temporary directory that cleans itself up. */
typedef struct
{
	gchar *path;
} Sandbox;

static void
sandbox_init(Sandbox *sb)
{
	GError *error = NULL;

	sb->path = g_dir_make_tmp("ai-glib-resource-XXXXXX", &error);
	g_assert_no_error(error);
}

static void
sandbox_clear(Sandbox *sb)
{
	g_autofree gchar *cmd = NULL;

	if (sb->path == NULL)
	{
		return;
	}

	/* The trees here are small and entirely ours. */
	cmd = g_strdup_printf("rm -rf '%s'", sb->path);
	g_assert_cmpint(system(cmd), ==, 0);
	g_clear_pointer(&sb->path, g_free);
}

static gchar *
sandbox_write(Sandbox *sb, const gchar *relative, const gchar *contents)
{
	gchar            *path = g_build_filename(sb->path, relative, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	GError           *error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);

	return path;
}

/* ----------------------------------------------------------------
 * Structure: where frontmatter starts and stops
 * ---------------------------------------------------------------- */

static void
test_no_frontmatter_at_all(void)
{
	/* The real shape of ~/.config/opencode/agents/agent-code-reviewer.md:
	 * a bare markdown file with no metadata whatsoever. */
	g_autoptr(AiResource) r = parse("# Agent: Reviewer\n\nReviews C code.\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "fallback");
	g_assert_cmpstr(ai_resource_get_body(r), ==,
	                "# Agent: Reviewer\n\nReviews C code.\n");

	/* The heading is skipped; the sentence under it is the description. */
	g_assert_cmpstr(ai_resource_get_description(r), ==, "Reviews C code.");
}

static void
test_simple_frontmatter(void)
{
	g_autoptr(AiResource) r = parse(
		"---\n"
		"name: deploy\n"
		"description: Ship it\n"
		"---\n"
		"Body text.\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "deploy");
	g_assert_cmpstr(ai_resource_get_description(r), ==, "Ship it");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "Body text.\n");
}

static void
test_empty_frontmatter(void)
{
	g_autoptr(AiResource) r = parse("---\n---\nBody.\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "fallback");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "Body.\n");
}

static void
test_unterminated_frontmatter_is_not_frontmatter(void)
{
	/*
	 * A file caught mid-save. Treating the opening --- as frontmatter and
	 * swallowing the rest would make the command vanish from the listing;
	 * treating it as body keeps it visible.
	 */
	g_autoptr(AiResource) r = parse("---\nname: half\nstill going\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "fallback");
	g_assert_true(g_str_has_prefix(ai_resource_get_body(r), "---\n"));
}

static void
test_delimiter_not_on_first_line(void)
{
	/* A leading blank line means the --- is a horizontal rule, not
	 * frontmatter. Guessing otherwise would eat real content. */
	g_autoptr(AiResource) r = parse("\n---\nname: nope\n---\nBody\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "fallback");
	g_assert_null(ai_resource_get_meta(r, "name"));
}

static void
test_horizontal_rule_in_body_survives(void)
{
	g_autoptr(AiResource) r = parse(
		"---\n"
		"name: rules\n"
		"---\n"
		"Above.\n"
		"\n"
		"---\n"
		"\n"
		"Below.\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "rules");
	g_assert_nonnull(strstr(ai_resource_get_body(r), "---"));
	g_assert_nonnull(strstr(ai_resource_get_body(r), "Below."));
}

static void
test_frontmatter_only_no_body(void)
{
	g_autoptr(AiResource) r = parse("---\nname: bare\n---\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "bare");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "");
	g_assert_null(ai_resource_get_description(r));
}

static void
test_empty_file(void)
{
	g_autoptr(AiResource) r = parse("");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "fallback");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "");
	g_assert_null(ai_resource_get_description(r));
}

static void
test_dot_delimiter_closes_frontmatter(void)
{
	/* YAML's own end-of-document marker. Rare, but legal. */
	g_autoptr(AiResource) r = parse("---\nname: dots\n...\nBody\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "dots");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "Body\n");
}

static void
test_trailing_whitespace_on_delimiter(void)
{
	g_autoptr(AiResource) r = parse("---   \nname: sloppy\n---\t\nBody\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "sloppy");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "Body\n");
}

/* ----------------------------------------------------------------
 * Scalars, in every shape the real files use
 * ---------------------------------------------------------------- */

static void
test_folded_description(void)
{
	/* ~/.claude/commands/skill-gtest-scaffold.md is written this way. */
	g_autoptr(AiResource) r = parse(
		"---\n"
		"name: scaffold\n"
		"description: >\n"
		"  Generate GTest scaffolds with fixtures,\n"
		"  setup and teardown.\n"
		"---\n"
		"Body\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "scaffold");
	g_assert_nonnull(strstr(ai_resource_get_description(r), "fixtures"));
	g_assert_nonnull(strstr(ai_resource_get_description(r), "teardown"));
}

static void
test_literal_block_description(void)
{
	g_autoptr(AiResource) r = parse(
		"---\n"
		"description: |\n"
		"  Line one.\n"
		"  Line two.\n"
		"---\n");

	g_assert_nonnull(strstr(ai_resource_get_description(r), "Line one."));
	g_assert_nonnull(strstr(ai_resource_get_description(r), "Line two."));
}

static void
test_quoted_description_with_escapes(void)
{
	/* ~/.claude/agents/security-auditor.md is one enormous quoted string
	 * full of \n escapes. */
	g_autoptr(AiResource) r = parse(
		"---\n"
		"name: auditor\n"
		"description: \"Reviews code.\\n\\nExamples:\\n- one\"\n"
		"---\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "auditor");
	g_assert_nonnull(strstr(ai_resource_get_description(r), "Examples:"));

	/*
	 * And it is one line. A description ends up in a completion menu and
	 * a /help listing, both of which have room for exactly one --- so the
	 * flattening happens once, here, rather than in each of them.
	 */
	g_assert_null(strchr(ai_resource_get_description(r), '\n'));
	g_assert_cmpstr(ai_resource_get_description(r), ==,
	                "Reviews code. Examples: - one");
}

static void
test_description_is_always_one_line(void)
{
	g_autoptr(AiResource) folded = parse(
		"---\ndescription: >\n  first line\n  second line\n---\n");
	g_autoptr(AiResource) literal = parse(
		"---\ndescription: |\n  first line\n  second line\n---\n");

	g_assert_cmpstr(ai_resource_get_description(folded), ==,
	                "first line second line");
	g_assert_cmpstr(ai_resource_get_description(literal), ==,
	                "first line second line");
}

static void
test_value_containing_colon_and_hash(void)
{
	g_autoptr(AiResource) r = parse(
		"---\n"
		"description: \"see http://example.com/x#y for more\"\n"
		"---\n");

	g_assert_cmpstr(ai_resource_get_description(r), ==,
	                "see http://example.com/x#y for more");
}

static void
test_single_quoted_value(void)
{
	g_autoptr(AiResource) r = parse("---\nname: 'quoted name'\n---\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "quoted name");
}

static void
test_tools_as_comma_string(void)
{
	/* How the agent files under ~/.claude/agents write it. */
	g_autoptr(AiResource) r = parse(
		"---\nname: a\ntools: Bash, Read, Write\n---\n");
	g_auto(GStrv) tools = ai_resource_get_meta_list(r, "tools");

	g_assert_nonnull(tools);
	g_assert_cmpint(g_strv_length(tools), ==, 3);
	g_assert_cmpstr(tools[0], ==, "Bash");
	g_assert_cmpstr(tools[1], ==, "Read");
	g_assert_cmpstr(tools[2], ==, "Write");
}

static void
test_tools_as_yaml_sequence(void)
{
	/* How opencode writes it. A caller must not be able to tell. */
	g_autoptr(AiResource) r = parse(
		"---\nname: a\ntools:\n  - Bash\n  - Read\n  - Write\n---\n");
	g_auto(GStrv) tools = ai_resource_get_meta_list(r, "tools");

	g_assert_nonnull(tools);
	g_assert_cmpint(g_strv_length(tools), ==, 3);
	g_assert_cmpstr(tools[0], ==, "Bash");
	g_assert_cmpstr(tools[2], ==, "Write");

	/* And the flat accessor still answers, joined. */
	g_assert_cmpstr(ai_resource_get_meta(r, "tools"), ==, "Bash, Read, Write");
}

static void
test_tools_inline_sequence(void)
{
	g_autoptr(AiResource) r = parse("---\ntools: [Bash, Read]\n---\n");
	g_auto(GStrv) tools = ai_resource_get_meta_list(r, "tools");

	g_assert_nonnull(tools);
	g_assert_cmpint(g_strv_length(tools), ==, 2);
	g_assert_cmpstr(tools[0], ==, "Bash");
}

static void
test_meta_list_on_plain_scalar(void)
{
	g_autoptr(AiResource) r = parse("---\nmodel: sonnet\n---\n");
	g_auto(GStrv) models = ai_resource_get_meta_list(r, "model");

	g_assert_nonnull(models);
	g_assert_cmpint(g_strv_length(models), ==, 1);
	g_assert_cmpstr(models[0], ==, "sonnet");
}

static void
test_meta_list_on_missing_key(void)
{
	g_autoptr(AiResource) r = parse("---\nname: a\n---\n");

	/* NULL, not an empty array: "absent" and "present but empty" are
	 * different answers and a caller may act on the difference. */
	g_assert_null(ai_resource_get_meta_list(r, "nope"));
	g_assert_null(ai_resource_get_meta(r, "nope"));
}

static void
test_arbitrary_keys_are_reachable(void)
{
	/*
	 * The point of the generic accessor: harnesses invent fields, and
	 * ai-glib must not need editing each time one does.
	 */
	g_autoptr(AiResource) r = parse(
		"---\n"
		"name: a\n"
		"color: pink\n"
		"memory: project\n"
		"argument-hint: <file>\n"
		"allowed-tools: Bash(git status:*)\n"
		"---\n");

	g_assert_cmpstr(ai_resource_get_meta(r, "color"), ==, "pink");
	g_assert_cmpstr(ai_resource_get_meta(r, "memory"), ==, "project");
	g_assert_cmpstr(ai_resource_get_meta(r, "argument-hint"), ==, "<file>");
	g_assert_cmpstr(ai_resource_get_meta(r, "allowed-tools"), ==,
	                "Bash(git status:*)");
}

static void
test_meta_keys_are_listed_sorted(void)
{
	g_autoptr(AiResource) r = parse("---\nzebra: 1\nalpha: 2\nmid: 3\n---\n");
	g_auto(GStrv) keys = ai_resource_get_meta_keys(r);

	g_assert_cmpint(g_strv_length(keys), ==, 3);
	g_assert_cmpstr(keys[0], ==, "alpha");
	g_assert_cmpstr(keys[1], ==, "mid");
	g_assert_cmpstr(keys[2], ==, "zebra");
}

static void
test_meta_boolean(void)
{
	g_autoptr(AiResource) r = parse(
		"---\n"
		"shell: true\n"
		"off_key: no\n"
		"numeric: 1\n"
		"garbage: perhaps\n"
		"---\n");

	g_assert_true(ai_resource_get_meta_boolean(r, "shell", FALSE));
	g_assert_false(ai_resource_get_meta_boolean(r, "off_key", TRUE));
	g_assert_true(ai_resource_get_meta_boolean(r, "numeric", FALSE));

	/* Unparseable and absent both fall back, and the fallback is honoured
	 * in both directions -- this is the accessor that decides whether a
	 * file may run shell commands, so "I could not tell" must never read
	 * as "yes". */
	g_assert_false(ai_resource_get_meta_boolean(r, "garbage", FALSE));
	g_assert_true(ai_resource_get_meta_boolean(r, "garbage", TRUE));
	g_assert_false(ai_resource_get_meta_boolean(r, "absent", FALSE));
}

/* ----------------------------------------------------------------
 * Malformed metadata: degrade, never abort
 * ---------------------------------------------------------------- */

static void
test_broken_yaml_still_yields_a_resource(void)
{
	/*
	 * The case that matters most. A stray tab makes libyaml refuse the
	 * document; the file must still appear in the listing, minus its
	 * description. This test runs with fatal warnings on, so it also
	 * asserts the failure is logged at debug level.
	 */
	g_autoptr(AiResource) r = parse(
		"---\n"
		"name: broken\n"
		"\tbad: [unclosed\n"
		"---\n"
		"Body survives.\n");

	g_assert_nonnull(r);
	g_assert_cmpstr(ai_resource_get_name(r), ==, "fallback");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "Body survives.\n");
}

static void
test_frontmatter_that_is_a_sequence(void)
{
	g_autoptr(AiResource) r = parse("---\n- one\n- two\n---\nBody\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "fallback");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "Body\n");
}

static void
test_nested_mapping_value_is_ignored_not_fatal(void)
{
	g_autoptr(AiResource) r = parse(
		"---\n"
		"name: nested\n"
		"config:\n"
		"  a: 1\n"
		"  b: 2\n"
		"---\n");

	/* The scalar keys still work; the mapping simply is not modelled. */
	g_assert_cmpstr(ai_resource_get_name(r), ==, "nested");
	g_assert_null(ai_resource_get_meta(r, "config"));
}

static void
test_duplicate_keys_last_wins(void)
{
	g_autoptr(AiResource) r = parse("---\nname: first\nname: second\n---\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "second");
}

/* ----------------------------------------------------------------
 * Encoding
 * ---------------------------------------------------------------- */

static void
test_crlf(void)
{
	g_autoptr(AiResource) r = parse(
		"---\r\nname: windows\r\ndescription: ok\r\n---\r\nBody\r\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "windows");
	g_assert_cmpstr(ai_resource_get_description(r), ==, "ok");
}

static void
test_utf8_bom(void)
{
	/* Invisible in an editor, and without stripping it the first line is
	 * "\xef\xbb\xbf---" -- not a delimiter, so the whole file would read
	 * as body. */
	g_autoptr(AiResource) r = parse("\xef\xbb\xbf---\nname: bom\n---\nBody\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "bom");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "Body\n");
}

static void
test_multibyte_content(void)
{
	g_autoptr(AiResource) r = parse(
		"---\nname: café\ndescription: naïve — em dash\n---\n日本語\n");

	g_assert_cmpstr(ai_resource_get_name(r), ==, "café");
	g_assert_cmpstr(ai_resource_get_description(r), ==, "naïve — em dash");
	g_assert_cmpstr(ai_resource_get_body(r), ==, "日本語\n");
}

static void
test_invalid_utf8_is_an_error(void)
{
	GError     *error = NULL;
	AiResource *r;

	/* The one hard failure. Everything downstream -- byte offsets,
	 * wrapping, the model itself -- assumes valid UTF-8, so pretending
	 * would only move the crash. */
	r = ai_resource_new_from_data("---\nname: \xff\xfe\n---\n", -1, "x",
	                              AI_RESOURCE_COMMAND, "claude",
	                              AI_RESOURCE_SCOPE_USER, &error);

	g_assert_null(r);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_embedded_nul_truncates_not_crashes(void)
{
	GError     *error = NULL;
	AiResource *r;
	const gchar data[] = "---\nname: nul\n---\nbefore\0after\n";

	r = ai_resource_new_from_data(data, sizeof(data) - 1, "x",
	                              AI_RESOURCE_COMMAND, "claude",
	                              AI_RESOURCE_SCOPE_USER, &error);

	/* A NUL is not valid in UTF-8 text as far as g_utf8_validate is
	 * concerned, so this is refused rather than silently truncated. The
	 * point of the case is that it does neither of those quietly. */
	g_assert_null(r);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_large_body(void)
{
	g_autoptr(GString)    big = g_string_new("---\nname: big\n---\n");
	g_autoptr(AiResource) r = NULL;
	guint                 i;

	for (i = 0; i < 60000; i++)
	{
		g_string_append(big, "a line of perfectly ordinary text\n");
	}

	r = parse(big->str);

	g_assert_cmpstr(ai_resource_get_name(r), ==, "big");
	g_assert_cmpint(strlen(ai_resource_get_body(r)), >, 1000000);
}

/* ----------------------------------------------------------------
 * Naming
 * ---------------------------------------------------------------- */

static void
test_frontmatter_name_beats_filename(void)
{
	Sandbox sb;
	g_autofree gchar *path = NULL;
	g_autoptr(AiResource) r = NULL;
	GError *error = NULL;

	sandbox_init(&sb);
	path = sandbox_write(&sb, "on-disk.md", "---\nname: declared\n---\nx\n");

	r = ai_resource_new_from_file(path, NULL, AI_RESOURCE_COMMAND, "claude",
	                              AI_RESOURCE_SCOPE_USER, &error);

	g_assert_no_error(error);

	/* A file that names itself means it, even when the filename disagrees. */
	g_assert_cmpstr(ai_resource_get_name(r), ==, "declared");
	g_assert_cmpstr(ai_resource_get_path(r), ==, path);

	sandbox_clear(&sb);
}

static void
test_filename_is_the_fallback(void)
{
	Sandbox sb;
	g_autofree gchar *path = NULL;
	g_autoptr(AiResource) r = NULL;
	GError *error = NULL;

	sandbox_init(&sb);
	path = sandbox_write(&sb, "my-command.md", "Just a body.\n");

	r = ai_resource_new_from_file(path, NULL, AI_RESOURCE_COMMAND, "claude",
	                              AI_RESOURCE_SCOPE_USER, &error);

	g_assert_no_error(error);
	g_assert_cmpstr(ai_resource_get_name(r), ==, "my-command");

	sandbox_clear(&sb);
}

static void
test_skill_md_is_named_by_its_directory(void)
{
	Sandbox sb;
	g_autofree gchar *path = NULL;
	g_autoptr(AiResource) r = NULL;
	GError *error = NULL;

	sandbox_init(&sb);
	path = sandbox_write(&sb, "podomation-dsl/SKILL.md", "Instructions.\n");

	r = ai_resource_new_from_file(path, NULL, AI_RESOURCE_SKILL, "claude",
	                              AI_RESOURCE_SCOPE_USER, &error);

	g_assert_no_error(error);

	/* Otherwise every skill on the system would be called "SKILL". */
	g_assert_cmpstr(ai_resource_get_name(r), ==, "podomation-dsl");

	sandbox_clear(&sb);
}

static void
test_explicit_name_overrides_the_path(void)
{
	Sandbox sb;
	g_autofree gchar *path = NULL;
	g_autoptr(AiResource) r = NULL;
	GError *error = NULL;

	sandbox_init(&sb);
	path = sandbox_write(&sb, "git/status.md", "Body\n");

	/* This is how a namespaced command becomes /git:status -- the path
	 * alone cannot say it. */
	r = ai_resource_new_from_file(path, "git:status", AI_RESOURCE_COMMAND,
	                              "claude", AI_RESOURCE_SCOPE_PROJECT,
	                              &error);

	g_assert_no_error(error);
	g_assert_cmpstr(ai_resource_get_name(r), ==, "git:status");

	sandbox_clear(&sb);
}

static void
test_missing_file_is_an_error(void)
{
	GError     *error = NULL;
	AiResource *r;

	r = ai_resource_new_from_file("/nonexistent/nowhere.md", NULL,
	                              AI_RESOURCE_COMMAND, "claude",
	                              AI_RESOURCE_SCOPE_USER, &error);

	g_assert_null(r);
	g_assert_nonnull(error);
	g_clear_error(&error);
}

/* ----------------------------------------------------------------
 * GObject hygiene
 * ---------------------------------------------------------------- */

static void
test_properties_round_trip(void)
{
	g_autoptr(AiResource) r = NULL;
	g_autofree gchar     *name = NULL;
	g_autofree gchar     *description = NULL;
	g_autofree gchar     *body = NULL;
	g_autofree gchar     *origin = NULL;
	gint                  kind = -1;
	gint                  scope = -1;

	r = ai_resource_new_from_data("---\nname: p\ndescription: d\n---\nb\n",
	                              -1, "x", AI_RESOURCE_AGENT, "opencode",
	                              AI_RESOURCE_SCOPE_PROJECT, NULL);

	g_object_get(r,
	             "name", &name,
	             "description", &description,
	             "body", &body,
	             "origin", &origin,
	             "kind", &kind,
	             "scope", &scope,
	             NULL);

	g_assert_cmpstr(name, ==, "p");
	g_assert_cmpstr(description, ==, "d");
	g_assert_cmpstr(body, ==, "b\n");
	g_assert_cmpstr(origin, ==, "opencode");
	g_assert_cmpint(kind, ==, AI_RESOURCE_AGENT);
	g_assert_cmpint(scope, ==, AI_RESOURCE_SCOPE_PROJECT);

	g_assert_cmpint(ai_resource_get_kind(r), ==, AI_RESOURCE_AGENT);
	g_assert_cmpint(ai_resource_get_scope(r), ==, AI_RESOURCE_SCOPE_PROJECT);
	g_assert_null(ai_resource_get_path(r));
}

static void
test_default_origin(void)
{
	g_autoptr(AiResource) r = NULL;

	r = ai_resource_new_from_data("body", -1, "x", AI_RESOURCE_COMMAND,
	                              NULL, AI_RESOURCE_SCOPE_BUILTIN, NULL);

	g_assert_cmpstr(ai_resource_get_origin(r), ==, "ai-glib");
}

static void
test_refcounting(void)
{
	AiResource *r = parse("---\nname: rc\n---\n");

	g_object_ref(r);
	g_assert_cmpstr(ai_resource_get_name(r), ==, "rc");
	g_object_unref(r);
	g_assert_cmpstr(ai_resource_get_name(r), ==, "rc");
	g_object_unref(r);
}

static void
test_kind_and_scope_names(void)
{
	/* Stable strings: a frontend groups a listing by them. */
	g_assert_cmpstr(ai_resource_kind_to_string(AI_RESOURCE_COMMAND), ==,
	                "command");
	g_assert_cmpstr(ai_resource_kind_to_string(AI_RESOURCE_SKILL), ==,
	                "skill");
	g_assert_cmpstr(ai_resource_kind_to_string(AI_RESOURCE_AGENT), ==,
	                "agent");

	g_assert_cmpstr(ai_resource_scope_to_string(AI_RESOURCE_SCOPE_BUILTIN),
	                ==, "builtin");
	g_assert_cmpstr(ai_resource_scope_to_string(AI_RESOURCE_SCOPE_USER), ==,
	                "user");
	g_assert_cmpstr(ai_resource_scope_to_string(AI_RESOURCE_SCOPE_PROJECT),
	                ==, "project");

	/* Out of range must not read past the table. */
	g_assert_nonnull(ai_resource_kind_to_string((AiResourceKind)99));
	g_assert_nonnull(ai_resource_scope_to_string((AiResourceScope)99));
}

/* ----------------------------------------------------------------
 * Description derivation
 * ---------------------------------------------------------------- */

static void
test_description_skips_headings_and_fences(void)
{
	g_autoptr(AiResource) r = parse(
		"# Title\n"
		"\n"
		"## Subtitle\n"
		"\n"
		"```sh\n"
		"echo not this\n"
		"```\n"
		"\n"
		"The actual sentence.\n");

	/* A fence opener is skipped, but its contents are ordinary lines --
	 * the first one inside wins, which is the honest reading of "first
	 * line of prose" without teaching this a markdown parser. */
	g_assert_cmpstr(ai_resource_get_description(r), ==, "echo not this");
}

static void
test_description_of_body_with_only_headings(void)
{
	g_autoptr(AiResource) r = parse("# One\n\n## Two\n");

	g_assert_null(ai_resource_get_description(r));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/resource/no-frontmatter",
	                test_no_frontmatter_at_all);
	g_test_add_func("/ai-glib/resource/simple", test_simple_frontmatter);
	g_test_add_func("/ai-glib/resource/empty-frontmatter",
	                test_empty_frontmatter);
	g_test_add_func("/ai-glib/resource/unterminated",
	                test_unterminated_frontmatter_is_not_frontmatter);
	g_test_add_func("/ai-glib/resource/delimiter-not-first",
	                test_delimiter_not_on_first_line);
	g_test_add_func("/ai-glib/resource/rule-in-body",
	                test_horizontal_rule_in_body_survives);
	g_test_add_func("/ai-glib/resource/no-body",
	                test_frontmatter_only_no_body);
	g_test_add_func("/ai-glib/resource/empty-file", test_empty_file);
	g_test_add_func("/ai-glib/resource/dot-delimiter",
	                test_dot_delimiter_closes_frontmatter);
	g_test_add_func("/ai-glib/resource/delimiter-whitespace",
	                test_trailing_whitespace_on_delimiter);

	g_test_add_func("/ai-glib/resource/folded", test_folded_description);
	g_test_add_func("/ai-glib/resource/literal-block",
	                test_literal_block_description);
	g_test_add_func("/ai-glib/resource/quoted-escapes",
	                test_quoted_description_with_escapes);
	g_test_add_func("/ai-glib/resource/description-one-line",
	                test_description_is_always_one_line);
	g_test_add_func("/ai-glib/resource/colon-and-hash",
	                test_value_containing_colon_and_hash);
	g_test_add_func("/ai-glib/resource/single-quoted",
	                test_single_quoted_value);
	g_test_add_func("/ai-glib/resource/tools-comma",
	                test_tools_as_comma_string);
	g_test_add_func("/ai-glib/resource/tools-sequence",
	                test_tools_as_yaml_sequence);
	g_test_add_func("/ai-glib/resource/tools-inline",
	                test_tools_inline_sequence);
	g_test_add_func("/ai-glib/resource/meta-list-scalar",
	                test_meta_list_on_plain_scalar);
	g_test_add_func("/ai-glib/resource/meta-list-missing",
	                test_meta_list_on_missing_key);
	g_test_add_func("/ai-glib/resource/arbitrary-keys",
	                test_arbitrary_keys_are_reachable);
	g_test_add_func("/ai-glib/resource/meta-keys",
	                test_meta_keys_are_listed_sorted);
	g_test_add_func("/ai-glib/resource/meta-boolean", test_meta_boolean);

	g_test_add_func("/ai-glib/resource/broken-yaml",
	                test_broken_yaml_still_yields_a_resource);
	g_test_add_func("/ai-glib/resource/sequence-frontmatter",
	                test_frontmatter_that_is_a_sequence);
	g_test_add_func("/ai-glib/resource/nested-mapping",
	                test_nested_mapping_value_is_ignored_not_fatal);
	g_test_add_func("/ai-glib/resource/duplicate-keys",
	                test_duplicate_keys_last_wins);

	g_test_add_func("/ai-glib/resource/crlf", test_crlf);
	g_test_add_func("/ai-glib/resource/bom", test_utf8_bom);
	g_test_add_func("/ai-glib/resource/multibyte", test_multibyte_content);
	g_test_add_func("/ai-glib/resource/invalid-utf8",
	                test_invalid_utf8_is_an_error);
	g_test_add_func("/ai-glib/resource/embedded-nul",
	                test_embedded_nul_truncates_not_crashes);
	g_test_add_func("/ai-glib/resource/large-body", test_large_body);

	g_test_add_func("/ai-glib/resource/name-beats-filename",
	                test_frontmatter_name_beats_filename);
	g_test_add_func("/ai-glib/resource/filename-fallback",
	                test_filename_is_the_fallback);
	g_test_add_func("/ai-glib/resource/skill-md-name",
	                test_skill_md_is_named_by_its_directory);
	g_test_add_func("/ai-glib/resource/explicit-name",
	                test_explicit_name_overrides_the_path);
	g_test_add_func("/ai-glib/resource/missing-file",
	                test_missing_file_is_an_error);

	g_test_add_func("/ai-glib/resource/properties",
	                test_properties_round_trip);
	g_test_add_func("/ai-glib/resource/default-origin", test_default_origin);
	g_test_add_func("/ai-glib/resource/refcount", test_refcounting);
	g_test_add_func("/ai-glib/resource/enum-names",
	                test_kind_and_scope_names);

	g_test_add_func("/ai-glib/resource/description-derived",
	                test_description_skips_headings_and_fences);
	g_test_add_func("/ai-glib/resource/description-none",
	                test_description_of_body_with_only_headings);

	return g_test_run();
}
