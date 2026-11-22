/*
 * ai-higgsfield-client.c - Higgsfield image and video generation API
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Higgsfield is a hosted, closed, prepaid service.  Its API is a queue:
 * POST a model's JSON body to the model's path, get a request id back, poll
 * /requests/<id>/status until it is terminal, then fetch the result URLs
 * from its CDN.  docs/providers/higgsfield.org records every fact this file
 * relies on, with the URL it was checked against.
 *
 * Three rules shape the code:
 *
 * - The credential goes to the API origin and nowhere else.  Download and
 *   upload URLs come from the service and point at storage; they get no
 *   Authorization header, and API requests do not follow redirects, so a
 *   3xx cannot carry the key to another host.  Status and cancel URLs are
 *   built from the configured base URL and a validated id rather than
 *   taken from the server's `status_url`, for the same reason.
 * - The credential is never in an error string.  The service is unlikely
 *   to echo it, but "unlikely" is not a property anybody can test, so every
 *   message that leaves this file passes through hf_redact().
 * - Nothing is read from the environment.  A host decides where a key comes
 *   from (a secrets file, a keyring) and hands it over.
 */

#include "config.h"

#include <string.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "providers/ai-higgsfield-client.h"
#include "core/ai-error.h"

/* What a 4xx/5xx body is quoted to, at most, when it is not JSON. */
#define HF_ERROR_EXCERPT (400)

/* The shortest Hf-Expires-After the API accepts, in seconds. */
#define HF_MIN_EXPIRES_AFTER (3600)

/* Default whole-request timeout, in seconds. */
#define HF_DEFAULT_TIMEOUT (60)

/* What a request id may be: the API issues UUIDs. */
#define HF_MAX_JOB_ID (64)

struct _AiHiggsfieldClient
{
	GObject parent_instance;

	gchar        *base_url;
	gchar        *key_id;
	gchar        *key_secret;
	guint         timeout;

	/*
	 * A SoupSession belongs to the context that was thread-default when it
	 * was made, and a request sent from another context is answered into a
	 * loop nobody runs.  So the cached session is kept only for its own
	 * context; an operation started anywhere else gets a session of its own.
	 */
	SoupSession  *session;
	GMainContext *session_context;
};

