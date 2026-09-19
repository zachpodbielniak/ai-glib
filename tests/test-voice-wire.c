/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <libsoup/soup.h>

typedef struct {
	GMainLoop *loop;
	SoupServer *server;
	SoupWebsocketConnection *ws;
	gchar *url;
	guint binary_frames;
	guint partials;
	guint finals;
	GByteArray *audio;
	gboolean done;
	GBytes *body;
	GError *error;
	const gchar *response;
} Wire;

static gboolean
expired(gpointer data)
{
	g_error("Voice wire test exceeded its deadline");
	return G_SOURCE_REMOVE;
}

static void
server_message(SoupWebsocketConnection *ws, gint type, GBytes *bytes, gpointer data)
{
	Wire *w = data;
	gsize len;
	const gchar *s = g_bytes_get_data(bytes, &len);
	if (type == SOUP_WEBSOCKET_DATA_BINARY) {
		g_assert_cmpuint(len, ==, 320);
		w->binary_frames++;
		soup_websocket_connection_send_text(
			ws, "{\"type\":\"partial\",\"text\":\"hello\",\"stable_ms\":120}");
	} else {
		g_assert_cmpmem(s, len, "EOS", 3);
		g_assert_cmpuint(w->binary_frames, ==, 1);
		soup_websocket_connection_send_text(
			ws, w->response != NULL
					? w->response
					: "{\"type\":\"final\",\"text\":\"hello "
					  "world\",\"start\":0.25,\"end\":1.5,\"transcribe_ms\":12}");
	}
}

static void
connected(SoupServer *server, SoupServerMessage *msg, const gchar *path,
		  SoupWebsocketConnection *ws, gpointer data)
{
	((Wire *)data)->ws = g_object_ref(ws);
	g_signal_connect(ws, "message", G_CALLBACK(server_message), data);
	soup_websocket_connection_send_text(ws, "{\"type\":\"ready\",\"sample_rate\":16000}");
}

static void
transcript(AiSpeechRecognizer *stt, const gchar *id, const gchar *text, gboolean final,
		   gpointer data)
{
	Wire *w = data;
	g_assert_cmpstr(id, ==, "caller");
	g_assert_cmpstr(text, ==,
					final ? (w->response != NULL ? "" : "hello world") : "hello");
	if (final) {
		w->finals++;
		g_main_loop_quit(w->loop);
	} else
		w->partials++;
}

static void
http_request(SoupServer *server, SoupServerMessage *msg, const gchar *path,
			 GHashTable *query, gpointer data)
{
	static const guint8 body[] = {'S', 'R', '=', '1', '6', '0', '0', '0', '\n', 0, 0,
								  0,   4,	1,	 0,	  2,   0,	0,	 0,	  0,	0};
	SoupMessageBody *request = soup_server_message_get_request_body(msg);
	g_autoptr(JsonParser) parser = json_parser_new();
	g_assert_cmpstr(soup_server_message_get_method(msg), ==, "POST");
	g_assert_true(
		json_parser_load_from_data(parser, request->data, request->length, NULL));
	g_assert_cmpstr(json_object_get_string_member(
						json_node_get_object(json_parser_get_root(parser)), "text"),
					==, "hello world");
	soup_server_message_set_status(msg, 200, NULL);
	if (((Wire *)data)->body != NULL) {
		gsize size;
		const gchar *custom = g_bytes_get_data(((Wire *)data)->body, &size);
		soup_server_message_set_response(msg, "application/octet-stream",
										 SOUP_MEMORY_COPY, custom, size);
	} else
		soup_server_message_set_response(msg, "application/octet-stream",
										 SOUP_MEMORY_COPY, (const gchar *)body,
										 sizeof(body));
}

