/*
 * ai-comfyui-client.c - A self-hosted ComfyUI server as a media job service
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * ComfyUI (https://github.com/comfyanonymous/ComfyUI, GPL-3.0) runs a
 * node graph -- a "workflow" -- on the operator's own GPU.  Its HTTP API is
 * already a queue: POST /prompt with the graph returns a prompt_id, GET
 * /history/<id> answers {} until the graph has run and then its status and
 * outputs, GET /queue lists what is running and pending, and GET /view
 * serves an output file.  docs/providers/comfyui.org records each fact this
 * file relies on and where it was checked.
 *
 * Four rules shape the code:
 *
 * - The workflow is the operator's.  ComfyUI has no notion of "a model and
 *   a prompt"; a graph names its own checkpoint, sampler and save node.  The
 *   client fills placeholders in the graph's string inputs and changes
 *   nothing else.  It walks the parsed graph rather than replacing text in
 *   the JSON, so a prompt holding quotes or braces is one string value and
 *   can neither break the document nor add a node to it.
 * - Every request goes to the configured base URL.  A download URL must be
 *   under it -- scheme, host, port and path prefix -- because a result URL is
 *   a server's answer, and a client that fetched whatever a job named would
 *   fetch arbitrary hosts (with the token, when one is set).
 * - What accumulates is bounded.  A response is read in chunks and refused
 *   once it passes a ceiling, never read whole and measured afterwards.
 * - The token (for a ComfyUI behind an authenticating proxy; ComfyUI itself
 *   has no authentication) is never logged and is redacted from every error.
 */

#include "config.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "providers/ai-comfyui-client.h"
#include "core/ai-error.h"
#include "core/ai-http-redirect-private.h"
#include "core/ai-json-util.h"

/* Default whole-request I/O timeout, in seconds. */
#define CF_DEFAULT_TIMEOUT (60)

/* The most a JSON answer may hold; /queue carries every queued graph. */
#define CF_MAX_JSON_BYTES ((gsize)16 * 1024 * 1024)

/* The most one downloaded output may hold. */
#define CF_MAX_DOWNLOAD_BYTES ((gsize)512 * 1024 * 1024)

/* One read from a response stream. */
#define CF_READ_CHUNK (64 * 1024)

/* What a 4xx/5xx body is quoted to, at most, when it is not JSON. */
#define CF_ERROR_EXCERPT (400)

/* ComfyUI issues UUIDs; anything longer is not one of its ids. */
#define CF_MAX_JOB_ID (64)

/* A random seed stays below 2^53, so every JSON reader holds it exactly. */
#define CF_SEED_MASK ((G_GUINT64_CONSTANT(1) << 53) - 1)

struct _AiComfyuiClient
{
	GObject parent_instance;

	gchar        *base_url;      /* no trailing slash */
	gchar        *token;
	guint         timeout;
	gchar        *client_id;
	gchar        *workflow_json; /* validated, compact */

	/*
	 * A SoupSession belongs to the context that was thread-default when it
	 * was made; a request sent from another context is answered into a loop
	 * nobody runs.  The cached session serves its own context only.
	 */
	SoupSession  *session;
	GMainContext *session_context;
};

static void media_generator_iface_init(AiMediaGeneratorInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(AiComfyuiClient, ai_comfyui_client, G_TYPE_OBJECT,
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

/* Overwrites a string before freeing it, so a freed token is not left in the heap. */
static void
cf_wipe_free(gchar *text)
{
	if (text != NULL)
	{
		memset(text, 0, strlen(text));
		g_free(text);
	}
}

/*
 * Takes @text and returns it with the token replaced.  A proxy's 401 page
 * may well quote the header it refused; nothing here should repeat it.
 */
static gchar *
cf_redact(
	AiComfyuiClient *self,
	gchar           *text
){
	GString *out;
	const gchar *cursor;
	const gchar *hit;
	gsize needle_len;

	if (text == NULL || self->token == NULL || strlen(self->token) < 4 ||
	    strstr(text, self->token) == NULL)
	{
		return text;
	}

	needle_len = strlen(self->token);
	out = g_string_new(NULL);
	cursor = text;
	while ((hit = strstr(cursor, self->token)) != NULL)
	{
		g_string_append_len(out, cursor, hit - cursor);
		g_string_append(out, "[REDACTED]");
		cursor = hit + needle_len;
	}
	g_string_append(out, cursor);
	cf_wipe_free(text);

	return g_string_free(out, FALSE);
}

/* An AI_ERROR from @message (taken), redacted on the way out. */
static GError *
cf_error_new(
	AiComfyuiClient *self,
	gint             code,
	gchar           *message
){
	GError *error;

	message = cf_redact(self, message);
	error = g_error_new_literal(AI_ERROR, code, message);
	g_free(message);

	return error;
}

/* ---------------------------------------------------------------------- */
/* Validation                                                             */
/* ---------------------------------------------------------------------- */

/* Prompt ids are UUIDs; anything else would be spliced into a path. */
static gboolean
cf_job_id_is_valid(const gchar *job_id)
{
	const gchar *p;

	if (job_id == NULL || job_id[0] == '\0' || strlen(job_id) > CF_MAX_JOB_ID)
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

/*
 * A file name for the server's input folder: no directory part, no
 * traversal, nothing a multipart header or a LoadImage input cannot carry.
 * @allow_subfolder admits one "subfolder/name" form, which is how an upload
 * into a subfolder is named to LoadImage.
 */
static gboolean
cf_file_name_is_valid(
	const gchar *name,
	gboolean     allow_subfolder
){
	const gchar *p;

	if (name == NULL || name[0] == '\0' || name[0] == '/' || name[0] == '.' ||
	    strlen(name) > 255 || strstr(name, "..") != NULL || strchr(name, '\\') != NULL ||
	    !g_utf8_validate(name, -1, NULL))
	{
		return FALSE;
	}
	for (p = name; *p != '\0'; p++)
	{
		if ((guchar)*p < 0x20 || *p == 0x7f || *p == '"')
		{
			return FALSE;
		}
		if (*p == '/' && (!allow_subfolder || p[1] == '\0' || p[1] == '/'))
		{
			return FALSE;
		}
	}
	return TRUE;
}

/* Parses an http(s) URL with a host, refusing file:, data: and the rest. */
static GUri *
cf_parse_http_uri(const gchar *url)
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
	if (g_uri_get_host(uri) == NULL || g_uri_get_host(uri)[0] == '\0' ||
	    (g_ascii_strcasecmp(scheme, "https") != 0 && g_ascii_strcasecmp(scheme, "http") != 0))
	{
		g_uri_unref(uri);
		return NULL;
	}
	return uri;
}

/* The port a URI reaches, filling in the scheme's default. */
static gint
cf_effective_port(GUri *uri)
{
	gint port = g_uri_get_port(uri);

	if (port > 0)
	{
		return port;
	}
	return (g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0) ? 443 : 80;
}

/*
 * Whether @url is under the configured base: same scheme, host and port,
 * no userinfo, and a path inside the base's path.  Any dot segment, plain
 * or percent-encoded, is refused outright rather than normalised -- the
 * server or a proxy in front of it may decode it differently from GUri.
 */
static gboolean
cf_url_is_under_base(
	AiComfyuiClient *self,
	const gchar     *url
){
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GUri) base = NULL;
	g_autofree gchar *lower_path = NULL;
	g_autofree gchar *base_path = NULL;
	const gchar *path;
	gsize base_len;

	uri = cf_parse_http_uri(url);
	base = cf_parse_http_uri(self->base_url);
	if (uri == NULL || base == NULL || g_uri_get_userinfo(uri) != NULL)
	{
		return FALSE;
	}
	if (g_ascii_strcasecmp(g_uri_get_scheme(uri), g_uri_get_scheme(base)) != 0 ||
	    g_ascii_strcasecmp(g_uri_get_host(uri), g_uri_get_host(base)) != 0 ||
	    cf_effective_port(uri) != cf_effective_port(base))
	{
		return FALSE;
	}

	path = g_uri_get_path(uri);
	lower_path = g_ascii_strdown(path, -1);
	if (strstr(path, "..") != NULL || strstr(lower_path, "%2e") != NULL ||
	    strstr(path, "/./") != NULL || strstr(path, "//") != NULL)
	{
		return FALSE;
	}

	base_path = g_strdup(g_uri_get_path(base));
	base_len = strlen(base_path);
	while (base_len > 0 && base_path[base_len - 1] == '/')
	{
		base_path[--base_len] = '\0';
	}
	return strncmp(path, base_path, base_len) == 0 && path[base_len] == '/';
}

/* ---------------------------------------------------------------------- */
/* Workflows                                                              */
/* ---------------------------------------------------------------------- */

/*
 * Parses and checks a workflow in ComfyUI's API format: a JSON object of
 * nodes, each an object with a string `class_type` and, if present, an
 * object of `inputs`.  The editor's ordinary Save writes a different
 * document (`nodes` and `links` arrays) that /prompt cannot run, which is
 * the one mistake everybody makes once, so it gets its own message.
 *
 * Returns: (transfer full): the parser holding the graph, or %NULL
 */
static JsonParser *
cf_parse_workflow(
	const gchar  *text,
	GError      **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) parse_error = NULL;
	JsonObject *graph;
	GList *members;
	GList *iter;
	gboolean ok = TRUE;

	if (text == NULL || !json_parser_load_from_data(parser, text, -1, &parse_error))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "ComfyUI workflow is not JSON%s%s",
		            (parse_error != NULL) ? ": " : "",
		            (parse_error != NULL) ? parse_error->message : "");
		return NULL;
	}

	graph = ai_json_root_object(parser);
	if (graph == NULL || json_object_get_size(graph) == 0)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "ComfyUI workflow must be a non-empty JSON object of nodes "
		                    "(export it with \"Save (API format)\")");
		return NULL;
	}
	if (ai_json_get_array(graph, "nodes") != NULL && ai_json_get_node(graph, "links") != NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "ComfyUI workflow is in the editor's format (nodes and links), "
		                    "which /prompt cannot run: export it with \"Save (API format)\"");
		return NULL;
	}

	members = json_object_get_members(graph);
	for (iter = members; iter != NULL && ok; iter = iter->next)
	{
		const gchar *id = iter->data;
		JsonObject *node = ai_json_get_object(graph, id);

		if (node == NULL || ai_json_get_string(node, "class_type", NULL) == NULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "ComfyUI workflow node '%s' is not an object with a class_type "
			            "(export it with \"Save (API format)\")", id);
			ok = FALSE;
		}
		else if (ai_json_get_node(node, "inputs") != NULL &&
		         ai_json_get_object(node, "inputs") == NULL)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			            "ComfyUI workflow node '%s' has inputs that are not an object", id);
			ok = FALSE;
		}
	}
	g_list_free(members);

	if (!ok)
	{
		return NULL;
	}
	return (JsonParser *)g_steal_pointer(&parser);
}

