/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef AI_TUI_PANEL_H
#define AI_TUI_PANEL_H

#include <ncurses.h>
#include <ai-glib.h>

/**
 * panel_text:
 * @y: first screen row
 * @x: left screen column
 * @width: available terminal columns
 * @text: (nullable): panel value
 * @attr: terminal attributes
 * @bullet: whether to reserve a hanging bullet gutter
 *
 * Wrap panel values at word boundaries, splitting long identifiers only
 * when necessary. Sanitize controls before wrapping so external labels
 * cannot move the cursor. Keep the final screen row free for shortcuts.
 *
 * Returns: the next available row
 */
static inline gint
panel_text(gint y, gint x, gint width, const gchar *text, attr_t attr, gboolean bullet)
{
	g_autofree gchar *valid = NULL;
	g_autoptr(GString) clean = g_string_new(NULL);
	g_autoptr(AiRenderedText) source = ai_rendered_text_new();
	g_autoptr(AiRenderedText) wrapped = NULL;
	g_auto(GStrv) lines = NULL;
	const gchar *p;
	gint gutter = bullet ? 2 : 0;
	guint i;

	width = MIN(width, COLS - x - 1);
	if (y < 0 || y >= LINES - 1 || x < 0 || width - gutter < 2)
		return y;
	valid = g_utf8_make_valid(text != NULL ? text : "", -1);
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p))
	{
		if (g_unichar_iscntrl(g_utf8_get_char(p)))
			g_string_append_c(clean, ' ');
		else
			g_string_append_len(clean, p, g_utf8_next_char(p) - p);
	}
	ai_rendered_text_append(source, clean->str, AI_STYLE_DEFAULT);
	wrapped = ai_rendered_text_wrap(source, (guint)(width - gutter));
	lines = g_strsplit(ai_rendered_text_get_text(wrapped), "\n", -1);
	attrset(attr);
	for (i = 0; lines[i] != NULL && y < LINES - 1; i++, y++)
	{
		if (bullet && i == 0)
			mvaddstr(y, x, g_get_charset(NULL) ? "•" : "*");
		mvaddstr(y, x + gutter, lines[i]);
	}
	return y;
}

#endif /* AI_TUI_PANEL_H */
