/*
 * test-tool-multi-edit.c - Several edits to one file, all or nothing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The contract under test is a promise callers will rely on: if any edit
 * in the batch fails, the file is byte-for-byte what it was. Several tests
 * below therefore hash the file before and after a deliberate failure --
 * asserting "the tool returned an error" would pass just as happily
 * against an implementation that had already written half the batch.
 */

#include <ai-glib.h>

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

static gchar *
write_fixture(const gchar *name, const gchar *contents)
{
	gchar  *path = g_build_filename(sandbox, name, NULL);
	GError *error = NULL;

	g_file_set_contents(path, contents, -1, &error);
	g_assert_no_error(error);

	return path;
}

static gchar *
read_back(const gchar *path)
{
	gchar  *contents = NULL;
	GError *error = NULL;

	g_file_get_contents(path, &contents, NULL, &error);
	g_assert_no_error(error);

	return contents;
}

static gchar *
digest(const gchar *path)
{
	g_autofree gchar *contents = read_back(path);

	return g_compute_checksum_for_string(G_CHECKSUM_SHA256, contents, -1);
}

static gchar *
call_multi_edit(
	const gchar  *path,
	const gchar  *edits_json,
	GError      **error
){
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(JsonParser)     parser = json_parser_new();
	g_autoptr(AiToolUse)      use = NULL;
	g_autofree gchar         *json = NULL;
	GError                   *parse_error = NULL;

	json = g_strdup_printf("{\"path\":\"%s\",\"edits\":%s}", path,
	                       edits_json);

	g_assert_true(json_parser_load_from_data(parser, json, -1, &parse_error));
	g_assert_no_error(parse_error);

	use = ai_tool_use_new("id-1", "multi_edit", json_parser_get_root(parser));

	return ai_tool_executor_execute(executor, use, NULL, error);
}

/* ----------------------------------------------------------------
 * The ordinary path
 * ---------------------------------------------------------------- */

static void
test_applies_every_edit(void)
{
	g_autofree gchar *path = write_fixture("a.c", "alpha beta gamma\n");
	g_autofree gchar *result = NULL;
	g_autofree gchar *after = NULL;
	GError           *error = NULL;

	result = call_multi_edit(path,
		"[{\"old_string\":\"alpha\",\"new_string\":\"ALPHA\"},"
		"{\"old_string\":\"gamma\",\"new_string\":\"GAMMA\"}]", &error);

	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_nonnull(strstr(result, "2 edits"));

	after = read_back(path);
	g_assert_cmpstr(after, ==, "ALPHA beta GAMMA\n");
}

static void
test_edits_apply_in_order(void)
{
	g_autofree gchar *path = write_fixture("b.c", "one\n");
	g_autofree gchar *result = NULL;
	g_autofree gchar *after = NULL;

	/* The second edit operates on text the first produced, which is what
	 * "in order" has to mean for a batch to be useful. */
	result = call_multi_edit(path,
		"[{\"old_string\":\"one\",\"new_string\":\"two\"},"
		"{\"old_string\":\"two\",\"new_string\":\"three\"}]", NULL);

	g_assert_nonnull(result);
	after = read_back(path);
	g_assert_cmpstr(after, ==, "three\n");
}

static void
test_single_edit(void)
{
	g_autofree gchar *path = write_fixture("c.c", "x\n");
	g_autofree gchar *result = NULL;
	g_autofree gchar *after = NULL;

	result = call_multi_edit(path,
		"[{\"old_string\":\"x\",\"new_string\":\"y\"}]", NULL);

	g_assert_nonnull(result);
	g_assert_nonnull(strstr(result, "1 edit "));
	after = read_back(path);
	g_assert_cmpstr(after, ==, "y\n");
}

static void
test_multibyte_content(void)
{
	g_autofree gchar *path = write_fixture("d.c", "naïve café — dash\n");
	g_autofree gchar *result = NULL;
	g_autofree gchar *after = NULL;

	result = call_multi_edit(path,
		"[{\"old_string\":\"café\",\"new_string\":\"thé\"}]", NULL);

	g_assert_nonnull(result);
	after = read_back(path);
	g_assert_cmpstr(after, ==, "naïve thé — dash\n");
	g_assert_true(g_utf8_validate(after, -1, NULL));
}

static void
test_file_without_a_trailing_newline(void)
{
	g_autofree gchar *path = write_fixture("e.c", "no newline");
	g_autofree gchar *result = NULL;
	g_autofree gchar *after = NULL;

	result = call_multi_edit(path,
		"[{\"old_string\":\"no\",\"new_string\":\"still no\"}]", NULL);

	g_assert_nonnull(result);
	after = read_back(path);
	g_assert_cmpstr(after, ==, "still no newline");
}

