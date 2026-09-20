/*
 * ai-gui-settings.h - The handful of preferences that belong to the window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Appearance is not a library concern and not a session's: it is this
 * application's, for this person, on this machine. It therefore lives
 * beside the sessions in $XDG_DATA_HOME/ai-glib/gui rather than in
 * ~/.config/ai-glib/config.yaml, which ai-glib's own config validator
 * owns and which `ai` and `ai-tui` read too. A key there would have to
 * mean something to all three.
 *
 * No GTK: tests/test-ai-gui-settings.c links this directly.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * AiGuiSettings:
 * @theme: the palette name, as `--theme` spells it
 * @color_scheme: `system`, `light` or `dark`
 *
 * What the window remembers between runs.
 */
typedef struct
{
	gchar *theme;
	gchar *color_scheme;
} AiGuiSettings;

/**
 * ai_gui_settings_default_directory:
 *
 * Returns: (transfer full): where settings live when nobody says otherwise
 */
gchar *
ai_gui_settings_default_directory(void);

/**
 * ai_gui_settings_load:
 * @directory: (nullable): where to read from, or %NULL for the default
 *
 * Reads the saved preferences.
 *
 * A file that will not parse, or a value that is not one of the ones
 * this build knows, costs itself and nothing else: the field falls back
 * to its default and the rest of the file is still used. A window that
 * refused to start because somebody hand-edited one line would be worse
 * than a window that starts in the default theme.
 *
 * Returns: (transfer full): never %NULL
 */
AiGuiSettings *
ai_gui_settings_load(const gchar *directory);

/**
 * ai_gui_settings_save:
 * @self: the settings
 * @directory: (nullable): where to write, or %NULL for the default
 * @error: (out) (optional): where a write failure goes
 *
 * Returns: %TRUE on success
 */
gboolean
ai_gui_settings_save(
	AiGuiSettings  *self,
	const gchar    *directory,
	GError        **error
);

void
ai_gui_settings_set_theme(
	AiGuiSettings *self,
	const gchar   *theme
);

void
ai_gui_settings_set_color_scheme(
	AiGuiSettings *self,
	const gchar   *color_scheme
);

void
ai_gui_settings_free(AiGuiSettings *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiGuiSettings, ai_gui_settings_free)

G_END_DECLS
