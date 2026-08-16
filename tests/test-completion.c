/*
 * test-completion.c - Completing / and @ at a cursor
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The replacement range is the product here, not the candidate list. A
 * frontend -- ncurses today, `completion-at-point-functions` tomorrow --
 * replaces buffer[start..end) with the chosen text and does nothing else,
 * so a range that is off by one byte corrupts the line. Most of what
 * follows is therefore about offsets, and about the multibyte cases where
 * getting them wrong is easiest.
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

static void
make_tree(void)
{
	static const gchar *files[] = {
		"src/core/ai-event.c", "src/core/ai-event.h",
		"src/core/ai-client.c", "src/view/ai-style.c",
		"README.org", ".hidden", ".git/config",
		"node_modules/pkg/index.js",
		NULL
	};
	gsize i;

	for (i = 0; files[i] != NULL; i++)
	{
		g_autofree gchar *path = g_build_filename(sandbox, files[i], NULL);
		g_autofree gchar *dir = g_path_get_dirname(path);
		GError           *error = NULL;

		g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);
		g_file_set_contents(path, "x\n", -1, &error);
		g_assert_no_error(error);
	}
}

static AiCompletionContext *
context_with_commands(void)
{
	g_autoptr(AiResourceRegistry) registry = ai_resource_registry_new();
	g_autoptr(AiCommandSet)       set = NULL;
	static const gchar           *names[] = {
		"skill-gtest-scaffold", "skill-gobject-type", "deploy", NULL
	};
	gsize                         i;

	for (i = 0; names[i] != NULL; i++)
	{
		g_autoptr(AiResource) resource =
			ai_resource_new_from_data("---\ndescription: A thing\n---\nbody\n",
			                          -1, names[i], AI_RESOURCE_COMMAND,
			                          "claude", AI_RESOURCE_SCOPE_USER, NULL);

		ai_resource_registry_add(registry, resource);
	}

	set = ai_command_set_new(registry);

	return ai_completion_context_new(set, sandbox);
}

/* Does the result contain a candidate whose insert text is @text? */
static gboolean
has_item(AiCompletionResult *result, const gchar *text)
{
	guint i;

	for (i = 0; i < ai_completion_result_get_n_items(result); i++)
	{
		const AiCompletionItem *item =
			ai_completion_result_get_item(result, i);

		if (g_strcmp0(item->text, text) == 0)
		{
			return TRUE;
		}
	}

	return FALSE;
}

/* ----------------------------------------------------------------
 * Nothing to complete
 * ---------------------------------------------------------------- */

static void
test_empty_buffer(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "", 0);

	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_NONE);
	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 0);
	g_assert_null(ai_completion_result_get_common_prefix(r));
}

static void
test_null_buffer(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, NULL, 0);

	/* Never NULL: a caller should not have to test the result before
	 * asking it how many items it has. */
	g_assert_nonnull(r);
	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_NONE);
}

static void
test_plain_text(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, "just some words", 15);

	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_NONE);
}

static void
test_cursor_past_the_end_is_clamped(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/dep", 9999);

	/* A frontend bug is not a reason to read out of bounds. Under ASAN
	 * this case is the one that would catch it. */
	g_assert_cmpuint(ai_completion_result_get_end(r), ==, 4);
	g_assert_true(has_item(r, "deploy"));
}

/* ----------------------------------------------------------------
 * Commands
 * ---------------------------------------------------------------- */

static void
test_slash_at_start(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/dep", 4);

	g_assert_cmpint(ai_completion_result_get_kind(r), ==,
	                AI_COMPLETION_COMMAND);

	/* The range covers the name and not the slash, so inserting the
	 * candidate leaves "/deploy" rather than "deploy" or "//deploy". */
	g_assert_cmpuint(ai_completion_result_get_start(r), ==, 1);
	g_assert_cmpuint(ai_completion_result_get_end(r), ==, 4);
	g_assert_true(has_item(r, "deploy"));
}

