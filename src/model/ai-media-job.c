/*
 * ai-media-job.c - The state of one asynchronous media generation job
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A snapshot, not a live handle: each poll produces a new one.  The
 * provider's own status word is kept beside the normalised state because
 * the normalisation loses something a caller may need -- Higgsfield's
 * `nsfw` and `failed` are both FAILED, are both refunded, and call for
 * different follow-ups.
 */

#include "config.h"

#include "model/ai-media-job.h"

struct _AiMediaJob
{
	GObject parent_instance;

	gchar           *id;
	AiMediaJobState  state;
	gchar           *provider_status;
	gchar           *error_message;
	GPtrArray       *result_urls;   /* NULL-terminated, of gchar * */
	GPtrArray       *result_names;  /* NULL-terminated, of gchar *, unique */
	GHashTable      *named_results; /* name -> URL */
	gint64           cost_micro_usd;
	gint64           credits;
	gchar           *expires_at;
};

G_DEFINE_FINAL_TYPE(AiMediaJob, ai_media_job, G_TYPE_OBJECT)

/*
 * GType registration for AiMediaJobState, so bindings see the names.
 */
GType
ai_media_job_state_get_type(void)
{
	static GType state_type = 0;

	if (g_once_init_enter(&state_type))
	{
		static const GEnumValue values[] = {
			{ AI_MEDIA_JOB_STATE_QUEUED, "AI_MEDIA_JOB_STATE_QUEUED", "queued" },
			{ AI_MEDIA_JOB_STATE_RUNNING, "AI_MEDIA_JOB_STATE_RUNNING", "running" },
			{ AI_MEDIA_JOB_STATE_DONE, "AI_MEDIA_JOB_STATE_DONE", "done" },
			{ AI_MEDIA_JOB_STATE_FAILED, "AI_MEDIA_JOB_STATE_FAILED", "failed" },
			{ AI_MEDIA_JOB_STATE_CANCELLED, "AI_MEDIA_JOB_STATE_CANCELLED", "cancelled" },
			{ 0, NULL, NULL }
		};
		GType type = g_enum_register_static("AiMediaJobState", values);

		g_once_init_leave(&state_type, type);
	}

	return state_type;
}

/**
 * ai_media_job_state_is_finished:
 * @state: an #AiMediaJobState
 *
 * Whether @state is terminal.  Every value is named, with no default, so
 * a state added later is a -Wswitch warning here rather than a job some
 * waiter polls for ever.
 *
 * Returns: %TRUE for done, failed and cancelled
 */
gboolean
ai_media_job_state_is_finished(AiMediaJobState state)
{
	switch (state)
	{
		case AI_MEDIA_JOB_STATE_QUEUED:
		case AI_MEDIA_JOB_STATE_RUNNING:
			return FALSE;
		case AI_MEDIA_JOB_STATE_DONE:
		case AI_MEDIA_JOB_STATE_FAILED:
		case AI_MEDIA_JOB_STATE_CANCELLED:
			return TRUE;
	}

	return FALSE;
}

/**
 * ai_media_job_state_to_string:
 * @state: an #AiMediaJobState
 *
 * Returns: (transfer none): the state's nickname, such as "running"
 */
const gchar *
ai_media_job_state_to_string(AiMediaJobState state)
{
	switch (state)
	{
		case AI_MEDIA_JOB_STATE_QUEUED:
			return "queued";
		case AI_MEDIA_JOB_STATE_RUNNING:
			return "running";
		case AI_MEDIA_JOB_STATE_DONE:
			return "done";
		case AI_MEDIA_JOB_STATE_FAILED:
			return "failed";
		case AI_MEDIA_JOB_STATE_CANCELLED:
			return "cancelled";
	}

	return "unknown";
}

static void
ai_media_job_finalize(GObject *object)
{
	AiMediaJob *self = AI_MEDIA_JOB(object);

	g_clear_pointer(&self->id, g_free);
	g_clear_pointer(&self->provider_status, g_free);
	g_clear_pointer(&self->error_message, g_free);
	g_clear_pointer(&self->result_urls, g_ptr_array_unref);
	g_clear_pointer(&self->result_names, g_ptr_array_unref);
	g_clear_pointer(&self->named_results, g_hash_table_unref);
	g_clear_pointer(&self->expires_at, g_free);

	G_OBJECT_CLASS(ai_media_job_parent_class)->finalize(object);
}

static void
ai_media_job_class_init(AiMediaJobClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize = ai_media_job_finalize;
}

static void
ai_media_job_init(AiMediaJob *self)
{
	self->result_urls = g_ptr_array_new_null_terminated(2, g_free, TRUE);
	self->result_names = g_ptr_array_new_null_terminated(2, g_free, TRUE);
	self->named_results = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	self->cost_micro_usd = AI_MEDIA_COST_UNKNOWN;
	self->credits = AI_MEDIA_COST_UNKNOWN;
}

