/*
 * ai-pixellab-client.c - PixelLab pixel-art generation API (v2)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * PixelLab is a hosted, closed, prepaid service for pixel art: images,
 * eight-direction characters and their animations, tilesets, isometric
 * tiles and map objects.  Written from its OpenAPI document
 * (https://api.pixellab.ai/v2/openapi.json, checked 2026-11-23) and
 * nothing else -- its Python client carries no licence, so it was not
 * read.  docs/providers/pixellab.org records the facts relied on here.
 *
 * The v2 API answers two ways, and one AiMediaGenerator job covers both:
 *
 * - Synchronous endpoints (images, rotate, inpaint, animate-with-text)
 *   send the PNG in the response, base64.  The job is complete when submit
 *   returns: this client keeps the finished snapshot, poll answers with it
 *   at once, and its results are `data:` URLs that download decodes
 *   locally.  The id is `sync:<uuid>` and lives as long as the client.
 * - Asynchronous endpoints (characters, character animations, tilesets,
 *   isometric tiles, map objects) return background job ids and the id of
 *   the thing being made.  Poll asks GET /background-jobs/{id} for each,
 *   and once all are complete fetches the thing itself for its files.  The
 *   job id carries everything that takes -- `<operation>:<object>:<group>:
 *   <job>[,<job>...]` -- so a fresh process can resume it.
 *
 * The token goes to the API origin and nowhere else, never follows a
 * redirect, and is redacted from every error.  Nothing is read from the
 * environment.
 */

#include "config.h"

#include <string.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "providers/ai-pixellab-client.h"
#include "providers/ai-media-shared.h"
#include "core/ai-error.h"
#include "core/ai-http-redirect-private.h"

/*
 * Default whole-request timeout, in seconds.  Longer than the other media
 * clients' because a synchronous endpoint holds the connection open while
 * it generates.
 */
#define PL_DEFAULT_TIMEOUT (180)

/* Object and job ids are UUIDs. */
#define PL_MAX_ID (64)

/* One character animation is one background job per direction. */
#define PL_MAX_BACKGROUND_JOBS (16)

/* The job-id prefix of a job that finished at submit. */
#define PL_SYNC_PREFIX "sync:"

typedef enum
{
	PL_RESULT_IMAGE,        /* {"image": {...}} -> "image" */
	PL_RESULT_FRAMES,       /* {"images": [...]} -> "frame:<n>" */
	PL_RESULT_CHARACTER,    /* GET characters/{id}: rotations + spritesheet */
	PL_RESULT_ANIMATION,    /* GET characters/{id}: one animation's frames */
	PL_RESULT_TILESET,      /* GET tilesets[-sidescroller]/{id}: sheet + data */
	PL_RESULT_ISOMETRIC,    /* GET isometric-tiles/{id}: "image" */
	PL_RESULT_MAP_OBJECT    /* GET map-objects/{id}: download_url */
} PlResult;

typedef struct
{
	const gchar *name;
	const gchar *path;
	gboolean     async;
	const gchar *prompt_field;      /* where the request's prompt goes; NULL: none */
	gboolean     size_required;     /* image_size must be given */
	gint         size_min;          /* 0: no image_size to check */
	gint         size_max;
	gint         size_multiple;
	const gchar *resource_member;   /* the made object's id in the submit reply */
	const gchar *resource_path;     /* where to fetch it */
	PlResult     result;
} PlOperation;

/*
 * Every operation, with its limits from the OpenAPI document.  The limits
 * are checked before submit so a refusal names the operation and the
 * range rather than arriving as a 422 after a round trip.
 */
static const PlOperation pl_operations[] = {
	{ AI_PIXELLAB_OPERATION_IMAGE, "create-image-pixflux", FALSE, "description",
	  TRUE, 16, 400, 1, NULL, NULL, PL_RESULT_IMAGE },
	{ AI_PIXELLAB_MODEL_PIXEN, "create-image-pixen", FALSE, "description",
	  TRUE, 16, 768, 4, NULL, NULL, PL_RESULT_IMAGE },
	{ AI_PIXELLAB_OPERATION_BITFORGE, "create-image-bitforge", FALSE, "description",
	  TRUE, 16, 200, 1, NULL, NULL, PL_RESULT_IMAGE },
	{ AI_PIXELLAB_OPERATION_ROTATE, "rotate", FALSE, NULL,
	  TRUE, 16, 200, 1, NULL, NULL, PL_RESULT_IMAGE },
	{ AI_PIXELLAB_OPERATION_INPAINT, "inpaint", FALSE, "description",
	  TRUE, 16, 200, 1, NULL, NULL, PL_RESULT_IMAGE },
	{ AI_PIXELLAB_OPERATION_ANIMATE, "animate-with-text", FALSE, "description",
	  TRUE, 64, 64, 1, NULL, NULL, PL_RESULT_FRAMES },
	{ AI_PIXELLAB_OPERATION_CHARACTER_4, "create-character-with-4-directions", TRUE, "description",
	  TRUE, 16, 128, 1, "character_id", "characters", PL_RESULT_CHARACTER },
	{ AI_PIXELLAB_OPERATION_CHARACTER_8, "create-character-with-8-directions", TRUE, "description",
	  TRUE, 16, 128, 1, "character_id", "characters", PL_RESULT_CHARACTER },
	{ AI_PIXELLAB_OPERATION_CHARACTER_ANIMATE, "characters/animations", TRUE, "action_description",
	  FALSE, 0, 0, 0, "character_id", "characters", PL_RESULT_ANIMATION },
	{ AI_PIXELLAB_OPERATION_TILESET_TOPDOWN, "create-tileset", TRUE, NULL,
	  FALSE, 0, 0, 0, "tileset_id", "tilesets", PL_RESULT_TILESET },
	{ AI_PIXELLAB_OPERATION_TILESET_SIDESCROLLER, "create-tileset-sidescroller", TRUE, NULL,
	  FALSE, 0, 0, 0, "tileset_id", "tilesets-sidescroller", PL_RESULT_TILESET },
	{ AI_PIXELLAB_OPERATION_ISOMETRIC_TILE, "create-isometric-tile", TRUE, "description",
	  TRUE, 16, 64, 1, "tile_id", "isometric-tiles", PL_RESULT_ISOMETRIC },
	{ AI_PIXELLAB_OPERATION_MAP_OBJECT, "map-objects", TRUE, "description",
	  FALSE, 32, 400, 1, "object_id", "map-objects", PL_RESULT_MAP_OBJECT },
	{ NULL, NULL, FALSE, NULL, FALSE, 0, 0, 0, NULL, NULL, PL_RESULT_IMAGE }
};

/* What a submit's `usage` said, for a background job whose own does not. */
typedef struct
{
	gint64  micro_usd;
	gdouble generations;
} PlCost;

struct _AiPixellabClient
{
	GObject parent_instance;

	gchar        *base_url;
	gchar        *api_key;
	guint         timeout;

	/* See ai-higgsfield-client.c: a session belongs to its context. */
	SoupSession  *session;
	GMainContext *session_context;

	GMutex        lock;
	GHashTable   *sync_jobs;    /* job id -> AiMediaJob, finished at submit */
	GHashTable   *costs;        /* job id -> PlCost, from the submit reply */
};

