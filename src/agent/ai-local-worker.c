/*
 * ai-local-worker.c - Running an agent in this process
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "agent/ai-local-worker.h"

#include "agent/ai-agent.h"
#include "core/ai-error.h"
#include "core/ai-event.h"
#include "core/ai-event-source.h"
#include "model/ai-message.h"
#include "model/ai-usage.h"

/*
 * One run in flight.
 *
 * Kept in a table on the worker rather than passed around, because
 * ai_agent_worker_cancel() and ai_agent_worker_read_output() arrive
 * later with nothing but the agent and have to find it again.
 */
typedef struct
{
    AiLocalWorker *worker;      /* borrowed; outlives the run */
    AiAgent       *agent;       /* owned */
    GString       *output;      /* whatever has been produced so far */
    gulong         event_id;    /* on the agent's executor */
} Run;

struct _AiLocalWorker
{
    GObject parent_instance;

    GHashTable *runs;   /* agent id -> Run */
};

static void ai_local_worker_worker_init (AiAgentWorkerInterface *iface);

G_DEFINE_TYPE_WITH_CODE (AiLocalWorker, ai_local_worker, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (AI_TYPE_AGENT_WORKER,
                                                ai_local_worker_worker_init))

static void
run_free (gpointer data)
{
    Run *run = data;

    if (run == NULL) return;

    if (run->event_id != 0 && run->agent != NULL)
    {
        AiToolExecutor *executor = ai_agent_get_executor (run->agent);

        if (executor != NULL)
            g_signal_handler_disconnect (executor, run->event_id);
    }

    g_clear_object (&run->agent);
    if (run->output != NULL) g_string_free (run->output, TRUE);
    g_free (run);
}

static void
ai_local_worker_finalize (GObject *object)
{
    AiLocalWorker *self = AI_LOCAL_WORKER (object);

    g_clear_pointer (&self->runs, g_hash_table_unref);

    G_OBJECT_CLASS (ai_local_worker_parent_class)->finalize (object);
}

static void
ai_local_worker_class_init (AiLocalWorkerClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = ai_local_worker_finalize;
}

static void
ai_local_worker_init (AiLocalWorker *self)
{
    self->runs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                        run_free);
}

AiLocalWorker *
ai_local_worker_new (void)
{
    return g_object_new (AI_TYPE_LOCAL_WORKER, NULL);
}

/*
 * Everything the agent's executor says, as it says it.
 *
 * Two things are wanted from the stream. Text deltas are accumulated so
 * read_output() can show a partial answer while the run is still going --
 * "peek at what it has written so far" is most of what checking on a
 * background agent means. Usage events are charged to the agent's budget,
 * which is also the brigade's, so a ceiling is enforced against what was
 * actually spent rather than against an estimate kept in parallel.
 */
static void
on_agent_event (
    AiEventSource *source,
    AiEvent       *event,
    gpointer       user_data
){
    Run *run = user_data;

    (void)source;

    if (event == NULL) return;

    switch (ai_event_get_kind (event))
    {
        case AI_EVENT_TEXT_DELTA:
        {
            const gchar *text = ai_event_get_text (event);

            if (text != NULL) g_string_append (run->output, text);
            break;
        }

        case AI_EVENT_USAGE:
        {
            AiUsage *usage = ai_event_get_usage (event);

            if (usage != NULL)
                ai_agent_record_turn (run->agent,
                                      ai_usage_get_input_tokens (usage),
                                      ai_usage_get_output_tokens (usage),
                                      ai_event_get_cost_micros (event));
            break;
        }

        default:
            break;
    }
}

static void
on_run_done (
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    g_autoptr(GTask)   task   = user_data;
    Run               *run    = g_task_get_task_data (task);
    g_autofree gchar  *text   = NULL;
    g_autoptr(GError)  error  = NULL;
    /* Own both for the duration: the run is dropped from the worker's
     * table at the end of this function, taking its refs with it. */
    g_autoptr(AiAgent)       agent  = g_object_ref (run->agent);
    g_autoptr(AiLocalWorker) worker = g_object_ref (run->worker);

    text = ai_tool_executor_run_finish (AI_TOOL_EXECUTOR (source), result,
                                        &error);

    if (text != NULL)
    {
        ai_agent_set_result (agent, text);
        ai_agent_set_state (agent, AI_AGENT_STATE_DONE);
        g_task_return_boolean (task, TRUE);
    }
    else
    {
        /*
         * A cancelled run is not a failed one. The distinction survives
         * all the way to the status listing, where "cancelled" says
         * somebody stopped it and "failed" says it broke -- and a caller
         * deciding whether to retry needs to know which.
         */
        gboolean cancelled =
            g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
            g_error_matches (error, AI_ERROR, AI_ERROR_CANCELLED) ||
            g_cancellable_is_cancelled (ai_agent_get_cancellable (agent));

        /* Whatever it managed to say before it stopped is kept: a
         * half-finished answer is worth more than an empty one. */
        if (run->output->len > 0 && ai_agent_get_result (agent) == NULL)
            ai_agent_set_result (agent, run->output->str);

        if (error != NULL)
            ai_agent_take_error (agent, g_steal_pointer (&error));

        ai_agent_set_state (agent, cancelled ? AI_AGENT_STATE_CANCELLED
                                             : AI_AGENT_STATE_FAILED);

        /* The GTask reports the same outcome the agent already records,
         * so a caller may wait on either. */
        if (cancelled)
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                     "agent '%s' was cancelled",
                                     ai_agent_get_id (agent));
        else
            g_task_return_new_error (
                task, AI_ERROR, AI_ERROR_TOOL_ERROR, "agent '%s' failed: %s",
                ai_agent_get_id (agent),
                ai_agent_get_error (agent) != NULL
                    ? ai_agent_get_error (agent)->message
                    : "no reason given");
    }

    /* The run is over; drop the accumulated state and the executor
     * subscription with it.  The agent itself is kept by the brigade. */
    g_hash_table_remove (worker->runs, ai_agent_get_id (agent));
}

