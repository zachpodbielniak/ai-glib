/*
 * ai-meshy-client.c - Meshy 3D generation API
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Meshy is a hosted, closed, credit-billed service that turns text and
 * images into textured, rigged and animated 3D models.  Every operation
 * is a task: POST a JSON body to the operation's path, get a task id back
 * (`{"result": "<id>"}`), GET `<path>/<id>` until the status is terminal,
 * then fetch the signed result URLs from its CDN within three days.
 * docs/providers/meshy.org records every fact this file relies on, with
 * the page it was checked against.
 *
 * Shape of the code:
 *
 * - One AiMediaGenerator job is one Meshy task, except Text to 3D, which
 *   Meshy splits into a preview (the mesh) and a refine (the texture).  A
 *   `text-to-3d` job without an explicit `mode` runs both: the poll that
 *   sees the preview succeed creates the refine, and from then on the job
 *   id names both tasks (`text-to-3d:<preview>:<refine>`), so a caller
 *   that stores the id from the last poll can resume after a restart.  The
 *   refine's parameters are only held in memory between the two, so a
 *   preview-only id polled by a fresh process is refused rather than
 *   refined with the wrong texture or refined twice.
 * - The job id carries the operation (`<operation>:<task id>`), because
 *   Meshy has no single status endpoint: each operation polls its own path.
 * - The credential goes to the API origin and nowhere else, never follows
 *   a redirect, and never appears in an error (ms_error_new() redacts).
 *   Nothing is read from the environment.
 */

#include "config.h"

#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "providers/ai-meshy-client.h"
#include "providers/ai-media-shared.h"
#include "core/ai-error.h"
#include "core/ai-http-redirect-private.h"

/* Default whole-request timeout, in seconds. */
#define MS_DEFAULT_TIMEOUT (60)

/* Meshy task ids are UUIDs; this leaves room and refuses anything silly. */
#define MS_MAX_TASK_ID (64)

/* The job-id prefix for a Text to 3D task the caller staged themselves. */
#define MS_TEXT_TO_3D_STEP "text-to-3d.step"

/* What Text to 3D's refine stage accepts, copied from the request. */
static const gchar * const ms_refine_keys[] = {
	"enable_pbr", "texture_resolution", "hd_texture", "texture_prompt",
	"texture_image_url", "moderation", "remove_lighting", "target_formats",
	"alpha_thumbnail", "name", NULL
};

/* What a preview refuses ("A preview with this parameter returns 400"). */
static const gchar * const ms_refine_only_keys[] = {
	"texture_resolution", "hd_texture", "texture_model", NULL
};

typedef struct
{
	const gchar *name;
	const gchar *path;
	gboolean     public_name;   /* may a caller ask for it by name */
} MsOperation;

static const MsOperation ms_operations[] = {
	{ AI_MESHY_OPERATION_TEXT_TO_3D,        "openapi/v2/text-to-3d",        TRUE  },
	{ MS_TEXT_TO_3D_STEP,                   "openapi/v2/text-to-3d",        FALSE },
	{ AI_MESHY_OPERATION_IMAGE_TO_3D,       "openapi/v1/image-to-3d",       TRUE  },
	{ AI_MESHY_OPERATION_MULTI_IMAGE_TO_3D, "openapi/v1/multi-image-to-3d", TRUE  },
	{ AI_MESHY_OPERATION_RETEXTURE,         "openapi/v1/retexture",         TRUE  },
	{ AI_MESHY_OPERATION_REMESH,            "openapi/v1/remesh",            TRUE  },
	{ AI_MESHY_OPERATION_RIG,               "openapi/v1/rigging",           TRUE  },
	{ AI_MESHY_OPERATION_ANIMATE,           "openapi/v1/animations",        TRUE  },
	{ NULL, NULL, FALSE }
};

/*
 * What a chained Text to 3D job still has to do after its preview.  Held
 * per preview id, under the client's lock: two polls of one job can be in
 * flight at once, and only one of them may create the refine.
 */
typedef struct
{
	gchar    *refine_json;      /* the refine body, without mode or preview id */
	gchar    *refine_id;        /* once created */
	gboolean  refine_pending;   /* a POST is in flight */
} MsPlan;

struct _AiMeshyClient
{
	GObject parent_instance;

	gchar        *base_url;
	gchar        *api_key;
	guint         timeout;

	/* See the note in ai-higgsfield-client.c: a session belongs to its context. */
	SoupSession  *session;
	GMainContext *session_context;

	GMutex        lock;
	GHashTable   *plans;        /* preview id -> MsPlan */
};

static void media_generator_iface_init(AiMediaGeneratorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(AiMeshyClient, ai_meshy_client, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(AI_TYPE_MEDIA_GENERATOR,
                                                    media_generator_iface_init))

enum
{
	PROP_0,
	PROP_BASE_URL,
	PROP_TIMEOUT,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

static void
ms_plan_free(gpointer data)
{
	MsPlan *plan = data;

	g_free(plan->refine_json);
	g_free(plan->refine_id);
	g_free(plan);
}

/* ---------------------------------------------------------------------- */
/* Errors                                                                 */
/* ---------------------------------------------------------------------- */

/* A GError from a message, with the API key removed on the way out. */
static GError *
ms_error_new(
	AiMeshyClient *self,
	gint           code,
	gchar         *message
){
	GError *error;

	message = ai_media_shared_redact(message, self->api_key);
	error = g_error_new_literal(AI_ERROR, code, message);
	g_free(message);

	return error;
}

/*
 * Maps a non-2xx status onto an AiError, naming what Meshy's
 * documentation says each one means.  @job_scoped says whether a 404 is
 * "no such task" (poll, cancel) rather than a bad request.
 */
static GError *
ms_status_error(
	AiMeshyClient *self,
	const gchar   *what,
	guint          status,
	GBytes        *body,
	SoupMessage   *message,
	gboolean       job_scoped
){
	g_autofree gchar *detail = ai_media_shared_error_detail(body);
	const gchar *retry_after = NULL;
	const gchar *hint = "";
	gint code;

	if (SOUP_STATUS_IS_REDIRECTION(status))
	{
		return ms_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                    g_strdup_printf("Meshy %s answered with a redirect (HTTP %u), "
		                                    "which is not followed so the API key stays on "
		                                    "the API origin", what, status));
	}

	switch (status)
	{
		case 400:
		case 422:
			code = AI_ERROR_INVALID_REQUEST;
			break;
		case 401:
			code = AI_ERROR_INVALID_API_KEY;
			break;
		case 402:
			code = AI_ERROR_INSUFFICIENT_QUOTA;
			hint = " -- not enough credits, or the plan does not allow this";
			break;
		case 403:
			code = AI_ERROR_PERMISSION_DENIED;
			break;
		case 404:
			code = AI_ERROR_INVALID_REQUEST;
			if (job_scoped)
			{
				hint = " -- no such task; Meshy deletes tasks three days after they finish";
			}
			break;
		case 409:
			code = AI_ERROR_INVALID_REQUEST;
			hint = " -- the task is already IN_PROGRESS: Meshy does not delete a task "
			       "whose credits are spent, so it runs to the end";
			break;
		case 429:
			code = AI_ERROR_RATE_LIMITED;
			if (message != NULL)
			{
				retry_after = soup_message_headers_get_one(
					soup_message_get_response_headers(message), "Retry-After");
			}
			if (retry_after == NULL)
			{
				/* Documented: the concurrency cap's 429 has no Retry-After. */
				hint = " -- the account's limit on concurrent tasks is reached; "
				       "wait for one of its tasks to finish before creating another";
			}
			break;
		case 503:
			code = AI_ERROR_SERVICE_UNAVAILABLE;
			break;
		default:
			code = (status >= 500) ? AI_ERROR_SERVER_ERROR : AI_ERROR_UNKNOWN;
			break;
	}

	return ms_error_new(self, code,
	                    g_strdup_printf("Meshy %s failed (HTTP %u)%s%s%s%s%s%s",
	                                    what, status,
	                                    (detail != NULL) ? ": " : "",
	                                    (detail != NULL) ? detail : "",
	                                    hint,
	                                    (retry_after != NULL) ? " (rate limited; retry after " : "",
	                                    (retry_after != NULL) ? retry_after : "",
	                                    (retry_after != NULL) ? " s)" : ""));
}

/* Parses a 2xx body that must be a JSON object, or explains why not. */
static JsonObject *
ms_parse_object(
	AiMeshyClient  *self,
	const gchar    *what,
	GBytes         *body,
	GError        **error
){
	g_autofree gchar *why = NULL;
	JsonObject *object = ai_media_shared_parse_object(body, &why);

	if (object == NULL)
	{
		g_propagate_error(error,
		                  ms_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                               g_strdup_printf("Meshy %s returned %s", what, why)));
	}
	return object;
}

/* ---------------------------------------------------------------------- */
/* Operations and job ids                                                 */
/* ---------------------------------------------------------------------- */

