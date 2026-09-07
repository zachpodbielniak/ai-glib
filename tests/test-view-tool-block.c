/*
 * test-view-tool-block.c - The tool-group summariser
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The highest-value tests in the view layer. Everything else models a
 * conversation; this file pins the wording a reader actually sees, which is
 * the thing the whole layer exists to get right once instead of once per
 * frontend.
 */

#include <ai-glib.h>

/* ----------------------------------------------------------------
 * Harness
 * ---------------------------------------------------------------- */

static AiToolUse *
tool_use(const gchar *id, const gchar *name, const gchar *input_json)
{
	return ai_tool_use_new_from_json_string(id, name,
	                                        input_json != NULL ? input_json : "{}");
}

/* Add a call and mark it succeeded, which is the ordinary case. */
static AiToolCall *
add_ok(AiViewToolBlock *block, const gchar *id, const gchar *name,
       const gchar *input_json)
{
	g_autoptr(AiToolUse) tu = tool_use(id, name, input_json);
	AiToolCall *call = ai_view_tool_block_add_call(block, tu);
	g_autoptr(AiToolResult) result = ai_tool_result_new(id, "done", FALSE);

	ai_tool_call_finish(call, result);

	return call;
}

static AiToolCall *
add_failed(AiViewToolBlock *block, const gchar *id, const gchar *name,
           const gchar *input_json)
{
	g_autoptr(AiToolUse) tu = tool_use(id, name, input_json);
	AiToolCall *call = ai_view_tool_block_add_call(block, tu);
	g_autoptr(AiToolResult) result = ai_tool_result_new(id, "boom", TRUE);

	ai_tool_call_finish(call, result);

	return call;
}

static gchar *
summary_of(AiViewToolBlock *block)
{
	return ai_view_tool_block_get_summary(block);
}

/* The full collapsed rendering, marker included. */
static gchar *
collapsed_of(AiViewToolBlock *block)
{
	return ai_view_block_render_text(AI_VIEW_BLOCK(block), 0);
}

/* ----------------------------------------------------------------
 * The summary strings
 * ---------------------------------------------------------------- */

static void
test_single_edit_names_the_file(void)
{
	/*
	 * One call with a known target shows the target, not a count. "Edited
	 * 1 file" tells a reader nothing they could act on; the file name does.
	 */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Edit",
	       "{\"file_path\": \"/home/zach/src/cmacs-office-package.c\","
	       " \"old_string\": \"a\\nb\\nc\\nd\\ne\\nf\\ng\\nh\\ni\\nj\\nk\\nl\","
	       " \"new_string\": \"1\\n2\\n3\\n4\\n5\\n6\\n7\\n8\\n9\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Edited cmacs-office-package.c  +9-12");
}

static void
test_single_bash_has_no_target_count(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Bash",
	       "{\"command\": \"make -j8\"}");

	/* A command's target is the command itself, which is worth showing. */
	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran make -j8");
}

static void
test_two_writes_count_and_sum(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Write",
	       "{\"file_path\": \"a.c\", \"content\": \"x\\ny\\nz\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "t2", "Write",
	       "{\"file_path\": \"b.c\", \"content\": \"p\\nq\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Created 2 files  +5-0");
}

static void
test_additions_only_keeps_the_minus_zero(void)
{
	/*
	 * "+335-0" rather than "+335": the reference rendering shows both
	 * halves, and a reader scanning a column of them wants them aligned.
	 */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Write",
	       "{\"file_path\": \"a.c\", \"content\": \"one\\ntwo\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Created a.c  +2-0");
}

static void
test_mixed_categories_lowercase_after_the_first(void)
{
	/* The reference line from a real session. */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "e1", "Edit",
	       "{\"file_path\": \"a.c\", \"old_string\": \"1\\n2\","
	       " \"new_string\": \"x\\ny\\nz\\nw\\nv\\nu\\nt\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "e2", "Edit",
	       "{\"file_path\": \"b.c\", \"old_string\": \"1\\n2\","
	       " \"new_string\": \"x\\ny\\nz\\nw\\nv\\nu\\nt\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "e3", "Edit",
	       "{\"file_path\": \"c.c\", \"old_string\": \"1\\n2\","
	       " \"new_string\": \"x\\ny\\nz\\nw\\nv\\nu\\nt\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"make\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"ls\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Edited 3 files, ran 2 commands  +21-6");
}

