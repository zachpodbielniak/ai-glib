/*
 * test-higgsfield-client.c - AiHiggsfieldClient and AiMediaGenerator
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Hermetic: every request goes to a loopback SoupServer on its own thread
 * that imitates the Higgsfield API -- submit, the status transitions, the
 * estimate, cancel, the upload handshake, a CDN, and the error envelopes.
 * Nothing leaves the machine.
 *
 * The credential is checked negatively throughout: the mock echoes it
 * back in a 401's detail, the way a careless server might, and every error
 * string and every log line the run produced must not contain it.
 */

#include <string.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "ai-glib.h"

#define TEST_KEY_ID     "hfkid-test-0123456789"
#define TEST_KEY_SECRET "hfsec-SHOULD-NEVER-APPEAR-9876543210"
#define TEST_AUTH       "Key " TEST_KEY_ID ":" TEST_KEY_SECRET

#define JOB_OK      "11111111-2222-3333-4444-555555555555"
#define JOB_FAIL    "22222222-2222-3333-4444-555555555555"
#define JOB_NSFW    "33333333-2222-3333-4444-555555555555"
#define JOB_FLAKY   "44444444-2222-3333-4444-555555555555"
#define JOB_STUCK   "55555555-2222-3333-4444-555555555555"
#define JOB_QUEUED  "66666666-2222-3333-4444-555555555555"
#define JOB_UNKNOWN "77777777-2222-3333-4444-555555555555"

/* Every log line the process wrote, so the last test can search it. */
static GMutex   log_lock;
static GString *log_text;

/* ---------------------------------------------------------------------- */
/* The mock API                                                           */
/* ---------------------------------------------------------------------- */

typedef struct
{
	SoupServer   *server;
	GMainContext *context;
	GMainLoop    *loop;
	GThread      *thread;
	gchar        *base_url;
	GMutex        lock;

	GHashTable   *polls;          /* job id -> GUINT_TO_POINTER(count) */
	GHashTable   *headers;        /* path -> GHashTable(lower name -> value) */
	GHashTable   *bodies;         /* path -> body text */
	GHashTable   *queries;        /* path -> query text */
	guint         hits;
} MockHf;

static MockHf *mock;

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

/* What a job's Nth status poll answers. */
static void
mock_status(
	SoupServerMessage *msg,
	const gchar       *job_id,
	guint              n
){
	g_autofree gchar *body = NULL;

	if (g_strcmp0(job_id, JOB_OK) == 0)
	{
		if (n == 1)
			body = g_strdup_printf("{\"status\":\"queued\",\"request_id\":\"%s\"}", job_id);
		else if (n == 2)
			body = g_strdup_printf("{\"status\":\"in_progress\",\"request_id\":\"%s\"}", job_id);
		else
			body = g_strdup_printf(
				"{\"status\":\"completed\",\"request_id\":\"%s\","
				"\"expires_at\":\"2026-11-28T12:00:00Z\","
				"\"images\":[{\"url\":\"%s/cdn/a.png\"},{\"url\":\"%s/cdn/b.png\"}],"
				"\"audio\":{\"url\":\"%s/cdn/c.wav\"},"
				"\"audios\":[{\"url\":\"%s/cdn/c.wav\"}]}",
				job_id, mock->base_url, mock->base_url, mock->base_url, mock->base_url);
	}
	else if (g_strcmp0(job_id, JOB_FAIL) == 0)
	{
		body = g_strdup_printf("{\"status\":\"failed\",\"request_id\":\"%s\","
		                       "\"error\":\"Generation failed\"}", job_id);
	}
	else if (g_strcmp0(job_id, JOB_NSFW) == 0)
	{
		body = g_strdup_printf("{\"status\":\"nsfw\",\"request_id\":\"%s\",\"error\":null}", job_id);
	}
	else if (g_strcmp0(job_id, JOB_FLAKY) == 0)
	{
		if (n <= 2)
		{
			mock_reply(msg, 503, "{\"detail\":\"admission_control_unavailable\"}");
			return;
		}
		body = g_strdup_printf("{\"status\":\"completed\",\"request_id\":\"%s\","
		                       "\"video\":{\"url\":\"%s/cdn/v.mp4\"}}", job_id, mock->base_url);
	}
	else if (g_strcmp0(job_id, JOB_STUCK) == 0 || g_strcmp0(job_id, JOB_QUEUED) == 0)
	{
		body = g_strdup_printf("{\"status\":\"queued\",\"request_id\":\"%s\"}", job_id);
	}
	else if (g_strcmp0(job_id, JOB_UNKNOWN) == 0)
	{
		body = g_strdup_printf("{\"status\":\"teleporting\",\"request_id\":\"%s\"}", job_id);
	}
	else
	{
		mock_reply(msg, 404, "{\"detail\":\"Request not found\"}");
		return;
	}

	mock_reply(msg, 200, body);
}

