/*
 * ai-subprocess-util.c - bounded synchronous subprocess communication
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-subprocess-util.h"
#include "ai-error.h"

/*
 * How long after killing the child we keep waiting for its pipes to
 * drain before abandoning them.  The kill closes the child's own fd
 * copies, but a grandchild that inherited a pipe (tmux forks its
 * server from the client, shells background jobs, ...) keeps the
 * write end open and EOF never comes.  After this grace period we
 * cancel the pending reads outright so the caller is guaranteed to
 * get control back.
 */
#define AI_SUBPROCESS_KILL_GRACE_MS 2000

typedef struct
{
    GSubprocess  *subprocess;       /* borrowed */
    GMainContext *context;          /* borrowed */
    GCancellable *read_cancellable; /* borrowed; cancels the pipe reads */
    GAsyncResult *result;           /* owned; set when communicate returns */
    gboolean      done;
    gboolean      timed_out;
    gboolean      cancelled;
    guint         grace_armed;
} BoundedCommState;

static void
on_bounded_comm_finished(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    BoundedCommState *state = user_data;

    (void)source;

    state->result = g_object_ref(result);
    state->done   = TRUE;
    g_main_context_wakeup(state->context);
}

/*
 * Second-stage escalation: the child was killed but the pipes still
 * have not drained (a grandchild holds them open).  Cancel the reads
 * so g_subprocess_communicate_utf8_async() completes now.
 */
static gboolean
on_bounded_comm_grace_expired(gpointer user_data)
{
    BoundedCommState *state = user_data;

    if (!state->done)
        g_cancellable_cancel(state->read_cancellable);

    return G_SOURCE_REMOVE;
}

/*
 * First-stage escalation shared by the deadline and the caller's
 * cancellable: kill the child, then give the pipes a short grace
 * period to drain before abandoning them.
 */
static void
bounded_comm_escalate(BoundedCommState *state)
{
    GSource *grace;

    if (state->done || state->grace_armed)
        return;

    g_subprocess_force_exit(state->subprocess);

    grace = g_timeout_source_new(AI_SUBPROCESS_KILL_GRACE_MS);
    g_source_set_callback(grace, on_bounded_comm_grace_expired,
                          state, NULL);
    g_source_attach(grace, state->context);
    g_source_unref(grace);
    state->grace_armed = 1;
}

static gboolean
on_bounded_comm_deadline(gpointer user_data)
{
    BoundedCommState *state = user_data;

    if (!state->done)
    {
        state->timed_out = TRUE;
        bounded_comm_escalate(state);
    }

    return G_SOURCE_REMOVE;
}

static gboolean
on_bounded_comm_cancelled(
    GCancellable *cancellable,
    gpointer      user_data
){
    BoundedCommState *state = user_data;

    (void)cancellable;

    if (!state->done)
    {
        state->cancelled = TRUE;
        bounded_comm_escalate(state);
    }

    return G_SOURCE_REMOVE;
}

