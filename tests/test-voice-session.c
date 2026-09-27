#include "voice-mocks.h"
#include "voice-stalled-provider.h"

typedef struct {
	TestTransport *transport;
	TestRecognizer *stt;
	TestSynthesizer *tts;
	TestActivity *vad;
	AiMockProvider *provider;
	AiConversation *conversation;
	AiVoiceSession *session;
	GArray *states;
	GPtrArray *speakers;
} Fixture;
static void
on_state(AiVoiceSession *s, gint state, gpointer data)
{
	Fixture *f = data;
	g_array_append_val(f->states, state);
}
static void
on_transcript(AiVoiceSession *s, const gchar *speaker, const gchar *text, gboolean final,
			  gpointer data)
{
	if (final)
		g_ptr_array_add(((Fixture *)data)->speakers, g_strdup(speaker));
}
static void
setup(Fixture *f, gconstpointer data)
{
	gint initial = AI_VOICE_LISTENING;
	f->transport = g_object_new(test_transport_get_type(), NULL);
	f->stt = g_object_new(test_recognizer_get_type(), NULL);
	f->tts = g_object_new(test_synthesizer_get_type(), NULL);
	f->vad = g_object_new(test_activity_get_type(), NULL);
	f->provider = ai_mock_provider_new();
	f->conversation = ai_conversation_new(G_OBJECT(f->provider));
	ai_conversation_set_local_tools(f->conversation, TRUE);
	f->states = g_array_new(FALSE, FALSE, sizeof(gint));
	f->speakers = g_ptr_array_new_with_free_func(g_free);
	f->session = g_object_new(AI_TYPE_VOICE_SESSION, "transport", f->transport,
							  "recognizer", f->stt, "synthesizer", f->tts, "activity",
							  f->vad, "conversation", f->conversation, NULL);
	if (data != GINT_TO_POINTER(1))
		g_object_set(f->session, "barge-in-ms", 10, NULL);
	g_array_append_val(f->states, initial);
	g_signal_connect(f->session, "state-changed", G_CALLBACK(on_state), f);
	g_signal_connect(f->session, "transcript", G_CALLBACK(on_transcript), f);
	g_signal_emit_by_name(f->transport, "participant-joined", "caller", "Caller");
}
static void
drain(void)
{
	while (g_main_context_iteration(NULL, FALSE))
		;
}
static void
teardown(Fixture *f, gconstpointer data)
{
	ai_voice_session_stop(f->session);
	if (f->tts->held != NULL) {
		g_task_return_boolean(f->tts->held, TRUE);
		g_clear_object(&f->tts->held);
	}
	drain();
	g_object_unref(f->session);
	g_object_unref(f->transport);
	g_object_unref(f->stt);
	g_object_unref(f->tts);
	g_object_unref(f->vad);
	g_object_unref(f->conversation);
	g_object_unref(f->provider);
	g_array_unref(f->states);
	g_ptr_array_unref(f->speakers);
	drain();
}
static void
frame(Fixture *f, const gchar *speaker, guint8 activity)
{
	guint8 samples[320] = {0};
	g_autoptr(GBytes) pcm = NULL;
	samples[0] = activity;
	pcm = g_bytes_new(samples, sizeof(samples));
	g_signal_emit_by_name(f->transport, "audio", speaker, pcm);
}
static void
utterance(Fixture *f, const gchar *speaker)
{
	frame(f, speaker, 1);
	frame(f, speaker, 2);
}
static void
wait_replies(Fixture *f, guint n)
{
	gint64 limit = g_get_monotonic_time() + 3000000;
	while ((f->tts->texts->len < n ||
			ai_voice_session_get_state(f->session) != AI_VOICE_LISTENING) &&
		   g_get_monotonic_time() < limit) {
		drain();
		g_usleep(1000);
	}
	g_assert_cmpuint(f->tts->texts->len, >=, n);
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_LISTENING);
}
static void
full_turn(Fixture *f, gconstpointer data)
{
	static const gint expected[] = {AI_VOICE_LISTENING, AI_VOICE_TRANSCRIBING,
									AI_VOICE_THINKING, AI_VOICE_SPEAKING,
									AI_VOICE_LISTENING};
	ai_mock_provider_push_text(f->provider, "Hello Caller.");
	utterance(f, "caller");
	wait_replies(f, 1);
	g_assert_cmpuint(f->transport->writes, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "Hello Caller.");
	g_assert_cmpmem(f->states->data, f->states->len * sizeof(gint), expected,
					sizeof(expected));
	g_assert_cmpstr(g_ptr_array_index(f->speakers, 0), ==, "Caller");
}
static void
barge_in(Fixture *f, gconstpointer data)
{
	gint64 limit = g_get_monotonic_time() + 3000000;
	f->tts->hold_after = 2;
	ai_mock_provider_push_text(f->provider, "First sentence. Unspoken ending.");
	utterance(f, "caller");
	while (f->tts->held == NULL && g_get_monotonic_time() < limit) {
		drain();
		g_usleep(1000);
	}
	g_assert_nonnull(f->tts->held);
	frame(f, "caller", 1);
	g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpuint(f->transport->flushes, ==, 1);
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_TRANSCRIBING);
	{
		GList *messages = ai_conversation_get_messages(f->conversation);
		g_autofree gchar *text = ai_message_get_text(g_list_last(messages)->data);
		g_assert_cmpstr(text, ==, "First sentence.");
	}
}
static void
speakers(Fixture *f, gconstpointer data)
{
	g_signal_emit_by_name(f->transport, "participant-joined", "sam", "Sam");
	frame(f, "caller", 1);
	frame(f, "sam", 1);
	g_signal_emit_by_name(f->transport, "participant-left", "sam");
	g_assert_true(g_hash_table_contains(f->stt->active, "caller"));
	g_assert_false(g_hash_table_contains(f->stt->active, "sam"));
	g_assert_cmpuint(f->stt->cancelled, ==, 1);
	ai_mock_provider_push_text(f->provider, "Hello.");
	frame(f, "caller", 2);
	wait_replies(f, 1);
	g_signal_emit_by_name(f->transport, "participant-joined", "sam", "Sam");
	ai_mock_provider_push_text(f->provider, "Welcome.");
	utterance(f, "sam");
	wait_replies(f, 2);
	g_assert_cmpstr(g_ptr_array_index(f->speakers, 0), ==, "Caller");
	g_assert_cmpstr(g_ptr_array_index(f->speakers, 1), ==, "Sam");
}
static void
timeout_turn(Fixture *f, gconstpointer data)
{
	g_object_set(f->session, "turn-deadline-ms", 30, "deadline-message",
				 "Custom deadline fallback", NULL);
	ai_mock_provider_set_delay_ms(f->provider, 5000);
	ai_mock_provider_push_text(f->provider, "Too late.");
	utterance(f, "caller");
	wait_replies(f, 1);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "Custom deadline fallback");
}
static void
tools(Fixture *f, gconstpointer data)
{
	ai_mock_provider_push_tool_use(f->provider, "read",
								   "{\"path\":\"/definitely-missing-voice-fixture\"}");
	ai_mock_provider_push_text(f->provider, "The file was unavailable.");
	utterance(f, "caller");
	wait_replies(f, 2);
	g_assert_nonnull(strstr(g_ptr_array_index(f->tts->texts, 0), "error"));
}
/* A tool error is cut at 512 bytes before it is spoken. Cut through a
 * two-byte character, the line must still be spoken rather than rejected by
 * the synthesizer as invalid text, which the session reads as a TTS outage. */
