/*
 * ai-steerable-private.h - Helpers for AiSteerable implementations
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "This is a private ai-glib header."
#endif

#include "core/ai-steerable.h"

G_BEGIN_DECLS

/*
 * Every implementation returns its outcome through a GTask the same way:
 * a refusal is an error, anything else is the result as an integer.  One
 * pair of helpers keeps the "error only when refused" rule in one place.
 *
 * ai_steer_task_new() also turns off the task's cancellable check: a
 * steer cancelled after its words went out is still delivered or
 * indeterminate, and must not be reported as a refusal the caller would
 * queue again.
 */
GTask *
ai_steer_task_new(
    gpointer             source_object,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

/* Takes ownership of @error, which must be non-NULL exactly when @result
 * is AI_STEER_REFUSED. */
void
ai_steer_task_return(
    GTask         *task,
    AiSteerResult  result,
    GError        *error
);

AiSteerResult
ai_steer_task_propagate(
    GTask   *task,
    GError **error
);

G_END_DECLS
