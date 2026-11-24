/*
 * ai-media-job.h - The state of one asynchronous media generation job
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

G_BEGIN_DECLS

/**
 * AI_MEDIA_COST_UNKNOWN:
 *
 * The cost value meaning "the service did not say".  Never read it as
 * free: zero is a real price and this is not one.
 */
#define AI_MEDIA_COST_UNKNOWN ((gint64)-1)

/**
 * AiMediaJobState:
 * @AI_MEDIA_JOB_STATE_QUEUED: accepted and waiting to start
 * @AI_MEDIA_JOB_STATE_RUNNING: generation has started
 * @AI_MEDIA_JOB_STATE_DONE: finished; the result URLs are set
 * @AI_MEDIA_JOB_STATE_FAILED: finished without a result, including a
 *   moderation refusal (see ai_media_job_get_provider_status())
 * @AI_MEDIA_JOB_STATE_CANCELLED: cancelled before it started
 *
 * Where a job is.  The last three are terminal.
 */
typedef enum
{
	AI_MEDIA_JOB_STATE_QUEUED,
	AI_MEDIA_JOB_STATE_RUNNING,
	AI_MEDIA_JOB_STATE_DONE,
	AI_MEDIA_JOB_STATE_FAILED,
	AI_MEDIA_JOB_STATE_CANCELLED
} AiMediaJobState;

GType
ai_media_job_state_get_type(void) G_GNUC_CONST;
#define AI_TYPE_MEDIA_JOB_STATE (ai_media_job_state_get_type())

gboolean
ai_media_job_state_is_finished(
	AiMediaJobState state
);

const gchar *
ai_media_job_state_to_string(
	AiMediaJobState state
);

#define AI_TYPE_MEDIA_JOB (ai_media_job_get_type())

G_DECLARE_FINAL_TYPE(AiMediaJob, ai_media_job, AI, MEDIA_JOB, GObject)

AiMediaJob *
ai_media_job_new(
	const gchar     *id,
	AiMediaJobState  state
);

const gchar *
ai_media_job_get_id(
	AiMediaJob *self
);

AiMediaJobState
ai_media_job_get_state(
	AiMediaJob *self
);

gboolean
ai_media_job_is_finished(
	AiMediaJob *self
);

const gchar *
ai_media_job_get_provider_status(
	AiMediaJob *self
);

void
ai_media_job_set_provider_status(
	AiMediaJob  *self,
	const gchar *status
);

const gchar *
ai_media_job_get_error_message(
	AiMediaJob *self
);

void
ai_media_job_set_error_message(
	AiMediaJob  *self,
	const gchar *message
);

void
ai_media_job_add_result_url(
	AiMediaJob  *self,
	const gchar *url
);

const gchar * const *
ai_media_job_get_result_urls(
	AiMediaJob *self
);

guint
ai_media_job_get_n_result_urls(
	AiMediaJob *self
);

void
ai_media_job_add_named_result(
	AiMediaJob  *self,
	const gchar *name,
	const gchar *url
);

const gchar * const *
ai_media_job_get_result_names(
	AiMediaJob *self
);

const gchar *
ai_media_job_get_result_url(
	AiMediaJob  *self,
	const gchar *name
);

gint64
ai_media_job_get_credits(
	AiMediaJob *self
);

void
ai_media_job_set_credits(
	AiMediaJob *self,
	gint64      credits
);

gint64
ai_media_job_get_cost_micro_usd(
	AiMediaJob *self
);

void
ai_media_job_set_cost_micro_usd(
	AiMediaJob *self,
	gint64      micro_usd
);

const gchar *
ai_media_job_get_expires_at(
	AiMediaJob *self
);

void
ai_media_job_set_expires_at(
	AiMediaJob  *self,
	const gchar *expires_at
);

G_END_DECLS
