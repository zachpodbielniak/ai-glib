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
	if (data != NULL)
		g_object_set(f->session, "barge-in-confirm", FALSE, NULL);
	frame(f, "caller", 1);
	if (data == NULL) {
		/* Sound alone is not an interruption while speaking: it may be our own
		 * voice coming back through the caller's speaker. Words are. */
		g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
		g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_SPEAKING);
		g_assert_true(g_hash_table_contains(f->stt->active, "caller"));
		g_signal_emit_by_name(f->stt, "transcript", "caller", "wait a moment", FALSE);
	}
	g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpuint(f->transport->flushes, ==, 1);
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_TRANSCRIBING);
	{
		GList *messages = ai_conversation_get_messages(f->conversation);
		g_autofree gchar *text = ai_message_get_text(g_list_last(messages)->data);
		g_assert_cmpstr(text, ==, "First sentence.");
	}
}
/* Hold the reply on its second segment, then let the caller's line carry
 * something while the assistant is still talking. */
static void
speak_and_hold(Fixture *f)
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
	g_assert_true(g_hash_table_contains(f->stt->active, "caller"));
}
static void
echo_is_not_a_turn(Fixture *f, gconstpointer data)
{
	guint heard;
	speak_and_hold(f);
	heard = f->speakers->len;
	/* What the caller's microphone picked up was our own first sentence. */
	g_signal_emit_by_name(f->stt, "transcript", "caller", "first sentence", FALSE);
	g_signal_emit_by_name(f->stt, "transcript", "caller", "First sentence.", TRUE);
	drain();
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_SPEAKING);
	g_assert_cmpuint(f->speakers->len, ==, heard);
	g_assert_cmpuint(f->transport->flushes, ==, 0);
}
static void
noise_is_not_a_turn(Fixture *f, gconstpointer data)
{
	speak_and_hold(f);
	g_signal_emit_by_name(f->stt, "transcript", "caller", "", TRUE);
	drain();
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_SPEAKING);
	g_assert_cmpuint(f->transport->flushes, ==, 0);
}
static void
final_words_interrupt(Fixture *f, gconstpointer data)
{
	guint heard;
	speak_and_hold(f);
	heard = f->speakers->len;
	/* No partial arrived; the final alone is enough, and it becomes the turn. */
	ai_mock_provider_push_text(f->provider, "Stopping.");
	g_signal_emit_by_name(f->stt, "transcript", "caller", "Hold on, stop.", TRUE);
	g_assert_true(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpuint(f->transport->flushes, ==, 1);
	g_assert_cmpuint(f->speakers->len, ==, heard + 1);
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
/* Every tool the model ran during a call is reported, with what it was
 * asked and what it answered, whether or not it succeeded. */
typedef struct {
	gchar *name, *arguments, *result;
	gboolean is_error;
	guint count;
} ToolSeen;
static void
on_tool(AiVoiceSession *s, const gchar *name, const gchar *arguments, const gchar *result,
		gboolean is_error, gpointer data)
{
	ToolSeen *seen = data;
	seen->count++;
	g_free(seen->name);
	g_free(seen->arguments);
	g_free(seen->result);
	seen->name = g_strdup(name);
	seen->arguments = g_strdup(arguments);
	seen->result = g_strdup(result);
	seen->is_error = is_error;
}
static void
tool_reported(Fixture *f, gconstpointer data)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-voice-tool-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "fixture.txt", NULL);
	g_autofree gchar *input = g_strdup_printf("{\"path\":\"%s\"}", path);
	ToolSeen seen = {0};
	g_assert_true(g_file_set_contents(path, "voice fixture content", -1, NULL));
	g_signal_connect(f->session, "tool", G_CALLBACK(on_tool), &seen);
	if (data == NULL)
		ai_mock_provider_push_tool_use(f->provider, "read", input);
	else
		ai_mock_provider_push_tool_use(f->provider, "read",
									   "{\"path\":\"/definitely-missing-voice-fixture\"}");
	ai_mock_provider_push_text(f->provider, "Done.");
	utterance(f, "caller");
	wait_replies(f, 1);
	g_signal_handlers_disconnect_by_data(f->session, &seen);
	g_assert_cmpuint(seen.count, ==, 1);
	g_assert_cmpstr(seen.name, ==, "read");
	g_assert_nonnull(strstr(seen.arguments, "\"path\""));
	if (data == NULL) {
		g_assert_false(seen.is_error);
		g_assert_nonnull(strstr(seen.result, "voice fixture content"));
	} else {
		g_assert_true(seen.is_error);
		g_assert_cmpstr(seen.result, !=, "");
	}
	g_free(seen.name);
	g_free(seen.arguments);
	g_free(seen.result);
	g_unlink(path);
	g_rmdir(directory);
}
/* A turn that ends with nothing to say must say so. Silence reads to a
 * caller as a dropped call, and they cannot see a log. */