static void media_generator_iface_init(AiMediaGeneratorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(AiHiggsfieldClient, ai_higgsfield_client, G_TYPE_OBJECT,
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
/* Secrets                                                                */
/* ---------------------------------------------------------------------- */

/* Overwrites a string before freeing it, so a freed secret is not left in the heap. */
static void
hf_wipe_free(gchar *text)
{
	if (text != NULL)
	{
		memset(text, 0, strlen(text));
		g_free(text);
	}
}

/*
 * Replaces every occurrence of @needle in @text.  Values shorter than four
 * bytes are left alone: they cannot be a real credential, and redacting
 * every "a" would make messages unreadable without protecting anything.
 */
static gchar *
hf_replace_all(
	gchar       *text,
	const gchar *needle
){
	GString *out;
	const gchar *cursor;
	const gchar *hit;
	gsize needle_len;

	if (text == NULL || needle == NULL || strlen(needle) < 4)
	{
		return text;
	}

	needle_len = strlen(needle);
	if (strstr(text, needle) == NULL)
	{
		return text;
	}

	out = g_string_new(NULL);
	cursor = text;
	while ((hit = strstr(cursor, needle)) != NULL)
	{
		g_string_append_len(out, cursor, hit - cursor);
		g_string_append(out, "[REDACTED]");
		cursor = hit + needle_len;
	}
	g_string_append(out, cursor);
	hf_wipe_free(text);

	return g_string_free(out, FALSE);
}

/* Takes @text and returns it with the configured key id and secret removed. */
static gchar *
hf_redact(
	AiHiggsfieldClient *self,
	gchar              *text
){
	text = hf_replace_all(text, self->key_secret);
	text = hf_replace_all(text, self->key_id);
	return text;
}

/* Sets @error from a message, redacted on the way out. */
static GError *
hf_error_new(
	AiHiggsfieldClient *self,
	gint                code,
	gchar              *message
){
	GError *error;

	message = hf_redact(self, message);
	error = g_error_new_literal(AI_ERROR, code, message);
	g_free(message);

	return error;
}

/* ---------------------------------------------------------------------- */
/* Validation                                                             */
/* ---------------------------------------------------------------------- */

/*
 * A model id is the endpoint path: `vendor/model/.../workflow`.  It is
 * spliced into a URL, so anything that could leave the path -- a `?`, a
 * `#`, `..`, an empty segment -- is refused rather than escaped.
 */
static gboolean
hf_model_is_valid(const gchar *model)
{
	const gchar *p;

	if (model == NULL || model[0] == '\0' || model[0] == '/' ||
	    strstr(model, "..") != NULL || strstr(model, "//") != NULL)
	{
		return FALSE;
	}
	for (p = model; *p != '\0'; p++)
	{
		if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_' && *p != '.' && *p != '/')
		{
			return FALSE;
		}
	}
	return model[strlen(model) - 1] != '/';
}

/* Request ids are UUIDs; anything else would be spliced into a path. */
static gboolean
hf_job_id_is_valid(const gchar *job_id)
{
	const gchar *p;

	if (job_id == NULL || job_id[0] == '\0' || strlen(job_id) > HF_MAX_JOB_ID)
	{
		return FALSE;
	}
	for (p = job_id; *p != '\0'; p++)
	{
		if (!g_ascii_isalnum(*p) && *p != '-')
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* 1 to 255 visible ASCII characters, the API's rule for Idempotency-Key. */
static gboolean
hf_idempotency_key_is_valid(const gchar *key)
{
	const gchar *p;

	if (key == NULL || key[0] == '\0' || strlen(key) > 255)
	{
		return FALSE;
	}
	for (p = key; *p != '\0'; p++)
	{
		if (*p < 0x21 || *p > 0x7e)
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* A header value we were handed: no line breaks, nothing empty. */
static gboolean
hf_header_value_is_valid(const gchar *value)
{
	return value != NULL && value[0] != '\0' && strpbrk(value, "\r\n") == NULL;
}

/* Parses an http(s) URL, refusing file:, data: and the rest. */
static GUri *
hf_parse_http_uri(const gchar *url)
{
	GUri *uri;
	const gchar *scheme;

	if (url == NULL)
	{
		return NULL;
	}
	uri = g_uri_parse(url, SOUP_HTTP_URI_FLAGS, NULL);
	if (uri == NULL)
	{
		return NULL;
	}
	scheme = g_uri_get_scheme(uri);
	if (g_uri_get_host(uri) == NULL ||
	    (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0))
	{
		g_uri_unref(uri);
		return NULL;
	}
	return uri;
}

/* ---------------------------------------------------------------------- */
/* Money                                                                  */
/* ---------------------------------------------------------------------- */

/*
 * Converts the estimate's `usd` ("0.094") to micro-USD without going
 * through a double, so 0.1 is 100000 and not 99999.  Digits past the sixth
 * decimal round half up.  Negative and malformed amounts are refused.
 */
static gboolean
hf_parse_usd(
	const gchar *text,
	gint64      *micro_usd
){
	const gchar *p = text;
	gint64 whole = 0;
	gint64 fraction = 0;
	guint fraction_digits = 0;
	gboolean any_digit = FALSE;

	if (text == NULL)
	{
		return FALSE;
	}
	while (g_ascii_isspace(*p))
	{
		p++;
	}
	if (*p == '+')
	{
		p++;
	}
	for (; g_ascii_isdigit(*p); p++)
	{
		any_digit = TRUE;
		if (whole > (G_MAXINT64 / 1000000 - 9) / 10)
		{
			return FALSE;
		}
		whole = whole * 10 + (*p - '0');
	}
	if (*p == '.')
	{
		p++;
		for (; g_ascii_isdigit(*p); p++)
		{
			any_digit = TRUE;
			if (fraction_digits < 6)
			{
				fraction = fraction * 10 + (*p - '0');
				fraction_digits++;
			}
			else if (fraction_digits == 6)
			{
				/* The seventh digit decides the rounding; the rest are noise. */
				if (*p >= '5')
				{
					fraction++;
				}
				fraction_digits++;
			}
		}
	}
	while (g_ascii_isspace(*p))
	{
		p++;
	}
	if (!any_digit || *p != '\0')
	{
		return FALSE;
	}

	while (fraction_digits < 6)
	{
		fraction *= 10;
		fraction_digits++;
	}
	*micro_usd = whole * 1000000 + fraction;
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Errors from the wire                                                   */
/* ---------------------------------------------------------------------- */

/*
 * The API's error envelope is FastAPI's: {"detail": "..."}, or for a
 * validation failure {"detail": [{"loc": [...], "msg": "..."}]}.  The list
 * is flattened to "field: message; field: message" because the field name
 * is the useful part when a model rejects a parameter.
 */
static gchar *
hf_error_detail(GBytes *body)
{
	gconstpointer data;
	gsize length = 0;
	g_autoptr(JsonParser) parser = NULL;
	JsonNode *root;
	JsonNode *detail;

	if (body == NULL)
	{
		return NULL;
	}
	data = g_bytes_get_data(body, &length);
	if (length == 0)
	{
		return NULL;
	}

	parser = json_parser_new();
	if (json_parser_load_from_data(parser, data, (gssize)length, NULL) &&
	    (root = json_parser_get_root(parser)) != NULL &&
	    JSON_NODE_HOLDS_OBJECT(root) &&
	    (detail = json_object_get_member(json_node_get_object(root), "detail")) != NULL)
	{
		if (JSON_NODE_HOLDS_VALUE(detail) &&
		    json_node_get_value_type(detail) == G_TYPE_STRING)
		{
			return g_strdup(json_node_get_string(detail));
		}
		if (JSON_NODE_HOLDS_ARRAY(detail))
		{
			JsonArray *items = json_node_get_array(detail);
			GString *out = g_string_new(NULL);
			guint i;

			for (i = 0; i < json_array_get_length(items); i++)
			{
				JsonNode *item = json_array_get_element(items, i);
				JsonObject *object;
				const gchar *msg;

				if (!JSON_NODE_HOLDS_OBJECT(item))
				{
					continue;
				}
				object = json_node_get_object(item);
				msg = json_object_get_string_member_with_default(object, "msg", NULL);
				if (msg == NULL)
				{
					continue;
				}
				if (out->len > 0)
				{
					g_string_append(out, "; ");
				}
				if (json_object_has_member(object, "loc") &&
				    JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "loc")))
				{
					JsonArray *loc = json_object_get_array_member(object, "loc");
					guint n = json_array_get_length(loc);

					if (n > 0)
					{
						JsonNode *last = json_array_get_element(loc, n - 1);

						if (JSON_NODE_HOLDS_VALUE(last) &&
						    json_node_get_value_type(last) == G_TYPE_STRING)
						{
							g_string_append_printf(out, "%s: ", json_node_get_string(last));
						}
					}
				}
				g_string_append(out, msg);
			}
			if (out->len > 0)
			{
				return g_string_free(out, FALSE);
			}
			g_string_free(out, TRUE);
		}
		return json_to_string(detail, FALSE);
	}

	/* Not the envelope: a bounded, valid-UTF-8 excerpt beats nothing. */
	{
		g_autofree gchar *raw = g_strndup(data, MIN(length, (gsize)HF_ERROR_EXCERPT));
		gchar *valid = g_utf8_make_valid(raw, -1);

		return g_strstrip(valid);
	}
}

/*
 * Maps a non-2xx status onto an AiError.  @job_scoped says whether a 404
 * means "no such request" (status, cancel) or "no such model" (submit,
 * estimate) -- the API uses the one code for both.
 */
static GError *
hf_status_error(
	AiHiggsfieldClient *self,
	const gchar        *what,
	guint               status,
	GBytes             *body,
	SoupMessage        *message,
	gboolean            job_scoped
){
	g_autofree gchar *detail = hf_error_detail(body);
	const gchar *retry_after = NULL;
	gint code;

	if (SOUP_STATUS_IS_REDIRECTION(status))
	{
		return hf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                    g_strdup_printf("Higgsfield %s answered with a redirect (HTTP %u), "
		                                    "which is not followed so the credential stays on "
		                                    "the API origin", what, status));
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
			break;
		case 403:
			/* Insufficient balance, or a disabled or expired key. */
			code = AI_ERROR_INSUFFICIENT_QUOTA;
			break;
		case 404:
			code = job_scoped ? AI_ERROR_INVALID_REQUEST : AI_ERROR_MODEL_NOT_FOUND;
			break;
		case 423:
		case 503:
			code = AI_ERROR_SERVICE_UNAVAILABLE;
			break;
		case 429:
			code = AI_ERROR_RATE_LIMITED;
			if (message != NULL)
			{
				retry_after = soup_message_headers_get_one(
					soup_message_get_response_headers(message), "Retry-After");
			}
			break;
		default:
			code = (status >= 500) ? AI_ERROR_SERVER_ERROR : AI_ERROR_UNKNOWN;
			break;
	}

	return hf_error_new(self, code,
	                    g_strdup_printf("Higgsfield %s failed (HTTP %u)%s%s%s%s%s",
	                                    what, status,
	                                    (detail != NULL) ? ": " : "",
	                                    (detail != NULL) ? detail : "",
	                                    (retry_after != NULL) ? " (retry after " : "",
	                                    (retry_after != NULL) ? retry_after : "",
	                                    (retry_after != NULL) ? " s)" : ""));
}

/* Parses a 2xx body that must be a JSON object. */
static JsonObject *
hf_parse_object(
	AiHiggsfieldClient  *self,
	const gchar         *what,
	GBytes              *body,
	GError             **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) parse_error = NULL;
	gconstpointer data;
	gsize length = 0;
	JsonNode *root;

	data = (body != NULL) ? g_bytes_get_data(body, &length) : NULL;
	if (length == 0 ||
	    !json_parser_load_from_data(parser, data, (gssize)length, &parse_error) ||
	    (root = json_parser_get_root(parser)) == NULL ||
	    !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_propagate_error(error,
		                  hf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                               g_strdup_printf("Higgsfield %s returned a body that is "
		                                               "not a JSON object%s%s", what,
		                                               (parse_error != NULL) ? ": " : "",
		                                               (parse_error != NULL) ? parse_error->message : "")));
		return NULL;
	}

	return json_object_ref(json_node_get_object(root));
}

/* ---------------------------------------------------------------------- */
/* Sending                                                                */
/* ---------------------------------------------------------------------- */

typedef void (*HfReply)(GTask *task, guint status, GBytes *body);

typedef struct
{
	SoupSession *session;
	SoupMessage *message;
	HfReply      reply;
	const gchar *what;          /* static: names the call in messages */
	gchar       *job_id;

	/* upload */
	GBytes      *upload_data;
	gchar       *content_type;
	gchar       *public_url;
} HfOp;

static void
hf_op_free(gpointer data)
{
	HfOp *op = data;

	g_clear_object(&op->session);
	g_clear_object(&op->message);
	g_free(op->job_id);
	g_clear_pointer(&op->upload_data, g_bytes_unref);
	g_free(op->content_type);
	g_free(op->public_url);
	g_free(op);
}

/*
 * The session for an operation that started on @context.  Called with
 * @context thread-default -- every caller has just made its GTask, which
 * captured the same context -- so a new session binds to it.
 */
static SoupSession *
hf_session_for(
	AiHiggsfieldClient *self,
	GMainContext       *context
){
	SoupSession *session;

	if (self->session != NULL && self->session_context == context)
	{
		return g_object_ref(self->session);
	}

	session = soup_session_new_with_options("timeout", self->timeout,
	                                        "user-agent", "ai-glib/" AI_GLIB_PACKAGE_VERSION,
	                                        NULL);
	if (self->session == NULL)
	{
		self->session = g_object_ref(session);
		self->session_context = g_main_context_ref(context);
	}
	return session;
}

/* Starts an operation's task with its HfOp. */
static GTask *
hf_task_new(
	AiHiggsfieldClient  *self,
	gpointer             tag,
	const gchar         *what,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	HfOp *op;

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, tag);
	g_task_set_static_name(task, what);

	op = g_new0(HfOp, 1);
	op->what = what;
	op->session = hf_session_for(self, g_task_get_context(task));
	g_task_set_task_data(task, op, hf_op_free);

	return task;
}

/*
 * Builds an authenticated request to `<base-url>/<path>[?query]`.  The
 * Authorization header is the only place the credential is ever written,
 * and redirects are off for this message so it cannot follow one.
 */
static SoupMessage *
hf_api_message(
	AiHiggsfieldClient  *self,
	const gchar         *method,
	const gchar         *path,
	const gchar         *query,
	GBytes              *json_body,
	GError             **error
){
	g_autofree gchar *base = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GUri) uri = NULL;
	gchar *authorization;
	SoupMessage *message;
	SoupMessageHeaders *headers;

	if (self->key_id == NULL || self->key_secret == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_API_KEY,
		                    "No Higgsfield credentials: call "
		                    "ai_higgsfield_client_set_credentials() with a key id and "
		                    "secret from console.higgsfield.ai");
		return NULL;
	}

	base = g_strdup(self->base_url);
	while (base[0] != '\0' && base[strlen(base) - 1] == '/')
	{
		base[strlen(base) - 1] = '\0';
	}
	url = g_strdup_printf("%s/%s%s%s", base, path,
	                      (query != NULL) ? "?" : "",
	                      (query != NULL) ? query : "");

	uri = hf_parse_http_uri(url);
	if (uri == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		            "Higgsfield base URL '%s' is not an http or https URL", self->base_url);
		return NULL;
	}

	message = soup_message_new_from_uri(method, uri);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	authorization = g_strdup_printf("Key %s:%s", self->key_id, self->key_secret);
	soup_message_headers_replace(headers, "Authorization", authorization);
	hf_wipe_free(authorization);
	soup_message_headers_replace(headers, "Accept", "application/json");

	if (json_body != NULL)
	{
		soup_message_set_request_body_from_bytes(message, "application/json", json_body);
	}

	return message;
}