static void
tool_error_multibyte(Fixture *f, gconstpointer data)
{
	GString *input = g_string_new("{\"path\":\"/missing-voice-fixture-");
	guint i;
	if (GPOINTER_TO_UINT(data) != 0)
		g_string_append_c(input, 'x');
	for (i = 0; i < 400; i++)
		g_string_append(input, "\303\251");
	g_string_append(input, "\"}");
	ai_mock_provider_push_tool_use(f->provider, "read", input->str);
	ai_mock_provider_push_text(f->provider, "The file was unavailable.");
	g_string_free(input, TRUE);
	utterance(f, "caller");
	wait_replies(f, 2);
	for (i = 0; i < f->tts->texts->len; i++) {
		const gchar *text = g_ptr_array_index(f->tts->texts, i);
		g_assert_true(g_utf8_validate(text, -1, NULL));
		g_assert_null(strstr(text, "speech service"));
	}
	g_assert_nonnull(strstr(g_ptr_array_index(f->tts->texts, 0), "error"));
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 1), ==, "The file was unavailable.");
}
static gboolean
contains_tool_result(Fixture *f, const gchar *needle)
{
	GList *l;
	for (l = ai_mock_provider_get_last_messages(f->provider); l != NULL; l = l->next) {
		GList *b;
		for (b = ai_message_get_content_blocks(l->data); b != NULL; b = b->next)
			if (AI_IS_TOOL_RESULT(b->data) &&
				strstr(ai_tool_result_get_content(b->data), needle) != NULL)
				return TRUE;
	}
	return FALSE;
}
static void
read_fixture(Fixture *f, gconstpointer data)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-voice-read-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "fixture.txt", NULL);
	g_autofree gchar *input = g_strdup_printf("{\"path\":\"%s\"}", path);
	g_assert_true(g_file_set_contents(path, "voice fixture content", -1, NULL));
	ai_mock_provider_push_tool_use(f->provider, "read", input);
	ai_mock_provider_push_text(f->provider, "voice fixture content");
	utterance(f, "caller");
	wait_replies(f, 1);
	g_assert_true(contains_tool_result(f, "voice fixture content"));
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "voice fixture content");
	g_unlink(path);
	g_rmdir(directory);
}
static void
background(Fixture *f, gconstpointer data)
{
	g_autoptr(AiBrigade) brigade = ai_brigade_new();
	g_autoptr(GObject) worker = g_object_new(test_agent_worker_get_type(), NULL);
	guint i, notices = 0;
	ai_brigade_set_worker(brigade, AI_AGENT_WORKER(worker));
	ai_conversation_set_brigade(f->conversation, brigade);
	ai_mock_provider_push_tool_use(f->provider, "agent_spawn",
								   "{\"prompt\":\"read background fixture\"}");
	ai_mock_provider_push_text(f->provider, "I started it.");
	utterance(f, "caller");
	wait_replies(f, 2);
	for (i = 0; i < f->tts->texts->len; i++)
		if (strstr(g_ptr_array_index(f->tts->texts, i),
				   "Background agent agent-1 finished") != NULL)
			notices++;
	g_assert_cmpuint(notices, ==, 1);
	g_assert_nonnull(ai_brigade_get(brigade, "agent-1"));
	ai_mock_provider_push_tool_use(f->provider, "agent_result",
								   "{\"agent_id\":\"agent-1\"}");
	ai_mock_provider_push_text(f->provider, "The background fixture result");
	utterance(f, "caller");
	wait_replies(f, 3);
	g_assert_true(contains_tool_result(f, "The background fixture result"));
	g_assert_null(ai_brigade_get(brigade, "agent-1"));
}
static void
stalled_turn(Fixture *f, gconstpointer data)
{
	g_autoptr(GObject) provider = g_object_new(test_stalled_provider_get_type(), NULL);
	TestStalledProvider *stalled = (TestStalledProvider *)provider;
	gint64 limit = g_get_monotonic_time() + 3000000;
	ai_voice_session_stop(f->session);
	g_clear_object(&f->session);
	g_clear_object(&f->conversation);
	f->conversation = ai_conversation_new(provider);
	f->session = g_object_new(AI_TYPE_VOICE_SESSION, "transport", f->transport,
							  "recognizer", f->stt, "synthesizer", f->tts, "activity",
							  f->vad, "conversation", f->conversation, NULL);
	g_object_set(f->session, "barge-in-ms", 10, NULL);
	g_signal_emit_by_name(f->transport, "participant-joined", "caller", "Caller");
	if (data != NULL) {
		stalled->emit_text = FALSE;
		g_object_set(f->session, "turn-deadline-ms", 30, NULL);
		utterance(f, "caller");
		wait_replies(f, 1);
		g_assert_nonnull(strstr(g_ptr_array_index(f->tts->texts, 0), "taking too long"));
	} else {
		f->tts->hold_after = 1;
		utterance(f, "caller");
		while (f->tts->held == NULL && g_get_monotonic_time() < limit) {
			drain();
			g_usleep(1000);
		}
		g_assert_nonnull(f->tts->held);
		g_assert_cmpint(g_atomic_int_get(&stalled->cancelled), ==, 0);
		frame(f, "caller", 1);
		g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
		g_assert_cmpuint(f->transport->flushes, >=, 1);
	}
	g_assert_cmpint(g_atomic_int_get(&stalled->cancelled), ==, 1);
}