static void
empty_reply_spoken(Fixture *f, gconstpointer data)
{
	g_object_set(f->session, "empty-reply-message", "I came up empty on that one.", NULL);
	g_test_expect_message("ai-glib", G_LOG_LEVEL_INFO,
						  "Provider turn ended with nothing to say*");
	ai_mock_provider_push_text(f->provider, "");
	utterance(f, "caller");
	wait_replies(f, 1);
	g_test_assert_expected_messages();
	g_assert_cmpuint(f->tts->texts->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "I came up empty on that one.");
}
/* A reply that is only unpronounceable symbols is also nothing to say. */
static void
symbols_only_reply_spoken(Fixture *f, gconstpointer data)
{
	gchar *message = NULL;
	g_object_get(f->session, "empty-reply-message", &message, NULL);
	g_assert_nonnull(message);
	g_assert_cmpstr(message, !=, "");
	ai_mock_provider_push_text(f->provider, "\360\237\230\210");
	utterance(f, "caller");
	wait_replies(f, 1);
	g_assert_cmpuint(f->tts->texts->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, message);
	g_free(message);
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
		g_signal_emit_by_name(f->stt, "transcript", "caller", "never mind", FALSE);
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
		"possible barge-in by Caller after 250 ms of speech, waiting for words");
	frame(f, "caller", 1);
	g_test_assert_expected_messages();
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_test_expect_message("ai-glib", G_LOG_LEVEL_INFO,
						  "barge-in by Caller confirmed by speech, cancelling speaking");
	g_signal_emit_by_name(f->stt, "transcript", "caller", "stop", FALSE);
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
	const gchar *spoken[6];
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
	{"Steps:\n1. Pull the branch.\n2. Build it.",
	 {"Steps:", "1. Pull the branch.", "2. Build it.", NULL}},
	{"## Summary\n> All green.\n- One fix.\n* Two tests.",
	 {"Summary", "All green.", "One fix.", "Two tests.", NULL}},
	{"It is 5 - 3 degrees.", {"It is 5 - 3 degrees.", NULL}},
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
	"Let's get to work.", "Version 3.5 is out.", "Hmm...", "okay.", NULL};
/* A delta that ends on a period is not yet a sentence end: the next one
 * may continue a file name or a domain. Heard live as "garden." / "org)". */
static const gchar *const file_name_deltas[] = {
	"One note (garden", ".", "org) lists a plot", ".", " The other is bigger", ".", NULL};
static const gchar *const file_name_spoken[] = {
	"One note (garden.org) lists a plot.", "The other is bigger.", NULL};
static void
streamed_segments(Fixture *f, gconstpointer data)
{
	g_autoptr(GObject) provider = g_object_new(test_stalled_provider_get_type(), NULL);
	gint64 limit = g_get_monotonic_time() + 3000000;
	guint i;
	const gchar *const *deltas = data != NULL ? file_name_deltas : streamed_deltas;
	const gchar *const *spoken = data != NULL ? file_name_spoken : streamed_spoken;
	guint expected = g_strv_length((gchar **)spoken);
	((TestStalledProvider *)provider)->deltas = deltas;
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
	while (f->tts->texts->len < expected && g_get_monotonic_time() < limit) {
		drain();
		g_usleep(1000);
	}
	iterate_for(50);
	for (i = 0; i < f->tts->texts->len; i++)
		g_test_message("tts[%u] = '%s'", i, (gchar *)g_ptr_array_index(f->tts->texts, i));
	iterate_for(400);
	g_assert_cmpuint(f->tts->texts->len, ==, expected);
	for (i = 0; spoken[i] != NULL; i++)
		g_assert_cmpstr(g_ptr_array_index(f->tts->texts, i), ==, spoken[i]);
}
/* What the assistant actually said, as the caller heard it: one report per
 * segment that produced audio, marked complete or cut off. */
