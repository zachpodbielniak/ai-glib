/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <ai-glib.h>
#include <gio/gio.h>

typedef struct {
	GMainLoop *loop;
	gboolean ok;
	GError *error;
	gboolean audio;
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
	for (i = 0; i < n; i++)
		if (p[i] != 0) {
			r->audio = TRUE;
			break;
		}
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
	Result result = {0};
	guint timeout;
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
	rx = jwt("receiver");
	tx = jwt("publisher");
	transport = ai_livekit_transport_new("ws://127.0.0.1:17980", rx);
	loop = g_main_loop_new(NULL, FALSE);
	result.loop = loop;
	timeout = g_timeout_add_seconds(20, expired, NULL);
	ai_audio_transport_join_async(AI_AUDIO_TRANSPORT(transport), "voice-test", tx, NULL,
								  joined, &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	g_assert_true(result.ok);
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
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(peer), NULL, left, &result);
	g_main_loop_run(loop);
	g_assert_no_error(result.error);
	ai_audio_transport_leave_async(AI_AUDIO_TRANSPORT(transport), NULL, left, &result);
	g_main_loop_run(loop);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	g_assert_true(result.ok);
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
