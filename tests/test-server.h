/*
 * test-server.h - A loopback SoupServer for driving providers over HTTP
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Point a provider's base URL at one of these and its real request goes on
 * a real socket, so a test asserts on the bytes that actually go on the
 * wire rather than on an intermediate builder.  It is also the only way to
 * get a provider's async and streaming paths to execute at all, which
 * matters under ASAN: the GTask leaks in every HTTP provider's chat and
 * stream paths survived for as long as they did because nothing drove them.
 *
 * Header-only, and everything is `static inline` so a test that uses half
 * the API does not trip -Wunused-function.
 */

#pragma once

#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

/*
 * The server runs on its own thread with its own GMainContext.
 *
 * That is not incidental: ai_image_generator_generate_image() deliberately
 * drives a nested loop on a *private* context so it cannot re-enter the
 * caller's, which means a server attached to the default context would
 * never be dispatched while a synchronous request was in flight, and the
 * request would simply time out.  Giving the server its own thread keeps
 * the sync and async paths testable through the same harness.
 */
typedef struct
{
	SoupServer   *server;
	GMainContext *context;
	GMainLoop    *loop;
	GThread      *thread;
	gchar        *base_url;
	GMutex        lock;

	/* What to reply with. */
	guint       status;
	gchar      *body;
	gchar      *content_type;
	/* Fail this many times with `fail_status` before replying normally,
	 * so retry behaviour can be exercised deterministically. */
	guint       fail_times;
	guint       fail_status;
	/* Stall this long before replying, so a cancellation has a window. */
	guint       delay_ms;
	/* Redirect all paths except /redirect-target when configured. */
	gchar      *redirect_to;

	/* What was received. */
	guint       hits;
	gchar      *last_body;
	gchar      *last_path;
	gchar      *last_api_key_header;
	gchar      *last_query;
	GHashTable *last_headers;   /* lowercased name -> value */
} TServer;

static inline void
tserver_handler(
	SoupServer        *server,
	SoupServerMessage *msg,
	const char        *path,
	GHashTable        *query,
	gpointer           user_data
){
	TServer *ts = user_data;
	SoupMessageBody *request_body;
	SoupMessageHeaders *headers;
	SoupMessageHeadersIter iter;
	const char *name;
	const char *value;
	const gchar *api_key;
	guint delay_ms;

	(void)server;

	g_mutex_lock(&ts->lock);

	ts->hits++;

	g_clear_pointer(&ts->last_path, g_free);
	ts->last_path = g_strdup(path);

	g_clear_pointer(&ts->last_query, g_free);
	if (query != NULL)
	{
		GString *out = g_string_new(NULL);
		GHashTableIter qiter;
		gpointer k;
		gpointer v;

		g_hash_table_iter_init(&qiter, query);
		while (g_hash_table_iter_next(&qiter, &k, &v))
		{
			g_string_append_printf(out, "%s=%s;", (gchar *)k, (gchar *)v);
		}
		ts->last_query = g_string_free(out, FALSE);
	}

	headers = soup_server_message_get_request_headers(msg);

	/*
	 * Every header, not just the one Gemini uses: the providers authenticate
	 * three different ways (x-goog-api-key, x-api-key, Authorization) and a
	 * test should be able to assert on whichever its provider sends.
	 */
	g_hash_table_remove_all(ts->last_headers);
	soup_message_headers_iter_init(&iter, headers);
	while (soup_message_headers_iter_next(&iter, &name, &value))
	{
		g_hash_table_insert(ts->last_headers,
		                    g_ascii_strdown(name, -1),
		                    g_strdup(value));
	}

	api_key = soup_message_headers_get_one(headers, "x-goog-api-key");
	g_clear_pointer(&ts->last_api_key_header, g_free);
	ts->last_api_key_header = g_strdup(api_key);

	request_body = soup_server_message_get_request_body(msg);
	g_clear_pointer(&ts->last_body, g_free);
	ts->last_body = g_strndup(request_body->data, request_body->length);

	delay_ms = ts->delay_ms;

	if (ts->redirect_to != NULL && g_strcmp0(path, "/redirect-target") != 0)
	{
		soup_server_message_set_redirect(msg, SOUP_STATUS_FOUND,
		                                 ts->redirect_to);
		g_mutex_unlock(&ts->lock);
		return;
	}

	if (ts->fail_times > 0)
	{
		ts->fail_times--;
		soup_server_message_set_status(msg, ts->fail_status, NULL);
		soup_server_message_set_response(msg, "application/json",
		                                 SOUP_MEMORY_COPY, "{}", 2);
		g_mutex_unlock(&ts->lock);
		return;
	}

	{
		guint status = ts->status;
		g_autofree gchar *body = g_strdup(ts->body);
		g_autofree gchar *content_type = g_strdup(ts->content_type);

		g_mutex_unlock(&ts->lock);

		/*
		 * Sleep with the lock dropped: a cancellation test wants the client
		 * to give up while this is stalling, and holding the lock would
		 * block the test thread reading `hits` at the same moment.
		 */
		if (delay_ms > 0)
		{
			g_usleep(delay_ms * 1000);
		}

		soup_server_message_set_status(msg, status, NULL);
		soup_server_message_set_response(msg, content_type, SOUP_MEMORY_COPY,
		                                 body, strlen(body));
	}
}

