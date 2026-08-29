/*
 * ai-message.h - Conversation message
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
#include <json-glib/json-glib.h>

#include "core/ai-enums.h"
#include "model/ai-content-block.h"

G_BEGIN_DECLS

#define AI_TYPE_MESSAGE (ai_message_get_type())

G_DECLARE_FINAL_TYPE(AiMessage, ai_message, AI, MESSAGE, GObject)

typedef struct _AiResponse AiResponse;

AiMessage *
ai_message_new(AiRole role);

AiMessage *
ai_message_new_user(const gchar *text);

AiMessage *
ai_message_new_assistant(const gchar *text);

AiMessage *
ai_message_new_from_response(AiResponse *response);

AiMessage *
ai_message_new_tool_result(
    const gchar *tool_use_id,
    const gchar *content,
    gboolean     is_error
);

AiMessage *
ai_message_new_tool_result_with_name(
    const gchar *tool_use_id,
    const gchar *tool_name,
    const gchar *content,
    gboolean     is_error
);

AiRole
ai_message_get_role(AiMessage *self);

gchar *
ai_message_get_text(AiMessage *self);

GList *
ai_message_get_content_blocks(AiMessage *self);

void
ai_message_add_content_block(
    AiMessage      *self,
    AiContentBlock *block
);

void
ai_message_add_text(
    AiMessage   *self,
    const gchar *text
);

JsonNode *
ai_message_to_json(AiMessage *self);

AiMessage *
ai_message_new_from_json(
    JsonNode  *json,
    GError   **error
);

G_END_DECLS
