/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice-mocks.h"
#include <libsoup/soup.h>
#include <stdarg.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>
static void
joined_mock(AiAudioTransport *self, const gchar *room, const gchar *token,
			GCancellable *cancel, GAsyncReadyCallback cb, gpointer data)
{
	g_autoptr(GTask) task = g_task_new(self, cancel, cb, data);
	g_task_return_boolean(task, TRUE);
}
static void
left_mock(AiAudioTransport *self, GCancellable *cancel, GAsyncReadyCallback cb,
		  gpointer data)
{
	joined_mock(self, NULL, NULL, cancel, cb, data);
}
static AiLivekitTransport *
mock_livekit(const gchar *url, const gchar *token)
{
	TestTransport *t = g_object_new(test_transport_get_type(), NULL);
	AiAudioTransportInterface *iface = AI_AUDIO_TRANSPORT_GET_IFACE(t);
	iface->join_async = joined_mock;
	iface->join_finish = write_finish;
	iface->leave_async = left_mock;
	iface->leave_finish = write_finish;
	return (AiLivekitTransport *)t;
}
static AiWebsocketRecognizer *
mock_stt(const gchar *url)
{
	return (AiWebsocketRecognizer *)g_object_new(test_recognizer_get_type(), NULL);
}
static AiHttpSynthesizer *
mock_tts(const gchar *url)
{
	return (AiHttpSynthesizer *)g_object_new(test_synthesizer_get_type(), NULL);
}
static AiWebrtcVoiceActivity *
mock_vad(void)
{
	return (AiWebrtcVoiceActivity *)g_object_new(test_activity_get_type(), NULL);
}
static GObject *
mock_provider(AiProviderType type, AiConfig *config, GError **error)
{
	return G_OBJECT(ai_mock_provider_new());
}
static gpointer
mock_object_new(GType type, const gchar *first, ...)
{
	gpointer object;
	va_list args;
	if (type == AI_TYPE_LIVEKIT_TRANSPORT || type == AI_TYPE_LOCAL_AUDIO_TRANSPORT)
		return mock_livekit(NULL, NULL);
	va_start(args, first);
	object = g_object_new_valist(type, first, args);
	va_end(args);
	return object;
}
#define g_object_new mock_object_new
#define ai_websocket_recognizer_new mock_stt
#define ai_http_synthesizer_new mock_tts
#define ai_webrtc_voice_activity_new mock_vad
#define ai_provider_factory_new mock_provider
#define main ai_call_program_main
#include "../bin/ai-call.c"
#undef main
#undef g_object_new