static void
test_bare_slash_offers_everything(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/", 1);

	g_assert_cmpint(ai_completion_result_get_kind(r), ==,
	                AI_COMPLETION_COMMAND);
	g_assert_cmpuint(ai_completion_result_get_n_items(r), >, 10);
	g_assert_true(has_item(r, "help"));
	g_assert_true(has_item(r, "deploy"));
}

static void
test_slash_not_at_start_does_not_complete(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, "text /dep", 9);

	/* A slash mid-line is a path separator or prose, never a command. */
	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_NONE);
}

static void
test_no_completion_past_the_command_name(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, "/deploy production", 18);

	/* The name is settled; what follows is arguments. */
	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_NONE);
}

static void
test_command_candidates_carry_their_origin(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/deploy", 7);
	const AiCompletionItem        *item;

	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 1);
	item = ai_completion_result_get_item(r, 0);

	/* When two harnesses both define "review", the only useful thing a
	 * menu can say is which one this is. */
	g_assert_nonnull(strstr(item->description, "claude"));
	g_assert_cmpint(item->kind, ==, AI_COMPLETION_COMMAND);
	g_assert_false(item->is_directory);
}

static void
test_no_matching_command(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/zzz", 4);

	/* Kind is still COMMAND -- the user is typing a command, there is
	 * just nothing to offer. A frontend needs that difference to decide
	 * between "no menu" and "no matches". */
	g_assert_cmpint(ai_completion_result_get_kind(r), ==,
	                AI_COMPLETION_COMMAND);
	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 0);
}

static void
test_common_prefix(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/skill-g", 8);
	g_autofree gchar              *prefix = NULL;

	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 2);

	/* One Tab inserts as much as is certain and shows the menu, instead
	 * of making the user choose between two things that agree so far. */
	prefix = ai_completion_result_get_common_prefix(r);
	g_assert_cmpstr(prefix, ==, "skill-g");
}

static void
test_common_prefix_of_one(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/deplo", 6);
	g_autofree gchar              *prefix =
		ai_completion_result_get_common_prefix(r);

	g_assert_cmpstr(prefix, ==, "deploy");
}

static void
test_context_without_commands(void)
{
	g_autoptr(AiCompletionContext) ctx =
		ai_completion_context_new(NULL, sandbox);
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "/h", 2);

	/* No command set is legal; paths still complete. */
	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 0);
}

/* ----------------------------------------------------------------
 * Paths
 * ---------------------------------------------------------------- */

static void
test_path_at_start(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "@RE", 3);

	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_PATH);
	g_assert_cmpuint(ai_completion_result_get_start(r), ==, 1);
	g_assert_cmpuint(ai_completion_result_get_end(r), ==, 3);
	g_assert_true(has_item(r, "README.org"));
}

static void
test_path_mid_line(void)
{
	const gchar                   *buffer = "please read @src";
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, buffer, (guint)strlen(buffer));

	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_PATH);
	g_assert_cmpuint(ai_completion_result_get_start(r), ==, 13);
	g_assert_cmpuint(buffer[ai_completion_result_get_start(r) - 1], ==, '@');
	g_assert_true(has_item(r, "src/"));
}

static void
test_directories_get_a_trailing_slash(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "@s", 2);
	const AiCompletionItem        *item;

	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 1);
	item = ai_completion_result_get_item(r, 0);

	/* One Tab walks into the directory instead of stopping at its name. */
	g_assert_cmpstr(item->text, ==, "src/");
	g_assert_true(item->is_directory);
	g_assert_cmpstr(item->display, ==, "src");
}

static void
test_path_with_a_directory_component(void)
{
	const gchar                   *buffer = "@src/core/ai-ev";
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, buffer, (guint)strlen(buffer));

	/*
	 * The range covers everything after the `@`, directory component
	 * included, and the candidate carries the whole path -- so a frontend
	 * replaces the path rather than splicing a fragment into the middle
	 * of it.
	 */
	g_assert_cmpuint(ai_completion_result_get_start(r), ==, 1);
	g_assert_cmpuint(ai_completion_result_get_end(r), ==,
	                 (guint)strlen(buffer));
	g_assert_true(has_item(r, "src/core/ai-event.c"));
	g_assert_true(has_item(r, "src/core/ai-event.h"));
	g_assert_false(has_item(r, "ai-event.c"));
}

