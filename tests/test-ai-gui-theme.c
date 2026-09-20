/*
 * test-ai-gui-theme.c - The shared palette, and the appearance a window remembers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * src/core/ai-theme.h is the one place that decides which palette entry
 * each style tag draws in, and both front-ends read it. The assertions
 * about that mapping are therefore not decoration: they are what makes
 * "the window and the terminal agree" a checked claim rather than an
 * intention.
 *
 * No display and no toolkit. XDG_DATA_HOME is sandboxed: a suite that
 * wrote into the developer's real settings would change the theme of
 * whatever ai-gui they had open.
 */

#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <ai-glib.h>

#include "core/ai-theme.h"

#include "ai-gui-settings.h"

/* ================================================================
 * The palette table
 * ================================================================ */

/*
 * The names, in order. The first is the default in both front-ends and
 * `ai-tui --list-themes` prints this same list, so a palette added in
 * one place must appear in the other by construction.
 */
static void
test_theme_names(void)
{
	static const gchar *const EXPECTED[] = {
		"catppuccin-mocha", "catppuccin-latte", "nord", "terminal",
		"monochrome"
	};
	guint i;

	g_assert_cmpuint(ai_theme_count(), ==, G_N_ELEMENTS(EXPECTED));

	for (i = 0; i < G_N_ELEMENTS(EXPECTED); i++)
		g_assert_cmpstr(ai_theme_get(i)->name, ==, EXPECTED[i]);

	/* The default, which is what a bare launch of either front-end
	 * starts in. */
	g_assert_cmpstr(ai_theme_get(0)->name, ==, "catppuccin-mocha");

	/* Out of range is the default rather than a read past the end. */
	g_assert_cmpstr(ai_theme_get(ai_theme_count())->name, ==,
	                "catppuccin-mocha");
}

static void
test_theme_lookup(void)
{
	guint index = 99;

	g_assert_nonnull(ai_theme_find("nord", &index));
	g_assert_cmpuint(index, ==, 2);

	/*
	 * A desktop user reaches for `system` and a terminal user for
	 * `terminal`. They are one palette under two vocabularies.
	 */
	g_assert_true(ai_theme_find("system", NULL) ==
	              ai_theme_find("terminal", NULL));

	g_assert_null(ai_theme_find("solarized", NULL));
	g_assert_null(ai_theme_find("", NULL));
	g_assert_null(ai_theme_find(NULL, NULL));
}

/*
 * `terminal` and `monochrome` name no colours, so reading one out of
 * them is a bug. ai_theme_is_native() is the guard both front-ends ask
 * before they try.
 */
static void
test_theme_native(void)
{
	g_assert_false(ai_theme_is_native(ai_theme_find("catppuccin-mocha", NULL)));
	g_assert_false(ai_theme_is_native(ai_theme_find("catppuccin-latte", NULL)));
	g_assert_false(ai_theme_is_native(ai_theme_find("nord", NULL)));
	g_assert_true(ai_theme_is_native(ai_theme_find("terminal", NULL)));
	g_assert_true(ai_theme_is_native(ai_theme_find("monochrome", NULL)));
	g_assert_true(ai_theme_is_native(NULL));

	g_assert_true(ai_theme_is_monochrome(ai_theme_find("monochrome", NULL)));
	g_assert_false(ai_theme_is_monochrome(ai_theme_find("terminal", NULL)));
	g_assert_false(ai_theme_is_monochrome(NULL));
}

/*
 * Which light/dark a palette carries decides which way the window forces
 * libadwaita, so getting it backwards renders a near-black page with
 * light-theme widget internals.
 */
static void
test_theme_darkness(void)
{
	g_assert_true(ai_theme_is_dark(ai_theme_find("catppuccin-mocha", NULL)));
	g_assert_true(ai_theme_is_dark(ai_theme_find("nord", NULL)));
	g_assert_false(ai_theme_is_dark(ai_theme_find("catppuccin-latte", NULL)));

	/* A palette with no colours has no answer, and %FALSE is the one
	 * that makes the caller fall through to the desktop's preference. */
	g_assert_false(ai_theme_is_dark(ai_theme_find("terminal", NULL)));
	g_assert_false(ai_theme_is_dark(NULL));
}

