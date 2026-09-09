/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <locale.h>
#include "../bin/ai-tui-panel.h"

/**
 * assert_row:
 * @y: screen row
 * @x: screen column
 * @expected: expected text after trailing padding is removed
 *
 * Inspect actual curses cells without needing an interactive terminal.
 */
static void
assert_row(gint y, gint x, const gchar *expected)
{
	gchar actual[512];

	mvinnstr(y, x, actual, sizeof(actual) - 1);
	g_strchomp(actual);
	g_assert_cmpstr(actual, ==, expected);
}

/**
 * test_panel_wrapping:
 *
 * Exercise todo/agent bullets, ordinary metadata, long identifiers,
 * hostile controls, Unicode columns, and the footer boundary in curses.
 */
static void
test_panel_wrapping(void)
{
	FILE *input = tmpfile();
	FILE *output = tmpfile();
	SCREEN *screen;
	gint next;

	g_assert_nonnull(input);
	g_assert_nonnull(output);
	screen = newterm("xterm", output, input);
	g_assert_nonnull(screen);
	resize_term(24, 120);
	erase();
	next = panel_text(2, 90, 20, "pending: Review the project README", A_NORMAL, TRUE);
	g_assert_cmpint(next, ==, 4);
	assert_row(2, 90, "• pending: Review");
	assert_row(3, 90, "  the project README");
	next = panel_text(next, 90, 20, "agent-with-a-long-identifier: running", A_NORMAL, TRUE);
	g_assert_cmpint(next, ==, 7);
	assert_row(4, 90, "• agent-with-a-long-");
	assert_row(5, 90, "  identifier:");
	assert_row(6, 90, "  running");
	panel_text(7, 90, 20, "Provider model with a long name", A_NORMAL, FALSE);
	assert_row(7, 90, "Provider model with");
	assert_row(8, 90, "a long name");
	panel_text(10, 90, 20, "safe\tvalue\r\nnext", A_NORMAL, FALSE);
	assert_row(10, 90, "safe value  next");
	panel_text(12, 90, 8, "中文中文中文", A_NORMAL, TRUE);
	assert_row(12, 90, "• 中文中");
	assert_row(13, 90, "  文中文");
	mvaddstr(23, 90, "footer preserved");
	next = panel_text(22, 90, 10, "one two three four five six", A_NORMAL, TRUE);
	g_assert_cmpint(next, ==, 23);
	assert_row(23, 90, "footer preserved");
	/* A value near the right edge is measured against the actual screen. */
	panel_text(16, 113, 28, "abcdefgh", A_NORMAL, TRUE);
	assert_row(16, 113, "• abcd");
	assert_row(17, 113, "  efgh");
	endwin();
	delscreen(screen);
	fclose(input);
	fclose(output);
}

/**
 * main:
 * @argc: argument count
 * @argv: (array length=argc): arguments
 *
 * Returns: the GLib test result
 */
gint
main(gint argc, gchar **argv)
{
	setlocale(LC_ALL, "C.UTF-8");
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-glib/tui/panel-wrapping", test_panel_wrapping);
	return g_test_run();
}