static void
test_empty_replacement_deletes(void)
{
	g_autofree gchar *path = write_fixture("f.c", "keep DELETE keep\n");
	g_autofree gchar *result = NULL;
	g_autofree gchar *after = NULL;

	result = call_multi_edit(path,
		"[{\"old_string\":\"DELETE \",\"new_string\":\"\"}]", NULL);

	g_assert_nonnull(result);
	after = read_back(path);
	g_assert_cmpstr(after, ==, "keep keep\n");
}

/* ----------------------------------------------------------------
 * All-or-nothing
 * ---------------------------------------------------------------- */

static void
test_failure_leaves_the_file_untouched(void)
{
	g_autofree gchar *path =
		write_fixture("g.c", "one\ntwo\nthree\nfour\n");
	g_autofree gchar *before = digest(path);
	g_autofree gchar *after = NULL;
	gchar            *result;
	GError           *error = NULL;

	/*
	 * The third of four edits cannot match. An implementation that wrote
	 * as it went would leave "one" and "two" already replaced -- a state
	 * neither the model nor the user asked for, and worse than either
	 * end of the operation.
	 */
	result = call_multi_edit(path,
		"[{\"old_string\":\"one\",\"new_string\":\"1\"},"
		"{\"old_string\":\"two\",\"new_string\":\"2\"},"
		"{\"old_string\":\"MISSING\",\"new_string\":\"x\"},"
		"{\"old_string\":\"four\",\"new_string\":\"4\"}]", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "no edits were applied"));
	g_clear_error(&error);

	after = digest(path);
	g_assert_cmpstr(before, ==, after);
}

static void
test_ambiguous_match_is_refused(void)
{
	g_autofree gchar *path = write_fixture("h.c", "dup\nother\ndup\n");
	g_autofree gchar *before = digest(path);
	g_autofree gchar *after = NULL;
	gchar            *result;
	GError           *error = NULL;

	/*
	 * Deliberately stricter than `edit`, which takes the first match. In
	 * a batch the wrong match is buried among the right ones and the
	 * model cannot see what it did.
	 */
	result = call_multi_edit(path,
		"[{\"old_string\":\"dup\",\"new_string\":\"x\"}]", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "more than once"));
	g_clear_error(&error);

	after = digest(path);
	g_assert_cmpstr(before, ==, after);
}

static void
test_ambiguity_created_by_an_earlier_edit(void)
{
	g_autofree gchar *path = write_fixture("i.c", "a\nb\n");
	g_autofree gchar *before = digest(path);
	g_autofree gchar *after = NULL;
	gchar            *result;
	GError           *error = NULL;

	/* The first edit makes the second one's target ambiguous. Checking
	 * against the working copy rather than the original is what catches
	 * this. */
	result = call_multi_edit(path,
		"[{\"old_string\":\"b\",\"new_string\":\"a\"},"
		"{\"old_string\":\"a\",\"new_string\":\"c\"}]", &error);

	g_assert_null(result);
	g_assert_nonnull(strstr(error->message, "more than once"));
	g_clear_error(&error);

	after = digest(path);
	g_assert_cmpstr(before, ==, after);
}

/* ----------------------------------------------------------------
 * Malformed input
 * ---------------------------------------------------------------- */

