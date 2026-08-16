/*
 * ai-event-source.c - Anything that reports what it is doing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-event-source.h"

G_DEFINE_INTERFACE(AiEventSource, ai_event_source, G_TYPE_OBJECT)

/*
 * Signal IDs.
 */
enum
{
    SIGNAL_EVENT,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
ai_event_source_default_init(AiEventSourceInterface *iface)
{
    /**
     * AiEventSource::event:
     * @self: the object that received the signal
     * @event: the #AiEvent describing what happened
     *
     * Emitted for each thing that happens during a turn.
     *
     * Events arrive in the order they occurred and always on the thread that
     * is driving the source --- for the async paths, whichever thread owns
     * the #GMainContext the request was started on.  A handler may keep
     * @event by taking a reference; the emitter does not reuse it.
     */
    signals[SIGNAL_EVENT] =
        g_signal_new("event",
                     G_TYPE_FROM_INTERFACE(iface),
                     G_SIGNAL_RUN_LAST,
                     0,
                     NULL, NULL,
                     NULL,
                     G_TYPE_NONE, 1,
                     AI_TYPE_EVENT);
}

/**
 * ai_event_source_emit:
 * @self: an #AiEventSource
 * @event: (transfer none): the event to publish
 *
 * Publishes @event to every subscriber.
 *
 * If @event has no source label yet, one is stamped from @self's type name
 * so that a transcript merging several sources can tell them apart without
 * every emitter having to remember to label its own events.
 */
void
ai_event_source_emit(
    AiEventSource *self,
    AiEvent       *event
){
    g_return_if_fail(AI_IS_EVENT_SOURCE(self));
    g_return_if_fail(event != NULL);

    if (ai_event_get_source(event) == NULL)
    {
        ai_event_set_source(event, G_OBJECT_TYPE_NAME(self));
    }

    g_signal_emit(self, signals[SIGNAL_EVENT], 0, event);
}

/**
 * ai_event_source_emit_all:
 * @self: an #AiEventSource
 * @events: (nullable) (element-type AiEvent): events to publish, in order
 *
 * Publishes every event in @events.
 *
 * The parsers hand back a whole line's worth of events at once --- a single
 * NDJSON line can carry text, a tool call and a token count together --- and
 * this saves each of them writing the same loop.  A %NULL or empty array is
 * not an error; most lines produce nothing at all.
 */
void
ai_event_source_emit_all(
    AiEventSource *self,
    GPtrArray     *events
){
    guint i;

    g_return_if_fail(AI_IS_EVENT_SOURCE(self));

    if (events == NULL)
    {
        return;
    }

    for (i = 0; i < events->len; i++)
    {
        ai_event_source_emit(self, (AiEvent *)g_ptr_array_index(events, i));
    }
}