/* Runs the server's context until tserver_free() quits it. */
static inline gpointer
tserver_thread(gpointer user_data)
{
	TServer *ts = user_data;

	g_main_context_push_thread_default(ts->context);
	g_main_loop_run(ts->loop);
	g_main_context_pop_thread_default(ts->context);

	return NULL;
}

/* Signalled once the server is listening and base_url is known. */
static inline gboolean
tserver_started(gpointer user_data)
{
	TServer *ts = user_data;
	g_autoptr(GError) error = NULL;
	GSList *uris;

	soup_server_listen_local(ts->server, 0, 0, &error);
	g_assert_no_error(error);

	uris = soup_server_get_uris(ts->server);
	g_assert_nonnull(uris);

	{
		g_autofree gchar *full = g_uri_to_string((GUri *)uris->data);
		gsize len = strlen(full);

		/* Trim the trailing slash; the providers concatenate paths. */
		if (len > 0 && full[len - 1] == '/')
		{
			full[len - 1] = '\0';
		}

		g_mutex_lock(&ts->lock);
		ts->base_url = g_strdup(full);
		g_mutex_unlock(&ts->lock);
	}

	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	return G_SOURCE_REMOVE;
}

static inline TServer *
tserver_new(void)
{
	TServer *ts = g_new0(TServer, 1);
	guint waited = 0;

	g_mutex_init(&ts->lock);
	ts->status = SOUP_STATUS_OK;
	ts->body = g_strdup("{}");
	ts->content_type = g_strdup("application/json");
	ts->fail_status = 500;
	ts->last_headers = g_hash_table_new_full(g_str_hash, g_str_equal,
	                                         g_free, g_free);

	ts->context = g_main_context_new();
	ts->loop = g_main_loop_new(ts->context, FALSE);

	/* Build the server against the server context so its sources attach
	 * there rather than to whatever the test thread is using. */
	g_main_context_push_thread_default(ts->context);
	ts->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(ts->server, NULL, tserver_handler, ts, NULL);
	g_main_context_pop_thread_default(ts->context);

	g_main_context_invoke(ts->context, tserver_started, ts);

	ts->thread = g_thread_new("ai-glib-test-server", tserver_thread, ts);

	/* Wait for the listen to complete on the server thread. */
	while (TRUE)
	{
		g_mutex_lock(&ts->lock);
		if (ts->base_url != NULL)
		{
			g_mutex_unlock(&ts->lock);
			break;
		}
		g_mutex_unlock(&ts->lock);

		g_assert_cmpuint(waited++, <, 5000);
		g_usleep(1000);
	}

	return ts;
}