static void
say_non_silent(Fixture *f, gconstpointer data)
{
	guint8 samples[3200];
	guint i;
	for (i = 0; i < sizeof(samples); i += 2) {
		samples[i] = 0x40;
		samples[i + 1] = 0x1f;
	}
	f->tts->pcm = g_bytes_new(samples, sizeof(samples));
	ai_voice_session_say(f->session, "A generic greeting.");
	wait_replies(f, 1);
	g_assert_cmpuint(f->transport->non_silent_samples, ==, 1600);
}
static void
recognition_error_spoken(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error =
		g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "Recognition unavailable");
	g_object_set(f->session, "transcription-error-message", "Please repeat that.", NULL);
	frame(f, "caller", 1);
	g_signal_emit_by_name(f->stt, "error", "caller", error);
	wait_replies(f, 1);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "Please repeat that.");
	g_assert_cmpuint(f->speakers->len, ==, 0);
}
static void
silence_final(Fixture *f, gconstpointer data)
{
	frame(f, "caller", 1);
	g_signal_emit_by_name(f->stt, "transcript", "caller", " \t\r\n", TRUE);
	drain();
	g_assert_cmpuint(f->speakers->len, ==, 0);
	g_assert_cmpuint(f->tts->texts->len, ==, 0);
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_LISTENING);
}
static void
debounce_noise(Fixture *f, gconstpointer data)
{
	guint i, debounce;
	g_object_get(f->session, "barge-in-ms", &debounce, NULL);
	g_assert_cmpuint(debounce, ==, 250);
	for (i = 0; i < 24; i++)
		frame(f, "caller", 1);
	g_assert_cmpuint(g_hash_table_size(f->stt->active), ==, 0);
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_LISTENING);
	frame(f, "caller", 0);
	for (i = 0; i < 24; i++)
		frame(f, "caller", 1);
	g_assert_cmpuint(g_hash_table_size(f->stt->active), ==, 0);
	frame(f, "caller", 1);
	g_assert_true(g_hash_table_contains(f->stt->active, "caller"));
	g_assert_cmpuint(f->stt->fed, ==, 25);
}
static void
debounce_notice(Fixture *f, gconstpointer data)
{
	guint i;
	f->tts->hold_after = 1;
	f->tts->delay_audio = TRUE;
	ai_voice_session_say(f->session, "A notice.");
	g_assert_nonnull(f->tts->held);
	for (i = 0; i < 24; i++)
		frame(f, "caller", 1);
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpuint(g_hash_table_size(f->stt->active), ==, 0);
	g_signal_emit_by_name(f->transport, "participant-joined", "second", "Second");
	frame(f, "second", 1);
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_test_expect_message(
		"ai-glib", G_LOG_LEVEL_INFO,
		"barge-in by Caller after 250 ms of speech, cancelling speaking");
	frame(f, "caller", 1);
	g_test_assert_expected_messages();
	g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_true(g_hash_table_contains(f->stt->active, "caller"));
	g_assert_cmpuint(f->stt->fed, ==, 25);
	g_assert_false(g_hash_table_contains(f->stt->active, "second"));
}
static void
media_recovery(Fixture *f, gconstpointer data)
{
	guint states;
	f->tts->hold_after = 1;
	ai_voice_session_say(f->session, "Continued notice.");
	g_assert_nonnull(f->tts->held);
	states = f->states->len;
	g_signal_emit_by_name(f->transport, "reconnecting");
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_SPEAKING);
	g_assert_cmpuint(f->states->len, ==, states);
	g_assert_cmpuint(f->transport->flushes, ==, 0);
	ai_voice_session_say(f->session, "Queued during recovery.");
	drain();
	g_assert_cmpuint(f->tts->texts->len, ==, 1);
	g_signal_emit_by_name(f->transport, "reconnected");
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_task_return_boolean(f->tts->held, TRUE);
	g_clear_object(&f->tts->held);
	f->tts->hold_after = 0;
	wait_replies(f, 2);
	ai_mock_provider_push_text(f->provider, "The next turn works.");
	utterance(f, "caller");
	wait_replies(f, 3);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 2), ==, "The next turn works.");
}

