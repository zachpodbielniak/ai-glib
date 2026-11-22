/*
 * test-comfyui-client.c - AiComfyuiClient
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Hermetic: every request goes to a loopback SoupServer on its own thread
 * that imitates ComfyUI's HTTP API -- /prompt with its validation errors,
 * /history moving from {} to a result, /queue, /view and /upload/image --
 * plus two path prefixes: /locked, an authenticating proxy that echoes a
 * refused Authorization header back in its 401, and /huge, which answers
 * a history poll with more than the client will hold.  Nothing leaves the
 * machine.
 */

#include <string.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "ai-glib.h"

#define TEST_TOKEN  "cfy-token-SHOULD-NEVER-APPEAR-31415926"

#define JOB_OK      "11111111-2222-3333-4444-555555555555"
#define JOB_FAIL    "22222222-2222-3333-4444-555555555555"
#define JOB_RACE    "33333333-2222-3333-4444-555555555555"
#define JOB_UNKNOWN "44444444-2222-3333-4444-555555555555"
#define JOB_EMPTY   "55555555-2222-3333-4444-555555555555"
#define JOB_STOPPED "66666666-2222-3333-4444-555555555555"

/*
 * The workflow every placeholder test runs: a seed that is the whole
 * input (becomes a number), a prompt inside other text, an unknown
 * placeholder that must be left alone, a negative prompt, an image, a seed
 * inside other text (stays text), and links and numbers that must not be
 * touched.
 */
static const gchar *WORKFLOW =
	"{"
	"\"3\":{\"class_type\":\"KSampler\",\"inputs\":{\"seed\":\"{{seed}}\",\"steps\":20,"
	"\"model\":[\"4\",0],\"positive\":[\"6\",0],\"negative\":[\"7\",0]}},"
	"\"4\":{\"class_type\":\"CheckpointLoaderSimple\",\"inputs\":{\"ckpt_name\":\"sd.safetensors\"}},"
	"\"6\":{\"class_type\":\"CLIPTextEncode\",\"inputs\":{\"text\":\"portrait of {{prompt}}, {{style}}\","
	"\"clip\":[\"4\",1]}},"
	"\"7\":{\"class_type\":\"CLIPTextEncode\",\"inputs\":{\"text\":\"{{negative_prompt}}\",\"clip\":[\"4\",1]}},"
	"\"8\":{\"class_type\":\"LoadImage\",\"inputs\":{\"image\":\"{{image}}\"}},"
	"\"9\":{\"class_type\":\"SaveImage\",\"inputs\":{\"filename_prefix\":\"seed-{{seed}}\","
	"\"images\":[\"8\",0]}}"
	"}";

/* Text-only: no {{image}}, so a request without an image is fine. */
static const gchar *WORKFLOW_TEXT =
	"{"
	"\"3\":{\"class_type\":\"KSampler\",\"inputs\":{\"seed\":\"{{seed}}\",\"positive\":[\"6\",0]}},"
	"\"6\":{\"class_type\":\"CLIPTextEncode\",\"inputs\":{\"text\":\"{{prompt}}\"}},"
	"\"9\":{\"class_type\":\"SaveImage\",\"inputs\":{\"filename_prefix\":\"x\",\"images\":[\"3\",0]}}"
	"}";

/* Every log line the process wrote, so the last test can search it. */
static GMutex   log_lock;
static GString *log_text;

/* ---------------------------------------------------------------------- */
/* The mock server                                                        */
/* ---------------------------------------------------------------------- */

typedef struct
{
	SoupServer   *server;
	GMainContext *context;
	GMainLoop    *loop;
	GThread      *thread;
	gchar        *base_url;
	GMutex        lock;

	GHashTable   *polls;    /* job id -> GUINT_TO_POINTER(history polls) */
	GHashTable   *headers;  /* path -> GHashTable(lower name -> value) */
	GHashTable   *bodies;   /* path -> body text */
	guint         hits;

	/* The last upload, as the server parsed it. */
	gchar        *upload_filename;
	gchar        *upload_type;
	gchar        *upload_overwrite;
	GBytes       *upload_bytes;
} MockCf;

static MockCf *mock;

static void
mock_reply(
	SoupServerMessage *msg,
	guint              status,
	const gchar       *body
){
	soup_server_message_set_status(msg, status, NULL);
	soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY,
	                                  body, strlen(body));
}

static guint
mock_count_poll(const gchar *job_id)
{
	guint n;

	g_mutex_lock(&mock->lock);
	n = GPOINTER_TO_UINT(g_hash_table_lookup(mock->polls, job_id)) + 1;
	g_hash_table_insert(mock->polls, g_strdup(job_id), GUINT_TO_POINTER(n));
	g_mutex_unlock(&mock->lock);
	return n;
}

static guint
mock_polls(const gchar *job_id)
{
	guint n;

	g_mutex_lock(&mock->lock);
	n = GPOINTER_TO_UINT(g_hash_table_lookup(mock->polls, job_id));
	g_mutex_unlock(&mock->lock);
	return n;
}

static void
mock_reset_polls(const gchar *job_id)
{
	g_mutex_lock(&mock->lock);
	g_hash_table_remove(mock->polls, job_id);
	g_mutex_unlock(&mock->lock);
}