static void
hf_send_cb(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = G_TASK(user_data);
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);
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
		                    hf_error_new(self,
		                                 g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)
		                                     ? AI_ERROR_TIMEOUT : AI_ERROR_NETWORK_ERROR,
		                                 g_strdup_printf("Higgsfield %s failed: %s",
		                                                 op->what, error->message)));
		return;
	}

	op->reply(task, soup_message_get_status(op->message), body);
}

/* Sends @message (transfer full) for @task; @reply gets the answer. */
static void
hf_send(
	GTask       *task,
	SoupMessage *message,
	HfReply      reply
){
	HfOp *op = g_task_get_task_data(task);

	g_set_object(&op->message, message);
	g_object_unref(message);
	op->reply = reply;

	g_debug("higgsfield: %s %s", soup_message_get_method(message), op->what);
	soup_session_send_and_read_async(op->session, op->message, G_PRIORITY_DEFAULT,
	                                 g_task_get_cancellable(task),
	                                 hf_send_cb, g_object_ref(task));
}

/*
 * The body of a submit or estimate, and the checks both share.  Returns
 * NULL with @error set when the request cannot be sent.
 */
static GBytes *
hf_request_body(
	AiMediaRequest  *request,
	GError         **error
){
	const gchar *model = ai_media_request_get_model(request);
	gchar *json;

	if (!hf_model_is_valid(model))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "Higgsfield model '%s' is not an endpoint path such as '%s'",
		            (model != NULL) ? model : "(null)", AI_HIGGSFIELD_MODEL_SOUL_V2);
		return NULL;
	}

	json = ai_media_request_to_json(request);
	return g_bytes_new_take(json, strlen(json));
}