static void media_generator_iface_init(AiMediaGeneratorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(AiPixellabClient, ai_pixellab_client, G_TYPE_OBJECT,
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

/* ---------------------------------------------------------------------- */
/* Errors                                                                 */
/* ---------------------------------------------------------------------- */

static GError *
pl_error_new(
	AiPixellabClient *self,
	gint              code,
	gchar            *message
){
	GError *error;

	message = ai_media_shared_redact(message, self->api_key);
	error = g_error_new_literal(AI_ERROR, code, message);
	g_free(message);

	return error;
}

/*
 * Maps a non-2xx status onto an AiError, with the meaning the OpenAPI
 * document gives each.  PixelLab sends two rate limits: 429 "too many
 * requests" and 529 "rate limit exceeded"; both are RATE_LIMITED, named
 * apart in the message.
 */
static GError *
pl_status_error(
	AiPixellabClient *self,
	const gchar      *what,
	guint             status,
	GBytes           *body,
	SoupMessage      *message,
	gboolean          job_scoped
){
	g_autofree gchar *detail = ai_media_shared_error_detail(body);
	const gchar *retry_after = NULL;
	const gchar *hint = "";
	gint code;

	if (SOUP_STATUS_IS_REDIRECTION(status))
	{
		return pl_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                    g_strdup_printf("PixelLab %s answered with a redirect (HTTP %u), "
		                                    "which is not followed so the token stays on the "
		                                    "API origin", what, status));
	}

	switch (status)
	{
		case 400:
		case 409:
		case 422:
			code = AI_ERROR_INVALID_REQUEST;
			break;
		case 401:
			code = AI_ERROR_INVALID_API_KEY;
			hint = " -- invalid API token";
			break;
		case 402:
			code = AI_ERROR_INSUFFICIENT_QUOTA;
			hint = " -- insufficient credits";
			break;
		case 403:
			code = AI_ERROR_PERMISSION_DENIED;
			break;
		case 404:
			code = AI_ERROR_INVALID_REQUEST;
			if (job_scoped)
			{
				hint = " -- no such job or object for this account";
			}
			break;
		case 423:
			code = AI_ERROR_SERVICE_UNAVAILABLE;
			hint = " -- still being generated";
			break;
		case 429:
		case 529:
			code = AI_ERROR_RATE_LIMITED;
			hint = (status == 429) ? " -- too many requests" : " -- rate limit exceeded";
			if (message != NULL)
			{
				retry_after = soup_message_headers_get_one(
					soup_message_get_response_headers(message), "Retry-After");
			}
			break;
		case 503:
			code = AI_ERROR_SERVICE_UNAVAILABLE;
			break;
		default:
			code = (status >= 500) ? AI_ERROR_SERVER_ERROR : AI_ERROR_UNKNOWN;
			break;
	}

	return pl_error_new(self, code,
	                    g_strdup_printf("PixelLab %s failed (HTTP %u)%s%s%s%s%s%s",
	                                    what, status,
	                                    (detail != NULL) ? ": " : "",
	                                    (detail != NULL) ? detail : "",
	                                    hint,
	                                    (retry_after != NULL) ? " (retry after " : "",
	                                    (retry_after != NULL) ? retry_after : "",
	                                    (retry_after != NULL) ? " s)" : ""));
}

static JsonObject *
pl_parse_object(
	AiPixellabClient  *self,
	const gchar       *what,
	GBytes            *body,
	GError           **error
){
	g_autofree gchar *why = NULL;
	JsonObject *object = ai_media_shared_parse_object(body, &why);

	if (object == NULL)
	{
		g_propagate_error(error,
		                  pl_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                               g_strdup_printf("PixelLab %s returned %s", what, why)));
	}
	return object;
}

/* ---------------------------------------------------------------------- */
/* JSON readers                                                           */
/* ---------------------------------------------------------------------- */

static const gchar *
pl_string(
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

static JsonObject *
pl_object(
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

static JsonArray *
pl_array(
	JsonObject  *object,
	const gchar *member
){
	JsonNode *node;

	if (object == NULL)
	{
		return NULL;
	}
	node = json_object_get_member(object, member);
	return (node != NULL && JSON_NODE_HOLDS_ARRAY(node)) ? json_node_get_array(node) : NULL;
}

/* A JSON number member as a double; FALSE when absent, null or not a number. */
static gboolean
pl_number(
	JsonObject  *object,
	const gchar *member,
	gdouble     *value
){
	JsonNode *node = (object != NULL) ? json_object_get_member(object, member) : NULL;
	GType type;

	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
	{
		return FALSE;
	}
	type = json_node_get_value_type(node);
	if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE)
	{
		return FALSE;
	}
	*value = json_node_get_double(node);
	return TRUE;
}

/*
 * Adds a `usage` object to a running total.  It is {"type": "usd",
 * "usd": 0.01} for a credit balance or {"type": "generations",
 * "generations": 1} for a subscription; either member may be null.
 */
static void
pl_add_usage(
	JsonObject *object,
	PlCost     *cost
){
	JsonObject *usage = pl_object(object, "usage");
	gint64 micro_usd;
	gdouble generations;

	if (usage == NULL)
	{
		return;
	}
	if (ai_media_shared_usd_node_to_micro(json_object_get_member(usage, "usd"), &micro_usd))
	{
		cost->micro_usd = (cost->micro_usd >= 0) ? cost->micro_usd + micro_usd : micro_usd;
	}
	if (pl_number(usage, "generations", &generations))
	{
		cost->generations = (cost->generations >= 0) ? cost->generations + generations : generations;
	}
}

/* Records a cost on a job: money, and generations as the job's credits. */
static void
pl_apply_cost(
	AiMediaJob   *job,
	const PlCost *cost
){
	ai_media_job_set_cost_micro_usd(job, cost->micro_usd);
	ai_media_job_set_credits(job, (cost->generations >= 0)
	                                  ? (gint64)(cost->generations + 0.5) : AI_MEDIA_COST_UNKNOWN);
}

/* A {"type": "base64", "base64": ..., "format": ...} image as a data URL. */
static gchar *
pl_image_data_url(JsonNode *node)
{
	JsonObject *image;

	if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
	{
		return NULL;
	}
	image = json_node_get_object(node);
	return ai_media_shared_base64_to_data_url(pl_string(image, "base64"),
	                                         pl_string(image, "format"));
}

/* ---------------------------------------------------------------------- */
/* Operations and job ids                                                 */
/* ---------------------------------------------------------------------- */

static const PlOperation *
pl_operation_lookup(const gchar *name)
{
	guint i;

	for (i = 0; name != NULL && pl_operations[i].name != NULL; i++)
	{
		if (g_strcmp0(pl_operations[i].name, name) == 0)
		{
			return &pl_operations[i];
		}
	}
	return NULL;
}

static gchar *
pl_operation_names(void)
{
	GString *out = g_string_new(NULL);
	guint i;

	for (i = 0; pl_operations[i].name != NULL; i++)
	{
		if (g_strcmp0(pl_operations[i].name, AI_PIXELLAB_MODEL_PIXEN) == 0)
		{
			continue;
		}
		if (out->len > 0)
		{
			g_string_append(out, ", ");
		}
		g_string_append(out, pl_operations[i].name);
	}
	return g_string_free(out, FALSE);
}

/*
 * Checks `image_size` against the operation's limits.  PixelLab's own
 * validation would answer 422 after a round trip; this names the
 * operation and the range before anything is sent or charged.
 */
static gboolean
pl_check_size(
	const PlOperation  *operation,
	JsonObject         *body,
	GError            **error
){
	JsonObject *size = pl_object(body, "image_size");
	gdouble width;
	gdouble height;
	gint w;
	gint h;

	if (operation->size_min == 0)
	{
		return TRUE;
	}
	if (size == NULL)
	{
		if (operation->size_required)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "PixelLab %s needs image_size {\"width\", \"height\"}, each %d to %d",
			            operation->path, operation->size_min, operation->size_max);
			return FALSE;
		}
		return TRUE;
	}
	if (!pl_number(size, "width", &width) || !pl_number(size, "height", &height) ||
	    width != (gint)width || height != (gint)height)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "PixelLab %s: image_size needs integer width and height", operation->path);
		return FALSE;
	}
	w = (gint)width;
	h = (gint)height;

	if (w < operation->size_min || w > operation->size_max ||
	    h < operation->size_min || h > operation->size_max)
	{
		if (operation->size_min == operation->size_max)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "PixelLab %s takes only %dx%d, not %dx%d", operation->path,
			            operation->size_min, operation->size_max, w, h);
		}
		else
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "PixelLab %s takes a width and height of %d to %d pixels, not %dx%d",
			            operation->path, operation->size_min, operation->size_max, w, h);
		}
		return FALSE;
	}
	if (operation->size_multiple > 1 &&
	    (w % operation->size_multiple != 0 || h % operation->size_multiple != 0))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "PixelLab %s takes a width and height divisible by %d, not %dx%d",
		            operation->path, operation->size_multiple, w, h);
		return FALSE;
	}
	return TRUE;
}

