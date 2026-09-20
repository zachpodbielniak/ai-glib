/*
 * ai-gui-composer.h - The input area: text, attachments and completion
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include "ai-gui.h"
#include "ai-gui-session.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_COMPOSER (ai_gui_composer_get_type())

G_DECLARE_FINAL_TYPE(AiGuiComposer, ai_gui_composer, AI_GUI, COMPOSER,
                     GtkWidget)

GtkWidget *
ai_gui_composer_new(void);

/**
 * ai_gui_composer_set_session:
 * @self: a composer
 * @session: (nullable): the session being typed into
 *
 * Switching sessions swaps the draft as well as the completion context:
 * a half-written prompt belongs to the conversation it was aimed at, and
 * carrying it to the next one is how it gets sent to the wrong model.
 */
void
ai_gui_composer_set_session(
	AiGuiComposer *self,
	AiGuiSession  *session
);

AiGuiSession *
ai_gui_composer_get_session(AiGuiComposer *self);

/**
 * ai_gui_composer_take_text:
 * @self: a composer
 *
 * Returns: (transfer full): the buffer's contents, and empties it
 */
gchar *
ai_gui_composer_take_text(AiGuiComposer *self);

/**
 * ai_gui_composer_take_images:
 * @self: a composer
 *
 * Returns: (transfer full) (element-type AiImage): the attached images,
 *   and clears the attachment strip
 */
GList *
ai_gui_composer_take_images(AiGuiComposer *self);

void
ai_gui_composer_set_text(
	AiGuiComposer *self,
	const gchar   *text
);

gchar *
ai_gui_composer_get_text(AiGuiComposer *self);

void
ai_gui_composer_insert(
	AiGuiComposer *self,
	const gchar   *text
);

void
ai_gui_composer_attach_file(
	AiGuiComposer *self,
	GFile         *file
);

void
ai_gui_composer_focus(AiGuiComposer *self);

void
ai_gui_composer_set_busy(
	AiGuiComposer *self,
	gboolean       busy
);

G_END_DECLS