/* ---------------------------------------------------------------------- */
/* Submit                                                                 */
/* ---------------------------------------------------------------------- */

static void
hf_submit_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	GError *error = NULL;
	const gchar *request_id;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, hf_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}

	object = hf_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	request_id = json_object_get_string_member_with_default(object, "request_id", NULL);
	if (!hf_job_id_is_valid(request_id))
	{
		g_task_return_error(task,
		                    hf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("Higgsfield submit returned no usable request_id")));
		return;
	}

	g_task_return_pointer(task, g_strdup(request_id), g_free);
}

static void
hf_submit_async(
	AiMediaGenerator    *generator,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *query = NULL;
	const gchar *key;
	const gchar *webhook;
	guint expires_after;
	SoupMessage *message;
	SoupMessageHeaders *headers;

	task = hf_task_new(self, hf_submit_async, "submit", cancellable, callback, user_data);

	body = hf_request_body(request, &error);
	if (body == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	/* The key stays on the request, so resubmitting it reaches the same job. */
	key = ai_media_request_ensure_idempotency_key(request);
	if (!hf_idempotency_key_is_valid(key))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "Higgsfield idempotency keys are 1 to 255 visible ASCII characters");
		return;
	}

	expires_after = ai_media_request_get_expires_after(request);
	if (expires_after != 0 && expires_after < HF_MIN_EXPIRES_AFTER)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "Higgsfield keeps request data for at least %d seconds; "
		                        "expires-after %u would be refused",
		                        HF_MIN_EXPIRES_AFTER, expires_after);
		return;
	}

	webhook = ai_media_request_get_webhook_url(request);
	if (webhook != NULL)
	{
		g_autofree gchar *escaped = g_uri_escape_string(webhook, NULL, FALSE);

		query = g_strdup_printf("hf_webhook=%s", escaped);
	}

	path = g_strdup(ai_media_request_get_model(request));
	message = hf_api_message(self, "POST", path, query, body, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	headers = soup_message_get_request_headers(message);
	soup_message_headers_replace(headers, "Idempotency-Key", key);
	if (expires_after != 0)
	{
		g_autofree gchar *value = g_strdup_printf("%u", expires_after);

		soup_message_headers_replace(headers, "Hf-Expires-After", value);
	}

	hf_send(task, message, hf_submit_reply);
}