/*
 * The mapping itself, spot-checked where it is load-bearing.
 *
 * Every one of these is a place the two front-ends used to answer
 * differently, or would if somebody edited one table and not the other.
 */
static void
test_theme_roles(void)
{
	static const struct
	{
		AiStyleTag  tag;
		AiThemeRole role;
	} CASES[] = {
		{ AI_STYLE_DEFAULT,         AI_THEME_ROLE_DEFAULT },
		{ AI_STYLE_TODO_PENDING,    AI_THEME_ROLE_DEFAULT },
		{ AI_STYLE_USER_PROMPT,     AI_THEME_ROLE_TEXT },
		{ AI_STYLE_HEADING,         AI_THEME_ROLE_TEXT },
		{ AI_STYLE_DIM,             AI_THEME_ROLE_MUTED },
		{ AI_STYLE_THINKING,        AI_THEME_ROLE_MUTED },
		{ AI_STYLE_LINK,            AI_THEME_ROLE_MUTED },
		{ AI_STYLE_MARKER,          AI_THEME_ROLE_MUTED },
		{ AI_STYLE_SYNTAX_COMMENT,  AI_THEME_ROLE_MUTED },
		{ AI_STYLE_TOOL_NAME,       AI_THEME_ROLE_ACCENT },
		{ AI_STYLE_COMMAND,         AI_THEME_ROLE_ACCENT },
		{ AI_STYLE_SYNTAX_KEYWORD,  AI_THEME_ROLE_ACCENT },
		{ AI_STYLE_TOOL_TARGET,     AI_THEME_ROLE_CYAN },
		{ AI_STYLE_CODE,            AI_THEME_ROLE_CYAN },
		{ AI_STYLE_MENTION,         AI_THEME_ROLE_CYAN },
		{ AI_STYLE_TOOL_OK,         AI_THEME_ROLE_GREEN },
		{ AI_STYLE_ADDED,           AI_THEME_ROLE_GREEN },
		{ AI_STYLE_TODO_DONE,       AI_THEME_ROLE_GREEN },
		{ AI_STYLE_SYNTAX_STRING,   AI_THEME_ROLE_GREEN },
		{ AI_STYLE_TOOL_PENDING,    AI_THEME_ROLE_YELLOW },
		{ AI_STYLE_STATUS,          AI_THEME_ROLE_YELLOW },
		{ AI_STYLE_TODO_ACTIVE,     AI_THEME_ROLE_YELLOW },
		{ AI_STYLE_SYNTAX_TYPE,     AI_THEME_ROLE_YELLOW },
		{ AI_STYLE_TOOL_FAILED,     AI_THEME_ROLE_RED },
		{ AI_STYLE_REMOVED,         AI_THEME_ROLE_RED },
		{ AI_STYLE_ERROR,           AI_THEME_ROLE_RED },
		{ AI_STYLE_SYNTAX_NUMBER,   AI_THEME_ROLE_NUMBER },
		{ AI_STYLE_SYNTAX_FUNCTION, AI_THEME_ROLE_FUNCTION }
	};
	guint i;

	/* Every tag the library has, so a new one cannot be forgotten
	 * silently here. */
	g_assert_cmpuint(G_N_ELEMENTS(CASES), ==, AI_STYLE_N_TAGS);

	for (i = 0; i < G_N_ELEMENTS(CASES); i++)
	{
		g_assert_cmpint(ai_theme_role_for_tag(CASES[i].tag), ==,
		                CASES[i].role);
	}
}

/*
 * Every role resolves to a colour on every palette that has any. A role
 * that fell through to text would make two different tags look the same
 * without anybody noticing.
 */
