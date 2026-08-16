/*
 * test-view-style.c - The rendering contract
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Spans are what a frontend actually consumes, and two properties of them
 * are load-bearing: offsets are byte offsets into UTF-8, and a span never
 * begins or ends inside a character. Get either wrong and an accented
 * filename corrupts every line after it.
 */

#include <ai-glib.h>

static const AiStyleTag ALL_TAGS[] = {
	AI_STYLE_DEFAULT, AI_STYLE_USER_PROMPT, AI_STYLE_HEADING, AI_STYLE_DIM,
	AI_STYLE_TOOL_NAME, AI_STYLE_TOOL_TARGET, AI_STYLE_TOOL_PENDING,
	AI_STYLE_TOOL_OK, AI_STYLE_TOOL_FAILED, AI_STYLE_ADDED, AI_STYLE_REMOVED,
	AI_STYLE_CODE, AI_STYLE_THINKING, AI_STYLE_ERROR, AI_STYLE_STATUS,
	AI_STYLE_LINK, AI_STYLE_MARKER
};

/* Assert every span is in range, non-empty, sorted, and UTF-8 aligned. */
static void
assert_spans_well_formed(AiRenderedText *rt)
{
	const gchar *text = ai_rendered_text_get_text(rt);
	guint length = ai_rendered_text_get_length(rt);
	guint previous_end = 0;
	guint i;

	for (i = 0; i < ai_rendered_text_get_n_spans(rt); i++)
	{
		guint start = 0, len = 0;
		AiStyleTag tag = AI_STYLE_DEFAULT;

		g_assert_true(ai_rendered_text_get_span(rt, i, &start, &len, &tag));

		g_assert_cmpuint(len, >, 0);
		g_assert_cmpuint(start + len, <=, length);

		/* Sorted and non-overlapping. */
		g_assert_cmpuint(start, >=, previous_end);
		previous_end = start + len;

		/* Never splits a character. */
		g_assert_true(g_utf8_validate(text + start, len, NULL));
	}
}

static void
test_tag_names_complete_and_distinct(void)
{
	/*
	 * The table and the enum have to stay in step; this is what catches a
	 * forgotten entry when somebody adds a tag.
	 */
	g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(ALL_TAGS); i++)
	{
		const gchar *name = ai_style_tag_to_string(ALL_TAGS[i]);

		g_assert_nonnull(name);
		g_assert_cmpstr(name, !=, "");
		g_assert_false(g_hash_table_contains(seen, name));
		g_hash_table_add(seen, (gpointer)name);

		/* Round-trips, which is what an Emacs face lookup relies on. */
		g_assert_cmpint(ai_style_tag_from_string(name), ==, ALL_TAGS[i]);
	}

	g_assert_cmpuint(g_hash_table_size(seen), ==, G_N_ELEMENTS(ALL_TAGS));
}

static void
test_tag_names_out_of_range(void)
{
	g_assert_cmpstr(ai_style_tag_to_string((AiStyleTag)9999), ==, "default");
	g_assert_cmpint(ai_style_tag_from_string("no-such-tag"), ==, AI_STYLE_DEFAULT);
	g_assert_cmpint(ai_style_tag_from_string(NULL), ==, AI_STYLE_DEFAULT);
}

static void
test_empty(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	g_assert_cmpstr(ai_rendered_text_get_text(rt), ==, "");
	g_assert_cmpuint(ai_rendered_text_get_length(rt), ==, 0);
	g_assert_cmpuint(ai_rendered_text_get_n_spans(rt), ==, 0);
}

static void
test_default_tag_makes_no_span(void)
{
	/* Plain text is the common case; a span for it would be noise. */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append(rt, "plain", AI_STYLE_DEFAULT);

	g_assert_cmpstr(ai_rendered_text_get_text(rt), ==, "plain");
	g_assert_cmpuint(ai_rendered_text_get_n_spans(rt), ==, 0);
}

static void
test_span_offsets_are_bytes(void)
{
	/*
	 * The contract, stated as a test. "café" is five bytes and four
	 * characters, and a frontend reading these as characters would style
	 * the wrong region.
	 */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	guint start = 0, len = 0;
	AiStyleTag tag = AI_STYLE_DEFAULT;

	ai_rendered_text_append(rt, "caf\xc3\xa9 ", AI_STYLE_DEFAULT);
	ai_rendered_text_append(rt, "code", AI_STYLE_CODE);

	g_assert_cmpuint(ai_rendered_text_get_n_spans(rt), ==, 1);
	g_assert_true(ai_rendered_text_get_span(rt, 0, &start, &len, &tag));

	g_assert_cmpuint(start, ==, 6);   /* 5 bytes for "café" plus a space */
	g_assert_cmpuint(len, ==, 4);
	g_assert_cmpint(tag, ==, AI_STYLE_CODE);

	assert_spans_well_formed(rt);
}