static gchar *
hf_submit_finish(
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

/* Adds `member.url`, for a member that is one output object or a list of them. */
static void
hf_collect_urls(
	AiMediaJob  *job,
	GHashTable  *seen,
	JsonObject  *object,
	const gchar *member
){
	JsonNode *node;
	GList *items = NULL;
	GList *iter;

	node = json_object_get_member(object, member);
	if (node == NULL)
	{
		return;
	}

	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		items = g_list_prepend(items, node);
	}
	else if (JSON_NODE_HOLDS_ARRAY(node))
	{
		items = json_array_get_elements(json_node_get_array(node));
	}

	for (iter = items; iter != NULL; iter = iter->next)
	{
		JsonNode *item = iter->data;
		const gchar *url;

		if (!JSON_NODE_HOLDS_OBJECT(item))
		{
			continue;
		}
		url = json_object_get_string_member_with_default(json_node_get_object(item), "url", NULL);
		/* `audio` and `audios` can name the same file; list it once. */
		if (url != NULL && g_hash_table_add(seen, g_strdup(url)))
		{
			ai_media_job_add_result_url(job, url);
		}
	}
	g_list_free(items);
}

/*
 * Maps the API's status word.  An unknown word is an error rather than a
 * guess: reading it as running would leave a waiter polling a finished
 * job until its deadline, and reading it as finished would lose a result.
 */
static gboolean
hf_state_from_status(
	const gchar     *status,
	AiMediaJobState *state
){
	if (g_strcmp0(status, "queued") == 0)
		*state = AI_MEDIA_JOB_STATE_QUEUED;
	else if (g_strcmp0(status, "in_progress") == 0)
		*state = AI_MEDIA_JOB_STATE_RUNNING;
	else if (g_strcmp0(status, "completed") == 0)
		*state = AI_MEDIA_JOB_STATE_DONE;
	else if (g_strcmp0(status, "failed") == 0 || g_strcmp0(status, "nsfw") == 0)
		*state = AI_MEDIA_JOB_STATE_FAILED;
	else if (g_strcmp0(status, "canceled") == 0 || g_strcmp0(status, "cancelled") == 0)
		*state = AI_MEDIA_JOB_STATE_CANCELLED;
	else
		return FALSE;

	return TRUE;
}

static void
hf_poll_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	static const gchar * const url_members[] = {
		"images", "video", "audio", "audios", "zip", "mov", "jsx", "fbx", "ply", NULL
	};
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autoptr(GHashTable) seen = NULL;
	GError *error = NULL;
	const gchar *status_word;
	const gchar *request_id;
	const gchar *message;
	AiMediaJobState state;
	JsonObject *outputs;
	guint i;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, hf_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}

	object = hf_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	status_word = json_object_get_string_member_with_default(object, "status", NULL);
	if (!hf_state_from_status(status_word, &state))
	{
		g_task_return_error(task,
		                    hf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup_printf("Higgsfield reported an unknown status "
		                                                 "'%s' for request %s",
		                                                 (status_word != NULL) ? status_word : "(none)",
		                                                 op->job_id)));
		return;
	}

	request_id = json_object_get_string_member_with_default(object, "request_id", op->job_id);
	job = ai_media_job_new(request_id, state);
	ai_media_job_set_provider_status(job, status_word);
	ai_media_job_set_expires_at(job,
	                            json_object_get_string_member_with_default(object, "expires_at", NULL));

	message = json_object_get_string_member_with_default(object, "error", NULL);
	if (message == NULL && g_strcmp0(status_word, "nsfw") == 0)
	{
		message = "Rejected by content moderation (nsfw)";
	}
	if (message != NULL)
	{
		g_autofree gchar *clean = hf_redact(self, g_strdup(message));

		ai_media_job_set_error_message(job, clean);
	}

	/*
	 * The status endpoint lists outputs at the top level; a webhook nests
	 * them in `payload`.  Read both, so a host can hand a webhook body to
	 * the same code path later without this changing.
	 */
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	outputs = object;
	for (i = 0; url_members[i] != NULL; i++)
	{
		hf_collect_urls(job, seen, outputs, url_members[i]);
	}
	if (json_object_has_member(object, "payload") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "payload")))
	{
		outputs = json_object_get_object_member(object, "payload");
		for (i = 0; url_members[i] != NULL; i++)
		{
			hf_collect_urls(job, seen, outputs, url_members[i]);
		}
	}

	/* The status response carries no price; see ai_media_job_get_cost_micro_usd(). */
	g_task_return_pointer(task, g_steal_pointer(&job), g_object_unref);
}