static void
ai_local_worker_start_async (
    AiAgentWorker       *worker,
    AiAgent             *agent,
    const gchar         *prompt,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiLocalWorker        *self = AI_LOCAL_WORKER (worker);
    g_autoptr(GTask)      task = NULL;
    g_autoptr(AiMessage)  message = NULL;
    AiToolExecutor       *executor;
    AiProvider           *provider;
    GList                *messages = NULL;
    Run                  *run;

    task = g_task_new (worker, cancellable, callback, user_data);

    provider = ai_agent_get_provider (agent);
    executor = ai_agent_get_executor (agent);

    if (provider == NULL)
    {
        ai_agent_take_error (agent,
                             g_error_new (AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                                          "agent '%s' has no provider",
                                          ai_agent_get_id (agent)));
        ai_agent_set_state (agent, AI_AGENT_STATE_FAILED);
        g_task_return_new_error (task, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                                 "agent '%s' has no provider",
                                 ai_agent_get_id (agent));
        return;
    }

    run           = g_new0 (Run, 1);
    run->worker   = self;
    run->agent    = g_object_ref (agent);
    run->output   = g_string_new (NULL);

    if (AI_IS_EVENT_SOURCE (executor))
        run->event_id = g_signal_connect (executor, "event",
                                          G_CALLBACK (on_agent_event), run);

    g_hash_table_insert (self->runs, g_strdup (ai_agent_get_id (agent)), run);

    /* Task data, so the completion callback finds the run even if the
     * table has been emptied under it. */
    g_task_set_task_data (task, run, NULL);

    ai_agent_set_state (agent, AI_AGENT_STATE_RUNNING);

    message  = ai_message_new_user (prompt != NULL ? prompt : "");
    messages = g_list_append (NULL, message);

    /*
     * The agent's own cancellable, not the caller's. Cancelling an agent
     * has to work from ai_agent_cancel() -- which is what the
     * agent_cancel tool and a /kill both reach -- and threading the
     * caller's here instead would leave those with nothing to cancel.
     */
    ai_tool_executor_run_async (executor, provider, messages,
                                ai_agent_get_system_prompt (agent),
                                ai_agent_get_max_tokens (agent),
                                (gint)ai_budget_get_max_turns (
                                    ai_agent_get_budget (agent)),
                                ai_agent_get_cancellable (agent),
                                on_run_done, g_steal_pointer (&task));

    g_list_free (messages);
}

static gboolean
ai_local_worker_start_finish (
    AiAgentWorker  *worker,
    GAsyncResult   *result,
    GError        **error
){
    g_return_val_if_fail (g_task_is_valid (result, worker), FALSE);
    return g_task_propagate_boolean (G_TASK (result), error);
}

static gchar *
ai_local_worker_read_output (
    AiAgentWorker  *worker,
    AiAgent        *agent,
    GError        **error
){
    AiLocalWorker *self = AI_LOCAL_WORKER (worker);
    Run           *run;

    (void)error;

    /*
     * The finished answer wins over the partial one. They differ when a
     * run produced no text deltas at all -- a non-streaming provider
     * produces none -- in which case the accumulated buffer is empty and
     * the result is the only thing there is.
     */
    if (ai_agent_get_result (agent) != NULL)
        return g_strdup (ai_agent_get_result (agent));

    run = g_hash_table_lookup (self->runs, ai_agent_get_id (agent));

    if (run != NULL && run->output->len > 0)
        return g_strdup (run->output->str);

    return NULL;
}

static gboolean
ai_local_worker_cancel (
    AiAgentWorker  *worker,
    AiAgent        *agent,
    GError        **error
){
    (void)worker;
    (void)error;

    /* Always allowed, and idempotent: a caller who wants an agent stopped
     * must never be refused, including one who asks twice. */
    ai_agent_cancel (agent);
    return TRUE;
}

static void
ai_local_worker_worker_init (AiAgentWorkerInterface *iface)
{
    iface->start_async  = ai_local_worker_start_async;
    iface->start_finish = ai_local_worker_start_finish;
    iface->read_output  = ai_local_worker_read_output;
    iface->cancel       = ai_local_worker_cancel;

    /* poll_state stays NULL: this worker pushes.  ai_brigade_sweep()
     * checks ai_agent_worker_can_poll() and skips us entirely. */
}