/* What one submission puts in place of the placeholders. */
typedef struct
{
	const gchar *prompt;           /* nullable: {{prompt}} is then an error */
	const gchar *negative_prompt;  /* never NULL */
	gint64       seed;
	gchar       *seed_text;
	const gchar *image;            /* nullable: {{image}} is then an error */
} CfValues;

/*
 * Expands the placeholders in one string input.  A single left-to-right
 * pass: substituted text is never scanned again, so a prompt that itself
 * says "{{seed}}" stays those eight characters.  An unrecognised "{{x}}" is
 * left alone, since a workflow may contain braces of its own.
 *
 * Returns: (transfer full) (nullable): the new text, or %NULL with @error set
 */
static gchar *
cf_expand(
	const gchar  *text,
	CfValues     *values,
	const gchar  *node_id,
	const gchar  *input,
	GError      **error
){
	static const gchar * const names[] = {
		AI_COMFYUI_PLACEHOLDER_PROMPT,
		AI_COMFYUI_PLACEHOLDER_NEGATIVE_PROMPT,
		AI_COMFYUI_PLACEHOLDER_SEED,
		AI_COMFYUI_PLACEHOLDER_IMAGE,
		NULL
	};
	GString *out = g_string_new(NULL);
	const gchar *cursor = text;
	const gchar *open;

	while ((open = strstr(cursor, "{{")) != NULL)
	{
		const gchar *value = NULL;
		gsize matched = 0;
		guint i;

		g_string_append_len(out, cursor, open - cursor);
		for (i = 0; names[i] != NULL; i++)
		{
			if (g_str_has_prefix(open, names[i]))
			{
				matched = strlen(names[i]);
				break;
			}
		}

		switch (i)
		{
			case 0:
				value = values->prompt;
				if (value == NULL)
				{
					g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
					            "ComfyUI workflow node '%s' input '%s' uses %s, but the "
					            "request has no prompt", node_id, input,
					            AI_COMFYUI_PLACEHOLDER_PROMPT);
					g_string_free(out, TRUE);
					return NULL;
				}
				break;
			case 1:
				value = values->negative_prompt;
				break;
			case 2:
				value = values->seed_text;
				break;
			case 3:
				value = values->image;
				if (value == NULL)
				{
					g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
					            "ComfyUI workflow node '%s' input '%s' uses %s, but the "
					            "request has no image: upload one with "
					            "ai_comfyui_client_upload_async() and pass its name to "
					            "ai_media_request_set_image_url()", node_id, input,
					            AI_COMFYUI_PLACEHOLDER_IMAGE);
					g_string_free(out, TRUE);
					return NULL;
				}
				break;
			default:
				/* Not ours: keep the braces and carry on after them. */
				g_string_append(out, "{{");
				cursor = open + 2;
				continue;
		}

		g_string_append(out, value);
		cursor = open + matched;
	}
	g_string_append(out, cursor);

	return g_string_free(out, FALSE);
}

/*
 * Fills the placeholders in every node's direct string inputs, in place.
 * Inputs that are links (`["4", 0]`), numbers or objects are not touched:
 * a placeholder is a string value somebody typed into a widget.  An input
 * that is exactly "{{seed}}" becomes a JSON number, because a seed widget
 * is an INT and ComfyUI's validation refuses a string there.
 */
static gboolean
cf_fill_graph(
	JsonObject  *graph,
	CfValues    *values,
	GError     **error
){
	GList *ids;
	GList *id_iter;
	gboolean ok = TRUE;

	ids = json_object_get_members(graph);
	for (id_iter = ids; id_iter != NULL && ok; id_iter = id_iter->next)
	{
		const gchar *node_id = id_iter->data;
		JsonObject *node = ai_json_get_object(graph, node_id);
		JsonObject *inputs = (node != NULL) ? ai_json_get_object(node, "inputs") : NULL;
		GList *names;
		GList *name_iter;

		if (inputs == NULL)
		{
			continue;
		}

		names = json_object_get_members(inputs);
		for (name_iter = names; name_iter != NULL && ok; name_iter = name_iter->next)
		{
			const gchar *input = name_iter->data;
			const gchar *text = ai_json_get_string(inputs, input, NULL);
			g_autofree gchar *expanded = NULL;

			if (text == NULL || strstr(text, "{{") == NULL)
			{
				continue;
			}
			if (strcmp(text, AI_COMFYUI_PLACEHOLDER_SEED) == 0)
			{
				json_object_set_int_member(inputs, input, values->seed);
				continue;
			}

			expanded = cf_expand(text, values, node_id, input, error);
			if (expanded == NULL)
			{
				ok = FALSE;
			}
			else if (strcmp(expanded, text) != 0)
			{
				json_object_set_string_member(inputs, input, expanded);
			}
		}
		g_list_free(names);
	}
	g_list_free(ids);

	return ok;
}

/* A seed from the kernel; a seed is not a secret, so a clock is the fallback. */
static gint64
cf_random_seed(void)
{
	guint64 seed = 0;
	gint fd;

	fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || read(fd, &seed, sizeof seed) != (gssize)sizeof seed)
	{
		seed = ((guint64)g_random_int() << 32) ^ (guint64)g_random_int() ^
		       (guint64)g_get_real_time();
	}
	if (fd >= 0)
	{
		close(fd);
	}
	return (gint64)(seed & CF_SEED_MASK);
}