/*
 * The request as the JSON body PixelLab wants.  `operation` is ai-glib's
 * word and is taken out; the prompt moves to the field the endpoint calls
 * it (usually `description`), because every v2 body refuses members it
 * does not declare.  An explicit parameter of that name wins over the
 * prompt.  An endpoint with no such field refuses a prompt rather than
 * dropping it.
 */
static JsonObject *
pl_request_body(
	AiMediaRequest     *request,
	const PlOperation **operation,
	GError            **error
){
	g_autofree gchar *json = ai_media_request_to_json(request);
	g_autoptr(JsonObject) body = ai_media_shared_object_from_text(json);
	const gchar *model = ai_media_request_get_model(request);
	const gchar *name = model;
	const gchar *prompt;

	if (body == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "The PixelLab request could not be serialised as a JSON object");
		return NULL;
	}

	if (json_object_has_member(body, "operation"))
	{
		name = pl_string(body, "operation");
	}
	/* `image` with model pixen is the pixen endpoint. */
	if (g_strcmp0(name, AI_PIXELLAB_OPERATION_IMAGE) == 0 &&
	    g_strcmp0(model, AI_PIXELLAB_MODEL_PIXEN) == 0)
	{
		name = AI_PIXELLAB_MODEL_PIXEN;
	}

	*operation = pl_operation_lookup(name);
	if (*operation == NULL)
	{
		g_autofree gchar *names = pl_operation_names();

		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "'%s' is not a PixelLab operation; set the request's 'operation' "
		            "parameter (or its model) to one of: %s",
		            (name != NULL) ? name : "(none)", names);
		return NULL;
	}
	json_object_remove_member(body, "operation");

	prompt = pl_string(body, "prompt");
	if (prompt != NULL)
	{
		if ((*operation)->prompt_field == NULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "PixelLab %s takes no prompt; describe it with the endpoint's own "
			            "parameters (see docs/providers/pixellab.org)", (*operation)->path);
			return NULL;
		}
		if (!json_object_has_member(body, (*operation)->prompt_field))
		{
			json_object_set_string_member(body, (*operation)->prompt_field, prompt);
		}
	}
	json_object_remove_member(body, "prompt");

	if (!pl_check_size(*operation, body, error))
	{
		return NULL;
	}
	return (JsonObject *)g_steal_pointer(&body);
}

/*
 * A background job id: `<operation>:<object>:<group>:<job>[,<job>...]`.
 * <group> is a character animation's group id, or "-".  Every part is
 * spliced into a URL path, so every part is checked.
 */
typedef struct
{
	const PlOperation *operation;
	gchar             *object_id;
	gchar             *group_id;    /* NULL for "-" */
	gchar            **job_ids;
} PlJobId;

static void
pl_job_id_clear(PlJobId *id)
{
	g_clear_pointer(&id->object_id, g_free);
	g_clear_pointer(&id->group_id, g_free);
	g_clear_pointer(&id->job_ids, g_strfreev);
}

static gboolean
pl_parse_job_id(
	const gchar *text,
	PlJobId     *id
){
	g_auto(GStrv) parts = NULL;
	guint i;

	memset(id, 0, sizeof *id);
	if (text == NULL || strlen(text) > (PL_MAX_BACKGROUND_JOBS + 4) * (PL_MAX_ID + 1))
	{
		return FALSE;
	}
	parts = g_strsplit(text, ":", 5);
	if (g_strv_length(parts) != 4)
	{
		return FALSE;
	}

	id->operation = pl_operation_lookup(parts[0]);
	if (id->operation == NULL || !id->operation->async ||
	    !ai_media_shared_id_is_valid(parts[1], PL_MAX_ID) ||
	    (g_strcmp0(parts[2], "-") != 0 && !ai_media_shared_id_is_valid(parts[2], PL_MAX_ID)))
	{
		return FALSE;
	}

	id->job_ids = g_strsplit(parts[3], ",", PL_MAX_BACKGROUND_JOBS + 1);
	if (g_strv_length(id->job_ids) == 0 || g_strv_length(id->job_ids) > PL_MAX_BACKGROUND_JOBS)
	{
		pl_job_id_clear(id);
		return FALSE;
	}
	for (i = 0; id->job_ids[i] != NULL; i++)
	{
		if (!ai_media_shared_id_is_valid(id->job_ids[i], PL_MAX_ID))
		{
			pl_job_id_clear(id);
			return FALSE;
		}
	}

	id->object_id = g_strdup(parts[1]);
	id->group_id = (g_strcmp0(parts[2], "-") != 0) ? g_strdup(parts[2]) : NULL;
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                */
/* ---------------------------------------------------------------------- */

typedef void (*PlReply)(GTask *task, guint status, GBytes *body);

typedef struct
{
	SoupSession       *session;
	SoupMessage       *message;
	PlReply            reply;
	const gchar       *what;        /* static: names the call in messages */

	const PlOperation *operation;
	gchar             *job_id;
	gchar             *request_object_id;   /* the object a request names, if any */
	PlJobId            id;
	guint              index;       /* the background job being asked about */
	gboolean           any_processing;
	gboolean           any_queued;
	gboolean           failed;
	gchar             *fail_message;
	PlCost             cost;
} PlOp;

static void
pl_op_free(gpointer data)
{
	PlOp *op = data;

	g_clear_object(&op->session);
	g_clear_object(&op->message);
	g_free(op->job_id);
	g_free(op->request_object_id);
	pl_job_id_clear(&op->id);
	g_free(op->fail_message);
	g_free(op);
}

static SoupSession *
pl_session_for(
	AiPixellabClient *self,
	GMainContext     *context
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
pl_task_new(
	AiPixellabClient    *self,
	gpointer             tag,
	const gchar         *what,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	PlOp *op;

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, tag);
	g_task_set_static_name(task, what);

	op = g_new0(PlOp, 1);
	op->what = what;
	op->cost.micro_usd = AI_MEDIA_COST_UNKNOWN;
	op->cost.generations = -1;
	op->session = pl_session_for(self, g_task_get_context(task));
	g_task_set_task_data(task, op, pl_op_free);

	return task;
}

/* An authenticated request to `<base-url>/<path>[?query]`, never redirected. */
static SoupMessage *
pl_api_message(
	AiPixellabClient  *self,
	const gchar       *method,
	const gchar       *path,
	const gchar       *query,
	JsonObject        *json_body,
	GError           **error
){
	g_autofree gchar *url = NULL;
	g_autoptr(GUri) uri = NULL;
	gchar *authorization;
	SoupMessage *message;
	SoupMessageHeaders *headers;

	if (self->api_key == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
		                    "No PixelLab API token: call ai_pixellab_client_set_api_key() "
		                    "with the token from pixellab.ai/account");
		return NULL;
	}

	url = ai_media_shared_join_url(self->base_url, path, query);
	uri = ai_media_shared_parse_http_uri(url);
	if (uri == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		            "PixelLab base URL '%s' is not an http or https URL", self->base_url);
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
pl_send_cb(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = G_TASK(user_data);
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);
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
		                    pl_error_new(self,
		                                 g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)
		                                     ? AI_ERROR_TIMEOUT : AI_ERROR_NETWORK_ERROR,
		                                 g_strdup_printf("PixelLab %s failed: %s",
		                                                 op->what, error->message)));
		return;
	}

	op->reply(task, soup_message_get_status(op->message), body);
}