typedef struct {
	GPtrArray *texts;
	GArray *complete;
} Spoken;
static void
on_spoken(AiVoiceSession *s, const gchar *text, gboolean complete, gpointer data)
{
	Spoken *spoken = data;
	g_ptr_array_add(spoken->texts, g_strdup(text));
	g_array_append_val(spoken->complete, complete);
}
static Spoken *
watch_spoken(Fixture *f)
{
	Spoken *spoken = g_new0(Spoken, 1);
	spoken->texts = g_ptr_array_new_with_free_func(g_free);
	spoken->complete = g_array_new(FALSE, FALSE, sizeof(gboolean));
	g_signal_connect(f->session, "spoken", G_CALLBACK(on_spoken), spoken);
	return spoken;
}
static void
spoken_free(Fixture *f, Spoken *spoken)
{
	g_signal_handlers_disconnect_by_data(f->session, spoken);
	g_ptr_array_unref(spoken->texts);
	g_array_unref(spoken->complete);
	g_free(spoken);
}
static void
spoken_complete(Fixture *f, gconstpointer data)
{
	Spoken *spoken = watch_spoken(f);
	ai_mock_provider_push_text(f->provider, "Hello Caller. Second line.");
	utterance(f, "caller");
	wait_replies(f, 2);
	g_assert_cmpuint(spoken->texts->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(spoken->texts, 0), ==, "Hello Caller.");
	g_assert_cmpstr(g_ptr_array_index(spoken->texts, 1), ==, "Second line.");
	g_assert_true(g_array_index(spoken->complete, gboolean, 0));
	g_assert_true(g_array_index(spoken->complete, gboolean, 1));
	spoken_free(f, spoken);
}
static void
spoken_interrupted(Fixture *f, gconstpointer data)
{
	Spoken *spoken = watch_spoken(f);
	speak_and_hold(f);
	g_signal_emit_by_name(f->stt, "transcript", "caller", "wait a moment", FALSE);
	g_task_return_boolean(f->tts->held, TRUE);
	g_clear_object(&f->tts->held);
	drain();
	g_assert_cmpuint(spoken->texts->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(spoken->texts, 0), ==, "First sentence.");
	g_assert_true(g_array_index(spoken->complete, gboolean, 0));
	g_assert_cmpstr(g_ptr_array_index(spoken->texts, 1), ==, "Unspoken ending.");
	g_assert_false(g_array_index(spoken->complete, gboolean, 1));
	spoken_free(f, spoken);
}
static void
silent_failure_not_spoken(Fixture *f, gconstpointer data)
{
	Spoken *spoken = watch_spoken(f);
	f->tts->hold_after = 1;
	f->tts->delay_audio = TRUE;
	g_object_set(f->session, "synthesis-error-message", "", NULL);
	ai_voice_session_say(f->session, "Never heard.");
	g_assert_nonnull(f->tts->held);
	g_task_return_new_error(f->tts->held, G_IO_ERROR, G_IO_ERROR_FAILED, "TTS down");
	g_clear_object(&f->tts->held);
	drain();
	/* No audio reached the caller, so nothing was said. */
	g_assert_cmpuint(spoken->texts->len, ==, 0);
	spoken_free(f, spoken);
}
/* While a tool runs and nothing has been said, one short line fills the
 * silence -- but only once it has gone on long enough to notice, and only
 * once per turn. It plays while the tool keeps working. */