static void
test_adjacent_same_tag_merges(void)
{
	/* A builder appending a word at a time must not produce a span each. */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append(rt, "one ", AI_STYLE_CODE);
	ai_rendered_text_append(rt, "two ", AI_STYLE_CODE);
	ai_rendered_text_append(rt, "three", AI_STYLE_CODE);

	g_assert_cmpuint(ai_rendered_text_get_n_spans(rt), ==, 1);
	assert_spans_well_formed(rt);
}

static void
test_different_tags_do_not_merge(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append(rt, "+21", AI_STYLE_ADDED);
	ai_rendered_text_append(rt, "-6", AI_STYLE_REMOVED);

	g_assert_cmpuint(ai_rendered_text_get_n_spans(rt), ==, 2);
	assert_spans_well_formed(rt);
}

static void
test_separated_same_tag_does_not_merge(void)
{
	/* Merging across an unstyled gap would style the gap too. */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append(rt, "a", AI_STYLE_CODE);
	ai_rendered_text_append(rt, " gap ", AI_STYLE_DEFAULT);
	ai_rendered_text_append(rt, "b", AI_STYLE_CODE);

	g_assert_cmpuint(ai_rendered_text_get_n_spans(rt), ==, 2);
	assert_spans_well_formed(rt);
}

static void
test_append_empty_and_null(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append(rt, NULL, AI_STYLE_CODE);
	ai_rendered_text_append(rt, "", AI_STYLE_CODE);

	/* No text and no zero-length span. */
	g_assert_cmpuint(ai_rendered_text_get_length(rt), ==, 0);
	g_assert_cmpuint(ai_rendered_text_get_n_spans(rt), ==, 0);
}

static void
test_get_span_out_of_range(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	guint start = 12345, len = 6789;
	AiStyleTag tag = AI_STYLE_ERROR;

	ai_rendered_text_append(rt, "x", AI_STYLE_CODE);

	g_assert_false(ai_rendered_text_get_span(rt, 1, &start, &len, &tag));

	/* The out parameters are left alone on failure. */
	g_assert_cmpuint(start, ==, 12345);
	g_assert_cmpuint(len, ==, 6789);
	g_assert_cmpint(tag, ==, AI_STYLE_ERROR);
}

static void
test_get_span_null_outs(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append(rt, "x", AI_STYLE_CODE);
	g_assert_true(ai_rendered_text_get_span(rt, 0, NULL, NULL, NULL));
}

static void
test_tag_at(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append(rt, "ab", AI_STYLE_DEFAULT);
	ai_rendered_text_append(rt, "cd", AI_STYLE_ERROR);

	g_assert_cmpint(ai_rendered_text_get_tag_at(rt, 0), ==, AI_STYLE_DEFAULT);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rt, 2), ==, AI_STYLE_ERROR);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rt, 3), ==, AI_STYLE_ERROR);

	/* Past the end is not an error, just unstyled. */
	g_assert_cmpint(ai_rendered_text_get_tag_at(rt, 99), ==, AI_STYLE_DEFAULT);
}

static void
test_refcount(void)
{
	AiRenderedText *rt = ai_rendered_text_new();
	AiRenderedText *second;

	ai_rendered_text_append(rt, "shared", AI_STYLE_CODE);
	second = ai_rendered_text_ref(rt);

	ai_rendered_text_unref(rt);
	g_assert_cmpstr(ai_rendered_text_get_text(second), ==, "shared");

	ai_rendered_text_unref(second);
	g_assert_null(ai_rendered_text_ref(NULL));
	ai_rendered_text_unref(NULL);
}

/* ----------------------------------------------------------------
 * Width
 * ---------------------------------------------------------------- */

static void
test_width_is_columns_not_characters(void)
{
	g_assert_cmpuint(ai_style_text_width("abc"), ==, 3);

	/* Four characters, five bytes, four columns. */
	g_assert_cmpuint(ai_style_text_width("caf\xc3\xa9"), ==, 4);

	/* Two CJK ideographs: two characters, six bytes, four columns. */
	g_assert_cmpuint(ai_style_text_width("\xe6\x97\xa5\xe6\x9c\xac"), ==, 4);

	g_assert_cmpuint(ai_style_text_width(""), ==, 0);
	g_assert_cmpuint(ai_style_text_width(NULL), ==, 0);
}