/*
 * Builds the body of a POST /prompt: the request's workflow (or the stored
 * one) with its placeholders filled, plus this client's client_id.
 *
 * Request members read: `prompt`, `negative_prompt`, `seed`, `image_url`
 * and `workflow`.  `model` is not read -- the graph names its own
 * checkpoint -- so it may be anything, NULL included.
 */
static GBytes *
cf_build_body(
	AiComfyuiClient  *self,
	AiMediaRequest   *request,
	GError          **error
){
	g_autofree gchar *request_json = NULL;
	g_autoptr(JsonParser) request_parser = json_parser_new();
	g_autoptr(JsonParser) graph_parser = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *override_text = NULL;
	JsonObject *params;
	JsonObject *graph;
	JsonNode *node;
	CfValues values;
	gchar *json;
	gboolean filled;

	memset(&values, 0, sizeof values);

	/* The request, as its own serialiser sees it: parameters plus prompt. */
	request_json = ai_media_request_to_json(request);
	if (!json_parser_load_from_data(request_parser, request_json, -1, NULL) ||
	    (params = ai_json_root_object(request_parser)) == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "ComfyUI request could not be serialised");
		return NULL;
	}

	/* The workflow: the request's own if it has one, else the stored one. */
	node = ai_json_get_node(params, "workflow");
	if (node != NULL)
	{
		if (JSON_NODE_HOLDS_OBJECT(node))
		{
			override_text = json_to_string(node, FALSE);
		}
		else if (ai_json_get_string(params, "workflow", NULL) != NULL)
		{
			override_text = g_strdup(ai_json_get_string(params, "workflow", NULL));
		}
		else
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                    "ComfyUI request parameter 'workflow' must be a JSON object "
			                    "(or a string holding one)");
			return NULL;
		}
		graph_parser = cf_parse_workflow(override_text, error);
	}
	else if (self->workflow_json != NULL)
	{
		graph_parser = cf_parse_workflow(self->workflow_json, error);
	}
	else
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		                    "No ComfyUI workflow: call ai_comfyui_client_set_workflow() with a "
		                    "graph exported with \"Save (API format)\", or pass one as the "
		                    "request's 'workflow' parameter");
		return NULL;
	}
	if (graph_parser == NULL)
	{
		return NULL;
	}
	graph = ai_json_root_object(graph_parser);

	/* The values. */
	values.prompt = ai_json_get_string(params, "prompt", NULL);
	node = ai_json_get_node(params, "negative_prompt");
	values.negative_prompt = ai_json_get_string(params, "negative_prompt", NULL);
	if (node != NULL && values.negative_prompt == NULL)
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                    "ComfyUI request parameter 'negative_prompt' must be a string");
		return NULL;
	}
	if (values.negative_prompt == NULL)
	{
		values.negative_prompt = "";
	}

	node = ai_json_get_node(params, "seed");
	if (node == NULL)
	{
		values.seed = cf_random_seed();
	}
	else if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 &&
	         json_node_get_int(node) >= 0)
	{
		values.seed = json_node_get_int(node);
	}
	else
	{
		const gchar *text = ai_json_get_string(params, "seed", NULL);
		guint64 parsed = 0;

		if (text == NULL ||
		    !g_ascii_string_to_unsigned(text, 10, 0, G_MAXINT64, &parsed, NULL))
		{
			g_set_error_literal(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
			                    "ComfyUI request parameter 'seed' must be a non-negative integer");
			return NULL;
		}
		values.seed = (gint64)parsed;
	}
	values.seed_text = g_strdup_printf("%" G_GINT64_FORMAT, values.seed);

	values.image = ai_json_get_string(params, "image_url", NULL);
	if (values.image != NULL && !cf_file_name_is_valid(values.image, TRUE))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		            "ComfyUI image '%s' is not the name of a file in the server's input "
		            "folder: upload it with ai_comfyui_client_upload_async() and pass the "
		            "name that returns", values.image);
		g_free(values.seed_text);
		return NULL;
	}

	filled = cf_fill_graph(graph, &values, error);
	g_free(values.seed_text);
	if (!filled)
	{
		return NULL;
	}

	/* {"prompt": <graph>, "client_id": "..."} */
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "prompt");
	json_builder_add_value(builder, json_node_copy(json_parser_get_root(graph_parser)));
	json_builder_set_member_name(builder, "client_id");
	json_builder_add_string_value(builder, self->client_id);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);

	json = json_to_string(root, FALSE);
	return g_bytes_new_take(json, strlen(json));
}

/* ---------------------------------------------------------------------- */
/* Errors from the wire                                                   */
/* ---------------------------------------------------------------------- */

/* "message: details", from one of ComfyUI's error objects. */
static void
cf_append_error_object(
	GString    *out,
	JsonObject *object
){
	const gchar *message = ai_json_get_string(object, "message", NULL);
	const gchar *details = ai_json_get_string(object, "details", NULL);

	if (message == NULL)
	{
		message = ai_json_get_string(object, "type", "error");
	}
	g_string_append(out, message);
	if (details != NULL && details[0] != '\0')
	{
		g_string_append_printf(out, ": %s", details);
	}
}

/*
 * The useful text of an error body.  /prompt's 400 is
 * {"error": {"type", "message", "details", "extra_info"},
 *  "node_errors": {"6": {"class_type", "errors": [{...}], ...}}}
 * and is flattened to "message: details; node 6 (CLIPTextEncode): message:
 * details", because the node is what the operator has to go and fix.  A
 * body that is not that (a proxy's page) becomes a bounded excerpt.
 */
static gchar *
cf_error_detail(GBytes *body)
{
	gconstpointer data;
	gsize length = 0;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;

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
	    (root = ai_json_root_object(parser)) != NULL &&
	    (ai_json_get_node(root, "error") != NULL || ai_json_get_node(root, "node_errors") != NULL))
	{
		GString *out = g_string_new(NULL);
		JsonObject *node_errors = ai_json_get_object(root, "node_errors");
		JsonObject *error_object = ai_json_get_object(root, "error");

		if (error_object != NULL)
		{
			cf_append_error_object(out, error_object);
		}
		else if (ai_json_get_string(root, "error", NULL) != NULL)
		{
			g_string_append(out, ai_json_get_string(root, "error", NULL));
		}

		if (node_errors != NULL)
		{
			GList *ids = json_object_get_members(node_errors);
			GList *iter;

			for (iter = ids; iter != NULL; iter = iter->next)
			{
				const gchar *id = iter->data;
				JsonObject *entry = ai_json_get_object(node_errors, id);
				JsonArray *errors = (entry != NULL) ? ai_json_get_array(entry, "errors") : NULL;
				guint i;

				if (errors == NULL)
				{
					continue;
				}
				for (i = 0; i < json_array_get_length(errors); i++)
				{
					JsonObject *item = ai_json_array_get_object(errors, i);

					if (item == NULL)
					{
						continue;
					}
					if (out->len > 0)
					{
						g_string_append(out, "; ");
					}
					g_string_append_printf(out, "node %s (%s): ", id,
					                       ai_json_get_string(entry, "class_type", "?"));
					cf_append_error_object(out, item);
				}
			}
			g_list_free(ids);
		}

		if (out->len > 0)
		{
			return g_string_free(out, FALSE);
		}
		g_string_free(out, TRUE);
	}

	{
		g_autofree gchar *raw = g_strndup(data, MIN(length, (gsize)CF_ERROR_EXCERPT));
		gchar *valid = g_utf8_make_valid(raw, -1);

		return g_strstrip(valid);
	}
}