static const MsOperation *
ms_operation_lookup(
	const gchar *name,
	gboolean     internal_too
){
	guint i;

	if (name == NULL)
	{
		return NULL;
	}
	for (i = 0; ms_operations[i].name != NULL; i++)
	{
		if ((internal_too || ms_operations[i].public_name) &&
		    g_strcmp0(ms_operations[i].name, name) == 0)
		{
			return &ms_operations[i];
		}
	}
	return NULL;
}

/* The operation names a caller may use, for an error message. */
static gchar *
ms_operation_names(void)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; ms_operations[i].name != NULL; i++)
	{
		if (!ms_operations[i].public_name)
		{
			continue;
		}
		if (out->len > 0)
		{
			g_string_append(out, ", ");
		}
		g_string_append(out, ms_operations[i].name);
	}
	return g_string_free(out, FALSE);
}

/*
 * The request as a mutable JSON object, with the operation it names taken
 * out of it: the `operation` parameter if there is one, else the model.
 * `operation` is ai-glib's word, not Meshy's, so it never reaches the wire.
 */
static JsonObject *
ms_request_body(
	AiMediaRequest     *request,
	const MsOperation **operation,
	GError            **error
){
	g_autofree gchar *json = ai_media_request_to_json(request);
	g_autoptr(JsonObject) body = ai_media_shared_object_from_text(json);
	const gchar *name = NULL;

	if (body == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "The Meshy request could not be serialised as a JSON object");
		return NULL;
	}

	if (json_object_has_member(body, "operation"))
	{
		JsonNode *node = json_object_get_member(body, "operation");

		if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING)
		{
			name = json_node_get_string(node);
		}
	}
	else
	{
		name = ai_media_request_get_model(request);
	}

	*operation = ms_operation_lookup(name, FALSE);
	if (*operation == NULL)
	{
		g_autofree gchar *names = ms_operation_names();

		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "'%s' is not a Meshy operation; set the request's 'operation' "
		            "parameter (or its model) to one of: %s",
		            (name != NULL) ? name : "(none)", names);
		return NULL;
	}
	json_object_remove_member(body, "operation");

	return (JsonObject *)g_steal_pointer(&body);
}

/*
 * Splits `<operation>:<task>[:<refine>]` into its parts, checking every
 * one: the ids are spliced into URL paths.
 */
static gboolean
ms_parse_job_id(
	const gchar        *job_id,
	const MsOperation **operation,
	gchar             **task_id,
	gchar             **refine_id
){
	g_auto(GStrv) parts = NULL;
	guint n;

	if (job_id == NULL || strlen(job_id) > 3 * MS_MAX_TASK_ID)
	{
		return FALSE;
	}
	parts = g_strsplit(job_id, ":", 4);
	n = g_strv_length(parts);
	if (n < 2 || n > 3)
	{
		return FALSE;
	}

	*operation = ms_operation_lookup(parts[0], TRUE);
	if (*operation == NULL || !ai_media_shared_id_is_valid(parts[1], MS_MAX_TASK_ID))
	{
		return FALSE;
	}
	/* Only a chained Text to 3D job has a second task. */
	if (n == 3 && (g_strcmp0((*operation)->name, AI_MESHY_OPERATION_TEXT_TO_3D) != 0 ||
	               !ai_media_shared_id_is_valid(parts[2], MS_MAX_TASK_ID)))
	{
		return FALSE;
	}

	*task_id = g_strdup(parts[1]);
	*refine_id = (n == 3) ? g_strdup(parts[2]) : NULL;
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                */
/* ---------------------------------------------------------------------- */

typedef void (*MsReply)(GTask *task, guint status, GBytes *body);

typedef struct
{
	SoupSession       *session;
	SoupMessage       *message;
	MsReply            reply;
	const gchar       *what;        /* static: names the call in messages */

	const MsOperation *operation;
	gchar             *task_id;     /* the task the job id names first */
	gchar             *refine_id;   /* a chained Text to 3D job's refine */
	JsonObject        *refine_task; /* the refine, while its preview is fetched */
} MsOp;

static void
ms_op_free(gpointer data)
{
	MsOp *op = data;

	g_clear_object(&op->session);
	g_clear_object(&op->message);
	g_free(op->task_id);
	g_free(op->refine_id);
	g_clear_pointer(&op->refine_task, json_object_unref);
	g_free(op);
}

/*
 * The session for an operation that started on @context, which is
 * thread-default here because every caller has just made its GTask.
 */
static SoupSession *
ms_session_for(
	AiMeshyClient *self,
	GMainContext  *context
){
	SoupSession *session;

	if (self->session != NULL && self->session_context == context)
	{
		return g_object_ref(self->session);
	}

	session = ai_http_session_new();
	g_object_set(session,
	             "timeout", self->timeout,
	             "user-agent", "ai-glib/" AI_GLIB_PACKAGE_VERSION,
	             NULL);
	if (self->session == NULL)
	{
		self->session = g_object_ref(session);
		self->session_context = g_main_context_ref(context);
	}
	return session;
}

static GTask *
ms_task_new(
	AiMeshyClient       *self,
	gpointer             tag,
	const gchar         *what,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	MsOp *op;

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, tag);
	g_task_set_static_name(task, what);

	op = g_new0(MsOp, 1);
	op->what = what;
	op->session = ms_session_for(self, g_task_get_context(task));
	g_task_set_task_data(task, op, ms_op_free);

	return task;
}

/*
 * An authenticated request to `<base-url>/<path>[?query]`.  The bearer
 * header is the only place the key is written, and the message never
 * follows a redirect.
 */
static SoupMessage *
ms_api_message(
	AiMeshyClient  *self,
	const gchar    *method,
	const gchar    *path,
	const gchar    *query,
	JsonObject     *json_body,
	GError        **error
){
	g_autofree gchar *url = NULL;
	g_autoptr(GUri) uri = NULL;
	gchar *authorization;
	SoupMessage *message;
	SoupMessageHeaders *headers;

	if (self->api_key == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
		                    "No Meshy API key: call ai_meshy_client_set_api_key() with a "
		                    "key from meshy.ai/developers/keys");
		return NULL;
	}

	url = ai_media_shared_join_url(self->base_url, path, query);
	uri = ai_media_shared_parse_http_uri(url);
	if (uri == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		            "Meshy base URL '%s' is not an http or https URL", self->base_url);
		return NULL;
	}

	message = ai_http_message_new_from_uri(method, uri, AI_HTTP_CARRIES_CREDENTIALS);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	authorization = g_strdup_printf("Bearer %s", self->api_key);
	soup_message_headers_replace(headers, "Authorization", authorization);
	ai_media_shared_wipe_free(authorization);
	soup_message_headers_replace(headers, "Accept", "application/json");

	if (json_body != NULL)
	{
		g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
		g_autoptr(GBytes) bytes = NULL;
		gchar *text;

		json_node_set_object(root, json_body);
		text = json_to_string(root, FALSE);
		bytes = g_bytes_new_take(text, strlen(text));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}

	return message;
}

static void
ms_send_cb(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = G_TASK(user_data);
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;

	body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);
	if (body == NULL)
	{
		if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		{
			g_task_return_error(task, (GError *)g_steal_pointer(&error));
			return;
		}
		g_task_return_error(task,
		                    ms_error_new(self,
		                                 g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)
		                                     ? AI_ERROR_TIMEOUT : AI_ERROR_NETWORK_ERROR,
		                                 g_strdup_printf("Meshy %s failed: %s",
		                                                 op->what, error->message)));
		return;
	}

	op->reply(task, soup_message_get_status(op->message), body);
}

/* Sends @message (transfer full) for @task; @reply gets the answer. */
static void
ms_send(
	GTask       *task,
	SoupMessage *message,
	MsReply      reply
){
	MsOp *op = g_task_get_task_data(task);

	g_set_object(&op->message, message);
	g_object_unref(message);
	op->reply = reply;

	g_debug("meshy: %s %s", soup_message_get_method(message), op->what);
	soup_session_send_and_read_async(op->session, op->message, G_PRIORITY_DEFAULT,
	                                 g_task_get_cancellable(task),
	                                 ms_send_cb, g_object_ref(task));
}