static void
test_theme_colours_are_distinct(void)
{
	guint index;

	for (index = 0; index < ai_theme_count(); index++)
	{
		const AiTheme *theme = ai_theme_get(index);
		AiThemeRole role;

		if (ai_theme_is_native(theme))
			continue;

		for (role = AI_THEME_ROLE_DEFAULT; role <= AI_THEME_ROLE_FUNCTION;
		     role++)
		{
			g_assert_cmpuint(ai_theme_colour(theme, role), !=, 0);
		}

		/* DEFAULT and TEXT are the same entry in a named palette; they
		 * differ only where there is no palette at all. */
		g_assert_cmpuint(ai_theme_colour(theme, AI_THEME_ROLE_DEFAULT), ==,
		                 ai_theme_colour(theme, AI_THEME_ROLE_TEXT));

		/* A palette whose background matched its text would be a page
		 * of invisible words. */
		g_assert_cmpuint(theme->background, !=, theme->text);
	}
}

/*
 * Emphasis is shared because a terminal can express it too. The exact
 * sets matter: this is what ai-tui already drew, and changing one of
 * them changes both front-ends at once.
 */
static void
test_theme_emphasis(void)
{
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_USER_PROMPT), ==,
	                AI_THEME_EMPHASIS_BOLD);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_HEADING), ==,
	                AI_THEME_EMPHASIS_BOLD);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_TOOL_NAME), ==,
	                AI_THEME_EMPHASIS_BOLD);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_COMMAND), ==,
	                AI_THEME_EMPHASIS_BOLD);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_TODO_ACTIVE), ==,
	                AI_THEME_EMPHASIS_BOLD);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_ERROR), ==,
	                AI_THEME_EMPHASIS_BOLD);

	/* Underline is the mention and nothing else. A link is already told
	 * apart by its colour, and ai-tui has never underlined one -- which
	 * also means a link carries no emphasis at all. */
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_MENTION), ==,
	                AI_THEME_EMPHASIS_UNDERLINE);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_LINK), ==,
	                AI_THEME_EMPHASIS_NONE);

	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_DIM), ==,
	                AI_THEME_EMPHASIS_FAINT);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_THINKING), ==,
	                AI_THEME_EMPHASIS_FAINT);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_TODO_DONE), ==,
	                AI_THEME_EMPHASIS_FAINT);

	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_DEFAULT), ==,
	                AI_THEME_EMPHASIS_NONE);
	g_assert_cmpint(ai_theme_emphasis_for_tag(AI_STYLE_CODE), ==,
	                AI_THEME_EMPHASIS_NONE);
}

/* ================================================================
 * What the window remembers
 * ================================================================ */

typedef struct
{
	gchar *directory;
} Fixture;

static void
fixture_set_up(
	Fixture       *fixture,
	gconstpointer  data
){
	fixture->directory = g_dir_make_tmp("ai-gui-theme-XXXXXX", NULL);
	g_assert_nonnull(fixture->directory);
	g_setenv("XDG_DATA_HOME", fixture->directory, TRUE);
}

static void
fixture_tear_down(
	Fixture       *fixture,
	gconstpointer  data
){
	g_free(fixture->directory);
}

static void
test_settings_defaults(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiSettings) settings =
		ai_gui_settings_load(fixture->directory);

	/* Nothing saved yet: the default palette, and the desktop's own
	 * light or dark. */
	g_assert_cmpstr(settings->theme, ==, "catppuccin-mocha");
	g_assert_cmpstr(settings->color_scheme, ==, "system");
}

static void
test_settings_round_trip(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiSettings) saved = ai_gui_settings_load(fixture->directory);
	g_autoptr(AiGuiSettings) reloaded = NULL;
	g_autoptr(GError) error = NULL;

	ai_gui_settings_set_theme(saved, "nord");
	ai_gui_settings_set_color_scheme(saved, "dark");
	g_assert_true(ai_gui_settings_save(saved, fixture->directory, &error));
	g_assert_no_error(error);

	reloaded = ai_gui_settings_load(fixture->directory);
	g_assert_cmpstr(reloaded->theme, ==, "nord");
	g_assert_cmpstr(reloaded->color_scheme, ==, "dark");
}

