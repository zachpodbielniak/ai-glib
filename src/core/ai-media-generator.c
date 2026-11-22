/*
 * ai-media-generator.c - Asynchronous media jobs: submit, poll, download
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The wrappers here do two things beyond dispatching to the vfunc.  A
 * missing vfunc -- an optional one, or a required one in an incomplete
 * third-party implementation -- is answered through the async contract
 * with AI_ERROR_NOT_SUPPORTED, because a callback that never runs is a
 * hang and not an error anybody can report.  And ai_media_generator_wait_async()
 * is the one polling loop, so every implementation gets the same backoff,
 * retry and deadline behaviour instead of a copy each.
 */

#include "config.h"

#include "core/ai-media-generator.h"
#include "core/ai-error.h"

/*
 * How many consecutive transient poll failures (5xx, a dropped connection,
 * a 429) the wait loop rides out before giving up.  A status endpoint
 * that is down for six rounds of backoff is down, and saying so beats
 * waiting out the caller's whole deadline in silence.
 */
#define AI_MEDIA_WAIT_MAX_TRANSIENT_FAILURES (5)

G_DEFINE_INTERFACE(AiMediaGenerator, ai_media_generator, G_TYPE_OBJECT)

static void
ai_media_generator_default_init(AiMediaGeneratorInterface *iface)
{
	(void)iface;
}

/*
 * Answers an operation the implementation does not provide, through the
 * same async contract the real one would have used.  @tag is the public
 * wrapper, which is how its _finish recognises the result as ours.
 */
static void
return_not_supported(
	AiMediaGenerator    *self,
	gpointer             tag,
	const gchar         *what,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, tag);
	g_task_return_new_error(task, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
	                        "%s does not support %s",
	                        G_OBJECT_TYPE_NAME(self), what);
}

/**
 * ai_media_generator_submit_async:
 * @self: an #AiMediaGenerator
 * @request: what to generate
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the service has answered
 * @user_data: data for @callback
 *
 * Queues a job.  The service answers at once with an id; the work happens
 * afterwards.  Cancelling @cancellable abandons the wait for that answer,
 * not a job the service may already have accepted -- which is what the
 * request's idempotency key is for: submit the same request again and a
 * service that honours the key returns the original job.
 */
void
ai_media_generator_submit_async(
	AiMediaGenerator    *self,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMediaGeneratorInterface *iface;

	g_return_if_fail(AI_IS_MEDIA_GENERATOR(self));
	g_return_if_fail(AI_IS_MEDIA_REQUEST(request));

	iface = AI_MEDIA_GENERATOR_GET_IFACE(self);
	if (iface->submit_async == NULL || iface->submit_finish == NULL)
	{
		return_not_supported(self, ai_media_generator_submit_async, "submitting jobs",
		                     cancellable, callback, user_data);
		return;
	}
	iface->submit_async(self, request, cancellable, callback, user_data);
}

/**
 * ai_media_generator_submit_finish:
 * @self: an #AiMediaGenerator
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the job id, or %NULL on error
 */
gchar *
ai_media_generator_submit_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), NULL);

	if (g_async_result_is_tagged(result, ai_media_generator_submit_async))
	{
		return g_task_propagate_pointer(G_TASK(result), error);
	}
	return AI_MEDIA_GENERATOR_GET_IFACE(self)->submit_finish(self, result, error);
}

/**
 * ai_media_generator_poll_async:
 * @self: an #AiMediaGenerator
 * @job_id: an id from ai_media_generator_submit_finish()
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the job's state
 * @user_data: data for @callback
 *
 * Asks once where a job is.  Use ai_media_generator_wait_async() to ask
 * until it finishes.
 */
void
ai_media_generator_poll_async(
	AiMediaGenerator    *self,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMediaGeneratorInterface *iface;

	g_return_if_fail(AI_IS_MEDIA_GENERATOR(self));
	g_return_if_fail(job_id != NULL);

	iface = AI_MEDIA_GENERATOR_GET_IFACE(self);
	if (iface->poll_async == NULL || iface->poll_finish == NULL)
	{
		return_not_supported(self, ai_media_generator_poll_async, "polling jobs",
		                     cancellable, callback, user_data);
		return;
	}
	iface->poll_async(self, job_id, cancellable, callback, user_data);
}

/**
 * ai_media_generator_poll_finish:
 * @self: an #AiMediaGenerator
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * A job that failed is not an error here: it is a snapshot whose state is
 * %AI_MEDIA_JOB_STATE_FAILED.  An error means the question went
 * unanswered (bad credentials, an unknown id, the service down).
 *
 * Returns: (transfer full) (nullable): the job snapshot, or %NULL on error
 */
