/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice-mocks.h"
#include <libsoup/soup.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gio/gio.h>
#include <math.h>

/* Observe the real transport appsrc without a test-only library API. */
typedef struct {
	GstClockTime next;
	gboolean started;
} Timeline;
static gint timestamp_gaps;
static gint timestamp_buffers;
static GstElement *publisher_pipeline;
static gint low100, low200, dsp_count;
static guint media_created, recovery_started;
static void
recovering(AiAudioTransport *transport, gpointer data)
{
	recovery_started++;
}
static GstPadProbeReturn
check_timeline(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
	Timeline *timeline = data;
	GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
	if (!GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) ||
		(timeline->started && GST_BUFFER_PTS(buffer) != timeline->next)) {
		g_test_message("Timestamp gap: expected %" G_GUINT64_FORMAT
					   " got %" G_GUINT64_FORMAT,
					   timeline->next, GST_BUFFER_PTS(buffer));
		g_atomic_int_inc(&timestamp_gaps);
	}
	timeline->next = GST_BUFFER_PTS(buffer) + GST_BUFFER_DURATION(buffer);
	timeline->started = TRUE;
	g_atomic_int_inc(&timestamp_buffers);
	return GST_PAD_PROBE_OK;
}
static gboolean
element_added(GSignalInvocationHint *hint, guint n, const GValue *values, gpointer data)
{
	GstElement *element = g_value_get_object(&values[1]);
	GstElementFactory *factory = gst_element_get_factory(element);
	if (factory != NULL &&
		g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)),
				  "webrtcdsp") == 0)
		g_atomic_int_inc(&dsp_count);
	if (factory != NULL && g_strcmp0(GST_ELEMENT_NAME(element), "voice-output") == 0 &&
		g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "appsrc") ==
			0) {
		GstPad *pad = gst_element_get_static_pad(element, "src");
		media_created++;
		gst_clear_object(&publisher_pipeline);
		publisher_pipeline = GST_ELEMENT(gst_object_get_parent(GST_OBJECT(element)));
		gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, check_timeline,
						  g_new0(Timeline, 1), g_free);
		gst_object_unref(pad);
	}
	return TRUE;
}
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
	const guint8 end[] = {0, 0, 0, 0};
	guint i, offset;
	SoupMessageBody *request = soup_server_message_get_request_body(message);
	gboolean low = g_strstr_len(request->data, request->length, "low") != NULL;
	guint total =
		g_strstr_len(request->data, request->length, "long") != NULL ? 720000 : 24000;
	g_byte_array_append(body, (const guint8 *)"SR=24000\n", 9);
	/* Non-10ms synthesis frames exercise partial transport buffers. */
	for (offset = 0; offset < total; offset += 333) {
		guint count = MIN(333, total - offset);
		guint32 size = GUINT32_TO_BE(count * 2);
		g_byte_array_append(body, (const guint8 *)&size, 4);
		for (i = offset; i < offset + count; i++) {
			gint16 value = low ? (gint16)(4000 * sin(2 * G_PI * (i % 240) / 240) +
										  4000 * sin(2 * G_PI * (i % 120) / 120))
							   : (gint16)(8000 * sin(2 * G_PI * 5 * (i % 12) / 12));
			guint8 sample[] = {(guint16)value & 255, (guint16)value >> 8};
			g_byte_array_append(body, sample, 2);
		}
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
	gdouble real = 0, imaginary = 0;
	gdouble r100 = 0, i100 = 0, r200 = 0, i200 = 0;
	if (sample == NULL)
		return GST_FLOW_EOS;
	buffer = gst_sample_get_buffer(sample);
	if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
		for (i = 0; i + 1 < map.size; i += 2) {
			gint32 value =
				(gint16)((guint16)map.data[i] | ((guint16)map.data[i + 1] << 8));
			r100 += value * cos(2 * G_PI * ((i / 2) % 480) / 480);
			i100 += value * sin(2 * G_PI * ((i / 2) % 480) / 480);
			r200 += value * cos(2 * G_PI * ((i / 2) % 240) / 240);
			i200 += value * sin(2 * G_PI * ((i / 2) % 240) / 240);
			real += value * cos(2 * G_PI * 5 * ((i / 2) % 24) / 24);
			imaginary += value * sin(2 * G_PI * 5 * ((i / 2) % 24) / 24);
		}
		/* At least half a second of 10 kHz tone, not a lower-frequency alias. */
		if (map.size > 0 &&
			(real * real + imaginary * imaginary) / ((map.size / 2) * (map.size / 2)) >
				250000)
			g_atomic_int_add((gint *)data, map.size / 2);
		/* Source amplitude is 4000 per tone; 3 dB power ratio is 0.501187. */
		if (map.size >= 1920) {
			gdouble scale = (map.size / 2) * (map.size / 2);
			if ((r100 * r100 + i100 * i100) / scale > 2004749)
				g_atomic_int_add(&low100, map.size / 2);
			if ((r200 * r200 + i200 * i200) / scale > 2004749)
				g_atomic_int_add(&low200, map.size / 2);
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
									   "audio/x-raw,format=S16LE,rate=48000,channels=1 ! "
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
	g_autoptr(GSocketListener) ports = NULL;
	g_autofree gchar *server_url = NULL, *server_config = NULL;
	guint http_port, tcp_port, udp_port;
	guint added_signal;
	gulong hook;
	GstElement *observer, *observer_source;
	GstElement *main_pipeline;
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
	added_signal = g_signal_lookup("element-added", GST_TYPE_BIN);
	hook = g_signal_add_emission_hook(added_signal, 0, element_added, NULL, NULL);
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
	g_subprocess_launcher_set_stdout_file_path(launcher,
											   "/tmp/ai-voice-livekit-test-server.log");
	g_subprocess_launcher_set_stderr_file_path(
		launcher, "/tmp/ai-voice-livekit-test-server-error.log");
	ports = g_socket_listener_new();
	http_port = g_socket_listener_add_any_inet_port(ports, NULL, &error);
	g_assert_no_error(error);
	tcp_port = g_socket_listener_add_any_inet_port(ports, NULL, &error);
	g_assert_no_error(error);
	udp_port = g_socket_listener_add_any_inet_port(ports, NULL, &error);
	g_assert_no_error(error);
	server_url = g_strdup_printf("ws://127.0.0.1:%u", http_port);
	server_config = g_strdup_printf("port: %u\nrtc:\n  tcp_port: %u\n  udp_port: %u\n",
									http_port, tcp_port, udp_port);
	g_socket_listener_close(ports);
	server = g_subprocess_launcher_spawn(launcher, &error, executable, "--dev", "--bind",
										 "127.0.0.1", "--node-ip", "127.0.0.1",
										 "--config-body", server_config, NULL);
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
	g_object_set(signaller, "ws-url", server_url, "auth-token", observer_token,
				 "room-name", "voice-test", NULL);
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
	transport = ai_livekit_transport_new(server_url, rx);
	loop = g_main_loop_new(NULL, FALSE);
	result.loop = loop;
	inbound.loop = loop;
	g_signal_connect(transport, "reconnecting", G_CALLBACK(recovering), NULL);
	g_signal_connect(transport, "audio", G_CALLBACK(received), &inbound);
	g_signal_connect(transport, "participant-joined", G_CALLBACK(participant_joined),
					 &inbound);
	timeout = g_timeout_add_seconds(100, expired, NULL);
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
	/* Deliberately delay dispatch; media timestamps must remain contiguous. */
	g_usleep(60000);
	limit = g_get_monotonic_time() + 5000000;
	while (g_atomic_int_get(&heard) < 24000 && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	g_assert_cmpint(g_atomic_int_get(&heard), >=, 24000);
	g_assert_cmpint(g_atomic_int_get(&timestamp_buffers), >, 50);
	g_assert_cmpint(g_atomic_int_get(&timestamp_gaps), ==, 0);
	g_test_message("Greeting: %d samples in packets with 10 kHz amplitude > 1000; %d "
				   "appsrc buffers, "
				   "zero timestamp gaps",
				   g_atomic_int_get(&heard), g_atomic_int_get(&timestamp_buffers));
	limit = g_get_monotonic_time() + 3000000;
	while (ai_voice_session_get_state(session) != AI_VOICE_LISTENING &&
		   g_get_monotonic_time() < limit)
		g_main_context_iteration(NULL, TRUE);
	g_atomic_int_set(&heard, 0);
	ai_voice_session_say(session, "A long test reply.");
	limit = g_get_monotonic_time() + 40000000;
	while ((g_atomic_int_get(&heard) < 1400000 ||
			ai_voice_session_get_state(session) != AI_VOICE_LISTENING) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	g_assert_cmpint(g_atomic_int_get(&heard), >=, 1400000);
	g_assert_cmpint(ai_voice_session_get_state(session), ==, AI_VOICE_LISTENING);
	g_test_message("30 second reply: %d decoded tone samples", g_atomic_int_get(&heard));
	/* A publisher bus error must not permanently stop the voice session. */
	{
		g_autoptr(GError) injected = g_error_new_literal(
			GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, "Injected publisher failure");
		g_test_expect_message(
			"ai-glib", G_LOG_LEVEL_INFO,
			"*element=*Injected publisher failure*debug=publisher regression detail*");
		gst_element_post_message(publisher_pipeline,
								 gst_message_new_error(GST_OBJECT(publisher_pipeline),
													   injected,
													   "publisher regression detail"));
	}
	limit = g_get_monotonic_time() + 5000000;
	while (g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	g_test_assert_expected_messages();
	g_atomic_int_set(&heard, 0);
	ai_voice_session_say(session, "After recovery.");
	limit = g_get_monotonic_time() + 5000000;
	while (g_atomic_int_get(&heard) < 24000 && g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	g_assert_cmpint(g_atomic_int_get(&heard), >=, 24000);
	limit = g_get_monotonic_time() + 3000000;
	while (ai_voice_session_get_state(session) != AI_VOICE_LISTENING &&
		   g_get_monotonic_time() < limit)
		g_main_context_iteration(NULL, TRUE);
	ai_voice_session_say(session, "A low frequency test.");
	limit = g_get_monotonic_time() + 5000000;
	while ((g_atomic_int_get(&low100) < 24000 || g_atomic_int_get(&low200) < 24000) &&
		   g_get_monotonic_time() < limit) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	g_assert_cmpint(g_atomic_int_get(&low100), >=, 24000);
	g_assert_cmpint(g_atomic_int_get(&low200), >=, 24000);
	g_assert_cmpint(g_atomic_int_get(&dsp_count), ==, 0);
	g_test_message("100/200 Hz: %d/%d samples with less than 3 dB loss", low100, low200);
	main_pipeline = gst_object_ref(publisher_pipeline);
	ai_voice_session_stop(session);
	g_assert_cmpuint(inbound.participants, ==, 0);
	g_assert_false(inbound.audio);
	peer_rx = jwt("peer-receiver");
	peer_tx = jwt("peer-publisher");
	peer = ai_livekit_transport_new(server_url, peer_rx);
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
	{
		g_autoptr(GError) injected = g_error_new_literal(
			GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED, "Cancel pending recovery");
		gst_element_post_message(main_pipeline,
								 gst_message_new_error(GST_OBJECT(main_pipeline),
													   injected, "leave regression"));
	}
	limit = g_get_monotonic_time() + 1000000;
	while (recovery_started < 2 && g_get_monotonic_time() < limit)
		g_main_context_iteration(NULL, TRUE);
	g_assert_cmpuint(recovery_started, ==, 2);
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(transport), NULL, left, &result);
	g_main_loop_run(loop);
	{
		guint before = media_created;
		limit = g_get_monotonic_time() + 1000000;
		while (g_get_monotonic_time() < limit) {
			while (g_main_context_iteration(NULL, FALSE))
				;
			g_usleep(1000);
		}
		g_assert_cmpuint(media_created, ==, before);
	}
	gst_object_unref(main_pipeline);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	g_assert_true(result.ok);
	gst_element_set_state(observer, GST_STATE_NULL);
	gst_object_unref(observer);
	soup_server_disconnect(speech_server);
	g_signal_remove_emission_hook(added_signal, hook);
	gst_clear_object(&publisher_pipeline);
	g_assert_cmpint(g_atomic_int_get(&timestamp_gaps), ==, 0);
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