static void
hf_poll_async(
	AiMediaGenerator    *generator,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	HfOp *op;
	SoupMessage *message;

	task = hf_task_new(self, hf_poll_async, "status", cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	if (!hf_job_id_is_valid(job_id))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a Higgsfield request id", job_id);
		return;
	}
	op->job_id = g_strdup(job_id);

	path = g_strdup_printf("requests/%s/status", job_id);
	message = hf_api_message(self, "GET", path, NULL, NULL, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	hf_send(task, message, hf_poll_reply);
}

static AiMediaJob *
hf_poll_finish(
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
hf_download_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, hf_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}
	g_task_return_pointer(task, g_bytes_ref(body), (GDestroyNotify)g_bytes_unref);
}

static void
hf_download_async(
	AiMediaGenerator    *generator,
	const gchar         *url,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GUri) uri = NULL;

	task = hf_task_new(self, hf_download_async, "download", cancellable, callback, user_data);

	uri = hf_parse_http_uri(url);
	if (uri == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "Refusing to download '%s': not an http or https URL", url);
		return;
	}

	/* The CDN is not the API: no Authorization header, redirects allowed. */
	hf_send(task, soup_message_new_from_uri("GET", uri), hf_download_reply);
}

static GBytes *
hf_download_finish(
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

typedef struct
{
	gint64 micro_usd;
} HfEstimate;

static void
hf_estimate_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	GError *error = NULL;
	JsonNode *usd;
	HfEstimate *estimate;
	gint64 micro_usd = 0;
	gboolean parsed = FALSE;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, hf_status_error(self, op->what, status, body,
		                                          op->message, FALSE));
		return;
	}

	object = hf_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	/* Documented as a decimal string ("0.094"); accept a JSON number too. */
	usd = json_object_get_member(object, "usd");
	if (usd != NULL && JSON_NODE_HOLDS_VALUE(usd))
	{
		if (json_node_get_value_type(usd) == G_TYPE_STRING)
		{
			parsed = hf_parse_usd(json_node_get_string(usd), &micro_usd);
		}
		else
		{
			gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

			g_ascii_formatd(buffer, sizeof buffer, "%.9f", json_node_get_double(usd));
			parsed = hf_parse_usd(buffer, &micro_usd);
		}
	}
	if (!parsed)
	{
		g_task_return_error(task,
		                    hf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("Higgsfield estimate returned no usable 'usd' amount")));
		return;
	}

	estimate = g_new0(HfEstimate, 1);
	estimate->micro_usd = micro_usd;
	g_task_return_pointer(task, estimate, g_free);
}

static void
hf_estimate_async(
	AiMediaGenerator    *generator,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	SoupMessage *message;

	task = hf_task_new(self, hf_estimate_async, "estimate", cancellable, callback, user_data);

	body = hf_request_body(request, &error);
	if (body == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	path = g_strdup_printf("estimate/%s", ai_media_request_get_model(request));
	message = hf_api_message(self, "POST", path, NULL, body, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	hf_send(task, message, hf_estimate_reply);
}

static gboolean
hf_estimate_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	gint64            *micro_usd,
	GError           **error
){
	g_autofree HfEstimate *estimate = NULL;

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
hf_cancel_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, hf_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}
	g_task_return_boolean(task, TRUE);
}

static void
hf_cancel_async(
	AiMediaGenerator    *generator,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	SoupMessage *message;

	task = hf_task_new(self, hf_cancel_async, "cancel", cancellable, callback, user_data);

	if (!hf_job_id_is_valid(job_id))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a Higgsfield request id", job_id);
		return;
	}

	path = g_strdup_printf("requests/%s/cancel", job_id);
	message = hf_api_message(self, "POST", path, NULL, NULL, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	hf_send(task, message, hf_cancel_reply);
}

static gboolean
hf_cancel_finish(
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
	iface->submit_async    = hf_submit_async;
	iface->submit_finish   = hf_submit_finish;
	iface->poll_async      = hf_poll_async;
	iface->poll_finish     = hf_poll_finish;
	iface->download_async  = hf_download_async;
	iface->download_finish = hf_download_finish;
	iface->estimate_async  = hf_estimate_async;
	iface->estimate_finish = hf_estimate_finish;
	iface->cancel_async    = hf_cancel_async;
	iface->cancel_finish   = hf_cancel_finish;
}

/* ---------------------------------------------------------------------- */
/* Upload                                                                 */
/* ---------------------------------------------------------------------- */

static void
hf_upload_put_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, hf_status_error(self, "upload", status, body,
		                                          op->message, TRUE));
		return;
	}
	g_task_return_pointer(task, g_steal_pointer(&op->public_url), g_free);
}

/*
 * The upload URL is presigned storage.  It gets exactly the headers the
 * API listed for it and never the Higgsfield credential.
 */
