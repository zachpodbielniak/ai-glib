/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Private ai-tui integration with herdr's newline-delimited local socket API.
 * A single worker owns all transport: curses and provider callbacks never wait
 * for herdr. No CLI, shell command, extra dependency or global hook is needed.
 */
#pragma once

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <sys/un.h>
#include "core/ai-json-util.h"

#define AI_TUI_HERDR_TIMEOUT_US (250 * G_TIME_SPAN_MILLISECOND)
#define AI_TUI_HERDR_REFRESH_US (5 * G_TIME_SPAN_SECOND)
#define AI_TUI_HERDR_QUEUE_LIMIT (32)
#define AI_TUI_HERDR_REPLY_LIMIT (16384)

typedef struct
{
	gchar *socket_path;
	gchar *pane_id;
	gchar *source;
	GThread *thread;
	GMutex mutex;
	GCond changed;
	GQueue queue;
	const gchar *state;
	gboolean stopping;
	gint64 stop_deadline;
} AiTuiHerdr;

/**
 * ai_tui_herdr_detect:
 * @environment: herdr's environment marker
 * @socket_path: filesystem socket path
 * @pane_id: pane identifier
 *
 * Require the complete injected context. In particular, an explicitly set
 * socket alone must never claim an arbitrary pane outside herdr. Validate the
 * path length before GUnixSocketAddress can silently truncate it. Pane IDs
 * remain opaque UTF-8 strings, so future herdr identifier formats still work.
 *
 * Returns: whether native reporting can be enabled
 */
static gboolean
ai_tui_herdr_detect(
	const gchar *environment,
	const gchar *socket_path,
	const gchar *pane_id
){
	return g_strcmp0(environment, "1") == 0 &&
		socket_path != NULL && g_path_is_absolute(socket_path) &&
		strlen(socket_path) < sizeof(((struct sockaddr_un *)0)->sun_path) &&
		pane_id != NULL && pane_id[0] != '\0' && strlen(pane_id) <= 256 &&
		g_utf8_validate(pane_id, -1, NULL);
}

/**
 * ai_tui_herdr_wait:
 * @socket: nonblocking socket
 * @condition: readiness to await
 * @deadline: absolute monotonic deadline shared by the entire request
 * @error: return location for an error
 *
 * Returns: whether transport may continue before the deadline
 */
static gboolean
ai_tui_herdr_wait(
	GSocket *socket,
	GIOCondition condition,
	gint64 deadline,
	GError **error
){
	gint64 remaining = deadline - g_get_monotonic_time();

	if (remaining <= 0)
	{
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
			"herdr request deadline exceeded");
		return FALSE;
	}
	return g_socket_condition_timed_wait(socket, condition, remaining, NULL, error);
}

/**
 * ai_tui_herdr_request:
 * @self: immutable connection identity
 * @state: semantic state, or %NULL to release this reporter
 * @error: return location for transport or protocol failures
 *
 * Send one JSON line and validate the correlated acknowledgement. Reconnect
 * for every report, supporting server socket replacement. Bound partial I/O,
 * silent peers and oversized replies; never interpret errors as success.
 *
 * Returns: whether herdr acknowledged the report
 */