AiMediaJob *
ai_media_generator_poll_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), NULL);

	if (g_async_result_is_tagged(result, ai_media_generator_poll_async))
	{
		return g_task_propagate_pointer(G_TASK(result), error);
	}
	return AI_MEDIA_GENERATOR_GET_IFACE(self)->poll_finish(self, result, error);
}

/**
 * ai_media_generator_download_async:
 * @self: an #AiMediaGenerator
 * @url: a URL from ai_media_job_get_result_urls()
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the bytes
 * @user_data: data for @callback
 *
 * Fetches one output file.  Result URLs live on the service's storage, not
 * its API, so implementations send no credentials with this request.
 */
void
ai_media_generator_download_async(
	AiMediaGenerator    *self,
	const gchar         *url,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMediaGeneratorInterface *iface;

	g_return_if_fail(AI_IS_MEDIA_GENERATOR(self));
	g_return_if_fail(url != NULL);

	iface = AI_MEDIA_GENERATOR_GET_IFACE(self);
	if (iface->download_async == NULL || iface->download_finish == NULL)
	{
		return_not_supported(self, ai_media_generator_download_async, "downloading results",
		                     cancellable, callback, user_data);
		return;
	}
	iface->download_async(self, url, cancellable, callback, user_data);
}

/**
 * ai_media_generator_download_finish:
 * @self: an #AiMediaGenerator
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the file's bytes, or %NULL on error
 */
GBytes *
ai_media_generator_download_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), NULL);

	if (g_async_result_is_tagged(result, ai_media_generator_download_async))
	{
		return g_task_propagate_pointer(G_TASK(result), error);
	}
	return AI_MEDIA_GENERATOR_GET_IFACE(self)->download_finish(self, result, error);
}

/**
 * ai_media_generator_can_estimate:
 * @self: an #AiMediaGenerator
 *
 * Returns: whether @self can price a request before submitting it
 */
gboolean
ai_media_generator_can_estimate(AiMediaGenerator *self)
{
	AiMediaGeneratorInterface *iface;

	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), FALSE);

	iface = AI_MEDIA_GENERATOR_GET_IFACE(self);
	return iface->estimate_async != NULL && iface->estimate_finish != NULL;
}

/**
 * ai_media_generator_estimate_async:
 * @self: an #AiMediaGenerator
 * @request: the request to price
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the price
 * @user_data: data for @callback
 *
 * Asks what @request would cost, without submitting it.
 */
void
ai_media_generator_estimate_async(
	AiMediaGenerator    *self,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMediaGeneratorInterface *iface;

	g_return_if_fail(AI_IS_MEDIA_GENERATOR(self));
	g_return_if_fail(AI_IS_MEDIA_REQUEST(request));

	iface = AI_MEDIA_GENERATOR_GET_IFACE(self);
	if (!ai_media_generator_can_estimate(self))
	{
		return_not_supported(self, ai_media_generator_estimate_async, "cost estimates",
		                     cancellable, callback, user_data);
		return;
	}
	iface->estimate_async(self, request, cancellable, callback, user_data);
}

/**
 * ai_media_generator_estimate_finish:
 * @self: an #AiMediaGenerator
 * @result: the #GAsyncResult
 * @micro_usd: (out) (optional): the price in millionths of a US dollar
 * @error: return location for a #GError
 *
 * Returns: %TRUE when @micro_usd was set
 */
gboolean
ai_media_generator_estimate_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	gint64            *micro_usd,
	GError           **error
){
	gint64 value = AI_MEDIA_COST_UNKNOWN;
	gboolean ok;

	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), FALSE);

	if (g_async_result_is_tagged(result, ai_media_generator_estimate_async))
	{
		ok = g_task_propagate_boolean(G_TASK(result), error);
	}
	else
	{
		ok = AI_MEDIA_GENERATOR_GET_IFACE(self)->estimate_finish(self, result, &value, error);
	}

	if (micro_usd != NULL)
	{
		*micro_usd = ok ? value : AI_MEDIA_COST_UNKNOWN;
	}
	return ok;
}

/**
 * ai_media_generator_can_cancel:
 * @self: an #AiMediaGenerator
 *
 * Returns: whether @self can cancel a queued job
 */
gboolean
ai_media_generator_can_cancel(AiMediaGenerator *self)
{
	AiMediaGeneratorInterface *iface;

	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), FALSE);

	iface = AI_MEDIA_GENERATOR_GET_IFACE(self);
	return iface->cancel_async != NULL && iface->cancel_finish != NULL;
}

/**
 * ai_media_generator_cancel_async:
 * @self: an #AiMediaGenerator
 * @job_id: the job to cancel
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the service has answered
 * @user_data: data for @callback
 *
 * Asks the service to cancel a job.  Services generally refuse once work
 * has started, and report that as an error.
 */