/* GET /history/<id>: what the Nth poll of each job sees. */
static void
mock_history(
	SoupServerMessage *msg,
	const gchar       *job_id
){
	guint n = mock_count_poll(job_id);
	g_autofree gchar *body = NULL;

	if (g_strcmp0(job_id, JOB_OK) == 0 && n >= 3)
	{
		/* Node 12 is listed first; the client must still put node 9 first. */
		body = g_strdup_printf(
			"{\"%s\":{\"prompt\":[1,\"%s\",{},{},[\"9\"]],"
			"\"outputs\":{"
			"\"12\":{\"images\":[{\"filename\":\"b (1).png\",\"subfolder\":\"sub/dir\",\"type\":\"temp\"}]},"
			"\"9\":{\"images\":[{\"filename\":\"a.png\",\"subfolder\":\"\",\"type\":\"output\"},"
			"{\"filename\":\"a.png\",\"subfolder\":\"\",\"type\":\"output\"}]}},"
			"\"status\":{\"status_str\":\"success\",\"completed\":true,\"messages\":"
			"[[\"execution_start\",{\"prompt_id\":\"%s\"}],[\"execution_success\",{}]]}}}",
			job_id, job_id, job_id);
	}
	else if (g_strcmp0(job_id, JOB_RACE) == 0 && n >= 2)
	{
		body = g_strdup_printf(
			"{\"%s\":{\"outputs\":{\"9\":{\"images\":[{\"filename\":\"r.png\",\"subfolder\":\"\","
			"\"type\":\"output\"}]}},\"status\":{\"status_str\":\"success\",\"completed\":true,"
			"\"messages\":[]}}}", job_id);
	}
	else if (g_strcmp0(job_id, JOB_FAIL) == 0)
	{
		body = g_strdup_printf(
			"{\"%s\":{\"outputs\":{},\"status\":{\"status_str\":\"error\",\"completed\":false,"
			"\"messages\":[[\"execution_start\",{}],[\"execution_error\",{\"prompt_id\":\"%s\","
			"\"node_id\":\"3\",\"node_type\":\"KSampler\",\"exception_type\":\"RuntimeError\","
			"\"exception_message\":\"CUDA out of memory\\n\",\"traceback\":[]}]]}}}",
			job_id, job_id);
	}
	else if (g_strcmp0(job_id, JOB_STOPPED) == 0)
	{
		body = g_strdup_printf(
			"{\"%s\":{\"outputs\":{},\"status\":{\"status_str\":\"error\",\"completed\":false,"
			"\"messages\":[[\"execution_start\",{}],[\"execution_interrupted\",{\"node_id\":\"3\"}]]}}}",
			job_id);
	}
	else if (g_strcmp0(job_id, JOB_EMPTY) == 0)
	{
		body = g_strdup_printf("{\"%s\":{\"outputs\":{},\"status\":{\"status_str\":\"success\","
		                       "\"completed\":true,\"messages\":[]}}}", job_id);
	}
	else
	{
		body = g_strdup("{}");
	}
	mock_reply(msg, 200, body);
}

/* GET /queue: JOB_OK is pending at its first poll and running at its second. */
static void
mock_queue(SoupServerMessage *msg)
{
	guint n = mock_polls(JOB_OK);
	g_autofree gchar *body = NULL;
	const gchar *item = "[[7,\"" JOB_OK "\",{\"3\":{}},{\"client_id\":\"x\"},[\"9\"]]]";

	body = g_strdup_printf("{\"queue_running\":%s,\"queue_pending\":%s}",
	                       (n == 2) ? item : "[]",
	                       (n == 1) ? item : "[]");
	mock_reply(msg, 200, body);
}