static void
mock_handler(
	SoupServer        *server,
	SoupServerMessage *msg,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	SoupMessageHeaders *request_headers = soup_server_message_get_request_headers(msg);
	SoupMessageBody *request_body = soup_server_message_get_request_body(msg);
	const gchar *method = soup_server_message_get_method(msg);
	const gchar *auth = soup_message_headers_get_one(request_headers, "Authorization");
	GHashTable *seen;
	SoupMessageHeadersIter iter;
	const char *name;
	const char *value;
	GUri *uri;

	(void)server;
	(void)query;
	(void)user_data;

	/* Record what arrived, per path. */
	g_mutex_lock(&mock->lock);
	mock->hits++;
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	soup_message_headers_iter_init(&iter, request_headers);
	while (soup_message_headers_iter_next(&iter, &name, &value))
	{
		g_hash_table_insert(seen, g_ascii_strdown(name, -1), g_strdup(value));
	}
	g_hash_table_insert(mock->headers, g_strdup(path), seen);
	g_hash_table_insert(mock->bodies, g_strdup(path),
	                    g_strndup(request_body->data, request_body->length));
	uri = soup_server_message_get_uri(msg);
	g_hash_table_insert(mock->queries, g_strdup(path),
	                    g_strdup(g_uri_get_query(uri) != NULL ? g_uri_get_query(uri) : ""));
	g_mutex_unlock(&mock->lock);

	/* Storage and CDN: no credential expected (and none must arrive). */
	if (g_str_has_prefix(path, "/cdn/"))
	{
		soup_server_message_set_status(msg, 200, NULL);
		soup_server_message_set_response(msg, "image/png", SOUP_MEMORY_STATIC,
		                                 "PNGDATA\0X", 9);
		return;
	}
	if (g_strcmp0(path, "/storage/put") == 0 && g_strcmp0(method, "PUT") == 0)
	{
		soup_server_message_set_status(msg, 200, NULL);
		return;
	}

	/* Everything else is the API, and wants the key -- a 401 echoes it back. */
	if (g_strcmp0(auth, TEST_AUTH) != 0)
	{
		g_autofree gchar *body = g_strdup_printf("{\"detail\":\"Invalid credentials: %s\"}",
		                                         auth != NULL ? auth : "(none)");

		mock_reply(msg, 401, body);
		return;
	}

	if (g_strcmp0(method, "POST") == 0 && g_strcmp0(path, "/higgsfield-ai/soul/v2/standard") == 0)
	{
		g_autofree gchar *body = g_strdup_printf(
			"{\"status\":\"queued\",\"request_id\":\"%s\","
			"\"status_url\":\"https://elsewhere.invalid/requests/%s/status\","
			"\"cancel_url\":\"https://elsewhere.invalid/requests/%s/cancel\"}",
			JOB_OK, JOB_OK, JOB_OK);

		mock_reply(msg, 200, body);
		return;
	}
	if (g_strcmp0(path, "/estimate/higgsfield-ai/soul/v2/standard") == 0)
	{
		mock_reply(msg, 200, "{\"usd\":\"0.094\"}");
		return;
	}
	if (g_strcmp0(path, "/vendor/needs-prompt") == 0)
	{
		mock_reply(msg, 422, "{\"detail\":[{\"loc\":[\"body\",\"prompt\"],"
		                     "\"msg\":\"Field required\",\"type\":\"missing\"}]}");
		return;
	}
	if (g_strcmp0(path, "/vendor/broke") == 0)
	{
		mock_reply(msg, 403, "{\"detail\":\"Insufficient balance\"}");
		return;
	}
	if (g_strcmp0(path, "/vendor/busy") == 0)
	{
		soup_message_headers_replace(soup_server_message_get_response_headers(msg),
		                             "Retry-After", "7");
		mock_reply(msg, 429, "{\"detail\":\"account_queue_full\"}");
		return;
	}
	if (g_strcmp0(path, "/vendor/redirect") == 0)
	{
		soup_server_message_set_redirect(msg, 302, "http://127.0.0.2:1/stolen");
		return;
	}
	if (g_strcmp0(path, "/files/generate-upload-url") == 0)
	{
		g_autofree gchar *body = g_strdup_printf(
			"{\"public_url\":\"%s/cdn/uploaded.png\",\"upload_url\":\"%s/storage/put\","
			"\"content_type\":\"image/png\",\"upload_headers\":{\"Content-Type\":\"image/png\","
			"\"x-amz-tagging\":\"retention=temporary\",\"Authorization\":\"Bearer nope\"}}",
			mock->base_url, mock->base_url);

		mock_reply(msg, 200, body);
		return;
	}
	if (g_str_has_prefix(path, "/requests/") && g_str_has_suffix(path, "/status"))
	{
		g_autofree gchar *job_id = g_strndup(path + strlen("/requests/"),
		                                     strlen(path) - strlen("/requests/") - strlen("/status"));
		guint n;

		g_mutex_lock(&mock->lock);
		n = GPOINTER_TO_UINT(g_hash_table_lookup(mock->polls, job_id)) + 1;
		g_hash_table_insert(mock->polls, g_strdup(job_id), GUINT_TO_POINTER(n));
		g_mutex_unlock(&mock->lock);

		mock_status(msg, job_id, n);
		return;
	}
	if (g_str_has_prefix(path, "/requests/") && g_str_has_suffix(path, "/cancel"))
	{
		if (g_str_has_prefix(path, "/requests/" JOB_QUEUED))
		{
			soup_server_message_set_status(msg, 202, NULL);
			return;
		}
		mock_reply(msg, 400, "{\"detail\":\"Request has already started\"}");
		return;
	}

	mock_reply(msg, 404, "{\"detail\":\"Not found\"}");
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
 * Starts the server on its own thread and context.  The client's own
 * operations run on the test thread's context, so the two never share a
 * loop -- which is the arrangement that catches a source attached to the
 * wrong one.
 */
static void
mock_start(void)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;
	gchar *full;

	mock = g_new0(MockHf, 1);
	g_mutex_init(&mock->lock);
	mock->polls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	mock->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                      (GDestroyNotify)g_hash_table_unref);
	mock->bodies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	mock->queries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
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

	mock->thread = g_thread_new("mock-higgsfield", mock_thread, NULL);
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
	g_hash_table_unref(mock->queries);
	g_free(mock->base_url);
	g_mutex_clear(&mock->lock);
	g_free(mock);
}