/* Builds and sends an API request, or returns the build error on @task. */
static void
ms_send_api(
	GTask       *task,
	const gchar *method,
	const gchar *path,
	const gchar *query,
	JsonObject  *json_body,
	MsReply      reply
){
	AiMeshyClient *self = g_task_get_source_object(task);
	g_autoptr(GError) error = NULL;
	SoupMessage *message;

	message = ms_api_message(self, method, path, query, json_body, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	ms_send(task, message, reply);
}

/* ---------------------------------------------------------------------- */
/* Request checks                                                         */
/* ---------------------------------------------------------------------- */

/* The decoded size of a base64 data URI, from its length alone. */
static gsize
ms_data_uri_size(const gchar *uri)
{
	const gchar *comma = strchr(uri, ',');
	gsize length;

	if (comma == NULL)
	{
		return 0;
	}
	length = strlen(comma + 1);
	return (length / 4) * 3;
}

/* Refuses an inline image larger than Meshy's 20 MB, before it is uploaded. */
static gboolean
ms_check_image_member(
	JsonObject   *body,
	const gchar  *member,
	GError      **error
){
	JsonNode *node = json_object_get_member(body, member);
	GList *items = NULL;
	GList *iter;
	gboolean ok = TRUE;

	if (node == NULL)
	{
		return TRUE;
	}
	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		items = json_array_get_elements(json_node_get_array(node));
	}
	else
	{
		items = g_list_prepend(items, node);
	}

	for (iter = items; iter != NULL && ok; iter = iter->next)
	{
		JsonNode *item = iter->data;
		const gchar *value;

		if (!JSON_NODE_HOLDS_VALUE(item) || json_node_get_value_type(item) != G_TYPE_STRING)
		{
			continue;
		}
		value = json_node_get_string(item);
		if (g_str_has_prefix(value, "data:") && ms_data_uri_size(value) > AI_MESHY_MAX_IMAGE_BYTES)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "Meshy's '%s' takes an image of at most 20 MB; this data URI holds "
			            "about %" G_GSIZE_FORMAT " bytes",
			            member, ms_data_uri_size(value));
			ok = FALSE;
		}
	}
	g_list_free(items);
	return ok;
}

static gboolean
ms_check_body(
	JsonObject  *body,
	GError     **error
){
	static const gchar * const members[] = {
		"image_url", "image_urls", "texture_image_url", "texture_image_urls",
		"image_style_url", "multiview_image_urls", NULL
	};
	guint i;

	for (i = 0; members[i] != NULL; i++)
	{
		if (!ms_check_image_member(body, members[i], error))
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Submit                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct
{
	gchar *prefix;          /* the job-id operation */
	gchar *refine_json;     /* a chained Text to 3D job's refine body */
} MsSubmit;

static void
ms_submit_free(gpointer data)
{
	MsSubmit *submit = data;

	g_free(submit->prefix);
	g_free(submit->refine_json);
	g_free(submit);
}

static void
ms_submit_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);
	MsSubmit *submit = g_object_get_data(G_OBJECT(task), "ms-submit");
	g_autoptr(JsonObject) object = NULL;
	GError *error = NULL;
	const gchar *task_id;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, ms_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}

	object = ms_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	task_id = json_object_get_string_member_with_default(object, "result", NULL);
	if (!ai_media_shared_id_is_valid(task_id, MS_MAX_TASK_ID))
	{
		g_task_return_error(task,
		                    ms_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("Meshy submit returned no usable task id "
		                                          "in 'result'")));
		return;
	}

	/* The refine is created by the poll that sees this preview succeed. */
	if (submit->refine_json != NULL)
	{
		MsPlan *plan = g_new0(MsPlan, 1);

		plan->refine_json = g_steal_pointer(&submit->refine_json);
		g_mutex_lock(&self->lock);
		g_hash_table_replace(self->plans, g_strdup(task_id), plan);
		g_mutex_unlock(&self->lock);
	}

	g_task_return_pointer(task, g_strdup_printf("%s:%s", submit->prefix, task_id), g_free);
}

static void
ms_submit_async(
	AiMediaGenerator    *generator,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMeshyClient *self = AI_MESHY_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(JsonObject) body = NULL;
	g_autoptr(GError) error = NULL;
	const MsOperation *operation = NULL;
	MsSubmit *submit;

	task = ms_task_new(self, ms_submit_async, "submit", cancellable, callback, user_data);

	/* Meshy registers webhooks per account, not per task, and keeps files 3 days. */
	if (ai_media_request_get_webhook_url(request) != NULL ||
	    ai_media_request_get_expires_after(request) != 0)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		                        "Meshy takes no per-task webhook or retention: webhooks are "
		                        "registered on the account, and results are kept three days");
		return;
	}

	body = ms_request_body(request, &operation, &error);
	if (body == NULL || !ms_check_body(body, &error))
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	submit = g_new0(MsSubmit, 1);
	g_object_set_data_full(G_OBJECT(task), "ms-submit", submit, ms_submit_free);
	submit->prefix = g_strdup(operation->name);

	/*
	 * Text to 3D with no mode runs preview and refine as one job.  The
	 * refine's parameters are lifted out of the request now, because a
	 * preview refuses the texture-only ones.
	 */
	if (g_strcmp0(operation->name, AI_MESHY_OPERATION_TEXT_TO_3D) == 0)
	{
		if (json_object_has_member(body, "mode"))
		{
			g_free(submit->prefix);
			submit->prefix = g_strdup(MS_TEXT_TO_3D_STEP);
		}
		else
		{
			g_autoptr(JsonObject) refine = json_object_new();
			g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
			guint i;

			for (i = 0; ms_refine_keys[i] != NULL; i++)
			{
				if (json_object_has_member(body, ms_refine_keys[i]))
				{
					json_object_set_member(refine, ms_refine_keys[i],
					                       json_node_copy(json_object_get_member(body,
					                                                             ms_refine_keys[i])));
				}
			}
			/* The refine's `ai_model` is a texture model; the preview's is a mesh model. */
			if (json_object_has_member(body, "texture_model"))
			{
				json_object_set_member(refine, "ai_model",
				                       json_node_copy(json_object_get_member(body, "texture_model")));
			}
			for (i = 0; ms_refine_only_keys[i] != NULL; i++)
			{
				json_object_remove_member(body, ms_refine_only_keys[i]);
			}
			json_object_set_string_member(body, "mode", "preview");

			json_node_set_object(root, refine);
			submit->refine_json = json_to_string(root, FALSE);
		}
	}

	ms_send_api(task, "POST", operation->path, NULL, body, ms_submit_reply);
}