static void
progress_setup(Fixture *f, guint delay_ms, guint provider_delay_ms)
{
	g_object_set(f->session, "tool-progress-message", "One moment, let me check.",
				 "tool-progress-delay-ms", delay_ms, NULL);
	ai_mock_provider_set_delay_ms(f->provider, provider_delay_ms);
}
static void
tool_progress_slow(Fixture *f, gconstpointer data)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-voice-progress-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "fixture.txt", NULL);
	g_autofree gchar *input = g_strdup_printf("{\"path\":\"%s\"}", path);
	g_assert_true(g_file_set_contents(path, "content", -1, NULL));
	progress_setup(f, 50, 300);
	ai_mock_provider_push_tool_use(f->provider, "read", input);
	ai_mock_provider_push_tool_use(f->provider, "read", input);
	ai_mock_provider_push_text(f->provider, "Found nothing.");
	utterance(f, "caller");
	wait_replies(f, 2);
	iterate_for(100);
	g_unlink(path);
	g_rmdir(directory);
	/* Once, however many tools ran, and the answer after it. */
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "One moment, let me check.");
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, f->tts->texts->len - 1), ==,
					"Found nothing.");
	{
		guint i, fillers = 0;
		for (i = 0; i < f->tts->texts->len; i++)
			fillers += g_str_equal(g_ptr_array_index(f->tts->texts, i),
								   "One moment, let me check.");
		g_assert_cmpuint(fillers, ==, 1);
	}
}
static void
tool_progress_fast(Fixture *f, gconstpointer data)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-voice-progress-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "fixture.txt", NULL);
	g_autofree gchar *input = g_strdup_printf("{\"path\":\"%s\"}", path);
	g_assert_true(g_file_set_contents(path, "quick", -1, NULL));
	/* The answer lands long before the delay: nothing to fill. */
	progress_setup(f, 2000, 0);
	ai_mock_provider_push_tool_use(f->provider, "read", input);
	ai_mock_provider_push_text(f->provider, "Quick answer.");
	utterance(f, "caller");
	wait_replies(f, 1);
	iterate_for(100);
	g_assert_cmpuint(f->tts->texts->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "Quick answer.");
	g_unlink(path);
	g_rmdir(directory);
}
static void
tool_progress_not_an_answer(Fixture *f, gconstpointer data)
{
	g_autofree gchar *directory = g_dir_make_tmp("ai-voice-progress-XXXXXX", NULL);
	g_autofree gchar *path = g_build_filename(directory, "fixture.txt", NULL);
	g_autofree gchar *input = g_strdup_printf("{\"path\":\"%s\"}", path);
	g_assert_true(g_file_set_contents(path, "content", -1, NULL));
	/* The filler is not a reply: a turn that then ends with nothing still
	 * says so. */
	progress_setup(f, 50, 300);
	g_object_set(f->session, "empty-reply-message", "I came up empty.", NULL);
	ai_mock_provider_push_tool_use(f->provider, "read", input);
	ai_mock_provider_push_text(f->provider, "");
	utterance(f, "caller");
	wait_replies(f, 2);
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 0), ==, "One moment, let me check.");
	g_assert_cmpstr(g_ptr_array_index(f->tts->texts, 1), ==, "I came up empty.");
	g_unlink(path);
	g_rmdir(directory);
}
static void
tool_progress_disabled(Fixture *f, gconstpointer data)
{
	progress_setup(f, 50, 300);
	g_object_set(f->session, "tool-progress-message", "", NULL);
	ai_mock_provider_push_tool_use(f->provider, "read", "{\"path\":\"/definitely-missing\"}");
	ai_mock_provider_push_text(f->provider, "Done.");
	utterance(f, "caller");
	wait_replies(f, 2);
	iterate_for(100);
	{
		guint i;
		for (i = 0; i < f->tts->texts->len; i++)
			g_assert_cmpstr(g_ptr_array_index(f->tts->texts, i), !=,
							"One moment, let me check.");
	}
}
/* Recognizers mark non-speech as [throat clearing], (coughs) or *sniff*.
 * Those are not words: alone they are silence, inside a sentence they are
 * dropped. */