static inline void
tserver_set_response(TServer *ts, guint status, const gchar *body)
{
	g_mutex_lock(&ts->lock);
	ts->status = status;
	g_free(ts->body);
	ts->body = g_strdup(body);
	g_free(ts->content_type);
	ts->content_type = g_strdup("application/json");
	g_mutex_unlock(&ts->lock);
}

/*
 * As above, but naming the content type -- text/event-stream for the
 * streaming paths, which reject a body they are not told is SSE.
 */
static inline void
tserver_set_response_full(
	TServer     *ts,
	guint        status,
	const gchar *content_type,
	const gchar *body
){
	g_mutex_lock(&ts->lock);
	ts->status = status;
	g_free(ts->body);
	ts->body = g_strdup(body);
	g_free(ts->content_type);
	ts->content_type = g_strdup(content_type);
	g_mutex_unlock(&ts->lock);
}

static inline void
tserver_set_failures(TServer *ts, guint times, guint status)
{
	g_mutex_lock(&ts->lock);
	ts->fail_times = times;
	ts->fail_status = status;
	g_mutex_unlock(&ts->lock);
}

/* Stall every reply, so a cancellation has somewhere to land. */
static inline void
tserver_set_delay(TServer *ts, guint delay_ms)
{
	g_mutex_lock(&ts->lock);
	ts->delay_ms = delay_ms;
	g_mutex_unlock(&ts->lock);
}

static inline guint
tserver_hits(TServer *ts)
{
	guint hits;

	g_mutex_lock(&ts->lock);
	hits = ts->hits;
	g_mutex_unlock(&ts->lock);

	return hits;
}

/* Snapshot of the captured request, owned by the caller. */
static inline gchar *
tserver_dup_last_body(TServer *ts)
{
	gchar *out;

	g_mutex_lock(&ts->lock);
	out = g_strdup(ts->last_body);
	g_mutex_unlock(&ts->lock);

	return out;
}

static inline gchar *
tserver_dup_last_path(TServer *ts)
{
	gchar *out;

	g_mutex_lock(&ts->lock);
	out = g_strdup(ts->last_path);
	g_mutex_unlock(&ts->lock);

	return out;
}

/* One captured request header by name, case-insensitively. */
static inline gchar *
tserver_dup_header(TServer *ts, const gchar *name)
{
	g_autofree gchar *key = g_ascii_strdown(name, -1);
	gchar *out;

	g_mutex_lock(&ts->lock);
	out = g_strdup(g_hash_table_lookup(ts->last_headers, key));
	g_mutex_unlock(&ts->lock);

	return out;
}

static inline void
tserver_free(TServer *ts)
{
	g_main_loop_quit(ts->loop);
	g_thread_join(ts->thread);

	soup_server_disconnect(ts->server);
	g_object_unref(ts->server);

	g_main_loop_unref(ts->loop);
	g_main_context_unref(ts->context);

	g_free(ts->base_url);
	g_free(ts->body);
	g_free(ts->content_type);
	g_free(ts->last_body);
	g_free(ts->redirect_to);
	g_free(ts->last_path);
	g_free(ts->last_api_key_header);
	g_free(ts->last_query);
	g_hash_table_unref(ts->last_headers);
	g_mutex_clear(&ts->lock);
	g_free(ts);
}

/* Parse the captured request body as JSON. */
static inline JsonObject *
tserver_last_json(TServer *ts, JsonParser **out_parser)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = tserver_dup_last_body(ts);
	JsonNode *root;

	g_assert_nonnull(body);

	*out_parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(*out_parser, body, -1, &error));
	g_assert_no_error(error);

	root = json_parser_get_root(*out_parser);
	g_assert_nonnull(root);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(root));

	return json_node_get_object(root);
}
