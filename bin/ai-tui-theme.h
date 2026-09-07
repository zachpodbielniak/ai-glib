/* Private terminal themes. SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

typedef struct
{
	const gchar *name;
	guint background;
	guint surface;
	guint text;
	guint muted;
	guint accent;
	guint cyan;
	guint green;
	guint yellow;
	guint red;
} TuiTheme;

static const TuiTheme THEMES[] = {
	{ "catppuccin-mocha", 0x1e1e2e, 0x313244, 0xcdd6f4, 0xa6adc8, 0xcba6f7, 0x89dceb, 0xa6e3a1, 0xf9e2af, 0xf38ba8 },
	{ "catppuccin-latte", 0xeff1f5, 0xdce0e8, 0x4c4f69, 0x6c6f85, 0x8839ef, 0x047e98, 0x40a02b, 0x9a6700, 0xd20f39 },
	{ "nord", 0x2e3440, 0x3b4252, 0xeceff4, 0xd8dee9, 0x88c0d0, 0x8fbcbb, 0xa3be8c, 0xebcb8b, 0xbf616a },
	{ "terminal", 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "monochrome", 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

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
