/*
 * test-view-todo-block.c - Rendering the todo list
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The rendered strings are asserted literally, in the manner of
 * test-view-tool-block.c: if the wording changes, these change with it,
 * deliberately and visibly. The other half of the file is the in-place
 * rule -- a model revises its plan repeatedly over a long task, and the
 * difference between one block that updates and nine that accumulate is
 * the difference between a readable transcript and an unreadable one.
 */

#include <ai-glib.h>

#include <string.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static GPtrArray *
todo_list(void)
{
	return g_ptr_array_new_with_free_func((GDestroyNotify)ai_todo_free);
}

static void
push(GPtrArray *list, const gchar *content, const gchar *active,
     AiTodoState state)
{
	g_ptr_array_add(list, ai_todo_new(content, active, state));
}

static gchar *
render(AiViewTodoBlock *block, guint width)
{
	g_autoptr(AiRenderedText) rendered =
		ai_view_block_render(AI_VIEW_BLOCK(block), width);

	return g_strdup(ai_rendered_text_get_text(rendered));
}

/* ----------------------------------------------------------------
 * The strings
 * ---------------------------------------------------------------- */

static void
test_empty_block(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autofree gchar          *text = render(block, 0);

	/* Not nothing: an empty block that rendered to "" would be an
	 * invisible row in the transcript. */
	g_assert_cmpstr(text, ==, "No todos");
	g_assert_cmpuint(ai_view_todo_block_get_n_todos(block), ==, 0);
}

static void
test_renders_each_state(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *text = NULL;

	push(list, "Add the parser", "Adding the parser", AI_TODO_PENDING);
	push(list, "Wire the registry", "Wiring the registry",
	     AI_TODO_IN_PROGRESS);
	push(list, "Read the files", "Reading the files", AI_TODO_COMPLETED);

	ai_view_todo_block_set_todos(block, list);
	text = render(block, 0);

	g_assert_cmpstr(text, ==,
	                "☐ Add the parser\n"
	                "◐ Wiring the registry\n"
	                "☑ Read the files\n"
	                "1/3 done");
}

static void
test_single_item(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *text = NULL;

	push(list, "Just the one", NULL, AI_TODO_PENDING);
	ai_view_todo_block_set_todos(block, list);
	text = render(block, 0);

	g_assert_cmpstr(text, ==, "☐ Just the one\n0/1 done");
}

static void
test_all_complete(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *text = NULL;

	push(list, "One", NULL, AI_TODO_COMPLETED);
	push(list, "Two", NULL, AI_TODO_COMPLETED);
	ai_view_todo_block_set_todos(block, list);
	text = render(block, 0);

	g_assert_cmpstr(text, ==, "☑ One\n☑ Two\n2/2 done");
}

static void
test_active_form_only_while_active(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *text = NULL;

	/* Same item, two states. The active phrasing is not a label, it is a
	 * tense. */
	push(list, "Add it", "Adding it", AI_TODO_PENDING);
	push(list, "Add it", "Adding it", AI_TODO_IN_PROGRESS);
	push(list, "Add it", "Adding it", AI_TODO_COMPLETED);

	ai_view_todo_block_set_todos(block, list);
	text = render(block, 0);

	g_assert_cmpstr(text, ==,
	                "☐ Add it\n◐ Adding it\n☑ Add it\n1/3 done");
}

static void
test_missing_active_form(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *text = NULL;

	push(list, "Add it", NULL, AI_TODO_IN_PROGRESS);
	ai_view_todo_block_set_todos(block, list);
	text = render(block, 0);

	g_assert_cmpstr(text, ==, "◐ Add it\n0/1 done");
}

/* ----------------------------------------------------------------
 * Spans
 * ---------------------------------------------------------------- */