static void
pl_send(
	GTask       *task,
	SoupMessage *message,
	PlReply      reply
){
	PlOp *op = g_task_get_task_data(task);

	g_set_object(&op->message, message);
	g_object_unref(message);
	op->reply = reply;

	g_debug("pixellab: %s %s", soup_message_get_method(message), op->what);
	soup_session_send_and_read_async(op->session, op->message, G_PRIORITY_DEFAULT,
	                                 g_task_get_cancellable(task),
	                                 pl_send_cb, g_object_ref(task));
}

static void
pl_send_api(
	GTask       *task,
	const gchar *method,
	const gchar *path,
	const gchar *query,
	JsonObject  *json_body,
	PlReply      reply
){
	AiPixellabClient *self = g_task_get_source_object(task);
	g_autoptr(GError) error = NULL;
	SoupMessage *message;

	message = pl_api_message(self, method, path, query, json_body, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	pl_send(task, message, reply);
}

/* ---------------------------------------------------------------------- */
/* Submit                                                                 */
/* ---------------------------------------------------------------------- */

/* A synchronous answer: the finished job, kept for poll. */
static void
pl_submit_sync_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *job_id = NULL;
	GError *error = NULL;
	PlCost cost = { AI_MEDIA_COST_UNKNOWN, -1 };
	guint added = 0;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, pl_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}
	object = pl_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	uuid = g_uuid_string_random();
	job_id = g_strconcat(PL_SYNC_PREFIX, uuid, NULL);
	job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_DONE);
	ai_media_job_set_provider_status(job, "completed");

	if (op->operation->result == PL_RESULT_FRAMES)
	{
		JsonArray *images = pl_array(object, "images");
		guint i;

		for (i = 0; images != NULL && i < json_array_get_length(images); i++)
		{
			g_autofree gchar *url = pl_image_data_url(json_array_get_element(images, i));
			g_autofree gchar *name = g_strdup_printf("frame:%u", i);

			if (url != NULL)
			{
				ai_media_job_add_named_result(job, name, url);
				added++;
			}
		}
	}
	else
	{
		g_autofree gchar *url = pl_image_data_url(json_object_get_member(object, "image"));

		if (url != NULL)
		{
			ai_media_job_add_named_result(job, "image", url);
			added++;
		}
	}
	if (added == 0)
	{
		g_task_return_error(task,
		                    pl_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup_printf("PixelLab %s returned no usable base64 "
		                                                 "image", op->what)));
		return;
	}

	pl_add_usage(object, &cost);
	pl_apply_cost(job, &cost);

	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->sync_jobs, g_strdup(job_id), g_object_ref(job));
	g_mutex_unlock(&self->lock);

	g_task_return_pointer(task, g_steal_pointer(&job_id), g_free);
}

/* An asynchronous answer: background job ids and the object being made. */
static void
pl_submit_async_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	GString *jobs = NULL;
	GError *error = NULL;
	const gchar *object_id;
	const gchar *group_id;
	const gchar *single;
	JsonArray *many;
	PlCost *cost;
	guint count = 0;
	guint i;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, pl_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}
	object = pl_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	jobs = g_string_new(NULL);
	single = pl_string(object, "background_job_id");
	many = pl_array(object, "background_job_ids");
	if (single != NULL && ai_media_shared_id_is_valid(single, PL_MAX_ID))
	{
		g_string_append(jobs, single);
		count++;
	}
	for (i = 0; many != NULL && i < json_array_get_length(many); i++)
	{
		JsonNode *node = json_array_get_element(many, i);
		const gchar *one;

		if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
		{
			continue;
		}
		one = json_node_get_string(node);
		if (!ai_media_shared_id_is_valid(one, PL_MAX_ID))
		{
			continue;
		}
		g_string_append_printf(jobs, "%s%s", (count > 0) ? "," : "", one);
		count++;
	}

	/*
	 * Most replies name the object they are making.  A character
	 * animation's does not (CreateCharacterAnimationResponse has no
	 * character_id): the character is the one the request named.
	 */
	object_id = pl_string(object, op->operation->resource_member);
	if (object_id == NULL)
	{
		object_id = op->request_object_id;
	}
	if (count == 0 || count > PL_MAX_BACKGROUND_JOBS ||
	    !ai_media_shared_id_is_valid(object_id, PL_MAX_ID))
	{
		g_string_free(jobs, TRUE);
		g_task_return_error(task,
		                    pl_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup_printf("PixelLab %s returned no usable "
		                                                 "background job and %s", op->what,
		                                                 op->operation->resource_member)));
		return;
	}

	group_id = pl_string(object, "animation_group_id");
	if (!ai_media_shared_id_is_valid(group_id, PL_MAX_ID))
	{
		group_id = NULL;
	}
	op->job_id = g_strdup_printf("%s:%s:%s:%s", op->operation->name, object_id,
	                             (group_id != NULL) ? group_id : "-", jobs->str);
	g_string_free(jobs, TRUE);

	/* What the submit cost, for a poll whose background jobs do not say. */
	cost = g_new0(PlCost, 1);
	cost->micro_usd = AI_MEDIA_COST_UNKNOWN;
	cost->generations = -1;
	pl_add_usage(object, cost);
	g_mutex_lock(&self->lock);
	g_hash_table_replace(self->costs, g_strdup(op->job_id), cost);
	g_mutex_unlock(&self->lock);

	g_task_return_pointer(task, g_strdup(op->job_id), g_free);
}

