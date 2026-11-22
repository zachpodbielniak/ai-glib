/*
 * ai-media-generator.h - Asynchronous media jobs: submit, poll, download
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

#include <gio/gio.h>

#include "model/ai-media-request.h"
#include "model/ai-media-job.h"

G_BEGIN_DECLS

/**
 * AI_MEDIA_WAIT_INITIAL_INTERVAL_MS:
 *
 * The first delay between polls when the caller passes 0.
 */
#define AI_MEDIA_WAIT_INITIAL_INTERVAL_MS (2000)

/**
 * AI_MEDIA_WAIT_MAX_INTERVAL_MS:
 *
 * The longest delay between polls when the caller passes 0.
 */
#define AI_MEDIA_WAIT_MAX_INTERVAL_MS (10000)

#define AI_TYPE_MEDIA_GENERATOR (ai_media_generator_get_type())

G_DECLARE_INTERFACE(AiMediaGenerator, ai_media_generator, AI, MEDIA_GENERATOR, GObject)

/**
 * AiMediaGeneratorInterface:
 * @parent_iface: the parent interface
 * @submit_async: queues a job described by a request
 * @submit_finish: returns the job id
 * @poll_async: asks where a job is
 * @poll_finish: returns an #AiMediaJob snapshot
 * @download_async: fetches one result URL
 * @download_finish: returns its bytes
 * @estimate_async: prices a request without submitting it; optional
 * @estimate_finish: returns the price in micro-USD; optional
 * @cancel_async: cancels a job that has not started; optional
 * @cancel_finish: reports whether it was cancelled; optional
 * @_reserved: reserved for future expansion
 *
 * A service that generates media as a queued job: submit returns an id at
 * once, the work happens elsewhere, and the result is a set of URLs that
 * expire.  #AiImageGenerator and #AiVideoGenerator hide that lifecycle and
 * return a finished artifact; this interface exposes it, so a caller can
 * store the id, survive a restart, price a request first, or cancel.
 *
 * Submit, poll and download are required.  Estimate and cancel are
 * optional; without them the wrappers answer %AI_ERROR_NOT_SUPPORTED.
 *
 * Every operation completes on the thread-default main context of the
 * thread that started it, captured when it starts.
 */
struct _AiMediaGeneratorInterface
{
	GTypeInterface parent_iface;

	void         (*submit_async)    (AiMediaGenerator    *self,
	                                 AiMediaRequest      *request,
	                                 GCancellable        *cancellable,
	                                 GAsyncReadyCallback  callback,
	                                 gpointer             user_data);
	gchar *      (*submit_finish)   (AiMediaGenerator    *self,
	                                 GAsyncResult        *result,
	                                 GError             **error);
	void         (*poll_async)      (AiMediaGenerator    *self,
	                                 const gchar         *job_id,
	                                 GCancellable        *cancellable,
	                                 GAsyncReadyCallback  callback,
	                                 gpointer             user_data);
	AiMediaJob * (*poll_finish)     (AiMediaGenerator    *self,
	                                 GAsyncResult        *result,
	                                 GError             **error);
	void         (*download_async)  (AiMediaGenerator    *self,
	                                 const gchar         *url,
	                                 GCancellable        *cancellable,
	                                 GAsyncReadyCallback  callback,
	                                 gpointer             user_data);
	GBytes *     (*download_finish) (AiMediaGenerator    *self,
	                                 GAsyncResult        *result,
	                                 GError             **error);
	void         (*estimate_async)  (AiMediaGenerator    *self,
	                                 AiMediaRequest      *request,
	                                 GCancellable        *cancellable,
	                                 GAsyncReadyCallback  callback,
	                                 gpointer             user_data);
	gboolean     (*estimate_finish) (AiMediaGenerator    *self,
	                                 GAsyncResult        *result,
	                                 gint64              *micro_usd,
	                                 GError             **error);
	void         (*cancel_async)    (AiMediaGenerator    *self,
	                                 const gchar         *job_id,
	                                 GCancellable        *cancellable,
	                                 GAsyncReadyCallback  callback,
	                                 gpointer             user_data);
	gboolean     (*cancel_finish)   (AiMediaGenerator    *self,
	                                 GAsyncResult        *result,
	                                 GError             **error);

	/* Reserved for future expansion */
	gpointer _reserved[8];
};

void
ai_media_generator_submit_async(
	AiMediaGenerator    *self,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gchar *
ai_media_generator_submit_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
);

void
ai_media_generator_poll_async(
	AiMediaGenerator    *self,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

AiMediaJob *
ai_media_generator_poll_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
);

void
ai_media_generator_download_async(
	AiMediaGenerator    *self,
	const gchar         *url,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

GBytes *
ai_media_generator_download_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
);

gboolean
ai_media_generator_can_estimate(
	AiMediaGenerator *self
);

void
ai_media_generator_estimate_async(
	AiMediaGenerator    *self,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gboolean
ai_media_generator_estimate_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	gint64            *micro_usd,
	GError           **error
);

gboolean
ai_media_generator_can_cancel(
	AiMediaGenerator *self
);

void
ai_media_generator_cancel_async(
	AiMediaGenerator    *self,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

gboolean
ai_media_generator_cancel_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
);

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
);

AiMediaJob *
ai_media_generator_wait_finish(
	AiMediaGenerator  *self,
	GAsyncResult      *result,
	GError           **error
);

G_END_DECLS