/*
 * A value the setters do not recognise is refused rather than stored.
 * Writing it would produce a settings file that this build reads back
 * and then ignores, which is a setting that appears to have been
 * accepted and does nothing.
 */
static void
test_settings_refuse_unknown(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autoptr(AiGuiSettings) settings =
		ai_gui_settings_load(fixture->directory);

	ai_gui_settings_set_theme(settings, "solarized");
	g_assert_cmpstr(settings->theme, ==, "catppuccin-mocha");

	ai_gui_settings_set_color_scheme(settings, "sepia");
	g_assert_cmpstr(settings->color_scheme, ==, "system");

	ai_gui_settings_set_theme(settings, NULL);
	g_assert_cmpstr(settings->theme, ==, "catppuccin-mocha");

	/* `system` is a real spelling of `terminal` and must be storable. */
	ai_gui_settings_set_theme(settings, "system");
	g_assert_cmpstr(settings->theme, ==, "system");
}

/*
 * A file somebody hand-edited, or one written by a different build,
 * costs itself and nothing else. Refusing to start over one bad line
 * would be far worse than starting in the default theme.
 */
static void
test_settings_survive_a_bad_file(
	Fixture       *fixture,
	gconstpointer  data
){
	g_autofree gchar *path =
		g_build_filename(fixture->directory, "settings.json", NULL);
	g_autoptr(AiGuiSettings) settings = NULL;

	g_assert_true(g_file_set_contents(path, "{ \"theme\": ", -1, NULL));
	settings = ai_gui_settings_load(fixture->directory);
	g_assert_cmpstr(settings->theme, ==, "catppuccin-mocha");
	g_clear_pointer(&settings, ai_gui_settings_free);

	/* Valid JSON, unusable values: each field falls back on its own. */
	g_assert_true(g_file_set_contents(path,
		"{\"theme\": \"gruvbox\", \"color-scheme\": \"dark\"}", -1, NULL));
	settings = ai_gui_settings_load(fixture->directory);
	g_assert_cmpstr(settings->theme, ==, "catppuccin-mocha");
	g_assert_cmpstr(settings->color_scheme, ==, "dark");
	g_clear_pointer(&settings, ai_gui_settings_free);

	/* A bare `null` document is not an object; json-glib answers NULL
	 * for it, which is the case ai_json_root_object() exists to catch. */
	g_assert_true(g_file_set_contents(path, "null", -1, NULL));
	settings = ai_gui_settings_load(fixture->directory);
	g_assert_cmpstr(settings->theme, ==, "catppuccin-mocha");
	g_assert_cmpstr(settings->color_scheme, ==, "system");
}

gint
main(
	gint   argc,
	gchar *argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/ai-gui/theme/names", test_theme_names);
	g_test_add_func("/ai-gui/theme/lookup", test_theme_lookup);
	g_test_add_func("/ai-gui/theme/native", test_theme_native);
	g_test_add_func("/ai-gui/theme/darkness", test_theme_darkness);
	g_test_add_func("/ai-gui/theme/roles", test_theme_roles);
	g_test_add_func("/ai-gui/theme/colours-are-distinct",
	                test_theme_colours_are_distinct);
	g_test_add_func("/ai-gui/theme/emphasis", test_theme_emphasis);

	g_test_add("/ai-gui/settings/defaults", Fixture, NULL,
	           fixture_set_up, test_settings_defaults, fixture_tear_down);
	g_test_add("/ai-gui/settings/round-trip", Fixture, NULL,
	           fixture_set_up, test_settings_round_trip, fixture_tear_down);
	g_test_add("/ai-gui/settings/refuse-unknown", Fixture, NULL,
	           fixture_set_up, test_settings_refuse_unknown,
	           fixture_tear_down);
	g_test_add("/ai-gui/settings/survive-a-bad-file", Fixture, NULL,
	           fixture_set_up, test_settings_survive_a_bad_file,
	           fixture_tear_down);

	return g_test_run();
}