static void
test_spans_are_tagged_per_state(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autoptr(AiRenderedText)  rendered = NULL;
	guint                      i;
	gboolean                   saw_pending = FALSE;
	gboolean                   saw_active = FALSE;
	gboolean                   saw_done = FALSE;

	push(list, "A", NULL, AI_TODO_PENDING);
	push(list, "B", NULL, AI_TODO_IN_PROGRESS);
	push(list, "C", NULL, AI_TODO_COMPLETED);
	ai_view_todo_block_set_todos(block, list);

	rendered = ai_view_block_render(AI_VIEW_BLOCK(block), 0);

	for (i = 0; i < ai_rendered_text_get_n_spans(rendered); i++)
	{
		guint      start = 0;
		guint      len = 0;
		AiStyleTag tag = AI_STYLE_DEFAULT;

		g_assert_true(ai_rendered_text_get_span(rendered, i, &start, &len,
		                                        &tag));

		switch (tag)
		{
			case AI_STYLE_TODO_PENDING:
				saw_pending = TRUE;
				break;
			case AI_STYLE_TODO_ACTIVE:
				saw_active = TRUE;
				break;
			case AI_STYLE_TODO_DONE:
				saw_done = TRUE;
				break;
			default:
				break;
		}
	}

	/* Three roles, so a frontend can colour them differently without
	 * parsing the glyphs back out of the text. */
	g_assert_true(saw_pending);
	g_assert_true(saw_active);
	g_assert_true(saw_done);
}

static void
test_span_offsets_are_bytes(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autoptr(AiRenderedText)  rendered = NULL;
	const gchar               *text;
	guint                      i;

	/* The markers are themselves multibyte, and so is the content: every
	 * span boundary here is a chance to split a character. */
	push(list, "café", NULL, AI_TODO_PENDING);
	push(list, "日本語のタスク", NULL, AI_TODO_COMPLETED);
	ai_view_todo_block_set_todos(block, list);

	rendered = ai_view_block_render(AI_VIEW_BLOCK(block), 0);
	text = ai_rendered_text_get_text(rendered);

	g_assert_true(g_utf8_validate(text, -1, NULL));

	for (i = 0; i < ai_rendered_text_get_n_spans(rendered); i++)
	{
		guint      start = 0;
		guint      len = 0;
		AiStyleTag tag = AI_STYLE_DEFAULT;

		ai_rendered_text_get_span(rendered, i, &start, &len, &tag);

		g_assert_cmpuint(start + len, <=, strlen(text));
		g_assert_true(g_utf8_validate(text + start, len, NULL));
	}
}

/* ----------------------------------------------------------------
 * Wrapping
 * ---------------------------------------------------------------- */

static void
test_long_content_wraps(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *narrow = NULL;
	g_autofree gchar          *wide = NULL;

	push(list,
	     "Rewrite the entire provider layer so that every wire format "
	     "round-trips through one serializer instead of five", NULL,
	     AI_TODO_PENDING);
	ai_view_todo_block_set_todos(block, list);

	wide = render(block, 0);
	narrow = render(block, 30);

	/* Width 0 means do not wrap, which is what Emacs wants. */
	{
		g_auto(GStrv) wide_lines = g_strsplit(wide, "\n", -1);
		g_auto(GStrv) narrow_lines = g_strsplit(narrow, "\n", -1);

		g_assert_cmpint(g_strv_length(wide_lines), ==, 2);
		g_assert_cmpint(g_strv_length(narrow_lines), >, 2);
	}
}

static void
test_render_cache_survives_repeated_calls(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *first = NULL;
	g_autofree gchar          *second = NULL;

	push(list, "Stable", NULL, AI_TODO_PENDING);
	ai_view_todo_block_set_todos(block, list);

	first = render(block, 40);
	second = render(block, 40);

	g_assert_cmpstr(first, ==, second);
}

/* ----------------------------------------------------------------
 * In-place mutation
 * ---------------------------------------------------------------- */

typedef struct
{
	guint items_changed;
	guint block_changed;
} Counters;

static void
on_items_changed(GListModel *model, guint position, guint removed,
                 guint added, gpointer user_data)
{
	Counters *c = user_data;

	c->items_changed++;
}

static void
on_block_changed(AiTranscript *transcript, guint position,
                 AiViewBlock *block, gpointer user_data)
{
	Counters *c = user_data;

	c->block_changed++;
}