static void
test_three_categories(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "r1", "Read", "{\"file_path\": \"a.c\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "r2", "Read", "{\"file_path\": \"b.c\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"make\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"ls\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "g1", "Grep", "{\"pattern\": \"foo\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "g2", "Grep", "{\"pattern\": \"bar\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==,
	                "Read 2 files, ran 2 commands, searched 2 patterns");
}

static void
test_category_order_is_first_seen(void)
{
	/*
	 * The summary reads as a sequence of what happened, so categories
	 * appear in the order they were first used -- not in enum order.
	 */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"make\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"ls\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "r1", "Read", "{\"file_path\": \"a.c\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "r2", "Read", "{\"file_path\": \"b.c\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran 2 commands, read 2 files");
}

static void
test_no_diff_no_suffix(void)
{
	/* A group that only ran commands carries no diff figure at all. */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"make\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"ls\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b3", "Bash", "{\"command\": \"pwd\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran 3 commands");
	g_assert_null(strstr(summary, "+0"));
}

static void
test_one_failure_among_successes(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"make\"}");
	add_failed(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"boom\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b3", "Bash", "{\"command\": \"ls\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran 3 commands  (1 failed)");
}

static void
test_all_failed(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_failed(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"a\"}");
	add_failed(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"b\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran 2 commands  (2 failed)");
}

static void
test_denied_counts_as_a_failure(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiToolUse) tu = tool_use("b1", "Bash", "{\"command\": \"rm -rf /\"}");
	AiToolCall *call;
	g_autofree gchar *summary = NULL;

	call = ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), tu);
	ai_tool_call_deny(call);

	g_assert_cmpint(ai_tool_call_get_state(call), ==, AI_TOOL_CALL_DENIED);

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran rm -rf /  (1 failed)");
}

static void
test_unknown_tool_falls_back(void)
{
	/*
	 * A transcript that silently omitted an unrecognised call would be
	 * lying about what ran. A generic verb is the honest answer.
	 */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "x1", "SomeFutureTool", "{\"thing\": 1}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Used 1 tool");
}

static void
test_registered_tool_is_used(void)
{
	/* The extensibility hook: one struct literal teaches the summary. */
	const AiToolStyle style = {
		"deploy", "Deployed", "service", "services",
		AI_TOOL_CATEGORY_COMMAND, "target", FALSE
	};
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	ai_tool_style_register(&style);

	add_ok(AI_VIEW_TOOL_BLOCK(block), "d1", "deploy",
	       "{\"target\": \"production\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Deployed production");
}

static void
test_pending_call_still_summarised(void)
{
	/* A call in flight is part of the group from the moment it starts. */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiToolUse) tu = tool_use("t1", "Bash", "{\"command\": \"sleep 30\"}");
	g_autofree gchar *summary = NULL;

	ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), tu);

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran sleep 30");
}

static void
test_long_path_becomes_a_basename(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Read",
	       "{\"file_path\": \"/a/very/deep/path/that/goes/on/forever/file.c\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Read file.c");
}

static void
test_multiline_command_is_flattened(void)
{
	/* A newline in the target would break the one-line summary. */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Bash",
	       "{\"command\": \"cd src\\nmake\\n\"}");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_null(strchr(summary, '\n'));
	g_assert_cmpstr(summary, ==, "Ran cd src make");
}

static void
test_many_calls_stay_sane(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *summary = NULL;
	guint i;

	for (i = 0; i < 100; i++)
	{
		g_autofree gchar *id = g_strdup_printf("t%u", i);
		g_autofree gchar *input =
			g_strdup_printf("{\"command\": \"echo %u\"}", i);

		add_ok(AI_VIEW_TOOL_BLOCK(block), id, "Bash", input);
	}

	g_assert_cmpuint(ai_view_tool_block_get_n_calls(AI_VIEW_TOOL_BLOCK(block)),
	                 ==, 100);

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Ran 100 commands");
}

/* ----------------------------------------------------------------
 * Rendering, markers and spans
 * ---------------------------------------------------------------- */

static void
test_collapsed_has_the_marker(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *text = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"ls\"}");

	g_assert_false(ai_view_block_get_expanded(block));

	text = collapsed_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(text, ==, "Ran ls \xe2\x80\xba");

	/* One line collapsed, whatever is in the group. */
	g_assert_null(strchr(text, '\n'));
}