static gchar *
ms_submit_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(g_task_is_valid(result, generator), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* Reading a task                                                         */
/* ---------------------------------------------------------------------- */

/*
 * Maps Meshy's status.  An unknown word is an error rather than a guess:
 * running would leave a waiter polling a finished task until its deadline,
 * finished would lose a result.
 */
static gboolean
ms_state_from_status(
	const gchar     *status,
	AiMediaJobState *state
){
	if (g_strcmp0(status, "PENDING") == 0)
		*state = AI_MEDIA_JOB_STATE_QUEUED;
	else if (g_strcmp0(status, "IN_PROGRESS") == 0)
		*state = AI_MEDIA_JOB_STATE_RUNNING;
	else if (g_strcmp0(status, "SUCCEEDED") == 0)
		*state = AI_MEDIA_JOB_STATE_DONE;
	else if (g_strcmp0(status, "FAILED") == 0)
		*state = AI_MEDIA_JOB_STATE_FAILED;
	else if (g_strcmp0(status, "CANCELED") == 0)
		*state = AI_MEDIA_JOB_STATE_CANCELLED;
	else
		return FALSE;

	return TRUE;
}

/* `object.member` if it is a non-empty string. */
static const gchar *
ms_string(
	JsonObject  *object,
	const gchar *member
){
	JsonNode *node;
	const gchar *value;

	if (object == NULL)
	{
		return NULL;
	}
	node = json_object_get_member(object, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
	{
		return NULL;
	}
	value = json_node_get_string(node);
	return (value != NULL && value[0] != '\0') ? value : NULL;
}

/* `object.member` if it is a JSON object. */
static JsonObject *
ms_object(
	JsonObject  *object,
	const gchar *member
){
	JsonNode *node;

	if (object == NULL)
	{
		return NULL;
	}
	node = json_object_get_member(object, member);
	return (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
}

/* `object.member` as an integer, or -1 when absent or null. */
static gint64
ms_int(
	JsonObject  *object,
	const gchar *member
){
	JsonNode *node = (object != NULL) ? json_object_get_member(object, member) : NULL;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
	{
		return -1;
	}
	if (json_node_get_value_type(node) == G_TYPE_INT64)
	{
		return json_node_get_int(node);
	}
	if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
	{
		return (gint64)json_node_get_double(node);
	}
	return -1;
}

/* Adds every non-empty string member of @urls as `<prefix><member>`. */
static void
ms_add_members(
	AiMediaJob  *job,
	JsonObject  *urls,
	const gchar *prefix
){
	GList *names;
	GList *iter;

	if (urls == NULL)
	{
		return;
	}
	names = json_object_get_members(urls);
	for (iter = names; iter != NULL; iter = iter->next)
	{
		const gchar *member = iter->data;
		const gchar *url = ms_string(urls, member);

		if (url != NULL)
		{
			g_autofree gchar *name = g_strconcat(prefix, member, NULL);

			ai_media_job_add_named_result(job, name, url);
		}
	}
	g_list_free(names);
}

/*
 * The named outputs of a finished task.  The three families of response
 * each name their files differently; the result names here are ai-glib's,
 * documented in docs/providers/meshy.org, so a caller asks for "glb" or
 * "rig:fbx" without knowing which family produced it.
 */
static void
ms_collect_results(
	AiMediaJob *job,
	JsonObject *task
){
	static const struct { const gchar *member; const gchar *name; } result_members[] = {
		/* Rigging */
		{ "rigged_character_glb_url", "rig:glb" },
		{ "rigged_character_fbx_url", "rig:fbx" },
		/* Animation */
		{ "animation_glb_url", "anim:glb" },
		{ "animation_fbx_url", "anim:fbx" },
		{ "processed_usdz_url", "anim:usdz" },
		{ "processed_armature_fbx_url", "anim:armature-fbx" },
		{ "processed_animation_fps_fbx_url", "anim:fps-fbx" },
		{ NULL, NULL }
	};
	static const struct { const gchar *member; const gchar *name; } basic_members[] = {
		{ "walking_glb_url", "anim:walking" },
		{ "walking_fbx_url", "anim:walking:fbx" },
		{ "walking_armature_glb_url", "anim:walking:armature" },
		{ "running_glb_url", "anim:running" },
		{ "running_fbx_url", "anim:running:fbx" },
		{ "running_armature_glb_url", "anim:running:armature" },
		{ NULL, NULL }
	};
	JsonObject *result;
	JsonNode *textures;
	const gchar *url;
	guint i;

	/* Text to 3D, Image to 3D, Multi-Image to 3D, Retexture, Remesh. */
	ms_add_members(job, ms_object(task, "model_urls"), "");
	if ((url = ms_string(task, "thumbnail_url")) != NULL)
	{
		ai_media_job_add_named_result(job, "thumbnail", url);
	}
	if ((url = ms_string(task, "alpha_thumbnail_url")) != NULL)
	{
		ai_media_job_add_named_result(job, "thumbnail:alpha", url);
	}

	/* One texture set per material; the first is plain, the rest numbered. */
	textures = json_object_get_member(task, "texture_urls");
	if (textures != NULL && JSON_NODE_HOLDS_ARRAY(textures))
	{
		JsonArray *sets = json_node_get_array(textures);

		for (i = 0; i < json_array_get_length(sets); i++)
		{
			JsonNode *set = json_array_get_element(sets, i);
			GList *names;
			GList *iter;

			if (!JSON_NODE_HOLDS_OBJECT(set))
			{
				continue;
			}
			names = json_object_get_members(json_node_get_object(set));
			for (iter = names; iter != NULL; iter = iter->next)
			{
				const gchar *kind = iter->data;
				g_autofree gchar *name = NULL;

				url = ms_string(json_node_get_object(set), kind);
				if (url == NULL)
				{
					continue;
				}
				name = (i == 0) ? g_strdup_printf("texture:%s", kind)
				                : g_strdup_printf("texture:%s:%u", kind, i);
				ai_media_job_add_named_result(job, name, url);
			}
			g_list_free(names);
		}
	}

	/* Rigging and Animation nest theirs in `result`. */
	result = ms_object(task, "result");
	for (i = 0; result != NULL && result_members[i].member != NULL; i++)
	{
		if ((url = ms_string(result, result_members[i].member)) != NULL)
		{
			ai_media_job_add_named_result(job, result_members[i].name, url);
		}
	}
	result = ms_object(result, "basic_animations");
	for (i = 0; result != NULL && basic_members[i].member != NULL; i++)
	{
		if ((url = ms_string(result, basic_members[i].member)) != NULL)
		{
			ai_media_job_add_named_result(job, basic_members[i].name, url);
		}
	}
}

/*
 * An AiMediaJob from one task object.  @stage ("preview", "refine" or
 * NULL) prefixes the provider status, so a caller can tell which half of
 * a chained Text to 3D job a status word describes.
 */
static AiMediaJob *
ms_job_from_task(
	AiMeshyClient  *self,
	const gchar    *job_id,
	JsonObject     *task,
	const gchar    *stage,
	GError        **error
){
	g_autoptr(AiMediaJob) job = NULL;
	g_autofree gchar *provider_status = NULL;
	g_autofree gchar *expires = NULL;
	const gchar *status;
	AiMediaJobState state;
	JsonObject *task_error;
	gint64 credits;

	status = ms_string(task, "status");
	if (!ms_state_from_status(status, &state))
	{
		g_propagate_error(error,
		                  ms_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                               g_strdup_printf("Meshy reported an unknown status '%s' "
		                                               "for job %s",
		                                               (status != NULL) ? status : "(none)",
		                                               job_id)));
		return NULL;
	}

	job = ai_media_job_new(job_id, state);
	provider_status = (stage != NULL) ? g_strdup_printf("%s:%s", stage, status) : g_strdup(status);
	ai_media_job_set_provider_status(job, provider_status);

	task_error = ms_object(task, "task_error");
	if (ms_string(task_error, "message") != NULL)
	{
		g_autofree gchar *clean = ai_media_shared_redact(g_strdup(ms_string(task_error, "message")),
		                                                 self->api_key);

		ai_media_job_set_error_message(job, clean);
	}

	expires = ai_media_shared_iso8601_from_unix_ms(ms_int(task, "expires_at"));
	ai_media_job_set_expires_at(job, expires);

	credits = ms_int(task, "consumed_credits");
	ai_media_job_set_credits(job, (credits >= 0) ? credits : AI_MEDIA_COST_UNKNOWN);

	ms_collect_results(job, task);

	/* Meshy bills in credits; their price in money depends on the plan. */
	return (AiMediaJob *)g_steal_pointer(&job);
}

/* ---------------------------------------------------------------------- */
/* Poll                                                                   */
/* ---------------------------------------------------------------------- */

/* Parses a 2xx task body or returns the right error on @task. */
static JsonObject *
ms_reply_object(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);
	GError *error = NULL;
	JsonObject *object;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, ms_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return NULL;
	}
	object = ms_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
	}
	return object;
}

/* Returns a job built from @object, or its error, on @task. */
static void
ms_return_job(
	GTask       *task,
	const gchar *job_id,
	JsonObject  *object,
	const gchar *stage
){
	AiMeshyClient *self = g_task_get_source_object(task);
	GError *error = NULL;
	AiMediaJob *job = ms_job_from_task(self, job_id, object, stage, &error);

	if (job == NULL)
	{
		g_task_return_error(task, error);
		return;
	}
	g_task_return_pointer(task, job, g_object_unref);
}

/* One task, the whole job. */
static void
ms_poll_single_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = ms_reply_object(task, status, body);
	g_autofree gchar *job_id = NULL;

	if (object == NULL)
	{
		return;
	}
	job_id = g_strdup_printf("%s:%s", op->operation->name, op->task_id);
	ms_return_job(task, job_id, object, NULL);
}

/* A chained job whose preview failed or was cancelled needs no plan. */
static void
ms_forget_plan(
	AiMeshyClient *self,
	const gchar   *preview_id
){
	g_mutex_lock(&self->lock);
	g_hash_table_remove(self->plans, preview_id);
	g_mutex_unlock(&self->lock);
}

/* The preview was fetched after its refine finished, for its credits. */
static void
ms_poll_preview_credits_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) preview = ms_reply_object(task, status, body);
	g_autofree gchar *job_id = NULL;
	gint64 preview_credits;
	gint64 refine_credits;

	if (preview == NULL)
	{
		return;
	}

	/* A chained job cost both tasks; one unknown makes the sum unknown. */
	preview_credits = ms_int(preview, "consumed_credits");
	refine_credits = ms_int(op->refine_task, "consumed_credits");
	json_object_set_int_member(op->refine_task, "consumed_credits",
	                           (preview_credits >= 0 && refine_credits >= 0)
	                               ? preview_credits + refine_credits : -1);

	job_id = g_strdup_printf("%s:%s:%s", AI_MESHY_OPERATION_TEXT_TO_3D,
	                         op->task_id, op->refine_id);
	ms_return_job(task, job_id, op->refine_task, "refine");
}

/* A chained job's refine. */
static void
ms_poll_refine_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) refine = ms_reply_object(task, status, body);
	g_autofree gchar *job_id = NULL;
	g_autofree gchar *path = NULL;
	const gchar *word;
	AiMediaJobState state;

	if (refine == NULL)
	{
		return;
	}

	word = ms_string(refine, "status");
	if (ms_state_from_status(word, &state) && state == AI_MEDIA_JOB_STATE_DONE)
	{
		op->refine_task = json_object_ref(refine);
		op->what = "status";
		path = g_strdup_printf("%s/%s", op->operation->path, op->task_id);
		ms_send_api(task, "GET", path, NULL, NULL, ms_poll_preview_credits_reply);
		return;
	}

	job_id = g_strdup_printf("%s:%s:%s", AI_MESHY_OPERATION_TEXT_TO_3D,
	                         op->task_id, op->refine_id);
	ms_return_job(task, job_id, refine, "refine");
}