static void
setup(Wire *w)
{
	GSList *uris;
	w->loop = g_main_loop_new(NULL, FALSE);
	w->server = soup_server_new(NULL, NULL);
	g_assert_true(soup_server_listen_local(w->server, 0, 0, NULL));
	uris = soup_server_get_uris(w->server);
	w->url = g_uri_to_string(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	w->audio = g_byte_array_new();
}

static void
teardown(Wire *w)
{
	g_clear_object(&w->ws);
	soup_server_disconnect(w->server);
	g_object_unref(w->server);
	g_main_loop_unref(w->loop);
	g_free(w->url);
	g_byte_array_unref(w->audio);
	g_clear_error(&w->error);
	g_clear_pointer(&w->body, g_bytes_unref);
}

static void
test_stt(void)
{
	Wire w = {0};
	g_autoptr(AiWebsocketRecognizer) stt = NULL;
	g_autoptr(GBytes) pcm = NULL;
	g_autofree gchar *url = NULL;
	guint8 silence[320] = {0};
	guint timeout;
	setup(&w);
	soup_server_add_websocket_handler(w.server, "/stt/stream", NULL, NULL, connected, &w,
									  NULL);
	url = g_strconcat("ws", w.url + 4, "stt/stream", NULL);
	stt = ai_websocket_recognizer_new(url);
	g_signal_connect(stt, "transcript", G_CALLBACK(transcript), &w);
	pcm = g_bytes_new(silence, sizeof(silence));
	g_assert_true(
		ai_speech_recognizer_begin(AI_SPEECH_RECOGNIZER(stt), "caller", &w.error));
	g_assert_true(
		ai_speech_recognizer_feed(AI_SPEECH_RECOGNIZER(stt), "caller", pcm, &w.error));
	ai_speech_recognizer_end(AI_SPEECH_RECOGNIZER(stt), "caller");
	timeout = g_timeout_add_seconds(3, expired, NULL);
	g_main_loop_run(w.loop);
	g_source_remove(timeout);
	g_assert_no_error(w.error);
	g_assert_cmpuint(w.partials, ==, 1);
	g_assert_cmpuint(w.finals, ==, 1);
	g_clear_object(&stt);
	teardown(&w);
}

static void
silent_connected(SoupServer *server, SoupServerMessage *msg, const gchar *path,
				 SoupWebsocketConnection *ws, gpointer data)
{
	((Wire *)data)->ws = g_object_ref(ws);
}
static void
stt_failed(AiSpeechRecognizer *stt, const gchar *speaker, GError *error, gpointer data)
{
	Wire *w = data;
	g_assert_cmpstr(speaker, ==, "caller");
	w->error = g_error_copy(error);
	g_main_loop_quit(w->loop);
}
static void
test_stt_response(gconstpointer data)
{
	Wire w = {0};
	g_autoptr(AiWebsocketRecognizer) stt = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GBytes) pcm = NULL;
	guint8 silence[320] = {0};
	guint timeout;
	setup(&w);
	w.response = data;
	soup_server_add_websocket_handler(w.server, "/stt/stream", NULL, NULL, connected, &w,
									  NULL);
	url = g_strconcat("ws", w.url + 4, "stt/stream", NULL);
	stt = ai_websocket_recognizer_new(url);
	g_signal_connect(stt, "error", G_CALLBACK(stt_failed), &w);
	g_signal_connect(stt, "transcript", G_CALLBACK(transcript), &w);
	g_assert_true(ai_speech_recognizer_begin(AI_SPEECH_RECOGNIZER(stt), "caller", NULL));
	pcm = g_bytes_new(silence, sizeof(silence));
	g_assert_true(
		ai_speech_recognizer_feed(AI_SPEECH_RECOGNIZER(stt), "caller", pcm, NULL));
	ai_speech_recognizer_end(AI_SPEECH_RECOGNIZER(stt), "caller");
	timeout = g_timeout_add_seconds(3, expired, NULL);
	g_main_loop_run(w.loop);
	g_source_remove(timeout);
	if (strstr(w.response, "detail") != NULL) {
		g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_FAILED);
		g_assert_cmpstr(w.error->message, ==, "Recognition unavailable");
		g_assert_cmpuint(w.finals, ==, 0);
	} else {
		g_assert_no_error(w.error);
		g_assert_cmpuint(w.finals, ==, 1);
	}
	/* Completion, including silence or failure, releases just this stream. */
	g_assert_true(ai_speech_recognizer_begin(AI_SPEECH_RECOGNIZER(stt), "caller", NULL));
	ai_speech_recognizer_cancel(AI_SPEECH_RECOGNIZER(stt), "caller");
	g_clear_object(&stt);
	teardown(&w);
}