static void
pl_submit_async(
	AiMediaGenerator    *generator,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiPixellabClient *self = AI_PIXELLAB_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(JsonObject) body = NULL;
	g_autoptr(GError) error = NULL;
	PlOp *op;

	task = pl_task_new(self, pl_submit_async, "submit", cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	if (ai_media_request_get_webhook_url(request) != NULL ||
	    ai_media_request_get_expires_after(request) != 0)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
		                        "PixelLab takes no webhook or retention: poll the job, and "
		                        "fetch map objects within 8 hours");
		return;
	}

	body = pl_request_body(request, &op->operation, &error);
	if (body == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	if (op->operation->resource_member != NULL)
	{
		op->request_object_id = g_strdup(pl_string(body, op->operation->resource_member));
	}

	pl_send_api(task, "POST", op->operation->path, NULL, body,
	            op->operation->async ? pl_submit_async_reply : pl_submit_sync_reply);
}

static gchar *
pl_submit_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(g_task_is_valid(result, generator), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* Poll                                                                   */
/* ---------------------------------------------------------------------- */

static void pl_poll_next(GTask *task);

/* A job, in a state, with the cost found so far. */
static AiMediaJob *
pl_job_new(
	AiPixellabClient *self,
	PlOp             *op,
	AiMediaJobState   state,
	const gchar      *provider_status
){
	AiMediaJob *job = ai_media_job_new(op->job_id, state);
	PlCost cost = op->cost;

	/* The background jobs' own usage, else what the submit said. */
	if (cost.micro_usd < 0 && cost.generations < 0)
	{
		PlCost *remembered;

		g_mutex_lock(&self->lock);
		remembered = g_hash_table_lookup(self->costs, op->job_id);
		if (remembered != NULL)
		{
			cost = *remembered;
		}
		g_mutex_unlock(&self->lock);
	}
	pl_apply_cost(job, &cost);
	ai_media_job_set_provider_status(job, provider_status);
	return job;
}

static void
pl_return_job(
	GTask      *task,
	AiMediaJob *job
){
	g_task_return_pointer(task, job, g_object_unref);
}

/* Adds a character's rotations and its spritesheet export. */
static void
pl_add_character(
	AiPixellabClient *self,
	AiMediaJob       *job,
	JsonObject       *character,
	const gchar      *character_id
){
	JsonObject *rotations = pl_object(character, "rotation_urls");
	g_autofree gchar *path = g_strdup_printf("characters/%s/spritesheet", character_id);
	g_autofree gchar *sheet = ai_media_shared_join_url(self->base_url, path, NULL);
	GList *names;
	GList *iter;

	/* The ZIP of sheet and layout: public by character id, no token needed. */
	ai_media_job_add_named_result(job, "spritesheet", sheet);

	if (rotations == NULL)
	{
		return;
	}
	names = json_object_get_members(rotations);
	for (iter = names; iter != NULL; iter = iter->next)
	{
		const gchar *direction = iter->data;
		const gchar *url = pl_string(rotations, direction);

		if (url != NULL)
		{
			ai_media_job_add_named_result(job, direction, url);
		}
	}
	g_list_free(names);
}

/*
 * Adds one animation's frames as `<direction>:<n>`.  Without a group id
 * (an id from a reply that named none) every animation is taken, named
 * `<animation type>:<direction>:<n>` so two cannot collide.
 */
static guint
pl_add_animation(
	AiMediaJob  *job,
	JsonObject  *character,
	const gchar *group_id
){
	JsonArray *groups = pl_array(character, "animations");
	guint added = 0;
	guint i;

	for (i = 0; groups != NULL && i < json_array_get_length(groups); i++)
	{
		JsonNode *node = json_array_get_element(groups, i);
		JsonObject *group;
		JsonArray *directions;
		const gchar *type;
		guint d;

		if (!JSON_NODE_HOLDS_OBJECT(node))
		{
			continue;
		}
		group = json_node_get_object(node);
		if (group_id != NULL && g_strcmp0(pl_string(group, "animation_group_id"), group_id) != 0)
		{
			continue;
		}
		type = pl_string(group, "animation_type");
		directions = pl_array(group, "directions");

		for (d = 0; directions != NULL && d < json_array_get_length(directions); d++)
		{
			JsonNode *dnode = json_array_get_element(directions, d);
			JsonArray *frames;
			const gchar *direction;
			guint f;

			if (!JSON_NODE_HOLDS_OBJECT(dnode))
			{
				continue;
			}
			direction = pl_string(json_node_get_object(dnode), "direction");
			frames = pl_array(json_node_get_object(dnode), "frames");
			for (f = 0; direction != NULL && frames != NULL && f < json_array_get_length(frames); f++)
			{
				JsonNode *frame = json_array_get_element(frames, f);
				g_autofree gchar *name = NULL;

				if (!JSON_NODE_HOLDS_VALUE(frame) || json_node_get_value_type(frame) != G_TYPE_STRING)
				{
					continue;
				}
				name = (group_id != NULL)
					? g_strdup_printf("%s:%u", direction, f)
					: g_strdup_printf("%s:%s:%u", (type != NULL) ? type : "animation", direction, f);
				ai_media_job_add_named_result(job, name, json_node_get_string(frame));
				added++;
			}
		}
	}
	return added;
}

/* The made object, read once every background job has completed. */
static void
pl_poll_object_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	GError *error = NULL;

	/* 423: the jobs are done but the object is still being stored. */
	if (status == 423)
	{
		pl_return_job(task, pl_job_new(self, op, AI_MEDIA_JOB_STATE_RUNNING, "processing"));
		return;
	}
	/* 410: a map object whose generation failed for good. */
	if (status == 410)
	{
		g_autofree gchar *detail = ai_media_shared_error_detail(body);

		job = pl_job_new(self, op, AI_MEDIA_JOB_STATE_FAILED, "failed");
		ai_media_job_set_error_message(job, (detail != NULL) ? detail : "Generation failed");
		pl_return_job(task, g_steal_pointer(&job));
		return;
	}
	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, pl_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}
	object = pl_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	job = pl_job_new(self, op, AI_MEDIA_JOB_STATE_DONE, "completed");

	switch (op->id.operation->result)
	{
		case PL_RESULT_CHARACTER:
			/* Rotation URLs are null until the character's status is completed. */
			if (pl_object(object, "rotation_urls") == NULL)
			{
				g_clear_object(&job);
				job = pl_job_new(self, op, AI_MEDIA_JOB_STATE_RUNNING, "processing");
				break;
			}
			pl_add_character(self, job, object, op->id.object_id);
			break;

		case PL_RESULT_ANIMATION:
			/* The group appears in the listing once its frames are stored. */
			if (pl_add_animation(job, object, op->id.group_id) == 0)
			{
				g_clear_object(&job);
				job = pl_job_new(self, op, AI_MEDIA_JOB_STATE_RUNNING, "processing");
				break;
			}
			{
				g_autofree gchar *path = g_strdup_printf("characters/%s/spritesheet",
				                                         op->id.object_id);
				g_autofree gchar *sheet = ai_media_shared_join_url(self->base_url, path, NULL);

				ai_media_job_add_named_result(job, "spritesheet", sheet);
			}
			break;

		case PL_RESULT_TILESET:
		{
			JsonObject *sheet = pl_object(object, "sheet");
			g_autofree gchar *image = NULL;
			g_autofree gchar *data = NULL;
			gsize length = 0;
			g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);

			if (sheet != NULL)
			{
				image = pl_image_data_url(json_object_get_member(sheet, "image"));
			}
			if (image != NULL)
			{
				ai_media_job_add_named_result(job, "sheet", image);
			}
			/* The tiles, their corners and patterns: what a map needs to place them. */
			json_node_set_object(root, object);
			data = json_to_string(root, FALSE);
			length = strlen(data);
			{
				g_autofree gchar *url = ai_media_shared_data_url("application/json", data, length);

				ai_media_job_add_named_result(job, "tileset", url);
			}
			break;
		}

		case PL_RESULT_ISOMETRIC:
		{
			g_autofree gchar *image = pl_image_data_url(json_object_get_member(object, "image"));

			if (image != NULL)
			{
				ai_media_job_add_named_result(job, "image", image);
			}
			break;
		}

		case PL_RESULT_MAP_OBJECT:
		{
			const gchar *url = pl_string(object, "download_url");

			if (url == NULL)
			{
				g_clear_object(&job);
				job = pl_job_new(self, op, AI_MEDIA_JOB_STATE_RUNNING,
				                 (pl_string(object, "status") != NULL)
				                     ? pl_string(object, "status") : "processing");
				break;
			}
			ai_media_job_add_named_result(job, "image", url);
			break;
		}

		case PL_RESULT_IMAGE:
		case PL_RESULT_FRAMES:
			/* Synchronous: never polled here. */
			break;
	}

	pl_return_job(task, g_steal_pointer(&job));
}

