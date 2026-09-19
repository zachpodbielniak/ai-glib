/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
#include "model/ai-image-content.h"
G_BEGIN_DECLS
#define AI_TYPE_PROMPT_QUEUE (ai_prompt_queue_get_type())
G_DECLARE_FINAL_TYPE(AiPromptQueue, ai_prompt_queue, AI, PROMPT_QUEUE, GObject)
AiPromptQueue *ai_prompt_queue_new(void);
guint ai_prompt_queue_get_length(AiPromptQueue *self);
gboolean ai_prompt_queue_push(AiPromptQueue *self, const gchar *text,
                              GList *images, gboolean separate, GError **error);
gchar *ai_prompt_queue_pop(AiPromptQueue *self, GList **images);
void ai_prompt_queue_clear(AiPromptQueue *self);
G_END_DECLS