static void
test_expanded_lists_every_call(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *text = NULL;
	gchar **lines = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"make\"}");
	add_ok(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"ls\"}");

	ai_view_block_set_expanded(block, TRUE);
	text = collapsed_of(AI_VIEW_TOOL_BLOCK(block));

	lines = g_strsplit(text, "\n", -1);
	g_assert_cmpuint(g_strv_length(lines), ==, 3);   /* summary + two calls */
	g_assert_true(strstr(lines[1], "make") != NULL);
	g_assert_true(strstr(lines[2], "ls") != NULL);
	g_strfreev(lines);

	/* The marker flips to the open form. */
	g_assert_true(strstr(text, "\xe2\x8c\x84") != NULL);
}

static void
test_marker_span_is_tagged(void)
{
	/*
	 * The affordance is a span, not something the frontend appends, so
	 * every frontend can style it and none has to know it exists.
	 */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiRenderedText) rendered = NULL;
	guint i;
	gboolean found_marker = FALSE;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"ls\"}");
	rendered = ai_view_block_render(block, 0);

	for (i = 0; i < ai_rendered_text_get_n_spans(rendered); i++)
	{
		guint start = 0, len = 0;
		AiStyleTag tag = AI_STYLE_DEFAULT;

		g_assert_true(ai_rendered_text_get_span(rendered, i, &start, &len, &tag));

		if (tag == AI_STYLE_MARKER)
		{
			const gchar *text = ai_rendered_text_get_text(rendered);

			found_marker = TRUE;

			/* Offsets are bytes, and the marker is three of them. */
			g_assert_cmpuint(start + len, <=,
			                 ai_rendered_text_get_length(rendered));
			g_assert_true(g_utf8_validate(text + start, len, NULL));
		}
	}

	g_assert_true(found_marker);
}

static void
test_verb_and_target_are_tagged_apart(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiRenderedText) rendered = NULL;
	const gchar *text;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Read", "{\"file_path\": \"a.c\"}");
	rendered = ai_view_block_render(block, 0);
	text = ai_rendered_text_get_text(rendered);

	/* "Read" is the verb, "a.c" is what it acted on. */
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, 0), ==,
	                AI_STYLE_TOOL_NAME);
	g_assert_cmpint(
		ai_rendered_text_get_tag_at(rendered, (guint)(strstr(text, "a.c") - text)),
		==, AI_STYLE_TOOL_TARGET);
}

static void
test_diff_halves_are_tagged_apart(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiRenderedText) rendered = NULL;
	const gchar *text;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Write",
	       "{\"file_path\": \"a.c\", \"content\": \"x\\ny\"}");

	rendered = ai_view_block_render(block, 0);
	text = ai_rendered_text_get_text(rendered);

	g_assert_cmpint(
		ai_rendered_text_get_tag_at(rendered, (guint)(strstr(text, "+2") - text)),
		==, AI_STYLE_ADDED);
	g_assert_cmpint(
		ai_rendered_text_get_tag_at(rendered, (guint)(strstr(text, "-0") - text)),
		==, AI_STYLE_REMOVED);
}

/* ----------------------------------------------------------------
 * Call bookkeeping
 * ---------------------------------------------------------------- */

static void
test_repeat_start_updates_not_duplicates(void)
{
	/*
	 * A streamed call announces its name before its arguments exist, so
	 * TOOL_STARTED arrives twice for one id. The second must fill in the
	 * first, not add a second call.
	 */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiToolUse) first = tool_use("t1", "Edit", "{}");
	g_autoptr(AiToolUse) second =
		tool_use("t1", "Edit", "{\"file_path\": \"late.c\"}");
	AiToolCall *a;
	AiToolCall *b;
	g_autofree gchar *summary = NULL;

	a = ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), first);
	g_assert_null(ai_tool_call_get_target(a));

	b = ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), second);

	g_assert_true(a == b);
	g_assert_cmpuint(ai_view_tool_block_get_n_calls(AI_VIEW_TOOL_BLOCK(block)),
	                 ==, 1);

	/* The target the first event could not know is now there. */
	g_assert_cmpstr(ai_tool_call_get_target(b), ==, "late.c");

	summary = summary_of(AI_VIEW_TOOL_BLOCK(block));
	g_assert_cmpstr(summary, ==, "Edited late.c");
}

