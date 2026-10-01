/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../bin/call/ai-call-speech-cache.h"
#include <libsoup/soup.h>

static gboolean completed;
static gboolean succeeded;
static void
done(AiCallSpeechCache *cache, gboolean success, gpointer data)
{
	completed = TRUE;
	succeeded = success;
}
static void
request(SoupServer *server, SoupServerMessage *msg, const gchar *path, GHashTable *query,
		gpointer data)
{
	static const guint8 body[] = {'S', 'R', '=', '2', '4', '0', '0', '0', '\n', 0, 0,
								  0,   4,	1,	 0,	  2,   0,	0,	 0,	  0,	0};
	soup_server_message_set_status(msg, 200, NULL);
	if (GPOINTER_TO_INT(data) == 3) {
		g_autoptr(GByteArray) large = g_byte_array_new();
		g_autofree guint8 *frame = g_malloc0(1024 * 1024);
		guint32 length = GUINT32_TO_BE(1024 * 1024);
		guint32 sentinel = 0;
		guint i;
		g_byte_array_append(large, body, 9);
		for (i = 0; i < 5; i++) {
			g_byte_array_append(large, (guint8 *)&length, 4);
			g_byte_array_append(large, frame, 1024 * 1024);
		}
		g_byte_array_append(large, (guint8 *)&sentinel, 4);
		soup_server_message_set_response(msg, "application/octet-stream",
										 SOUP_MEMORY_COPY, (const gchar *)large->data,
										 large->len);
		return;
	}
	soup_server_message_set_response(
		msg, "application/octet-stream", SOUP_MEMORY_COPY, (const gchar *)body,
		GPOINTER_TO_INT(data) ? sizeof(body) - 4 : sizeof(body));
}
static void
run(gconstpointer data)
{
	g_autoptr(SoupServer) server = soup_server_new(NULL, NULL);
	g_autoptr(AiCallSpeechCache) cache = NULL;
	g_autofree gchar *url = NULL;
	GSList *uris;
	gint64 until;
	completed = succeeded = FALSE;
	g_assert_true(soup_server_listen_local(server, 0, 0, NULL));
	uris = soup_server_get_uris(server);
	url = g_uri_to_string(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
	soup_server_add_handler(server, NULL, request, (gpointer)data, NULL);
	cache = ai_call_speech_cache_new(url, "Speech is temporarily unavailable.");
	g_signal_connect(cache, "completed", G_CALLBACK(done), NULL);
	if (!GPOINTER_TO_INT(data))
		g_test_expect_message("ai-call", G_LOG_LEVEL_INFO, "*Speech fallback prewarm ready:*");
	ai_call_speech_cache_start(cache);
	if (GPOINTER_TO_INT(data) == 2)
		ai_call_speech_cache_cancel(cache);
	until = g_get_monotonic_time() + 5000000;
	while (!completed && g_get_monotonic_time() < until)
		g_main_context_iteration(NULL, TRUE);
	g_assert_true(completed);
	if (!GPOINTER_TO_INT(data))
		g_test_assert_expected_messages();
	g_assert_cmpint(succeeded, ==, !GPOINTER_TO_INT(data));
	if (succeeded) {
		const guint8 expected[] = {1, 0, 2, 0};
		gsize size;
		const guint8 *pcm = g_bytes_get_data(ai_call_speech_cache_get_pcm(cache), &size);
		g_assert_cmpuint(ai_call_speech_cache_get_sample_rate(cache), ==, 24000);
		g_assert_cmpmem(pcm, size, expected, sizeof(expected));
	} else
		g_assert_null(ai_call_speech_cache_get_pcm(cache));
	soup_server_disconnect(server);
}
int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_data_func("/call/speech-cache/native-rate", GINT_TO_POINTER(0), run);
	g_test_add_data_func("/call/speech-cache/truncated", GINT_TO_POINTER(1), run);
	g_test_add_data_func("/call/speech-cache/cancelled", GINT_TO_POINTER(2), run);
	g_test_add_data_func("/call/speech-cache/bounded", GINT_TO_POINTER(3), run);
	return g_test_run();
}
