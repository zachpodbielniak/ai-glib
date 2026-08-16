/*
 * ai-event-source.h - Anything that reports what it is doing
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

#include "core/ai-event.h"

G_BEGIN_DECLS

#define AI_TYPE_EVENT_SOURCE (ai_event_source_get_type())

G_DECLARE_INTERFACE(AiEventSource, ai_event_source, AI, EVENT_SOURCE, GObject)

/**
 * AiEventSourceInterface:
 * @parent_iface: the parent interface
 * @_reserved: reserved for future expansion
 *
 * Implemented by anything that can narrate a turn: the HTTP clients, the CLI
 * wrappers, and #AiToolExecutor.
 *
 * There are no virtual methods.  The interface exists for its signal and for
 * the type check --- a frontend asks `AI_IS_EVENT_SOURCE(obj)` and connects,
 * without caring whether the answer arrives over libsoup or off a
 * subprocess's stdout.
 *
 * Signals:
 * - "event": (AiEvent *event) - emitted for each thing that happens
 *
 * The signal is declared here and **nowhere else**.  #AiCliClient and
 * #AiStreamable both declare `delta`, `stream-start`, `stream-end` and
 * `tool-use`, so on a CLI provider those names resolve to the class signal
 * and a handler connected through the interface's signal id never fires.
 * One declaration site avoids repeating that.
 */
struct _AiEventSourceInterface
{
    GTypeInterface parent_iface;

    /*< private >*/
    gpointer _reserved[8];
};

void
ai_event_source_emit(
    AiEventSource *self,
    AiEvent       *event
);

void
ai_event_source_emit_all(
    AiEventSource *self,
    GPtrArray     *events
);

G_END_DECLS