static gboolean
ai_tui_herdr_request(
	AiTuiHerdr *self,
	const gchar *state,
	GError **error
){
	g_autoptr(GSocket) socket = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonGenerator) generator = json_generator_new();
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) root = NULL;
	g_autoptr(GString) reply = g_string_new(NULL);
	g_autofree gchar *json = NULL;
	g_autofree gchar *line = NULL;
	g_autofree gchar *id = g_uuid_string_random();
	gsize offset = 0;
	gint64 deadline = g_get_monotonic_time() + AI_TUI_HERDR_TIMEOUT_US;
	JsonObject *object;
	JsonObject *result;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, id);
	json_builder_set_member_name(builder, "method");
	json_builder_add_string_value(builder, state != NULL ?
		"pane.report_agent" : "pane.release_agent");
	json_builder_set_member_name(builder, "params");
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "pane_id");
	json_builder_add_string_value(builder, self->pane_id);
	json_builder_set_member_name(builder, "source");
	json_builder_add_string_value(builder, self->source);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, "ai-tui");
	if (state != NULL)
	{
		json_builder_set_member_name(builder, "state");
		json_builder_add_string_value(builder, state);
	}
	json_builder_end_object(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	json = json_generator_to_data(generator, NULL);
	line = g_strconcat(json, "\n", NULL);

	socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, 0, error);
	if (socket == NULL) return FALSE;
	g_socket_set_blocking(socket, FALSE);
	address = g_unix_socket_address_new(self->socket_path);
	if (!g_socket_connect(socket, address, NULL, error))
	{
		if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_PENDING)) return FALSE;
		g_clear_error(error);
		if (!ai_tui_herdr_wait(socket, G_IO_OUT, deadline, error) ||
			!g_socket_check_connect_result(socket, error)) return FALSE;
	}

	/* Nonblocking writes and reads share one budget, including connect. */
	while (offset < strlen(line))
	{
		gssize written;

		if (!ai_tui_herdr_wait(socket, G_IO_OUT, deadline, error)) return FALSE;
		written = g_socket_send(socket, line + offset, strlen(line) - offset, NULL, error);
		if (written < 0)
		{
			if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) return FALSE;
			g_clear_error(error);
			continue;
		}
		if (written == 0) break;
		offset += (gsize)written;
	}
	while (reply->len < AI_TUI_HERDR_REPLY_LIMIT)
	{
		gchar buffer[512];
		gssize received;
		gchar *newline;

		if (!ai_tui_herdr_wait(socket, G_IO_IN, deadline, error)) return FALSE;
		received = g_socket_receive(socket, buffer,
			MIN(sizeof(buffer), AI_TUI_HERDR_REPLY_LIMIT - reply->len), NULL, error);
		if (received < 0)
		{
			if (!g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) return FALSE;
			g_clear_error(error);
			continue;
		}
		if (received == 0) break;
		g_string_append_len(reply, buffer, received);
		newline = memchr(reply->str, '\n', reply->len);
		if (newline == NULL) continue;
		if (!json_parser_load_from_data(parser, reply->str, newline - reply->str, error))
			return FALSE;
		if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) break;
		object = json_node_get_object(json_parser_get_root(parser));
		result = ai_json_get_object(object, "result");
		if (!json_object_has_member(object, "error") &&
			g_strcmp0(ai_json_get_string(object, "id", NULL), id) == 0 &&
			result != NULL &&
			g_strcmp0(ai_json_get_string(result, "type", NULL), "ok") == 0)
			return TRUE;
		break;
	}
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		"herdr returned an incomplete, oversized, rejected or uncorrelated reply");
	return FALSE;
}

/**
 * ai_tui_herdr_worker:
 * @data: reporter owned until this thread is joined
 *
 * Preserve ordinary lifecycle ordering with a bounded queue. Refresh current
 * state every five seconds even while idle, recovering from a missing/restarted
 * server. Shutdown drains for at most 500 ms plus one in-flight request and a
 * release request. Diagnostics use debug logging, leaving curses undisturbed.
 *
 * Returns: %NULL
 */
static gpointer
ai_tui_herdr_worker(gpointer data)
{
	AiTuiHerdr *self = data;
	gint64 refresh = 0;

	g_mutex_lock(&self->mutex);
	for (;;)
	{
		const gchar *state;
		g_autoptr(GError) error = NULL;

		while (g_queue_is_empty(&self->queue) && !self->stopping &&
			g_get_monotonic_time() < refresh)
			g_cond_wait_until(&self->changed, &self->mutex, refresh);
		if (self->stopping && (g_queue_is_empty(&self->queue) ||
			g_get_monotonic_time() >= self->stop_deadline)) break;
		state = g_queue_is_empty(&self->queue) ? self->state : g_queue_pop_head(&self->queue);
		g_mutex_unlock(&self->mutex);
		if (!ai_tui_herdr_request(self, state, &error))
			g_debug("ai-tui herdr: %s", error->message);
		refresh = g_get_monotonic_time() + AI_TUI_HERDR_REFRESH_US;
		g_mutex_lock(&self->mutex);
	}
	g_mutex_unlock(&self->mutex);
	{
		g_autoptr(GError) error = NULL;

		if (!ai_tui_herdr_request(self, NULL, &error))
			g_debug("ai-tui herdr release: %s", error->message);
	}
	return NULL;
}