/* The refine was created: the job is now running under a two-task id. */
static void
ms_poll_refine_created_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autofree gchar *job_id = NULL;
	GError *error = NULL;
	const gchar *refine_id = NULL;
	MsPlan *plan;

	if (SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		object = ms_parse_object(self, op->what, body, &error);
		if (object != NULL)
		{
			refine_id = json_object_get_string_member_with_default(object, "result", NULL);
			if (!ai_media_shared_id_is_valid(refine_id, MS_MAX_TASK_ID))
			{
				refine_id = NULL;
				error = ms_error_new(self, AI_ERROR_INVALID_RESPONSE,
				                     g_strdup("Meshy refine returned no usable task id in 'result'"));
			}
		}
	}
	else
	{
		error = ms_status_error(self, op->what, status, body, op->message, FALSE);
	}

	/* Success or not, the POST is no longer in flight. */
	g_mutex_lock(&self->lock);
	plan = g_hash_table_lookup(self->plans, op->task_id);
	if (plan != NULL)
	{
		plan->refine_pending = FALSE;
		if (refine_id != NULL)
		{
			/*
			 * Keep the refine's id, so the preview-only id still reaches
			 * it, but not its parameters, which are spent.
			 */
			g_set_str(&plan->refine_id, refine_id);
			g_clear_pointer(&plan->refine_json, g_free);
		}
	}
	g_mutex_unlock(&self->lock);

	if (refine_id == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	job_id = g_strdup_printf("%s:%s:%s", AI_MESHY_OPERATION_TEXT_TO_3D, op->task_id, refine_id);
	job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_QUEUED);
	ai_media_job_set_provider_status(job, "refine:PENDING");
	g_task_return_pointer(task, g_steal_pointer(&job), g_object_unref);
}

/* A chained job's preview, before there is a refine. */
static void
ms_poll_preview_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) preview = ms_reply_object(task, status, body);
	g_autoptr(JsonObject) refine = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autofree gchar *job_id = NULL;
	const gchar *word;
	AiMediaJobState state;
	MsPlan *plan;
	gboolean pending = FALSE;
	gchar *refine_id = NULL;

	if (preview == NULL)
	{
		return;
	}

	job_id = g_strdup_printf("%s:%s", AI_MESHY_OPERATION_TEXT_TO_3D, op->task_id);
	word = ms_string(preview, "status");
	if (!ms_state_from_status(word, &state) || state != AI_MEDIA_JOB_STATE_DONE)
	{
		/* Not done yet, failed, cancelled or unknown: the preview is the job. */
		if (ms_state_from_status(word, &state) && ai_media_job_state_is_finished(state))
		{
			ms_forget_plan(self, op->task_id);
		}
		ms_return_job(task, job_id, preview, "preview");
		return;
	}

	/*
	 * The preview succeeded: create the refine, once.  Claim the plan under
	 * the lock, so a second poll in flight sees the claim and reports the
	 * job running rather than creating -- and billing -- a second refine.
	 */
	g_mutex_lock(&self->lock);
	plan = g_hash_table_lookup(self->plans, op->task_id);
	if (plan != NULL)
	{
		if (plan->refine_id != NULL)
		{
			refine_id = g_strdup(plan->refine_id);
		}
		else if (plan->refine_pending)
		{
			pending = TRUE;
		}
		else
		{
			plan->refine_pending = TRUE;
			refine = ai_media_shared_object_from_text(plan->refine_json);
		}
	}
	g_mutex_unlock(&self->lock);

	if (plan == NULL)
	{
		g_task_return_error(task,
		                    ms_error_new(self, AI_ERROR_INVALID_REQUEST,
		                                 g_strdup_printf("Meshy preview %s succeeded, but this "
		                                                 "process never submitted it, so it does "
		                                                 "not hold its refine parameters; submit "
		                                                 "a text-to-3d request with mode 'refine' "
		                                                 "and preview_task_id '%s' to texture it",
		                                                 op->task_id, op->task_id)));
		return;
	}
	if (refine_id != NULL)
	{
		/* Another poll created it after this one started: read it. */
		g_autofree gchar *path = g_strdup_printf("%s/%s", op->operation->path, refine_id);

		op->refine_id = refine_id;
		ms_send_api(task, "GET", path, NULL, NULL, ms_poll_refine_reply);
		return;
	}
	if (pending)
	{
		job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_RUNNING);
		ai_media_job_set_provider_status(job, "preview:SUCCEEDED");
		g_task_return_pointer(task, g_steal_pointer(&job), g_object_unref);
		return;
	}

	if (refine == NULL)
	{
		refine = json_object_new();
	}
	json_object_set_string_member(refine, "mode", "refine");
	json_object_set_string_member(refine, "preview_task_id", op->task_id);
	op->what = "refine";
	ms_send_api(task, "POST", op->operation->path, NULL, refine, ms_poll_refine_created_reply);

	/* A build failure returned on the task above must not leave the claim. */
	if (g_task_had_error(task))
	{
		g_mutex_lock(&self->lock);
		plan = g_hash_table_lookup(self->plans, op->task_id);
		if (plan != NULL)
		{
			plan->refine_pending = FALSE;
		}
		g_mutex_unlock(&self->lock);
	}
}

static void
ms_poll_async(
	AiMediaGenerator    *generator,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMeshyClient *self = AI_MESHY_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autofree gchar *path = NULL;
	MsOp *op;

	task = ms_task_new(self, ms_poll_async, "status", cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	if (!ms_parse_job_id(job_id, &op->operation, &op->task_id, &op->refine_id))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a Meshy job id (operation:task)", job_id);
		return;
	}

	if (g_strcmp0(op->operation->name, AI_MESHY_OPERATION_TEXT_TO_3D) != 0)
	{
		path = g_strdup_printf("%s/%s", op->operation->path, op->task_id);
		ms_send_api(task, "GET", path, NULL, NULL, ms_poll_single_reply);
		return;
	}

	/* A chained job: the refine if there is one, known from the id or memory. */
	if (op->refine_id == NULL)
	{
		MsPlan *plan;

		g_mutex_lock(&self->lock);
		plan = g_hash_table_lookup(self->plans, op->task_id);
		if (plan != NULL && plan->refine_id != NULL)
		{
			op->refine_id = g_strdup(plan->refine_id);
		}
		g_mutex_unlock(&self->lock);
	}

	if (op->refine_id != NULL)
	{
		path = g_strdup_printf("%s/%s", op->operation->path, op->refine_id);
		ms_send_api(task, "GET", path, NULL, NULL, ms_poll_refine_reply);
		return;
	}

	path = g_strdup_printf("%s/%s", op->operation->path, op->task_id);
	ms_send_api(task, "GET", path, NULL, NULL, ms_poll_preview_reply);
}

static AiMediaJob *
ms_poll_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(g_task_is_valid(result, generator), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* Download                                                               */
/* ---------------------------------------------------------------------- */

static void
ms_download_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, ms_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}
	g_task_return_pointer(task, g_bytes_ref(body), (GDestroyNotify)g_bytes_unref);
}

static void
ms_download_async(
	AiMediaGenerator    *generator,
	const gchar         *url,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMeshyClient *self = AI_MESHY_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GUri) uri = NULL;

	task = ms_task_new(self, ms_download_async, "download", cancellable, callback, user_data);

	if (url != NULL && g_str_has_prefix(url, "data:"))
	{
		GError *error = NULL;
		GBytes *bytes = ai_media_shared_decode_data_url(url, &error);

		if (bytes == NULL)
		{
			g_task_return_error(task, error);
			return;
		}
		g_task_return_pointer(task, bytes, (GDestroyNotify)g_bytes_unref);
		return;
	}

	uri = ai_media_shared_parse_http_uri(url);
	if (uri == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "Refusing to download '%s': not an http, https or data URL", url);
		return;
	}

	/* The CDN URLs are signed: no Authorization header, redirects allowed. */
	ms_send(task, ai_http_message_new_from_uri("GET", uri, AI_HTTP_PUBLIC_URL), ms_download_reply);
}

static GBytes *
ms_download_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(g_task_is_valid(result, generator), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* Estimate                                                               */
/* ---------------------------------------------------------------------- */

typedef enum
{
	MS_MODEL_FULL,      /* meshy-7.1, meshy-7, latest */
	MS_MODEL_SIX,       /* meshy-6 */
	MS_MODEL_LITE,      /* meshy-6-lite */
	MS_MODEL_T2         /* meshy-t2, meshy-t2.5 */
} MsModelClass;

static const gchar *
ms_body_string(
	JsonObject  *body,
	const gchar *member,
	const gchar *fallback
){
	const gchar *value = ms_string(body, member);

	return (value != NULL) ? value : fallback;
}

static gboolean
ms_body_boolean(
	JsonObject  *body,
	const gchar *member,
	gboolean     fallback
){
	JsonNode *node = json_object_get_member(body, member);

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
	    json_node_get_value_type(node) != G_TYPE_BOOLEAN)
	{
		return fallback;
	}
	return json_node_get_boolean(node);
}