/* Every background job has answered: decide, or fetch the object. */
static void
pl_poll_settle(GTask *task)
{
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);
	g_autofree gchar *path = NULL;
	const gchar *query = NULL;

	if (op->failed)
	{
		AiMediaJob *job = pl_job_new(self, op, AI_MEDIA_JOB_STATE_FAILED, "failed");

		ai_media_job_set_error_message(job, (op->fail_message != NULL)
		                                        ? op->fail_message : "Generation failed");
		pl_return_job(task, job);
		return;
	}
	if (op->any_processing)
	{
		pl_return_job(task, pl_job_new(self, op,
		                               op->any_queued ? AI_MEDIA_JOB_STATE_QUEUED
		                                              : AI_MEDIA_JOB_STATE_RUNNING,
		                               "processing"));
		return;
	}

	path = g_strdup_printf("%s/%s", op->id.operation->resource_path, op->id.object_id);
	if (op->id.operation->result == PL_RESULT_TILESET)
	{
		/* Without a layout the reply carries tiles but no sheet. */
		query = "layout=tileset15";
	}
	op->what = "result";
	pl_send_api(task, "GET", path, query, NULL, pl_poll_object_reply);
}

/* What a failed background job said, from wherever it put it. */
static gchar *
pl_failure_text(
	AiPixellabClient *self,
	JsonObject       *job
){
	JsonObject *last = pl_object(job, "last_response");
	static const gchar * const members[] = { "error", "message", "detail", "reason", NULL };
	guint i;

	for (i = 0; last != NULL && members[i] != NULL; i++)
	{
		if (pl_string(last, members[i]) != NULL)
		{
			return ai_media_shared_redact(g_strdup(pl_string(last, members[i])), self->api_key);
		}
	}
	return NULL;
}

static void
pl_poll_job_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	GError *error = NULL;
	const gchar *word;

	/*
	 * PixelLab cleans a background job up after it completes, so a 404 for
	 * one is not a failure: the object itself is asked once all are done.
	 */
	if (status == 404)
	{
		op->index++;
		pl_poll_next(task);
		return;
	}
	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, pl_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}
	object = pl_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	pl_add_usage(object, &op->cost);
	word = pl_string(object, "status");
	if (g_strcmp0(word, "processing") == 0)
	{
		gdouble position = 0;

		op->any_processing = TRUE;
		if (pl_number(pl_object(object, "last_response"), "queue_position", &position) &&
		    position > 0)
		{
			op->any_queued = TRUE;
		}
	}
	else if (g_strcmp0(word, "failed") == 0)
	{
		op->failed = TRUE;
		if (op->fail_message == NULL)
		{
			op->fail_message = pl_failure_text(self, object);
		}
	}
	else if (g_strcmp0(word, "completed") != 0)
	{
		/* An unknown word is not a guess: see the Meshy client's note. */
		g_task_return_error(task,
		                    pl_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup_printf("PixelLab reported an unknown status "
		                                                 "'%s' for background job %s",
		                                                 (word != NULL) ? word : "(none)",
		                                                 op->id.job_ids[op->index])));
		return;
	}

	op->index++;
	pl_poll_next(task);
}

/* Asks about the next background job, or settles once all have answered. */
static void
pl_poll_next(GTask *task)
{
	PlOp *op = g_task_get_task_data(task);
	g_autofree gchar *path = NULL;

	if (op->id.job_ids[op->index] == NULL)
	{
		pl_poll_settle(task);
		return;
	}
	path = g_strdup_printf("background-jobs/%s", op->id.job_ids[op->index]);
	op->what = "status";
	pl_send_api(task, "GET", path, NULL, NULL, pl_poll_job_reply);
}

static void
pl_poll_async(
	AiMediaGenerator    *generator,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiPixellabClient *self = AI_PIXELLAB_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	PlOp *op;

	task = pl_task_new(self, pl_poll_async, "status", cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	/* Finished at submit: the snapshot this client kept. */
	if (job_id != NULL && g_str_has_prefix(job_id, PL_SYNC_PREFIX))
	{
		AiMediaJob *job;

		g_mutex_lock(&self->lock);
		job = g_hash_table_lookup(self->sync_jobs, job_id);
		if (job != NULL)
		{
			g_object_ref(job);
		}
		g_mutex_unlock(&self->lock);

		if (job == NULL)
		{
			g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                        "PixelLab job %s finished at submit in another client; "
			                        "its images were held only there", job_id);
			return;
		}
		pl_return_job(task, job);
		return;
	}

	if (!pl_parse_job_id(job_id, &op->id))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a PixelLab job id", job_id);
		return;
	}
	op->job_id = g_strdup(job_id);
	pl_poll_next(task);
}

static AiMediaJob *
pl_poll_finish(
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
pl_download_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, pl_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}
	g_task_return_pointer(task, g_bytes_ref(body), (GDestroyNotify)g_bytes_unref);
}

/*
 * A `data:` result is decoded here, without a request.  Anything else is
 * fetched without the token: rotation and frame URLs are public storage,
 * and the spritesheet export is public by character id.
 */
static void
pl_download_async(
	AiMediaGenerator    *generator,
	const gchar         *url,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiPixellabClient *self = AI_PIXELLAB_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GUri) uri = NULL;

	task = pl_task_new(self, pl_download_async, "download", cancellable, callback, user_data);

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
	pl_send(task, ai_http_message_new_from_uri("GET", uri, AI_HTTP_PUBLIC_URL), pl_download_reply);
}