/**
 * ai_tui_herdr_new:
 * @environment: HERDR_ENV
 * @socket_path: HERDR_SOCKET_PATH
 * @pane_id: HERDR_PANE_ID
 *
 * Capture environment on the main thread before starting the worker. Unique
 * ownership prevents an old process's delayed release from clearing a newer
 * reporter. FIFO requests need no sequenced-source slots in herdr.
 *
 * Returns: (nullable): reporter, or %NULL outside herdr/on thread failure
 */
static AiTuiHerdr *
ai_tui_herdr_new(
	const gchar *environment,
	const gchar *socket_path,
	const gchar *pane_id
){
	AiTuiHerdr *self;
	g_autofree gchar *uuid = NULL;
	g_autoptr(GError) error = NULL;

	if (!ai_tui_herdr_detect(environment, socket_path, pane_id)) return NULL;
	self = g_new0(AiTuiHerdr, 1);
	self->socket_path = g_strdup(socket_path);
	self->pane_id = g_strdup(pane_id);
	uuid = g_uuid_string_random();
	self->source = g_strconcat("custom:ai-tui:", uuid, NULL);
	self->state = "idle";
	g_mutex_init(&self->mutex);
	g_cond_init(&self->changed);
	g_queue_init(&self->queue);
	g_queue_push_tail(&self->queue, (gpointer)self->state);
	self->thread = g_thread_try_new("ai-tui-herdr", ai_tui_herdr_worker, self, &error);
	if (self->thread == NULL)
	{
		g_debug("ai-tui herdr: %s", error->message);
		g_queue_clear(&self->queue);
		g_cond_clear(&self->changed);
		g_mutex_clear(&self->mutex);
		g_free(self->socket_path);
		g_free(self->pane_id);
		g_free(self->source);
		g_free(self);
		return NULL;
	}
	return self;
}

/**
 * ai_tui_herdr_update:
 * @self: (nullable): reporter
 * @busy: whether a turn is in progress
 * @blocked: whether ai-tui is awaiting a tool approval
 *
 * Approval takes precedence over busy. Collapse duplicates and discard obsolete
 * queued states only on overload, bounding memory when a peer stops responding.
 */
static void
ai_tui_herdr_update(AiTuiHerdr *self, gboolean busy, gboolean blocked)
{
	const gchar *state = blocked ? "blocked" : (busy ? "working" : "idle");

	if (self == NULL) return;
	g_mutex_lock(&self->mutex);
	if (!self->stopping && g_strcmp0(self->state, state) != 0)
	{
		self->state = state;
		if (g_queue_get_length(&self->queue) >= AI_TUI_HERDR_QUEUE_LIMIT)
			g_queue_clear(&self->queue);
		g_queue_push_tail(&self->queue, (gpointer)state);
		g_cond_signal(&self->changed);
	}
	g_mutex_unlock(&self->mutex);
}

/**
 * ai_tui_herdr_free:
 * @self: (nullable): reporter
 *
 * Stop refreshes, drain bounded pending reports and release only our identity.
 * Joining guarantees no callbacks or worker accesses survive application exit.
 */
static void
ai_tui_herdr_free(AiTuiHerdr *self)
{
	if (self == NULL) return;
	g_mutex_lock(&self->mutex);
	self->stopping = TRUE;
	self->stop_deadline = g_get_monotonic_time() + 500 * G_TIME_SPAN_MILLISECOND;
	g_cond_signal(&self->changed);
	g_mutex_unlock(&self->mutex);
	g_thread_join(self->thread);
	g_queue_clear(&self->queue);
	g_cond_clear(&self->changed);
	g_mutex_clear(&self->mutex);
	g_free(self->socket_path);
	g_free(self->pane_id);
	g_free(self->source);
	g_free(self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiTuiHerdr, ai_tui_herdr_free)