/* A recorded request header, or NULL. */
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
mock_lookup(
	GHashTable  *table,
	const gchar *path
){
	gchar *value;

	g_mutex_lock(&mock->lock);
	value = g_strdup(g_hash_table_lookup(table, path));
	g_mutex_unlock(&mock->lock);
	return value;
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

static AiHiggsfieldClient *
new_client(void)
{
	AiHiggsfieldClient *client = ai_higgsfield_client_new_with_credentials(TEST_KEY_ID,
	                                                                       TEST_KEY_SECRET);

	ai_higgsfield_client_set_base_url(client, mock->base_url);
	ai_higgsfield_client_set_timeout(client, 10);
	return client;
}

/* The negative check: the credential is nowhere in @text. */
static void
assert_no_credential(const gchar *text)
{
	g_assert_nonnull(text);
	g_assert_null(strstr(text, TEST_KEY_SECRET));
	g_assert_null(strstr(text, TEST_KEY_ID));
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
	AiHiggsfieldClient  *client,
	AiMediaRequest      *request,
	GError             **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_submit_async(AI_MEDIA_GENERATOR(client), request, NULL,
	                                store_result, &result);
	run_until(&result);
	return ai_media_generator_submit_finish(AI_MEDIA_GENERATOR(client), result, error);
}

static AiMediaJob *
poll_once(
	AiHiggsfieldClient  *client,
	const gchar         *job_id,
	GError             **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_poll_async(AI_MEDIA_GENERATOR(client), job_id, NULL,
	                              store_result, &result);
	run_until(&result);
	return ai_media_generator_poll_finish(AI_MEDIA_GENERATOR(client), result, error);
}

static AiMediaJob *
wait_for(
	AiHiggsfieldClient  *client,
	const gchar         *job_id,
	guint                timeout_ms,
	GCancellable        *cancellable,
	GError             **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_wait_async(AI_MEDIA_GENERATOR(client), job_id, 5, 20, timeout_ms,
	                              cancellable, store_result, &result);
	run_until(&result);
	return ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(client), result, error);
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

static void
test_request_json(void)
{
	g_autoptr(AiMediaRequest) request = ai_media_request_new(AI_HIGGSFIELD_MODEL_KLING_2_5_TURBO_PRO_I2V,
	                                                         "A heron takes off");
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *json = NULL;
	JsonObject *body;
	const gchar *key;

	ai_media_request_set_image_url(request, "https://example.com/heron.jpg");
	ai_media_request_set_param_int(request, "duration", 5);
	ai_media_request_set_param_double(request, "cfg_scale", 0.5);
	ai_media_request_set_param_boolean(request, "enhance", TRUE);
	ai_media_request_set_param_string(request, "prompt", "overridden by the property");
	g_assert_true(ai_media_request_has_param(request, "duration"));

	json = ai_media_request_to_json(request);
	g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
	body = json_node_get_object(json_parser_get_root(parser));
	g_assert_cmpstr(json_object_get_string_member(body, "prompt"), ==, "A heron takes off");
	g_assert_cmpstr(json_object_get_string_member(body, "image_url"), ==,
	                "https://example.com/heron.jpg");
	g_assert_cmpint(json_object_get_int_member(body, "duration"), ==, 5);
	g_assert_true(json_object_get_boolean_member(body, "enhance"));

	/* The model is the path, never a body field. */
	g_assert_false(json_object_has_member(body, "model"));

	ai_media_request_set_image_url(request, NULL);
	g_assert_false(ai_media_request_has_param(request, "image_url"));

	/* Generated once, then kept: that is what makes a resubmit safe. */
	g_assert_null(ai_media_request_get_idempotency_key(request));
	key = ai_media_request_ensure_idempotency_key(request);
	g_assert_nonnull(key);
	g_assert_cmpstr(ai_media_request_ensure_idempotency_key(request), ==, key);
}

static void
test_job_state(void)
{
	g_autoptr(AiMediaJob) job = ai_media_job_new("id", AI_MEDIA_JOB_STATE_RUNNING);
	GEnumClass *klass;
	guint i;

	g_assert_false(ai_media_job_is_finished(job));
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(job), ==, AI_MEDIA_COST_UNKNOWN);
	g_assert_cmpuint(ai_media_job_get_n_result_urls(job), ==, 0);
	g_assert_null(ai_media_job_get_result_urls(job)[0]);

	/* Walk the enum, so a state added without a classification shows up here. */
	klass = g_type_class_ref(AI_TYPE_MEDIA_JOB_STATE);
	for (i = 0; i < klass->n_values; i++)
	{
		AiMediaJobState state = (AiMediaJobState)klass->values[i].value;
		gboolean terminal = ai_media_job_state_is_finished(state);

		g_assert_cmpstr(ai_media_job_state_to_string(state), ==, klass->values[i].value_nick);
		g_assert_true(terminal == (state != AI_MEDIA_JOB_STATE_QUEUED &&
		                           state != AI_MEDIA_JOB_STATE_RUNNING));
	}
	g_type_class_unref(klass);
}