/* POST /prompt: a graph with a node of class "Broken" is refused as ComfyUI would. */
static void
mock_prompt(
	SoupServerMessage *msg,
	const gchar       *body
){
	g_autoptr(JsonParser) parser = json_parser_new();
	JsonObject *root;
	JsonObject *graph;
	g_autofree gchar *reply = NULL;

	if (!json_parser_load_from_data(parser, body, -1, NULL) ||
	    !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
	{
		mock_reply(msg, 400, "{\"error\":{\"type\":\"invalid_json\",\"message\":\"bad\"}}");
		return;
	}
	root = json_node_get_object(json_parser_get_root(parser));
	graph = json_object_get_object_member(root, "prompt");
	if (json_object_has_member(graph, "5"))
	{
		mock_reply(msg, 400,
		           "{\"error\":{\"type\":\"prompt_outputs_failed_validation\","
		           "\"message\":\"Prompt outputs failed validation\",\"details\":\"\","
		           "\"extra_info\":{}},"
		           "\"node_errors\":{\"5\":{\"errors\":[{\"type\":\"value_not_in_list\","
		           "\"message\":\"Value not in list\",\"details\":\"ckpt_name: 'nope.safetensors' "
		           "not in []\",\"extra_info\":{}}],\"dependent_outputs\":[\"9\"],"
		           "\"class_type\":\"Broken\"}}}");
		return;
	}
	reply = g_strdup_printf("{\"prompt_id\":\"%s\",\"number\":7,\"node_errors\":{}}", JOB_OK);
	mock_reply(msg, 200, reply);
}

/* POST /upload/image: parse the multipart body the way aiohttp would. */
static void
mock_upload(SoupServerMessage *msg)
{
	SoupMessageBody *request_body = soup_server_message_get_request_body(msg);
	g_autoptr(GBytes) bytes = soup_message_body_flatten(request_body);
	SoupMultipart *multipart;
	SoupMessageHeaders *part_headers;
	GBytes *part_body;
	gint i;

	multipart = soup_multipart_new_from_message(soup_server_message_get_request_headers(msg), bytes);
	if (multipart == NULL)
	{
		mock_reply(msg, 400, "not multipart");
		return;
	}

	g_mutex_lock(&mock->lock);
	for (i = 0; i < soup_multipart_get_length(multipart); i++)
	{
		g_autoptr(GHashTable) params = NULL;
		const gchar *name;

		if (!soup_multipart_get_part(multipart, i, &part_headers, &part_body) ||
		    !soup_message_headers_get_content_disposition(part_headers, NULL, &params))
		{
			continue;
		}
		name = g_hash_table_lookup(params, "name");
		if (g_strcmp0(name, "image") == 0)
		{
			g_free(mock->upload_filename);
			mock->upload_filename = g_strdup(g_hash_table_lookup(params, "filename"));
			g_free(mock->upload_type);
			mock->upload_type = g_strdup(soup_message_headers_get_content_type(part_headers, NULL));
			g_clear_pointer(&mock->upload_bytes, g_bytes_unref);
			mock->upload_bytes = g_bytes_ref(part_body);
		}
		else if (g_strcmp0(name, "overwrite") == 0)
		{
			g_free(mock->upload_overwrite);
			mock->upload_overwrite = g_strndup(g_bytes_get_data(part_body, NULL),
			                                   g_bytes_get_size(part_body));
		}
	}
	g_mutex_unlock(&mock->lock);
	soup_multipart_free(multipart);

	mock_reply(msg, 200, "{\"name\":\"ref.png\",\"subfolder\":\"\",\"type\":\"input\"}");
}

static void
mock_handler(
	SoupServer        *server,
	SoupServerMessage *msg,
	const char        *full_path,
	GHashTable        *query,
	gpointer           user_data
){
	SoupMessageHeaders *request_headers = soup_server_message_get_request_headers(msg);
	SoupMessageBody *request_body = soup_server_message_get_request_body(msg);
	const gchar *method = soup_server_message_get_method(msg);
	const gchar *path = full_path;
	g_autofree gchar *body = NULL;
	GHashTable *seen;
	SoupMessageHeadersIter iter;
	const char *name;
	const char *value;

	(void)server;
	(void)user_data;

	body = g_strndup(request_body->data, request_body->length);

	g_mutex_lock(&mock->lock);
	mock->hits++;
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	soup_message_headers_iter_init(&iter, request_headers);
	while (soup_message_headers_iter_next(&iter, &name, &value))
	{
		g_hash_table_insert(seen, g_ascii_strdown(name, -1), g_strdup(value));
	}
	g_hash_table_insert(mock->headers, g_strdup(full_path), seen);
	g_hash_table_insert(mock->bodies, g_strdup(full_path), g_strdup(body));
	g_mutex_unlock(&mock->lock);

	/* The authenticating proxy: the wrong token comes back in its 401. */
	if (g_str_has_prefix(full_path, "/locked/"))
	{
		const gchar *auth = soup_message_headers_get_one(request_headers, "Authorization");

		if (g_strcmp0(auth, "Bearer " TEST_TOKEN) != 0)
		{
			g_autofree gchar *page = g_strdup_printf("<html>401: refused '%s'</html>",
			                                         (auth != NULL) ? auth : "(none)");

			soup_server_message_set_status(msg, 401, NULL);
			soup_server_message_set_response(msg, "text/html", SOUP_MEMORY_COPY,
			                                  page, strlen(page));
			return;
		}
		path = full_path + strlen("/locked");
	}

	/* A server that answers a poll with 17 MiB, chunked, so no length warns the client. */
	if (g_str_has_prefix(full_path, "/huge/"))
	{
		SoupMessageBody *response = soup_server_message_get_response_body(msg);
		gsize size = 17 * 1024 * 1024;
		gchar *blob = g_malloc(size);

		memset(blob, ' ', size);
		blob[0] = '{';
		blob[size - 1] = '}';
		soup_server_message_set_status(msg, 200, NULL);
		soup_message_headers_set_encoding(soup_server_message_get_response_headers(msg),
		                                  SOUP_ENCODING_CHUNKED);
		soup_message_body_append(response, SOUP_MEMORY_TAKE, blob, size);
		soup_message_body_complete(response);
		return;
	}

	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(path, "/prompt") == 0)
	{
		mock_prompt(msg, body);
		return;
	}
	if (g_strcmp0(method, "GET") == 0 && g_str_has_prefix(path, "/history/"))
	{
		mock_history(msg, path + strlen("/history/"));
		return;
	}
	if (g_strcmp0(method, "GET") == 0 && g_strcmp0(path, "/queue") == 0)
	{
		mock_queue(msg);
		return;
	}
	if (g_strcmp0(method, "GET") == 0 && g_strcmp0(path, "/view") == 0)
	{
		if (query == NULL || g_strcmp0(g_hash_table_lookup(query, "filename"), "a.png") != 0 ||
		    g_strcmp0(g_hash_table_lookup(query, "type"), "output") != 0)
		{
			soup_server_message_set_status(msg, 404, NULL);
			return;
		}
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/png", SOUP_MEMORY_STATIC, "PNGDATA\0X", 9);
		return;
	}
	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(path, "/upload/image") == 0)
	{
		mock_upload(msg);
		return;
	}

	soup_server_message_set_status(msg, 404, NULL);
}

static gpointer
mock_thread(gpointer user_data)
{
	(void)user_data;

	g_main_context_push_thread_default(mock->context);
	g_main_loop_run(mock->loop);
	g_main_context_pop_thread_default(mock->context);
	return NULL;
}

/*
 * The server runs on its own thread and context; the client's operations
 * run on the test thread's, so a source attached to the wrong context
 * hangs (into the watchdog) instead of passing by luck.
 */
static void
mock_start(void)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;
	gchar *full;

	mock = g_new0(MockCf, 1);
	g_mutex_init(&mock->lock);
	mock->polls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	mock->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      (GDestroyNotify)g_hash_table_unref);
	mock->bodies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	mock->context = g_main_context_new();
	mock->loop = g_main_loop_new(mock->context, FALSE);

	g_main_context_push_thread_default(mock->context);
	mock->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(mock->server, NULL, mock_handler, NULL, NULL);
	soup_server_listen_local(mock->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error);
	g_assert_no_error(error);
	g_main_context_pop_thread_default(mock->context);

	uris = soup_server_get_uris(mock->server);
	g_assert_nonnull(uris);
	full = g_uri_to_string((GUri *)uris->data);
	if (g_str_has_suffix(full, "/"))
	{
		full[strlen(full) - 1] = '\0';
	}
	mock->base_url = full;
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	mock->thread = g_thread_new("mock-comfyui", mock_thread, NULL);
}

static gboolean
mock_quit(gpointer user_data)
{
	(void)user_data;
	g_main_loop_quit(mock->loop);
	return G_SOURCE_REMOVE;
}

static void
mock_stop(void)
{
	g_main_context_invoke(mock->context, mock_quit, NULL);
	g_thread_join(mock->thread);

	g_main_context_push_thread_default(mock->context);
	soup_server_disconnect(mock->server);
	g_object_unref(mock->server);
	while (g_main_context_iteration(mock->context, FALSE))
		;
	g_main_context_pop_thread_default(mock->context);

	g_main_loop_unref(mock->loop);
	g_main_context_unref(mock->context);
	g_hash_table_unref(mock->polls);
	g_hash_table_unref(mock->headers);
	g_hash_table_unref(mock->bodies);
	g_free(mock->upload_filename);
	g_free(mock->upload_type);
	g_free(mock->upload_overwrite);
	g_clear_pointer(&mock->upload_bytes, g_bytes_unref);
	g_free(mock->base_url);
	g_mutex_clear(&mock->lock);
	g_free(mock);
}