/* ----------------------------------------------------------------
 * Wrapping
 * ---------------------------------------------------------------- */

static void
test_wrap_zero_does_not_wrap(void)
{
	/* What Emacs wants: it fills text itself. */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;

	ai_rendered_text_append(rt, "a very long line that would otherwise wrap",
	                        AI_STYLE_DEFAULT);
	wrapped = ai_rendered_text_wrap(rt, 0);

	/* Same instance, not a copy -- there is nothing to change. */
	g_assert_true(wrapped == rt);
}

static void
test_wrap_breaks_at_spaces(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;

	ai_rendered_text_append(rt, "one two three four", AI_STYLE_DEFAULT);
	wrapped = ai_rendered_text_wrap(rt, 8);

	g_assert_cmpstr(ai_rendered_text_get_text(wrapped), ==,
	                "one two\nthree\nfour");
}

static void
test_wrap_preserves_existing_newlines(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;

	ai_rendered_text_append(rt, "short\nlines\nhere", AI_STYLE_DEFAULT);
	wrapped = ai_rendered_text_wrap(rt, 40);

	g_assert_cmpstr(ai_rendered_text_get_text(wrapped), ==,
	                "short\nlines\nhere");
}

static void
test_wrap_breaks_an_overlong_word(void)
{
	/* Overflowing is worse than breaking. */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;
	gchar **lines;
	guint i;

	ai_rendered_text_append(rt, "aaaaaaaaaaaaaaaaaaaa", AI_STYLE_DEFAULT);
	wrapped = ai_rendered_text_wrap(rt, 6);

	lines = g_strsplit(ai_rendered_text_get_text(wrapped), "\n", -1);
	g_assert_cmpuint(g_strv_length(lines), >, 1);

	for (i = 0; lines[i] != NULL; i++)
	{
		g_assert_cmpuint(ai_style_text_width(lines[i]), <=, 6);
	}

	g_strfreev(lines);
}

static void
test_wrap_width_one_terminates(void)
{
	/* Degenerate, but must not loop forever. */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;

	ai_rendered_text_append(rt, "abc def", AI_STYLE_DEFAULT);
	wrapped = ai_rendered_text_wrap(rt, 1);

	g_assert_cmpuint(ai_rendered_text_get_length(wrapped), >, 0);
}

static void
test_wrap_never_splits_a_character(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;

	/* Multi-byte throughout, wrapped tightly. */
	ai_rendered_text_append(rt,
	                        "caf\xc3\xa9 caf\xc3\xa9 caf\xc3\xa9 caf\xc3\xa9",
	                        AI_STYLE_CODE);
	wrapped = ai_rendered_text_wrap(rt, 5);

	g_assert_true(g_utf8_validate(ai_rendered_text_get_text(wrapped), -1, NULL));
	assert_spans_well_formed(wrapped);
}

static void
test_wrap_respects_wide_characters(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;
	gchar **lines;
	guint i;

	/* Six ideographs: twelve columns, so at width 4 that is three lines. */
	ai_rendered_text_append(rt,
	                        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
	                        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
	                        AI_STYLE_DEFAULT);
	wrapped = ai_rendered_text_wrap(rt, 4);

	lines = g_strsplit(ai_rendered_text_get_text(wrapped), "\n", -1);

	for (i = 0; lines[i] != NULL; i++)
	{
		g_assert_cmpuint(ai_style_text_width(lines[i]), <=, 4);
	}

	g_strfreev(lines);
}

static void
test_wrap_splits_a_span_across_the_break(void)
{
	/*
	 * A span straddling a break has to become two, or the second half of a
	 * wrapped filename loses its styling.
	 */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;
	const gchar *text;

	ai_rendered_text_append(rt, "alpha beta gamma", AI_STYLE_CODE);
	wrapped = ai_rendered_text_wrap(rt, 10);

	text = ai_rendered_text_get_text(wrapped);
	g_assert_true(strchr(text, '\n') != NULL);

	/* Both halves keep the tag; the newline between them does not. */
	g_assert_cmpint(ai_rendered_text_get_tag_at(wrapped, 0), ==, AI_STYLE_CODE);
	g_assert_cmpint(
		ai_rendered_text_get_tag_at(wrapped, (guint)(strlen(text) - 1)),
		==, AI_STYLE_CODE);

	assert_spans_well_formed(wrapped);
}

