/*
 * ai-gui-block-row.h - One transcript block, as a widget
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include "ai-gui.h"

G_BEGIN_DECLS

#define AI_GUI_TYPE_BLOCK_ROW (ai_gui_block_row_get_type())

G_DECLARE_FINAL_TYPE(AiGuiBlockRow, ai_gui_block_row, AI_GUI, BLOCK_ROW,
                     GtkWidget)

GtkWidget *
ai_gui_block_row_new(void);

/**
 * ai_gui_block_row_set_block:
 * @self: a row
 * @block: (nullable): the block to show
 *
 * Binds @self to @block, subscribing to its ::changed.
 *
 * Subscribing per row rather than watching the transcript's
 * ::block-changed is what makes recycling work: a #GtkListView hands a
 * row a different block whenever it scrolls, and a position captured at
 * bind time stops being true the moment it does.
 */
void
ai_gui_block_row_set_block(
	AiGuiBlockRow *self,
	AiViewBlock   *block
);

AiViewBlock *
ai_gui_block_row_get_block(AiGuiBlockRow *self);

void
ai_gui_block_row_refresh(AiGuiBlockRow *self);

G_END_DECLS