static gchar *
mock_header(
	const gchar *path,
	const gchar *name
){
	GHashTable *seen;
	gchar *value = NULL;

	g_mutex_lock(&mock->lock);
	seen = g_hash_table_lookup(mock->headers, path);
	if (seen != NULL)
	{
		value = g_strdup(g_hash_table_lookup(seen, name));
	}
	g_mutex_unlock(&mock->lock);
	return value;
}

static gchar *
mock_body(const gchar *path)
{
	gchar *value;

	g_mutex_lock(&mock->lock);
	value = g_strdup(g_hash_table_lookup(mock->bodies, path));
	g_mutex_unlock(&mock->lock);
	return value;
}

static guint
mock_hits(void)
{
	guint n;

	g_mutex_lock(&mock->lock);
	n = mock->hits;
	g_mutex_unlock(&mock->lock);
	return n;
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static AiComfyuiClient *
new_client(const gchar *workflow)
{
	AiComfyuiClient *client = ai_comfyui_client_new(mock->base_url);
	g_autoptr(GError) error = NULL;

	ai_comfyui_client_set_timeout(client, 10);
	if (workflow != NULL)
	{
		g_assert_true(ai_comfyui_client_set_workflow(client, workflow, &error));
		g_assert_no_error(error);
	}
	return client;
}

static gboolean
watchdog_cb(gpointer user_data)
{
	(void)user_data;
	g_error("test timed out waiting for an async result");
	return G_SOURCE_REMOVE;
}

/* Iterates the default context until *@result is set, with a watchdog. */
static void
run_until(GAsyncResult **result)
{
	GSource *watchdog = g_timeout_source_new_seconds(20);

	g_source_set_callback(watchdog, watchdog_cb, NULL, NULL);
	g_source_attach(watchdog, NULL);
	while (*result == NULL)
	{
		g_main_context_iteration(NULL, TRUE);
	}
	g_source_destroy(watchdog);
	g_source_unref(watchdog);
}

static void
store_result(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	(void)source;
	*(GAsyncResult **)user_data = g_object_ref(result);
}

static gchar *
submit(
	AiComfyuiClient  *client,
	AiMediaRequest   *request,
	GError          **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_submit_async(AI_MEDIA_GENERATOR(client), request, NULL,
	                                store_result, &result);
	run_until(&result);
	return ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(client), result, error);
}

static AiMediaJob *
poll_once(
	AiComfyuiClient  *client,
	const gchar      *job_id,
	GError          **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_poll_async(AI_MEDIA_GENERATOR(client), job_id, NULL,
	                              store_result, &result);
	run_until(&result);
	return ai_media_generator_poll_finish(AI_MEDIA_GENERATOR(client), result, error);
}

static GBytes *
download(
	AiComfyuiClient  *client,
	const gchar      *url,
	GError          **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_download_async(AI_MEDIA_GENERATOR(client), url, NULL,
	                                  store_result, &result);
	run_until(&result);
	return ai_media_generator_download_finish(AI_MEDIA_GENERATOR(client), result, error);
}

/* The graph the server last received at @path. */
static JsonParser *
sent_graph(
	const gchar  *path,
	JsonObject  **graph
){
	g_autofree gchar *body = mock_body(path);
	JsonParser *parser = json_parser_new();
	JsonObject *root;

	g_assert_nonnull(body);
	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	root = json_node_get_object(json_parser_get_root(parser));
	g_assert_true(json_object_has_member(root, "client_id"));
	*graph = json_object_get_object_member(root, "prompt");
	return parser;
}