static void
test_empty_edits_array(void)
{
	g_autofree gchar *path = write_fixture("j.c", "x\n");
	gchar            *result;
	GError           *error = NULL;

	result = call_multi_edit(path, "[]", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_edits_not_an_array(void)
{
	g_autofree gchar *path = write_fixture("k.c", "x\n");
	gchar            *result;
	GError           *error = NULL;

	result = call_multi_edit(path, "\"not an array\"", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_edit_missing_a_field(void)
{
	g_autofree gchar *path = write_fixture("l.c", "x\n");
	g_autofree gchar *before = digest(path);
	g_autofree gchar *after = NULL;
	gchar            *result;
	GError           *error = NULL;

	result = call_multi_edit(path,
		"[{\"old_string\":\"x\",\"new_string\":\"y\"},"
		"{\"old_string\":\"y\"}]", &error);

	g_assert_null(result);
	g_assert_nonnull(strstr(error->message, "edit 2"));
	g_clear_error(&error);

	/* And the first edit, which was fine, did not happen either. */
	after = digest(path);
	g_assert_cmpstr(before, ==, after);
}

static void
test_edit_field_of_the_wrong_type(void)
{
	g_autofree gchar *path = write_fixture("m.c", "x\n");
	gchar            *result;
	GError           *error = NULL;

	/* A number where a string was asked for: reported, never a critical,
	 * because a critical is fatal under G_DEBUG=fatal-warnings. */
	result = call_multi_edit(path,
		"[{\"old_string\":42,\"new_string\":\"y\"}]", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_edit_that_is_not_an_object(void)
{
	g_autofree gchar *path = write_fixture("n.c", "x\n");
	gchar            *result;
	GError           *error = NULL;

	result = call_multi_edit(path, "[\"a string\"]", &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
}

static void
test_empty_old_string_is_refused(void)
{
	g_autofree gchar *path = write_fixture("o.c", "x\n");
	gchar            *result;
	GError           *error = NULL;

	/* An empty old_string would match at offset zero every time; a
	 * caller who wants to create a file wants `write`. */
	result = call_multi_edit(path,
		"[{\"old_string\":\"\",\"new_string\":\"y\"}]", &error);

	g_assert_null(result);
	g_assert_nonnull(strstr(error->message, "write"));
	g_clear_error(&error);
}

static void
test_missing_file(void)
{
	g_autofree gchar *path = g_build_filename(sandbox, "nope.c", NULL);
	gchar            *result;
	GError           *error = NULL;

	result = call_multi_edit(path,
		"[{\"old_string\":\"x\",\"new_string\":\"y\"}]", &error);

	g_assert_null(result);
	g_assert_nonnull(error);
	g_clear_error(&error);
}

static void
test_missing_path(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(JsonParser)     parser = json_parser_new();
	g_autoptr(AiToolUse)      use = NULL;
	gchar                    *result;
	GError                   *error = NULL;

	g_assert_true(json_parser_load_from_data(parser, "{\"edits\":[]}", -1,
	                                          NULL));
	use = ai_tool_use_new("id-1", "multi_edit", json_parser_get_root(parser));

	result = ai_tool_executor_execute(executor, use, NULL, &error);

	g_assert_null(result);
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);
}

static void
test_relative_path_uses_the_working_directory(void)
{
	g_autoptr(AiToolExecutor) executor = ai_tool_executor_new();
	g_autoptr(JsonParser)     parser = json_parser_new();
	g_autoptr(AiToolUse)      use = NULL;
	g_autofree gchar         *path = write_fixture("relative.c", "old\n");
	g_autofree gchar         *result = NULL;
	g_autofree gchar         *after = NULL;
	GError                   *error = NULL;

	ai_tool_executor_set_working_directory(executor, sandbox);

	g_assert_true(json_parser_load_from_data(parser,
		"{\"path\":\"relative.c\",\"edits\":"
		"[{\"old_string\":\"old\",\"new_string\":\"new\"}]}", -1, NULL));
	use = ai_tool_use_new("id-1", "multi_edit", json_parser_get_root(parser));

	result = ai_tool_executor_execute(executor, use, NULL, &error);

	g_assert_no_error(error);
	g_assert_nonnull(result);

	after = read_back(path);
	g_assert_cmpstr(after, ==, "new\n");
}

int
main(int argc, char *argv[])
{
	GError *error = NULL;
	int     status;

	g_test_init(&argc, &argv, NULL);

	sandbox = g_dir_make_tmp("ai-glib-multiedit-XXXXXX", &error);
	g_assert_no_error(error);

	g_test_add_func("/ai-glib/multi-edit/applies-all",
	                test_applies_every_edit);
	g_test_add_func("/ai-glib/multi-edit/in-order", test_edits_apply_in_order);
	g_test_add_func("/ai-glib/multi-edit/single", test_single_edit);
	g_test_add_func("/ai-glib/multi-edit/multibyte", test_multibyte_content);
	g_test_add_func("/ai-glib/multi-edit/no-trailing-newline",
	                test_file_without_a_trailing_newline);
	g_test_add_func("/ai-glib/multi-edit/deletion",
	                test_empty_replacement_deletes);

	g_test_add_func("/ai-glib/multi-edit/all-or-nothing",
	                test_failure_leaves_the_file_untouched);
	g_test_add_func("/ai-glib/multi-edit/ambiguous",
	                test_ambiguous_match_is_refused);
	g_test_add_func("/ai-glib/multi-edit/ambiguity-created",
	                test_ambiguity_created_by_an_earlier_edit);

	g_test_add_func("/ai-glib/multi-edit/empty-array", test_empty_edits_array);
	g_test_add_func("/ai-glib/multi-edit/not-an-array",
	                test_edits_not_an_array);
	g_test_add_func("/ai-glib/multi-edit/missing-field",
	                test_edit_missing_a_field);
	g_test_add_func("/ai-glib/multi-edit/wrong-type",
	                test_edit_field_of_the_wrong_type);
	g_test_add_func("/ai-glib/multi-edit/not-an-object",
	                test_edit_that_is_not_an_object);
	g_test_add_func("/ai-glib/multi-edit/empty-old-string",
	                test_empty_old_string_is_refused);
	g_test_add_func("/ai-glib/multi-edit/missing-file", test_missing_file);
	g_test_add_func("/ai-glib/multi-edit/missing-path", test_missing_path);
	g_test_add_func("/ai-glib/multi-edit/relative-path",
	                test_relative_path_uses_the_working_directory);

	status = g_test_run();

	rm_rf(sandbox);
	g_free(sandbox);

	return status;
}