static void
test_calls_without_ids_are_not_merged(void)
{
	/*
	 * Several providers omit the id. Treating every one of those as the
	 * same call would merge unrelated work into one line.
	 */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiToolUse) a = tool_use("", "Bash", "{\"command\": \"one\"}");
	g_autoptr(AiToolUse) b = tool_use("", "Bash", "{\"command\": \"two\"}");

	ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), a);
	ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), b);

	g_assert_cmpuint(ai_view_tool_block_get_n_calls(AI_VIEW_TOOL_BLOCK(block)),
	                 ==, 2);
}

static void
test_find_call(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();

	add_ok(AI_VIEW_TOOL_BLOCK(block), "wanted", "Bash", "{\"command\": \"ls\"}");

	g_assert_nonnull(ai_view_tool_block_find_call(AI_VIEW_TOOL_BLOCK(block),
	                                              "wanted"));
	g_assert_null(ai_view_tool_block_find_call(AI_VIEW_TOOL_BLOCK(block),
	                                           "absent"));
	g_assert_null(ai_view_tool_block_find_call(AI_VIEW_TOOL_BLOCK(block), NULL));
	g_assert_null(ai_view_tool_block_find_call(AI_VIEW_TOOL_BLOCK(block), ""));
}

static void
test_get_call_out_of_range(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();

	add_ok(AI_VIEW_TOOL_BLOCK(block), "t1", "Bash", "{\"command\": \"ls\"}");

	g_assert_nonnull(ai_view_tool_block_get_call(AI_VIEW_TOOL_BLOCK(block), 0));
	g_assert_null(ai_view_tool_block_get_call(AI_VIEW_TOOL_BLOCK(block), 1));
	g_assert_null(ai_view_tool_block_get_call(AI_VIEW_TOOL_BLOCK(block), 9999));
}

static void
test_empty_block_renders_empty(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *text = collapsed_of(AI_VIEW_TOOL_BLOCK(block));

	g_assert_cmpstr(text, ==, "");
}

static void
test_diff_counts_only_where_declared(void)
{
	/* Running a command must never inflate the diff figure. */
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash",
	       "{\"command\": \"echo one\\ntwo\\nthree\"}");

	g_assert_cmpuint(
		ai_view_tool_block_get_lines_added(AI_VIEW_TOOL_BLOCK(block)), ==, 0);
	g_assert_cmpuint(
		ai_view_tool_block_get_lines_removed(AI_VIEW_TOOL_BLOCK(block)), ==, 0);
}

static void
test_changed_invalidates_the_render(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b1", "Bash", "{\"command\": \"make\"}");
	before = collapsed_of(AI_VIEW_TOOL_BLOCK(block));

	add_ok(AI_VIEW_TOOL_BLOCK(block), "b2", "Bash", "{\"command\": \"ls\"}");
	after = collapsed_of(AI_VIEW_TOOL_BLOCK(block));

	g_assert_cmpstr(before, !=, after);
	g_assert_cmpstr(after, ==, "Ran 2 commands \xe2\x80\xba");
}

/* Preview spans keep the same sorted, non-overlapping UTF-8 contract as
 * ordinary transcript text, including after wrapping. */
static void
assert_preview_spans(AiRenderedText *rendered)
{
	const gchar *text = ai_rendered_text_get_text(rendered);
	guint i, previous_end = 0;
	g_assert_true(g_utf8_validate(text, -1, NULL));
	for (i = 0; i < ai_rendered_text_get_n_spans(rendered); i++)
	{
		guint start, len;
		AiStyleTag tag;
		g_assert_true(ai_rendered_text_get_span(rendered, i, &start, &len, &tag));
		g_assert_cmpuint(start, >=, previous_end);
		g_assert_cmpuint(len, >, 0);
		g_assert_cmpuint(start + len, <=, strlen(text));
		g_assert_true(g_utf8_validate(text + start, len, NULL));
		previous_end = start + len;
	}
}