static void
iterate_for(guint milliseconds)
{
	gint64 until = g_get_monotonic_time() + (gint64)milliseconds * 1000;
	while (g_get_monotonic_time() < until) {
		drain();
		g_usleep(1000);
	}
}
static void
completed_provider_playback(Fixture *f, gconstpointer data)
{
	gint64 until = g_get_monotonic_time() + 3000000;
	g_object_set(f->session, "turn-deadline-ms", 100, NULL);
	f->tts->hold_after = 1;
	ai_mock_provider_push_text(f->provider, "A complete long reply.");
	utterance(f, "caller");
	while (f->tts->held == NULL && g_get_monotonic_time() < until) {
		drain();
		g_usleep(1000);
	}
	g_assert_nonnull(f->tts->held);
	iterate_for(250);
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_SPEAKING);
	g_assert_cmpuint(f->tts->texts->len, ==, 1);
	g_task_return_boolean(f->tts->held, TRUE);
	g_clear_object(&f->tts->held);
	wait_replies(f, 1);
	{
		GList *messages = ai_conversation_get_messages(f->conversation);
		g_autofree gchar *text = ai_message_get_text(g_list_last(messages)->data);
		g_assert_cmpstr(text, ==, "A complete long reply.");
	}
}
/* A TTS model handed nothing it can pronounce does not fail: it invents
 * several seconds of audio, identical on every call when it is seeded. */