/* Maps a non-2xx status onto an AiError. */
static GError *
cf_status_error(
	AiComfyuiClient *self,
	const gchar     *what,
	guint            status,
	GBytes          *body
){
	g_autofree gchar *detail = cf_error_detail(body);
	const gchar *hint = "";
	gint code;

	if (SOUP_STATUS_IS_REDIRECTION(status))
	{
		return cf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                    g_strdup_printf("ComfyUI %s answered with a redirect (HTTP %u), "
		                                    "which is not followed: point the base URL at "
		                                    "where ComfyUI actually answers", what, status));
	}

	switch (status)
	{
		case 400:
		case 413:
		case 422:
			code = AI_ERROR_INVALID_REQUEST;
			break;
		case 401:
			code = AI_ERROR_INVALID_API_KEY;
			hint = " (ComfyUI has no authentication of its own: a proxy in front of it "
			       "refused the request; set its token with ai_comfyui_client_set_token())";
			break;
		case 403:
			code = AI_ERROR_PERMISSION_DENIED;
			hint = " (a proxy refused the token, or ComfyUI refused a cross-origin request)";
			break;
		case 404:
			code = AI_ERROR_INVALID_REQUEST;
			hint = " (no such file, or the base URL does not point at ComfyUI)";
			break;
		case 429:
			code = AI_ERROR_RATE_LIMITED;
			break;
		case 502:
		case 503:
		case 504:
			code = AI_ERROR_SERVICE_UNAVAILABLE;
			break;
		default:
			code = (status >= 500) ? AI_ERROR_SERVER_ERROR : AI_ERROR_UNKNOWN;
			break;
	}

	return cf_error_new(self, code,
	                    g_strdup_printf("ComfyUI %s failed (HTTP %u)%s%s%s", what, status,
	                                    (detail != NULL) ? ": " : "",
	                                    (detail != NULL) ? detail : "", hint));
}

/* Parses a 2xx body that must be a JSON object. */
static JsonParser *
cf_parse_object(
	AiComfyuiClient  *self,
	const gchar      *what,
	GBytes           *body,
	GError          **error
){
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) parse_error = NULL;
	gconstpointer data;
	gsize length = 0;

	data = (body != NULL) ? g_bytes_get_data(body, &length) : NULL;
	if (length == 0 ||
	    !json_parser_load_from_data(parser, data, (gssize)length, &parse_error) ||
	    ai_json_root_object(parser) == NULL)
	{
		g_propagate_error(error,
		                  cf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                               g_strdup_printf("ComfyUI %s returned a body that is not "
		                                               "a JSON object%s%s", what,
		                                               (parse_error != NULL) ? ": " : "",
		                                               (parse_error != NULL) ? parse_error->message : "")));
		return NULL;
	}
	return (JsonParser *)g_steal_pointer(&parser);
}

/* ---------------------------------------------------------------------- */
/* Sending and bounded reading                                            */
/* ---------------------------------------------------------------------- */

typedef void (*CfReply)(GTask *task, guint status, GBytes *body);

typedef struct
{
	SoupSession  *session;
	SoupMessage  *message;
	GInputStream *stream;
	GByteArray   *buffer;
	gsize         limit;
	CfReply       reply;
	const gchar  *what;   /* static: names the call in messages */
	gchar        *job_id;
	guint         stage;  /* poll: 0 history, 1 queue, 2 history again */
} CfOp;

static void
cf_op_free(gpointer data)
{
	CfOp *op = data;

	g_clear_object(&op->session);
	g_clear_object(&op->message);
	g_clear_object(&op->stream);
	if (op->buffer != NULL)
	{
		g_byte_array_unref(op->buffer);
	}
	g_free(op->job_id);
	g_free(op);
}

/*
 * The session for an operation that started on @context.  Called with
 * @context thread-default (the caller has just made its GTask, which
 * captured the same one), so a new session binds to it.
 */
static SoupSession *
cf_session_for(
	AiComfyuiClient *self,
	GMainContext    *context
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
cf_task_new(
	AiComfyuiClient     *self,
	gpointer             tag,
	const gchar         *what,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	GTask *task;
	CfOp *op;

	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, tag);
	g_task_set_static_name(task, what);

	op = g_new0(CfOp, 1);
	op->what = what;
	op->session = cf_session_for(self, g_task_get_context(task));
	g_task_set_task_data(task, op, cf_op_free);

	return task;
}

/*
 * A request to an absolute URL already checked to be under the base.  It
 * is declared as carrying a credential whether or not a token is set: the
 * body of a /prompt is the prompt, and a request to the operator's own
 * server has no business following a redirect anywhere.
 */
static SoupMessage *
cf_message_for_uri(
	AiComfyuiClient *self,
	const gchar     *method,
	GUri            *uri
){
	SoupMessage *message;
	SoupMessageHeaders *headers;

	message = ai_http_message_new_from_uri(method, uri, AI_HTTP_CARRIES_CREDENTIALS);
	soup_message_add_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);
	if (self->token != NULL)
	{
		gchar *authorization = g_strdup_printf("Bearer %s", self->token);

		soup_message_headers_replace(headers, "Authorization", authorization);
		cf_wipe_free(authorization);
	}
	return message;
}

/* A request to `<base-url>/<path>[?query]`. */
static SoupMessage *
cf_api_message(
	AiComfyuiClient  *self,
	const gchar      *method,
	const gchar      *path,
	const gchar      *query,
	GError          **error
){
	g_autofree gchar *url = NULL;
	g_autoptr(GUri) uri = NULL;
	SoupMessage *message;

	url = g_strdup_printf("%s/%s%s%s", self->base_url, path,
	                      (query != NULL) ? "?" : "", (query != NULL) ? query : "");
	uri = cf_parse_http_uri(url);
	if (uri == NULL)
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		            "ComfyUI base URL '%s' is not an http or https URL", self->base_url);
		return NULL;
	}

	message = cf_message_for_uri(self, method, uri);
	soup_message_headers_replace(soup_message_get_request_headers(message),
	                             "Accept", "application/json");
	return message;
}

/* A transport failure, named with where the server was expected. */
static void
cf_return_transport_error(
	GTask  *task,
	GError *error
){
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);

	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
	{
		g_task_return_error(task, g_error_copy(error));
		return;
	}
	g_task_return_error(task,
	                    cf_error_new(self,
	                                 g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)
	                                     ? AI_ERROR_TIMEOUT : AI_ERROR_NETWORK_ERROR,
	                                 g_strdup_printf("ComfyUI %s failed: %s (is ComfyUI "
	                                                 "running at %s?)", op->what,
	                                                 error->message, self->base_url)));
}

static void cf_read_cb(GObject *source, GAsyncResult *result, gpointer user_data);

/*
 * Asks for the next chunk.  The task's context is pushed explicitly so the
 * read's own GTask lands where this operation started, however this was
 * reached.
 */
static void
cf_read_next(GTask *task)
{
	CfOp *op = g_task_get_task_data(task);
	GMainContext *context = g_task_get_context(task);

	g_main_context_push_thread_default(context);
	g_input_stream_read_bytes_async(op->stream, CF_READ_CHUNK, G_PRIORITY_DEFAULT,
	                                g_task_get_cancellable(task), cf_read_cb,
	                                g_object_ref(task));
	g_main_context_pop_thread_default(context);
}

static void
cf_read_cb(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = G_TASK(user_data);
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) chunk = NULL;
	g_autoptr(GBytes) body = NULL;
	gsize size;

	chunk = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
	if (chunk == NULL)
	{
		cf_return_transport_error(task, error);
		return;
	}

	size = g_bytes_get_size(chunk);
	if (size == 0)
	{
		body = g_byte_array_free_to_bytes(op->buffer);
		op->buffer = NULL;
		g_clear_object(&op->stream);
		op->reply(task, soup_message_get_status(op->message), body);
		return;
	}

	/* The ceiling is checked before the chunk is kept, not after. */
	if (size > op->limit - op->buffer->len)
	{
		g_task_return_error(task,
		                    cf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup_printf("ComfyUI %s answer is larger than %"
		                                                 G_GSIZE_FORMAT " bytes; refused",
		                                                 op->what, op->limit)));
		return;
	}
	g_byte_array_append(op->buffer, g_bytes_get_data(chunk, NULL), (guint)size);
	cf_read_next(task);
}

