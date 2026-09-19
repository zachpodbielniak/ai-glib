/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice-mocks.h"
#include <libsoup/soup.h>
#include <stdarg.h>
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
	if (type == AI_TYPE_LIVEKIT_TRANSPORT)
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
	guint publications, clears, jwt_requests;
	gboolean fail_clear;
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
	else if (strstr(path, "/messages") != NULL)
		body = "{\"chunk\":[]}";
	else if (strstr(path, "/state/" MEMBER) != NULL) {
		g_autoptr(JsonParser) parser = json_parser_new();
		SoupMessageBody *request_body = soup_server_message_get_request_body(message);
		g_assert_true(json_parser_load_from_data(parser, request_body->data,
												 request_body->length, NULL));
		if (json_object_get_size(ai_json_root_object(parser)) == 0) {
			f->clears++;
			if (f->fail_clear) {
				f->fail_clear = FALSE;
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
	g_assert_cmpuint(f->clears, ==, GPOINTER_TO_INT(data) ? 2 : 1);
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
	g_test_add("/voice/matrix/answer-cleanup", MatrixFixture, NULL, matrix_setup,
			   answer_cleanup, matrix_teardown);
	g_test_add("/voice/matrix/cleanup-retry", MatrixFixture, GINT_TO_POINTER(1),
			   matrix_setup, answer_cleanup, matrix_teardown);
	g_test_add("/voice/matrix/stale-membership", MatrixFixture, NULL, matrix_setup,
			   stale_membership, matrix_teardown);
	return g_test_run();
}