static void
test_submit(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(AiMediaRequest) request = ai_media_request_new(AI_HIGGSFIELD_MODEL_SOUL_V2,
	                                                         "Editorial portrait in soft daylight");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *job_id = NULL;
	g_autofree gchar *again = NULL;
	g_autofree gchar *auth = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *key_again = NULL;
	g_autofree gchar *expires = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *query = NULL;
	const gchar *path = "/higgsfield-ai/soul/v2/standard";

	ai_media_request_set_param_string(request, "aspect_ratio", "16:9");
	ai_media_request_set_webhook_url(request, "https://example.com/hook?x=1&y=2");
	ai_media_request_set_expires_after(request, 7200);

	job_id = submit(client, request, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(job_id, ==, JOB_OK);

	auth = mock_header(path, "authorization");
	g_assert_cmpstr(auth, ==, TEST_AUTH);
	key = mock_header(path, "idempotency-key");
	g_assert_cmpstr(key, ==, ai_media_request_get_idempotency_key(request));
	expires = mock_header(path, "hf-expires-after");
	g_assert_cmpstr(expires, ==, "7200");

	body = mock_lookup(mock->bodies, path);
	g_assert_nonnull(strstr(body, "\"prompt\":\"Editorial portrait in soft daylight\""));
	g_assert_nonnull(strstr(body, "\"aspect_ratio\":\"16:9\""));

	/* The webhook is one escaped query parameter, not two. */
	query = mock_lookup(mock->queries, path);
	g_assert_cmpstr(query, ==, "hf_webhook=https%3A%2F%2Fexample.com%2Fhook%3Fx%3D1%26y%3D2");

	/* Resubmitting the same request carries the same key. */
	again = submit(client, request, &error);
	g_assert_no_error(error);
	key_again = mock_header(path, "idempotency-key");
	g_assert_cmpstr(key_again, ==, key);
}

static void
test_poll_transitions(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) first = NULL;
	g_autoptr(AiMediaJob) second = NULL;
	g_autoptr(AiMediaJob) third = NULL;
	const gchar * const *urls;
	g_autofree gchar *auth = NULL;

	mock_reset_polls(JOB_OK);

	first = poll_once(client, JOB_OK, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(first), ==, AI_MEDIA_JOB_STATE_QUEUED);
	g_assert_cmpstr(ai_media_job_get_provider_status(first), ==, "queued");

	second = poll_once(client, JOB_OK, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(second), ==, AI_MEDIA_JOB_STATE_RUNNING);
	g_assert_false(ai_media_job_is_finished(second));

	third = poll_once(client, JOB_OK, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(third), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpstr(ai_media_job_get_id(third), ==, JOB_OK);
	g_assert_cmpstr(ai_media_job_get_expires_at(third), ==, "2026-11-28T12:00:00Z");
	g_assert_cmpint(ai_media_job_get_cost_micro_usd(third), ==, AI_MEDIA_COST_UNKNOWN);

	/* Two images, and one audio file listed twice is listed once. */
	g_assert_cmpuint(ai_media_job_get_n_result_urls(third), ==, 3);
	urls = ai_media_job_get_result_urls(third);
	g_assert_true(g_str_has_suffix(urls[0], "/cdn/a.png"));
	g_assert_true(g_str_has_suffix(urls[1], "/cdn/b.png"));
	g_assert_true(g_str_has_suffix(urls[2], "/cdn/c.wav"));
	g_assert_null(urls[3]);

	/* Polled on our base URL, not the status_url the server handed back. */
	auth = mock_header("/requests/" JOB_OK "/status", "authorization");
	g_assert_cmpstr(auth, ==, TEST_AUTH);
}

static void
test_wait_done(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;

	mock_reset_polls(JOB_OK);

	job = wait_for(client, JOB_OK, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpuint(mock_polls(JOB_OK), ==, 3);
}

static void
test_wait_failed_and_nsfw(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) failed = NULL;
	g_autoptr(AiMediaJob) nsfw = NULL;

	/* A failed job is a result, not an error. */
	failed = wait_for(client, JOB_FAIL, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(failed), ==, AI_MEDIA_JOB_STATE_FAILED);
	g_assert_cmpstr(ai_media_job_get_error_message(failed), ==, "Generation failed");
	g_assert_cmpuint(ai_media_job_get_n_result_urls(failed), ==, 0);

	nsfw = wait_for(client, JOB_NSFW, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(nsfw), ==, AI_MEDIA_JOB_STATE_FAILED);
	g_assert_cmpstr(ai_media_job_get_provider_status(nsfw), ==, "nsfw");
	g_assert_nonnull(strstr(ai_media_job_get_error_message(nsfw), "moderation"));
}

static void
test_wait_rides_out_5xx(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;

	job = wait_for(client, JOB_FLAKY, 0, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
	g_assert_cmpuint(mock_polls(JOB_FLAKY), ==, 3);
	g_assert_true(g_str_has_suffix(ai_media_job_get_result_urls(job)[0], "/cdn/v.mp4"));
}

static void
test_wait_timeout(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;

	job = wait_for(client, JOB_STUCK, 150, NULL, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_TIMEOUT);
	g_assert_null(job);
	g_assert_cmpuint(mock_polls(JOB_STUCK), >=, 2);
}

static gboolean
cancel_soon(gpointer user_data)
{
	g_cancellable_cancel(G_CANCELLABLE(user_data));
	return G_SOURCE_REMOVE;
}

static void
test_wait_cancelled(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GCancellable) cancellable = g_cancellable_new();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	gint64 started = g_get_monotonic_time();

	/* A long poll interval: only the cancellation can end this promptly. */
	ai_media_generator_wait_async(AI_MEDIA_GENERATOR(client), JOB_STUCK, 60000, 60000, 0,
	                              cancellable, store_result, &result);
	g_timeout_add(100, cancel_soon, cancellable);
	run_until(&result);
	job = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(client), result, &error);
	g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	g_assert_null(job);
	g_assert_cmpint(g_get_monotonic_time() - started, <, 5 * G_USEC_PER_SEC);
}

