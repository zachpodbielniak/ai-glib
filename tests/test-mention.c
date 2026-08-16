/*
 * test-mention.c - @file references
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Two halves, deliberately separate. Scanning is pure and syntactic --- it
 * runs on every keystroke, so it must never touch a disk -- and expansion
 * is where a mention meets the filesystem. That split is what leaves a
 * Python decorator alone without this file knowing what Python is: it
 * scans like anything else and then resolves to nothing.
 */

#include <ai-glib.h>

#include <glib/gstdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static gchar *sandbox = NULL;

static void
rm_rf(const gchar *path)
{
	g_autofree gchar *cmd = g_strdup_printf("rm -rf '%s'", path);

	g_assert_cmpint(system(cmd), ==, 0);
}

static void
write_file(const gchar *relative, const gchar *contents, gssize len)
{
	g_autofree gchar *path = g_build_filename(sandbox, relative, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	GError           *error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
	g_file_set_contents(path, contents, len, &error);
	g_assert_no_error(error);
}

/* The nth mention, or NULL. */
static const AiMention *
nth(GList *mentions, guint index)
{
	return g_list_nth_data(mentions, index);
}

/* ----------------------------------------------------------------
 * Scanning: boundaries
 * ---------------------------------------------------------------- */

static void
test_scan_nothing(void)
{
	g_assert_null(ai_mention_scan(NULL));
	g_assert_null(ai_mention_scan(""));
	g_assert_null(ai_mention_scan("no mentions here"));
}

static void
test_scan_at_start(void)
{
	GList *m = ai_mention_scan("@src/foo.c please");

	g_assert_cmpint(g_list_length(m), ==, 1);
	g_assert_cmpstr(nth(m, 0)->path, ==, "src/foo.c");
	g_assert_cmpuint(nth(m, 0)->start, ==, 0);
	g_assert_cmpuint(nth(m, 0)->len, ==, strlen("@src/foo.c"));

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_email_is_not_a_mention(void)
{
	/* The `@` follows a letter, so the boundary rule excludes it. This
	 * is the case that made the rule necessary. */
	GList *m = ai_mention_scan("write to z.podbielniak@gmail.com about it");

	g_assert_null(m);
}

static void
test_scan_after_brackets(void)
{
	GList *m = ai_mention_scan("see (@a.c) and [@b.c] and {@c.c}");

	g_assert_cmpint(g_list_length(m), ==, 3);
	g_assert_cmpstr(nth(m, 0)->path, ==, "a.c");
	g_assert_cmpstr(nth(m, 1)->path, ==, "b.c");
	g_assert_cmpstr(nth(m, 2)->path, ==, "c.c");

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_trailing_punctuation(void)
{
	GList *m = ai_mention_scan("look at @src/foo.c. Then @bar.h, then @baz.c!");

	g_assert_cmpint(g_list_length(m), ==, 3);

	/* The stop belongs to the sentence, not the path. */
	g_assert_cmpstr(nth(m, 0)->path, ==, "src/foo.c");
	g_assert_cmpstr(nth(m, 1)->path, ==, "bar.h");
	g_assert_cmpstr(nth(m, 2)->path, ==, "baz.c");

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_bare_at(void)
{
	g_assert_null(ai_mention_scan("@"));
	g_assert_null(ai_mention_scan("a @ b"));
	g_assert_null(ai_mention_scan("trailing @"));
	g_assert_null(ai_mention_scan("@."));
}

static void
test_scan_adjacent_mentions(void)
{
	GList *m = ai_mention_scan("@a.c @b.c");

	g_assert_cmpint(g_list_length(m), ==, 2);
	g_assert_cmpuint(nth(m, 0)->start, ==, 0);
	g_assert_cmpuint(nth(m, 1)->start, ==, 5);

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_across_lines(void)
{
	GList *m = ai_mention_scan("first @a.c\nsecond @b.c\n");

	g_assert_cmpint(g_list_length(m), ==, 2);
	g_assert_cmpstr(nth(m, 1)->path, ==, "b.c");

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_quoted_path(void)
{
	GList *m = ai_mention_scan("open @\"my notes/today.org\" now");

	g_assert_cmpint(g_list_length(m), ==, 1);
	g_assert_cmpstr(nth(m, 0)->path, ==, "my notes/today.org");
	g_assert_cmpuint(nth(m, 0)->len, ==,
	                 strlen("@\"my notes/today.org\""));

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_unterminated_quote(void)
{
	/* Recovered as a bare mention rather than swallowing the line. */
	GList *m = ai_mention_scan("open @\"unfinished and more");

	g_assert_cmpint(g_list_length(m), ==, 1);
	g_assert_cmpstr(nth(m, 0)->path, ==, "\"unfinished");

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_empty_quotes(void)
{
	GList *m = ai_mention_scan("empty @\"\" here");

	g_assert_null(m);
}

/* ----------------------------------------------------------------
 * Scanning: offsets
 * ---------------------------------------------------------------- */

static void
test_scan_offsets_are_bytes(void)
{
	/*
	 * The same invariant AiStyleSpan carries. "héllo" is six bytes and
	 * five characters; a mention after it must report six.
	 */
	const gchar *text = "héllo @foo.c";
	GList       *m = ai_mention_scan(text);

	g_assert_cmpint(g_list_length(m), ==, 1);
	g_assert_cmpuint(nth(m, 0)->start, ==, 7);
	g_assert_cmpint(text[nth(m, 0)->start], ==, '@');

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_scan_multibyte_path(void)
{
	const gchar *text = "read @docs/café.org now";
	GList       *m = ai_mention_scan(text);
	const AiMention *mention;

	g_assert_cmpint(g_list_length(m), ==, 1);
	mention = nth(m, 0);

	g_assert_cmpstr(mention->path, ==, "docs/café.org");

	/* The span covers exactly the mention, and both ends land on
	 * character boundaries. */
	g_assert_cmpint(text[mention->start], ==, '@');
	g_assert_true(g_utf8_validate(text + mention->start, mention->len, NULL));
	g_assert_cmpint(text[mention->start + mention->len], ==, ' ');

	g_list_free_full(m, (GDestroyNotify)ai_mention_free);
}

static void
test_mention_boxed_type(void)
{
	g_autoptr(AiMention) m = ai_mention_new(3, 7, "some/path");
	g_autoptr(AiMention) copy = ai_mention_copy(m);

	g_assert_cmpuint(copy->start, ==, 3);
	g_assert_cmpuint(copy->len, ==, 7);
	g_assert_cmpstr(copy->path, ==, "some/path");

	g_assert_null(ai_mention_copy(NULL));
	ai_mention_free(NULL);

	g_assert_true(AI_TYPE_MENTION != G_TYPE_INVALID);
}

/* ----------------------------------------------------------------
 * Resolution
 * ---------------------------------------------------------------- */

static void
test_resolve(void)
{
	g_autofree gchar *relative = ai_mention_resolve("a/b.c", "/tmp/proj");
	g_autofree gchar *absolute = ai_mention_resolve("/etc/hosts", "/tmp/proj");
	g_autofree gchar *tilde = ai_mention_resolve("~/notes.org", "/tmp/proj");
	g_autofree gchar *expected_tilde =
		g_build_filename(g_get_home_dir(), "notes.org", NULL);

	g_assert_cmpstr(relative, ==, "/tmp/proj/a/b.c");
	g_assert_cmpstr(absolute, ==, "/etc/hosts");
	g_assert_cmpstr(tilde, ==, expected_tilde);

	g_assert_null(ai_mention_resolve("", "/tmp"));
}

static void
test_resolve_no_globbing(void)
{
	/*
	 * A literal path, always. If `*` were expanded, `@*` would pull in
	 * every file in the directory -- something the user did not name.
	 */
	g_autofree gchar *starred = ai_mention_resolve("*.c", "/tmp/proj");

	g_assert_cmpstr(starred, ==, "/tmp/proj/*.c");
}

/* ----------------------------------------------------------------
 * Expansion
 * ---------------------------------------------------------------- */

static void
test_expand_no_mentions(void)
{
	g_autofree gchar *out = ai_mention_expand("plain text", sandbox, 0, NULL);

	g_assert_cmpstr(out, ==, "plain text");
}

static void
test_expand_unresolvable_is_untouched(void)
{
	GList            *files = NULL;
	g_autofree gchar *out =
		ai_mention_expand("@nothing/here.c please", sandbox, 0, &files);

	/* No trailer at all, and the prompt survives verbatim. */
	g_assert_cmpstr(out, ==, "@nothing/here.c please");
	g_assert_null(files);
}

static void
test_expand_python_decorator(void)
{
	/*
	 * The reason expansion, not scanning, decides what is real. There is
	 * no file called "property", so the decorator is left exactly as
	 * written -- and this file never had to learn what Python is.
	 */
	const gchar      *code = "@property\ndef x(self):\n    return 1\n";
	g_autofree gchar *out = ai_mention_expand(code, sandbox, 0, NULL);

	g_assert_cmpstr(out, ==, code);
}

static void
test_expand_file(void)
{
	GList            *files = NULL;
	g_autofree gchar *out = NULL;

	write_file("src/hello.c", "int main(void) { return 0; }\n", -1);

	out = ai_mention_expand("explain @src/hello.c please", sandbox, 0,
	                        &files);

	/* The prompt keeps its shape: the sentence is not cut in half by
	 * nine hundred lines of file. */
	g_assert_true(g_str_has_prefix(out, "explain @src/hello.c please\n\n"));
	g_assert_nonnull(strstr(out, "--- Referenced files ---"));
	g_assert_nonnull(strstr(out, "int main(void)"));

	/* Fenced, with a language a model can use. */
	g_assert_nonnull(strstr(out, "```c\n"));

	g_assert_cmpint(g_list_length(files), ==, 1);
	g_assert_true(g_str_has_suffix(files->data, "src/hello.c"));
	g_list_free_full(files, g_free);
}

static void
test_expand_several_files(void)
{
	g_autofree gchar *out = NULL;

	write_file("a.py", "print('a')\n", -1);
	write_file("b.sh", "echo b\n", -1);

	out = ai_mention_expand("compare @a.py and @b.sh", sandbox, 0, NULL);

	g_assert_nonnull(strstr(out, "print('a')"));
	g_assert_nonnull(strstr(out, "echo b"));
	g_assert_nonnull(strstr(out, "```python\n"));
	g_assert_nonnull(strstr(out, "```sh\n"));
}

static void
test_expand_same_file_twice(void)
{
	g_autofree gchar *out = NULL;
	const gchar      *first;

	write_file("dup.c", "UNIQUE_MARKER\n", -1);

	out = ai_mention_expand("@dup.c versus @dup.c", sandbox, 0, NULL);

	first = strstr(out, "UNIQUE_MARKER");
	g_assert_nonnull(first);

	/* Included once: sending the model the same file twice wastes the
	 * budget it was meant to protect. */
	g_assert_null(strstr(first + 1, "UNIQUE_MARKER"));
}

static void
test_expand_directory(void)
{
	g_autofree gchar *out = NULL;

	write_file("tree/one.c", "1\n", -1);
	write_file("tree/two.c", "2\n", -1);
	write_file("tree/sub/three.c", "3\n", -1);

	out = ai_mention_expand("look in @tree", sandbox, 0, NULL);

	g_assert_nonnull(strstr(out, "(directory)"));
	g_assert_nonnull(strstr(out, "one.c"));
	g_assert_nonnull(strstr(out, "two.c"));

	/* Directories are marked, and the listing does not recurse. */
	g_assert_nonnull(strstr(out, "sub/"));
	g_assert_null(strstr(out, "three.c"));
}

static void
test_expand_binary_file_is_named_not_inlined(void)
{
	g_autofree gchar *out = NULL;

	write_file("blob.bin", "\x00\x01\xff\xfe binary", 10);

	out = ai_mention_expand("what is @blob.bin", sandbox, 0, NULL);

	/* Named, so the model knows it exists; not inlined, because the
	 * bytes would be noise at best. */
	g_assert_nonnull(strstr(out, "@blob.bin (binary"));
	g_assert_null(strstr(out, "```"));
}

static void
test_expand_unreadable_file_is_reported(void)
{
	g_autofree gchar *path = NULL;
	g_autofree gchar *out = NULL;

	write_file("secret.c", "hidden\n", -1);
	path = g_build_filename(sandbox, "secret.c", NULL);
	g_assert_cmpint(g_chmod(path, 0), ==, 0);

	out = ai_mention_expand("read @secret.c", sandbox, 0, NULL);

	/* Silence here would read as "the file was empty", which is a
	 * different and much more misleading claim. */
	g_assert_nonnull(strstr(out, "cannot be read"));

	g_assert_cmpint(g_chmod(path, 0644), ==, 0);
}

static void
test_expand_truncates_and_says_so(void)
{
	g_autoptr(GString) big = g_string_new(NULL);
	g_autofree gchar  *out = NULL;
	guint              i;

	for (i = 0; i < 5000; i++)
	{
		g_string_append(big, "0123456789\n");
	}

	write_file("big.txt", big->str, -1);

	out = ai_mention_expand("@big.txt", sandbox, 1024, NULL);

	/* Stated, never silent: a model given half a file without being told
	 * will answer confidently about the half it never saw. */
	g_assert_nonnull(strstr(out, "(truncated at"));
	g_assert_cmpint(strlen(out), <, 4096);
}

static void
test_expand_truncation_respects_characters(void)
{
	g_autoptr(GString) big = g_string_new(NULL);
	g_autofree gchar  *out = NULL;
	guint              i;

	/* Every line is multibyte, so a byte-counted cut lands mid-character
	 * unless it is corrected. */
	for (i = 0; i < 500; i++)
	{
		g_string_append(big, "日本語のテキストです\n");
	}

	write_file("wide.txt", big->str, -1);

	out = ai_mention_expand("@wide.txt", sandbox, 101, NULL);

	g_assert_true(g_utf8_validate(out, -1, NULL));
}

static void
test_expand_budget_is_shared(void)
{
	g_autoptr(GString) big = g_string_new(NULL);
	g_autofree gchar  *out = NULL;
	guint              i;

	for (i = 0; i < 2000; i++)
	{
		g_string_append(big, "aaaaaaaaaa\n");
	}

	write_file("one.txt", big->str, -1);
	write_file("two.txt", big->str, -1);

	out = ai_mention_expand("@one.txt @two.txt", sandbox, 2048, NULL);

	/* One budget across the whole expansion, not one per file. */
	g_assert_cmpint(strlen(out), <, 8192);
}

static void
test_expand_is_not_reentrant(void)
{
	g_autofree gchar *out = NULL;

	/* An inlined file that itself contains a mention must not pull in a
	 * second file: one round, always, or a self-referential file would
	 * expand forever. */
	write_file("outer.txt", "see @inner.txt for details\n", -1);
	write_file("inner.txt", "INNER_MARKER\n", -1);

	out = ai_mention_expand("@outer.txt", sandbox, 0, NULL);

	g_assert_nonnull(strstr(out, "see @inner.txt"));
	g_assert_null(strstr(out, "INNER_MARKER"));
}

static void
test_expand_absolute_and_parent_paths(void)
{
	GList            *files = NULL;
	g_autofree gchar *nested = g_build_filename(sandbox, "deep", NULL);
	g_autofree gchar *out = NULL;

	write_file("above.txt", "ABOVE\n", -1);
	write_file("deep/below.txt", "BELOW\n", -1);

	out = ai_mention_expand("@../above.txt and @below.txt", nested, 0,
	                        &files);

	/*
	 * Traversal upward is allowed -- a user naming a file outside the
	 * project usually means it -- but every resolved path is reported so
	 * a frontend can show what was actually pulled in.
	 */
	g_assert_nonnull(strstr(out, "ABOVE"));
	g_assert_nonnull(strstr(out, "BELOW"));
	g_assert_cmpint(g_list_length(files), ==, 2);
	g_list_free_full(files, g_free);
}

static void
test_expand_out_files_may_be_null(void)
{
	g_autofree gchar *out = NULL;

	write_file("solo.c", "x\n", -1);

	/* Passing NULL must not leak the list it would have built. */
	out = ai_mention_expand("@solo.c", sandbox, 0, NULL);
	g_assert_nonnull(strstr(out, "```c"));
}

static void
test_expand_quoted_path_with_space(void)
{
	g_autofree gchar *out = NULL;

	write_file("my notes/today.org", "TODAY\n", -1);

	out = ai_mention_expand("open @\"my notes/today.org\"", sandbox, 0, NULL);

	g_assert_nonnull(strstr(out, "TODAY"));
}

static void
test_expand_unquoted_space_stops_at_the_space(void)
{
	g_autofree gchar *out = NULL;

	write_file("my notes/today.org", "TODAY\n", -1);

	/* The bare form cannot contain a space, and this asserts the rule
	 * rather than leaving it to chance. */
	out = ai_mention_expand("open @my notes/today.org", sandbox, 0, NULL);

	g_assert_null(strstr(out, "TODAY"));
}

static void
test_expand_large_directory_is_capped(void)
{
	g_autofree gchar *out = NULL;
	guint             i;

	for (i = 0; i < 400; i++)
	{
		g_autofree gchar *name =
			g_strdup_printf("many/file-%03u.txt", i);

		write_file(name, "x\n", -1);
	}

	out = ai_mention_expand("@many", sandbox, 0, NULL);

	/* Capped, and it says how many it left out rather than pretending
	 * the directory was that size. */
	g_assert_nonnull(strstr(out, "and 200 more"));
}

int
main(int argc, char *argv[])
{
	GError *error = NULL;
	int     status;

	g_test_init(&argc, &argv, NULL);

	sandbox = g_dir_make_tmp("ai-glib-mention-XXXXXX", &error);
	g_assert_no_error(error);

	g_test_add_func("/ai-glib/mention/scan-nothing", test_scan_nothing);
	g_test_add_func("/ai-glib/mention/scan-at-start", test_scan_at_start);
	g_test_add_func("/ai-glib/mention/scan-email",
	                test_scan_email_is_not_a_mention);
	g_test_add_func("/ai-glib/mention/scan-brackets", test_scan_after_brackets);
	g_test_add_func("/ai-glib/mention/scan-punctuation",
	                test_scan_trailing_punctuation);
	g_test_add_func("/ai-glib/mention/scan-bare-at", test_scan_bare_at);
	g_test_add_func("/ai-glib/mention/scan-adjacent",
	                test_scan_adjacent_mentions);
	g_test_add_func("/ai-glib/mention/scan-lines", test_scan_across_lines);
	g_test_add_func("/ai-glib/mention/scan-quoted", test_scan_quoted_path);
	g_test_add_func("/ai-glib/mention/scan-unterminated-quote",
	                test_scan_unterminated_quote);
	g_test_add_func("/ai-glib/mention/scan-empty-quotes",
	                test_scan_empty_quotes);

	g_test_add_func("/ai-glib/mention/offsets-bytes",
	                test_scan_offsets_are_bytes);
	g_test_add_func("/ai-glib/mention/multibyte-path",
	                test_scan_multibyte_path);
	g_test_add_func("/ai-glib/mention/boxed", test_mention_boxed_type);

	g_test_add_func("/ai-glib/mention/resolve", test_resolve);
	g_test_add_func("/ai-glib/mention/resolve-no-glob",
	                test_resolve_no_globbing);

	g_test_add_func("/ai-glib/mention/expand-none", test_expand_no_mentions);
	g_test_add_func("/ai-glib/mention/expand-unresolvable",
	                test_expand_unresolvable_is_untouched);
	g_test_add_func("/ai-glib/mention/expand-decorator",
	                test_expand_python_decorator);
	g_test_add_func("/ai-glib/mention/expand-file", test_expand_file);
	g_test_add_func("/ai-glib/mention/expand-several",
	                test_expand_several_files);
	g_test_add_func("/ai-glib/mention/expand-dedup",
	                test_expand_same_file_twice);
	g_test_add_func("/ai-glib/mention/expand-directory",
	                test_expand_directory);
	g_test_add_func("/ai-glib/mention/expand-binary",
	                test_expand_binary_file_is_named_not_inlined);
	g_test_add_func("/ai-glib/mention/expand-unreadable",
	                test_expand_unreadable_file_is_reported);
	g_test_add_func("/ai-glib/mention/expand-truncates",
	                test_expand_truncates_and_says_so);
	g_test_add_func("/ai-glib/mention/expand-truncate-utf8",
	                test_expand_truncation_respects_characters);
	g_test_add_func("/ai-glib/mention/expand-budget",
	                test_expand_budget_is_shared);
	g_test_add_func("/ai-glib/mention/expand-not-reentrant",
	                test_expand_is_not_reentrant);
	g_test_add_func("/ai-glib/mention/expand-parent-paths",
	                test_expand_absolute_and_parent_paths);
	g_test_add_func("/ai-glib/mention/expand-null-out",
	                test_expand_out_files_may_be_null);
	g_test_add_func("/ai-glib/mention/expand-quoted",
	                test_expand_quoted_path_with_space);
	g_test_add_func("/ai-glib/mention/expand-unquoted-space",
	                test_expand_unquoted_space_stops_at_the_space);
	g_test_add_func("/ai-glib/mention/expand-large-directory",
	                test_expand_large_directory_is_capped);

	status = g_test_run();

	rm_rf(sandbox);
	g_free(sandbox);

	return status;
}