static void
test_directories_sort_first(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "@", 1);
	guint                          i;
	gboolean                       saw_file = FALSE;

	g_assert_cmpuint(ai_completion_result_get_n_items(r), >, 1);

	for (i = 0; i < ai_completion_result_get_n_items(r); i++)
	{
		const AiCompletionItem *item =
			ai_completion_result_get_item(r, i);

		if (!item->is_directory)
		{
			saw_file = TRUE;
		}
		else
		{
			/* Completing a path is usually a walk down, and the next
			 * step is nearly always a directory. */
			g_assert_false(saw_file);
		}
	}
}

static void
test_hidden_files_need_the_dot(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  bare = ai_completion_query(ctx, "@", 1);
	g_autoptr(AiCompletionResult)  dotted = ai_completion_query(ctx, "@.", 2);

	g_assert_false(has_item(bare, ".hidden"));

	/* Typing the dot is the commitment. */
	g_assert_true(has_item(dotted, ".hidden"));
}

static void
test_noise_directories_are_skipped(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  dotted = ai_completion_query(ctx, "@.", 2);
	g_autoptr(AiCompletionResult)  node = ai_completion_query(ctx, "@n", 2);

	/* .git is hidden anyway, but it stays out even when the dot is
	 * typed; node_modules is neither hidden nor ever the answer. */
	g_assert_false(has_item(dotted, ".git/"));
	g_assert_cmpuint(ai_completion_result_get_n_items(node), ==, 0);
}

static void
test_nonexistent_directory_is_quiet(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, "@no/such/place/x", 16);

	/* Half-typed paths name directories that do not exist yet; that is
	 * the normal case, not a problem. */
	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_PATH);
	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 0);
}

static void
test_at_in_an_email_does_not_complete(void)
{
	const gchar                   *buffer = "mail z.podbielniak@gmail";
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, buffer, (guint)strlen(buffer));

	/* Same boundary rule the scanner uses, applied at the cursor. */
	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_NONE);
}

/* ----------------------------------------------------------------
 * Offsets under multibyte text
 * ---------------------------------------------------------------- */

static void
test_multibyte_before_the_cursor(void)
{
	/* "héllo" is six bytes and five characters. A range computed in
	 * characters would be one short here. */
	const gchar                   *buffer = "héllo @src";
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r =
		ai_completion_query(ctx, buffer, (guint)strlen(buffer));

	g_assert_cmpuint(ai_completion_result_get_start(r), ==, 8);
	g_assert_cmpint(buffer[ai_completion_result_get_start(r) - 1], ==, '@');
	g_assert_cmpuint(ai_completion_result_get_end(r), ==,
	                 (guint)strlen(buffer));
}

static void
test_common_prefix_never_splits_a_character(void)
{
	g_autoptr(AiCompletionContext) ctx = NULL;
	g_autoptr(AiCompletionResult)  r = NULL;
	g_autofree gchar              *prefix = NULL;
	g_autofree gchar              *dir = NULL;

	/* Two names agreeing on the first byte of a multibyte character but
	 * not the second: a byte-counted prefix would cut it in half, and a
	 * frontend inserts this verbatim. */
	dir = g_build_filename(sandbox, "wide", NULL);
	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);

	{
		const gchar *names[] = { "wide/日本.txt", "wide/日曜.txt", NULL };
		gsize        i;

		for (i = 0; names[i] != NULL; i++)
		{
			g_autofree gchar *path =
				g_build_filename(sandbox, names[i], NULL);
			GError           *error = NULL;

			g_file_set_contents(path, "x\n", -1, &error);
			g_assert_no_error(error);
		}
	}

	ctx = context_with_commands();
	r = ai_completion_query(ctx, "@wide/", 6);

	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 2);

	prefix = ai_completion_result_get_common_prefix(r);
	g_assert_nonnull(prefix);
	g_assert_true(g_utf8_validate(prefix, -1, NULL));
}