static gboolean
tick_cb(gpointer user_data)
{
	(void)user_data;
	return G_SOURCE_CONTINUE;
}

/*
 * The wait captures the context it was started on.  Start it on a private
 * context with the default one idle, and the result must still arrive --
 * which it would not if a timer or the HTTP session took the global
 * default.
 */
static void
test_wait_private_context(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;
	gint64 deadline;

	mock_reset_polls(JOB_OK);

	g_main_context_push_thread_default(context);
	ai_media_generator_wait_async(AI_MEDIA_GENERATOR(client), JOB_OK, 5, 20, 0, NULL,
	                              store_result, &result);
	/* A wake-up on the private context, so a lost result fails instead of hanging. */
	{
		GSource *tick = g_timeout_source_new(100);

		g_source_set_callback(tick, tick_cb, NULL, NULL);
		g_source_attach(tick, context);
		deadline = g_get_monotonic_time() + 20 * G_USEC_PER_SEC;
		while (result == NULL && g_get_monotonic_time() < deadline)
		{
			g_main_context_iteration(context, TRUE);
		}
		g_source_destroy(tick);
		g_source_unref(tick);
	}
	g_main_context_pop_thread_default(context);

	g_assert_nonnull(result);
	job = ai_media_generator_wait_finish(AI_MEDIA_GENERATOR(client), result, &error);
	g_assert_no_error(error);
	g_assert_cmpint(ai_media_job_get_state(job), ==, AI_MEDIA_JOB_STATE_DONE);
}