static gboolean
subprocess_communicate_bounded(
    GSubprocess  *subprocess,
    gconstpointer stdin_data,
    gboolean      utf8,
    gint          timeout_ms,
    GCancellable *cancellable,
    gchar       **stdout_data,
    gchar       **stderr_data,
    GBytes      **stdout_bytes,
    GBytes      **stderr_bytes,
    GError      **error
){
    g_autoptr(GMainContext)  context = NULL;
    g_autoptr(GCancellable)  read_cancellable = NULL;
    g_autoptr(GError)        comm_error = NULL;
    g_autofree gchar        *out_buf = NULL;
    g_autofree gchar        *err_buf = NULL;
    g_autoptr(GBytes)        out_bytes = NULL;
    g_autoptr(GBytes)        err_bytes = NULL;
    GSource                 *deadline_source = NULL;
    GSource                 *cancel_source = NULL;
    BoundedCommState         state = { NULL, };
    gboolean                 ok;

    g_return_val_if_fail(G_IS_SUBPROCESS(subprocess), FALSE);

    context          = g_main_context_new();
    read_cancellable = g_cancellable_new();

    state.subprocess       = subprocess;
    state.context          = context;
    state.read_cancellable = read_cancellable;

    g_main_context_push_thread_default(context);

    if (utf8)
        g_subprocess_communicate_utf8_async(subprocess,
                                        stdin_data,
                                        read_cancellable,
                                        on_bounded_comm_finished,
                                        &state);
    else
        g_subprocess_communicate_async(subprocess, (GBytes *)stdin_data,
                                       read_cancellable,
                                       on_bounded_comm_finished, &state);

    if (timeout_ms > 0)
    {
        deadline_source = g_timeout_source_new((guint)timeout_ms);
        g_source_set_callback(deadline_source, on_bounded_comm_deadline,
                              &state, NULL);
        g_source_attach(deadline_source, context);
    }

    if (cancellable != NULL)
    {
        cancel_source = g_cancellable_source_new(cancellable);
        g_source_set_callback(cancel_source,
                              G_SOURCE_FUNC(on_bounded_comm_cancelled),
                              &state, NULL);
        g_source_attach(cancel_source, context);
    }

    while (!state.done)
        g_main_context_iteration(context, TRUE);

    if (deadline_source != NULL)
    {
        g_source_destroy(deadline_source);
        g_source_unref(deadline_source);
    }
    if (cancel_source != NULL)
    {
        g_source_destroy(cancel_source);
        g_source_unref(cancel_source);
    }

    if (utf8)
        ok = g_subprocess_communicate_utf8_finish(subprocess, state.result,
                                              &out_buf, &err_buf,
                                              &comm_error);
    else
        ok = g_subprocess_communicate_finish(subprocess, state.result,
                                             &out_bytes, &err_bytes, &comm_error);
    g_clear_object(&state.result);

    g_main_context_pop_thread_default(context);

    if (state.timed_out)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_TIMEOUT,
                    "Subprocess did not complete within %d ms "
                    "(killed)", timeout_ms);
        return FALSE;
    }

    if (state.cancelled)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CANCELLED,
                    "Subprocess communication cancelled (killed)");
        return FALSE;
    }

    if (!ok)
    {
        g_propagate_error(error, g_steal_pointer(&comm_error));
        return FALSE;
    }

    if (stdout_data != NULL)
        *stdout_data = g_steal_pointer(&out_buf);
    if (stderr_data != NULL)
        *stderr_data = g_steal_pointer(&err_buf);

    if (stdout_bytes != NULL)
        *stdout_bytes = g_steal_pointer(&out_bytes);
    if (stderr_bytes != NULL)
        *stderr_bytes = g_steal_pointer(&err_bytes);

    return TRUE;
}

/**
 * ai_subprocess_communicate_utf8_bounded: (skip)
 * @subprocess: the spawned #GSubprocess to communicate with
 * @stdin_data: (nullable): UTF-8 data to pipe to the child's stdin
 * @timeout_ms: wall-clock deadline in milliseconds; <= 0 means no
 *   deadline (the caller's @cancellable is then the only bound)
 * @cancellable: (nullable): a #GCancellable
 * @stdout_data: (out) (optional): captured stdout
 * @stderr_data: (out) (optional): captured stderr
 * @error: (out) (optional): return location for a #GError
 *
 * Like g_subprocess_communicate_utf8(), but guaranteed to return.
 * When the deadline expires or @cancellable fires, the child is
 * killed with g_subprocess_force_exit(); if its pipes still have not
 * reached EOF after a short grace period (a grandchild inherited
 * them), the pending reads are cancelled outright.
 *
 * On timeout the error is %AI_ERROR_TIMEOUT; on cancellation it is
 * %AI_ERROR_CANCELLED.  In both cases the output locations are left
 * unset.  Otherwise behaves exactly like the GLib original: %TRUE
 * means communication completed (the caller still checks the exit
 * status).
 *
 * Runs a private #GMainContext, so it is safe on any worker thread
 * regardless of what the default main loop is doing.
 *
 * Returns: %TRUE on successful communication, %FALSE on error
 */
gboolean
ai_subprocess_communicate_utf8_bounded(
    GSubprocess *subprocess, const gchar *stdin_data, gint timeout_ms,
    GCancellable *cancellable, gchar **stdout_data, gchar **stderr_data,
    GError **error)
{
    return subprocess_communicate_bounded(subprocess, stdin_data, TRUE,
        timeout_ms, cancellable, stdout_data, stderr_data, NULL, NULL, error);
}

/**
 * ai_subprocess_communicate_bounded: (skip)
 *
 * Byte-preserving counterpart of ai_subprocess_communicate_utf8_bounded(),
 * with the same cancellation, deadline and pipe-draining guarantees.
 */
gboolean
ai_subprocess_communicate_bounded(
    GSubprocess *subprocess, GBytes *stdin_data, gint timeout_ms,
    GCancellable *cancellable, GBytes **stdout_data, GBytes **stderr_data,
    GError **error)
{
    return subprocess_communicate_bounded(subprocess, stdin_data, FALSE,
        timeout_ms, cancellable, NULL, NULL, stdout_data, stderr_data, error);
}