static GBytes *
pl_download_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	GError           **error
){
	g_return_val_if_fail(g_task_is_valid(result, generator), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* Cancel                                                                 */
/* ---------------------------------------------------------------------- */

static void pl_cancel_next(GTask *task);

static void
pl_cancel_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);

	/* A direction already cleaned up is as cancelled as it will get. */
	if (status != 404 && !SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		GError *error = pl_status_error(self, op->what, status, body, op->message, TRUE);

		if (status == 400 || status == 409)
		{
			g_autofree gchar *message = g_strdup_printf("%s -- the job had already finished",
			                                            error->message);

			g_clear_error(&error);
			error = g_error_new_literal(AI_ERROR, AI_ERROR_INVALID_REQUEST, message);
		}
		g_task_return_error(task, error);
		return;
	}
	op->index++;
	pl_cancel_next(task);
}

static void
pl_cancel_next(GTask *task)
{
	PlOp *op = g_task_get_task_data(task);
	g_autofree gchar *path = NULL;

	if (op->id.job_ids[op->index] == NULL)
	{
		g_task_return_boolean(task, TRUE);
		return;
	}
	path = g_strdup_printf("background-jobs/%s", op->id.job_ids[op->index]);
	pl_send_api(task, "DELETE", path, NULL, NULL, pl_cancel_reply);
}

/*
 * DELETE /background-jobs/{id} for every job of the request.  A job that
 * finished at submit has nothing to cancel.
 */
static void
pl_cancel_async(
	AiMediaGenerator    *generator,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiPixellabClient *self = AI_PIXELLAB_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	PlOp *op;

	task = pl_task_new(self, pl_cancel_async, "cancel", cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	if (job_id != NULL && g_str_has_prefix(job_id, PL_SYNC_PREFIX))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "PixelLab job %s finished when it was submitted; there is "
		                        "nothing to cancel", job_id);
		return;
	}
	if (!pl_parse_job_id(job_id, &op->id))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a PixelLab job id", job_id);
		return;
	}
	pl_cancel_next(task);
}

static gboolean
pl_cancel_finish(
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
	iface->submit_async    = pl_submit_async;
	iface->submit_finish   = pl_submit_finish;
	iface->poll_async      = pl_poll_async;
	iface->poll_finish     = pl_poll_finish;
	iface->download_async  = pl_download_async;
	iface->download_finish = pl_download_finish;
	/* No estimate: PixelLab prices only its pro-flash endpoints in advance. */
	iface->cancel_async    = pl_cancel_async;
	iface->cancel_finish   = pl_cancel_finish;
}

/* ---------------------------------------------------------------------- */
/* Balance                                                                */
/* ---------------------------------------------------------------------- */

typedef struct
{
	gint64   micro_usd;
	gdouble  generations;
	gdouble  generations_total;
	gchar   *status;
} PlBalance;

static void
pl_balance_free(gpointer data)
{
	PlBalance *balance = data;

	g_free(balance->status);
	g_free(balance);
}

static void
pl_balance_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiPixellabClient *self = g_task_get_source_object(task);
	PlOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	GError *error = NULL;
	JsonObject *credits;
	JsonObject *subscription;
	PlBalance *balance;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, pl_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}
	object = pl_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	balance = g_new0(PlBalance, 1);
	balance->micro_usd = AI_MEDIA_COST_UNKNOWN;
	balance->generations = -1;
	balance->generations_total = -1;

	credits = pl_object(object, "credits");
	subscription = pl_object(object, "subscription");
	if (credits == NULL ||
	    !ai_media_shared_usd_node_to_micro(json_object_get_member(credits, "usd"),
	                                       &balance->micro_usd))
	{
		pl_balance_free(balance);
		g_task_return_error(task,
		                    pl_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("PixelLab balance returned no 'credits.usd'")));
		return;
	}
	if (subscription != NULL)
	{
		pl_number(subscription, "generations", &balance->generations);
		pl_number(subscription, "total", &balance->generations_total);
		balance->status = g_strdup(pl_string(subscription, "status"));
	}
	g_task_return_pointer(task, balance, pl_balance_free);
}

/**
 * ai_pixellab_client_balance_async:
 * @self: an #AiPixellabClient
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the balance
 * @user_data: data for @callback
 *
 * Asks what the account has left (`GET /balance`): prepaid credit in
 * dollars, and the subscription's generations this billing period.
 */
void
ai_pixellab_client_balance_async(
	AiPixellabClient    *self,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;

	g_return_if_fail(AI_IS_PIXELLAB_CLIENT(self));

	task = pl_task_new(self, ai_pixellab_client_balance_async, "balance",
	                   cancellable, callback, user_data);
	pl_send_api(task, "GET", "balance", NULL, NULL, pl_balance_reply);
}

/**
 * ai_pixellab_client_balance_finish:
 * @self: an #AiPixellabClient
 * @result: the #GAsyncResult
 * @credits_micro_usd: (out) (optional): the prepaid credit, in micro-USD
 * @generations: (out) (optional): subscription generations left this
 *   period, or -1 when the account has no subscription block
 * @generations_total: (out) (optional): the period's allowance, or -1
 * @subscription_status: (out) (optional) (transfer full) (nullable): the
 *   subscription's status ("active", "trial", "expired", "none", ...)
 * @error: return location for a #GError
 *
 * Returns: %TRUE on success
 */
gboolean
ai_pixellab_client_balance_finish(
	AiPixellabClient  *self,
	GAsyncResult      *result,
	gint64            *credits_micro_usd,
	gdouble           *generations,
	gdouble           *generations_total,
	gchar            **subscription_status,
	GError           **error
){
	PlBalance *balance;

	g_return_val_if_fail(AI_IS_PIXELLAB_CLIENT(self), FALSE);
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

	balance = g_task_propagate_pointer(G_TASK(result), error);
	if (balance == NULL)
	{
		return FALSE;
	}
	if (credits_micro_usd != NULL)
	{
		*credits_micro_usd = balance->micro_usd;
	}
	if (generations != NULL)
	{
		*generations = balance->generations;
	}
	if (generations_total != NULL)
	{
		*generations_total = balance->generations_total;
	}
	if (subscription_status != NULL)
	{
		*subscription_status = g_steal_pointer(&balance->status);
	}
	pl_balance_free(balance);
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Input images                                                           */
/* ---------------------------------------------------------------------- */

/**
 * ai_pixellab_image_node_new:
 * @data: a PNG, JPEG or WebP image
 * @error: return location for a #GError
 *
 * Wraps an image in the object every PixelLab endpoint takes for an input
 * image -- `{"type": "base64", "base64": "...", "format": "png"}` -- for
 * ai_media_request_set_param_json() with `init_image`, `reference_image`,
 * `from_image`, `inpainting_image`, `mask_image` and the rest.  The
 * format is read from the bytes, not trusted from a filename.
 *
 * Returns: (transfer full) (nullable): the JSON object, or %NULL with
 *   @error set
 */
JsonNode *
ai_pixellab_image_node_new(
	GBytes  *data,
	GError **error
){
	g_autoptr(JsonObject) image = NULL;
	g_autofree gchar *payload = NULL;
	gconstpointer bytes;
	gsize length = 0;
	const gchar *mime;
	JsonNode *node;

	g_return_val_if_fail(data != NULL, NULL);

	bytes = g_bytes_get_data(data, &length);
	mime = ai_media_shared_sniff_image(bytes, length);
	if (mime == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "PixelLab takes PNG, JPEG or WebP input images; these bytes are "
		                    "none of them");
		return NULL;
	}

	payload = g_base64_encode(bytes, length);
	image = json_object_new();
	json_object_set_string_member(image, "type", "base64");
	json_object_set_string_member(image, "base64", payload);
	json_object_set_string_member(image, "format", mime + strlen("image/"));

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, image);
	return node;
}