static void
cf_sent_cb(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	g_autoptr(GTask) task = G_TASK(user_data);
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	SoupMessageHeaders *headers;
	GInputStream *stream;

	stream = soup_session_send_finish(SOUP_SESSION(source), result, &error);
	if (stream == NULL)
	{
		cf_return_transport_error(task, error);
		return;
	}
	op->stream = stream;

	/* A declared length past the ceiling is refused before a byte is read. */
	headers = soup_message_get_response_headers(op->message);
	if (soup_message_headers_get_encoding(headers) == SOUP_ENCODING_CONTENT_LENGTH &&
	    (guint64)soup_message_headers_get_content_length(headers) > (guint64)op->limit)
	{
		g_task_return_error(task,
		                    cf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup_printf("ComfyUI %s answer declares %"
		                                                 G_GINT64_FORMAT " bytes, more than the %"
		                                                 G_GSIZE_FORMAT " allowed; refused",
		                                                 op->what,
		                                                 soup_message_headers_get_content_length(headers),
		                                                 op->limit)));
		return;
	}

	if (op->buffer != NULL)
	{
		g_byte_array_unref(op->buffer);
	}
	op->buffer = g_byte_array_new();
	cf_read_next(task);
}

/* Sends @message (transfer full) for @task; @reply gets the bounded answer. */
static void
cf_send(
	GTask       *task,
	SoupMessage *message,
	gsize        limit,
	CfReply      reply
){
	CfOp *op = g_task_get_task_data(task);
	GMainContext *context = g_task_get_context(task);

	g_set_object(&op->message, message);
	g_object_unref(message);
	op->reply = reply;
	op->limit = limit;

	g_debug("comfyui: %s %s", soup_message_get_method(message), op->what);
	g_main_context_push_thread_default(context);
	soup_session_send_async(op->session, op->message, G_PRIORITY_DEFAULT,
	                        g_task_get_cancellable(task), cf_sent_cb, g_object_ref(task));
	g_main_context_pop_thread_default(context);
}

/* ---------------------------------------------------------------------- */
/* Submit                                                                 */
/* ---------------------------------------------------------------------- */

static void
cf_submit_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);
	g_autoptr(JsonParser) parser = NULL;
	GError *error = NULL;
	JsonObject *object;
	JsonObject *node_errors;
	const gchar *prompt_id;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, cf_status_error(self, op->what, status, body));
		return;
	}

	parser = cf_parse_object(self, op->what, body, &error);
	if (parser == NULL)
	{
		g_task_return_error(task, error);
		return;
	}
	object = ai_json_root_object(parser);

	prompt_id = ai_json_get_string(object, "prompt_id", NULL);
	if (!cf_job_id_is_valid(prompt_id))
	{
		g_task_return_error(task,
		                    cf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("ComfyUI submit returned no usable prompt_id")));
		return;
	}

	/* Accepted with some outputs refused: it runs the rest, so say so quietly. */
	node_errors = ai_json_get_object(object, "node_errors");
	if (node_errors != NULL && json_object_get_size(node_errors) > 0)
	{
		g_debug("comfyui: prompt %s accepted with errors in %u node(s)",
		        prompt_id, json_object_get_size(node_errors));
	}

	g_task_return_pointer(task, g_strdup(prompt_id), g_free);
}

static void
cf_submit_async(
	AiMediaGenerator    *generator,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiComfyuiClient *self = AI_COMFYUI_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;
	SoupMessage *message;

	task = cf_task_new(self, cf_submit_async, "submit", cancellable, callback, user_data);

	body = cf_build_body(self, request, &error);
	if (body == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	message = cf_api_message(self, "POST", "prompt", NULL, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	soup_message_set_request_body_from_bytes(message, "application/json", body);
	cf_send(task, message, CF_MAX_JSON_BYTES, cf_submit_reply);
}

static gchar *
cf_submit_finish(
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

/*
 * Orders node ids as a person reads them: numbers numerically ("9" before
 * "12"), then anything else ("12:3" from a group node) by bytes.
 */
static gint
cf_node_id_compare(
	gconstpointer a,
	gconstpointer b
){
	const gchar *x = a;
	const gchar *y = b;
	guint64 nx = 0;
	guint64 ny = 0;
	gboolean x_num = g_ascii_string_to_unsigned(x, 10, 0, G_MAXUINT64, &nx, NULL);
	gboolean y_num = g_ascii_string_to_unsigned(y, 10, 0, G_MAXUINT64, &ny, NULL);

	if (x_num && y_num)
	{
		return (nx < ny) ? -1 : (nx > ny) ? 1 : 0;
	}
	if (x_num != y_num)
	{
		return x_num ? -1 : 1;
	}
	return strcmp(x, y);
}

/* The /view URL for one output entry. */
static gchar *
cf_view_url(
	AiComfyuiClient *self,
	const gchar     *filename,
	const gchar     *subfolder,
	const gchar     *type
){
	g_autofree gchar *f = g_uri_escape_string(filename, NULL, FALSE);
	g_autofree gchar *s = g_uri_escape_string(subfolder, NULL, FALSE);
	g_autofree gchar *t = g_uri_escape_string(type, NULL, FALSE);

	return g_strdup_printf("%s/view?filename=%s&subfolder=%s&type=%s",
	                       self->base_url, f, s, t);
}

/*
 * Adds every output file of a finished prompt, in node order.  SaveImage
 * and PreviewImage report `images`; common video and audio nodes report
 * `gifs`, `videos` or `audio` in the same {filename, subfolder, type} shape.
 */
static void
cf_collect_outputs(
	AiComfyuiClient *self,
	AiMediaJob      *job,
	JsonObject      *outputs
){
	static const gchar * const kinds[] = { "images", "gifs", "videos", "audio", NULL };
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GList *ids;
	GList *iter;

	ids = g_list_sort(json_object_get_members(outputs), cf_node_id_compare);
	for (iter = ids; iter != NULL; iter = iter->next)
	{
		JsonObject *node = ai_json_get_object(outputs, iter->data);
		guint k;

		if (node == NULL)
		{
			continue;
		}
		for (k = 0; kinds[k] != NULL; k++)
		{
			JsonArray *files = ai_json_get_array(node, kinds[k]);
			guint i;

			for (i = 0; files != NULL && i < json_array_get_length(files); i++)
			{
				JsonObject *file = ai_json_array_get_object(files, i);
				const gchar *filename;
				gchar *url;

				if (file == NULL)
				{
					continue;
				}
				filename = ai_json_get_string(file, "filename", NULL);
				if (filename == NULL || filename[0] == '\0')
				{
					continue;
				}
				url = cf_view_url(self, filename,
				                  ai_json_get_string(file, "subfolder", ""),
				                  ai_json_get_string(file, "type", "output"));
				if (g_hash_table_add(seen, url))
				{
					ai_media_job_add_result_url(job, url);
				}
			}
		}
	}
	g_list_free(ids);
}

/*
 * Why a prompt failed, from its status messages.  Each message is a
 * [event, data] pair; `execution_error` carries node_id, node_type,
 * exception_type and exception_message.
 */
static gchar *
cf_failure_message(
	JsonArray *messages,
	gboolean  *interrupted
){
	gchar *found = NULL;
	guint i;

	*interrupted = FALSE;
	for (i = 0; messages != NULL && i < json_array_get_length(messages); i++)
	{
		JsonArray *pair;
		JsonObject *data;
		const gchar *event;

		if (!JSON_NODE_HOLDS_ARRAY(json_array_get_element(messages, i)))
		{
			continue;
		}
		pair = json_array_get_array_element(messages, i);
		event = ai_json_array_get_string(pair, 0, NULL);
		data = ai_json_array_get_object(pair, 1);

		if (g_strcmp0(event, "execution_interrupted") == 0)
		{
			*interrupted = TRUE;
		}
		else if (g_strcmp0(event, "execution_error") == 0 && data != NULL && found == NULL)
		{
			const gchar *type = ai_json_get_string(data, "exception_type", NULL);
			/* Python's messages end in a newline more often than not. */
			g_autofree gchar *text = g_strstrip(g_strdup(ai_json_get_string(data, "exception_message",
			                                                                "no message")));

			found = g_strdup_printf("node %s (%s): %s%s%s",
			                        ai_json_get_string(data, "node_id", "?"),
			                        ai_json_get_string(data, "node_type", "?"),
			                        (type != NULL) ? type : "",
			                        (type != NULL) ? ": " : "", text);
		}
	}
	return found;
}

/* Turns one history entry into a job; FALSE with @error for a shape we do not know. */
static AiMediaJob *
cf_job_from_history(
	AiComfyuiClient  *self,
	const gchar      *job_id,
	JsonObject       *entry,
	GError          **error
){
	g_autoptr(AiMediaJob) job = NULL;
	JsonObject *status = ai_json_get_object(entry, "status");
	JsonObject *outputs = ai_json_get_object(entry, "outputs");
	const gchar *status_str = (status != NULL) ? ai_json_get_string(status, "status_str", NULL) : NULL;
	g_autofree gchar *failure = NULL;
	gboolean interrupted = FALSE;

	if (status != NULL)
	{
		failure = cf_failure_message(ai_json_get_array(status, "messages"), &interrupted);
	}

	if (interrupted)
	{
		job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_CANCELLED);
		ai_media_job_set_provider_status(job, "interrupted");
		ai_media_job_set_error_message(job, "Interrupted on the ComfyUI server");
	}
	else if (g_strcmp0(status_str, "error") == 0)
	{
		job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_FAILED);
		ai_media_job_set_provider_status(job, status_str);
		if (failure != NULL)
		{
			failure = cf_redact(self, failure);
		}
		ai_media_job_set_error_message(job, (failure != NULL) ? failure
		                               : "ComfyUI reported an error and no message");
	}
	else if (g_strcmp0(status_str, "success") == 0 || status == NULL)
	{
		/* An entry with no status at all is what older servers wrote on success. */
		job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_DONE);
		ai_media_job_set_provider_status(job, (status_str != NULL) ? status_str : "success");
		if (outputs != NULL)
		{
			cf_collect_outputs(self, job, outputs);
		}
		/*
		 * Finished with nothing to download is not a result anybody asked
		 * for: the workflow has no save node, or every output was cached
		 * away.  Say which, rather than hand back an empty success.
		 */
		if (ai_media_job_get_n_result_urls(job) == 0)
		{
			g_clear_object(&job);
			job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_FAILED);
			ai_media_job_set_provider_status(job, "success");
			ai_media_job_set_error_message(job,
			                               "ComfyUI finished the prompt with no image output: "
			                               "the workflow needs a SaveImage (or PreviewImage) node");
		}
	}
	else
	{
		g_propagate_error(error,
		                  cf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                               g_strdup_printf("ComfyUI reported an unknown status '%s' "
		                                               "for prompt %s", status_str, job_id)));
		return NULL;
	}

	/* A self-hosted server charges nothing: zero, not unknown. */
	ai_media_job_set_cost_micro_usd(job, 0);
	return (AiMediaJob *)g_steal_pointer(&job);
}

