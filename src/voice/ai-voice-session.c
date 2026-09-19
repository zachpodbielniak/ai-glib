/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voice-session.h"
#include "voice/ai-voice-worker-private.h"
#include "model/ai-tool-result.h"

/**
 * ai_voice_state_get_type:
 *
 * Returns: the registered voice state enumeration type
 */
GType
ai_voice_state_get_type(void)
{
	static gsize type_id;
	if (g_once_init_enter(&type_id)) {
		static const GEnumValue values[] = {
			{AI_VOICE_LISTENING, "AI_VOICE_LISTENING", "listening"},
			{AI_VOICE_TRANSCRIBING, "AI_VOICE_TRANSCRIBING", "transcribing"},
			{AI_VOICE_THINKING, "AI_VOICE_THINKING", "thinking"},
			{AI_VOICE_SPEAKING, "AI_VOICE_SPEAKING", "speaking"},
			{0, NULL, NULL}};
		GType registered = g_enum_register_static("AiVoiceState", values);
		g_once_init_leave(&type_id, registered);
	}
	return type_id;
}

typedef struct {
	gchar *name;
	GByteArray *frames;
	gboolean recognizing, ended;
} Participant;
typedef struct {
	AiVoiceSession *session;
	gchar *text;
	GCancellable *cancel;
	guint64 generation;
	guint pending;
	gsize queued;
	gboolean synthesized, discarded, remember, notice;
} Speech;
typedef struct {
	Speech *speech;
	gsize size;
} Playback;
typedef struct {
	gchar *text;
	gboolean remember, notice;
} Line;

struct _AiVoiceSession {
	GObject parent_instance;
	AiAudioTransport *transport;
	AiSpeechRecognizer *recognizer;
	AiSpeechSynthesizer *synthesizer;
	AiVoiceActivity *activity;
	AiVoiceWorker *worker;
	AiConversation *conversation;
	gchar *deadline_message, *transcription_error_message;
	GMainContext *context;
	GHashTable *participants, *notices;
	GQueue turns, lines, pending_notices;
	GString *pending_text, *spoken;
	Speech *speech;
	GCancellable *turn_cancel;
	GSource *deadline;
	guint deadline_ms;
	guint64 generation, provider_generation;
	AiVoiceState state;
	gboolean stopped, provider_pending;
};
enum {
	PROP_0,
	PROP_TURN_DEADLINE,
	PROP_STATE,
	PROP_TRANSPORT,
	PROP_RECOGNIZER,
	PROP_SYNTHESIZER,
	PROP_ACTIVITY,
	PROP_CONVERSATION,
	PROP_DEADLINE_MESSAGE,
	PROP_TRANSCRIPTION_ERROR_MESSAGE
};
G_DEFINE_TYPE(AiVoiceSession, ai_voice_session, G_TYPE_OBJECT)
static void
pump(AiVoiceSession *self);
static void
interrupt_turn(AiVoiceSession *self);
static void
stt_error(AiSpeechRecognizer *recognizer, const gchar *speaker, GError *error,
		  gpointer data);