static void
test_unknown_status(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GError) error = NULL;
	g_autoptr(AiMediaJob) job = NULL;

	job = wait_for(client, JOB_UNKNOWN, 0, NULL, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	g_assert_nonnull(strstr(error->message, "teleporting"));
}

static void
test_estimate(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(AiMediaRequest) request = ai_media_request_new(AI_HIGGSFIELD_MODEL_SOUL_V2, "x");
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	gint64 micro_usd = 0;

	g_assert_true(ai_media_generator_can_estimate(AI_MEDIA_GENERATOR(client)));
	ai_media_generator_estimate_async(AI_MEDIA_GENERATOR(client), request, NULL,
	                                  store_result, &result);
	run_until(&result);
	g_assert_true(ai_media_generator_estimate_finish(AI_MEDIA_GENERATOR(client), result,
	                                                 &micro_usd, &error));
	g_assert_no_error(error);
	g_assert_cmpint(micro_usd, ==, 94000);
}

static void
test_cancel(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GAsyncResult) ok_result = NULL;
	g_autoptr(GAsyncResult) late_result = NULL;
	g_autoptr(GError) error = NULL;

	g_assert_true(ai_media_generator_can_cancel(AI_MEDIA_GENERATOR(client)));

	ai_media_generator_cancel_async(AI_MEDIA_GENERATOR(client), JOB_QUEUED, NULL,
	                                store_result, &ok_result);
	run_until(&ok_result);
	g_assert_true(ai_media_generator_cancel_finish(AI_MEDIA_GENERATOR(client), ok_result, &error));
	g_assert_no_error(error);

	ai_media_generator_cancel_async(AI_MEDIA_GENERATOR(client), JOB_OK, NULL,
	                                store_result, &late_result);
	run_until(&late_result);
	g_assert_false(ai_media_generator_cancel_finish(AI_MEDIA_GENERATOR(client), late_result, &error));
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "already started"));
}

static void
test_download(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *url = g_strdup_printf("%s/cdn/a.png", mock->base_url);
	g_autofree gchar *auth = NULL;
	gsize length = 0;
	const gchar *data;

	ai_media_generator_download_async(AI_MEDIA_GENERATOR(client), url, NULL,
	                                  store_result, &result);
	run_until(&result);
	bytes = ai_media_generator_download_finish(AI_MEDIA_GENERATOR(client), result, &error);
	g_assert_no_error(error);

	/* Bytes, not a string: the NUL in the middle survives. */
	data = g_bytes_get_data(bytes, &length);
	g_assert_cmpuint(length, ==, 9);
	g_assert_cmpmem(data, length, "PNGDATA\0X", 9);

	/* The CDN never sees the credential. */
	auth = mock_header("/cdn/a.png", "authorization");
	g_assert_null(auth);
}

