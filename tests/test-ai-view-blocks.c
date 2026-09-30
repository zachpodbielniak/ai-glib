/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <string.h>

static void
test_plain_prose_is_unchanged(void)
{
	g_autoptr(AiViewBlock) block = ai_view_text_block_new();
	g_autoptr(AiRenderedText) rendered = NULL;

	ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block), "2 * 3 = 6\n");
	rendered = ai_view_block_render(block, 0);
	g_assert_cmpstr(ai_rendered_text_get_text(rendered), ==, "2 * 3 = 6\n");
	g_assert_cmpint(ai_rendered_text_get_tag_at(rendered, 0), ==, AI_STYLE_DEFAULT);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/view-blocks/plain-prose", test_plain_prose_is_unchanged);
	return g_test_run();
}