static void
line_free(gpointer data)
{
	Line *line = data;
	g_free(line->text);
	g_free(line);
}
static void
participant_free(gpointer data)
{
	Participant *p = data;
	g_free(p->name);
	g_byte_array_unref(p->frames);
	g_free(p);
}
static void
state(AiVoiceSession *self, AiVoiceState next)
{
	if (self->state == next)
		return;
	self->state = next;
	g_object_notify(G_OBJECT(self), "state");
	g_signal_emit_by_name(self, "state-changed", (gint)next);
}
static void
report(AiVoiceSession *self, const GError *error)
{
	if (error != NULL && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_signal_emit_by_name(self, "error", error);
}
static void
clear_deadline(AiVoiceSession *self)
{
	if (self->deadline == NULL)
		return;
	g_source_destroy(self->deadline);
	g_clear_pointer(&self->deadline, g_source_unref);
}
static void
queue_line(AiVoiceSession *self, const gchar *text, gboolean remember)
{
	Line *line;
	if (text == NULL || *text == '\0' || self->stopped)
		return;
	if (g_queue_get_length(&self->lines) >= 128)
		return;
	line = g_new0(Line, 1);
	line->text = g_strdup(text);
	line->remember = remember;
	g_queue_push_tail(&self->lines, line);
}
static void
segment(AiVoiceSession *self, gboolean final)
{
	gsize i = 0;
	while (i < self->pending_text->len) {
		gchar c = self->pending_text->str[i];
		if (c == '.' || c == '!' || c == '?' || c == '\n') {
			g_autofree gchar *text = g_strndup(self->pending_text->str, i + 1);
			queue_line(self, g_strstrip(text), TRUE);
			g_string_erase(self->pending_text, 0, i + 1);
			i = 0;
		} else
			i++;
	}
	if (final && self->pending_text->len != 0) {
		queue_line(self, self->pending_text->str, TRUE);
		g_string_truncate(self->pending_text, 0);
	}
}
static void
speech_complete(Speech *s)
{
	AiVoiceSession *self = s->session;
	if (!s->synthesized || s->pending != 0)
		return;
	self->speech = NULL;
	if (!s->discarded && s->generation == self->generation && s->remember) {
		if (self->spoken->len != 0)
			g_string_append_c(self->spoken, ' ');
		g_string_append(self->spoken, s->text);
		if (self->worker != NULL)
			ai_voice_worker_spoken(self->worker, self->spoken->str,
								   self->provider_generation);
	}
	g_clear_object(&s->cancel);
	g_free(s->text);
	g_free(s);
	pump(self);
	g_object_unref(self);
}
static void
played(GObject *source, GAsyncResult *result, gpointer data)
{
	Playback *p = data;
	Speech *s = p->speech;
	g_autoptr(GError) error = NULL;
	if (!ai_audio_transport_write_finish(AI_AUDIO_TRANSPORT(source), result, &error)) {
		s->discarded = TRUE;
		report(s->session, error);
	}
	s->pending--;
	s->queued -= p->size;
	g_free(p);
	speech_complete(s);
}
static void
audio_out(AiSpeechSynthesizer *synthesizer, GBytes *pcm, gpointer data)
{
	AiVoiceSession *self = data;
	Speech *s = self->speech;
	Playback *p;
	gsize size = g_bytes_get_size(pcm);
	if (s == NULL || s->discarded || g_cancellable_is_cancelled(s->cancel))
		return;
	if (size % 2 != 0 || size + s->queued > 4 * 1024 * 1024) {
		g_autoptr(GError) error = g_error_new_literal(
			G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Voice playback queue exceeded its bound");
		report(self, error);
		interrupt_turn(self);
		return;
	}
	p = g_new0(Playback, 1);
	p->speech = s;
	p->size = size;
	s->pending++;
	s->queued += size;
	ai_audio_transport_write_async(self->transport, pcm, s->cancel, played, p);
}
static void
synthesized(GObject *source, GAsyncResult *result, gpointer data)
{
	Speech *s = data;
	g_autoptr(GError) error = NULL;
	if (!ai_speech_synthesizer_synthesize_finish(AI_SPEECH_SYNTHESIZER(source), result,
												 &error)) {
		s->discarded = TRUE;
		report(s->session, error);
	}
	s->synthesized = TRUE;
	speech_complete(s);
}
static gboolean
deadline(gpointer data)
{
	AiVoiceSession *self = data;
	g_clear_pointer(&self->deadline, g_source_unref);
	interrupt_turn(self);
	queue_line(self, self->deadline_message, FALSE);
	pump(self);
	return G_SOURCE_REMOVE;
}
static void
start_turn(AiVoiceSession *self, const gchar *text)
{
	self->generation++;
	self->provider_generation = self->generation;
	self->provider_pending = TRUE;
	g_clear_object(&self->turn_cancel);
	self->turn_cancel = g_cancellable_new();
	g_string_truncate(self->spoken, 0);
	state(self, AI_VOICE_THINKING);
	self->deadline = g_timeout_source_new(self->deadline_ms);
	g_source_set_callback(self->deadline, deadline, self, NULL);
	g_source_attach(self->deadline, self->context);
	ai_voice_worker_send(self->worker, text, self->turn_cancel, self->generation);
}
static gboolean
recognizing(AiVoiceSession *self)
{
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, self->participants);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (((Participant *)value)->recognizing)
			return TRUE;
	return FALSE;
}
static void
pump(AiVoiceSession *self)
{
	Line *line;
	if (self->stopped || self->speech != NULL)
		return;
	line = g_queue_pop_head(&self->lines);
	if (line == NULL && !self->provider_pending && !recognizing(self) &&
		g_queue_is_empty(&self->turns))
		line = g_queue_pop_head(&self->pending_notices);
	if (line != NULL) {
		Speech *s = g_new0(Speech, 1);
		s->session = g_object_ref(self);
		s->text = g_steal_pointer(&line->text);
		s->remember = line->remember;
		s->notice = line->notice;
		s->cancel = g_cancellable_new();
		s->generation = self->generation;
		self->speech = s;
		line_free(line);
		state(self, AI_VOICE_SPEAKING);
		g_signal_emit_by_name(self, "reply", s->text);
		ai_speech_synthesizer_synthesize_async(self->synthesizer, s->text, s->cancel,
											   synthesized, s);
		return;
	}
	if (!self->provider_pending) {
		g_autofree gchar *turn = g_queue_pop_head(&self->turns);
		clear_deadline(self);
		if (turn != NULL)
			start_turn(self, turn);
		else
			state(self, recognizing(self) ? AI_VOICE_TRANSCRIBING : AI_VOICE_LISTENING);
	} else if (self->generation != self->provider_generation)
		state(self, recognizing(self) ? AI_VOICE_TRANSCRIBING : AI_VOICE_LISTENING);
	else
		state(self, AI_VOICE_THINKING);
}
static void
interrupt_turn(AiVoiceSession *self)
{
	clear_deadline(self);
	if (self->turn_cancel != NULL)
		g_cancellable_cancel(self->turn_cancel);
	if (self->speech != NULL) {
		if (!self->stopped && self->speech->notice && !self->speech->discarded) {
			Line *notice = g_new0(Line, 1);
			notice->text = g_strdup(self->speech->text);
			notice->notice = TRUE;
			g_queue_push_head(&self->pending_notices, notice);
		}
		self->speech->discarded = TRUE;
		g_cancellable_cancel(self->speech->cancel);
	}
	if (self->transport != NULL)
		ai_audio_transport_flush(self->transport);
	g_queue_clear_full(&self->lines, line_free);
	g_string_truncate(self->pending_text, 0);
	self->generation++;
}
static void
worker_mail(GObject *object, AiVoiceMailKind kind, guint64 generation, AiEvent *event,
			const gchar *text, const GError *error)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	if (self->stopped)
		return;
	if (kind == AI_VOICE_MAIL_AGENT) {
		if (!g_hash_table_contains(self->notices, text)) {
			Line *notice = g_new0(Line, 1);
			notice->text = g_strdup(text);
			notice->notice = TRUE;
			g_hash_table_add(self->notices, g_strdup(text));
			g_queue_push_tail(&self->pending_notices, notice);
		}
	} else if (kind == AI_VOICE_MAIL_DONE) {
		if (generation == self->provider_generation)
			self->provider_pending = FALSE;
		if (generation == self->generation) {
			if (error != NULL) {
				g_autofree gchar *line =
					g_strdup_printf("There was an error: %s", error->message);
				report(self, error);
				queue_line(self, line, FALSE);
			} else {
				if (text != NULL)
					g_string_append(self->pending_text, text);
				segment(self, TRUE);
			}
		}
	} else if (generation == self->generation) {
		AiEventKind k = ai_event_get_kind(event);
		if (k == AI_EVENT_TEXT_DELTA) {
			if (self->pending_text->len < 65536)
				g_string_append(self->pending_text, ai_event_get_text(event));
			segment(self, FALSE);
		} else if (k == AI_EVENT_TOOL_FINISHED) {
			AiToolResult *result = ai_event_get_tool_result(event);
			if (result != NULL && ai_tool_result_get_is_error(result)) {
				g_autofree gchar *line =
					g_strdup_printf("The tool reported an error: %.512s",
									ai_tool_result_get_content(result));
				queue_line(self, line, FALSE);
			}
		}
	}
	pump(self);
}
static void
joined(AiAudioTransport *transport, const gchar *speaker, const gchar *name,
	   gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p;
	if (self->stopped || g_hash_table_contains(self->participants, speaker))
		return;
	p = g_new0(Participant, 1);
	p->name = g_strdup(name != NULL && *name != '\0' ? name : speaker);
	p->frames = g_byte_array_new();
	g_hash_table_insert(self->participants, g_strdup(speaker), p);
}
static void
left(AiAudioTransport *transport, const gchar *speaker, gpointer data)
{
	AiVoiceSession *self = data;
	ai_speech_recognizer_cancel(self->recognizer, speaker);
	ai_voice_activity_reset(self->activity, speaker);
	g_hash_table_remove(self->participants, speaker);
	pump(self);
}
static void
audio_in(AiAudioTransport *transport, const gchar *speaker, GBytes *pcm, gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p = g_hash_table_lookup(self->participants, speaker);
	gsize n;
	const guint8 *raw = g_bytes_get_data(pcm, &n);
	if (self->stopped || p == NULL || n % 2 != 0 || n > 32000)
		return;
	g_byte_array_append(p->frames, raw, n);
	while (p->frames->len >= 320) {
		g_autoptr(GBytes) frame = g_bytes_new(p->frames->data, 320);
		g_autoptr(GError) error = NULL;
		gint activity;
		g_byte_array_remove_range(p->frames, 0, 320);
		activity = ai_voice_activity_process(self->activity, speaker, frame, &error);
		if (activity < 0) {
			report(self, error);
			return;
		}
		if (activity & AI_VOICE_ACTIVITY_SPEECH) {
			if (self->state == AI_VOICE_SPEAKING || self->state == AI_VOICE_THINKING)
				interrupt_turn(self);
			if (p->ended) {
				ai_speech_recognizer_cancel(self->recognizer, speaker);
				p->recognizing = FALSE;
				p->ended = FALSE;
			}
			if (!p->recognizing)
				p->recognizing =
					ai_speech_recognizer_begin(self->recognizer, speaker, &error);
			state(self, AI_VOICE_TRANSCRIBING);
		}
		if (p->recognizing && !p->ended && error == NULL)
			ai_speech_recognizer_feed(self->recognizer, speaker, frame, &error);
		if (error != NULL) {
			ai_speech_recognizer_cancel(self->recognizer, speaker);
			stt_error(self->recognizer, speaker, error, self);
			return;
		}
		if ((activity & AI_VOICE_ACTIVITY_END) && p->recognizing && !p->ended) {
			p->ended = TRUE;
			ai_speech_recognizer_end(self->recognizer, speaker);
			if (self->stopped || g_hash_table_lookup(self->participants, speaker) != p)
				return;
		}
	}
}
static void
transcript(AiSpeechRecognizer *recognizer, const gchar *speaker, const gchar *text,
		   gboolean final, gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p = g_hash_table_lookup(self->participants, speaker);
	g_autofree gchar *labelled = NULL;
	if (self->stopped || p == NULL)
		return;
	if (!final) {
		g_signal_emit_by_name(self, "transcript", p->name, text, FALSE);
		return;
	}
	p->recognizing = FALSE;
	p->ended = FALSE;
	ai_voice_activity_reset(self->activity, speaker);
	if (text != NULL && *text != '\0' && g_queue_get_length(&self->turns) < 32) {
		labelled = g_strdup_printf("[%s]: %s", p->name, text);
		g_queue_push_tail(&self->turns, g_steal_pointer(&labelled));
	}
	g_signal_emit_by_name(self, "transcript", p->name, text, TRUE);
	pump(self);
}
static void
stt_error(AiSpeechRecognizer *recognizer, const gchar *speaker, GError *error,
		  gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p = g_hash_table_lookup(self->participants, speaker);
	if (self->stopped)
		return;
	ai_voice_activity_reset(self->activity, speaker);
	if (p != NULL) {
		p->recognizing = FALSE;
		p->ended = FALSE;
	}
	report(self, error);
	queue_line(self, self->transcription_error_message, FALSE);
	pump(self);
}
static void
transport_error(AiAudioTransport *transport, GError *error, gpointer data)
{
	AiVoiceSession *self = data;
	report(self, error);
	ai_voice_session_stop(self);
}
/**
 * ai_voice_session_stop:
 * @self: a voice session
 *
 * Cancels all speech and conversation work. Call before releasing an active
 * session. Room membership and transport leave belong to the embedding host.
 */
