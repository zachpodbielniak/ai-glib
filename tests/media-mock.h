/*
 * media-mock.h - A recording mock API for the media-job provider tests
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The same arrangement as tests/test-higgsfield-client.c: a SoupServer on
 * loopback, on its own thread and its own GMainContext, so the client's
 * operations run on the test thread's context and a source attached to
 * the wrong one hangs (and the watchdog names it) instead of passing by
 * luck.  It records, per "<METHOD> <path>", the last request's body,
 * headers and query, and counts hits per key so a mock can script a
 * sequence of answers.  Every log line the process writes is captured, so
 * a test can check the credential is in none of them.
 *
 * Header-only and `static inline` throughout, so a test that uses part of
 * it does not trip -Wunused-function.
 */

#pragma once

#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

typedef void (*MockRoute)(SoupServerMessage *msg, const gchar *method, const gchar *path,
                          const gchar *body, guint hit);

typedef struct
{
	SoupServer   *server;
	GMainContext *context;
	GMainLoop    *loop;
	GThread      *thread;
	gchar        *base_url;
	GMutex        lock;
	MockRoute     route;

	GHashTable   *hits;     /* key -> GUINT_TO_POINTER(count) */
	GHashTable   *headers;  /* key -> GHashTable(lower name -> value) */
	GHashTable   *bodies;   /* key -> body text */
	GHashTable   *queries;  /* key -> query text */
} MediaMock;

static MediaMock *media_mock;

/* Every log line the process wrote. */
static GMutex       media_log_lock;
static GString     *media_log_text;
static GLogFunc     media_previous_handler;

static inline void
media_mock_reply(
	SoupServerMessage *msg,
	guint              status,
	const gchar       *body
){
	soup_server_message_set_status(msg, status, NULL);
	soup_server_message_set_response(msg, "application/json", SOUP_MEMORY_COPY,
	                                  body, strlen(body));
}

static inline void
media_mock_handler(
	SoupServer        *server,
	SoupServerMessage *msg,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	SoupMessageHeaders *request_headers = soup_server_message_get_request_headers(msg);
	SoupMessageBody *request_body = soup_server_message_get_request_body(msg);
	const gchar *method = soup_server_message_get_method(msg);
	g_autofree gchar *key = g_strdup_printf("%s %s", method, path);
	g_autofree gchar *body = g_strndup(request_body->data, request_body->length);
	GHashTable *seen;
	SoupMessageHeadersIter iter;
	const char *name;
	const char *value;
	GUri *uri;
	guint hit;

	(void)server;
	(void)query;
	(void)user_data;

	g_mutex_lock(&media_mock->lock);
	hit = GPOINTER_TO_UINT(g_hash_table_lookup(media_mock->hits, key)) + 1;
	g_hash_table_insert(media_mock->hits, g_strdup(key), GUINT_TO_POINTER(hit));
	seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	soup_message_headers_iter_init(&iter, request_headers);
	while (soup_message_headers_iter_next(&iter, &name, &value))
	{
		g_hash_table_insert(seen, g_ascii_strdown(name, -1), g_strdup(value));
	}
	g_hash_table_insert(media_mock->headers, g_strdup(key), seen);
	g_hash_table_insert(media_mock->bodies, g_strdup(key), g_strdup(body));
	uri = soup_server_message_get_uri(msg);
	g_hash_table_insert(media_mock->queries, g_strdup(key),
	                    g_strdup(g_uri_get_query(uri) != NULL ? g_uri_get_query(uri) : ""));
	g_mutex_unlock(&media_mock->lock);

	media_mock->route(msg, method, path, body, hit);
}

static inline gpointer
media_mock_thread(gpointer user_data)
{
	(void)user_data;

	g_main_context_push_thread_default(media_mock->context);
	g_main_loop_run(media_mock->loop);
	g_main_context_pop_thread_default(media_mock->context);
	return NULL;
}

static inline void
media_mock_start(MockRoute route)
{
	g_autoptr(GError) error = NULL;
	GSList *uris;
	gchar *full;

	media_mock = g_new0(MediaMock, 1);
	g_mutex_init(&media_mock->lock);
	media_mock->route = route;
	media_mock->hits = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	media_mock->headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                            (GDestroyNotify)g_hash_table_unref);
	media_mock->bodies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	media_mock->queries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	media_mock->context = g_main_context_new();
	media_mock->loop = g_main_loop_new(media_mock->context, FALSE);

	g_main_context_push_thread_default(media_mock->context);
	media_mock->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(media_mock->server, NULL, media_mock_handler, NULL, NULL);
	soup_server_listen_local(media_mock->server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error);
	g_assert_no_error(error);
	g_main_context_pop_thread_default(media_mock->context);

	uris = soup_server_get_uris(media_mock->server);
	g_assert_nonnull(uris);
	full = g_uri_to_string((GUri *)uris->data);
	if (g_str_has_suffix(full, "/"))
	{
		full[strlen(full) - 1] = '\0';
	}
	media_mock->base_url = full;
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	media_mock->thread = g_thread_new("media-mock", media_mock_thread, NULL);
}