static void
test_wrap_leaves_the_source_alone(void)
{
	/* A block caches one unwrapped rendering and wraps it per frontend. */
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) narrow = NULL;
	g_autoptr(AiRenderedText) wide = NULL;

	ai_rendered_text_append(rt, "one two three four five", AI_STYLE_DEFAULT);

	narrow = ai_rendered_text_wrap(rt, 8);
	wide = ai_rendered_text_wrap(rt, 40);

	g_assert_cmpstr(ai_rendered_text_get_text(rt), ==,
	                "one two three four five");
	g_assert_cmpstr(ai_rendered_text_get_text(wide), ==,
	                "one two three four five");
	g_assert_cmpstr(ai_rendered_text_get_text(narrow), !=,
	                ai_rendered_text_get_text(wide));
}

static void
test_wrap_empty(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = ai_rendered_text_wrap(rt, 10);

	g_assert_cmpstr(ai_rendered_text_get_text(wrapped), ==, "");
}

static void
test_printf_append(void)
{
	g_autoptr(AiRenderedText) rt = ai_rendered_text_new();

	ai_rendered_text_append_printf(rt, AI_STYLE_ADDED, "+%u", 21u);

	g_assert_cmpstr(ai_rendered_text_get_text(rt), ==, "+21");
	g_assert_cmpint(ai_rendered_text_get_tag_at(rt, 0), ==, AI_STYLE_ADDED);
}

static void
test_span_boxed_type(void)
{
	AiStyleSpan span = { 3, 4, AI_STYLE_CODE };
	AiStyleSpan *copy;

	copy = (AiStyleSpan *)g_boxed_copy(AI_TYPE_STYLE_SPAN, &span);
	g_assert_nonnull(copy);
	g_assert_cmpuint(copy->start, ==, 3);
	g_assert_cmpuint(copy->len, ==, 4);
	g_assert_cmpint(copy->tag, ==, AI_STYLE_CODE);

	g_boxed_free(AI_TYPE_STYLE_SPAN, copy);
	ai_style_span_free(NULL);
	g_assert_null(ai_style_span_copy(NULL));
}

int
main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-glib/style/tag-names", test_tag_names_complete_and_distinct);
	g_test_add_func("/ai-glib/style/tag-names-range", test_tag_names_out_of_range);
	g_test_add_func("/ai-glib/style/empty", test_empty);
	g_test_add_func("/ai-glib/style/default-no-span", test_default_tag_makes_no_span);
	g_test_add_func("/ai-glib/style/offsets-are-bytes", test_span_offsets_are_bytes);
	g_test_add_func("/ai-glib/style/merge-adjacent", test_adjacent_same_tag_merges);
	g_test_add_func("/ai-glib/style/no-merge-different", test_different_tags_do_not_merge);
	g_test_add_func("/ai-glib/style/no-merge-separated",
	                test_separated_same_tag_does_not_merge);
	g_test_add_func("/ai-glib/style/append-empty", test_append_empty_and_null);
	g_test_add_func("/ai-glib/style/span-range", test_get_span_out_of_range);
	g_test_add_func("/ai-glib/style/span-null-outs", test_get_span_null_outs);
	g_test_add_func("/ai-glib/style/tag-at", test_tag_at);
	g_test_add_func("/ai-glib/style/refcount", test_refcount);
	g_test_add_func("/ai-glib/style/width", test_width_is_columns_not_characters);

	g_test_add_func("/ai-glib/style/wrap/zero", test_wrap_zero_does_not_wrap);
	g_test_add_func("/ai-glib/style/wrap/at-spaces", test_wrap_breaks_at_spaces);
	g_test_add_func("/ai-glib/style/wrap/keeps-newlines",
	                test_wrap_preserves_existing_newlines);
	g_test_add_func("/ai-glib/style/wrap/overlong-word",
	                test_wrap_breaks_an_overlong_word);
	g_test_add_func("/ai-glib/style/wrap/width-one", test_wrap_width_one_terminates);
	g_test_add_func("/ai-glib/style/wrap/utf8-safe",
	                test_wrap_never_splits_a_character);
	g_test_add_func("/ai-glib/style/wrap/wide-chars",
	                test_wrap_respects_wide_characters);
	g_test_add_func("/ai-glib/style/wrap/splits-spans",
	                test_wrap_splits_a_span_across_the_break);
	g_test_add_func("/ai-glib/style/wrap/source-unchanged",
	                test_wrap_leaves_the_source_alone);
	g_test_add_func("/ai-glib/style/wrap/empty", test_wrap_empty);

	g_test_add_func("/ai-glib/style/printf", test_printf_append);
	g_test_add_func("/ai-glib/style/span-boxed", test_span_boxed_type);

	return g_test_run();
}
