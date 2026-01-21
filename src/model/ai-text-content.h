/*
 * ai-text-content.h - Text content block
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "model/ai-content-block.h"

G_BEGIN_DECLS

#define AI_TYPE_TEXT_CONTENT (ai_text_content_get_type())

G_DECLARE_FINAL_TYPE(AiTextContent, ai_text_content, AI, TEXT_CONTENT, AiContentBlock)

/**
 * ai_text_content_new:
 * @text: the text content
 *
 * Creates a new #AiTextContent with the given text.
 *
 * Returns: (transfer full): a new #AiTextContent
 */
AiTextContent *
ai_text_content_new(const gchar *text);

/**
 * ai_text_content_get_text:
 * @self: an #AiTextContent
 *
 * Gets the text content.
 *
 * Returns: (transfer none): the text content
 */
const gchar *
ai_text_content_get_text(AiTextContent *self);

/**
 * ai_text_content_set_text:
 * @self: an #AiTextContent
 * @text: the text to set
 *
 * Sets the text content.
 */
void
ai_text_content_set_text(
    AiTextContent *self,
    const gchar   *text
);

G_END_DECLS
