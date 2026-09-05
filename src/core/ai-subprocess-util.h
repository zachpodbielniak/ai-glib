/*
 * ai-subprocess-util.h - bounded synchronous subprocess communication
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API.
 *
 * g_subprocess_communicate_utf8() has no deadline and only unblocks
 * when the child has exited AND every captured pipe has hit EOF.  A
 * wedged child — or a grandchild that inherited a pipe fd and never
 * closes it — therefore blocks the calling thread forever.  That is
 * exactly the failure that froze a libreclaw session for hours: one
 * hung `tmux` invocation pinned the session worker with no timeout
 * and no cancellable anywhere in the stack.
 *
 * ai_subprocess_communicate_utf8_bounded() is the drop-in cure used
 * by every synchronous CLI call path in ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-subprocess-util.h is an internal header"
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

/* Byte-preserving variant for arbitrary command output. */
gboolean
ai_subprocess_communicate_bounded(
    GSubprocess  *subprocess,
    GBytes       *stdin_data,
    gint          timeout_ms,
    GCancellable *cancellable,
    GBytes      **stdout_data,
    GBytes      **stderr_data,
    GError      **error
);

gboolean
ai_subprocess_communicate_utf8_bounded(
    GSubprocess  *subprocess,
    const gchar  *stdin_data,
    gint          timeout_ms,
    GCancellable *cancellable,
    gchar       **stdout_data,
    gchar       **stderr_data,
    GError      **error
);

G_END_DECLS