static void
test_updates_in_place(void)
{
	g_autoptr(AiTranscript)    transcript = ai_transcript_new();
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       first = todo_list();
	g_autoptr(GPtrArray)       second = todo_list();
	Counters                   counters = { 0, 0 };

	ai_transcript_append(transcript, AI_VIEW_BLOCK(block));

	g_signal_connect(transcript, "items-changed",
	                 G_CALLBACK(on_items_changed), &counters);
	g_signal_connect(transcript, "block-changed",
	                 G_CALLBACK(on_block_changed), &counters);

	push(first, "One", NULL, AI_TODO_PENDING);
	ai_view_todo_block_set_todos(block, first);

	push(second, "One", NULL, AI_TODO_COMPLETED);
	push(second, "Two", NULL, AI_TODO_IN_PROGRESS);
	ai_view_todo_block_set_todos(block, second);

	/*
	 * Two updates, no new rows. This is the assertion that keeps the
	 * transcript readable: ::items-changed would mean a second block
	 * appeared, and eight revisions of a nine-line plan would bury the
	 * conversation.
	 */
	g_assert_cmpuint(counters.items_changed, ==, 0);
	g_assert_cmpuint(counters.block_changed, ==, 2);
	g_assert_cmpuint(ai_transcript_get_n_blocks(transcript), ==, 1);
	g_assert_cmpuint(ai_view_todo_block_get_n_todos(block), ==, 2);
}

static void
test_items_are_copied(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *text = NULL;

	push(list, "Mine", NULL, AI_TODO_PENDING);
	ai_view_todo_block_set_todos(block, list);

	/*
	 * The executor replaces its own array on the next todo_write. If the
	 * block held those pointers, the next call would pull the rug from
	 * under whatever is on screen.
	 */
	g_ptr_array_set_size(list, 0);

	text = render(block, 0);
	g_assert_cmpstr(text, ==, "☐ Mine\n0/1 done");
}

static void
test_set_null_empties(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();
	g_autoptr(GPtrArray)       list = todo_list();
	g_autofree gchar          *text = NULL;

	push(list, "One", NULL, AI_TODO_PENDING);
	ai_view_todo_block_set_todos(block, list);
	ai_view_todo_block_set_todos(block, NULL);

	text = render(block, 0);
	g_assert_cmpstr(text, ==, "No todos");
}

static void
test_block_kind(void)
{
	g_autoptr(AiViewTodoBlock) block = ai_view_todo_block_new();

	g_assert_cmpint(ai_view_block_get_kind(AI_VIEW_BLOCK(block)), ==,
	                AI_VIEW_BLOCK_TODO);
	g_assert_cmpuint(ai_view_block_get_id(AI_VIEW_BLOCK(block)), >, 0);
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/todo-block/empty", test_empty_block);
	g_test_add_func("/ai-glib/todo-block/states", test_renders_each_state);
	g_test_add_func("/ai-glib/todo-block/single", test_single_item);
	g_test_add_func("/ai-glib/todo-block/all-complete", test_all_complete);
	g_test_add_func("/ai-glib/todo-block/active-form",
	                test_active_form_only_while_active);
	g_test_add_func("/ai-glib/todo-block/no-active-form",
	                test_missing_active_form);

	g_test_add_func("/ai-glib/todo-block/spans",
	                test_spans_are_tagged_per_state);
	g_test_add_func("/ai-glib/todo-block/span-offsets",
	                test_span_offsets_are_bytes);

	g_test_add_func("/ai-glib/todo-block/wrapping", test_long_content_wraps);
	g_test_add_func("/ai-glib/todo-block/cache",
	                test_render_cache_survives_repeated_calls);

	g_test_add_func("/ai-glib/todo-block/in-place", test_updates_in_place);
	g_test_add_func("/ai-glib/todo-block/copies", test_items_are_copied);
	g_test_add_func("/ai-glib/todo-block/set-null", test_set_null_empties);
	g_test_add_func("/ai-glib/todo-block/kind", test_block_kind);

	return g_test_run();
}
