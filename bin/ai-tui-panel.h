/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef AI_TUI_PANEL_H
#define AI_TUI_PANEL_H

#include <ncurses.h>
#include <ai-glib.h>
#include "ai-tui-usage.h"

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

/* Quota rows stay compact so a long native label cannot consume the sidebar.
 * panel_text still performs the shared control/UTF-8 sanitation. */
static inline gint
panel_usage_line(gint y, gint x, gint width, const gchar *text, attr_t attr)
{
	g_autofree gchar *valid = g_utf8_make_valid(text, -1);
	g_autoptr(GString) line = g_string_new(NULL);
	const gchar *p;
	gint columns = 0;

	width = MIN(width, COLS - x - 1);
	for (p = valid; *p != '\0'; p = g_utf8_next_char(p))
	{
		gunichar ch = g_utf8_get_char(p);
		gint cells;
		if (g_unichar_iscntrl(ch)) ch = ' ';
		cells = g_unichar_iszerowidth(ch) ? 0 : g_unichar_iswide(ch) ? 2 : 1;
		if (columns + cells > width) break;
		g_string_append_unichar(line, ch);
		columns += cells;
	}
	return panel_text(y, x, width, line->str, attr, FALSE);
}

static inline gint
panel_usage(gint y, gint x, gint width, AiTuiUsage *usage, attr_t heading, attr_t normal)
{
	JsonObject *data = usage_object(usage->data);
	JsonArray *entries = ai_json_get_array(data, "entries");
	guint i, count = entries != NULL ? json_array_get_length(entries) : 0;
	gboolean partial = g_strcmp0(ai_json_get_string(data, "availability", NULL), "partial") == 0;
	const gchar *title = usage->failed && data != NULL ? "REMAINING (stale)" :
		usage->pending && data != NULL ? "REMAINING (refreshing)" :
		partial ? "REMAINING (partial)" : "ACCOUNT REMAINING";

	y = panel_usage_line(y, x, width, title, heading);
	if (data == NULL || count == 0)
		return panel_usage_line(y, x, width,
			usage->pending ? "Loading..." : "Unavailable", normal);
	for (i = 0; i < count && i < 3 && y < LINES - 4; i++)
	{
		JsonNode *node = json_array_get_element(entries, i);
		JsonObject *row = usage_object(node);
		const gchar *reset = ai_json_get_string(row, "reset_text", NULL);
		g_autofree gchar *bar = usage_bar(row);

		y = panel_usage_line(y, x, width, ai_json_get_string(row, "label", "Allowance"), normal);
		y = panel_usage_line(y, x, width, bar, normal);
		if (reset == NULL) reset = ai_json_get_string(row, "reset_at", NULL);
		if (reset != NULL && y < LINES - 3)
		{
			g_autofree gchar *label = g_strdup_printf("Reset: %s", reset);
			y = panel_usage_line(y, x, width, label, normal);
		}
	}
	if (i < count)
	{
		g_autofree gchar *more = g_strdup_printf("+%u more (ai --usage)", count - i);
		y = panel_usage_line(y, x, width, more, normal);
	}
	return y;
}

#endif /* AI_TUI_PANEL_H */