static gboolean
ms_model_class(
	const gchar   *model,
	MsModelClass  *klass
){
	if (g_strcmp0(model, "latest") == 0 || g_strcmp0(model, "meshy-7.1") == 0 ||
	    g_strcmp0(model, "meshy-7") == 0)
		*klass = MS_MODEL_FULL;
	else if (g_strcmp0(model, "meshy-6") == 0)
		*klass = MS_MODEL_SIX;
	else if (g_strcmp0(model, "meshy-6-lite") == 0)
		*klass = MS_MODEL_LITE;
	else if (g_strcmp0(model, "meshy-t2") == 0 || g_strcmp0(model, "meshy-t2.5") == 0)
		*klass = MS_MODEL_T2;
	else
		return FALSE;

	return TRUE;
}

/* 2k, 4k or 8k; `hd_texture: true` is the deprecated spelling of 4k. */
static const gchar *
ms_texture_resolution(JsonObject *body)
{
	const gchar *resolution = ms_string(body, "texture_resolution");

	if (resolution != NULL)
	{
		return resolution;
	}
	return ms_body_boolean(body, "hd_texture", FALSE) ? "4k" : "2k";
}

/* The mesh price, the Ultra geometry surcharge included. */
static gboolean
ms_mesh_credits(
	JsonObject    *body,
	MsModelClass   klass,
	const gchar   *what,
	gint64        *credits,
	GError       **error
){
	const gchar *geometry = ms_body_string(body, "geometry_resolution", "standard");

	*credits = (klass == MS_MODEL_FULL || klass == MS_MODEL_SIX) ? 20 : 5;
	if (g_strcmp0(geometry, "2k") == 0 || g_strcmp0(geometry, "4k") == 0)
	{
		if (klass != MS_MODEL_FULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "Meshy %s: Ultra geometry (geometry_resolution '%s') needs "
			            "ai_model meshy-7.1", what, geometry);
			return FALSE;
		}
		*credits += 5;
	}
	else if (g_strcmp0(geometry, "standard") != 0)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Meshy %s: geometry_resolution '%s' is not standard, 2k or 4k",
		            what, geometry);
		return FALSE;
	}
	return TRUE;
}

/*
 * Image to 3D and Multi-Image to 3D: the mesh, plus texturing at the
 * Retexture price (10, 15 at 8K), which is how Meshy's tables add up for
 * every pairing they list.  The pairs they mark "not supported" are
 * refused, so an estimate never prices a request Meshy will reject.
 */
static gboolean
ms_image_to_3d_credits(
	JsonObject    *body,
	const gchar   *what,
	gboolean       multi,
	gint64        *credits,
	GError       **error
){
	const gchar *model = ms_body_string(body, "ai_model", "latest");
	const gchar *texture_model = ms_string(body, "texture_model");
	const gchar *resolution;
	MsModelClass klass;
	gboolean high;

	if (!ms_model_class(model, &klass) || (multi && klass == MS_MODEL_T2))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Meshy %s has no price for ai_model '%s'", what, model);
		return FALSE;
	}
	if (!ms_mesh_credits(body, klass, what, credits, error))
	{
		return FALSE;
	}
	if (!ms_body_boolean(body, "should_texture", TRUE))
	{
		return TRUE;
	}

	resolution = ms_texture_resolution(body);
	if (g_strcmp0(resolution, "2k") != 0 && g_strcmp0(resolution, "4k") != 0 &&
	    g_strcmp0(resolution, "8k") != 0)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Meshy %s: texture_resolution '%s' is not 2k, 4k or 8k", what, resolution);
		return FALSE;
	}
	high = g_strcmp0(resolution, "2k") != 0;

	/* meshy-n1 and meshy-6-lite texture at 2K only; so does a lite mesh by default. */
	if (high && (g_strcmp0(texture_model, "meshy-n1") == 0 ||
	             g_strcmp0(texture_model, "meshy-6-lite") == 0 ||
	             (klass == MS_MODEL_LITE && texture_model == NULL)))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Meshy %s: %s textures at 2K only", what,
		            (texture_model != NULL) ? texture_model : model);
		return FALSE;
	}

	*credits += (g_strcmp0(resolution, "8k") == 0) ? 15 : 10;
	return TRUE;
}

/**
 * ai_meshy_estimate_credits:
 * @operation: a Meshy operation, such as %AI_MESHY_OPERATION_IMAGE_TO_3D
 * @request: (nullable): the request, for the parameters that change the price
 * @error: return location for a #GError
 *
 * What a request will cost in Meshy credits, from the price table in
 * Meshy's documentation (checked 2026-11-23; see docs/providers/meshy.org).
 * Meshy has no estimate endpoint, so this is arithmetic, not a quote.
 *
 * - `text-to-3d`: preview 20 (meshy-7.1, meshy-7, meshy-6) or 5
 *   (meshy-6-lite, meshy-t2, meshy-t2.5), plus 5 for Ultra geometry
 *   (`geometry_resolution` 2k or 4k, meshy-7.1 only), plus the refine:
 *   10, or 15 at an 8K `texture_resolution`.  `mode` `preview` or `refine`
 *   prices that step alone.
 * - `image-to-3d`, `multi-image-to-3d`: the mesh (as above), plus 10 for
 *   textures at 2K or 4K or 15 at 8K unless `should_texture` is false.
 * - `retexture`: 10, or 15 at 8K.  `remesh`: 5.  `rig`: 5.
 * - `animate`: 3 per action (`action_ids`), or 3 for one `action_id` or
 *   `motion_task_id`.
 *
 * Combinations Meshy marks as not supported are refused rather than
 * priced.
 *
 * Returns: the credits, or -1 with @error set
 */
gint64
ai_meshy_estimate_credits(
	const gchar     *operation,
	AiMediaRequest  *request,
	GError         **error
){
	g_autoptr(JsonObject) body = NULL;
	gint64 credits = 0;

	g_return_val_if_fail(operation != NULL, -1);
	g_return_val_if_fail(request == NULL || AI_IS_MEDIA_REQUEST(request), -1);

	if (request != NULL)
	{
		g_autofree gchar *json = ai_media_request_to_json(request);

		body = ai_media_shared_object_from_text(json);
	}
	if (body == NULL)
	{
		body = json_object_new();
	}

	if (g_strcmp0(operation, AI_MESHY_OPERATION_TEXT_TO_3D) == 0 ||
	    g_strcmp0(operation, MS_TEXT_TO_3D_STEP) == 0)
	{
		const gchar *mode = ms_string(body, "mode");
		const gchar *model = ms_body_string(body, "ai_model", "latest");
		MsModelClass klass;
		gint64 refine = (g_strcmp0(ms_texture_resolution(body), "8k") == 0) ? 15 : 10;

		if (g_strcmp0(mode, "refine") == 0)
		{
			return refine;
		}
		if (!ms_model_class(model, &klass))
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "Meshy text-to-3d has no price for ai_model '%s'", model);
			return -1;
		}
		if (!ms_mesh_credits(body, klass, "text-to-3d", &credits, error))
		{
			return -1;
		}
		return (g_strcmp0(mode, "preview") == 0) ? credits : credits + refine;
	}
	if (g_strcmp0(operation, AI_MESHY_OPERATION_IMAGE_TO_3D) == 0 ||
	    g_strcmp0(operation, AI_MESHY_OPERATION_MULTI_IMAGE_TO_3D) == 0)
	{
		gboolean multi = g_strcmp0(operation, AI_MESHY_OPERATION_MULTI_IMAGE_TO_3D) == 0;

		return ms_image_to_3d_credits(body, operation, multi, &credits, error) ? credits : -1;
	}
	if (g_strcmp0(operation, AI_MESHY_OPERATION_RETEXTURE) == 0)
	{
		return (g_strcmp0(ms_texture_resolution(body), "8k") == 0) ? 15 : 10;
	}
	if (g_strcmp0(operation, AI_MESHY_OPERATION_REMESH) == 0 ||
	    g_strcmp0(operation, AI_MESHY_OPERATION_RIG) == 0)
	{
		return 5;
	}
	if (g_strcmp0(operation, AI_MESHY_OPERATION_ANIMATE) == 0)
	{
		JsonNode *ids = json_object_get_member(body, "action_ids");

		if (ids != NULL && JSON_NODE_HOLDS_ARRAY(ids))
		{
			guint n = json_array_get_length(json_node_get_array(ids));

			if (n < 1 || n > 10)
			{
				g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
				            "Meshy animate takes 1 to 10 action_ids, not %u", n);
				return -1;
			}
			return 3 * (gint64)n;
		}
		return 3;
	}

	{
		g_autofree gchar *names = ms_operation_names();

		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "'%s' is not a Meshy operation (one of: %s)", operation, names);
	}
	return -1;
}

typedef struct
{
	gint64 micro_usd;
} MsEstimate;

/*
 * The interface's estimate answers in money, which Meshy does not: a
 * credit's price depends on the plan.  So it checks the request against
 * the price table -- an unsupported combination is an error here, before
 * any credit is spent -- and answers %AI_MEDIA_COST_UNKNOWN.  The credit
 * figure is ai_meshy_estimate_credits().
 */