typedef struct {
	App app;
	SoupServer *server;
	gchar *url;
	guint publications, clears, jwt_requests, summaries;
	guint fail_clear; /* 1: one 503; 2: 403 every time */
	GPtrArray *sent; /* m.room.message bodies; event id "$live<index>" */
} MatrixFixture;
static void
matrix_request(SoupServer *server, SoupServerMessage *message, const gchar *path,
			   GHashTable *query, gpointer data)
{
	MatrixFixture *f = data;
	const gchar *body = "{}";
	guint status = 200;
	g_assert_cmpstr(soup_message_headers_get_one(
						soup_server_message_get_request_headers(message), "User-Agent"),
					==, USER_AGENT);
	if (strstr(path, "/sfu/get") != NULL) {
		g_autoptr(JsonParser) parser = json_parser_new();
		SoupMessageBody *request_body = soup_server_message_get_request_body(message);
		const gchar *device;
		g_assert_true(json_parser_load_from_data(parser, request_body->data,
												 request_body->length, NULL));
		device = ai_json_get_string(ai_json_root_object(parser), "device_id", "");
		g_assert_true(g_str_equal(device, "TESTDEVICE") ||
					  g_str_equal(device, "TESTDEVICE-RX"));
		f->jwt_requests++;
		body = "{\"jwt\":\"test-token\"}";
	} else if (strstr(path, "/openid/") != NULL)
		body = "{\"access_token\":\"openid-test\",\"token_type\":\"Bearer\",\"matrix_"
			   "server_name\":\"test\",\"expires_in\":600}";
	else if (strstr(path, "/send/m.room.message/") != NULL) {
		SoupMessageBody *request_body = soup_server_message_get_request_body(message);
		g_autofree gchar *reply = g_strdup_printf("{\"event_id\":\"$live%u\"}", f->sent->len);
		g_ptr_array_add(f->sent, g_strndup(request_body->data, request_body->length));
		soup_server_message_set_status(message, 200, NULL);
		soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY,
										 reply, strlen(reply));
		return;
	} else if (strstr(path, "/messages") != NULL)
		body = "{\"chunk\":[]}";
	else if (strstr(path, "/state/" MEMBER) != NULL) {
		g_autoptr(JsonParser) parser = json_parser_new();
		SoupMessageBody *request_body = soup_server_message_get_request_body(message);
		g_assert_true(json_parser_load_from_data(parser, request_body->data,
												 request_body->length, NULL));
		if (json_object_get_size(ai_json_root_object(parser)) == 0) {
			f->clears++;
			if (f->fail_clear == 2)
				status = 403;
			else if (f->fail_clear) {
				f->fail_clear = 0;
				status = 503;
			}
		} else {
			f->publications++;
			body = "{\"event_id\":\"$member\"}";
		}
	} else if (g_str_has_suffix(path, "/state"))
		body = "[{\"type\":\"org.matrix.msc3401.call.member\",\"sender\":\"@assistant:"
			   "test\","
			   "\"state_key\":\"old-device\",\"content\":{\"application\":\"m.call\"}}]";
	else if (strstr(path, "/sync") != NULL) {
		soup_server_message_pause(message);
		return;
	}
	soup_server_message_set_status(message, status, NULL);
	soup_server_message_set_response(message, "application/json", SOUP_MEMORY_COPY, body,
									 strlen(body));
}
static void
matrix_setup(MatrixFixture *f, gconstpointer data)
{
	GSList *uris;
	App *a = &f->app;
	f->sent = g_ptr_array_new_with_free_func(g_free);
	f->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(f->server, NULL, matrix_request, f, NULL);
	g_assert_true(soup_server_listen_local(f->server, 0, 0, NULL));
	uris = soup_server_get_uris(f->server);
	f->url = g_uri_to_string(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	a->loop = g_main_loop_new(NULL, FALSE);
	a->http = soup_session_new();
	a->config = ai_config_new();
	a->sync_cancel = g_cancellable_new();
	a->calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, call_unref);
	a->homeserver = g_strndup(f->url, strlen(f->url) - 1);
	a->jwt_url = g_strconcat(f->url, "sfu/get", NULL);
	a->mxid = "@assistant:test";
	a->access = "test-token";
	a->device = "TESTDEVICE";
	a->identity = "Test voice";
	a->foci = "https://test/jwt";
	a->stt_url = "ws://test/stt";
	a->tts_url = "http://test/tts";
	a->livekit_url = "ws://test/livekit";
	a->greeting = "Hello from Assistant.";
}
static void
matrix_teardown(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	g_assert_cmpuint(g_hash_table_size(a->calls), ==, 0);
	g_hash_table_unref(a->calls);
	g_object_unref(a->config);
	g_object_unref(a->http);
	g_object_unref(a->sync_cancel);
	soup_server_disconnect(f->server);
	g_object_unref(f->server);
	g_main_loop_unref(a->loop);
	g_free(a->homeserver);
	g_free(a->jwt_url);
	g_free(a->since);
	g_free(f->url);
	g_ptr_array_unref(f->sent);
}
static gboolean
matrix_timeout(gpointer data)
{
	g_error("Matrix lifecycle test timed out");
	return G_SOURCE_REMOVE;
}
static void
answer_cleanup(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	Call *call = start_call(a, "!room:test", NULL, NULL, FALSE);
	gint64 limit = g_get_monotonic_time() + 3000000;
	guint timeout;
	while ((!call->greeted || ((TestTransport *)call->transport)->writes == 0) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(call->greeted);
	g_assert_cmpuint(f->jwt_requests, ==, 2);
	g_assert_cmpuint(f->publications, ==, 1);
	f->fail_clear = GPOINTER_TO_INT(data);
	timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
	shutdown_app(a);
	g_main_loop_run(a->loop);
	g_source_remove(timeout);
	/* A refusal is not retried: shutdown ends rather than asking forever. */
	g_assert_cmpuint(f->clears, ==, GPOINTER_TO_INT(data) == 1 ? 2 : 1);
}
static void
pump_ms(guint ms);
/* A call that ended can finish clearing after the caller rang back: it must
 * not remove the new call, nor clear the membership that is now the new one's. */
static void
stale_clear(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	Call stale = {0};
	Call *fresh = start_call(a, "!room:test", NULL, NULL, FALSE);
	guint clears = f->clears;
	stale.app = a;
	stale.room = "!room:test";
	stale.key = "stale";
	g_ref_count_init(&stale.refs);
	g_ref_count_inc(&stale.refs);
	stale.left = TRUE;
	removed_if_done(&stale);
	g_assert_true(g_hash_table_lookup(a->calls, "!room:test") == fresh);
	stale.closing = TRUE;
	member_posted(a, NULL, NULL, &stale);
	pump_ms(100);
	g_assert_cmpuint(f->clears, ==, clears);
	g_assert_true(g_hash_table_lookup(a->calls, "!room:test") == fresh);
	/* With nobody in the room, the late reply may have re-added the old
	 * membership after its clear: clear it once more. */
	{
		gpointer key = NULL;
		g_assert_true(g_hash_table_steal_extended(a->calls, "!room:test", &key, NULL));
		g_free(key);
	}
	member_posted(a, NULL, NULL, &stale);
	pump_ms(100);
	g_assert_cmpuint(f->clears, ==, clears + 1);
	g_hash_table_insert(a->calls, g_strdup("!room:test"), fresh);
	{
		guint timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
		shutdown_app(a);
		g_main_loop_run(a->loop);
		g_source_remove(timeout);
	}
}
/* An outbound drop file too large to be a request is removed, not reread
 * every second for the life of the process, and starts no call. */
static void
drop_oversized(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	g_autofree gchar *dir = g_dir_make_tmp("call-drop-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "outbound.json", NULL);
	g_autofree gchar *big = g_strnfill(70000, ' ');
	gint64 limit = g_get_monotonic_time() + 3000000;
	g_assert_true(g_file_set_contents(path, big, -1, NULL));
	a->drop_path = path;
	a->primed = TRUE;
	g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "*over 64 KiB*");
	watch_drop(a);
	while (g_file_test(path, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < limit)
		pump_ms(10);
	pump_ms(20);
	g_test_assert_expected_messages();
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_false(a->drop_busy);
	g_assert_cmpuint(g_hash_table_size(a->calls), ==, 0);
	a->drop_path = NULL;
	g_rmdir(dir);
}
/* A transcript that could not be written to is still closed, and the hook
 * still runs: the file is on disk, and post-call processing expects it. */
static void
transcript_write_failure(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	g_autofree gchar *dir = g_dir_make_tmp("call-transcripts-XXXXXX", NULL);
	g_autofree gchar *calls = g_build_filename(dir, "calls", NULL);
	g_autofree gchar *script = g_build_filename(dir, "hook.sh", NULL);
	g_autofree gchar *marker = g_build_filename(dir, "hook-ran", NULL);
	g_autofree gchar *path = NULL, *seen = NULL;
	gint64 limit = g_get_monotonic_time() + 3000000;
	Call *call;
	guint timeout;
	gint full, fd;
	g_assert_true(g_file_set_contents(
		script, "#!/bin/sh\nprintf '%s' \"$1\" > \"$(dirname \"$0\")/hook-ran.tmp\" && "
				"mv \"$(dirname \"$0\")/hook-ran.tmp\" \"$(dirname \"$0\")/hook-ran\"\n",
		-1, NULL));
	g_assert_cmpint(g_chmod(script, 0755), ==, 0);
	a->transcript_dir = calls;
	a->transcript_hook = script;
	call = start_call(a, "!room:test", NULL, NULL, FALSE);
	while ((!call->greeted || call->transcript == NULL) && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_nonnull(call->transcript);
	path = g_strdup(ai_call_transcript_get_path(call->transcript));
	/* Every later write fails as a full disk would. */
	full = g_open("/dev/full", O_WRONLY, 0);
	g_assert_cmpint(full, >=, 0);
	for (fd = 3; fd < 1024; fd++) {
		g_autofree gchar *link = g_strdup_printf("/proc/self/fd/%d", fd);
		g_autofree gchar *target = g_file_read_link(link, NULL);
		if (target != NULL && g_str_equal(target, path))
			break;
	}
	g_assert_cmpint(fd, <, 1024);
	g_assert_cmpint(dup2(full, fd), ==, fd);
	g_close(full, NULL);
	g_signal_emit_by_name(call->voice, "transcript", "Caller", "Hello?", TRUE);
	g_signal_emit_by_name(call->voice, "transcript", "Caller", "Anyone?", TRUE);
	timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
	shutdown_app(a);
	g_main_loop_run(a->loop);
	g_source_remove(timeout);
	limit = g_get_monotonic_time() + 3000000;
	while (!g_file_test(marker, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(g_file_get_contents(marker, &seen, NULL, NULL));
	g_assert_cmpstr(seen, ==, path);
	a->transcript_dir = a->transcript_hook = NULL;
}
/* A whole call through ai-call: what the caller says is on disk as they say
 * it, the header is completed at hangup, and the hook gets the path. */
static void
transcript_on_disk(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	g_autofree gchar *dir = g_dir_make_tmp("call-transcripts-XXXXXX", NULL);
	g_autofree gchar *calls = g_build_filename(dir, "calls", NULL);
	g_autofree gchar *script = g_build_filename(dir, "hook.sh", NULL);
	g_autofree gchar *marker = g_build_filename(dir, "hook-ran", NULL);
	g_autofree gchar *path = NULL, *contents = NULL, *seen = NULL;
	gint64 limit = g_get_monotonic_time() + 3000000;
	Call *call;
	guint timeout;
	g_assert_true(g_file_set_contents(
		script, "#!/bin/sh\nprintf '%s' \"$1\" > \"$(dirname \"$0\")/hook-ran.tmp\" && "
				"mv \"$(dirname \"$0\")/hook-ran.tmp\" \"$(dirname \"$0\")/hook-ran\"\n",
		-1, NULL));
	g_assert_cmpint(g_chmod(script, 0755), ==, 0);
	a->transcript_dir = calls;
	a->transcript_hook = script;
	call = start_call(a, "!room:test", NULL, NULL, FALSE);
	while ((!call->greeted || call->transcript == NULL) && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_nonnull(call->transcript);
	path = g_strdup(ai_call_transcript_get_path(call->transcript));
	g_signal_emit_by_name(call->voice, "transcript", "Caller", "partial words", FALSE);
	g_signal_emit_by_name(call->voice, "transcript", "Caller", "What's on today?", TRUE);
	g_signal_emit_by_name(call->voice, "tool", "calendar", "{\"day\":\"today\"}",
						  "free until noon", FALSE);
	g_signal_emit_by_name(call->voice, "spoken", "Nothing until noon.", TRUE);
	/* Before hangup: the utterance is already there, the end is not. */
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "\"end\":null"));
	g_assert_nonnull(strstr(contents, "\"speaker\":\"Caller\""));
	g_assert_nonnull(strstr(contents, "\"text\":\"What's on today?\""));
	g_assert_null(strstr(contents, "partial words"));
	g_assert_nonnull(strstr(contents, "\"role\":\"assistant\""));
	g_assert_nonnull(strstr(contents, "\"speaker\":\"@assistant:test\""));
	g_assert_nonnull(strstr(contents, "\"text\":\"Nothing until noon.\""));
	g_assert_nonnull(strstr(contents, "\"type\":\"tool\""));
	g_assert_nonnull(strstr(contents, "\"name\":\"calendar\""));
	g_assert_nonnull(strstr(contents, "\"result\":\"free until noon\""));
	g_clear_pointer(&contents, g_free);
	timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
	shutdown_app(a);
	g_main_loop_run(a->loop);
	g_source_remove(timeout);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_null(strstr(contents, "\"end\":null"));
	g_assert_nonnull(strstr(contents, "\"duration_ms\":"));
	g_assert_nonnull(strstr(contents, "\"room\":\"!room:test\""));
	limit = g_get_monotonic_time() + 3000000;
	while (!g_file_test(marker, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(g_file_get_contents(marker, &seen, NULL, NULL));
	g_assert_cmpstr(seen, ==, path);
	a->transcript_dir = a->transcript_hook = NULL;
}
static void
pump_ms(guint ms)
{
	gint64 until = g_get_monotonic_time() + ms * 1000;
	while (g_get_monotonic_time() < until) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
}
/* Index of the one sent message whose body is TEXT; with EDIT, the one edit
 * whose new content is TEXT. -1 when there is none. */
static gint
find_sent(MatrixFixture *f, guint from, const gchar *text, gboolean edit, gchar **target)
{
	guint i;
	for (i = from; i < f->sent->len; i++) {
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *o, *relation, *content;
		g_assert_true(json_parser_load_from_data(parser, f->sent->pdata[i], -1, NULL));
		o = ai_json_root_object(parser);
		g_assert_cmpstr(ai_json_get_string(o, "msgtype", NULL), ==, "m.notice");
		relation = ai_json_get_object(o, "m.relates_to");
		content = ai_json_get_object(o, "m.new_content");
		if (!edit && relation == NULL &&
			g_strcmp0(ai_json_get_string(o, "body", NULL), text) == 0)
			return (gint)i;
		if (edit && relation != NULL && content != NULL &&
			g_strcmp0(ai_json_get_string(content, "body", NULL), text) == 0) {
			g_assert_cmpstr(ai_json_get_string(relation, "rel_type", NULL), ==,
							"m.replace");
			g_assert_cmpstr(ai_json_get_string(content, "msgtype", NULL), ==, "m.notice");
			*target = g_strdup(ai_json_get_string(relation, "event_id", NULL));
			return (gint)i;
		}
	}
	return -1;
}
/* live-text: the caller's words and the assistant's reply appear in the room as they
 * are spoken. A reply is one message, edited as each sentence plays, and the
 * next turn starts a new one. Off, nothing is sent. */
static void
live_text(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	gint mode = GPOINTER_TO_INT(data);
	gint64 limit = g_get_monotonic_time() + 3000000;
	g_autofree gchar *target = NULL, *first_id = NULL;
	guint base, expected = mode == 2 ? 5 : mode == 1 ? 3 : 0, timeout;
	gint first, edit, second;
	Call *call;
	a->live_text = mode;
	call = start_call(a, "!room:test", NULL, NULL, FALSE);
	while ((!call->greeted ||
			ai_voice_session_get_state(call->voice) != AI_VOICE_LISTENING) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(call->greeted);
	pump_ms(100); /* let the greeting's own message land */
	base = f->sent->len;
	g_signal_emit_by_name(call->voice, "transcript", "@caller:test:DEVICE", "partial",
						  FALSE);
	g_signal_emit_by_name(call->voice, "transcript", "@caller:test:DEVICE",
						  "What's on today?", TRUE);
	g_signal_emit_by_name(call->voice, "spoken", "Nothing until noon.", TRUE);
	g_signal_emit_by_name(call->voice, "spoken", "Then lunch.", TRUE);
	g_signal_emit_by_name(call->voice, "transcript", "@caller:test:DEVICE", "Thanks.",
						  TRUE);
	g_signal_emit_by_name(call->voice, "spoken", "You're welcome.", TRUE);
	limit = g_get_monotonic_time() + 3000000;
	while (f->sent->len < base + expected && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	pump_ms(100); /* nothing further arrives */
	g_assert_cmpuint(f->sent->len - base, ==, expected);
	if (mode != 0) {
		first = find_sent(f, base, "Nothing until noon.", FALSE, NULL);
		edit = find_sent(f, base, "Nothing until noon. Then lunch.", TRUE, &target);
		second = find_sent(f, base, "You're welcome.", FALSE, NULL);
		g_assert_cmpint(first, >=, 0);
		g_assert_cmpint(edit, >, first);
		g_assert_cmpint(second, >=, 0);
		first_id = g_strdup_printf("$live%d", first);
		g_assert_cmpstr(target, ==, first_id);
		if (mode == 2) {
			g_assert_cmpint(find_sent(f, base, "caller: What's on today?", FALSE, NULL),
							>=, 0);
			g_assert_cmpint(find_sent(f, base, "caller: Thanks.", FALSE, NULL), >=, 0);
		} else
			g_assert_cmpint(find_sent(f, base, "caller: What's on today?", FALSE, NULL),
							==, -1);
		g_assert_cmpint(find_sent(f, base, "caller: partial", FALSE, NULL), ==, -1);
	}
	timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
	shutdown_app(a);
	g_main_loop_run(a->loop);
	g_source_remove(timeout);
	a->live_text = 0;
}
/* A spoken command is on the record and in the room, and "hang up" ends the
 * call the way a shutdown does: goodbye, then membership cleared. */
static void
voice_command_call(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	g_autofree gchar *dir = g_dir_make_tmp("call-commands-XXXXXX", NULL);
	g_autofree gchar *path = NULL, *contents = NULL;
	gint64 limit = g_get_monotonic_time() + 3000000;
	guint base, timeout;
	Call *call;
	a->transcript_dir = dir;
	a->live_text = 2;
	call = call_ref(start_call(a, "!room:test", NULL, NULL, FALSE));
	while ((!call->greeted || call->transcript == NULL ||
			ai_voice_session_get_state(call->voice) != AI_VOICE_LISTENING) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_nonnull(call->transcript);
	path = g_strdup(ai_call_transcript_get_path(call->transcript));
	pump_ms(100);
	base = f->sent->len;
	g_signal_emit_by_name(call->voice, "command", "mute");
	limit = g_get_monotonic_time() + 3000000;
	while (f->sent->len == base && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_cmpint(find_sent(f, base, "(muted)", FALSE, NULL), >=, 0);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "\"type\":\"command\""));
	g_assert_nonnull(strstr(contents, "\"name\":\"mute\""));
	g_assert_false(call->closing);
	timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
	g_signal_emit_by_name(call->voice, "command", "hangup");
	limit = g_get_monotonic_time() + 4000000;
	while (g_hash_table_size(a->calls) != 0 && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_source_remove(timeout);
	g_assert_true(call->closing);
	g_assert_cmpuint(f->clears, ==, 1);
	g_assert_cmpuint(g_hash_table_size(a->calls), ==, 0);
	call_unref(call);
	a->transcript_dir = NULL;
	a->live_text = 0;
}
/* --local: one session on this machine's microphone and speaker. No Matrix
 * request is made, the greeting is spoken, and hanging up ends the program. */
static void
local_session(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	g_autofree gchar *dir = g_dir_make_tmp("call-local-XXXXXX", NULL);
	g_autofree gchar *path = NULL, *contents = NULL;
	gint64 limit = g_get_monotonic_time() + 3000000;
	guint timeout;
	Call *call;
	a->local = TRUE;
	a->mxid = "assistant";
	a->transcript_dir = dir;
	call = call_ref(start_local(a));
	g_assert_nonnull(call);
	while ((!call->greeted || call->transcript == NULL) && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(call->greeted);
	path = g_strdup(ai_call_transcript_get_path(call->transcript));
	timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
	g_signal_emit_by_name(call->voice, "command", "hangup");
	g_main_loop_run(a->loop);
	g_source_remove(timeout);
	g_assert_true(call->closing);
	g_assert_cmpuint(g_hash_table_size(a->calls), ==, 0);
	/* Nothing went to the homeserver: no membership published or cleared. */
	g_assert_cmpuint(f->publications, ==, 0);
	g_assert_cmpuint(f->clears, ==, 0);
	g_assert_cmpuint(f->jwt_requests, ==, 0);
	g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
	g_assert_nonnull(strstr(contents, "\"room\":\"local\""));
	call_unref(call);
	a->transcript_dir = NULL;
	a->local = FALSE;
}
static JsonObject *
read_state(const gchar *path, JsonParser *parser)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(json_parser_load_from_file(parser, path, &error));
	return ai_json_root_object(parser);
}
/* state-file: what a display beside the speaker shows. The state, whether
 * the assistant is muted, and the last thing heard and said, rewritten as
 * each changes; toggling mute from outside (a button) is reflected there. */
static void
state_file(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	g_autofree gchar *dir = g_dir_make_tmp("call-state-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(dir, "state.json", NULL);
	gint64 limit = g_get_monotonic_time() + 3000000;
	guint timeout;
	Call *call;
	a->local = TRUE;
	a->mxid = "assistant";
	a->state_file = path;
	call = call_ref(start_local(a));
	while (!call->greeted && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
	g_signal_emit_by_name(call->voice, "transcript", "local", "What's on today?", TRUE);
	g_signal_emit_by_name(call->voice, "spoken", "Nothing until noon.", TRUE);
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		JsonObject *o = read_state(path, parser);
		g_assert_cmpstr(ai_json_get_string(o, "heard", NULL), ==, "What's on today?");
		g_assert_cmpstr(ai_json_get_string(o, "said", NULL), ==, "Nothing until noon.");
		g_assert_false(ai_json_get_boolean(o, "muted", TRUE));
		g_assert_nonnull(ai_json_get_string(o, "state", NULL));
	}
	toggle_mute(a);
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_assert_true(ai_json_get_boolean(read_state(path, parser), "muted", FALSE));
	}
	timeout = g_timeout_add_seconds(5, matrix_timeout, NULL);
	g_signal_emit_by_name(call->voice, "command", "hangup");
	g_main_loop_run(a->loop);
	g_source_remove(timeout);
	{
		g_autoptr(JsonParser) parser = json_parser_new();
		g_assert_cmpstr(ai_json_get_string(read_state(path, parser), "state", NULL), ==,
						"ended");
	}
	call_unref(call);
	a->state_file = NULL;
	a->local = FALSE;
}
static void
summary_log(const gchar *domain, GLogLevelFlags level, const gchar *message,
			gpointer data)
{
	MatrixFixture *f = data;
	if (g_str_has_prefix(message, "Call summary:")) {
		f->summaries++;
		g_assert_nonnull(strstr(message, "duration-ms="));
		g_assert_nonnull(
			strstr(message, "reconnects=0 dropped-buffers=0 late-buffers=0"));
	}
}
static void
busy_synthesize(AiSpeechSynthesizer *self, const gchar *text, GCancellable *cancel,
				GAsyncReadyCallback callback, gpointer data)
{
	if (g_object_get_data(G_OBJECT(self), "pending-cancel") != NULL) {
		g_autoptr(GTask) task = g_task_new(self, cancel, callback, data);
		guint attempts =
			GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(self), "busy-attempts"));
		g_object_set_data(G_OBJECT(self), "busy-attempts",
						  GUINT_TO_POINTER(attempts + 1));
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PENDING,
								"Cancellation still draining");
		return;
	}
	synthesize(self, text, cancel, callback, data);
}
static gboolean
finish_cancel(gpointer data)
{
	GTask *task = data;
	g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(task)));
	g_object_set_data(g_task_get_source_object(task), "pending-cancel", NULL);
	g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
							"Cancelled reply drained");
	return G_SOURCE_REMOVE;
}
static void
signal_goodbye(MatrixFixture *f, gconstpointer data)
{
	App *a = &f->app;
	Call *call = start_call(a, "!room:test", NULL, NULL, FALSE);
	g_autoptr(GObject) synth_ref = NULL;
	g_autoptr(GObject) transport_ref = NULL;
	g_autoptr(GTask) interrupted = NULL;
	TestSynthesizer *synth;
	gint signal_number = GPOINTER_TO_INT(data);
	gboolean stalled = signal_number == 0;
	gboolean stuck_busy = signal_number == -1;
	guint expected_texts = stuck_busy ? 1 : signal_number == SIGINT ? 3 : 2;
	gint64 start, limit = g_get_monotonic_time() + 3000000;
	guint signal_source, timeout, logger;
	guint8 samples[320] = {1};
	while ((!call->greeted ||
			ai_voice_session_get_state(call->voice) != AI_VOICE_LISTENING) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(call->greeted);
	synth_ref = g_object_ref(G_OBJECT(call->synthesizer));
	transport_ref = g_object_ref(G_OBJECT(call->transport));
	synth = (TestSynthesizer *)synth_ref;
	synth->pcm = g_bytes_new(samples, sizeof(samples));
	logger = g_log_set_handler("ai-call", G_LOG_LEVEL_INFO, summary_log, f);
	if (signal_number == SIGINT) {
		synth->hold_after = 2;
		synth->delay_audio = TRUE;
		ai_voice_session_say(call->voice, "A reply still being generated.");
		g_assert_nonnull(synth->held);
		interrupted = g_steal_pointer(&synth->held);
		g_object_set_data(G_OBJECT(synth), "pending-cancel", interrupted);
		AI_SPEECH_SYNTHESIZER_GET_IFACE(synth)->synthesize_async = busy_synthesize;
		g_timeout_add_full(G_PRIORITY_DEFAULT, 100, finish_cancel,
						   g_object_ref(interrupted), g_object_unref);
		synth->hold_after = 0;
		synth->delay_audio = FALSE;
	}
	if (stuck_busy) {
		g_object_set_data(G_OBJECT(synth), "pending-cancel", synth);
		AI_SPEECH_SYNTHESIZER_GET_IFACE(synth)->synthesize_async = busy_synthesize;
		signal_number = SIGTERM;
	}
	if (stalled) {
		synth->hold_after = 2;
		synth->delay_audio = TRUE;
		signal_number = SIGTERM;
	}
	signal_source = g_unix_signal_add(signal_number, shutdown_app, a);
	timeout = g_timeout_add_seconds(4, matrix_timeout, NULL);
	start = g_get_monotonic_time();
	g_assert_cmpint(kill(getpid(), signal_number), ==, 0);
	g_main_loop_run(a->loop);
	g_source_remove(signal_source);
	g_log_remove_handler("ai-call", logger);
	g_assert_cmpuint(f->summaries, ==, 1);
	g_source_remove(timeout);
	g_assert_cmpuint(synth->texts->len, ==, expected_texts);
	if (!stuck_busy)
		g_assert_cmpstr(g_ptr_array_index(synth->texts, expected_texts - 1), ==,
						"Goodbye.");
	g_assert_cmpuint(f->clears, ==, 1);
	if (interrupted != NULL) {
		g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(interrupted)));
		g_assert_cmpuint(
			GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(synth), "busy-attempts")), >, 0);
		g_assert_true(g_task_get_completed(interrupted));
		AI_SPEECH_SYNTHESIZER_GET_IFACE(synth)->synthesize_async = synthesize;
		while (g_main_context_iteration(NULL, FALSE)) {
		}
	}
	g_assert_cmpint(g_get_monotonic_time() - start, <, 2500000);
	if (stuck_busy) {
		g_assert_cmpint(g_get_monotonic_time() - start, >=, 1900000);
		g_assert_cmpuint(
			GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(synth), "busy-attempts")), >, 1);
		g_object_set_data(G_OBJECT(synth), "pending-cancel", NULL);
		AI_SPEECH_SYNTHESIZER_GET_IFACE(synth)->synthesize_async = synthesize;
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_assert_cmpuint(((TestTransport *)transport_ref)->writes, ==, 1);
	} else if (stalled) {
		g_assert_nonnull(synth->held);
		g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(synth->held)));
		g_task_return_boolean(synth->held, TRUE);
		g_clear_object(&synth->held);
		while (g_main_context_iteration(NULL, FALSE)) {
		}
	} else {
		g_assert_cmpuint(((TestTransport *)transport_ref)->writes, ==, 2);
		g_assert_cmpuint(((TestTransport *)transport_ref)->non_silent_samples, >, 0);
	}
}
static void
terminal_media_error_test(MatrixFixture *f, gconstpointer data)
{
	Call *call = call_ref(start_call(&f->app, "!room:test", NULL, NULL, FALSE));
	g_autoptr(AiAudioTransport) transport = NULL;
	g_autoptr(GError) error =
		g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Media recovery exhausted");
	gint64 limit = g_get_monotonic_time() + 3000000;
	while ((!call->greeted ||
			ai_voice_session_get_state(call->voice) != AI_VOICE_LISTENING) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(call->greeted);
	transport = g_object_ref(call->transport);
	g_signal_emit_by_name(transport, "reconnecting");
	g_assert_false(call->closing);
	g_assert_cmpuint(f->clears, ==, 0);
	g_signal_emit_by_name(transport, "reconnected");
	g_signal_emit_by_name(transport, "error", error);
	g_assert_true(call->closing);
	limit = g_get_monotonic_time() + 3000000;
	while (g_hash_table_size(f->app.calls) != 0 && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(call->left);
	g_assert_cmpuint(call->errors, ==, 1);
	g_assert_cmpuint(f->clears, ==, 1);
	g_assert_cmpuint(g_hash_table_size(f->app.calls), ==, 0);
	call_unref(call);
	/* A retained transport cannot invoke the already released call again. */
	g_signal_emit_by_name(transport, "error", error);
}
static void
stale_membership(MatrixFixture *f, gconstpointer data)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	gint64 limit = g_get_monotonic_time() + 3000000;
	guint timeout;
	g_assert_true(json_parser_load_from_data(
		parser, "{\"next_batch\":\"s1\",\"rooms\":{\"join\":{\"!room:test\":{}}}}", -1,
		NULL));
	primed(&f->app, json_parser_get_root(parser), NULL, NULL);
	while (!f->app.primed && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE)) {
		}
		g_usleep(1000);
	}
	g_assert_true(f->app.primed);
	g_assert_cmpuint(f->clears, ==, 1);
	timeout = g_timeout_add_seconds(3, matrix_timeout, NULL);
	shutdown_app(&f->app);
	g_main_loop_run(f->app.loop);
	g_source_remove(timeout);
}
static void
info_visible(void)
{
	if (g_test_subprocess()) {
		g_unsetenv("G_MESSAGES_DEBUG");
		g_log_set_handler("ai-glib", G_LOG_LEVEL_INFO, info_log, NULL);
		g_log("ai-glib", G_LOG_LEVEL_INFO, "media lifecycle diagnostic");
		return;
	}
	g_test_trap_subprocess(NULL, 3000000, 0);
	g_test_trap_assert_passed();
	g_test_trap_assert_stderr("*ai-glib INFO: media lifecycle diagnostic*");
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/matrix/info-without-debug", info_visible);
	g_test_add("/voice/matrix/terminal-media-error", MatrixFixture, NULL, matrix_setup,
			   terminal_media_error_test, matrix_teardown);
	g_test_add("/voice/matrix/sigterm-goodbye", MatrixFixture, GINT_TO_POINTER(SIGTERM),
			   matrix_setup, signal_goodbye, matrix_teardown);
	g_test_add("/voice/matrix/sigint-goodbye", MatrixFixture, GINT_TO_POINTER(SIGINT),
			   matrix_setup, signal_goodbye, matrix_teardown);
	g_test_add("/voice/matrix/goodbye-busy-timeout", MatrixFixture, GINT_TO_POINTER(-1),
			   matrix_setup, signal_goodbye, matrix_teardown);
	g_test_add("/voice/matrix/goodbye-timeout", MatrixFixture, NULL, matrix_setup,
			   signal_goodbye, matrix_teardown);
	g_test_add("/voice/matrix/live-text-off", MatrixFixture, GINT_TO_POINTER(0),
			   matrix_setup, live_text, matrix_teardown);
	g_test_add("/voice/matrix/live-text-replies", MatrixFixture, GINT_TO_POINTER(1),
			   matrix_setup, live_text, matrix_teardown);
	g_test_add("/voice/matrix/live-text-both", MatrixFixture, GINT_TO_POINTER(2),
			   matrix_setup, live_text, matrix_teardown);
	g_test_add("/voice/matrix/voice-command", MatrixFixture, NULL, matrix_setup,
			   voice_command_call, matrix_teardown);
	g_test_add("/voice/matrix/local-session", MatrixFixture, NULL, matrix_setup,
			   local_session, matrix_teardown);
	g_test_add("/voice/matrix/state-file", MatrixFixture, NULL, matrix_setup, state_file,
			   matrix_teardown);
	g_test_add("/voice/matrix/transcript-on-disk", MatrixFixture, NULL, matrix_setup,
			   transcript_on_disk, matrix_teardown);
	g_test_add("/voice/matrix/answer-cleanup", MatrixFixture, NULL, matrix_setup,
			   answer_cleanup, matrix_teardown);
	g_test_add("/voice/matrix/cleanup-retry", MatrixFixture, GINT_TO_POINTER(1),
			   matrix_setup, answer_cleanup, matrix_teardown);
	g_test_add("/voice/matrix/cleanup-refused", MatrixFixture, GINT_TO_POINTER(2),
			   matrix_setup, answer_cleanup, matrix_teardown);
	g_test_add("/voice/matrix/stale-clear", MatrixFixture, NULL, matrix_setup, stale_clear,
			   matrix_teardown);
	g_test_add("/voice/matrix/transcript-write-failure", MatrixFixture, NULL, matrix_setup,
			   transcript_write_failure, matrix_teardown);
	g_test_add("/voice/matrix/drop-oversized", MatrixFixture, NULL, matrix_setup,
			   drop_oversized, matrix_teardown);
	g_test_add("/voice/matrix/stale-membership", MatrixFixture, NULL, matrix_setup,
			   stale_membership, matrix_teardown);
	return g_test_run();
}