static void
annotations_are_silence(Fixture *f, gconstpointer data)
{
	static const gchar *const noises[] = {"[throat clearing]", "(coughs)", "*sniff*",
										  "[BLANK_AUDIO]", " [Music] (laughs) ", NULL};
	guint i, heard = f->speakers->len;
	for (i = 0; noises[i] != NULL; i++) {
		frame(f, "caller", 1);
		g_signal_emit_by_name(f->stt, "transcript", "caller", noises[i], TRUE);
		drain();
	}
	g_assert_cmpuint(f->speakers->len, ==, heard);
	g_assert_cmpuint(f->tts->texts->len, ==, 0);
	g_assert_cmpuint(ai_mock_provider_get_call_count(f->provider), ==, 0);
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_LISTENING);
}
static void
annotations_stripped(Fixture *f, gconstpointer data)
{
	GList *messages;
	ai_mock_provider_push_text(f->provider, "Sure.");
	frame(f, "caller", 1);
	g_signal_emit_by_name(f->stt, "transcript", "caller",
						  "[clears throat] Okay, (coughs) do it.", TRUE);
	wait_replies(f, 1);
	messages = ai_mock_provider_get_last_messages(f->provider);
	g_assert_nonnull(messages);
	{
		g_autofree gchar *text = ai_message_get_text(g_list_last(messages)->data);
		g_assert_nonnull(strstr(text, "Okay, do it."));
		g_assert_null(strstr(text, "clears"));
		g_assert_null(strstr(text, "coughs"));
	}
}
/* A noise mark must not confirm an interruption either. */
static void
annotation_is_not_a_barge_in(Fixture *f, gconstpointer data)
{
	speak_and_hold(f);
	g_signal_emit_by_name(f->stt, "transcript", "caller", "[footsteps]", FALSE);
	g_assert_false(g_cancellable_is_cancelled(g_task_get_cancellable(f->tts->held)));
	g_assert_cmpint(ai_voice_session_get_state(f->session), ==, AI_VOICE_SPEAKING);
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
	g_test_add("/voice/session/streamed-file-name", Fixture, GINT_TO_POINTER(2), setup,
			   streamed_segments, teardown);
	g_test_add("/voice/session/tool-error-multibyte/0", Fixture, GUINT_TO_POINTER(0),
			   setup, tool_error_multibyte, teardown);
	g_test_add("/voice/session/tool-error-multibyte/1", Fixture, GUINT_TO_POINTER(2),
			   setup, tool_error_multibyte, teardown);
	g_test_add("/voice/session/spoken-complete", Fixture, NULL, setup, spoken_complete,
			   teardown);
	g_test_add("/voice/session/spoken-interrupted", Fixture, NULL, setup,
			   spoken_interrupted, teardown);
	g_test_add("/voice/session/silent-failure-not-spoken", Fixture, NULL, setup,
			   silent_failure_not_spoken, teardown);
	g_test_add("/voice/session/tool-reported", Fixture, NULL, setup, tool_reported,
			   teardown);
	g_test_add("/voice/session/tool-error-reported", Fixture, GINT_TO_POINTER(2), setup,
			   tool_reported, teardown);
	g_test_add("/voice/session/empty-reply-spoken", Fixture, NULL, setup,
			   empty_reply_spoken, teardown);
	g_test_add("/voice/session/symbols-only-reply-spoken", Fixture, NULL, setup,
			   symbols_only_reply_spoken, teardown);
	g_test_add("/voice/session/tool-progress-slow", Fixture, NULL, setup,
			   tool_progress_slow, teardown);
	g_test_add("/voice/session/tool-progress-fast", Fixture, NULL, setup,
			   tool_progress_fast, teardown);
	g_test_add("/voice/session/tool-progress-not-an-answer", Fixture, NULL, setup,
			   tool_progress_not_an_answer, teardown);
	g_test_add("/voice/session/tool-progress-disabled", Fixture, NULL, setup,
			   tool_progress_disabled, teardown);
	g_test_add("/voice/session/annotations-are-silence", Fixture, NULL, setup,
			   annotations_are_silence, teardown);
	g_test_add("/voice/session/annotations-stripped", Fixture, NULL, setup,
			   annotations_stripped, teardown);
	g_test_add("/voice/session/annotation-is-not-a-barge-in", Fixture, NULL, setup,
			   annotation_is_not_a_barge_in, teardown);
	g_test_add("/voice/session/full-turn", Fixture, NULL, setup, full_turn, teardown);
	g_test_add("/voice/session/barge-in", Fixture, NULL, setup, barge_in, teardown);
	g_test_add("/voice/session/barge-in-vad-only", Fixture, GINT_TO_POINTER(2), setup,
			   barge_in, teardown);
	g_test_add("/voice/session/echo-is-not-a-turn", Fixture, NULL, setup,
			   echo_is_not_a_turn, teardown);
	g_test_add("/voice/session/noise-is-not-a-turn", Fixture, NULL, setup,
			   noise_is_not_a_turn, teardown);
	g_test_add("/voice/session/final-words-interrupt", Fixture, NULL, setup,
			   final_words_interrupt, teardown);
	g_test_add("/voice/session/speakers", Fixture, NULL, setup, speakers, teardown);
	g_test_add("/voice/session/deadline", Fixture, NULL, setup, timeout_turn, teardown);
	g_test_add("/voice/session/tool-error", Fixture, NULL, setup, tools, teardown);
	g_test_add("/voice/session/read-fixture", Fixture, NULL, setup, read_fixture,
			   teardown);
	g_test_add("/voice/session/background-agent", Fixture, NULL, setup, background,
			   teardown);
	return g_test_run();
}