static void
hf_upload_url_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiHiggsfieldClient *self = g_task_get_source_object(task);
	HfOp *op = g_task_get_task_data(task);
	g_autoptr(JsonObject) object = NULL;
	g_autoptr(GUri) upload_uri = NULL;
	GError *error = NULL;
	const gchar *public_url;
	const gchar *upload_url;
	SoupMessage *message;
	SoupMessageHeaders *headers;
	g_autofree gchar *content_type = NULL;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, hf_status_error(self, op->what, status, body,
		                                          op->message, TRUE));
		return;
	}

	object = hf_parse_object(self, op->what, body, &error);
	if (object == NULL)
	{
		g_task_return_error(task, error);
		return;
	}

	public_url = json_object_get_string_member_with_default(object, "public_url", NULL);
	upload_url = json_object_get_string_member_with_default(object, "upload_url", NULL);
	upload_uri = hf_parse_http_uri(upload_url);
	if (public_url == NULL || upload_uri == NULL)
	{
		g_task_return_error(task,
		                    hf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("Higgsfield upload URL request returned no usable "
		                                          "public_url and upload_url")));
		return;
	}
	op->public_url = g_strdup(public_url);

	message = soup_message_new_from_uri("PUT", upload_uri);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);

	if (json_object_has_member(object, "upload_headers") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "upload_headers")))
	{
		JsonObject *extra = json_object_get_object_member(object, "upload_headers");
		GList *names = json_object_get_members(extra);
		GList *iter;

		for (iter = names; iter != NULL; iter = iter->next)
		{
			const gchar *name = iter->data;
			const gchar *value = json_object_get_string_member_with_default(extra, name, NULL);

			/* Storage has no business with our credential, whatever it asks. */
			if (!hf_header_value_is_valid(value) || !hf_header_value_is_valid(name) ||
			    g_ascii_strcasecmp(name, "Authorization") == 0)
			{
				continue;
			}
			/* The body setter owns Content-Type; it must match the URL request's. */
			if (g_ascii_strcasecmp(name, "Content-Type") == 0)
			{
				g_free(content_type);
				content_type = g_strdup(value);
				continue;
			}
			soup_message_headers_replace(headers, name, value);
		}
		g_list_free(names);
	}

	soup_message_set_request_body_from_bytes(message,
	                                         (content_type != NULL) ? content_type : op->content_type,
	                                         op->upload_data);
	op->what = "upload";
	hf_send(task, message, hf_upload_put_reply);
}

/**
 * ai_higgsfield_client_upload_async:
 * @self: an #AiHiggsfieldClient
 * @data: the file's bytes
 * @content_type: its MIME type: image/jpeg, image/png, image/webp,
 *   image/gif, audio/wav or video/mp4
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the public URL
 * @user_data: data for @callback
 *
 * Uploads an input file to Higgsfield's storage, for a model that needs an
 * image, video or audio URL and the file is not already public.  Asks the
 * API for a presigned upload URL, PUTs @data there (without the
 * credential), and returns the public URL to pass as a model parameter,
 * for instance with ai_media_request_set_image_url().
 */
void
ai_higgsfield_client_upload_async(
	AiHiggsfieldClient  *self,
	GBytes              *data,
	const gchar         *content_type,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GBytes) body = NULL;
	gchar *json;
	HfOp *op;
	SoupMessage *message;

	g_return_if_fail(AI_IS_HIGGSFIELD_CLIENT(self));
	g_return_if_fail(data != NULL);

	task = hf_task_new(self, ai_higgsfield_client_upload_async, "upload URL request",
	                   cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	if (!hf_header_value_is_valid(content_type))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "An upload needs a content type such as image/png");
		return;
	}
	op->upload_data = g_bytes_ref(data);
	op->content_type = g_strdup(content_type);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "content_type");
	json_builder_add_string_value(builder, content_type);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json = json_to_string(root, FALSE);
	body = g_bytes_new_take(json, strlen(json));

	message = hf_api_message(self, "POST", "files/generate-upload-url", NULL, body, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	hf_send(task, message, hf_upload_url_reply);
}

/**
 * ai_higgsfield_client_upload_finish:
 * @self: an #AiHiggsfieldClient
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the file's public URL, or %NULL
 */