/* Whether a /queue list holds @job_id; each item is [number, prompt_id, ...]. */
static gboolean
cf_queue_has(
	JsonArray   *items,
	const gchar *job_id
){
	guint i;

	for (i = 0; items != NULL && i < json_array_get_length(items); i++)
	{
		if (JSON_NODE_HOLDS_ARRAY(json_array_get_element(items, i)) &&
		    g_strcmp0(ai_json_array_get_string(json_array_get_array_element(items, i), 1, NULL),
		              job_id) == 0)
		{
			return TRUE;
		}
	}
	return FALSE;
}

static void cf_poll_send(GTask *task);

/*
 * A poll is up to three requests.  /history first; while it is empty the
 * prompt has not finished, and /queue says whether it is running or
 * waiting.  If /queue does not have it either, it finished in between
 * (ComfyUI moves a prompt from the queue to the history under one lock),
 * or the id is unknown -- so /history is asked once more to tell the two
 * apart, rather than reporting a finished prompt as lost.
 */
static void
cf_poll_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);
	g_autoptr(JsonParser) parser = NULL;
	GError *error = NULL;
	JsonObject *object;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, cf_status_error(self, op->what, status, body));
		return;
	}

	parser = cf_parse_object(self, op->what, body, &error);
	if (parser == NULL)
	{
		g_task_return_error(task, error);
		return;
	}
	object = ai_json_root_object(parser);

	if (op->stage == 1)
	{
		AiMediaJob *job = NULL;

		if (cf_queue_has(ai_json_get_array(object, "queue_running"), op->job_id))
		{
			job = ai_media_job_new(op->job_id, AI_MEDIA_JOB_STATE_RUNNING);
			ai_media_job_set_provider_status(job, "running");
		}
		else if (cf_queue_has(ai_json_get_array(object, "queue_pending"), op->job_id))
		{
			job = ai_media_job_new(op->job_id, AI_MEDIA_JOB_STATE_QUEUED);
			ai_media_job_set_provider_status(job, "pending");
		}
		if (job != NULL)
		{
			ai_media_job_set_cost_micro_usd(job, 0);
			g_task_return_pointer(task, job, g_object_unref);
			return;
		}
		op->stage = 2;
		cf_poll_send(task);
		return;
	}

	{
		JsonObject *entry = ai_json_get_object(object, op->job_id);
		AiMediaJob *job;

		if (entry != NULL)
		{
			job = cf_job_from_history(self, op->job_id, entry, &error);
			if (job == NULL)
			{
				g_task_return_error(task, error);
				return;
			}
			g_task_return_pointer(task, job, g_object_unref);
			return;
		}
	}

	if (op->stage == 0)
	{
		op->stage = 1;
		cf_poll_send(task);
		return;
	}

	g_task_return_error(task,
	                    cf_error_new(self, AI_ERROR_INVALID_REQUEST,
	                                 g_strdup_printf("ComfyUI has no prompt %s in its queue or "
	                                                 "its history: an unknown id, or the server "
	                                                 "restarted or its history was cleared",
	                                                 op->job_id)));
}

/* Sends the request for the poll's current stage. */
static void
cf_poll_send(GTask *task)
{
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *path = NULL;
	SoupMessage *message;

	if (op->stage == 1)
	{
		op->what = "queue";
		path = g_strdup("queue");
	}
	else
	{
		op->what = "history";
		path = g_strdup_printf("history/%s", op->job_id);
	}

	message = cf_api_message(self, "GET", path, NULL, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	cf_send(task, message, CF_MAX_JSON_BYTES, cf_poll_reply);
}

static void
cf_poll_async(
	AiMediaGenerator    *generator,
	const gchar         *job_id,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiComfyuiClient *self = AI_COMFYUI_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	CfOp *op;

	task = cf_task_new(self, cf_poll_async, "history", cancellable, callback, user_data);
	op = g_task_get_task_data(task);

	if (!cf_job_id_is_valid(job_id))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a ComfyUI prompt id", job_id);
		return;
	}
	op->job_id = g_strdup(job_id);
	op->stage = 0;
	cf_poll_send(task);
}

static AiMediaJob *
cf_poll_finish(
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
cf_download_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, cf_status_error(self, op->what, status, body));
		return;
	}
	g_task_return_pointer(task, g_bytes_ref(body), (GDestroyNotify)g_bytes_unref);
}

static void
cf_download_async(
	AiMediaGenerator    *generator,
	const gchar         *url,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiComfyuiClient *self = AI_COMFYUI_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GUri) uri = NULL;

	task = cf_task_new(self, cf_download_async, "download", cancellable, callback, user_data);

	/*
	 * Only the configured server.  A result URL is the server's answer,
	 * and this request carries the token when one is set.
	 */
	if (!cf_url_is_under_base(self, url) || (uri = cf_parse_http_uri(url)) == NULL)
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "Refusing to download '%s': not under the ComfyUI base URL %s",
		                        (url != NULL) ? url : "(null)", self->base_url);
		return;
	}

	cf_send(task, cf_message_for_uri(self, "GET", uri), CF_MAX_DOWNLOAD_BYTES, cf_download_reply);
}