static void
test_stt_timeout(void)
{
	Wire w = {0};
	g_autoptr(AiWebsocketRecognizer) stt = NULL;
	g_autofree gchar *url = NULL;
	guint timeout;
	setup(&w);
	soup_server_add_websocket_handler(w.server, "/stt/stream", NULL, NULL,
									  silent_connected, &w, NULL);
	url = g_strconcat("ws", w.url + 4, "stt/stream", NULL);
	stt = ai_websocket_recognizer_new(url);
	g_object_set(stt, "timeout-ms", 30, NULL);
	g_signal_connect(stt, "error", G_CALLBACK(stt_failed), &w);
	g_assert_true(ai_speech_recognizer_begin(AI_SPEECH_RECOGNIZER(stt), "caller", NULL));
	timeout = g_timeout_add_seconds(3, expired, NULL);
	g_main_loop_run(w.loop);
	g_source_remove(timeout);
	g_assert_error(w.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
	g_clear_object(&stt);
	teardown(&w);
}

static void
audio_received(AiSpeechSynthesizer *tts, GBytes *pcm, gpointer data)
{
	Wire *w = data;
	gsize n;
	const guint8 *p = g_bytes_get_data(pcm, &n);
	g_byte_array_append(w->audio, p, n);
}

static void
synthesized(GObject *source, GAsyncResult *result, gpointer data)
{
	Wire *w = data;
	w->done = ai_speech_synthesizer_synthesize_finish(AI_SPEECH_SYNTHESIZER(source),
													  result, &w->error);
	g_main_loop_quit(w->loop);
}

static void
test_tts(void)
{
	Wire w = {0};
	g_autoptr(AiHttpSynthesizer) tts = NULL;
	g_autofree gchar *url = NULL;
	guint timeout;
	static const guint8 expected[] = {1, 0, 2, 0};
	setup(&w);
	soup_server_add_handler(w.server, "/synthesize/stream", http_request, &w, NULL);
	url = g_strconcat(w.url, "synthesize/stream", NULL);
	tts = ai_http_synthesizer_new(url);
	g_signal_connect(tts, "audio", G_CALLBACK(audio_received), &w);
	ai_speech_synthesizer_synthesize_async(AI_SPEECH_SYNTHESIZER(tts), "hello world",
										   NULL, synthesized, &w);
	timeout = g_timeout_add_seconds(3, expired, NULL);
	g_main_loop_run(w.loop);
	g_source_remove(timeout);
	g_assert_no_error(w.error);
	g_assert_true(w.done);
	g_assert_cmpmem(w.audio->data, w.audio->len, expected, sizeof(expected));
	g_clear_object(&tts);
	teardown(&w);
}

static void
test_tts_bad(gconstpointer data)
{
	Wire w = {0};
	g_autoptr(AiHttpSynthesizer) tts = NULL;
	g_autofree gchar *url = NULL;
	guint timeout;
	const gchar *body = data;
	setup(&w);
	w.body = g_bytes_new(body, strlen(body));
	soup_server_add_handler(w.server, "/synthesize/stream", http_request, &w, NULL);
	url = g_strconcat(w.url, "synthesize/stream", NULL);
	tts = ai_http_synthesizer_new(url);
	ai_speech_synthesizer_synthesize_async(AI_SPEECH_SYNTHESIZER(tts), "hello world",
										   NULL, synthesized, &w);
	timeout = g_timeout_add_seconds(3, expired, NULL);
	g_main_loop_run(w.loop);
	g_source_remove(timeout);
	g_assert_false(w.done);
	g_assert_nonnull(w.error);
	g_clear_object(&tts);
	teardown(&w);
}
static void
test_tts_resample(void)
{
	Wire w = {0};
	g_autoptr(AiHttpSynthesizer) tts = NULL;
	g_autofree gchar *url = NULL;
	g_autoptr(GByteArray) body = g_byte_array_new();
	guint timeout, i;
	const guint8 length[] = {0, 0, 0x25, 0x80};
	const guint8 zero[] = {0, 0, 0, 0};
	setup(&w);
	g_byte_array_append(body, (const guint8 *)"SR=24000\n", 9);
	g_byte_array_append(body, length, 4);
	for (i = 0; i < 4800; i++) {
		const guint8 sample[] = {0x34, 0x12};
		g_byte_array_append(body, sample, 2);
	}
	g_byte_array_append(body, zero, 4);
	w.body = g_byte_array_free_to_bytes(g_steal_pointer(&body));
	soup_server_add_handler(w.server, "/synthesize/stream", http_request, &w, NULL);
	url = g_strconcat(w.url, "synthesize/stream", NULL);
	tts = ai_http_synthesizer_new(url);
	g_signal_connect(tts, "audio", G_CALLBACK(audio_received), &w);
	ai_speech_synthesizer_synthesize_async(AI_SPEECH_SYNTHESIZER(tts), "hello world",
										   NULL, synthesized, &w);
	timeout = g_timeout_add_seconds(3, expired, NULL);
	g_main_loop_run(w.loop);
	g_source_remove(timeout);
	g_assert_no_error(w.error);
	g_assert_true(w.done);
	g_assert_cmpuint(w.audio->len, ==, 6400);
	for (i = 0; i < w.audio->len; i += 2) {
		g_assert_cmpuint(w.audio->data[i], ==, 0x34);
		g_assert_cmpuint(w.audio->data[i + 1], ==, 0x12);
	}
	g_clear_object(&tts);
	teardown(&w);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/wire/stt-ready-pcm-eos", test_stt);
	g_test_add_data_func("/voice/wire/stt-server-error",
						 "{\"type\":\"error\",\"detail\":\"Recognition unavailable\"}",
						 test_stt_response);
	g_test_add_data_func("/voice/wire/stt-empty-final",
						 "{\"type\":\"final\",\"text\":\"\",\"start\":0.0,\"end\":0.25,"
						 "\"transcribe_ms\":1}",
						 test_stt_response);
	g_test_add_func("/voice/wire/stt-ready-timeout", test_stt_timeout);
	g_test_add_func("/voice/wire/tts-framing", test_tts);
	g_test_add_data_func("/voice/wire/tts-invalid-rate", "SR=nope\n", test_tts_bad);
	g_test_add_data_func("/voice/wire/tts-missing-sentinel", "SR=16000\n", test_tts_bad);
	g_test_add_func("/voice/wire/tts-resample-fragmented-frame", test_tts_resample);
	return g_test_run();
}