typedef struct {
	const gchar *reply;
	const gchar *spoken[4];
} SpeakableCase;
static const SpeakableCase speakable_cases[] = {
	{"Let's get to work. \360\237\230\210", {"Let's get to work.", NULL}},
	{"Done! \360\237\230\210\360\237\224\245", {"Done!", NULL}},
	{"Hmm... okay then.", {"Hmm...", "okay then.", NULL}},
	{"Version 3.5 is out. Upgrade.", {"Version 3.5 is out.", "Upgrade.", NULL}},
	{"That is **really** it.\n\n---\n", {"That is really it.", NULL}},
	{"Shipped \342\234\205 and green.", {"Shipped and green.", NULL}},
	{"Wait?! Really?", {"Wait?!", "Really?", NULL}},
	{"He said \"stop.\" Then left.", {"He said \"stop.\"", "Then left.", NULL}},
};
static void
speakable_segments(Fixture *f, gconstpointer data)
{
	const SpeakableCase *c = data;
	guint n = 0, i;
	while (c->spoken[n] != NULL)
		n++;
	ai_mock_provider_push_text(f->provider, c->reply);
	utterance(f, "caller");
	wait_replies(f, n);
	iterate_for(50);
	for (i = 0; i < f->tts->texts->len; i++)
		g_test_message("tts[%u] = '%s'", i, (gchar *)g_ptr_array_index(f->tts->texts, i));
	g_assert_cmpuint(f->tts->texts->len, ==, n);
	for (i = 0; i < n; i++)
		g_assert_cmpstr(g_ptr_array_index(f->tts->texts, i), ==, c->spoken[i]);
}
/* The same boundaries when the reply arrives a token at a time and the turn
 * has not finished: nothing is held back waiting for more, except a "3."
 * that may still become "3.5". */
static const gchar *const streamed_deltas[] = {
	"Let's get to work", ".", " ", "\360\237\230\210", " Version 3", ".", "5 is out",
	".", " Hmm", ".", ".", ".", " okay", ".", NULL};
static const gchar *const streamed_spoken[] = {
	"Let's get to work.", "Version 3.5 is out.", "Hmm.", "okay.", NULL};