void
ai_media_generator_cancel_async(
	AiMediaGenerator    *self,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMediaGeneratorInterface *iface;

	g_return_if_fail(AI_IS_MEDIA_GENERATOR(self));
	g_return_if_fail(job_id != NULL);

	iface = AI_MEDIA_GENERATOR_GET_IFACE(self);
	if (!ai_media_generator_can_cancel(self))
	{
		return_not_supported(self, ai_media_generator_cancel_async, "cancelling jobs",
		                     cancellable, callback, user_data);
		return;
	}
	iface->cancel_async(self, job_id, cancellable, callback, user_data);
}

/**
 * ai_media_generator_cancel_finish:
 * @self: an #AiMediaGenerator
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: %TRUE when the service cancelled the job
 */
gboolean
ai_media_generator_cancel_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), FALSE);

	if (g_async_result_is_tagged(result, ai_media_generator_cancel_async))
	{
		return g_task_propagate_boolean(G_TASK(result), error);
	}
	return AI_MEDIA_GENERATOR_GET_IFACE(self)->cancel_finish(self, result, error);
}

/* ---------------------------------------------------------------------- */
/* Waiting                                                                */
/* ---------------------------------------------------------------------- */

typedef struct
{
	gchar   *job_id;
	guint    interval_ms;
	guint    max_interval_ms;
	guint    timeout_ms;
	gint64   deadline_us;          /* monotonic; 0 for none */
	guint    transient_failures;
	GSource *timer;                /* held, destroyed by pointer */
} WaitData;

static void
wait_data_free(gpointer data)
{
	WaitData *wait = data;

	/*
	 * Destroy by pointer, never by id: the timer is attached to the
	 * caller's context, and an id means something only in the context
	 * that issued it.
	 */
	if (wait->timer != NULL)
	{
		g_source_destroy(wait->timer);
		g_clear_pointer(&wait->timer, g_source_unref);
	}
	g_free(wait->job_id);
	g_free(wait);
}

static void wait_poll(GTask *task);

/*
 * Whether a failed poll is worth repeating.  These are the failures the
 * service's own guidance says to retry with backoff; a 401, a 404 or a
 * malformed answer will not improve by asking again.
 */
static gboolean
wait_error_is_transient(const GError *error)
{
	if (error->domain != AI_ERROR)
	{
		return FALSE;
	}

	switch (error->code)
	{
		case AI_ERROR_SERVER_ERROR:
		case AI_ERROR_SERVICE_UNAVAILABLE:
		case AI_ERROR_NETWORK_ERROR:
		case AI_ERROR_TIMEOUT:
		case AI_ERROR_RATE_LIMITED:
			return TRUE;
		default:
			return FALSE;
	}
}

static gboolean
wait_timer_cb(gpointer user_data)
{
	GTask *task = G_TASK(user_data);
	WaitData *wait = g_task_get_task_data(task);

	/* The source is finishing; drop our hold so teardown has nothing to do. */
	g_clear_pointer(&wait->timer, g_source_unref);

	if (!g_task_return_error_if_cancelled(task))
	{
		wait_poll(task);
	}
	return G_SOURCE_REMOVE;
}

/*
 * Arms the next poll.  The delay grows by half each round up to the
 * ceiling, plus up to a fifth of itself as jitter so many waiters started
 * together do not poll in lockstep -- the service's own polling guidance.
 */
static void
wait_schedule(GTask *task)
{
	WaitData *wait = g_task_get_task_data(task);
	GCancellable *cancellable = g_task_get_cancellable(task);
	guint delay_ms;

	if (g_task_return_error_if_cancelled(task))
	{
		return;
	}

	delay_ms = wait->interval_ms + (guint)g_random_int_range(0, (gint32)(wait->interval_ms / 5) + 1);
	wait->interval_ms = MIN(wait->max_interval_ms, wait->interval_ms + wait->interval_ms / 2);

	if (wait->deadline_us != 0)
	{
		gint64 now = g_get_monotonic_time();
		gint64 remaining_ms;

		if (now >= wait->deadline_us)
		{
			g_task_return_new_error(task, AI_ERROR, AI_ERROR_TIMEOUT,
			                        "Media job %s did not finish within %u ms",
			                        wait->job_id, wait->timeout_ms);
			return;
		}
		remaining_ms = (wait->deadline_us - now) / 1000;
		delay_ms = (guint)MIN((gint64)delay_ms, MAX(remaining_ms, (gint64)1));
	}

	wait->timer = g_timeout_source_new(delay_ms);
	g_source_set_static_name(wait->timer, "[ai-glib] media job wait");

	/* A cancellation wakes the wait now rather than at the next tick. */
	if (cancellable != NULL)
	{
		GSource *cancel_source = g_cancellable_source_new(cancellable);

		g_source_set_dummy_callback(cancel_source);
		g_source_add_child_source(wait->timer, cancel_source);
		g_source_unref(cancel_source);
	}

	g_source_set_callback(wait->timer, wait_timer_cb, g_object_ref(task), g_object_unref);
	g_source_attach(wait->timer, g_task_get_context(task));
}

