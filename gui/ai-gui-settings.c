/*
 * ai-gui-settings.c - The handful of preferences that belong to the window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include "core/ai-json-util.h"
#include "core/ai-theme.h"

#include "ai-gui-settings.h"

/* A settings file that has grown past this is not a settings file. */
#define AI_GUI_SETTINGS_MAX_BYTES (64 * 1024)

static const gchar *const COLOR_SCHEMES[] = { "system", "light", "dark" };

static gboolean
settings_scheme_is_known(const gchar *name)
{
	gsize i;

	if (name == NULL)
		return FALSE;

	for (i = 0; i < G_N_ELEMENTS(COLOR_SCHEMES); i++)
	{
		if (g_str_equal(name, COLOR_SCHEMES[i]))
			return TRUE;
	}

	return FALSE;
}

gchar *
ai_gui_settings_default_directory(void)
{
	return g_build_filename(g_get_user_data_dir(), "ai-glib", "gui", NULL);
}

static gchar *
settings_path(const gchar *directory)
{
	g_autofree gchar *fallback = NULL;

	if (directory == NULL)
	{
		fallback = ai_gui_settings_default_directory();
		directory = fallback;
	}

	return g_build_filename(directory, "settings.json", NULL);
}

AiGuiSettings *
ai_gui_settings_load(const gchar *directory)
{
	AiGuiSettings *self = g_new0(AiGuiSettings, 1);
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = settings_path(directory);
	JsonObject *object;
	GStatBuf info;

	/*
	 * The first theme is the default, in both front-ends: whatever
	 * AI_THEMES lists first. Naming it here would be a second answer.
	 */
	self->theme = g_strdup(ai_theme_get(0)->name);
	self->color_scheme = g_strdup("system");

	if (g_stat(path, &info) != 0 || info.st_size > AI_GUI_SETTINGS_MAX_BYTES)
		return self;

	parser = json_parser_new();

	if (!json_parser_load_from_file(parser, path, &error))
	{
		g_debug("ai-gui: ignoring %s: %s", path, error->message);
		return self;
	}

	object = ai_json_root_object(parser);

	if (object == NULL)
	{
		g_debug("ai-gui: ignoring %s: not a JSON object", path);
		return self;
	}

	{
		const gchar *theme = ai_json_get_string(object, "theme", NULL);

		/*
		 * A palette this build does not have is not an error worth
		 * reporting: a file written by a newer ai-gui, or an older one,
		 * should still start. It falls back and says so to the log.
		 */
		if (theme != NULL && ai_theme_find(theme, NULL) != NULL)
		{
			g_free(self->theme);
			self->theme = g_strdup(theme);
		}
		else if (theme != NULL)
		{
			g_debug("ai-gui: unknown saved theme '%s'; using %s", theme,
			        self->theme);
		}
	}

	{
		const gchar *scheme = ai_json_get_string(object, "color-scheme", NULL);

		if (settings_scheme_is_known(scheme))
		{
			g_free(self->color_scheme);
			self->color_scheme = g_strdup(scheme);
		}
		else if (scheme != NULL)
		{
			g_debug("ai-gui: unknown saved colour scheme '%s'; using %s",
			        scheme, self->color_scheme);
		}
	}

	return self;
}

gboolean
ai_gui_settings_save(
	AiGuiSettings  *self,
	const gchar    *directory,
	GError        **error
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;
	g_autofree gchar *path = settings_path(directory);
	g_autofree gchar *parent = g_path_get_dirname(path);

	g_return_val_if_fail(self != NULL, FALSE);

	if (g_mkdir_with_parents(parent, 0700) != 0)
	{
		g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
		            "cannot create %s", parent);
		return FALSE;
	}

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "theme");
	json_builder_add_string_value(builder, self->theme);
	json_builder_set_member_name(builder, "color-scheme");
	json_builder_add_string_value(builder, self->color_scheme);
	json_builder_end_object(builder);

	root = json_builder_get_root(builder);
	json_generator_set_pretty(generator, TRUE);
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, NULL);

	/* Replaced atomically: a half-written settings file read on the next
	 * start would lose both values, not one. */
	return g_file_set_contents_full(path, text, -1,
	                                G_FILE_SET_CONTENTS_CONSISTENT |
	                                G_FILE_SET_CONTENTS_DURABLE,
	                                0600, error);
}

void
ai_gui_settings_set_theme(
	AiGuiSettings *self,
	const gchar   *theme
){
	g_return_if_fail(self != NULL);

	if (theme == NULL || ai_theme_find(theme, NULL) == NULL)
		return;

	g_free(self->theme);
	self->theme = g_strdup(theme);
}

void
ai_gui_settings_set_color_scheme(
	AiGuiSettings *self,
	const gchar   *color_scheme
){
	g_return_if_fail(self != NULL);

	if (!settings_scheme_is_known(color_scheme))
		return;

	g_free(self->color_scheme);
	self->color_scheme = g_strdup(color_scheme);
}

void
ai_gui_settings_free(AiGuiSettings *self)
{
	if (self == NULL)
		return;

	g_free(self->theme);
	g_free(self->color_scheme);
	g_free(self);
}
