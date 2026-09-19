/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice-mocks.h"
#include <libsoup/soup.h>

typedef struct {
	SoupServer *server;
	guint requests, errors, replies, endings;
	gboolean broken, ready;
	GPtrArray *connections;
	gchar *last_error;
} FailureServer;

static void
synthesis_request(SoupServer *server, SoupServerMessage *message, const gchar *path,
				  GHashTable *query, gpointer data)
{
	FailureServer *f = data;
	static const guint8 pcm[] = {'S', 'R', '=', '1', '6', '0', '0', '0', '\n', 0, 0,
								 0,	  4,   1,	0,	 2,	  0,   0,	0,	 0,	   0};
	f->requests++;
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "application/octet-stream",
									 SOUP_MEMORY_COPY, (const gchar *)pcm,
									 f->broken ? sizeof(pcm) - 4 : sizeof(pcm));
}
static void
voice_error(AiVoiceSession *session, GError *error, gpointer data)
{
	FailureServer *f = data;
	f->errors++;
	g_free(f->last_error);
	f->last_error = g_strdup(error->message);
}
static void
reply(AiVoiceSession *session, const gchar *text, gpointer data)
{
	FailureServer *f = data;
	f->replies++;
	if (strstr(text, "Queued ending") != NULL)
		f->endings++;
}
static void
wait_idle(AiVoiceSession *session, FailureServer *f, guint replies)
{
	gint64 limit = g_get_monotonic_time() + 3000000;
	while ((f->replies < replies ||
			ai_voice_session_get_state(session) != AI_VOICE_LISTENING) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	g_assert_cmpuint(f->replies, ==, replies);
	g_assert_cmpint(ai_voice_session_get_state(session), ==, AI_VOICE_LISTENING);
}
static void
stt_message(SoupWebsocketConnection *ws, gint type, GBytes *bytes, gpointer data)
{
	if (type == SOUP_WEBSOCKET_DATA_TEXT)
		soup_websocket_connection_send_text(ws,
											"{\"type\":\"final\",\"text\":\"hello\"}");
}
static void
stt_connected(SoupServer *server, SoupServerMessage *message, const gchar *path,
			  SoupWebsocketConnection *ws, gpointer data)
{
	FailureServer *f = data;
	g_ptr_array_add(f->connections, g_object_ref(ws));
	g_signal_connect(ws, "message", G_CALLBACK(stt_message), f);
	if (f->ready)
		soup_websocket_connection_send_text(ws, "{\"type\":\"ready\"}");
}
static void
frame(AiAudioTransport *transport, guint8 value)
{
	guint8 raw[320] = {0};
	g_autoptr(GBytes) pcm = NULL;
	raw[0] = value;
	pcm = g_bytes_new(raw, sizeof(raw));
	g_signal_emit_by_name(transport, "audio", "caller", pcm);
}
static void
speech_failure(gconstpointer data)
{
	FailureServer f = {0};
	gboolean stt_timeout = GPOINTER_TO_INT(data) == 2;
	gboolean disabled = GPOINTER_TO_INT(data) == 3;
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autoptr(GSocketListener) reservation = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(reservation, NULL, NULL);
	g_autofree gchar *tts_url = g_strdup_printf("http://127.0.0.1:%u/tts", port);
	g_autofree gchar *stt_url = g_strdup_printf("ws://127.0.0.1:%u/stt", port);
	g_autoptr(AiHttpSynthesizer) tts = ai_http_synthesizer_new(tts_url);
	g_autoptr(AiWebsocketRecognizer) real_stt = ai_websocket_recognizer_new(stt_url);
	g_autoptr(GObject) transport = g_object_new(test_transport_get_type(), NULL);
	g_autoptr(GObject) recognizer = g_object_new(test_recognizer_get_type(), NULL);
	g_autoptr(GObject) activity = g_object_new(test_activity_get_type(), NULL);
	g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
	g_autoptr(AiConversation) conversation = ai_conversation_new(G_OBJECT(provider));
	g_autoptr(AiVoiceSession) session = ai_voice_session_new(
		AI_AUDIO_TRANSPORT(transport),
		stt_timeout ? AI_SPEECH_RECOGNIZER(real_stt) : AI_SPEECH_RECOGNIZER(recognizer),
		AI_SPEECH_SYNTHESIZER(tts), AI_VOICE_ACTIVITY(activity), conversation);
	static const guint8 cached[] = {3, 0, 4, 0, 5, 0, 6, 0};
	g_autoptr(GBytes) fallback = g_bytes_new_static(cached, sizeof(cached));
	guint writes;
	f.server = server;
	f.connections = g_ptr_array_new_with_free_func(g_object_unref);
	f.broken = GPOINTER_TO_INT(data) == 1 || disabled;
	g_socket_listener_close(reservation);
	soup_server_add_handler(server, "/tts", synthesis_request, &f, NULL);
	soup_server_add_websocket_handler(server, "/stt", NULL, NULL, stt_connected, &f,
									  NULL);
	if (GPOINTER_TO_INT(data) != 0)
		g_assert_true(soup_server_listen_local(server, port, 0, NULL));
	if (g_object_class_find_property(G_OBJECT_GET_CLASS(session), "fallback-pcm") != NULL)
		g_object_set(session, "fallback-pcm", fallback, "fallback-sample-rate", 16000,
					 NULL);
	g_object_set(session, "barge-in-ms", 10, NULL);
	if (disabled)
		g_object_set(session, "synthesis-error-message", "", NULL);
	g_object_set(real_stt, "timeout-ms", 40, NULL);
	g_signal_connect(session, "error", G_CALLBACK(voice_error), &f);
	g_signal_connect(session, "reply", G_CALLBACK(reply), &f);
	g_signal_emit_by_name(transport, "participant-joined", "caller", "Caller");
	if (stt_timeout) {
		frame(AI_AUDIO_TRANSPORT(transport), 1);
		frame(AI_AUDIO_TRANSPORT(transport), 2);
		wait_idle(session, &f, 1);
	} else {
		ai_mock_provider_push_text(provider, "Initial utterance. Queued ending.");
		frame(AI_AUDIO_TRANSPORT(transport), 1);
		frame(AI_AUDIO_TRANSPORT(transport), 2);
		wait_idle(session, &f, disabled ? 1 : 2);
	}
	g_assert_cmpuint(f.endings, ==, 0);
	{
		GList *l;
		for (l = ai_conversation_get_messages(conversation); l != NULL; l = l->next) {
			g_autofree gchar *text = ai_message_get_text(l->data);
			if (text != NULL) {
				g_assert_null(strstr(text, "Queued ending"));
				g_assert_null(strstr(text, "Initial utterance"));
			}
		}
	}
	g_assert_cmpuint(f.errors, ==, 1);
	g_assert_nonnull(f.last_error);
	if (f.broken)
		g_assert_nonnull(strstr(f.last_error, "zero sentinel"));
	if (stt_timeout)
		g_assert_nonnull(strstr(f.last_error, "timed out"));
	g_assert_cmpuint(((TestTransport *)transport)->non_silent_samples, >=,
					 (stt_timeout || disabled) ? 2 : 4);
	if (disabled)
		g_assert_cmpuint(((TestTransport *)transport)->non_silent_samples, ==, 2);
	writes = ((TestTransport *)transport)->writes;
	f.broken = FALSE;
	f.ready = TRUE;
	if (GPOINTER_TO_INT(data) == 0)
		g_assert_true(soup_server_listen_local(server, port, 0, NULL));
	ai_mock_provider_push_text(provider, "The next turn works.");
	frame(AI_AUDIO_TRANSPORT(transport), 1);
	frame(AI_AUDIO_TRANSPORT(transport), 2);
	wait_idle(session, &f, (stt_timeout || disabled) ? 2 : 3);
	g_assert_cmpuint(((TestTransport *)transport)->writes, >, writes);
	g_assert_cmpuint(f.errors, ==, 1);
	g_assert_cmpuint(f.requests, ==, GPOINTER_TO_INT(data) == 0 ? 1 : 2);
	ai_voice_session_stop(session);
	while (g_main_context_iteration(NULL, FALSE))
		;
	soup_server_disconnect(server);
	g_ptr_array_unref(f.connections);
	g_free(f.last_error);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/voice/failures/tts-unreachable", GINT_TO_POINTER(0),
						 speech_failure);
	g_test_add_data_func("/voice/failures/tts-midstream-eof", GINT_TO_POINTER(1),
						 speech_failure);
	g_test_add_data_func("/voice/failures/stt-ready-timeout", GINT_TO_POINTER(2),
						 speech_failure);
	g_test_add_data_func("/voice/failures/tts-fallback-disabled", GINT_TO_POINTER(3),
						 speech_failure);
	return g_test_run();
}
