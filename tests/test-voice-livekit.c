/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice-mocks.h"
#include <libsoup/soup.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gio/gio.h>

typedef struct {
	GMainLoop *loop;
	gboolean ok;
	GError *error;
	gboolean audio;
	guint participants;
} Result;
static gchar *
base64url(const guchar *data, gsize size)
{
	gchar *s = g_base64_encode(data, size), *p;
	for (p = s; *p; p++) {
		if (*p == '+')
			*p = '-';
		else if (*p == '/')
			*p = '_';
		else if (*p == '=') {
			*p = '\0';
			break;
		}
	}
	return s;
}
static gchar *
jwt(const gchar *identity)
{
	static const gchar header[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
	static const gchar secret[] = "secret";
	g_autofree gchar *body = g_strdup_printf(
		"{\"iss\":\"devkey\",\"sub\":\"%s\",\"name\":\"%s\",\"exp\":%" G_GINT64_FORMAT
		",\"video\":{\"roomJoin\":true,\"room\":\"voice-test\",\"canPublish\":true,"
		"\"canSubscribe\":true}}",
		identity, identity, g_get_real_time() / G_USEC_PER_SEC + 600);
	g_autofree gchar *a = base64url((const guchar *)header, strlen(header));
	g_autofree gchar *b = base64url((const guchar *)body, strlen(body));
	g_autofree gchar *unsigned_token = g_strconcat(a, ".", b, NULL);
	g_autoptr(GHmac) hmac =
		g_hmac_new(G_CHECKSUM_SHA256, (const guchar *)secret, strlen(secret));
	guint8 digest[32];
	gsize size = sizeof(digest);
	g_autofree gchar *signature = NULL;
	g_hmac_update(hmac, (const guchar *)unsigned_token, strlen(unsigned_token));
	g_hmac_get_digest(hmac, digest, &size);
	signature = base64url(digest, size);
	return g_strconcat(unsigned_token, ".", signature, NULL);
}
static void
joined(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->ok = ai_audio_transport_join_finish(AI_AUDIO_TRANSPORT(source), result, &r->error);
	g_main_loop_quit(r->loop);
}
static void
left(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->ok =
		ai_audio_transport_leave_finish(AI_AUDIO_TRANSPORT(source), result, &r->error);
	g_main_loop_quit(r->loop);
}
static gboolean
expired(gpointer data)
{
	g_error("LiveKit test timed out");
	return G_SOURCE_REMOVE;
}
static void
received(AiAudioTransport *transport, const gchar *speaker, GBytes *pcm, gpointer data)
{
	Result *r = data;
	gsize n, i;
	const guint8 *p = g_bytes_get_data(pcm, &n);
	g_assert_cmpuint(n % 2, ==, 0);
	for (i = 0; i + 1 < n; i += 2)
		if (ABS((gint16)((guint16)p[i] | ((guint16)p[i + 1] << 8))) > 1000) {
			r->audio = TRUE;
			break;
		}
}
static void
participant_joined(AiAudioTransport *transport, const gchar *id, const gchar *name,
				   gpointer data)
{
	((Result *)data)->participants++;
}
static void
wrote(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->ok =
		ai_audio_transport_write_finish(AI_AUDIO_TRANSPORT(source), result, &r->error);
	g_main_loop_quit(r->loop);
}
static void
synthesis_request(SoupServer *server, SoupServerMessage *message, const gchar *path,
				  GHashTable *query, gpointer data)
{
	g_autoptr(GByteArray) body = g_byte_array_new();
	const guint8 size[] = {0, 0, 0x7d, 0};
	const guint8 end[] = {0, 0, 0, 0};
	guint i;
	g_byte_array_append(body, (const guint8 *)"SR=16000\n", 9);
	g_byte_array_append(body, size, sizeof(size));
	for (i = 0; i < 16000; i++) {
		gint16 value = i % 40 < 20 ? 8000 : -8000;
		guint8 sample[] = {(guint16)value & 255, (guint16)value >> 8};
		g_byte_array_append(body, sample, 2);
	}
	g_byte_array_append(body, end, sizeof(end));
	soup_server_message_set_status(message, 200, NULL);
	soup_server_message_set_response(message, "application/octet-stream",
									 SOUP_MEMORY_COPY, (const gchar *)body->data,
									 body->len);
}
static GstFlowReturn
observer_sample(GstAppSink *sink, gpointer data)
{
	GstSample *sample = gst_app_sink_pull_sample(sink);
	GstMapInfo map;
	GstBuffer *buffer;
	guint i;
	if (sample == NULL)
		return GST_FLOW_EOS;
	buffer = gst_sample_get_buffer(sample);
	if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
		for (i = 0; i + 1 < map.size; i += 2)
			if (ABS((gint16)((guint16)map.data[i] | ((guint16)map.data[i + 1] << 8))) >
				1000) {
				g_atomic_int_set((gint *)data, 1);
				break;
			}
		gst_buffer_unmap(buffer, &map);
	}
	gst_sample_unref(sample);
	return GST_FLOW_OK;
}
static void
observer_pad(GstElement *source, GstPad *pad, gpointer data)
{
	GstElement *bin, *sink;
	GstObject *pipeline;
	GstPad *target;
	if (!g_str_has_prefix(GST_PAD_NAME(pad), "audio_"))
		return;
	bin =
		gst_parse_bin_from_description("queue ! audioconvert ! audioresample ! "
									   "audio/x-raw,format=S16LE,rate=16000,channels=1 ! "
									   "appsink name=pcm sync=false emit-signals=true",
									   TRUE, NULL);
	sink = gst_bin_get_by_name(GST_BIN(bin), "pcm");
	g_signal_connect(sink, "new-sample", G_CALLBACK(observer_sample), data);
	pipeline = gst_object_get_parent(GST_OBJECT(source));
	gst_bin_add(GST_BIN(pipeline), bin);
	target = gst_element_get_static_pad(bin, "sink");
	g_assert_cmpint(gst_pad_link(pad, target), ==, GST_PAD_LINK_OK);
	gst_element_sync_state_with_parent(bin);
	gst_object_unref(target);
	gst_object_unref(sink);
	gst_object_unref(pipeline);
}
static void
test_livekit(void)
{
	g_autofree gchar *executable = g_find_program_in_path("livekit-server");
	g_autofree gchar *rx = NULL, *tx = NULL;
	g_autoptr(GSubprocess) server = NULL;
	g_autoptr(AiLivekitTransport) transport = NULL, peer = NULL;
	g_autofree gchar *peer_rx = NULL, *peer_tx = NULL;
	g_autoptr(GBytes) pcm = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	Result result = {0}, inbound = {0};
	guint timeout;
	GstElement *observer, *observer_source;
	g_autoptr(GObject) signaller = NULL;
	g_autoptr(SoupServer) speech_server = NULL;
	g_autofree gchar *speech_url = NULL, *observer_token = NULL;
	g_autoptr(AiVoiceSession) session = NULL;
	g_autoptr(AiHttpSynthesizer) synth = NULL;
	g_autoptr(GObject) stt = NULL, vad = NULL;
	g_autoptr(AiMockProvider) provider = NULL;
	g_autoptr(AiConversation) conversation = NULL;
	gint heard = 0, connected = 0;
	gint64 limit;
	if (executable == NULL || !ai_livekit_transport_is_available()) {
		g_test_skip("livekit-server or GStreamer LiveKit plugin is absent");
		return;
	}
	if (g_getenv("AI_VOICE_LIVEKIT_TEST") == NULL) {
		g_test_skip("Set AI_VOICE_LIVEKIT_TEST=1 for the local media integration test");
		return;
	}
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
	g_subprocess_launcher_set_stdout_file_path(launcher,
											   "/tmp/ai-voice-livekit-test-server.log");
	g_subprocess_launcher_set_stderr_file_path(
		launcher, "/tmp/ai-voice-livekit-test-server-error.log");
	server = g_subprocess_launcher_spawn(
		launcher, &error, executable, "--dev", "--bind", "127.0.0.1", "--node-ip",
		"127.0.0.1", "--config-body",
		"port: 17980\nrtc:\n  tcp_port: 17981\n  udp_port: 17982\n", NULL);
	g_assert_no_error(error);
	g_assert_nonnull(server);
	g_usleep(500000);
	/* The caller is receiving but has not published its microphone yet. */
	observer_token = jwt("listen-only-peer");
	observer = gst_pipeline_new(NULL);
	observer_source = gst_element_factory_make("livekitwebrtcsrc", NULL);
	gst_bin_add(GST_BIN(observer), observer_source);
	g_object_set(observer_source, "stun-server", NULL, NULL);
	g_object_get(observer_source, "signaller", &signaller, NULL);
	g_object_set(signaller, "ws-url", "ws://127.0.0.1:17980", "auth-token",
				 observer_token, "room-name", "voice-test", NULL);
	g_signal_connect(observer_source, "pad-added", G_CALLBACK(observer_pad), &heard);
	gst_element_set_state(observer, GST_STATE_PLAYING);
	limit = g_get_monotonic_time() + 5000000;
	while (!connected && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_object_get(signaller, "connection-state", &connected, NULL);
		g_usleep(1000);
	}
	g_assert_cmpint(connected, !=, 0);
	rx = jwt("receiver");
	tx = jwt("publisher");
	transport = ai_livekit_transport_new("ws://127.0.0.1:17980", rx);
	loop = g_main_loop_new(NULL, FALSE);
	result.loop = loop;
	inbound.loop = loop;
	g_signal_connect(transport, "audio", G_CALLBACK(received), &inbound);
	g_signal_connect(transport, "participant-joined", G_CALLBACK(participant_joined),
					 &inbound);
	timeout = g_timeout_add_seconds(20, expired, NULL);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(transport), "voice-test", tx, NULL,
								  joined, &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	g_assert_true(result.ok);
	speech_server = soup_server_new(NULL, NULL);
	soup_server_add_handler(speech_server, "/", synthesis_request, NULL, NULL);
	g_assert_true(soup_server_listen_local(speech_server, 0, 0, NULL));
	{
		GSList *uris = soup_server_get_uris(speech_server);
		speech_url = g_uri_to_string(uris->data);
		g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	}
	synth = ai_http_synthesizer_new(speech_url);
	stt = g_object_new(test_recognizer_get_type(), NULL);
	vad = g_object_new(test_activity_get_type(), NULL);
	provider = ai_mock_provider_new();
	conversation = ai_conversation_new(G_OBJECT(provider));
	session = ai_voice_session_new(
		AI_AUDIO_TRANSPORT(transport), AI_SPEECH_RECOGNIZER(stt),
		AI_SPEECH_SYNTHESIZER(synth), AI_VOICE_ACTIVITY(vad), conversation);
	ai_voice_session_say(session, "A generic test greeting.");
	limit = g_get_monotonic_time() + 5000000;
	while (!g_atomic_int_get(&heard) && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	g_assert_cmpint(g_atomic_int_get(&heard), ==, 1);
	ai_voice_session_stop(session);
	g_assert_cmpuint(inbound.participants, ==, 0);
	g_assert_false(inbound.audio);
	peer_rx = jwt("peer-receiver");
	peer_tx = jwt("peer-publisher");
	peer = ai_livekit_transport_new("ws://127.0.0.1:17980", peer_rx);
	g_signal_connect(peer, "audio", G_CALLBACK(received), &result);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(peer), "voice-test", peer_tx, NULL,
								  joined, &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	g_assert_true(result.ok);
	{
		guint8 samples[32000];
		guint i;
		for (i = 0; i < sizeof(samples); i += 2) {
			gint16 value = (i / 2 % 40 < 20) ? 8000 : -8000;
			samples[i] = (guint16)value & 255;
			samples[i + 1] = (guint16)value >> 8;
		}
		pcm = g_bytes_new(samples, sizeof(samples));
	}
	ai_audio_transport_write_async(AI_AUDIO_TRANSPORT(transport), pcm, NULL, wrote,
								   &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	g_assert_true(result.ok);
	g_assert_true(result.audio);
	ai_audio_transport_write_async(AI_AUDIO_TRANSPORT(peer), pcm, NULL, wrote, &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	g_assert_true(inbound.audio);
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(peer), NULL, left, &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(transport), NULL, left, &result);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	g_assert_true(result.ok);
	gst_element_set_state(observer, GST_STATE_NULL);
	gst_object_unref(observer);
	soup_server_disconnect(speech_server);
	g_subprocess_send_signal(server, 15);
	g_subprocess_wait(server, NULL, NULL);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/voice/livekit/join-leave", test_livekit);
	return g_test_run();
}
