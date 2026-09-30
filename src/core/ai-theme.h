/*
 * ai-theme.h - The palette both front-ends draw from
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API, for the
 * same reason `ai-json-util.h` is not: it is `static` data and `static
 * inline` functions, so nothing new is exported and nothing new is
 * introspected.
 *
 * It exists because there were two answers to "what colour is a tool
 * target". `bin/ai-tui.c` had a table of ncurses colours and `gui/` had
 * a table of hex strings, written at different times from the same
 * intent, and they had already drifted: the terminal drew a link in the
 * muted colour and the window drew it in the accent colour. A person
 * running both saw two programs disagreeing about their own theme.
 *
 * The library still has no opinion about what a style tag *looks like*
 * --- that is what `docs/transcript.org` means by "tags are roles, not
 * colours", and this header is not part of the library's ABI. What it
 * settles is narrower and worth settling once: given that a frontend has
 * chosen one of *these* named palettes, which entry does each tag use.
 * A frontend that wants a palette of its own ignores this file entirely.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-theme.h is an internal header"
#endif

#include <glib.h>

#include "view/ai-style.h"

G_BEGIN_DECLS

/**
 * AiTheme:
 * @name: the spelling `--theme` accepts
 * @background: the page behind everything
 * @surface: panels, cards and the composer
 * @text: ordinary foreground
 * @muted: present but secondary
 * @accent: the colour the theme is *about*
 * @cyan: a second cool hue
 * @green: success, additions
 * @yellow: attention, in progress
 * @red: failure, removals
 * @number: numeric literals
 * @function: callable names
 *
 * One named palette, as 0xRRGGBB.
 *
 * Eleven entries rather than a colour per tag: a tag is a *role*, and
 * several roles share a colour on purpose. Adding a tag therefore needs
 * a line in ai_theme_role_for_tag() and nothing in any palette.
 */
typedef struct
{
	const gchar *name;
	guint        background;
	guint        surface;
	guint        text;
	guint        muted;
	guint        accent;
	guint        cyan;
	guint        green;
	guint        yellow;
	guint        red;
	guint        number;
	guint        function;
} AiTheme;

/**
 * AI_THEMES:
 *
 * Every palette, in the order the front-ends cycle through them.
 *
 * The first is the default in both. `terminal` and `monochrome` are all
 * zeroes because neither names any colour: `terminal` says "use whatever
 * the host already provides" --- the terminal's own sixteen in ai-tui,
 * the GTK stylesheet's named colours in ai-gui --- and `monochrome` says
 * "none at all, weight and underline only". Reading a colour out of
 * either is a bug; ai_theme_is_native() is the guard.
 */
static const AiTheme AI_THEMES[] = {
	{ "catppuccin-mocha", 0x1e1e2e, 0x313244, 0xcdd6f4, 0xa6adc8, 0xcba6f7, 0x89dceb, 0xa6e3a1, 0xf9e2af, 0xf38ba8, 0xfab387, 0x89b4fa },
	{ "catppuccin-latte", 0xeff1f5, 0xdce0e8, 0x4c4f69, 0x6c6f85, 0x8839ef, 0x047e98, 0x40a02b, 0x9a6700, 0xd20f39, 0xfe640b, 0x1e66f5 },
	{ "nord",             0x2e3440, 0x3b4252, 0xeceff4, 0xd8dee9, 0x88c0d0, 0x8fbcbb, 0xa3be8c, 0xebcb8b, 0xbf616a, 0xd08770, 0x88c0d0 },
	{ "terminal",         0,        0,        0,        0,        0,        0,        0,        0,        0,        0,        0        },
	{ "monochrome",       0,        0,        0,        0,        0,        0,        0,        0,        0,        0,        0        }
};

/**
 * AiThemeRole:
 * @AI_THEME_ROLE_DEFAULT: whatever the host's ordinary foreground is
 * @AI_THEME_ROLE_TEXT: the palette's text colour, stated
 * @AI_THEME_ROLE_MUTED: secondary
 * @AI_THEME_ROLE_ACCENT: the theme's own hue
 * @AI_THEME_ROLE_CYAN: the second cool hue
 * @AI_THEME_ROLE_GREEN: success
 * @AI_THEME_ROLE_YELLOW: attention
 * @AI_THEME_ROLE_RED: failure
 * @AI_THEME_ROLE_NUMBER: numeric literals
 * @AI_THEME_ROLE_FUNCTION: callable names
 *
 * Which palette entry a tag uses.
 *
 * %AI_THEME_ROLE_DEFAULT and %AI_THEME_ROLE_TEXT are the same colour in
 * every named palette and differ only where there is no palette: a
 * terminal draws DEFAULT in whatever foreground the user configured and
 * TEXT in its white, and a window lets DEFAULT inherit while TEXT is the
 * label colour. Folding them together would have made ordinary prose
 * follow the palette instead of the person's own terminal.
 */