static void
test_preview_diff_syntax(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiRenderedText) rendered = NULL;
	g_autofree gchar *legacy = NULL;
	const gchar *text;
	const gchar *needles[] = { "static", "gint", "answer(", "/* note */", "\"hi\"", "42" };
	AiStyleTag tags[] = { AI_STYLE_SYNTAX_KEYWORD, AI_STYLE_SYNTAX_TYPE, AI_STYLE_SYNTAX_FUNCTION,
		AI_STYLE_SYNTAX_COMMENT, AI_STYLE_SYNTAX_STRING, AI_STYLE_SYNTAX_NUMBER };
	guint i;

	add_ok(AI_VIEW_TOOL_BLOCK(block), "edit", "Edit",
		"{\"file_path\":\"answer.c\",\"old_string\":\"static gint answer(void) { /* note */ const char *s = \\\"hi\\\"; return 41; }\\n\","
		"\"new_string\":\"static gint answer(void) { /* note */ const char *s = \\\"hi\\\"; return 42; }\\n\"}");
	legacy = ai_view_block_render_text(block, 0);
	g_assert_null(strchr(legacy, '\n'));
	g_object_set(block, "show-previews", TRUE, NULL);
	rendered = ai_view_block_render(block, 0);
	text = ai_rendered_text_get_text(rendered);
	g_assert_nonnull(strstr(text, "relative old/new lines"));
	g_assert_nonnull(strstr(text, "\n    -"));
	g_assert_nonnull(strstr(text, "\n    +"));
	for (i = 0; i < G_N_ELEMENTS(needles); i++)
	{
		const gchar *at = strstr(text, needles[i]);
		g_assert_nonnull(at);
		g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(at - text)), ==, tags[i]);
	}
	assert_preview_spans(rendered);
	g_clear_pointer(&rendered, ai_rendered_text_unref);
	rendered = ai_view_block_render(block, 35);
	assert_preview_spans(rendered);
	g_object_set(block, "show-previews", FALSE, NULL);
	{
		g_autofree gchar *again = ai_view_block_render_text(block, 0);
		g_assert_cmpstr(again, ==, legacy);
	}
}

static void
test_preview_commands(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiToolResult) result = ai_tool_result_new("bash", "\033[32mline-0\033[0m\nline-1\nline-2\nline-3\nline-4\nline-5\nline-6\n", FALSE);
	AiToolCall *call = add_ok(AI_VIEW_TOOL_BLOCK(block), "bash", "Bash", "{\"command\":\"make -j4\\nmake test\"}");
	g_autofree gchar *text = NULL;
	add_ok(AI_VIEW_TOOL_BLOCK(block), "other", "bash", "{\"command\":\"git status\"}");
	ai_tool_call_finish(call, result);
	ai_view_tool_block_call_changed(AI_VIEW_TOOL_BLOCK(block));
	g_object_set(block, "show-previews", TRUE, NULL);
	text = ai_view_block_render_text(block, 0);
	g_assert_nonnull(strstr(text, "$ make -j4\n      make test"));
	g_assert_nonnull(strstr(text, "$ git status"));
	g_assert_nonnull(strstr(text, "line-0"));
	g_assert_nonnull(strstr(text, "line-5"));
	g_assert_null(strstr(text, "line-6"));
	g_assert_null(strchr(text, '\033'));
	g_assert_nonnull(strstr(text, "expand for details"));
	ai_view_block_set_expanded(block, TRUE);
	g_clear_pointer(&text, g_free);
	text = ai_view_block_render_text(block, 0);
	g_assert_nonnull(strstr(text, "line-6"));
}

static void
test_preview_dialects(void)
{
	static const struct { const gchar *name, *input, *needle; } cases[] = {
		{ "edit", "{\"filePath\":\"test.py\",\"oldString\":\"return 1\",\"newString\":\"return 2\"}", "return 2" },
		{ "Edit", "{\"file_path\":\"test.c\",\"old_string\":\"same\",\"new_string\":\"same\\n\"}", "final newline added" },
		{ "Edit", "{\"file_path\":\"test.c\",\"old_string\":\"\\treturn 1;\",\"new_string\":\"    return 1;\"}", "\n    -" },
		{ "multi_edit", "{\"path\":\"test.c\",\"edits\":[{\"old_string\":\"a\",\"new_string\":\"b\"},{\"old_string\":\"c\",\"new_string\":\"d\"}]}", "| d" },
		{ "apply_patch", "{\"patchText\":\"*** Begin Patch\\n*** Update File: test.c\\n@@\\n-int x = 1;\\n+int x = 2;\\n*** End Patch\"}", "int x = 2;" },
		{ "file_change", "{\"changes\":[{\"path\":\"test.c\",\"diff\":\"@@ -1 +1 @@\\n-return 1;\\n+return 2;\"}]}", "return 2;" },
		{ "Write", "{\"file_path\":\"test.c\",\"content\":\"int x;\"}", "previous content unavailable" },
		{ "file_change", "{\"changes\":[{\"path\":\"test.c\",\"kind\":\"update\"}]}", "edit text unavailable" }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
		g_autoptr(AiRenderedText) rendered = NULL;
		add_ok(AI_VIEW_TOOL_BLOCK(block), "t", cases[i].name, cases[i].input);
		g_object_set(block, "show-previews", TRUE, NULL);
		rendered = ai_view_block_render(block, 0);
		g_assert_nonnull(strstr(ai_rendered_text_get_text(rendered), cases[i].needle));
		assert_preview_spans(rendered);
	}
}