static JsonObject *
node_inputs(
	JsonObject  *graph,
	const gchar *id
){
	return json_object_get_object_member(json_object_get_object_member(graph, id), "inputs");
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

static void
test_defaults(void)
{
	g_autoptr(AiComfyuiClient) client = ai_comfyui_client_new(NULL);

	g_assert_cmpstr(ai_comfyui_client_get_base_url(client), ==, AI_COMFYUI_DEFAULT_BASE_URL);
	ai_comfyui_client_set_base_url(client, "http://gpu.example:8188/comfy//");
	g_assert_cmpstr(ai_comfyui_client_get_base_url(client), ==, "http://gpu.example:8188/comfy");
	g_assert_null(ai_comfyui_client_get_workflow(client));

	/* No cancel: ComfyUI's interrupt and delete cannot say whether they worked. */
	g_assert_false(ai_media_generator_can_cancel(AI_MEDIA_GENERATOR(client)));
	g_assert_true(ai_media_generator_can_estimate(AI_MEDIA_GENERATOR(client)));

	g_assert_false(ai_comfyui_client_has_token(client));
	ai_comfyui_client_set_token(client, "abc\r\nX-Evil: 1");
	g_assert_false(ai_comfyui_client_has_token(client));
	ai_comfyui_client_set_token(client, "abc");
	g_assert_true(ai_comfyui_client_has_token(client));
	ai_comfyui_client_set_token(client, NULL);
	g_assert_false(ai_comfyui_client_has_token(client));
}

static void
test_workflow_validation(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW_TEXT);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *before = g_strdup(ai_comfyui_client_get_workflow(client));

	g_assert_nonnull(before);

	/* The editor's ordinary save, named as such. */
	g_assert_false(ai_comfyui_client_set_workflow(client,
	                                              "{\"last_node_id\":9,\"nodes\":[],\"links\":[]}",
	                                              &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "Save (API format)"));
	g_clear_error(&error);

	g_assert_false(ai_comfyui_client_set_workflow(client, "[1,2,3]", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	g_assert_false(ai_comfyui_client_set_workflow(client, "{\"3\":{\"inputs\":{}}}", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "'3'"));
	g_clear_error(&error);

	g_assert_false(ai_comfyui_client_set_workflow(client, "{\"3\":{\"class_type\":\"X\","
	                                                      "\"inputs\":[1]}}", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	g_assert_false(ai_comfyui_client_set_workflow(client, "{not json", &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* A failure leaves the stored workflow alone. */
	g_assert_cmpstr(ai_comfyui_client_get_workflow(client), ==, before);

	/* No placeholders is still a workflow. */
	g_assert_true(ai_comfyui_client_set_workflow(client, "{\"1\":{\"class_type\":\"X\"}}", &error));
	g_assert_no_error(error);
	g_assert_true(ai_comfyui_client_set_workflow(client, NULL, &error));
	g_assert_null(ai_comfyui_client_get_workflow(client));
}

static void
test_placeholders(void)
{
	/* Quotes, braces and a placeholder of our own inside the prompt. */
	static const gchar *PROMPT = "a \"quoted\" {brace} }} \", \"evil\": {\"class_type\":\"X\"} {{seed}}";
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW);
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, PROMPT);
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *job_id = NULL;
	g_autofree gchar *expected = NULL;
	JsonObject *graph;
	JsonNode *seed;

	ai_media_request_set_param_int(request, "seed", 42);
	ai_media_request_set_image_url(request, "ref.png");

	job_id = submit(client, request, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(job_id, ==, JOB_OK);

	parser = sent_graph("/prompt", &graph);
	g_assert_cmpuint(json_object_get_size(graph), ==, 6);
	g_assert_false(json_object_has_member(graph, "evil"));

	/* The prompt is one string value, exactly as given, and not rescanned. */
	expected = g_strdup_printf("portrait of %s, {{style}}", PROMPT);
	g_assert_cmpstr(json_object_get_string_member(node_inputs(graph, "6"), "text"), ==, expected);

	/* A whole-input seed is a number; a seed inside text is text. */
	seed = json_object_get_member(node_inputs(graph, "3"), "seed");
	g_assert_cmpint(json_node_get_value_type(seed), ==, G_TYPE_INT64);
	g_assert_cmpint(json_node_get_int(seed), ==, 42);
	g_assert_cmpstr(json_object_get_string_member(node_inputs(graph, "9"), "filename_prefix"),
	                ==, "seed-42");

	/* Absent negative prompt is empty; the image is the uploaded name. */
	g_assert_cmpstr(json_object_get_string_member(node_inputs(graph, "7"), "text"), ==, "");
	g_assert_cmpstr(json_object_get_string_member(node_inputs(graph, "8"), "image"), ==, "ref.png");

	/* Links and numbers are untouched. */
	g_assert_cmpint(json_object_get_int_member(node_inputs(graph, "3"), "steps"), ==, 20);
	g_assert_true(JSON_NODE_HOLDS_ARRAY(json_object_get_member(node_inputs(graph, "3"), "model")));

	/* The stored workflow keeps its placeholders for the next request. */
	g_assert_nonnull(strstr(ai_comfyui_client_get_workflow(client), "{{prompt}}"));
}

static void
test_seed_forms(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW_TEXT);
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "x");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *id2 = NULL;
	gint64 first;
	gint64 second;

	/* No seed: random, a number, below 2^53, and new each time. */
	{
		g_autoptr(JsonParser) parser = NULL;
		JsonObject *graph;
		JsonNode *seed;

		id = submit(client, request, &error);
		g_assert_no_error(error);
		parser = sent_graph("/prompt", &graph);
		seed = json_object_get_member(node_inputs(graph, "3"), "seed");
		g_assert_cmpint(json_node_get_value_type(seed), ==, G_TYPE_INT64);
		first = json_node_get_int(seed);
		g_assert_cmpint(first, >=, 0);
		g_assert_cmpint(first, <, G_GINT64_CONSTANT(1) << 53);
	}
	{
		g_autoptr(JsonParser) parser = NULL;
		JsonObject *graph;

		id2 = submit(client, request, &error);
		g_assert_no_error(error);
		parser = sent_graph("/prompt", &graph);
		second = json_object_get_int_member(node_inputs(graph, "3"), "seed");
		g_assert_cmpint(first, !=, second);
	}

	/* A seed given as digits. */
	ai_media_request_set_param_string(request, "seed", "123");
	{
		g_autoptr(JsonParser) parser = NULL;
		g_autofree gchar *id3 = submit(client, request, &error);
		JsonObject *graph;

		g_assert_no_error(error);
		parser = sent_graph("/prompt", &graph);
		g_assert_cmpint(json_object_get_int_member(node_inputs(graph, "3"), "seed"), ==, 123);
	}

	/* Not a seed. */
	ai_media_request_set_param_string(request, "seed", "-1");
	{
		g_autofree gchar *id4 = submit(client, request, &error);

		g_assert_null(id4);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_clear_error(&error);
	}
}

static void
test_refused_locally(void)
{
	g_autoptr(AiComfyuiClient) bare = new_client(NULL);
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW);
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "x");
	g_autoptr(AiMediaRequest) no_prompt = ai_media_request_new(NULL, NULL);
	g_autoptr(GError) error = NULL;
	guint hits = mock_hits();
	gchar *id;

	/* No workflow anywhere. */
	id = submit(bare, request, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);
	g_clear_error(&error);

	/* {{image}} with no image. */
	id = submit(client, request, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "{{image}}"));
	g_clear_error(&error);

	/* A URL is not a file on the server: the client will not fetch it. */
	ai_media_request_set_image_url(request, "https://elsewhere.example/a.png");
	id = submit(client, request, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	ai_media_request_set_image_url(request, "../../etc/passwd");
	id = submit(client, request, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* {{prompt}} with no prompt. */
	ai_media_request_set_image_url(no_prompt, "ref.png");
	id = submit(client, no_prompt, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "{{prompt}}"));
	g_clear_error(&error);

	/* A request workflow that is not one. */
	ai_media_request_set_param_int(no_prompt, "workflow", 3);
	id = submit(client, no_prompt, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* A prompt id that would leave the path. */
	{
		g_autoptr(AiMediaJob) job = poll_once(client, "../queue", &error);

		g_assert_null(job);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_clear_error(&error);
	}

	g_assert_cmpuint(mock_hits(), ==, hits);
}

static void
test_request_workflow(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW);
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "override");
	g_autoptr(JsonParser) workflow = json_parser_new();
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	JsonObject *graph;

	/* The request's own graph wins, and needs no image. */
	g_assert_true(json_parser_load_from_data(workflow, WORKFLOW_TEXT, -1, NULL));
	ai_media_request_set_param_json(request, "workflow", json_parser_get_root(workflow));

	id = submit(client, request, &error);
	g_assert_no_error(error);
	parser = sent_graph("/prompt", &graph);
	g_assert_cmpuint(json_object_get_size(graph), ==, 3);
	g_assert_cmpstr(json_object_get_string_member(node_inputs(graph, "6"), "text"), ==, "override");
}