static void
wait_polled_cb(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = G_TASK(user_data);
	WaitData *wait = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;

	job = ai_media_generator_poll_finish(AI_MEDIA_GENERATOR(source), result, &error);
	if (job == NULL)
	{
		if (!g_task_get_cancellable(task) ||
		    !g_cancellable_is_cancelled(g_task_get_cancellable(task)))
		{
			if (wait_error_is_transient(error) &&
			    wait->transient_failures < AI_MEDIA_WAIT_MAX_TRANSIENT_FAILURES)
			{
				wait->transient_failures++;
				g_debug("media job %s: poll failed (%s), retrying",
				        wait->job_id, error->message);
				wait_schedule(task);
				return;
			}
		}
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	wait->transient_failures = 0;
	if (ai_media_job_is_finished(job))
	{
		g_task_return_pointer(task, g_steal_pointer(&job), g_object_unref);
		return;
	}
	wait_schedule(task);
}

/*
 * Issues one poll.  A timer callback is dispatched on the task's context
 * but dispatching does not make that context thread-default, and the
 * implementation's own GTask and HTTP session take whatever is -- so push
 * it, or the answer is delivered to a loop nobody is running.
 */
static void
wait_poll(GTask *task)
{
	WaitData *wait = g_task_get_task_data(task);
	GMainContext *context = g_task_get_context(task);

	g_main_context_push_thread_default(context);
	ai_media_generator_poll_async(AI_MEDIA_GENERATOR(g_task_get_source_object(task)),
	                              wait->job_id,
	                              g_task_get_cancellable(task),
	                              wait_polled_cb,
	                              g_object_ref(task));
	g_main_context_pop_thread_default(context);
}

/**
 * ai_media_generator_wait_async:
 * @self: an #AiMediaGenerator
 * @job_id: the job to wait for
 * @initial_interval_ms: the first delay between polls; 0 for
 *   %AI_MEDIA_WAIT_INITIAL_INTERVAL_MS
 * @max_interval_ms: the longest delay between polls; 0 for
 *   %AI_MEDIA_WAIT_MAX_INTERVAL_MS
 * @timeout_ms: give up after this long; 0 to wait until @cancellable
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called once the job has finished
 * @user_data: data for @callback
 *
 * Polls a job until it finishes, backing off from @initial_interval_ms to
 * @max_interval_ms with jitter.  The first poll is immediate.  Transient
 * failures (5xx, 429, a dropped connection) are retried, up to five in a
 * row; anything else ends the wait with that error.
 *
 * The timers are attached to the thread-default main context of the
 * calling thread, captured here, and the callback runs there.  Cancelling
 * stops the wait, not the job: use ai_media_generator_cancel_async() for
 * that.
 */
void
ai_media_generator_wait_async(
	AiMediaGenerator    *self,
	const gchar         *job_id,
	guint                initial_interval_ms,
	guint                max_interval_ms,
	guint                timeout_ms,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;
	WaitData *wait;

	g_return_if_fail(AI_IS_MEDIA_GENERATOR(self));
	g_return_if_fail(job_id != NULL);

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, ai_media_generator_wait_async);
	g_task_set_static_name(task, "[ai-glib] ai_media_generator_wait_async");

	wait = g_new0(WaitData, 1);
	wait->job_id = g_strdup(job_id);
	wait->interval_ms = (initial_interval_ms != 0) ? initial_interval_ms
	                                               : AI_MEDIA_WAIT_INITIAL_INTERVAL_MS;
	wait->max_interval_ms = (max_interval_ms != 0) ? max_interval_ms
	                                               : AI_MEDIA_WAIT_MAX_INTERVAL_MS;
	wait->max_interval_ms = MAX(wait->max_interval_ms, wait->interval_ms);
	wait->timeout_ms = timeout_ms;
	if (timeout_ms != 0)
	{
		wait->deadline_us = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
	}
	g_task_set_task_data(task, wait, wait_data_free);

	wait_poll(task);
}

/**
 * ai_media_generator_wait_finish:
 * @self: an #AiMediaGenerator
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns the finished job.  As with ai_media_generator_poll_finish(), a
 * job that failed or was cancelled is a result, not an error: check
 * ai_media_job_get_state().  The errors are the wait's own: the deadline
 * (%AI_ERROR_TIMEOUT), cancellation, or a poll that could not be answered.
 *
 * Returns: (transfer full) (nullable): the finished job, or %NULL on error
 */
AiMediaJob *
ai_media_generator_wait_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(AI_IS_MEDIA_GENERATOR(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);
	g_return_val_if_fail(g_async_result_is_tagged(result, ai_media_generator_wait_async), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}