/* ----------------------------------------------------------------
 * Bounds and bindings
 * ---------------------------------------------------------------- */

static void
test_max_items_is_enforced(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = NULL;
	g_autofree gchar              *dir = NULL;
	guint                          i;

	dir = g_build_filename(sandbox, "many", NULL);
	g_assert_cmpint(g_mkdir_with_parents(dir, 0755), ==, 0);

	for (i = 0; i < 500; i++)
	{
		g_autofree gchar *name = g_strdup_printf("f-%04u.txt", i);
		g_autofree gchar *path = g_build_filename(dir, name, NULL);
		GError           *error = NULL;

		g_file_set_contents(path, "x\n", -1, &error);
		g_assert_no_error(error);
	}

	/* This runs on a keystroke. A directory of forty thousand files must
	 * not turn one into a pause. */
	ai_completion_context_set_max_items(ctx, 25);
	g_assert_cmpuint(ai_completion_context_get_max_items(ctx), ==, 25);

	r = ai_completion_query(ctx, "@many/f-", 8);
	g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 25);
}

static void
test_large_buffer(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(GString)             big = g_string_new(NULL);
	g_autoptr(AiCompletionResult)  r = NULL;
	guint                          i;

	for (i = 0; i < 20000; i++)
	{
		g_string_append(big, "word ");
	}

	g_string_append(big, "@src");

	r = ai_completion_query(ctx, big->str, (guint)big->len);

	g_assert_cmpint(ai_completion_result_get_kind(r), ==, AI_COMPLETION_PATH);
	g_assert_true(has_item(r, "src/"));
}

static void
test_item_fields_out_parameters(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	g_autoptr(AiCompletionResult)  r = ai_completion_query(ctx, "@RE", 3);
	const gchar                   *text = NULL;
	const gchar                   *display = NULL;
	const gchar                   *description = NULL;
	gboolean                       is_directory = TRUE;

	/*
	 * The accessor bindings use, for the same reason
	 * ai_rendered_text_get_span() exists. If this shape breaks, the
	 * Emacs path breaks with it.
	 */
	g_assert_true(ai_completion_result_get_item_fields(
		r, 0, &text, &display, &description, &is_directory));
	g_assert_cmpstr(text, ==, "README.org");
	g_assert_cmpstr(display, ==, "README.org");
	g_assert_null(description);
	g_assert_false(is_directory);

	/* Out of range leaves the outputs untouched. */
	text = (const gchar *)0x1;
	g_assert_false(ai_completion_result_get_item_fields(
		r, 99, &text, NULL, NULL, NULL));
	g_assert_true(text == (const gchar *)0x1);

	g_assert_null(ai_completion_result_get_item(r, 99));
	g_assert_true(ai_completion_result_get_item_fields(r, 0, NULL, NULL,
	                                                   NULL, NULL));
}

static void
test_item_boxed_type(void)
{
	AiCompletionItem  item = { NULL, NULL, NULL, AI_COMPLETION_PATH, TRUE };
	AiCompletionItem *copy;

	item.text = g_strdup("a");
	item.display = g_strdup("b");
	item.description = g_strdup("c");

	copy = ai_completion_item_copy(&item);
	g_assert_cmpstr(copy->text, ==, "a");
	g_assert_cmpstr(copy->description, ==, "c");
	g_assert_true(copy->is_directory);
	ai_completion_item_free(copy);

	g_assert_null(ai_completion_item_copy(NULL));
	ai_completion_item_free(NULL);

	g_assert_true(AI_TYPE_COMPLETION_ITEM != G_TYPE_INVALID);

	g_free(item.text);
	g_free(item.display);
	g_free(item.description);
}

static void
test_context_properties(void)
{
	g_autoptr(AiCompletionContext) ctx =
		ai_completion_context_new(NULL, NULL);
	g_autofree gchar              *cwd = NULL;
	guint                          max_items = 0;

	g_object_get(ctx, "working-directory", &cwd, "max-items", &max_items,
	             NULL);

	g_assert_nonnull(cwd);
	g_assert_cmpuint(max_items, >, 0);

	ai_completion_context_set_working_directory(ctx, sandbox);
	g_assert_cmpstr(ai_completion_context_get_working_directory(ctx), ==,
	                sandbox);
}