static void
test_submit_refused(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(
		"{\"5\":{\"class_type\":\"Broken\",\"inputs\":{\"ckpt_name\":\"nope.safetensors\"}},"
		"\"9\":{\"class_type\":\"SaveImage\",\"inputs\":{\"images\":[\"5\",0]}}}");
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "x");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;

	id = submit(client, request, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "HTTP 400"));
	g_assert_nonnull(strstr(error->message, "Prompt outputs failed validation"));
	g_assert_nonnull(strstr(error->message, "node 5 (Broken): Value not in list: ckpt_name: "
	                                        "'nope.safetensors' not in []"));
}

static void
test_poll_transitions(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) first = NULL;
	g_autoptr(AiMediaJob) second = NULL;
	g_autoptr(AiMediaJob) third = NULL;
	g_autofree gchar *url0 = NULL;
	g_autofree gchar *url1 = NULL;
	const gchar * const *urls;

	mock_reset_polls(JOB_OK);

	first = poll_once(client, JOB_OK, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(first), ==, AI_MEDIA_JOB_STATE_QUEUED);
	g_assert_cmpstr(ai_media_job_get_provider_status(first), ==, "pending");
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(first), ==, 0);

	second = poll_once(client, JOB_OK, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(second), ==, AI_MEDIA_JOB_STATE_RUNNING);

	third = poll_once(client, JOB_OK, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(third), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpstr(ai_media_job_get_provider_status(third), ==, "success");
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(third), ==, 0);

	/* Node order (9 before 12), each file once, every query part escaped. */
	g_assert_cmpuint(ai_media_job_get_n_result_urls(third), ==, 2);
	urls = ai_media_job_get_result_urls(third);
	url0 = g_strdup_printf("%s/view?filename=a.png&subfolder=&type=output", mock->base_url);
	url1 = g_strdup_printf("%s/view?filename=b%%20%%281%%29.png&subfolder=sub%%2Fdir&type=temp",
	                       mock->base_url);
	g_assert_cmpstr(urls[0], ==, url0);
	g_assert_cmpstr(urls[1], ==, url1);
	g_assert_null(urls[2]);
}

static void
test_wait_done(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;

	mock_reset_polls(JOB_OK);
	ai_media_generator_wait_async(AI_MEDIA_GENERATOR(client), JOB_OK, 5, 20, 0, NULL,
	                              store_result, &result);
	run_until(&result);
	job = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(client), result, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpuint(mock_polls(JOB_OK), ==, 3);
}

static gboolean
tick_cb(gpointer user_data)
{
	(void)user_data;
	return G_SOURCE_CONTINUE;
}

/* Started on a private context with the default one idle, it must still finish. */
static void
test_wait_private_context(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	GSource *tick;
	gint64 deadline;

	mock_reset_polls(JOB_OK);

	g_main_context_push_thread_default(context);
	ai_media_generator_wait_async(AI_MEDIA_GENERATOR(client), JOB_OK, 5, 20, 0, NULL,
	                              store_result, &result);
	tick = g_timeout_source_new(100);
	g_source_set_callback(tick, tick_cb, NULL, NULL);
	g_source_attach(tick, context);
	deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
	while (result == NULL && g_get_monotonic_time() < deadline)
	{
		g_main_context_iteration(context, TRUE);
	}
	g_source_destroy(tick);
	g_source_unref(tick);
	g_main_context_pop_thread_default(context);

	g_assert_nonnull(result);
	job = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(client), result, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
}

static void
test_poll_race_and_unknown(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) raced = NULL;
	g_autoptr(AiMediaJob) unknown = NULL;

	/* Not in history, not in the queue, then in history: finished in between. */
	raced = poll_once(client, JOB_RACE, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(raced), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpuint(mock_polls(JOB_RACE), ==, 2);

	unknown = poll_once(client, JOB_UNKNOWN, &error);
	g_assert_null(unknown);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, JOB_UNKNOWN));
}

static void
test_failed_states(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) failed = NULL;
	g_autoptr(AiMediaJob) stopped = NULL;
	g_autoptr(AiMediaJob) empty = NULL;

	/* A failed prompt is a result, with ComfyUI's own words. */
	failed = poll_once(client, JOB_FAIL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(failed), ==, AI_MEDIA_JOB_STATE_FAILED);
	g_assert_cmpstr(ai_media_job_get_provider_status(failed), ==, "error");
	g_assert_cmpstr(ai_media_job_get_error_message(failed), ==,
	                "node 3 (KSampler): RuntimeError: CUDA out of memory");
	g_assert_cmpuint(ai_media_job_get_n_result_urls(failed), ==, 0);

	stopped = poll_once(client, JOB_STOPPED, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(stopped), ==, AI_MEDIA_JOB_STATE_CANCELLED);
	g_assert_cmpstr(ai_media_job_get_provider_status(stopped), ==, "interrupted");

	/* Success with nothing to download is not a result. */
	empty = poll_once(client, JOB_EMPTY, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(empty), ==, AI_MEDIA_JOB_STATE_FAILED);
	g_assert_nonnull(strstr(ai_media_job_get_error_message(empty), "SaveImage"));
}