/* ---------------------------------------------------------------------- */
/* GObject                                                                */
/* ---------------------------------------------------------------------- */

static void
ai_pixellab_client_finalize(GObject *object)
{
	AiPixellabClient *self = AI_PIXELLAB_CLIENT(object);

	g_clear_object(&self->session);
	g_clear_pointer(&self->session_context, g_main_context_unref);
	g_clear_pointer(&self->sync_jobs, g_hash_table_unref);
	g_clear_pointer(&self->costs, g_hash_table_unref);
	g_mutex_clear(&self->lock);
	g_free(self->base_url);
	ai_media_shared_wipe_free(self->api_key);

	G_OBJECT_CLASS(ai_pixellab_client_parent_class)->finalize(object);
}

static void
ai_pixellab_client_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiPixellabClient *self = AI_PIXELLAB_CLIENT(object);

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
ai_pixellab_client_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiPixellabClient *self = AI_PIXELLAB_CLIENT(object);

	switch (prop_id)
	{
		case PROP_BASE_URL:
			ai_pixellab_client_set_base_url(self, g_value_get_string(value));
			break;
		case PROP_TIMEOUT:
			ai_pixellab_client_set_timeout(self, g_value_get_uint(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
ai_pixellab_client_class_init(AiPixellabClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize     = ai_pixellab_client_finalize;
	object_class->get_property = ai_pixellab_client_get_property;
	object_class->set_property = ai_pixellab_client_set_property;

	/**
	 * AiPixellabClient:base-url:
	 *
	 * The v2 API's base URL.  %NULL restores %AI_PIXELLAB_DEFAULT_BASE_URL.
	 * The token is sent to this origin and no other.
	 */
	properties[PROP_BASE_URL] =
		g_param_spec_string("base-url", NULL, NULL, AI_PIXELLAB_DEFAULT_BASE_URL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiPixellabClient:timeout:
	 *
	 * Seconds any one HTTP request may take; 0 for no limit.  A
	 * synchronous endpoint generates while the request is open, so this
	 * also bounds those generations.
	 */
	properties[PROP_TIMEOUT] =
		g_param_spec_uint("timeout", NULL, NULL, 0, G_MAXUINT, PL_DEFAULT_TIMEOUT,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                  G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_pixellab_client_init(AiPixellabClient *self)
{
	self->base_url = g_strdup(AI_PIXELLAB_DEFAULT_BASE_URL);
	self->timeout = PL_DEFAULT_TIMEOUT;
	g_mutex_init(&self->lock);
	self->sync_jobs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	self->costs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/**
 * ai_pixellab_client_new:
 *
 * Creates a client with no token.  Nothing is read from the environment:
 * call ai_pixellab_client_set_api_key() before any request.
 *
 * Returns: (transfer full): a new #AiPixellabClient
 */
AiPixellabClient *
ai_pixellab_client_new(void)
{
	g_autoptr(AiPixellabClient) self = g_object_new(AI_TYPE_PIXELLAB_CLIENT, NULL);

	return (AiPixellabClient *)g_steal_pointer(&self);
}

/**
 * ai_pixellab_client_new_with_api_key:
 * @api_key: a PixelLab API token
 *
 * Returns: (transfer full): a new #AiPixellabClient holding @api_key
 */
AiPixellabClient *
ai_pixellab_client_new_with_api_key(const gchar *api_key)
{
	g_autoptr(AiPixellabClient) self = ai_pixellab_client_new();

	ai_pixellab_client_set_api_key(self, api_key);
	return (AiPixellabClient *)g_steal_pointer(&self);
}

/**
 * ai_pixellab_client_set_api_key:
 * @self: an #AiPixellabClient
 * @api_key: (nullable): the API token, or %NULL to clear it
 *
 * Sets the token sent as `Authorization: Bearer <token>`.  Deliberately
 * not a property, and there is no getter.  The old token is overwritten
 * in memory before it is freed; one containing a line break is refused.
 */
void
ai_pixellab_client_set_api_key(
	AiPixellabClient *self,
	const gchar      *api_key
){
	g_return_if_fail(AI_IS_PIXELLAB_CLIENT(self));

	ai_media_shared_wipe_free(self->api_key);
	self->api_key = NULL;

	if (api_key == NULL || api_key[0] == '\0')
	{
		return;
	}
	if (!ai_media_shared_header_value_is_valid(api_key))
	{
		g_message("ai-glib: refusing a PixelLab API token containing a line break");
		return;
	}
	self->api_key = g_strdup(api_key);
}

/**
 * ai_pixellab_client_has_api_key:
 * @self: an #AiPixellabClient
 *
 * Returns: whether a token is set
 */
gboolean
ai_pixellab_client_has_api_key(AiPixellabClient *self)
{
	g_return_val_if_fail(AI_IS_PIXELLAB_CLIENT(self), FALSE);

	return self->api_key != NULL;
}

/**
 * ai_pixellab_client_get_base_url:
 * @self: an #AiPixellabClient
 *
 * Returns: (transfer none): the API base URL
 */
const gchar *
ai_pixellab_client_get_base_url(AiPixellabClient *self)
{
	g_return_val_if_fail(AI_IS_PIXELLAB_CLIENT(self), NULL);

	return self->base_url;
}

/**
 * ai_pixellab_client_set_base_url:
 * @self: an #AiPixellabClient
 * @base_url: (nullable): the API base URL, or %NULL for the default
 *
 * Points the client at another base URL -- a test server, a proxy.
 */
void
ai_pixellab_client_set_base_url(
	AiPixellabClient *self,
	const gchar      *base_url
){
	g_return_if_fail(AI_IS_PIXELLAB_CLIENT(self));

	if (base_url == NULL || base_url[0] == '\0')
	{
		base_url = AI_PIXELLAB_DEFAULT_BASE_URL;
	}
	if (g_set_str(&self->base_url, base_url))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BASE_URL]);
	}
}

/**
 * ai_pixellab_client_get_timeout:
 * @self: an #AiPixellabClient
 *
 * Returns: the per-request timeout in seconds, 0 for none
 */
guint
ai_pixellab_client_get_timeout(AiPixellabClient *self)
{
	g_return_val_if_fail(AI_IS_PIXELLAB_CLIENT(self), 0);

	return self->timeout;
}

/**
 * ai_pixellab_client_set_timeout:
 * @self: an #AiPixellabClient
 * @seconds: the per-request timeout, 0 for none
 *
 * Sets the per-request timeout for requests started afterwards.
 */
void
ai_pixellab_client_set_timeout(
	AiPixellabClient *self,
	guint             seconds
){
	g_return_if_fail(AI_IS_PIXELLAB_CLIENT(self));

	if (self->timeout == seconds)
	{
		return;
	}
	self->timeout = seconds;
	g_clear_object(&self->session);
	g_clear_pointer(&self->session_context, g_main_context_unref);
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_TIMEOUT]);
}