static void
ms_estimate_async(
	AiMediaGenerator    *generator,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMeshyClient *self = AI_MESHY_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(JsonObject) body = NULL;
	g_autoptr(GError) error = NULL;
	const MsOperation *operation = NULL;
	MsEstimate *estimate;

	task = ms_task_new(self, ms_estimate_async, "estimate", cancellable, callback, user_data);

	body = ms_request_body(request, &operation, &error);
	if (body == NULL || ai_meshy_estimate_credits(operation->name, request, &error) < 0)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	estimate = g_new0(MsEstimate, 1);
	estimate->micro_usd = AI_MEDIA_COST_UNKNOWN;
	g_task_return_pointer(task, estimate, g_free);
}

static gboolean
ms_estimate_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	gint64            *micro_usd,
	GError           **error
){
	g_autofree MsEstimate *estimate = NULL;

	g_return_val_if_fail(g_task_is_valid(result, generator), FALSE);

	estimate = g_task_propagate_pointer(G_TASK(result), error);
	if (estimate == NULL)
	{
		return FALSE;
	}
	*micro_usd = estimate->micro_usd;
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Cancel                                                                 */
/* ---------------------------------------------------------------------- */

static void
ms_cancel_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, ms_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}
	g_task_return_boolean(task, TRUE);
}

/*
 * Meshy's DELETE is its cancel: it removes a task that is still PENDING
 * and refunds it, and answers 409 for one that is IN_PROGRESS, because
 * the credits for started work are spent.  For a chained Text to 3D job
 * the stage that is current is the one deleted.
 */
static void
ms_cancel_async(
	AiMediaGenerator    *generator,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiMeshyClient *self = AI_MESHY_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autofree gchar *path = NULL;
	MsOp *op;

	task = ms_task_new(self, ms_cancel_async, "cancel", cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	if (!ms_parse_job_id(job_id, &op->operation, &op->task_id, &op->refine_id))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a Meshy job id (operation:task)", job_id);
		return;
	}

	if (op->refine_id == NULL &&
	    g_strcmp0(op->operation->name, AI_MESHY_OPERATION_TEXT_TO_3D) == 0)
	{
		MsPlan *plan;

		g_mutex_lock(&self->lock);
		plan = g_hash_table_lookup(self->plans, op->task_id);
		if (plan != NULL && plan->refine_id != NULL)
		{
			op->refine_id = g_strdup(plan->refine_id);
		}
		g_mutex_unlock(&self->lock);
	}

	path = g_strdup_printf("%s/%s", op->operation->path,
	                       (op->refine_id != NULL) ? op->refine_id : op->task_id);
	ms_send_api(task, "DELETE", path, NULL, NULL, ms_cancel_reply);
}

static gboolean
ms_cancel_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(g_task_is_valid(result, generator), FALSE);

	return g_task_propagate_boolean(G_TASK(result), error);
}

static void
media_generator_iface_init(AiMediaGeneratorInterface *iface)
{
	iface->submit_async    = ms_submit_async;
	iface->submit_finish   = ms_submit_finish;
	iface->poll_async      = ms_poll_async;
	iface->poll_finish     = ms_poll_finish;
	iface->download_async  = ms_download_async;
	iface->download_finish = ms_download_finish;
	iface->estimate_async  = ms_estimate_async;
	iface->estimate_finish = ms_estimate_finish;
	iface->cancel_async    = ms_cancel_async;
	iface->cancel_finish   = ms_cancel_finish;
}

/* ---------------------------------------------------------------------- */
/* Balance and the animation library                                      */
/* ---------------------------------------------------------------------- */

typedef struct
{
	gint64 balance;
	gint64 monthly_credits;
	gint64 permanent_credits;
} MsBalance;

static void
ms_balance_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	GError *error = NULL;
	MsBalance *balance;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, ms_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}
	object = ms_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}
	if (ms_int(object, "balance") < 0)
	{
		g_task_return_error(task,
		                    ms_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("Meshy balance returned no 'balance'")));
		return;
	}

	balance = g_new0(MsBalance, 1);
	balance->balance = ms_int(object, "balance");
	balance->monthly_credits = ms_int(object, "monthly_credits");
	balance->permanent_credits = ms_int(object, "permanent_credits");
	g_task_return_pointer(task, balance, g_free);
}

/**
 * ai_meshy_client_balance_async:
 * @self: an #AiMeshyClient
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the balance
 * @user_data: data for @callback
 *
 * Asks how many credits the account has left (`GET
 * /openapi/v1/balance`).
 */
void
ai_meshy_client_balance_async(
	AiMeshyClient       *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;

	g_return_if_fail(AI_IS_MESHY_CLIENT(self));

	task = ms_task_new(self, ai_meshy_client_balance_async, "balance",
	                   cancellable, callback, user_data);
	ms_send_api(task, "GET", "openapi/v1/balance", NULL, NULL, ms_balance_reply);
}

/**
 * ai_meshy_client_balance_finish:
 * @self: an #AiMeshyClient
 * @result: the #GAsyncResult
 * @balance: (out) (optional): the credits available now
 * @monthly_credits: (out) (optional): how many of them expire at the
 *   next monthly refresh, or -1 if not reported
 * @permanent_credits: (out) (optional): how many do not, or -1
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
ai_meshy_client_balance_finish(
	AiMeshyClient  *self,
	GAsyncResult   *result,
	gint64         *balance,
	gint64         *monthly_credits,
	gint64         *permanent_credits,
	GError        **error
){
	g_autofree MsBalance *answer = NULL;

	g_return_val_if_fail(AI_IS_MESHY_CLIENT(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	answer = g_task_propagate_pointer(G_TASK(result), error);
	if (answer == NULL)
	{
		return FALSE;
	}
	if (balance != NULL)
	{
		*balance = answer->balance;
	}
	if (monthly_credits != NULL)
	{
		*monthly_credits = answer->monthly_credits;
	}
	if (permanent_credits != NULL)
	{
		*permanent_credits = answer->permanent_credits;
	}
	return TRUE;
}

static void
ms_animations_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiMeshyClient *self = g_task_get_source_object(task);
	MsOp *op = g_task_get_task_data(task);
	g_autoptr(JsonParser) parser = json_parser_new();
	gconstpointer data;
	gsize length = 0;
	JsonNode *root;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, ms_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}

	data = g_bytes_get_data(body, &length);
	if (length == 0 || !json_parser_load_from_data(parser, data, (gssize)length, NULL) ||
	    (root = json_parser_get_root(parser)) == NULL || !JSON_NODE_HOLDS_ARRAY(root))
	{
		g_task_return_error(task,
		                    ms_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("Meshy animation library returned a body that "
		                                          "is not a JSON array")));
		return;
	}
	g_task_return_pointer(task, json_node_copy(root), (GDestroyNotify)json_node_unref);
}

/**
 * ai_meshy_client_list_animations_async:
 * @self: an #AiMeshyClient
 * @category: (nullable): a category such as "WalkAndRun", or %NULL for all
 * @search: (nullable): words to search the names for, or %NULL
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the library
 * @user_data: data for @callback
 *
 * Lists Meshy's animation library (`GET /openapi/v1/animations/library`):
 * the preset actions whose `action_id` an `animate` request takes.  The
 * ids are data, not constants: Meshy says they are stable but not
 * contiguous, so read them from here rather than hard-coding them.
 */
void
ai_meshy_client_list_animations_async(
	AiMeshyClient       *self,
	const gchar         *category,
	const gchar         *search,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;
	GString *query = g_string_new(NULL);
	g_autofree gchar *query_text = NULL;

	g_return_if_fail(AI_IS_MESHY_CLIENT(self));

	task = ms_task_new(self, ai_meshy_client_list_animations_async, "animation library",
	                   cancellable, callback, user_data);

	if (category != NULL && category[0] != '\0')
	{
		g_autofree gchar *escaped = g_uri_escape_string(category, NULL, FALSE);

		g_string_append_printf(query, "category=%s", escaped);
	}
	if (search != NULL && search[0] != '\0')
	{
		g_autofree gchar *escaped = g_uri_escape_string(search, NULL, FALSE);

		g_string_append_printf(query, "%ssearch=%s", (query->len > 0) ? "&" : "", escaped);
	}
	query_text = g_string_free(query, FALSE);

	ms_send_api(task, "GET", "openapi/v1/animations/library",
	            (query_text[0] != '\0') ? query_text : NULL, NULL, ms_animations_reply);
}

/**
 * ai_meshy_client_list_animations_finish:
 * @self: an #AiMeshyClient
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): a JSON array of objects with
 *   `action_id`, `name`, `key`, `category`, `sub_category`, `preview_url`
 *   and `rig_type`, or %NULL
 */