static GBytes *
cf_download_finish(
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

/*
 * A self-hosted server charges nothing, so the estimate is zero -- and
 * zero means "no charge", never "unknown" (that is AI_MEDIA_COST_UNKNOWN).
 * The request is still built, so an estimate refuses what a submit would.
 */
static void
cf_estimate_async(
	AiMediaGenerator    *generator,
	AiMediaRequest      *request,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	AiComfyuiClient *self = AI_COMFYUI_CLIENT(generator);
	g_autoptr(GTask) task = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GError) error = NULL;

	task = cf_task_new(self, cf_estimate_async, "estimate", cancellable, callback, user_data);

	body = cf_build_body(self, request, &error);
	if (body == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}
	g_task_return_boolean(task, TRUE);
}

static gboolean
cf_estimate_finish(
	AiMediaGenerator  *generator,
	GAsyncResult      *result,
	gint64            *micro_usd,
	GError           **error
){
	g_return_val_if_fail(g_task_is_valid(result, generator), FALSE);

	if (!g_task_propagate_boolean(G_TASK(result), error))
	{
		return FALSE;
	}
	*micro_usd = 0;
	return TRUE;
}

/*
 * No cancel: ComfyUI's POST /queue {"delete": [...]} and POST /interrupt
 * both answer 200 whether or not they did anything, so a cancel could not
 * honestly report its outcome.  can_cancel() answers FALSE.
 */
static void
media_generator_iface_init(AiMediaGeneratorInterface *iface)
{
	iface->submit_async    = cf_submit_async;
	iface->submit_finish   = cf_submit_finish;
	iface->poll_async      = cf_poll_async;
	iface->poll_finish     = cf_poll_finish;
	iface->download_async  = cf_download_async;
	iface->download_finish = cf_download_finish;
	iface->estimate_async  = cf_estimate_async;
	iface->estimate_finish = cf_estimate_finish;
}

/* ---------------------------------------------------------------------- */
/* Upload                                                                 */
/* ---------------------------------------------------------------------- */

static void
cf_upload_reply(
	GTask  *task,
	guint   status,
	GBytes *body
){
	AiComfyuiClient *self = g_task_get_source_object(task);
	CfOp *op = g_task_get_task_data(task);
	g_autoptr(JsonParser) parser = NULL;
	GError *error = NULL;
	JsonObject *object;
	const gchar *name;
	const gchar *subfolder;
	gchar *full;

	if (!SOUP_STATUS_IS_SUCCESSFUL(status))
	{
		g_task_return_error(task, cf_status_error(self, op->what, status, body));
		return;
	}

	parser = cf_parse_object(self, op->what, body, &error);
	if (parser == NULL)
	{
		g_task_return_error(task, error);
		return;
	}
	object = ai_json_root_object(parser);

	/* LoadImage names a file in a subfolder as "subfolder/name". */
	name = ai_json_get_string(object, "name", NULL);
	subfolder = ai_json_get_string(object, "subfolder", "");
	full = (subfolder[0] != '\0') ? g_strdup_printf("%s/%s", subfolder, name != NULL ? name : "")
	                              : g_strdup(name);
	if (!cf_file_name_is_valid(name, FALSE) || !cf_file_name_is_valid(full, TRUE))
	{
		g_free(full);
		g_task_return_error(task,
		                    cf_error_new(self, AI_ERROR_INVALID_RESPONSE,
		                                 g_strdup("ComfyUI upload returned no usable file name")));
		return;
	}
	g_task_return_pointer(task, full, g_free);
}

/**
 * ai_comfyui_client_upload_async:
 * @self: an #AiComfyuiClient
 * @data: the image's bytes
 * @filename: the name to store it under in the server's input folder, with
 *   no directory part, e.g. "reference.png"
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the stored name
 * @user_data: data for @callback
 *
 * Uploads a reference image with `POST /upload/image` (multipart, field
 * `image`, `overwrite` true), for a workflow whose LoadImage node takes
 * the `{{image}}` placeholder.  Finish with
 * ai_comfyui_client_upload_finish() and pass the name it returns to
 * ai_media_request_set_image_url(); ComfyUI may store the file under a
 * different name than asked, so use the returned one.
 */
void
ai_comfyui_client_upload_async(
	AiComfyuiClient     *self,
	GBytes              *data,
	const gchar         *filename,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
){
	g_autoptr(GTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *content_type = NULL;
	g_autofree gchar *mime = NULL;
	SoupMultipart *multipart;
	SoupMessage *message;

	g_return_if_fail(AI_IS_COMFYUI_CLIENT(self));
	g_return_if_fail(data != NULL);

	task = cf_task_new(self, ai_comfyui_client_upload_async, "upload", cancellable,
	                   callback, user_data);

	if (!cf_file_name_is_valid(filename, FALSE))
	{
		g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
		                        "'%s' is not a file name ComfyUI can store: no directory part, "
		                        "no leading dot, no control characters or quotes",
		                        (filename != NULL) ? filename : "(null)");
		return;
	}

	message = cf_api_message(self, "POST", "upload/image", NULL, &error);
	if (message == NULL)
	{
		g_task_return_error(task, (GError *)g_steal_pointer(&error));
		return;
	}

	content_type = g_content_type_guess(filename, g_bytes_get_data(data, NULL),
	                                    g_bytes_get_size(data), NULL);
	mime = g_content_type_get_mime_type(content_type);

	multipart = soup_multipart_new(SOUP_FORM_MIME_TYPE_MULTIPART);
	soup_multipart_append_form_file(multipart, "image", filename,
	                                (mime != NULL) ? mime : "application/octet-stream", data);
	soup_multipart_append_form_string(multipart, "overwrite", "true");
	soup_multipart_to_message(multipart, soup_message_get_request_headers(message), &body);
	soup_multipart_free(multipart);
	soup_message_set_request_body_from_bytes(message, NULL, body);

	cf_send(task, message, CF_MAX_JSON_BYTES, cf_upload_reply);
}

/**
 * ai_comfyui_client_upload_finish:
 * @self: an #AiComfyuiClient
 * @result: the #GAsyncResult
 * @error: return location for a #GError
 *
 * Returns: (transfer full) (nullable): the stored file's name, as
 *   LoadImage and the `{{image}}` placeholder take it, or %NULL
 */
