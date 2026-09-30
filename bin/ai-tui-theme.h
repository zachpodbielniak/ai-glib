/* Private terminal themes. SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The palettes themselves live in src/core/ai-theme.h, shared with the
 * GTK front-end. What is left here is the half only a terminal has:
 * turning an 0xRRGGBB into the nearest xterm cube entry, and colour
 * pairs.
 */
#pragma once

#include "core/ai-theme.h"

#define THEMES AI_THEMES
typedef AiTheme TuiTheme;

static guint theme_index;
static gboolean theme_colour;
#define PAIR_SURFACE (AI_STYLE_N_TAGS + 1)
#define PAIR_ACCENT (AI_STYLE_N_TAGS + 2)
#define PAIR_SELECTION (AI_STYLE_N_TAGS + 3)
#define PAIR_PANEL_ACCENT (AI_STYLE_N_TAGS + 4)

/* Search only the stable xterm cube and grayscale, never assume the user
 * has left the first sixteen palette entries unmodified. */
static inline short
theme_nearest(guint rgb)
{
	gint i, best = 16, distance = G_MAXINT;
	gint r = (gint)((rgb >> 16) & 255);
	gint g = (gint)((rgb >> 8) & 255);
	gint b = (gint)(rgb & 255);
	static const gint cube[] = { 0, 95, 135, 175, 215, 255 };

	for (i = 16; i < 256; i++)
	{
		gint n = i - 16;
		gint cr = i < 232 ? cube[n / 36] : 8 + (i - 232) * 10;
		gint cg = i < 232 ? cube[(n / 6) % 6] : cr;
		gint cb = i < 232 ? cube[n % 6] : cr;
		gint d = (r-cr)*(r-cr) + (g-cg)*(g-cg) + (b-cb)*(b-cb);
		if (d < distance) { distance = d; best = i; }
	}
	return (short)best;
}

static inline attr_t
theme_attr(gint pair)
{
	return theme_colour ? COLOR_PAIR(pair) : (pair == PAIR_SELECTION ? A_REVERSE : A_NORMAL);
}