JsonNode *
ai_meshy_client_list_animations_finish(
	AiMeshyClient  *self,
	GAsyncResult   *result,
	GError        **error
){
	g_return_val_if_fail(AI_IS_MESHY_CLIENT(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* Input images                                                           */
/* ---------------------------------------------------------------------- */

/**
 * ai_meshy_image_data_uri:
 * @path: (type filename): a PNG, JPEG or WebP file
 * @error: return location for a #GError
 *
 * Reads an image into the base64 data URI Meshy accepts wherever it takes
 * an image URL (`image_url`, `image_urls`, `texture_image_url`, ...), for
 * a file that is not already public.  The format is read from the file's
 * own bytes, not its name, and a file over %AI_MESHY_MAX_IMAGE_BYTES is
 * refused before it is read.
 *
 * Returns: (transfer full) (nullable): `data:image/...;base64,...`, or
 *   %NULL with @error set
 */
gchar *
ai_meshy_image_data_uri(
	const gchar  *path,
	GError      **error
){
	GStatBuf info;
	g_autofree gchar *contents = NULL;
	gsize length = 0;
	const gchar *mime;

	g_return_val_if_fail(path != NULL, NULL);

	if (g_stat(path, &info) != 0)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Cannot read image '%s': %s", path, g_strerror(errno));
		return NULL;
	}
	if ((guint64)info.st_size > AI_MESHY_MAX_IMAGE_BYTES)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Image '%s' is %" G_GUINT64_FORMAT " bytes; Meshy takes at most 20 MB",
		            path, (guint64)info.st_size);
		return NULL;
	}
	if (!g_file_get_contents(path, &contents, &length, error))
	{
		return NULL;
	}
	if (length > AI_MESHY_MAX_IMAGE_BYTES)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Image '%s' grew past 20 MB while it was read", path);
		return NULL;
	}

	mime = ai_media_shared_sniff_image(contents, length);
	if (mime == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Image '%s' is not a PNG, JPEG or WebP file, the formats Meshy takes",
		            path);
		return NULL;
	}
	return ai_media_shared_data_url(mime, contents, length);
}

/* ---------------------------------------------------------------------- */
/* GObject                                                                */
/* ---------------------------------------------------------------------- */

static void
ai_meshy_client_finalize(GObject *object)
{
	AiMeshyClient *self = AI_MESHY_CLIENT(object);

	g_clear_object(&self->session);
	g_clear_pointer(&self->session_context, g_main_context_unref);
	g_clear_pointer(&self->plans, g_hash_table_unref);
	g_mutex_clear(&self->lock);
	g_free(self->base_url);
	ai_media_shared_wipe_free(self->api_key);

	G_OBJECT_CLASS(ai_meshy_client_parent_class)->finalize(object);
}

static void
ai_meshy_client_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiMeshyClient *self = AI_MESHY_CLIENT(object);

	switch (prop_id)
	{
		case PROP_BASE_URL:
			g_value_set_string(value, self->base_url);
			break;
		case PROP_TIMEOUT:
			g_value_set_uint(value, self->timeout);
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
ai_meshy_client_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiMeshyClient *self = AI_MESHY_CLIENT(object);

	switch (prop_id)
	{
		case PROP_BASE_URL:
			ai_meshy_client_set_base_url(self, g_value_get_string(value));
			break;
		case PROP_TIMEOUT:
			ai_meshy_client_set_timeout(self, g_value_get_uint(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
ai_meshy_client_class_init(AiMeshyClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize     = ai_meshy_client_finalize;
	object_class->get_property = ai_meshy_client_get_property;
	object_class->set_property = ai_meshy_client_set_property;

	/**
	 * AiMeshyClient:base-url:
	 *
	 * The API's origin.  %NULL restores %AI_MESHY_DEFAULT_BASE_URL.  The
	 * API key is sent to this origin and no other.
	 */
	properties[PROP_BASE_URL] =
		g_param_spec_string("base-url", NULL, NULL, AI_MESHY_DEFAULT_BASE_URL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiMeshyClient:timeout:
	 *
	 * Seconds any one HTTP request may take, downloads included; 0 for no
	 * limit.  It bounds a request, not a task: use the timeout of
	 * ai_media_generator_wait_async() for that, and give it an hour --
	 * Meshy says busy periods can hold a task for twenty minutes or more.
	 */
	properties[PROP_TIMEOUT] =
		g_param_spec_uint("timeout", NULL, NULL, 0, G_MAXUINT, MS_DEFAULT_TIMEOUT,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                  G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_meshy_client_init(AiMeshyClient *self)
{
	self->base_url = g_strdup(AI_MESHY_DEFAULT_BASE_URL);
	self->timeout = MS_DEFAULT_TIMEOUT;
	g_mutex_init(&self->lock);
	self->plans = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, ms_plan_free);
}

/**
 * ai_meshy_client_new:
 *
 * Creates a client with no API key.  Nothing is read from the
 * environment: call ai_meshy_client_set_api_key() before any request, or
 * every request fails with %AI_ERROR_INVALID_API_KEY.
 *
 * Returns: (transfer full): a new #AiMeshyClient
 */
AiMeshyClient *
ai_meshy_client_new(void)
{
	g_autoptr(AiMeshyClient) self = g_object_new(AI_TYPE_MESHY_CLIENT, NULL);

	return (AiMeshyClient *)g_steal_pointer(&self);
}

/**
 * ai_meshy_client_new_with_api_key:
 * @api_key: a Meshy API key (`msy_...`)
 *
 * Returns: (transfer full): a new #AiMeshyClient holding @api_key
 */
AiMeshyClient *
ai_meshy_client_new_with_api_key(const gchar *api_key)
{
	g_autoptr(AiMeshyClient) self = ai_meshy_client_new();

	ai_meshy_client_set_api_key(self, api_key);
	return (AiMeshyClient *)g_steal_pointer(&self);
}

/**
 * ai_meshy_client_set_api_key:
 * @self: an #AiMeshyClient
 * @api_key: (nullable): the API key, or %NULL to clear it
 *
 * Sets the key sent as `Authorization: Bearer <key>`.  Deliberately not a
 * property, and there is no getter: a property is readable by anything
 * holding the object.  The old key is overwritten in memory before it is
 * freed.  A key containing a line break is refused (it would start a new
 * header) and leaves the client with none.
 */
void
ai_meshy_client_set_api_key(
	AiMeshyClient *self,
	const gchar   *api_key
){
	g_return_if_fail(AI_IS_MESHY_CLIENT(self));

	ai_media_shared_wipe_free(self->api_key);
	self->api_key = NULL;

	if (api_key == NULL || api_key[0] == '\0')
	{
		return;
	}
	if (!ai_media_shared_header_value_is_valid(api_key))
	{
		g_message("ai-glib: refusing a Meshy API key containing a line break");
		return;
	}
	self->api_key = g_strdup(api_key);
}

/**
 * ai_meshy_client_has_api_key:
 * @self: an #AiMeshyClient
 *
 * Returns: whether an API key is set
 */
gboolean
ai_meshy_client_has_api_key(AiMeshyClient *self)
{
	g_return_val_if_fail(AI_IS_MESHY_CLIENT(self), FALSE);

	return self->api_key != NULL;
}

/**
 * ai_meshy_client_get_base_url:
 * @self: an #AiMeshyClient
 *
 * Returns: (transfer none): the API origin
 */
const gchar *
ai_meshy_client_get_base_url(AiMeshyClient *self)
{
	g_return_val_if_fail(AI_IS_MESHY_CLIENT(self), NULL);

	return self->base_url;
}

/**
 * ai_meshy_client_set_base_url:
 * @self: an #AiMeshyClient
 * @base_url: (nullable): the API origin, or %NULL for the default
 *
 * Points the client at another origin -- a test server, a proxy.
 */
void
ai_meshy_client_set_base_url(
	AiMeshyClient *self,
	const gchar   *base_url
){
	g_return_if_fail(AI_IS_MESHY_CLIENT(self));

	if (base_url == NULL || base_url[0] == '\0')
	{
		base_url = AI_MESHY_DEFAULT_BASE_URL;
	}
	if (g_set_str(&self->base_url, base_url))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BASE_URL]);
	}
}

/**
 * ai_meshy_client_get_timeout:
 * @self: an #AiMeshyClient
 *
 * Returns: the per-request timeout in seconds, 0 for none
 */
guint
ai_meshy_client_get_timeout(AiMeshyClient *self)
{
	g_return_val_if_fail(AI_IS_MESHY_CLIENT(self), 0);

	return self->timeout;
}

/**
 * ai_meshy_client_set_timeout:
 * @self: an #AiMeshyClient
 * @seconds: the per-request timeout, 0 for none
 *
 * Sets the per-request timeout for requests started afterwards.
 */
void
ai_meshy_client_set_timeout(
	AiMeshyClient *self,
	guint          seconds
){
	g_return_if_fail(AI_IS_MESHY_CLIENT(self));

	if (self->timeout == seconds)
	{
		return;
	}
	self->timeout = seconds;

	/* The cached session carries the old timeout; the next request makes a new one. */
	g_clear_object(&self->session);
	g_clear_pointer(&self->session_context, g_main_context_unref);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TIMEOUT]);
}