static void
test_preview_context_and_states(void)
{
	g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
	g_autoptr(AiToolUse) use = tool_use("t", "Edit", "{\"file_path\":\"x.c\",\"old_string\":\"a\\nkeep\\nb\",\"new_string\":\"x\\nkeep\\ny\"}");
	AiToolCall *call = ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), use);
	g_autofree gchar *text = NULL;
	g_object_set(block, "show-previews", TRUE, NULL);
	text = ai_view_block_render_text(block, 0);
	g_assert_nonnull(strstr(text, "not confirmed successful"));
	g_assert_nonnull(strstr(text, "      2    2 | keep"));
	ai_tool_call_deny(call);
	ai_view_tool_block_call_changed(AI_VIEW_TOOL_BLOCK(block));
	g_clear_pointer(&text, g_free);
	text = ai_view_block_render_text(block, 0);
	g_assert_nonnull(strstr(text, "denied"));
	g_assert_nonnull(strstr(text, "not confirmed successful"));
}

static void
test_preview_malformed_and_limits(void)
{
	const gchar *inputs[] = { "null", "[]", "{\"old_string\":7,\"new_string\":null}",
		"{\"edits\":[null,3,{}, {\"old_string\":[],\"new_string\":true}]}", "{\"patchText\":false}" };
	guint i;
	for (i = 0; i < G_N_ELEMENTS(inputs); i++)
	{
		g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
		g_autoptr(AiRenderedText) rendered = NULL;
		add_ok(AI_VIEW_TOOL_BLOCK(block), "t", "edit", inputs[i]);
		g_object_set(block, "show-previews", TRUE, NULL);
		rendered = ai_view_block_render(block, 0);
		assert_preview_spans(rendered);
	}
	{
		g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
		g_autofree gchar *large = g_strnfill(70000, 'x');
		g_autoptr(AiToolResult) result = ai_tool_result_new("t", large, FALSE);
		AiToolCall *call = add_ok(AI_VIEW_TOOL_BLOCK(block), "t", "bash", "{\"command\":\"make\"}");
		g_autofree gchar *text = NULL;
		ai_tool_call_finish(call, result);
		g_object_set(block, "show-previews", TRUE, NULL);
		text = ai_view_block_render_text(block, 0);
		g_assert_cmpuint(strlen(text), <, 1024);
		g_assert_nonnull(strstr(text, "line truncated"));
		g_assert_nonnull(strstr(text, "expand for details"));
	}
}