gchar *
ai_higgsfield_client_upload_finish(
	AiHiggsfieldClient  *self,
	GAsyncResult        *result,
	GError             **error
){
	g_return_val_if_fail(AI_IS_HIGGSFIELD_CLIENT(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* GObject                                                                */
/* ---------------------------------------------------------------------- */

static void
ai_higgsfield_client_finalize(GObject *object)
{
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(object);

	g_clear_object(&self->session);
	g_clear_pointer(&self->session_context, g_main_context_unref);
	g_free(self->base_url);
	hf_wipe_free(self->key_id);
	hf_wipe_free(self->key_secret);

	G_OBJECT_CLASS(ai_higgsfield_client_parent_class)->finalize(object);
}

static void
ai_higgsfield_client_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(object);

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
ai_higgsfield_client_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiHiggsfieldClient *self = AI_HIGGSFIELD_CLIENT(object);

	switch (prop_id)
	{
		case PROP_BASE_URL:
			ai_higgsfield_client_set_base_url(self, g_value_get_string(value));
			break;
		case PROP_TIMEOUT:
			ai_higgsfield_client_set_timeout(self, g_value_get_uint(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
ai_higgsfield_client_class_init(AiHiggsfieldClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize     = ai_higgsfield_client_finalize;
	object_class->get_property = ai_higgsfield_client_get_property;
	object_class->set_property = ai_higgsfield_client_set_property;

	/**
	 * AiHiggsfieldClient:base-url:
	 *
	 * The API's base URL.  %NULL restores %AI_HIGGSFIELD_DEFAULT_BASE_URL.
	 * The credential is sent to this origin and no other.
	 */
	properties[PROP_BASE_URL] =
		g_param_spec_string("base-url", NULL, NULL, AI_HIGGSFIELD_DEFAULT_BASE_URL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiHiggsfieldClient:timeout:
	 *
	 * Seconds any one HTTP request may take, downloads included; 0 for no
	 * limit.  It bounds a request, not a job: use the timeout of
	 * ai_media_generator_wait_async() for that.
	 */
	properties[PROP_TIMEOUT] =
		g_param_spec_uint("timeout", NULL, NULL, 0, G_MAXUINT, HF_DEFAULT_TIMEOUT,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                  G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_higgsfield_client_init(AiHiggsfieldClient *self)
{
	self->base_url = g_strdup(AI_HIGGSFIELD_DEFAULT_BASE_URL);
	self->timeout = HF_DEFAULT_TIMEOUT;
}

/**
 * ai_higgsfield_client_new:
 *
 * Creates a client with no credentials.  Nothing is read from the
 * environment: call ai_higgsfield_client_set_credentials() before any
 * request, or every request fails with %AI_ERROR_INVALID_API_KEY.
 *
 * Returns: (transfer full): a new #AiHiggsfieldClient
 */
AiHiggsfieldClient *
ai_higgsfield_client_new(void)
{
	g_autoptr(AiHiggsfieldClient) self = g_object_new(AI_TYPE_HIGGSFIELD_CLIENT, NULL);

	return (AiHiggsfieldClient *)g_steal_pointer(&self);
}

/**
 * ai_higgsfield_client_new_with_credentials:
 * @key_id: the API key id
 * @key_secret: the API key secret
 *
 * Creates a client with credentials from console.higgsfield.ai.
 *
 * Returns: (transfer full): a new #AiHiggsfieldClient
 */
AiHiggsfieldClient *
ai_higgsfield_client_new_with_credentials(
	const gchar *key_id,
	const gchar *key_secret
){
	g_autoptr(AiHiggsfieldClient) self = ai_higgsfield_client_new();

	ai_higgsfield_client_set_credentials(self, key_id, key_secret);
	return (AiHiggsfieldClient *)g_steal_pointer(&self);
}

/**
 * ai_higgsfield_client_set_credentials:
 * @self: an #AiHiggsfieldClient
 * @key_id: (nullable): the API key id
 * @key_secret: (nullable): the API key secret
 *
 * Sets the credential sent as `Authorization: Key <id>:<secret>`.  Pass
 * %NULL for both to clear it.  Deliberately not a property: a property is
 * readable by anything holding the object, through g_object_get() and
 * every binding, and notify handlers see it change.  There is no getter.
 * The old values are overwritten in memory before they are freed.
 */
void
ai_higgsfield_client_set_credentials(
	AiHiggsfieldClient *self,
	const gchar        *key_id,
	const gchar        *key_secret
){
	g_return_if_fail(AI_IS_HIGGSFIELD_CLIENT(self));

	hf_wipe_free(self->key_id);
	hf_wipe_free(self->key_secret);
	self->key_id = NULL;
	self->key_secret = NULL;

	/*
	 * Both or neither, and nothing a header cannot carry: a ':' in the id
	 * would move the split, a line break would start a new header.
	 */
	if (key_id == NULL || key_secret == NULL || key_id[0] == '\0' || key_secret[0] == '\0')
	{
		return;
	}
	if (strpbrk(key_id, ":\r\n") != NULL || strpbrk(key_secret, "\r\n") != NULL)
	{
		g_message("ai-glib: refusing a Higgsfield credential containing a line break, "
		          "or a ':' in the key id");
		return;
	}

	self->key_id = g_strdup(key_id);
	self->key_secret = g_strdup(key_secret);
}

/**
 * ai_higgsfield_client_has_credentials:
 * @self: an #AiHiggsfieldClient
 *
 * Returns: whether a credential is set
 */
gboolean
ai_higgsfield_client_has_credentials(AiHiggsfieldClient *self)
{
	g_return_val_if_fail(AI_IS_HIGGSFIELD_CLIENT(self), FALSE);

	return self->key_id != NULL && self->key_secret != NULL;
}

/**
 * ai_higgsfield_client_get_base_url:
 * @self: an #AiHiggsfieldClient
 *
 * Returns: (transfer none): the API base URL
 */
const gchar *
ai_higgsfield_client_get_base_url(AiHiggsfieldClient *self)
{
	g_return_val_if_fail(AI_IS_HIGGSFIELD_CLIENT(self), NULL);

	return self->base_url;
}

/**
 * ai_higgsfield_client_set_base_url:
 * @self: an #AiHiggsfieldClient
 * @base_url: (nullable): the API base URL, or %NULL for the default
 *
 * Points the client at another origin -- a test server, a proxy.
 */
void
ai_higgsfield_client_set_base_url(
	AiHiggsfieldClient *self,
	const gchar        *base_url
){
	g_return_if_fail(AI_IS_HIGGSFIELD_CLIENT(self));

	if (base_url == NULL || base_url[0] == '\0')
	{
		base_url = AI_HIGGSFIELD_DEFAULT_BASE_URL;
	}
	if (g_set_str(&self->base_url, base_url))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BASE_URL]);
	}
}

/**
 * ai_higgsfield_client_get_timeout:
 * @self: an #AiHiggsfieldClient
 *
 * Returns: the per-request timeout in seconds, 0 for none
 */
guint
ai_higgsfield_client_get_timeout(AiHiggsfieldClient *self)
{
	g_return_val_if_fail(AI_IS_HIGGSFIELD_CLIENT(self), 0);

	return self->timeout;
}

/**
 * ai_higgsfield_client_set_timeout:
 * @self: an #AiHiggsfieldClient
 * @seconds: the per-request timeout, 0 for none
 *
 * Sets the per-request timeout.  It applies to requests started afterwards.
 */
void
ai_higgsfield_client_set_timeout(
	AiHiggsfieldClient *self,
	guint               seconds
){
	g_return_if_fail(AI_IS_HIGGSFIELD_CLIENT(self));

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