gchar *
ai_comfyui_client_upload_finish(
	AiComfyuiClient  *self,
	GAsyncResult     *result,
	GError          **error
){
	g_return_val_if_fail(AI_IS_COMFYUI_CLIENT(self), NULL);
	g_return_val_if_fail(g_task_is_valid(result, self), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}

/* ---------------------------------------------------------------------- */
/* GObject                                                                */
/* ---------------------------------------------------------------------- */

static void
ai_comfyui_client_finalize(GObject *object)
{
	AiComfyuiClient *self = AI_COMFYUI_CLIENT(object);

	g_clear_object(&self->session);
	g_clear_pointer(&self->session_context, g_main_context_unref);
	g_free(self->base_url);
	cf_wipe_free(self->token);
	g_free(self->client_id);
	g_free(self->workflow_json);

	G_OBJECT_CLASS(ai_comfyui_client_parent_class)->finalize(object);
}

static void
ai_comfyui_client_get_property(
	GObject    *object,
	guint       prop_id,
	GValue     *value,
	GParamSpec *pspec
){
	AiComfyuiClient *self = AI_COMFYUI_CLIENT(object);

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
ai_comfyui_client_set_property(
	GObject      *object,
	guint         prop_id,
	const GValue *value,
	GParamSpec   *pspec
){
	AiComfyuiClient *self = AI_COMFYUI_CLIENT(object);

	switch (prop_id)
	{
		case PROP_BASE_URL:
			ai_comfyui_client_set_base_url(self, g_value_get_string(value));
			break;
		case PROP_TIMEOUT:
			ai_comfyui_client_set_timeout(self, g_value_get_uint(value));
			break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
	}
}

static void
ai_comfyui_client_class_init(AiComfyuiClientClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->finalize     = ai_comfyui_client_finalize;
	object_class->get_property = ai_comfyui_client_get_property;
	object_class->set_property = ai_comfyui_client_set_property;

	/**
	 * AiComfyuiClient:base-url:
	 *
	 * Where ComfyUI answers, with any path prefix a proxy adds (e.g.
	 * "https://gpu.example/comfy").  %NULL restores
	 * %AI_COMFYUI_DEFAULT_BASE_URL.  Every request, downloads included, goes
	 * to this origin and under this path, and nowhere else.
	 */
	properties[PROP_BASE_URL] =
		g_param_spec_string("base-url", NULL, NULL, AI_COMFYUI_DEFAULT_BASE_URL,
		                    G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                    G_PARAM_EXPLICIT_NOTIFY);

	/**
	 * AiComfyuiClient:timeout:
	 *
	 * Seconds of I/O inactivity any one request may take, downloads
	 * included; 0 for no limit.  It bounds a request, not a job: use the
	 * timeout of ai_media_generator_wait_async() for that.
	 */
	properties[PROP_TIMEOUT] =
		g_param_spec_uint("timeout", NULL, NULL, 0, G_MAXUINT, CF_DEFAULT_TIMEOUT,
		                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
		                  G_PARAM_EXPLICIT_NOTIFY);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
ai_comfyui_client_init(AiComfyuiClient *self)
{
	self->base_url = g_strdup(AI_COMFYUI_DEFAULT_BASE_URL);
	self->timeout = CF_DEFAULT_TIMEOUT;
	/* Names this client's prompts to ComfyUI; an identifier, not a secret. */
	self->client_id = g_uuid_string_random();
}

/**
 * ai_comfyui_client_new:
 * @base_url: (nullable): where ComfyUI answers, or %NULL for
 *   %AI_COMFYUI_DEFAULT_BASE_URL
 *
 * Creates a client for a ComfyUI server.  Nothing is read from the
 * environment.  Give it a workflow with ai_comfyui_client_set_workflow()
 * (or each request a `workflow` parameter) before submitting.
 *
 * Returns: (transfer full): a new #AiComfyuiClient
 */
AiComfyuiClient *
ai_comfyui_client_new(const gchar *base_url)
{
	g_autoptr(AiComfyuiClient) self = g_object_new(AI_TYPE_COMFYUI_CLIENT, NULL);

	ai_comfyui_client_set_base_url(self, base_url);
	return (AiComfyuiClient *)g_steal_pointer(&self);
}

/**
 * ai_comfyui_client_get_base_url:
 * @self: an #AiComfyuiClient
 *
 * Returns: (transfer none): the base URL, without a trailing slash
 */
const gchar *
ai_comfyui_client_get_base_url(AiComfyuiClient *self)
{
	g_return_val_if_fail(AI_IS_COMFYUI_CLIENT(self), NULL);

	return self->base_url;
}

/**
 * ai_comfyui_client_set_base_url:
 * @self: an #AiComfyuiClient
 * @base_url: (nullable): where ComfyUI answers, or %NULL for the default
 *
 * Points the client at a server.  Trailing slashes are dropped.  An URL
 * that is not http or https is accepted here and refused, with its text,
 * by the first request.
 */
void
ai_comfyui_client_set_base_url(
	AiComfyuiClient *self,
	const gchar     *base_url
){
	g_autofree gchar *clean = NULL;
	gsize length;

	g_return_if_fail(AI_IS_COMFYUI_CLIENT(self));

	if (base_url == NULL || base_url[0] == '\0')
	{
		base_url = AI_COMFYUI_DEFAULT_BASE_URL;
	}
	clean = g_strdup(base_url);
	length = strlen(clean);
	while (length > 1 && clean[length - 1] == '/')
	{
		clean[--length] = '\0';
	}
	if (g_set_str(&self->base_url, clean))
	{
		g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BASE_URL]);
	}
}

/**
 * ai_comfyui_client_get_timeout:
 * @self: an #AiComfyuiClient
 *
 * Returns: the per-request timeout in seconds, 0 for none
 */
guint
ai_comfyui_client_get_timeout(AiComfyuiClient *self)
{
	g_return_val_if_fail(AI_IS_COMFYUI_CLIENT(self), 0);

	return self->timeout;
}

/**
 * ai_comfyui_client_set_timeout:
 * @self: an #AiComfyuiClient
 * @seconds: the per-request timeout, 0 for none
 *
 * Sets the per-request timeout.  It applies to requests started afterwards.
 */
void
ai_comfyui_client_set_timeout(
	AiComfyuiClient *self,
	guint            seconds
){
	g_return_if_fail(AI_IS_COMFYUI_CLIENT(self));

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

/**
 * ai_comfyui_client_set_token:
 * @self: an #AiComfyuiClient
 * @token: (nullable): a bearer token, or %NULL to clear it
 *
 * Sets a token sent as `Authorization: Bearer <token>` on every request,
 * for a ComfyUI behind an authenticating reverse proxy; ComfyUI itself has
 * no authentication.  Deliberately not a property and with no getter, so
 * nothing holding the object can read it back.  It is never logged and is
 * redacted from every error.  A token containing a line break is refused
 * (and the old one still cleared).
 */
void
ai_comfyui_client_set_token(
	AiComfyuiClient *self,
	const gchar     *token
){
	g_return_if_fail(AI_IS_COMFYUI_CLIENT(self));

	cf_wipe_free(self->token);
	self->token = NULL;

	if (token == NULL || token[0] == '\0')
	{
		return;
	}
	if (strpbrk(token, "\r\n") != NULL)
	{
		g_message("ai-glib: refusing a ComfyUI token containing a line break");
		return;
	}
	self->token = g_strdup(token);
}

/**
 * ai_comfyui_client_has_token:
 * @self: an #AiComfyuiClient
 *
 * Returns: whether a token is set
 */
gboolean
ai_comfyui_client_has_token(AiComfyuiClient *self)
{
	g_return_val_if_fail(AI_IS_COMFYUI_CLIENT(self), FALSE);

	return self->token != NULL;
}

/**
 * ai_comfyui_client_set_workflow:
 * @self: an #AiComfyuiClient
 * @workflow_json: (nullable): a graph exported with ComfyUI's
 *   "Save (API format)", or %NULL to clear it
 * @error: return location for a #GError
 *
 * Sets the workflow every submission runs, unless a request carries its
 * own `workflow` parameter.  It must be a JSON object of nodes, each with a
 * `class_type`; the editor's ordinary save format is refused with a message
 * saying so.  On submission the placeholders %AI_COMFYUI_PLACEHOLDER_PROMPT,
 * %AI_COMFYUI_PLACEHOLDER_NEGATIVE_PROMPT, %AI_COMFYUI_PLACEHOLDER_SEED and
 * %AI_COMFYUI_PLACEHOLDER_IMAGE are filled in the nodes' string inputs.  A
 * workflow with none of them is valid, and then generates the same picture
 * whatever the request says.  On failure the stored workflow is unchanged.
 *
 * Returns: %TRUE if the workflow was stored
 */
gboolean
ai_comfyui_client_set_workflow(
	AiComfyuiClient  *self,
	const gchar      *workflow_json,
	GError          **error
){
	g_autoptr(JsonParser) parser = NULL;

	g_return_val_if_fail(AI_IS_COMFYUI_CLIENT(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	if (workflow_json == NULL)
	{
		g_clear_pointer(&self->workflow_json, g_free);
		return TRUE;
	}

	parser = cf_parse_workflow(workflow_json, error);
	if (parser == NULL)
	{
		return FALSE;
	}

	g_free(self->workflow_json);
	self->workflow_json = json_to_string(json_parser_get_root(parser), FALSE);
	if (strstr(self->workflow_json, "{{") == NULL)
	{
		g_debug("comfyui: the workflow has no placeholders; every request will run it unchanged");
	}
	return TRUE;
}

/**
 * ai_comfyui_client_get_workflow:
 * @self: an #AiComfyuiClient
 *
 * Returns: (transfer none) (nullable): the stored workflow as compact JSON,
 *   placeholders unfilled, or %NULL
 */
const gchar *
ai_comfyui_client_get_workflow(AiComfyuiClient *self)
{
	g_return_val_if_fail(AI_IS_COMFYUI_CLIENT(self), NULL);

	return self->workflow_json;
}
