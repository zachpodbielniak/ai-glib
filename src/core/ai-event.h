/*
 * ai-event.h - One normalized thing that happened during a turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>

#include "model/ai-usage.h"
#include "model/ai-tool-use.h"
#include "model/ai-tool-result.h"

G_BEGIN_DECLS

/**
 * AiEventKind:
 * @AI_EVENT_STREAM_START: the provider began producing output
 * @AI_EVENT_TEXT_DELTA: a chunk of assistant prose
 * @AI_EVENT_THINKING_DELTA: a chunk of reasoning, which is not the answer
 * @AI_EVENT_TOOL_STARTED: the model asked for a tool
 * @AI_EVENT_TOOL_INPUT_DELTA: a fragment of a tool's arguments, streamed
 *   before the whole call is known
 * @AI_EVENT_TOOL_FINISHED: that tool produced a result, or failed
 * @AI_EVENT_USAGE: token counts, and cost when the provider reports it
 * @AI_EVENT_STATUS: something worth telling a human, in no fixed vocabulary
 * @AI_EVENT_ERROR: the turn failed
 * @AI_EVENT_STREAM_END: no further events for this turn
 *
 * What an #AiEvent describes.
 *
 * The set is deliberately small and provider-neutral.  Each provider speaks
 * a different wire format --- Anthropic SSE, Claude Code's NDJSON, opencode's
 * `{type,part}` envelope --- and a consumer that had to learn all of them
 * would gain a new bug every time one changed.  A translator per provider is
 * written once; everything downstream reads only this.
 */
typedef enum
{
    AI_EVENT_STREAM_START,
    AI_EVENT_TEXT_DELTA,
    AI_EVENT_THINKING_DELTA,
    AI_EVENT_TOOL_STARTED,
    AI_EVENT_TOOL_INPUT_DELTA,
    AI_EVENT_TOOL_FINISHED,
    AI_EVENT_USAGE,
    AI_EVENT_STATUS,
    AI_EVENT_ERROR,
    AI_EVENT_STREAM_END
} AiEventKind;

#define AI_TYPE_EVENT (ai_event_get_type())

/**
 * AiEvent:
 *
 * A refcounted boxed type describing one thing that happened during a turn.
 *
 * An event is immutable once constructed.  That is what lets it be handed to
 * several subscribers, queued, and logged without any of them having to copy
 * it defensively.
 */
typedef struct _AiEvent AiEvent;

GType
ai_event_get_type(void) G_GNUC_CONST;

AiEvent *
ai_event_new(AiEventKind kind);

AiEvent *
ai_event_new_text_delta(const gchar *text);

AiEvent *
ai_event_new_thinking_delta(const gchar *text);

AiEvent *
ai_event_new_tool_started(AiToolUse *tool_use);

AiEvent *
ai_event_new_tool_input_delta(
    const gchar *tool_use_id,
    const gchar *fragment
);

AiEvent *
ai_event_new_tool_finished(
    AiToolUse    *tool_use,
    AiToolResult *tool_result
);

AiEvent *
ai_event_new_usage(
    AiUsage *usage,
    gint64   cost_micros
);

AiEvent *
ai_event_new_status(const gchar *text);

AiEvent *
ai_event_new_error(const GError *error);

AiEvent *
ai_event_ref(AiEvent *self);

void
ai_event_unref(AiEvent *self);

AiEventKind
ai_event_get_kind(const AiEvent *self);

const gchar *
ai_event_get_text(const AiEvent *self);

AiToolUse *
ai_event_get_tool_use(const AiEvent *self);

AiToolResult *
ai_event_get_tool_result(const AiEvent *self);

const gchar *
ai_event_get_tool_use_id(const AiEvent *self);

AiUsage *
ai_event_get_usage(const AiEvent *self);

gint64
ai_event_get_cost_micros(const AiEvent *self);

const GError *
ai_event_get_error(const AiEvent *self);

const gchar *
ai_event_get_source(const AiEvent *self);

void
ai_event_set_source(
    AiEvent     *self,
    const gchar *source
);

gint64
ai_event_get_timestamp(const AiEvent *self);

const gchar *
ai_event_kind_to_string(AiEventKind kind);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiEvent, ai_event_unref)

G_END_DECLS