static void
test_repeated_queries_are_clean(void)
{
	g_autoptr(AiCompletionContext) ctx = context_with_commands();
	guint                          i;

	/* One allocation set per keystroke; this is the case ASAN watches. */
	for (i = 0; i < 200; i++)
	{
		g_autoptr(AiCompletionResult) r =
			ai_completion_query(ctx, "@src/core/ai-", 13);

		g_assert_cmpuint(ai_completion_result_get_n_items(r), ==, 3);
	}
}

int
main(int argc, char *argv[])
{
	GError *error = NULL;
	int     status;

	g_test_init(&argc, &argv, NULL);

	sandbox = g_dir_make_tmp("ai-glib-completion-XXXXXX", &error);
	g_assert_no_error(error);
	make_tree();

	g_test_add_func("/ai-glib/completion/empty", test_empty_buffer);
	g_test_add_func("/ai-glib/completion/null", test_null_buffer);
	g_test_add_func("/ai-glib/completion/plain-text", test_plain_text);
	g_test_add_func("/ai-glib/completion/cursor-clamped",
	                test_cursor_past_the_end_is_clamped);

	g_test_add_func("/ai-glib/completion/slash-at-start", test_slash_at_start);
	g_test_add_func("/ai-glib/completion/bare-slash",
	                test_bare_slash_offers_everything);
	g_test_add_func("/ai-glib/completion/slash-mid-line",
	                test_slash_not_at_start_does_not_complete);
	g_test_add_func("/ai-glib/completion/past-command-name",
	                test_no_completion_past_the_command_name);
	g_test_add_func("/ai-glib/completion/command-origin",
	                test_command_candidates_carry_their_origin);
	g_test_add_func("/ai-glib/completion/no-match", test_no_matching_command);
	g_test_add_func("/ai-glib/completion/common-prefix", test_common_prefix);
	g_test_add_func("/ai-glib/completion/common-prefix-one",
	                test_common_prefix_of_one);
	g_test_add_func("/ai-glib/completion/no-command-set",
	                test_context_without_commands);

	g_test_add_func("/ai-glib/completion/path-at-start", test_path_at_start);
	g_test_add_func("/ai-glib/completion/path-mid-line", test_path_mid_line);
	g_test_add_func("/ai-glib/completion/trailing-slash",
	                test_directories_get_a_trailing_slash);
	g_test_add_func("/ai-glib/completion/path-with-directory",
	                test_path_with_a_directory_component);
	g_test_add_func("/ai-glib/completion/directories-first",
	                test_directories_sort_first);
	g_test_add_func("/ai-glib/completion/hidden", test_hidden_files_need_the_dot);
	g_test_add_func("/ai-glib/completion/noise-dirs",
	                test_noise_directories_are_skipped);
	g_test_add_func("/ai-glib/completion/nonexistent-dir",
	                test_nonexistent_directory_is_quiet);
	g_test_add_func("/ai-glib/completion/email",
	                test_at_in_an_email_does_not_complete);

	g_test_add_func("/ai-glib/completion/multibyte-offsets",
	                test_multibyte_before_the_cursor);
	g_test_add_func("/ai-glib/completion/prefix-utf8",
	                test_common_prefix_never_splits_a_character);

	g_test_add_func("/ai-glib/completion/max-items", test_max_items_is_enforced);
	g_test_add_func("/ai-glib/completion/large-buffer", test_large_buffer);
	g_test_add_func("/ai-glib/completion/item-fields",
	                test_item_fields_out_parameters);
	g_test_add_func("/ai-glib/completion/item-boxed", test_item_boxed_type);
	g_test_add_func("/ai-glib/completion/properties", test_context_properties);
	g_test_add_func("/ai-glib/completion/repeated", test_repeated_queries_are_clean);

	status = g_test_run();

	rm_rf(sandbox);
	g_free(sandbox);

	return status;
}