/**
 * ai_media_job_new:
 * @id: the service's job id
 * @state: where the job is
 *
 * Creates a job snapshot.  Implementations of #AiMediaGenerator build one
 * per poll; callers normally only read them.
 *
 * Returns: (transfer full): a new #AiMediaJob
 */
AiMediaJob *
ai_media_job_new(
	const gchar     *id,
	AiMediaJobState  state
){
	g_autoptr(AiMediaJob) self = NULL;

	g_return_val_if_fail(id != NULL, NULL);

	self = g_object_new(AI_TYPE_MEDIA_JOB, NULL);
	self->id = g_strdup(id);
	self->state = state;

	return (AiMediaJob *)g_steal_pointer(&self);
}

/**
 * ai_media_job_get_id:
 * @self: an #AiMediaJob
 *
 * Returns: (transfer none): the service's job id
 */
const gchar *
ai_media_job_get_id(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), NULL);

	return self->id;
}

/**
 * ai_media_job_get_state:
 * @self: an #AiMediaJob
 *
 * Returns: where the job was when this snapshot was taken
 */
AiMediaJobState
ai_media_job_get_state(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), AI_MEDIA_JOB_STATE_FAILED);

	return self->state;
}

/**
 * ai_media_job_is_finished:
 * @self: an #AiMediaJob
 *
 * Returns: whether the job's state is terminal
 */
gboolean
ai_media_job_is_finished(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), TRUE);

	return ai_media_job_state_is_finished(self->state);
}

/**
 * ai_media_job_get_provider_status:
 * @self: an #AiMediaJob
 *
 * The status word the service itself sent, such as Higgsfield's `nsfw`,
 * which the normalised state folds into %AI_MEDIA_JOB_STATE_FAILED.
 *
 * Returns: (transfer none) (nullable): the service's status
 */
const gchar *
ai_media_job_get_provider_status(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), NULL);

	return self->provider_status;
}

/**
 * ai_media_job_set_provider_status:
 * @self: an #AiMediaJob
 * @status: (nullable): the service's status word
 *
 * Records the status word the service sent.
 */
void
ai_media_job_set_provider_status(
	AiMediaJob  *self,
	const gchar *status
){
	g_return_if_fail(AI_IS_MEDIA_JOB(self));

	g_set_str(&self->provider_status, status);
}

/**
 * ai_media_job_get_error_message:
 * @self: an #AiMediaJob
 *
 * Returns: (transfer none) (nullable): why a failed job failed, if known
 */
const gchar *
ai_media_job_get_error_message(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), NULL);

	return self->error_message;
}

/**
 * ai_media_job_set_error_message:
 * @self: an #AiMediaJob
 * @message: (nullable): why the job failed
 *
 * Records why the job failed.
 */
void
ai_media_job_set_error_message(
	AiMediaJob  *self,
	const gchar *message
){
	g_return_if_fail(AI_IS_MEDIA_JOB(self));

	g_set_str(&self->error_message, message);
}

/**
 * ai_media_job_add_result_url:
 * @self: an #AiMediaJob
 * @url: a URL of one output file
 *
 * Appends an output URL.
 */
void
ai_media_job_add_result_url(
	AiMediaJob  *self,
	const gchar *url
){
	g_return_if_fail(AI_IS_MEDIA_JOB(self));
	g_return_if_fail(url != NULL);

	g_ptr_array_add(self->result_urls, g_strdup(url));
}

/**
 * ai_media_job_get_result_urls:
 * @self: an #AiMediaJob
 *
 * The output files, in the order the service listed them.  Empty until
 * the job is done.  They are the service's URLs and expire (see
 * ai_media_job_get_expires_at()); download what you want to keep.
 *
 * Returns: (transfer none) (array zero-terminated=1): the result URLs
 */
const gchar * const *
ai_media_job_get_result_urls(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), NULL);

	return (const gchar * const *)self->result_urls->pdata;
}

/**
 * ai_media_job_get_n_result_urls:
 * @self: an #AiMediaJob
 *
 * Returns: how many result URLs there are
 */
guint
ai_media_job_get_n_result_urls(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), 0);

	return self->result_urls->len;
}

/**
 * ai_media_job_add_named_result:
 * @self: an #AiMediaJob
 * @name: what the file is, such as "glb", "thumbnail" or "south"
 * @url: its URL
 *
 * Records an output under a name, for a service whose outputs are not
 * interchangeable -- a mesh and its thumbnail, eight directions of a
 * sprite.  The URL is also appended to the plain list, so a caller that
 * only reads ai_media_job_get_result_urls() sees every file.
 *
 * Names are unique.  Adding a name a second time points it at the new
 * URL; the old URL stays in the plain list, because that list is a
 * record of what the service returned rather than a lookup.
 */