static void
test_download(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GBytes) missing = NULL;
	g_autofree gchar *url = g_strdup_printf("%s/view?filename=a.png&subfolder=&type=output",
	                                        mock->base_url);
	g_autofree gchar *gone = g_strdup_printf("%s/view?filename=gone.png&subfolder=&type=output",
	                                         mock->base_url);
	gsize length = 0;
	const gchar *data;

	bytes = download(client, url, &error);
	g_assert_no_error(error);
	data = g_bytes_get_data(bytes, &length);
	g_assert_cmpuint(length, ==, 9);
	g_assert_cmpmem(data, length, "PNGDATA\0X", 9);

	missing = download(client, gone, &error);
	g_assert_null(missing);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "HTTP 404"));
}

static void
test_download_refused(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(AiComfyuiClient) prefixed = ai_comfyui_client_new(NULL);
	g_autoptr(GUri) base = g_uri_parse(mock->base_url, G_URI_FLAGS_NONE, NULL);
	g_autofree gchar *other_host = g_strdup_printf("http://127.0.0.2:%d/view?filename=a.png",
	                                               g_uri_get_port(base));
	g_autofree gchar *other_port = g_strdup_printf("http://127.0.0.1:%d/view?filename=a.png",
	                                               g_uri_get_port(base) + 1);
	g_autofree gchar *https = g_strdup_printf("https://127.0.0.1:%d/view?filename=a.png",
	                                          g_uri_get_port(base));
	g_autofree gchar *userinfo = g_strdup_printf("http://user:pw@127.0.0.1:%d/view?filename=a.png",
	                                             g_uri_get_port(base));
	g_autofree gchar *dots = g_strdup_printf("%s/locked/../view?filename=a.png", mock->base_url);
	g_autofree gchar *encoded = g_strdup_printf("%s/locked/%%2e%%2e/view?filename=a.png",
	                                            mock->base_url);
	g_autofree gchar *sibling = g_strdup_printf("%s/lockedx/view?filename=a.png", mock->base_url);
	g_autofree gchar *prefix_base = g_strdup_printf("%s/locked", mock->base_url);
	const gchar *refused[] = { other_host, other_port, https, userinfo, "file:///etc/passwd",
	                           "not a url", NULL };
	const gchar *refused_prefixed[] = { dots, encoded, sibling, NULL };
	guint hits = mock_hits();
	guint i;

	for (i = 0; refused[i] != NULL; i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GBytes) bytes = download(client, refused[i], &error);

		g_assert_null(bytes);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_nonnull(strstr(error->message, "not under the ComfyUI base URL"));
	}

	ai_comfyui_client_set_base_url(prefixed, prefix_base);
	for (i = 0; refused_prefixed[i] != NULL; i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(GBytes) bytes = download(prefixed, refused_prefixed[i], &error);

		g_assert_null(bytes);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	}

	/* Refused before a byte went anywhere. */
	g_assert_cmpuint(mock_hits(), ==, hits);
}

static void
test_upload(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW);
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) data = g_bytes_new_static("\x89PNG\r\n\x1a\n\0rest", 13);
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "with a reference");
	g_autoptr(JsonParser) parser = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *id = NULL;
	JsonObject *graph;

	ai_comfyui_client_upload_async(client, data, "reference.png", NULL, store_result, &result);
	run_until(&result);
	name = ai_comfyui_client_upload_finish(client, result, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(name, ==, "ref.png");

	g_mutex_lock(&mock->lock);
	g_assert_cmpstr(mock->upload_filename, ==, "reference.png");
	g_assert_cmpstr(mock->upload_type, ==, "image/png");
	g_assert_cmpstr(mock->upload_overwrite, ==, "true");
	g_assert_nonnull(mock->upload_bytes);
	g_assert_cmpmem(g_bytes_get_data(mock->upload_bytes, NULL), g_bytes_get_size(mock->upload_bytes),
	                "\x89PNG\r\n\x1a\n\0rest", 13);
	g_mutex_unlock(&mock->lock);

	/* The returned name is what {{image}} takes. */
	ai_media_request_set_image_url(request, name);
	id = submit(client, request, &error);
	g_assert_no_error(error);
	parser = sent_graph("/prompt", &graph);
	g_assert_cmpstr(json_object_get_string_member(node_inputs(graph, "8"), "image"), ==, "ref.png");

	/* A name with a directory part never reaches the server. */
	{
		g_autoptr(GAsyncResult) bad = NULL;
		g_autofree gchar *refused = NULL;
		guint hits = mock_hits();

		ai_comfyui_client_upload_async(client, data, "../evil.png", NULL, store_result, &bad);
		run_until(&bad);
		refused = ai_comfyui_client_upload_finish(client, bad, &error);
		g_assert_null(refused);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_assert_cmpuint(mock_hits(), ==, hits);
	}
}