void
ai_voice_session_stop(AiVoiceSession *self)
{
	GHashTableIter iter;
	gpointer key;
	g_return_if_fail(AI_IS_VOICE_SESSION(self));
	if (self->stopped)
		return;
	self->stopped = TRUE;
	interrupt_turn(self);
	g_hash_table_iter_init(&iter, self->participants);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		ai_speech_recognizer_cancel(self->recognizer, key);
		ai_voice_activity_reset(self->activity, key);
	}
	g_hash_table_remove_all(self->participants);
	if (self->worker != NULL) {
		ai_voice_worker_stop(self->worker);
		self->worker = NULL;
	}
	state(self, AI_VOICE_LISTENING);
}
static void
dispose(GObject *object)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	ai_voice_session_stop(self);
	if (self->transport != NULL)
		g_signal_handlers_disconnect_by_data(self->transport, self);
	if (self->recognizer != NULL)
		g_signal_handlers_disconnect_by_data(self->recognizer, self);
	if (self->synthesizer != NULL)
		g_signal_handlers_disconnect_by_data(self->synthesizer, self);
	g_clear_object(&self->transport);
	g_clear_object(&self->recognizer);
	g_clear_object(&self->synthesizer);
	g_clear_object(&self->activity);
	g_clear_object(&self->conversation);
	g_clear_object(&self->turn_cancel);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	g_hash_table_unref(self->participants);
	g_hash_table_unref(self->notices);
	g_main_context_unref(self->context);
	g_queue_clear_full(&self->lines, line_free);
	g_queue_clear_full(&self->turns, g_free);
	g_queue_clear_full(&self->pending_notices, line_free);
	g_string_free(self->pending_text, TRUE);
	g_string_free(self->spoken, TRUE);
	g_free(self->deadline_message);
	g_free(self->transcription_error_message);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->finalize(object);
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	if (id == PROP_TURN_DEADLINE)
		g_value_set_uint(value, self->deadline_ms);
	else if (id == PROP_STATE)
		g_value_set_enum(value, self->state);
	else if (id == PROP_TRANSPORT)
		g_value_set_object(value, self->transport);
	else if (id == PROP_RECOGNIZER)
		g_value_set_object(value, self->recognizer);
	else if (id == PROP_SYNTHESIZER)
		g_value_set_object(value, self->synthesizer);
	else if (id == PROP_ACTIVITY)
		g_value_set_object(value, self->activity);
	else if (id == PROP_CONVERSATION)
		g_value_set_object(value, self->conversation);
	else if (id == PROP_DEADLINE_MESSAGE)
		g_value_set_string(value, self->deadline_message);
	else if (id == PROP_TRANSCRIPTION_ERROR_MESSAGE)
		g_value_set_string(value, self->transcription_error_message);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	if (id == PROP_TURN_DEADLINE)
		self->deadline_ms = g_value_get_uint(value);
	else if (id == PROP_TRANSPORT)
		g_set_object(&self->transport, g_value_get_object(value));
	else if (id == PROP_RECOGNIZER)
		g_set_object(&self->recognizer, g_value_get_object(value));
	else if (id == PROP_SYNTHESIZER)
		g_set_object(&self->synthesizer, g_value_get_object(value));
	else if (id == PROP_ACTIVITY)
		g_set_object(&self->activity, g_value_get_object(value));
	else if (id == PROP_CONVERSATION)
		g_set_object(&self->conversation, g_value_get_object(value));
	else if (id == PROP_DEADLINE_MESSAGE) {
		g_free(self->deadline_message);
		self->deadline_message = g_value_dup_string(value);
	} else if (id == PROP_TRANSCRIPTION_ERROR_MESSAGE) {
		g_free(self->transcription_error_message);
		self->transcription_error_message = g_value_dup_string(value);
	} else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