static inline gboolean
media_mock_quit(gpointer user_data)
{
	(void)user_data;
	g_main_loop_quit(media_mock->loop);
	return G_SOURCE_REMOVE;
}

static inline void
media_mock_stop(void)
{
	g_main_context_invoke(media_mock->context, media_mock_quit, NULL);
	g_thread_join(media_mock->thread);

	g_main_context_push_thread_default(media_mock->context);
	soup_server_disconnect(media_mock->server);
	g_object_unref(media_mock->server);
	while (g_main_context_iteration(media_mock->context, FALSE))
		;
	g_main_context_pop_thread_default(media_mock->context);

	g_main_loop_unref(media_mock->loop);
	g_main_context_unref(media_mock->context);
	g_hash_table_unref(media_mock->hits);
	g_hash_table_unref(media_mock->headers);
	g_hash_table_unref(media_mock->bodies);
	g_hash_table_unref(media_mock->queries);
	g_free(media_mock->base_url);
	g_mutex_clear(&media_mock->lock);
	g_free(media_mock);
	media_mock = NULL;
}

/* The last body recorded for "<METHOD> <path>", or NULL. */
static inline gchar *
media_mock_body(const gchar *key)
{
	gchar *value;

	g_mutex_lock(&media_mock->lock);
	value = g_strdup(g_hash_table_lookup(media_mock->bodies, key));
	g_mutex_unlock(&media_mock->lock);
	return value;
}

static inline gchar *
media_mock_query(const gchar *key)
{
	gchar *value;

	g_mutex_lock(&media_mock->lock);
	value = g_strdup(g_hash_table_lookup(media_mock->queries, key));
	g_mutex_unlock(&media_mock->lock);
	return value;
}

static inline gchar *
media_mock_header(
	const gchar *key,
	const gchar *name
){
	GHashTable *seen;
	gchar *value = NULL;

	g_mutex_lock(&media_mock->lock);
	seen = g_hash_table_lookup(media_mock->headers, key);
	if (seen != NULL)
	{
		value = g_strdup(g_hash_table_lookup(seen, name));
	}
	g_mutex_unlock(&media_mock->lock);
	return value;
}

static inline guint
media_mock_hits(const gchar *key)
{
	guint n;

	g_mutex_lock(&media_mock->lock);
	n = GPOINTER_TO_UINT(g_hash_table_lookup(media_mock->hits, key));
	g_mutex_unlock(&media_mock->lock);
	return n;
}

