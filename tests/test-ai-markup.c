/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <string.h>

static AiRenderedText *
render(const gchar *source)
{
	g_autoptr(AiViewBlock) block = ai_view_text_block_new();

	ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block), source);
	return ai_view_block_render(block, 0);
}

static void
test_markdown_hides_markers(void)
{
	g_autoptr(AiRenderedText) rendered = render("# Title\n\nUse **bold** and `code`.\n- item\n");
	const gchar *text = ai_rendered_text_get_text(rendered);
	const gchar *title = strstr(text, "Title");
	const gchar *bold = strstr(text, "bold");
	const gchar *code = strstr(text, "code");

	g_assert_nonnull(title);
	g_assert_null(strstr(text, "#"));
	g_assert_null(strstr(text, "**"));
	g_assert_null(strchr(text, '`'));
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(title - text)), ==, AI_STYLE_HEADING);
	g_assert_nonnull(bold);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(bold - text)), ==, AI_STYLE_HEADING);
	g_assert_nonnull(code);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(code - text)), ==, AI_STYLE_CODE);
	g_assert_nonnull(strstr(text, "• item"));
}

static void
test_markdown_fence_is_highlighted(void)
{
	g_autoptr(AiRenderedText) rendered = render("```c\nint x;\n```\n");
	const gchar *text = ai_rendered_text_get_text(rendered);
	const gchar *keyword = strstr(text, "int");

	g_assert_nonnull(keyword);
	g_assert_null(strstr(text, "```"));
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(keyword - text)), ==, AI_STYLE_SYNTAX_KEYWORD);
}

static void
test_unclosed_bold_stays_literal(void)
{
	g_autoptr(AiRenderedText) rendered = render("**still typing");

	g_assert_cmpstr(ai_rendered_text_get_text(rendered), ==, "**still typing");
}

static void
test_org_hides_markers(void)
{
	g_autoptr(AiRenderedText) rendered = render(
		"* Heading\n*bold* and /slant/ and =code=\n"
		"#+BEGIN_SRC python\ndef f():\n    return 1\n#+END_SRC\n");
	const gchar *text = ai_rendered_text_get_text(rendered);
	const gchar *heading = strstr(text, "Heading");
	const gchar *bold = strstr(text, "bold");
	const gchar *code = strstr(text, "code");
	const gchar *keyword = strstr(text, "def");

	g_assert_nonnull(heading);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(heading - text)), ==, AI_STYLE_HEADING);
	g_assert_nonnull(bold);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(bold - text)), ==, AI_STYLE_HEADING);
	g_assert_nonnull(code);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(code - text)), ==, AI_STYLE_CODE);
	g_assert_nonnull(keyword);
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, (guint)(keyword - text)), ==, AI_STYLE_SYNTAX_KEYWORD);
	g_assert_null(strstr(text, "#+BEGIN"));
	g_assert_null(strstr(text, "*bold*"));
	g_assert_null(strstr(text, "/slant/"));
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/markup/markdown", test_markdown_hides_markers);
	g_test_add_func("/ai-glib/markup/fence", test_markdown_fence_is_highlighted);
	g_test_add_func("/ai-glib/markup/unclosed", test_unclosed_bold_stays_literal);
	g_test_add_func("/ai-glib/markup/org", test_org_hides_markers);
	return g_test_run();
}