typedef enum
{
	AI_THEME_ROLE_DEFAULT = 0,
	AI_THEME_ROLE_TEXT,
	AI_THEME_ROLE_MUTED,
	AI_THEME_ROLE_ACCENT,
	AI_THEME_ROLE_CYAN,
	AI_THEME_ROLE_GREEN,
	AI_THEME_ROLE_YELLOW,
	AI_THEME_ROLE_RED,
	AI_THEME_ROLE_NUMBER,
	AI_THEME_ROLE_FUNCTION
} AiThemeRole;

/**
 * AiThemeEmphasis:
 * @AI_THEME_EMPHASIS_NONE: drawn plainly
 * @AI_THEME_EMPHASIS_BOLD: heavier
 * @AI_THEME_EMPHASIS_UNDERLINE: underlined
 * @AI_THEME_EMPHASIS_FAINT: quieter than ordinary text
 *
 * How a tag is drawn *besides* its colour.
 *
 * %AI_THEME_EMPHASIS_FAINT applies only where there is no colour. A
 * palette already says "muted" with its muted entry, and quieting that
 * colour further as well would make the same tag look different in the
 * two front-ends for no reason anybody asked for. ai-tui has always
 * applied dim only when it could not use colour; stating the rule here
 * is what keeps ai-gui doing the same.
 *
 * Font family and slant are deliberately absent. A window can draw code
 * in a monospace face and reasoning in italics and a terminal cannot, so
 * those stay a frontend's own decision --- unlike colour and weight,
 * which both can express and should therefore agree on.
 */
typedef enum
{
	AI_THEME_EMPHASIS_NONE      = 0,
	AI_THEME_EMPHASIS_BOLD      = 1 << 0,
	AI_THEME_EMPHASIS_UNDERLINE = 1 << 1,
	AI_THEME_EMPHASIS_FAINT     = 1 << 2
} AiThemeEmphasis;

/**
 * ai_theme_count:
 *
 * Returns: how many palettes there are
 */
static inline guint
ai_theme_count(void)
{
	return (guint)G_N_ELEMENTS(AI_THEMES);
}

/**
 * ai_theme_get:
 * @index: a palette index
 *
 * Returns: (transfer none): the palette, or the default for a bad index
 */
static inline const AiTheme *
ai_theme_get(guint index)
{
	return &AI_THEMES[index < G_N_ELEMENTS(AI_THEMES) ? index : 0];
}

/**
 * ai_theme_find:
 * @name: (nullable): a palette name
 * @out_index: (out) (optional): where the index goes
 *
 * Looks a palette up by the spelling `--theme` accepts.
 *
 * `system` resolves to `terminal`. They are the same thing under two
 * vocabularies --- a terminal user calls it the terminal's colours and a
 * desktop user calls it the system theme --- and refusing the word a
 * person actually reaches for buys nothing.
 *
 * Returns: (transfer none) (nullable): the palette, or %NULL if unknown
 */
static inline const AiTheme *
ai_theme_find(
	const gchar *name,
	guint       *out_index
){
	guint i;

	if (name == NULL || *name == '\0')
		return NULL;

	if (g_str_equal(name, "system"))
		name = "terminal";

	for (i = 0; i < G_N_ELEMENTS(AI_THEMES); i++)
	{
		if (g_str_equal(name, AI_THEMES[i].name))
		{
			if (out_index != NULL)
				*out_index = i;

			return &AI_THEMES[i];
		}
	}

	return NULL;
}

/**
 * ai_theme_is_native:
 * @theme: (nullable): a palette
 *
 * Returns: %TRUE when @theme names no colours of its own, so the host's
 *   must be used instead
 */
static inline gboolean
ai_theme_is_native(const AiTheme *theme)
{
	return theme == NULL || theme->text == 0;
}

/**
 * ai_theme_is_monochrome:
 * @theme: (nullable): a palette
 *
 * Returns: %TRUE when @theme asks for no colour at all
 */
static inline gboolean
ai_theme_is_monochrome(const AiTheme *theme)
{
	return theme != NULL && g_str_equal(theme->name, "monochrome");
}

/**
 * ai_theme_is_dark:
 * @theme: (nullable): a palette
 *
 * Whether @theme's background is darker than its text.
 *
 * Compared against the *text*, not against a fixed luminance threshold:
 * a palette is dark because its foreground is lighter than its
 * background, and that is true whatever absolute values it picked.
 *
 * Returns: %FALSE for a palette with no colours of its own
 */