static void
test_preview_languages(void)
{
	static const struct { const gchar *path, *code, *token; AiStyleTag tag; } cases[] = {
		{ "code.py", "value = \"\"\"start\n# inside string\nend\"\"\"", "# inside string", AI_STYLE_SYNTAX_STRING },
		{ "code.c", "/* start\nstill a comment\n*/", "still a comment", AI_STYLE_SYNTAX_COMMENT },
		{ "code.sh", "if test -f file; then echo \"hi\"; fi", "if", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.json", "{\"key\": true}", "true", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.yaml", "value: 42 # a note", "# a note", AI_STYLE_SYNTAX_COMMENT },
		{ "code.rs", "pub fn example() {}", "fn", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.go", "func example() {}", "func", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.ts", "const example = null;", "const", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.el", "(defun example () nil)", "defun", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.hs", "module Example where", "module", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.sql", "SELECT value FROM example", "SELECT", AI_STYLE_SYNTAX_KEYWORD },
		{ "code.unknown", "return 42", "return", AI_STYLE_CODE }
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(AiViewBlock) block = ai_view_tool_block_new();
		g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
		g_autoptr(AiToolUse) use = NULL;
		g_autoptr(AiRenderedText) rendered = NULL;
		JsonObject *object = json_object_new();
		const gchar *text, *at;
		json_object_set_string_member(object, "file_path", cases[i].path);
		json_object_set_string_member(object, "content", cases[i].code);
		json_node_take_object(node, object);
		use = ai_tool_use_new("t", "Write", node);
		ai_view_tool_block_add_call(AI_VIEW_TOOL_BLOCK(block), use);
		g_object_set(block, "show-previews", TRUE, NULL);
		rendered = ai_view_block_render(block, 0);
		text = ai_rendered_text_get_text(rendered);
		at = strstr(text, cases[i].token);
		g_assert_nonnull(at);
		g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(at - text)), ==, cases[i].tag);
		assert_preview_spans(rendered);
	}
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/view-tool/preview/diff-syntax", test_preview_diff_syntax);
	g_test_add_func("/ai-glib/view-tool/preview/commands", test_preview_commands);
	g_test_add_func("/ai-glib/view-tool/preview/dialects", test_preview_dialects);
	g_test_add_func("/ai-glib/view-tool/preview/context-states", test_preview_context_and_states);
	g_test_add_func("/ai-glib/view-tool/preview/malformed-limits", test_preview_malformed_and_limits);
	g_test_add_func("/ai-glib/view-tool/preview/languages", test_preview_languages);

	g_test_add_func("/ai-glib/tool-block/single-edit", test_single_edit_names_the_file);
	g_test_add_func("/ai-glib/tool-block/single-bash", test_single_bash_has_no_target_count);
	g_test_add_func("/ai-glib/tool-block/two-writes", test_two_writes_count_and_sum);
	g_test_add_func("/ai-glib/tool-block/additions-only",
	                test_additions_only_keeps_the_minus_zero);
	g_test_add_func("/ai-glib/tool-block/mixed-categories",
	                test_mixed_categories_lowercase_after_the_first);
	g_test_add_func("/ai-glib/tool-block/three-categories", test_three_categories);
	g_test_add_func("/ai-glib/tool-block/order-first-seen",
	                test_category_order_is_first_seen);
	g_test_add_func("/ai-glib/tool-block/no-diff-no-suffix", test_no_diff_no_suffix);
	g_test_add_func("/ai-glib/tool-block/one-failure", test_one_failure_among_successes);
	g_test_add_func("/ai-glib/tool-block/all-failed", test_all_failed);
	g_test_add_func("/ai-glib/tool-block/denied", test_denied_counts_as_a_failure);
	g_test_add_func("/ai-glib/tool-block/unknown-tool", test_unknown_tool_falls_back);
	g_test_add_func("/ai-glib/tool-block/registered-tool", test_registered_tool_is_used);
	g_test_add_func("/ai-glib/tool-block/pending", test_pending_call_still_summarised);
	g_test_add_func("/ai-glib/tool-block/basename", test_long_path_becomes_a_basename);
	g_test_add_func("/ai-glib/tool-block/flattened-command",
	                test_multiline_command_is_flattened);
	g_test_add_func("/ai-glib/tool-block/many-calls", test_many_calls_stay_sane);

	g_test_add_func("/ai-glib/tool-block/collapsed-marker", test_collapsed_has_the_marker);
	g_test_add_func("/ai-glib/tool-block/expanded", test_expanded_lists_every_call);
	g_test_add_func("/ai-glib/tool-block/marker-span", test_marker_span_is_tagged);
	g_test_add_func("/ai-glib/tool-block/verb-target-tags",
	                test_verb_and_target_are_tagged_apart);
	g_test_add_func("/ai-glib/tool-block/diff-tags", test_diff_halves_are_tagged_apart);

	g_test_add_func("/ai-glib/tool-block/repeat-start", test_repeat_start_updates_not_duplicates);
	g_test_add_func("/ai-glib/tool-block/no-id-no-merge",
	                test_calls_without_ids_are_not_merged);
	g_test_add_func("/ai-glib/tool-block/find-call", test_find_call);
	g_test_add_func("/ai-glib/tool-block/get-call-range", test_get_call_out_of_range);
	g_test_add_func("/ai-glib/tool-block/empty", test_empty_block_renders_empty);
	g_test_add_func("/ai-glib/tool-block/diff-scope", test_diff_counts_only_where_declared);
	g_test_add_func("/ai-glib/tool-block/cache-invalidated",
	                test_changed_invalidates_the_render);

	return g_test_run();
}