static void
test_token(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW_TEXT);
	g_autoptr(AiComfyuiClient) wrong = new_client(WORKFLOW_TEXT);
	g_autoptr(AiComfyuiClient) none = new_client(NULL);
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "behind a proxy");
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *base = g_strdup_printf("%s/locked", mock->base_url);
	g_autofree gchar *view = g_strdup_printf("%s/view?filename=a.png&subfolder=&type=output", base);
	g_autofree gchar *auth = NULL;
	g_autofree gchar *view_auth = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *wrong_id = NULL;
	g_autofree gchar *wrong_token = g_strdup(TEST_TOKEN "-but-wrong");

	ai_comfyui_client_set_base_url(client, base);
	ai_comfyui_client_set_token(client, TEST_TOKEN);

	/* Under a path prefix, with the bearer token on the API and on /view. */
	id = submit(client, request, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(id, ==, JOB_OK);
	auth = mock_header("/locked/prompt", "authorization");
	g_assert_cmpstr(auth, ==, "Bearer " TEST_TOKEN);

	bytes = download(client, view, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(g_bytes_get_size(bytes), ==, 9);
	view_auth = mock_header("/locked/view", "authorization");
	g_assert_cmpstr(view_auth, ==, "Bearer " TEST_TOKEN);

	/* A refusing proxy echoes the header; the error does not. */
	ai_comfyui_client_set_base_url(wrong, base);
	ai_comfyui_client_set_token(wrong, wrong_token);
	wrong_id = submit(wrong, request, &error);
	g_assert_null(wrong_id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_assert_nonnull(strstr(error->message, "HTTP 401"));
	g_assert_nonnull(strstr(error->message, "[REDACTED]"));
	g_assert_null(strstr(error->message, TEST_TOKEN));
	g_assert_nonnull(strstr(error->message, "ai_comfyui_client_set_token"));
	g_clear_error(&error);

	/* Without a token, no Authorization header at all. */
	{
		g_autoptr(AiMediaJob) job = poll_once(none, JOB_FAIL, &error);
		g_autofree gchar *none_auth = mock_header("/history/" JOB_FAIL, "authorization");

		g_assert_no_error(error);
		g_assert_null(none_auth);
	}
}

static void
test_bounded(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	g_autofree gchar *base = g_strdup_printf("%s/huge", mock->base_url);

	ai_comfyui_client_set_base_url(client, base);
	job = poll_once(client, JOB_OK, &error);
	g_assert_null(job);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	g_assert_nonnull(strstr(error->message, "larger than"));
}

static void
test_estimate(void)
{
	g_autoptr(AiComfyuiClient) client = new_client(WORKFLOW_TEXT);
	g_autoptr(AiComfyuiClient) bare = new_client(NULL);
	g_autoptr(AiMediaRequest) request = ai_media_request_new(NULL, "x");
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GAsyncResult) refused = NULL;
	g_autoptr(GError) error = NULL;
	gint64 micro_usd = -7;
	guint hits = mock_hits();

	/* Zero: no charge, not unknown. */
	ai_media_generator_estimate_async(AI_MEDIA_GENERATOR(client), request, NULL,
	                                  store_result, &result);
	run_until(&result);
	g_assert_true(ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(client), result,
	                                                 &micro_usd, &error));
	g_assert_no_error(error);
	g_assert_cmpint(micro_usd, ==, 0);
	g_assert_cmpint(micro_usd, !=, AI_MEDIA_COST_UNKNOWN);

	/* What a submit would refuse, an estimate refuses. */
	ai_media_generator_estimate_async(AI_MEDIA_GENERATOR(bare), request, NULL,
	                                  store_result, &refused);
	run_until(&refused);
	g_assert_false(ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(bare), refused,
	                                                  &micro_usd, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR);

	/* Nothing went over the wire for a price. */
	g_assert_cmpuint(mock_hits(), ==, hits);
}

/* Last: everything above has logged what it was going to log. */
static void
test_logs_clean(void)
{
	g_mutex_lock(&log_lock);
	g_assert_null(strstr(log_text->str, TEST_TOKEN));
	/* The capture is live: the client did log its requests at debug. */
	g_assert_nonnull(strstr(log_text->str, "comfyui:"));
	g_mutex_unlock(&log_lock);
}

/*
 * Captures every log line, at every level and from every domain, then lets
 * warnings and criticals through to the default writer so they still abort.
 */
static GLogWriterOutput
capture_writer(
	GLogLevelFlags   log_level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	gsize i;

	g_mutex_lock(&log_lock);
	for (i = 0; i < n_fields; i++)
	{
		if (fields[i].length < 0)
		{
			g_string_append(log_text, (const gchar *)fields[i].value);
		}
		else
		{
			g_string_append_len(log_text, (const gchar *)fields[i].value, fields[i].length);
		}
		g_string_append_c(log_text, ' ');
	}
	g_string_append_c(log_text, '\n');
	g_mutex_unlock(&log_lock);

	if (log_level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
	{
		return G_LOG_WRITER_HANDLED;
	}
	return g_log_writer_default(log_level, fields, n_fields, user_data);
}

/* GTest routes g_log() through its own default handler; capture that path too. */
static GLogFunc previous_handler;

static void
capture_handler(
	const gchar    *log_domain,
	GLogLevelFlags  log_level,
	const gchar    *message,
	gpointer        user_data
){
	g_mutex_lock(&log_lock);
	g_string_append_printf(log_text, "%s %s\n",
	                       (log_domain != NULL) ? log_domain : "", message);
	g_mutex_unlock(&log_lock);

	if (log_level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
	{
		return;
	}
	previous_handler(log_domain, log_level, message, user_data);
}

int
main(
	int    argc,
	char **argv
){
	int status;

	/* Debug output must be produced, or the log check would pass vacuously. */
	g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
	log_text = g_string_new(NULL);
	g_log_set_writer_func(capture_writer, NULL, NULL);

	g_test_init(&argc, &argv, NULL);
	previous_handler = g_log_set_default_handler(capture_handler, NULL);
	mock_start();

	g_test_add_func("/comfyui/defaults", test_defaults);
	g_test_add_func("/comfyui/workflow-validation", test_workflow_validation);
	g_test_add_func("/comfyui/placeholders", test_placeholders);
	g_test_add_func("/comfyui/seed-forms", test_seed_forms);
	g_test_add_func("/comfyui/refused-locally", test_refused_locally);
	g_test_add_func("/comfyui/request-workflow", test_request_workflow);
	g_test_add_func("/comfyui/submit-refused", test_submit_refused);
	g_test_add_func("/comfyui/poll-transitions", test_poll_transitions);
	g_test_add_func("/comfyui/wait-done", test_wait_done);
	g_test_add_func("/comfyui/wait-private-context", test_wait_private_context);
	g_test_add_func("/comfyui/poll-race-and-unknown", test_poll_race_and_unknown);
	g_test_add_func("/comfyui/failed-states", test_failed_states);
	g_test_add_func("/comfyui/download", test_download);
	g_test_add_func("/comfyui/download-refused", test_download_refused);
	g_test_add_func("/comfyui/upload", test_upload);
	g_test_add_func("/comfyui/token", test_token);
	g_test_add_func("/comfyui/bounded", test_bounded);
	g_test_add_func("/comfyui/estimate", test_estimate);
	g_test_add_func("/comfyui/logs-clean", test_logs_clean);

	status = g_test_run();

	mock_stop();
	g_string_free(log_text, TRUE);
	return status;
}
