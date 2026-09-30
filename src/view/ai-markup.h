/* Private markdown and org rendering. SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#ifndef AI_GLIB_COMPILATION
#error "This is a private ai-glib header."
#endif

#include "view/ai-style.h"

typedef struct
{
	guint      start;
	guint      length;
	AiStyleTag tag;
} AiMarkupToken;

typedef struct
{
	guint  body;
	guint  length;
	gchar *language;
} AiMarkupFence;

AiRenderedText *
ai_markup_render(const gchar *text, GArray *tokens);

GArray *
ai_markup_fences(const gchar *text);

void
ai_markup_fence_free(gpointer data);