static void
streamed_segments(Fixture *f, gconstpointer data)
{
	g_autoptr(GObject) provider = g_object_new(test_stalled_provider_get_type(), NULL);
	gint64 limit = g_get_monotonic_time() + 3000000;
	guint i;
	((TestStalledProvider *)provider)->deltas = streamed_deltas;
	ai_voice_session_stop(f->session);
	g_clear_object(&f->session);
	g_clear_object(&f->conversation);
	f->conversation = ai_conversation_new(provider);
	f->session = g_object_new(AI_TYPE_VOICE_SESSION, "transport", f->transport,
							  "recognizer", f->stt, "synthesizer", f->tts, "activity",
							  f->vad, "conversation", f->conversation, NULL);
	g_object_set(f->session, "barge-in-ms", 10, NULL);
	g_signal_emit_by_name(f->transport, "participant-joined", "caller", "Caller");
	utterance(f, "caller");
	while (f->tts->texts->len < G_N_ELEMENTS(streamed_spoken) - 1 &&
		   g_get_monotonic_time() < limit) {
		drain();
		g_usleep(1000);
	}
	iterate_for(50);
	for (i = 0; i < f->tts->texts->len; i++)
		g_test_message("tts[%u] = '%s'", i, (gchar *)g_ptr_array_index(f->tts->texts, i));
	g_assert_cmpuint(f->tts->texts->len, ==, G_N_ELEMENTS(streamed_spoken) - 1);
	for (i = 0; streamed_spoken[i] != NULL; i++)
		g_assert_cmpstr(g_ptr_array_index(f->tts->texts, i), ==, streamed_spoken[i]);
}
static void
recovery_pauses_deadline(Fixture *f, gconstpointer data)
{
	g_object_set(f->session, "turn-deadline-ms", 200, "deadline-message",
				 "Provider deadline expired.", NULL);
	ai_mock_provider_set_delay_ms(f->provider, 5000);
	ai_mock_provider_push_text(f->provider, "Too late.");
	utterance(f, "caller");
	iterate_for(60);
	g_signal_emit_by_name(f->transport, "reconnecting");
	iterate_for(300);
	g_assert_cmpuint(f->tts->texts->len, ==, 0);
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_THINKING);
	g_signal_emit_by_name(f->transport, "reconnected");
	iterate_for(60);
	g_assert_cmpuint(f->tts->texts->len, ==, 0);
	/* The remaining approximately 140 ms resumes, rather than a new 200 ms. */
	iterate_for(110);
	g_assert_cmpuint(f->tts->texts->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==,
					"Provider deadline expired.");
	wait_replies(f, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	{
		guint i;
		for (i = 0; i < G_N_ELEMENTS(speakable_cases); i++) {
			g_autofree gchar *path = g_strdup_printf("/voice/session/speakable/%u", i);
			g_test_add(path, Fixture, &speakable_cases[i], setup, speakable_segments,
					   teardown);
		}
	}
	g_test_add("/voice/session/completed-provider-playback", Fixture, NULL, setup,
			   completed_provider_playback, teardown);
	g_test_add("/voice/session/recovery-pauses-deadline", Fixture, NULL, setup,
			   recovery_pauses_deadline, teardown);
	g_test_add("/voice/session/media-recovery", Fixture, NULL, setup, media_recovery,
			   teardown);
	g_test_add("/voice/session/debounce-noise", Fixture, GINT_TO_POINTER(1), setup,
			   debounce_noise, teardown);
	g_test_add("/voice/session/debounce-notice", Fixture, GINT_TO_POINTER(1), setup,
			   debounce_notice, teardown);
	g_test_add("/voice/session/stt-error-spoken", Fixture, NULL, setup,
			   recognition_error_spoken, teardown);
	g_test_add("/voice/session/silence-final", Fixture, NULL, setup, silence_final,
			   teardown);
	g_test_add("/voice/session/say-non-silent-pcm", Fixture, NULL, setup, say_non_silent,
			   teardown);
	g_test_add("/voice/session/barge-in-pending-provider", Fixture, NULL, setup,
			   stalled_turn, teardown);
	g_test_add("/voice/session/never-answering-provider", Fixture, "deadline", setup,
			   stalled_turn, teardown);
	g_test_add("/voice/session/streamed-segments", Fixture, NULL, setup,
			   streamed_segments, teardown);
	g_test_add("/voice/session/tool-error-multibyte/0", Fixture, GUINT_TO_POINTER(0),
			   setup, tool_error_multibyte, teardown);
	g_test_add("/voice/session/tool-error-multibyte/1", Fixture, GUINT_TO_POINTER(2),
			   setup, tool_error_multibyte, teardown);
	g_test_add("/voice/session/full-turn", Fixture, NULL, setup, full_turn, teardown);
	g_test_add("/voice/session/barge-in", Fixture, NULL, setup, barge_in, teardown);
	g_test_add("/voice/session/speakers", Fixture, NULL, setup, speakers, teardown);
	g_test_add("/voice/session/deadline", Fixture, NULL, setup, timeout_turn, teardown);
	g_test_add("/voice/session/tool-error", Fixture, NULL, setup, tools, teardown);
	g_test_add("/voice/session/read-fixture", Fixture, NULL, setup, read_fixture,
			   teardown);
	g_test_add("/voice/session/background-agent", Fixture, NULL, setup, background,
			   teardown);
	return g_test_run();
}