static void
test_download_refuses_scheme(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = NULL;

	ai_media_generator_download_async(AI_MEDIA_GENERATOR(client), "file:///etc/passwd", NULL,
	                                  store_result, &result);
	run_until(&result);
	bytes = ai_media_generator_download_finish(AI_MEDIA_GENERATOR(client), result, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_null(bytes);
}

static void
test_upload(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(GAsyncResult) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) data = g_bytes_new_static("\x89PNG-fake", 9);
	g_autofree gchar *public_url = NULL;
	g_autofree gchar *put_auth = NULL;
	g_autofree gchar *put_tagging = NULL;
	g_autofree gchar *put_type = NULL;
	g_autofree gchar *put_body = NULL;
	g_autofree gchar *url_request = NULL;

	ai_higgsfield_client_upload_async(client, data, "image/png", NULL, store_result, &result);
	run_until(&result);
	public_url = ai_higgsfield_client_upload_finish(client, result, &error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_suffix(public_url, "/cdn/uploaded.png"));

	url_request = mock_lookup(mock->bodies, "/files/generate-upload-url");
	g_assert_cmpstr(url_request, ==, "{\"content_type\":\"image/png\"}");

	/* The PUT carries the listed headers and the bytes -- and no credential,
	 * even though the upload headers asked for an Authorization. */
	put_auth = mock_header("/storage/put", "authorization");
	g_assert_null(put_auth);
	put_tagging = mock_header("/storage/put", "x-amz-tagging");
	g_assert_cmpstr(put_tagging, ==, "retention=temporary");
	put_type = mock_header("/storage/put", "content-type");
	g_assert_cmpstr(put_type, ==, "image/png");
	put_body = mock_lookup(mock->bodies, "/storage/put");
	g_assert_cmpmem(put_body, 9, "\x89PNG-fake", 9);
}

static void
test_error_envelopes(void)
{
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(AiHiggsfieldClient) wrong = ai_higgsfield_client_new_with_credentials(TEST_KEY_ID,
	                                                                               TEST_KEY_SECRET "-but-wrong");
	g_autoptr(AiMediaRequest) request = ai_media_request_new("vendor/needs-prompt", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;

	/* A 401 whose detail echoes the header: the error must not. */
	ai_higgsfield_client_set_base_url(wrong, mock->base_url);
	ai_media_request_set_model(request, AI_HIGGSFIELD_MODEL_SOUL_V2);
	id = submit(wrong, request, &error);
	g_assert_null(id);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_assert_nonnull(strstr(error->message, "HTTP 401"));
	g_assert_nonnull(strstr(error->message, "[REDACTED]"));
	assert_no_credential(error->message);
	g_clear_error(&error);

	/* FastAPI's validation list is flattened to field: message. */
	ai_media_request_set_model(request, "vendor/needs-prompt");
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_assert_nonnull(strstr(error->message, "prompt: Field required"));
	assert_no_credential(error->message);
	g_clear_error(&error);

	ai_media_request_set_model(request, "vendor/broke");
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INSUFFICIENT_QUOTA);
	g_assert_nonnull(strstr(error->message, "Insufficient balance"));
	g_clear_error(&error);

	ai_media_request_set_model(request, "vendor/busy");
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_RATE_LIMITED);
	g_assert_nonnull(strstr(error->message, "retry after 7"));
	g_clear_error(&error);

	/* An unknown model is a 404 the API cannot distinguish from a typo. */
	ai_media_request_set_model(request, "vendor/nothing-here");
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_MODEL_NOT_FOUND);
	g_clear_error(&error);

	/* No redirect is followed with the credential attached. */
	ai_media_request_set_model(request, "vendor/redirect");
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_RESPONSE);
	assert_no_credential(error->message);
	g_clear_error(&error);

	/* An unknown request id is not a model error. */
	{
		g_autoptr(AiMediaJob) job = poll_once(client, "99999999-0000-0000-0000-000000000000", &error);

		g_assert_null(job);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_clear_error(&error);
	}
}