void
ai_media_job_add_named_result(
	AiMediaJob  *self,
	const gchar *name,
	const gchar *url
){
	g_return_if_fail(AI_IS_MEDIA_JOB(self));
	g_return_if_fail(name != NULL && name[0] != '\0');
	g_return_if_fail(url != NULL);

	if (!g_hash_table_contains(self->named_results, name))
	{
		g_ptr_array_add(self->result_names, g_strdup(name));
	}
	g_hash_table_insert(self->named_results, g_strdup(name), g_strdup(url));
	g_ptr_array_add(self->result_urls, g_strdup(url));
}

/**
 * ai_media_job_get_result_names:
 * @self: an #AiMediaJob
 *
 * The names of the outputs recorded with ai_media_job_add_named_result(),
 * in the order they were first added.  Empty for a service that only
 * lists URLs (Higgsfield, ComfyUI).
 *
 * Returns: (transfer none) (array zero-terminated=1): the result names
 */
const gchar * const *
ai_media_job_get_result_names(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), NULL);

	return (const gchar * const *)self->result_names->pdata;
}

/**
 * ai_media_job_get_result_url:
 * @self: an #AiMediaJob
 * @name: an output name
 *
 * Returns: (transfer none) (nullable): the URL recorded under @name, or
 *   %NULL when the job has no output of that name
 */
const gchar *
ai_media_job_get_result_url(
	AiMediaJob  *self,
	const gchar *name
){
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), NULL);
	g_return_val_if_fail(name != NULL, NULL);

	return g_hash_table_lookup(self->named_results, name);
}

/**
 * ai_media_job_get_credits:
 * @self: an #AiMediaJob
 *
 * What the job cost in the service's own unit, when it bills in one
 * rather than in money: Meshy's credits, PixelLab's subscription
 * generations.  Kept apart from ai_media_job_get_cost_micro_usd() because
 * the exchange rate depends on the account's plan, which the job does
 * not know.
 *
 * Returns: the amount, or %AI_MEDIA_COST_UNKNOWN
 */
gint64
ai_media_job_get_credits(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), AI_MEDIA_COST_UNKNOWN);

	return self->credits;
}

/**
 * ai_media_job_set_credits:
 * @self: an #AiMediaJob
 * @credits: the amount, or %AI_MEDIA_COST_UNKNOWN
 *
 * Records what the job cost in the service's own unit.
 */
void
ai_media_job_set_credits(
	AiMediaJob *self,
	gint64      credits
){
	g_return_if_fail(AI_IS_MEDIA_JOB(self));

	self->credits = credits;
}

/**
 * ai_media_job_get_cost_micro_usd:
 * @self: an #AiMediaJob
 *
 * What the job cost, in millionths of a US dollar, when the service
 * reports it.  Higgsfield's status response does not, so its jobs carry
 * %AI_MEDIA_COST_UNKNOWN; ask ai_media_generator_estimate_async() before
 * submitting instead.
 *
 * Returns: the cost, or %AI_MEDIA_COST_UNKNOWN
 */
gint64
ai_media_job_get_cost_micro_usd(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), AI_MEDIA_COST_UNKNOWN);

	return self->cost_micro_usd;
}

/**
 * ai_media_job_set_cost_micro_usd:
 * @self: an #AiMediaJob
 * @micro_usd: the cost, or %AI_MEDIA_COST_UNKNOWN
 *
 * Records what the job cost.
 */
void
ai_media_job_set_cost_micro_usd(
	AiMediaJob *self,
	gint64      micro_usd
){
	g_return_if_fail(AI_IS_MEDIA_JOB(self));

	self->cost_micro_usd = micro_usd;
}

/**
 * ai_media_job_get_expires_at:
 * @self: an #AiMediaJob
 *
 * When the service deletes the job's files, as the ISO 8601 text it sent.
 *
 * Returns: (transfer none) (nullable): the expiry time, if known
 */
const gchar *
ai_media_job_get_expires_at(AiMediaJob *self)
{
	g_return_val_if_fail(AI_IS_MEDIA_JOB(self), NULL);

	return self->expires_at;
}

/**
 * ai_media_job_set_expires_at:
 * @self: an #AiMediaJob
 * @expires_at: (nullable): an ISO 8601 time
 *
 * Records when the job's files expire.
 */
void
ai_media_job_set_expires_at(
	AiMediaJob  *self,
	const gchar *expires_at
){
	g_return_if_fail(AI_IS_MEDIA_JOB(self));

	g_set_str(&self->expires_at, expires_at);
}