static inline gboolean
ai_theme_is_dark(const AiTheme *theme)
{
	guint background;
	guint text;

	if (ai_theme_is_native(theme))
		return FALSE;

	/* Rec. 601 luma, integer: the weights matter more than the exact
	 * curve for a comparison this coarse. */
	background = ((theme->background >> 16) & 255) * 299
		+ ((theme->background >> 8) & 255) * 587
		+ (theme->background & 255) * 114;
	text = ((theme->text >> 16) & 255) * 299
		+ ((theme->text >> 8) & 255) * 587
		+ (theme->text & 255) * 114;

	return background < text;
}

/**
 * ai_theme_role_for_tag:
 * @tag: a style role from the view layer
 *
 * The single place that decides which palette entry a tag draws in.
 *
 * Returns: the role
 */
static inline AiThemeRole
ai_theme_role_for_tag(AiStyleTag tag)
{
	switch (tag)
	{
		case AI_STYLE_USER_PROMPT:
		case AI_STYLE_HEADING:
			return AI_THEME_ROLE_TEXT;

		case AI_STYLE_DIM:
		case AI_STYLE_THINKING:
		case AI_STYLE_LINK:
		case AI_STYLE_MARKER:
		case AI_STYLE_SYNTAX_COMMENT:
			return AI_THEME_ROLE_MUTED;

		case AI_STYLE_TOOL_NAME:
		case AI_STYLE_COMMAND:
		case AI_STYLE_SYNTAX_KEYWORD:
			return AI_THEME_ROLE_ACCENT;

		case AI_STYLE_TOOL_TARGET:
		case AI_STYLE_CODE:
		case AI_STYLE_MENTION:
			return AI_THEME_ROLE_CYAN;

		case AI_STYLE_TOOL_OK:
		case AI_STYLE_ADDED:
		case AI_STYLE_TODO_DONE:
		case AI_STYLE_SYNTAX_STRING:
			return AI_THEME_ROLE_GREEN;

		case AI_STYLE_TOOL_PENDING:
		case AI_STYLE_STATUS:
		case AI_STYLE_TODO_ACTIVE:
		case AI_STYLE_SYNTAX_TYPE:
			return AI_THEME_ROLE_YELLOW;

		case AI_STYLE_TOOL_FAILED:
		case AI_STYLE_REMOVED:
		case AI_STYLE_ERROR:
			return AI_THEME_ROLE_RED;

		case AI_STYLE_SYNTAX_NUMBER:
			return AI_THEME_ROLE_NUMBER;

		case AI_STYLE_SYNTAX_FUNCTION:
			return AI_THEME_ROLE_FUNCTION;

		case AI_STYLE_DEFAULT:
		case AI_STYLE_TODO_PENDING:
		default:
			/*
			 * A tag this build does not know about draws as ordinary
			 * text rather than not at all. The library can grow a role
			 * without either frontend having to hear about it first,
			 * which is the point of the tags being roles.
			 */
			return AI_THEME_ROLE_DEFAULT;
	}
}

/**
 * ai_theme_emphasis_for_tag:
 * @tag: a style role from the view layer
 *
 * Returns: how @tag is drawn besides its colour
 */
static inline AiThemeEmphasis
ai_theme_emphasis_for_tag(AiStyleTag tag)
{
	switch (tag)
	{
		case AI_STYLE_USER_PROMPT:
		case AI_STYLE_HEADING:
		case AI_STYLE_TOOL_NAME:
		case AI_STYLE_COMMAND:
		case AI_STYLE_TODO_ACTIVE:
		case AI_STYLE_ERROR:
			return AI_THEME_EMPHASIS_BOLD;

		case AI_STYLE_MENTION:
			return AI_THEME_EMPHASIS_UNDERLINE;

		case AI_STYLE_DIM:
		case AI_STYLE_THINKING:
		case AI_STYLE_MARKER:
		case AI_STYLE_TODO_DONE:
			return AI_THEME_EMPHASIS_FAINT;

		default:
			return AI_THEME_EMPHASIS_NONE;
	}
}

/**
 * ai_theme_colour:
 * @theme: a palette
 * @role: which entry
 *
 * Returns: the colour as 0xRRGGBB. Meaningless for a native palette;
 *   ask ai_theme_is_native() first.
 */
static inline guint
ai_theme_colour(
	const AiTheme *theme,
	AiThemeRole    role
){
	switch (role)
	{
		case AI_THEME_ROLE_MUTED:    return theme->muted;
		case AI_THEME_ROLE_ACCENT:   return theme->accent;
		case AI_THEME_ROLE_CYAN:     return theme->cyan;
		case AI_THEME_ROLE_GREEN:    return theme->green;
		case AI_THEME_ROLE_YELLOW:   return theme->yellow;
		case AI_THEME_ROLE_RED:      return theme->red;
		case AI_THEME_ROLE_NUMBER:   return theme->number;
		case AI_THEME_ROLE_FUNCTION: return theme->function;
		case AI_THEME_ROLE_DEFAULT:
		case AI_THEME_ROLE_TEXT:
		default:                     return theme->text;
	}
}

G_END_DECLS