static void
test_refused_locally(void)
{
	g_autoptr(AiHiggsfieldClient) anonymous = ai_higgsfield_client_new();
	g_autoptr(AiHiggsfieldClient) client = new_client();
	g_autoptr(AiMediaRequest) request = ai_media_request_new(AI_HIGGSFIELD_MODEL_SOUL_V2, "x");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *id = NULL;
	guint hits;

	/* No environment fallback: a client with no key has no key. */
	g_assert_false(ai_higgsfield_client_has_credentials(anonymous));
	g_assert_cmpstr(ai_higgsfield_client_get_base_url(anonymous), ==, AI_HIGGSFIELD_DEFAULT_BASE_URL);
	ai_higgsfield_client_set_base_url(anonymous, mock->base_url);

	hits = mock_hits();
	id = submit(anonymous, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_API_KEY);
	g_clear_error(&error);

	/* A model that would leave the path. */
	ai_media_request_set_model(request, "../requests/x/cancel");
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);
	ai_media_request_set_model(request, "vendor/model?hf_webhook=x");
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* A retention the API would refuse. */
	ai_media_request_set_model(request, AI_HIGGSFIELD_MODEL_SOUL_V2);
	ai_media_request_set_expires_after(request, 60);
	id = submit(client, request, &error);
	g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
	g_clear_error(&error);

	/* A job id that is not one. */
	{
		g_autoptr(AiMediaJob) job = poll_once(client, "../../files/generate-upload-url", &error);

		g_assert_null(job);
		g_assert_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST);
		g_clear_error(&error);
	}

	/* None of that reached the server. */
	g_assert_cmpuint(mock_hits(), ==, hits);

	/* A credential that would split a header is refused outright. */
	ai_higgsfield_client_set_credentials(anonymous, "id", "secret\r\nX-Evil: 1");
	g_assert_false(ai_higgsfield_client_has_credentials(anonymous));
	ai_higgsfield_client_set_credentials(anonymous, "a:b", "secret");
	g_assert_false(ai_higgsfield_client_has_credentials(anonymous));
	ai_higgsfield_client_set_credentials(anonymous, "id", "secret");
	g_assert_true(ai_higgsfield_client_has_credentials(anonymous));
	ai_higgsfield_client_set_credentials(anonymous, NULL, NULL);
	g_assert_false(ai_higgsfield_client_has_credentials(anonymous));
}

/* Last: everything above has logged what it was going to log. */
static void
test_logs_clean(void)
{
	g_mutex_lock(&log_lock);
	g_assert_null(strstr(log_text->str, TEST_KEY_SECRET));
	g_assert_null(strstr(log_text->str, TEST_KEY_ID));
	/* The capture is live: the client did log its requests at debug. */
	g_assert_nonnull(strstr(log_text->str, "higgsfield:"));
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

	(void)user_data;

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

/*
 * GTest routes g_log() through its own default handler rather than the
 * writer, so the unstructured path is captured here and handed on.
 */
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

	g_test_add_func("/media/request-json", test_request_json);
	g_test_add_func("/media/job-state", test_job_state);
	g_test_add_func("/higgsfield/submit", test_submit);
	g_test_add_func("/higgsfield/poll-transitions", test_poll_transitions);
	g_test_add_func("/higgsfield/wait-done", test_wait_done);
	g_test_add_func("/higgsfield/wait-failed-and-nsfw", test_wait_failed_and_nsfw);
	g_test_add_func("/higgsfield/wait-rides-out-5xx", test_wait_rides_out_5xx);
	g_test_add_func("/higgsfield/wait-timeout", test_wait_timeout);
	g_test_add_func("/higgsfield/wait-cancelled", test_wait_cancelled);
	g_test_add_func("/higgsfield/wait-private-context", test_wait_private_context);
	g_test_add_func("/higgsfield/unknown-status", test_unknown_status);
	g_test_add_func("/higgsfield/estimate", test_estimate);
	g_test_add_func("/higgsfield/cancel", test_cancel);
	g_test_add_func("/higgsfield/download", test_download);
	g_test_add_func("/higgsfield/download-refuses-scheme", test_download_refuses_scheme);
	g_test_add_func("/higgsfield/upload", test_upload);
	g_test_add_func("/higgsfield/error-envelopes", test_error_envelopes);
	g_test_add_func("/higgsfield/refused-locally", test_refused_locally);
	g_test_add_func("/higgsfield/logs-clean", test_logs_clean);

	status = g_test_run();

	mock_stop();
	g_string_free(log_text, TRUE);
	return status;
}