/* The last body for @key, parsed; the test fails if it is not an object. */
static inline JsonObject *
media_mock_body_object(const gchar *key)
{
	g_autofree gchar *body = media_mock_body(key);
	g_autoptr(JsonParser) parser = json_parser_new();

	g_assert_nonnull(body);
	g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
	g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
	return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

/* ---------------------------------------------------------------------- */
/* Driving async calls                                                    */
/* ---------------------------------------------------------------------- */

static inline gboolean
media_watchdog_cb(gpointer user_data)
{
	(void)user_data;
	g_error("test timed out waiting for an async result");
	return G_SOURCE_REMOVE;
}

/* Iterates the default context until *@result is set, with a watchdog. */
static inline void
media_run_until(GAsyncResult **result)
{
	GSource *watchdog = g_timeout_source_new_seconds(20);

	g_source_set_callback(watchdog, media_watchdog_cb, NULL, NULL);
	g_source_attach(watchdog, NULL);
	while (*result == NULL)
	{
		g_main_context_iteration(NULL, TRUE);
	}
	g_source_destroy(watchdog);
	g_source_unref(watchdog);
}

static inline void
media_store_result(
	GObject      *source,
	GAsyncResult *result,
	gpointer      user_data
){
	(void)source;
	*(GAsyncResult **)user_data = g_object_ref(result);
}

static inline gchar *
media_submit(
	AiMediaGenerator  *generator,
	AiMediaRequest    *request,
	GError           **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_submit_async(generator, request, NULL, media_store_result, &result);
	media_run_until(&result);
	return ai_media_generator_submit_finish(generator, result, error);
}

static inline AiMediaJob *
media_poll(
	AiMediaGenerator  *generator,
	const gchar       *job_id,
	GError           **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_poll_async(generator, job_id, NULL, media_store_result, &result);
	media_run_until(&result);
	return ai_media_generator_poll_finish(generator, result, error);
}

static inline AiMediaJob *
media_wait(
	AiMediaGenerator  *generator,
	const gchar       *job_id,
	GError           **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_wait_async(generator, job_id, 5, 20, 10000, NULL,
	                              media_store_result, &result);
	media_run_until(&result);
	return ai_media_generator_wait_finish(generator, result, error);
}

static inline GBytes *
media_download(
	AiMediaGenerator  *generator,
	const gchar       *url,
	GError           **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_download_async(generator, url, NULL, media_store_result, &result);
	media_run_until(&result);
	return ai_media_generator_download_finish(generator, result, error);
}

static inline gboolean
media_cancel(
	AiMediaGenerator  *generator,
	const gchar       *job_id,
	GError           **error
){
	g_autoptr(GAsyncResult) result = NULL;

	ai_media_generator_cancel_async(generator, job_id, NULL, media_store_result, &result);
	media_run_until(&result);
	return ai_media_generator_cancel_finish(generator, result, error);
}

/* ---------------------------------------------------------------------- */
/* Log capture                                                            */
/* ---------------------------------------------------------------------- */

static inline GLogWriterOutput
media_capture_writer(
	GLogLevelFlags   log_level,
	const GLogField *fields,
	gsize            n_fields,
	gpointer         user_data
){
	gsize i;

	(void)user_data;

	g_mutex_lock(&media_log_lock);
	for (i = 0; i < n_fields; i++)
	{
		if (fields[i].length < 0)
		{
			g_string_append(media_log_text, (const gchar *)fields[i].value);
		}
		else
		{
			g_string_append_len(media_log_text, (const gchar *)fields[i].value, fields[i].length);
		}
		g_string_append_c(media_log_text, ' ');
	}
	g_string_append_c(media_log_text, '\n');
	g_mutex_unlock(&media_log_lock);

	if (log_level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
	{
		return G_LOG_WRITER_HANDLED;
	}
	return g_log_writer_standard_streams(log_level, fields, n_fields, user_data);
}

static inline void
media_capture_handler(
	const gchar    *log_domain,
	GLogLevelFlags  log_level,
	const gchar    *message,
	gpointer        user_data
){
	g_mutex_lock(&media_log_lock);
	g_string_append_printf(media_log_text, "%s %s\n",
	                       (log_domain != NULL) ? log_domain : "", message);
	g_mutex_unlock(&media_log_lock);

	if (log_level & (G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_INFO))
	{
		return;
	}
	media_previous_handler(log_domain, log_level, message, user_data);
}

/* Starts capturing; call before g_test_init(). */
static inline void
media_log_capture_begin(void)
{
	/* Debug output must be produced, or the log check would pass vacuously. */
	g_setenv("G_MESSAGES_DEBUG", "all", TRUE);
	media_log_text = g_string_new(NULL);
	g_log_set_writer_func(media_capture_writer, NULL, NULL);
}

/* Takes over the default handler too; call after g_test_init(). */
static inline void
media_log_capture_handler(void)
{
	media_previous_handler = g_log_set_default_handler(media_capture_handler, NULL);
}

/* Whether the captured log contains @needle. */
static inline gboolean
media_log_contains(const gchar *needle)
{
	gboolean found;

	g_mutex_lock(&media_log_lock);
	found = strstr(media_log_text->str, needle) != NULL;
	g_mutex_unlock(&media_log_lock);
	return found;
}

static inline gsize
media_log_length(void)
{
	gsize length;

	g_mutex_lock(&media_log_lock);
	length = media_log_text->len;
	g_mutex_unlock(&media_log_lock);
	return length;
}

/* ---------------------------------------------------------------------- */
/* Temporary directories                                                  */
/* ---------------------------------------------------------------------- */

/* Removes a flat directory of files and the directory itself. */
static inline void
media_remove_dir(const gchar *dir)
{
	GDir *handle = g_dir_open(dir, 0, NULL);
	const gchar *name;

	if (handle != NULL)
	{
		while ((name = g_dir_read_name(handle)) != NULL)
		{
			g_autofree gchar *path = g_build_filename(dir, name, NULL);

			g_unlink(path);
		}
		g_dir_close(handle);
	}
	g_rmdir(dir);
}