constructed(GObject *object)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->constructed(object);
	g_return_if_fail(self->transport != NULL && self->recognizer != NULL &&
					 self->synthesizer != NULL && self->activity != NULL &&
					 self->conversation != NULL);
	g_signal_connect(self->transport, "audio", G_CALLBACK(audio_in), self);
	g_signal_connect(self->transport, "participant-joined", G_CALLBACK(joined), self);
	g_signal_connect(self->transport, "participant-left", G_CALLBACK(left), self);
	g_signal_connect(self->transport, "error", G_CALLBACK(transport_error), self);
	g_signal_connect(self->recognizer, "transcript", G_CALLBACK(transcript), self);
	g_signal_connect(self->recognizer, "error", G_CALLBACK(stt_error), self);
	g_signal_connect(self->synthesizer, "audio", G_CALLBACK(audio_out), self);
	self->worker = ai_voice_worker_new(self->conversation, G_OBJECT(self), self->context,
									   worker_mail);
}
static void
ai_voice_session_class_init(AiVoiceSessionClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->dispose = dispose;
	oc->constructed = constructed;
	oc->finalize = finalize;
	oc->get_property = get_property;
	oc->set_property = set_property;
	g_object_class_install_property(
		oc, PROP_TURN_DEADLINE,
		g_param_spec_uint("turn-deadline-ms", "Turn deadline", "Maximum turn duration", 1,
						  3600000, 20000, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_STATE,
		g_param_spec_enum("state", "State", "Current voice state", AI_TYPE_VOICE_STATE,
						  AI_VOICE_LISTENING, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_TRANSPORT,
		g_param_spec_object(
			"transport", "Transport", "Injected audio transport", AI_TYPE_AUDIO_TRANSPORT,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_RECOGNIZER,
		g_param_spec_object("recognizer", "Recognizer", "Injected speech recognizer",
							AI_TYPE_SPEECH_RECOGNIZER,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
								G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_SYNTHESIZER,
		g_param_spec_object("synthesizer", "Synthesizer", "Injected speech synthesizer",
							AI_TYPE_SPEECH_SYNTHESIZER,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
								G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_ACTIVITY,
		g_param_spec_object(
			"activity", "Activity", "Injected voice detector", AI_TYPE_VOICE_ACTIVITY,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_CONVERSATION,
		g_param_spec_object("conversation", "Conversation",
							"Exclusively owned conversation", AI_TYPE_CONVERSATION,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
								G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_DEADLINE_MESSAGE,
		g_param_spec_string(
			"deadline-message", "Deadline message", "Spoken turn timeout fallback",
			"Sorry, that is taking too long. Please try again.",
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_TRANSCRIPTION_ERROR_MESSAGE,
		g_param_spec_string("transcription-error-message", "Transcription error message",
							"Spoken recognition failure fallback",
							"Sorry, I could not transcribe that. Please try again.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_STRINGS));

	/**
	 * AiVoiceSession::transcript:
	 * @self: the session
	 * @speaker: display name
	 * @text: recognized text
	 * @final: whether the utterance is complete
	 */
	g_signal_new("transcript", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
	/**
	 * AiVoiceSession::reply:
	 * @self: the session
	 * @text: a segment submitted for synthesis
	 */
	g_signal_new("reply", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
	/**
	 * AiVoiceSession::state-changed:
	 * @self: the session
	 * @state: the new #AiVoiceState
	 */
	g_signal_new("state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL,
				 NULL, NULL, G_TYPE_NONE, 1, AI_TYPE_VOICE_STATE);
	/**
	 * AiVoiceSession::error:
	 * @self: the session
	 * @error: the failure
	 */
	g_signal_new("error", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 1, G_TYPE_ERROR);
}
static void
ai_voice_session_init(AiVoiceSession *self)
{
	self->context = g_main_context_ref_thread_default();
	self->participants =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, participant_free);
	self->notices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->pending_text = g_string_new(NULL);
	self->spoken = g_string_new(NULL);
	self->deadline_ms = 20000;
}
/**
 * ai_voice_session_new:
 * @transport: audio transport
 * @recognizer: streaming recognizer
 * @synthesizer: streaming synthesizer
 * @activity: per-participant voice detector
 * @conversation: conversation exclusively lent to the session until stop
 *
 * Audio APIs and signals use the constructing thread-default context. The
 * conversation runs on a dedicated context so existing synchronous tools
 * cannot block audio or cancellation. Do not drive it concurrently.
 *
 * Returns: (transfer full): a listening voice session
 */
AiVoiceSession *
ai_voice_session_new(AiAudioTransport *transport, AiSpeechRecognizer *recognizer,
					 AiSpeechSynthesizer *synthesizer, AiVoiceActivity *activity,
					 AiConversation *conversation)
{
	g_autoptr(AiVoiceSession) self = NULL;
	g_return_val_if_fail(AI_IS_AUDIO_TRANSPORT(transport), NULL);
	g_return_val_if_fail(AI_IS_SPEECH_RECOGNIZER(recognizer), NULL);
	g_return_val_if_fail(AI_IS_SPEECH_SYNTHESIZER(synthesizer), NULL);
	g_return_val_if_fail(AI_IS_VOICE_ACTIVITY(activity), NULL);
	g_return_val_if_fail(AI_IS_CONVERSATION(conversation), NULL);
	self = g_object_new(AI_TYPE_VOICE_SESSION, "transport", transport, "recognizer",
						recognizer, "synthesizer", synthesizer, "activity", activity,
						"conversation", conversation, NULL);

	return g_steal_pointer(&self);
}
/**
 * ai_voice_session_get_state:
 * @self: a voice session
 *
 * Returns: the current state
 */
AiVoiceState
ai_voice_session_get_state(AiVoiceSession *self)
{
	g_return_val_if_fail(AI_IS_VOICE_SESSION(self), AI_VOICE_LISTENING);
	return self->state;
}
/**
 * ai_voice_session_say:
 * @self: a voice session
 * @text: greeting or other out-of-band line
 *
 * Queues an interruptible spoken notice without starting a provider turn.
 */
void
ai_voice_session_say(AiVoiceSession *self, const gchar *text)
{
	g_return_if_fail(AI_IS_VOICE_SESSION(self));
	queue_line(self, text, FALSE);
	pump(self);
}
