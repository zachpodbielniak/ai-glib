/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voice-session.h"
#include "voice/ai-voice-worker-private.h"
#include "model/ai-tool-result.h"
#include "core/ai-provider.h"
#include "voice/ai-speaker-identifier.h"
#include "voice/ai-voice-room.h"
#include "voice/ai-address-detector.h"
#include "voice/ai-relevance-judge.h"
#include <string.h>

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
	GByteArray *frames, *onset;
	GByteArray *preroll; /* the last PREROLL_BYTES of audio, speech or not */
	GByteArray *utterance; /* audio sent to STT since recognition began, for naming */
	gchar *last_named;	   /* who a speaker identifier last said this was */
	gint64 last_named_us;
	guint speech_ms;
	gboolean recognizing, ended, pending_barge;
} Participant;
typedef struct {
	AiVoiceSession *session;
	gchar *text;
	GCancellable *cancel;
	guint64 generation;
	guint pending;
	gsize queued;
	gboolean synthesized, discarded, remember, notice, fallback, heard;
} Speech;
typedef struct {
	Speech *speech;
	gsize size;
} Playback;
typedef struct {
	gchar *text;
	gboolean remember, notice, fallback;
} Line;
typedef struct {
	gchar *text;
	gint64 at;
} Said;

typedef struct {
	AiAudioTransport *transport;
	AiSpeechRecognizer *recognizer;
	AiSpeechSynthesizer *synthesizer;
	AiVoiceActivity *activity;
	AiVoiceWorker *worker;
	AiConversation *conversation;
	gchar *deadline_message, *transcription_error_message, *synthesis_error_message;
	gchar *empty_reply_message, *tool_progress_message;
	guint turn_lines; /* lines queued since the current provider turn began */
	guint tool_progress_delay_ms;
	guint trim_after; /* 0: history keeps every tool result in full */
	/* speak-code off: this reply's position relative to markdown code */
	gboolean speak_code, in_fence, in_inline;
	guint ticks; /* backticks at the end of the last delta, not yet resolved */
	GString *span; /* the open inline span, decided when it closes */
	/* repeat-limit: copies of each line this reply, and whether it was stopped */
	guint repeat_limit;
	gchar *repeat_message;
	/* voice-commands: spoken controls, and whether the caller is being heard */
	gboolean voice_commands, muted;
	gchar **assistant_names;
	gchar *mute_message, *unmute_message;
	gchar *promoted_message, *promotion_refused_message;
	GHashTable *repeats;
	gboolean runaway;
	GSource *progress; /* pending tool-progress line, held for teardown */
	GSource *hold;     /* flushes a delta that ended on a period */
	guint64 progress_generation;
	gboolean progress_used; /* at most one per turn */
	GBytes *fallback_pcm;
	guint fallback_sample_rate;
	GMainContext *context;
	GHashTable *participants, *notices;
	GQueue turns, lines, pending_notices, said;
	GString *pending_text, *spoken;
	Speech *speech;
	GCancellable *turn_cancel;
	GSource *deadline;
	guint deadline_ms, barge_in_ms;
	gint64 paused_deadline_us;
	guint64 generation, provider_generation;
	AiVoiceState state;
	gboolean stopped, provider_pending, media_recovering, barge_in_confirm;
	/* room-mode: talk the address detector does not pass is heard, not answered */
	gboolean room_mode;
	AiVoiceRoom *room;		   /* what was heard since the last turn, and who is present */
	AiAddressDetector *detector; /* the application's, or NULL for name_detector */
	AiNameAddressDetector *name_detector; /* from assistant-names and -soundalikes */
	AiRelevanceJudge *judge;	 /* NULL: never chime in */
	guint chime_threshold, chime_cooldown_ms, relevance_debounce_ms;
	GCancellable *scoring; /* the one score in flight */
	gboolean rescore;	   /* talk arrived while it was in flight */
	GSource *debounce;
	gint64 last_turn_us;
	gchar *logged_tool; /* id of the last tool whose start was logged */
	/* speaker-identifier: finals wait here, in order, until each is named */
	GObject *identifier;
	gchar *unknown_speaker_name;
	GQueue naming;
	guint naming_timeout_ms; /* the longest a final waits to be named */
} AiVoiceSessionPrivate;
enum {
	PROP_0,
	PROP_TURN_DEADLINE,
	PROP_BARGE_IN,
	PROP_STATE,
	PROP_TRANSPORT,
	PROP_RECOGNIZER,
	PROP_SYNTHESIZER,
	PROP_ACTIVITY,
	PROP_CONVERSATION,
	PROP_DEADLINE_MESSAGE,
	PROP_TRANSCRIPTION_ERROR_MESSAGE,
	PROP_SYNTHESIS_ERROR_MESSAGE,
	PROP_FALLBACK_PCM,
	PROP_FALLBACK_SAMPLE_RATE,
	PROP_BARGE_IN_CONFIRM,
	PROP_EMPTY_REPLY_MESSAGE,
	PROP_TOOL_PROGRESS_MESSAGE,
	PROP_TOOL_PROGRESS_DELAY,
	PROP_TRIM_AFTER,
	PROP_SPEAK_CODE,
	PROP_REPEAT_LIMIT,
	PROP_REPEAT_MESSAGE,
	PROP_VOICE_COMMANDS,
	PROP_ASSISTANT_NAMES,
	PROP_MUTED,
	PROP_MUTE_MESSAGE,
	PROP_UNMUTE_MESSAGE,
	PROP_ROOM_MODE,
	PROP_RELEVANCE_PROVIDER,
	PROP_CHIME_IN_THRESHOLD,
	PROP_CHIME_IN_COOLDOWN,
	PROP_SPEAKER_IDENTIFIER,
	PROP_UNKNOWN_SPEAKER_NAME,
	PROP_ASSISTANT_SOUNDALIKES,
	PROP_NAMING_TIMEOUT,
	PROP_ROOM,
	PROP_ADDRESS_DETECTOR,
	PROP_RELEVANCE_JUDGE,
	PROP_RELEVANCE_DEBOUNCE,
	PROP_PROMOTED_MESSAGE,
	PROP_PROMOTION_REFUSED_MESSAGE
};
G_DEFINE_TYPE_WITH_PRIVATE(AiVoiceSession, ai_voice_session, G_TYPE_OBJECT)
/* Every field lives in the private struct, so a subclass's instance can grow
 * without breaking this one's layout. */
#define P(o) ((AiVoiceSessionPrivate *)ai_voice_session_get_instance_private(o))
/* A word that starts softly -- "f", "h", "th" -- is not speech to the
 * detector until its vowel, so recognition would begin mid-word: "Fai" heard
 * as "-ay", and transcribed "say" or "Bay". This much of what came just
 * before speech is sent too. */
#define PREROLL_BYTES (300 * 16 * 2)
#define UTTERANCE_BYTES (30 * 16000 * 2) /* the most of one utterance kept for naming */
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
said_free(gpointer data)
{
	Said *said = data;
	g_free(said->text);
	g_free(said);
}
/* What we said recently, so our own voice coming back through a caller's
 * speaker is recognised as echo rather than answered as a turn. */
#define ECHO_WINDOW_US (20 * G_USEC_PER_SEC)
static void
remember_said(AiVoiceSession *self, const gchar *text)
{
	Said *said = g_new0(Said, 1);
	said->text = g_strdup(text);
	said->at = g_get_monotonic_time();
	g_queue_push_tail(&P(self)->said, said);
	while (g_queue_get_length(&P(self)->said) > 16)
		said_free(g_queue_pop_head(&P(self)->said));
}
static void
add_words(GHashTable *words, const gchar *text)
{
	GString *word = g_string_new(NULL);
	const gchar *p;
	for (p = text;; p = g_utf8_next_char(p)) {
		gunichar c = *p != '\0' ? g_utf8_get_char(p) : 0;
		if (c != 0 && g_unichar_isalnum(c)) {
			g_string_append_unichar(word, g_unichar_tolower(c));
			continue;
		}
		if (word->len != 0)
			g_hash_table_add(words, g_strdup(word->str));
		g_string_truncate(word, 0);
		if (c == 0)
			break;
	}
	g_string_free(word, TRUE);
}
/* Echo when at least four in five of the words heard are words we just said. */
static gboolean
is_echo(AiVoiceSession *self, const gchar *text)
{
	g_autoptr(GHashTable) ours = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GHashTable) heard = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	gint64 now = g_get_monotonic_time();
	guint total = 0, matched = 0;
	GHashTableIter iter;
	gpointer word;
	GList *l;
	for (l = P(self)->said.head; l != NULL; l = l->next) {
		Said *said = l->data;
		if (now - said->at <= ECHO_WINDOW_US)
			add_words(ours, said->text);
	}
	add_words(heard, text);
	g_hash_table_iter_init(&iter, heard);
	while (g_hash_table_iter_next(&iter, &word, NULL)) {
		total++;
		if (g_hash_table_contains(ours, word))
			matched++;
	}
	return total != 0 && matched * 5 >= total * 4;
}
/* Recognizers mark non-speech inline: [throat clearing], (coughs), *sniff*,
 * [BLANK_AUDIO]. Those are not words said to us. Remove each bracketed,
 * parenthesized or starred span of up to 60 bytes, then collapse whitespace.
 * An unclosed opener is kept as text. */
static gchar *
strip_annotations(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p = text != NULL ? text : "";
	while (*p != '\0') {
		gchar close = *p == '[' ? ']' : *p == '(' ? ')' : *p == '*' ? '*' : '\0';
		const gchar *end = close != '\0' ? strchr(p + 1, close) : NULL;
		if (end != NULL && end - p <= 60) {
			if (out->len != 0 && out->str[out->len - 1] != ' ')
				g_string_append_c(out, ' ');
			p = end + 1;
			continue;
		}
		if (g_ascii_isspace(*p)) {
			if (out->len != 0 && out->str[out->len - 1] != ' ')
				g_string_append_c(out, ' ');
		} else {
			/* No space before punctuation left behind by a removed span. */
			if ((*p == ',' || *p == '.' || *p == '!' || *p == '?') && out->len != 0 &&
				out->str[out->len - 1] == ' ')
				g_string_truncate(out, out->len - 1);
			g_string_append_c(out, *p);
		}
		p++;
	}
	return g_strstrip(g_string_free(out, FALSE));
}
static gboolean
has_words(const gchar *text)
{
	const gchar *p;
	for (p = text; p != NULL && *p != '\0'; p = g_utf8_next_char(p))
		if (g_unichar_isalnum(g_utf8_get_char(p)))
			return TRUE;
	return FALSE;
}
static void
participant_free(gpointer data)
{
	Participant *p = data;
	g_free(p->name);
	g_byte_array_unref(p->frames);
	g_byte_array_unref(p->onset);
	g_byte_array_unref(p->preroll);
	g_byte_array_unref(p->utterance);
	g_free(p->last_named);
	g_free(p);
}
static void
state(AiVoiceSession *self, AiVoiceState next)
{
	if (P(self)->state == next)
		return;
	P(self)->state = next;
	g_object_notify(G_OBJECT(self), "state");
	g_signal_emit_by_name(self, "state-changed", (gint)next);
}
static void
report(AiVoiceSession *self, const GError *error)
{
	if (error != NULL && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Voice service error: %s", error->message);
		g_signal_emit_by_name(self, "error", error);
	}
}
static void
clear_deadline(AiVoiceSession *self)
{
	if (P(self)->deadline == NULL)
		return;
	g_source_destroy(P(self)->deadline);
	g_clear_pointer(&P(self)->deadline, g_source_unref);
}
/* Emoji, pictographs and markdown markup have no pronunciation. A TTS model
 * handed only those does not fail: it invents audio, and a seeded one invents
 * the same few seconds of noise every time. */
static gboolean
unspoken_char(gunichar c)
{
	switch (g_unichar_type(c)) {
	case G_UNICODE_OTHER_SYMBOL:
	case G_UNICODE_FORMAT:
	case G_UNICODE_PRIVATE_USE:
	case G_UNICODE_SURROGATE:
	case G_UNICODE_UNASSIGNED:
		return TRUE;
	default:
		break;
	}
	return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) ||
		   (c >= 0x1F3FB && c <= 0x1F3FF) || c == '*' || c == '`' || c == '~';
}
/* Returns NULL when nothing pronounceable remains. */
static gchar *
speakable(const gchar *text)
{
	GString *out = g_string_new(NULL);
	gboolean alnum = FALSE, space = FALSE;
	const gchar *p = text;
	/* Markdown block markers ("## ", "> ", "- ") would be read aloud. */
	for (;;) {
		const gchar *q;
		while (g_ascii_isspace(*p))
			p++;
		for (q = p; *q == '#' || *q == '>' || *q == '-' || *q == '+'; q++)
			;
		if (q == p || !g_ascii_isspace(*q))
			break;
		p = q;
	}
	for (; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		if (unspoken_char(c))
			continue;
		if (g_unichar_isspace(c)) {
			space = out->len != 0;
			continue;
		}
		if (space)
			g_string_append_c(out, ' ');
		space = FALSE;
		alnum |= g_unichar_isalnum(c);
		g_string_append_unichar(out, c);
	}
	if (!alnum) {
		g_string_free(out, TRUE);
		return NULL;
	}
	return g_string_free(out, FALSE);
}
static void
queue_line_full(AiVoiceSession *self, const gchar *text, gboolean remember,
				gboolean counted)
{
	Line *line;
	g_autofree gchar *valid = NULL;
	g_autofree gchar *spoken = NULL;
	if (text == NULL || P(self)->stopped)
		return;
	if (g_queue_get_length(&P(self)->lines) >= 128)
		return;
	/* A byte-bounded cut can split a character, and the synthesizer refuses
	 * invalid UTF-8, which would read as a TTS outage. The replacement
	 * character is a symbol, so speakable() drops it. */
	valid = g_utf8_make_valid(text, -1);
	spoken = speakable(valid);
	if (spoken == NULL) {
		g_debug("voice: not speaking unpronounceable segment '%s'", valid);
		return;
	}
	line = g_new0(Line, 1);
	line->text = g_steal_pointer(&spoken);
	line->remember = remember;
	g_queue_push_tail(&P(self)->lines, line);
	if (counted)
		P(self)->turn_lines++;
}
static void
queue_line(AiVoiceSession *self, const gchar *text, gboolean remember)
{
	queue_line_full(self, text, remember, TRUE);
}
static void
clear_progress(AiVoiceSession *self)
{
	if (P(self)->progress == NULL)
		return;
	g_source_destroy(P(self)->progress);
	g_clear_pointer(&P(self)->progress, g_source_unref);
}
static void
pump(AiVoiceSession *self);
/* Fires only if the tool is still running and nothing has been said: a
 * quick lookup, or a model that already said "let me look", hears nothing. */
static gboolean
progress_fire(gpointer data)
{
	AiVoiceSession *self = data;
	g_clear_pointer(&P(self)->progress, g_source_unref);
	if (P(self)->stopped || !P(self)->provider_pending ||
		P(self)->progress_generation != P(self)->generation || P(self)->turn_lines != 0)
		return G_SOURCE_REMOVE;
	/* Not counted: it is not a reply, so an empty turn still says so. */
	queue_line_full(self, P(self)->tool_progress_message, FALSE, FALSE);
	pump(self);
	return G_SOURCE_REMOVE;
}
static void
tool_started(AiVoiceSession *self)
{
	if (P(self)->progress_used || P(self)->tool_progress_message == NULL ||
		*P(self)->tool_progress_message == '\0' || P(self)->turn_lines != 0)
		return;
	P(self)->progress_used = TRUE;
	P(self)->progress_generation = P(self)->generation;
	P(self)->progress = g_timeout_source_new(P(self)->tool_progress_delay_ms);
	g_source_set_callback(P(self)->progress, progress_fire, self, NULL);
	g_source_attach(P(self)->progress, P(self)->context);
}
static gboolean
terminator(gchar c)
{
	return c == '.' || c == '!' || c == '?';
}
static gsize
closing_len(const gchar *text, gsize len)
{
	if (len >= 1 && (text[0] == '"' || text[0] == '\'' || text[0] == ')' || text[0] == ']'))
		return 1;
	/* U+2019 and U+201D, the curly closing quotes. */
	if (len >= 3 && (memcmp(text, "\342\200\231", 3) == 0 ||
					 memcmp(text, "\342\200\235", 3) == 0))
		return 3;
	return 0;
}
static gboolean
list_number(const gchar *text, gsize dot)
{
	gsize i = 0;
	while (i < dot && g_ascii_isspace(text[i]))
		i++;
	if (i == dot)
		return FALSE;
	for (; i < dot; i++)
		if (!g_ascii_isdigit(text[i]))
			return FALSE;
	return TRUE;
}
/* Byte length of the sentence at the head of @text, or 0 when none is
 * complete yet. Inside the text a terminator ends a sentence only when
 * whitespace follows it, so "3.5" is not cut apart, and a run of terminators
 * and closing quotes stays with its sentence. */
static gsize
sentence_end(const gchar *text, gsize len, gboolean final)
{
	gsize i;
	for (i = 0; i < len; i++) {
		gsize end, n;
		if (text[i] == '\n')
			return i + 1;
		if (!terminator(text[i]))
			continue;
		/* "1." opening a numbered item is not a sentence of its own. */
		if (text[i] == '.' && list_number(text, i))
			continue;
		end = i + 1;
		while (end < len && terminator(text[end]))
			end++;
		while ((n = closing_len(text + end, len - end)) != 0)
			end += n;
		/* A delta ending on "?" or "!" ends a sentence. One ending on "." may
		 * not: the next delta can make it "3.5" or "garden.org". The session
		 * holds that briefly and flushes it if nothing follows. */
		if (end == len) {
			gsize j;
			if (final)
				return end;
			for (j = i; j < end; j++)
				if (text[j] == '.')
					return 0;
			return end;
		}
		if (g_ascii_isspace(text[end]))
			return end;
		i = end - 1;
	}
	return 0;
}
#define PERIOD_HOLD_MS 250
static void
clear_hold(AiVoiceSession *self)
{
	if (P(self)->hold == NULL)
		return;
	g_source_destroy(P(self)->hold);
	g_clear_pointer(&P(self)->hold, g_source_unref);
}
static void
segment(AiVoiceSession *self, gboolean final);
static void
pump(AiVoiceSession *self);
/* Nothing followed the period: it was a sentence end after all. */
static gboolean
hold_fire(gpointer data)
{
	AiVoiceSession *self = data;
	g_clear_pointer(&P(self)->hold, g_source_unref);
	if (P(self)->stopped)
		return G_SOURCE_REMOVE;
	segment(self, TRUE);
	pump(self);
	return G_SOURCE_REMOVE;
}
/* After a streamed delta: arm the hold only while the unspoken text ends on
 * a period, so a turn that stops there is still spoken. */
static void
update_hold(AiVoiceSession *self)
{
	const gchar *end = P(self)->pending_text->str + P(self)->pending_text->len;
	gboolean waiting = FALSE;
	while (end > P(self)->pending_text->str && closing_len(end - 1, 1) != 0)
		end--;
	while (end > P(self)->pending_text->str && terminator(end[-1])) {
		if (end[-1] == '.')
			waiting = TRUE;
		end--;
	}
	if (!waiting) {
		clear_hold(self);
		return;
	}
	if (P(self)->hold != NULL)
		return;
	P(self)->hold = g_timeout_source_new(PERIOD_HOLD_MS);
	g_source_set_callback(P(self)->hold, hold_fire, self, NULL);
	g_source_attach(P(self)->hold, P(self)->context);
}
/* A model that loses the thread can emit the same line for as long as it is
 * allowed to generate, and the caller hears all of it. The third "grep the
 * notes" is not information; it is the reply looping. */
static gchar *
repeat_key(const gchar *text)
{
	g_autofree gchar *spoken = speakable(text);
	GString *key = g_string_new(NULL);
	const gchar *p;
	for (p = spoken; p != NULL && *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		if (g_unichar_isalnum(c))
			g_string_append_unichar(key, g_unichar_tolower(c));
	}
	return g_string_free(key, FALSE);
}
static void
stop_runaway(AiVoiceSession *self)
{
	g_log("ai-glib", G_LOG_LEVEL_INFO,
		  "Reply repeated a line more than %u times; stopping it", P(self)->repeat_limit);
	P(self)->runaway = TRUE;
	g_string_truncate(P(self)->pending_text, 0);
	clear_hold(self);
	clear_progress(self);
	if (P(self)->turn_cancel != NULL)
		g_cancellable_cancel(P(self)->turn_cancel);
	if (P(self)->repeat_message != NULL && *P(self)->repeat_message != '\0')
		queue_line(self, P(self)->repeat_message, FALSE);
}
static void
offer_line(AiVoiceSession *self, const gchar *text)
{
	if (P(self)->runaway)
		return;
	if (P(self)->repeat_limit > 0) {
		g_autofree gchar *key = repeat_key(text);
		if (*key != '\0') {
			guint seen = GPOINTER_TO_UINT(g_hash_table_lookup(P(self)->repeats, key)) + 1;
			g_hash_table_insert(P(self)->repeats, g_steal_pointer(&key), GUINT_TO_POINTER(seen));
			if (seen > P(self)->repeat_limit) {
				stop_runaway(self);
				return;
			}
		}
	}
	queue_line(self, text, TRUE);
}
static void
segment(AiVoiceSession *self, gboolean final)
{
	gsize end;
	while (!P(self)->runaway && (end = sentence_end(P(self)->pending_text->str,
												 P(self)->pending_text->len, final)) != 0) {
		g_autofree gchar *text = g_strndup(P(self)->pending_text->str, end);
		g_string_erase(P(self)->pending_text, 0, end);
		offer_line(self, text);
	}
	if (final && P(self)->pending_text->len != 0) {
		g_autofree gchar *text = g_strdup(P(self)->pending_text->str);
		g_string_truncate(P(self)->pending_text, 0);
		offer_line(self, text);
	}
}
/* An inline span with no whitespace is a name — "v81-roadmap.org" — and the
 * sentence around it means nothing without it. One with whitespace is a
 * command, and read aloud it is noise. */
static void
close_span(AiVoiceSession *self)
{
	const gchar *p;
	for (p = P(self)->span->str; *p != '\0'; p++)
		if (g_ascii_isspace(*p))
			break;
	if (*p == '\0')
		g_string_append_len(P(self)->pending_text, P(self)->span->str, P(self)->span->len);
	g_string_truncate(P(self)->span, 0);
	P(self)->in_inline = FALSE;
}
/* speak-code off: markdown code is what a model writes when it shows a command
 * instead of running it, and read aloud it is noise. Fenced blocks and
 * multi-word inline spans are left out; the prose around them is kept. A run
 * of backticks at the end of a delta is carried, since "``" and "```" mean
 * different things. */
static void
append_reply(AiVoiceSession *self, const gchar *text, gboolean final)
{
	const gchar *p;
	if (P(self)->speak_code) {
		g_string_append(P(self)->pending_text, text);
		return;
	}
	for (p = text; *p != '\0'; p++) {
		if (*p == '`') {
			P(self)->ticks++;
			continue;
		}
		if (P(self)->ticks >= 3) {
			P(self)->in_fence = !P(self)->in_fence;
			P(self)->in_inline = FALSE;
			g_string_truncate(P(self)->span, 0);
			g_string_append_c(P(self)->pending_text, '\n');
		} else if (P(self)->ticks > 0 && !P(self)->in_fence) {
			if (P(self)->in_inline)
				close_span(self);
			else
				P(self)->in_inline = TRUE;
		}
		P(self)->ticks = 0;
		if (P(self)->in_fence)
			continue;
		if (P(self)->in_inline) {
			/* An unclosed span ends with its line rather than eating the reply. */
			if (*p == '\n') {
				close_span(self);
				g_string_append_c(P(self)->pending_text, '\n');
			} else
				g_string_append_c(P(self)->span, *p);
			continue;
		}
		g_string_append_c(P(self)->pending_text, *p);
	}
	if (final) {
		if (P(self)->in_inline)
			close_span(self);
		P(self)->ticks = 0;
	}
}
static void
speech_complete(Speech *s)
{
	AiVoiceSession *self = s->session;
	if (!s->synthesized || s->pending != 0)
		return;
	P(self)->speech = NULL;
	/* Only speech that reached the caller was said; cut short is still said. */
	if (s->heard)
		g_signal_emit_by_name(self, "spoken", s->text, !s->discarded);
	if (!s->discarded && s->generation == P(self)->generation && s->remember) {
		if (P(self)->spoken->len != 0)
			g_string_append_c(P(self)->spoken, ' ');
		g_string_append(P(self)->spoken, s->text);
		if (P(self)->worker != NULL)
			ai_voice_worker_spoken(P(self)->worker, P(self)->spoken->str,
								   P(self)->provider_generation);
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
	if (!ai_audio_transport_write_pcm_finish(AI_AUDIO_TRANSPORT(source), result,
											 &error)) {
		s->discarded = TRUE;
		report(s->session, error);
	} else
		s->heard = TRUE;
	s->pending--;
	s->queued -= p->size;
	g_free(p);
	speech_complete(s);
}
static void
audio_out(AiSpeechSynthesizer *synthesizer, GBytes *pcm, guint sample_rate, gpointer data)
{
	AiVoiceSession *self = data;
	Speech *s = P(self)->speech;
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
	ai_audio_transport_write_pcm_async(P(self)->transport, pcm, sample_rate, s->cancel,
									   played, p);
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
		if (!s->fallback && !P(s->session)->stopped &&
			s->generation == P(s->session)->generation &&
			!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			/* A failed notice is not a barge-in: do not retry it indefinitely. */
			s->notice = FALSE;
			interrupt_turn(s->session);
			if (P(s->session)->synthesis_error_message != NULL &&
			    *P(s->session)->synthesis_error_message != '\0') {
				Line *fallback = g_new0(Line, 1);
				fallback->text = g_strdup(P(s->session)->synthesis_error_message);
				fallback->fallback = TRUE;
				g_queue_push_head(&P(s->session)->lines, fallback);
			}
		}
	}
	s->synthesized = TRUE;
	speech_complete(s);
}
static gboolean
deadline(gpointer data)
{
	AiVoiceSession *self = data;
	g_clear_pointer(&P(self)->deadline, g_source_unref);
	interrupt_turn(self);
	queue_line(self, P(self)->deadline_message, FALSE);
	pump(self);
	return G_SOURCE_REMOVE;
}
static void
start_turn(AiVoiceSession *self, const gchar *text)
{
	P(self)->last_turn_us = g_get_monotonic_time();
	P(self)->generation++;
	P(self)->provider_generation = P(self)->generation;
	P(self)->provider_pending = TRUE;
	g_clear_object(&P(self)->turn_cancel);
	P(self)->turn_cancel = g_cancellable_new();
	g_string_truncate(P(self)->spoken, 0);
	P(self)->turn_lines = 0;
	P(self)->progress_used = FALSE;
	P(self)->in_fence = P(self)->in_inline = P(self)->runaway = FALSE;
	P(self)->ticks = 0;
	g_string_truncate(P(self)->span, 0);
	g_hash_table_remove_all(P(self)->repeats);
	clear_progress(self);
	clear_hold(self);
	state(self, AI_VOICE_THINKING);
	P(self)->deadline = g_timeout_source_new(P(self)->deadline_ms);
	g_source_set_callback(P(self)->deadline, deadline, self, NULL);
	g_source_attach(P(self)->deadline, P(self)->context);
	ai_voice_worker_send(P(self)->worker, text, P(self)->turn_cancel, P(self)->generation);
}
static gboolean
recognizing(AiVoiceSession *self)
{
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, P(self)->participants);
	while (g_hash_table_iter_next(&iter, NULL, &value))
		if (((Participant *)value)->recognizing)
			return TRUE;
	return FALSE;
}
static void
pump(AiVoiceSession *self)
{
	Line *line;
	if (P(self)->stopped || P(self)->media_recovering || P(self)->speech != NULL)
		return;
	line = g_queue_pop_head(&P(self)->lines);
	if (line == NULL && !P(self)->provider_pending && !recognizing(self) &&
		g_queue_is_empty(&P(self)->turns))
		line = g_queue_pop_head(&P(self)->pending_notices);
	if (line != NULL) {
		Speech *s = g_new0(Speech, 1);
		s->session = g_object_ref(self);
		s->text = g_steal_pointer(&line->text);
		s->remember = line->remember;
		s->notice = line->notice;
		s->fallback = line->fallback;
		s->cancel = g_cancellable_new();
		s->generation = P(self)->generation;
		P(self)->speech = s;
		line_free(line);
		state(self, AI_VOICE_SPEAKING);
		remember_said(self, s->text);
		g_signal_emit_by_name(self, "reply", s->text);
		if (s->fallback && P(self)->fallback_pcm != NULL) {
			s->synthesized = TRUE;
			audio_out(P(self)->synthesizer, P(self)->fallback_pcm, P(self)->fallback_sample_rate,
					  self);
			speech_complete(s);
		} else {
			ai_speech_synthesizer_synthesize_async(P(self)->synthesizer, s->text, s->cancel,
												   synthesized, s);
		}
		return;
	}
	if (!P(self)->provider_pending) {
		g_autofree gchar *turn = g_queue_pop_head(&P(self)->turns);
		clear_deadline(self);
		if (turn != NULL)
			start_turn(self, turn);
		else
			state(self, recognizing(self) ? AI_VOICE_TRANSCRIBING : AI_VOICE_LISTENING);
	} else if (P(self)->generation != P(self)->provider_generation)
		state(self, recognizing(self) ? AI_VOICE_TRANSCRIBING : AI_VOICE_LISTENING);
	else
		state(self, AI_VOICE_THINKING);
}
static void
interrupt_turn(AiVoiceSession *self)
{
	clear_deadline(self);
	clear_progress(self);
	clear_hold(self);
	P(self)->paused_deadline_us = 0;
	if (P(self)->turn_cancel != NULL)
		g_cancellable_cancel(P(self)->turn_cancel);
	if (P(self)->speech != NULL) {
		if (!P(self)->stopped && P(self)->speech->notice && !P(self)->speech->discarded) {
			Line *notice = g_new0(Line, 1);
			notice->text = g_strdup(P(self)->speech->text);
			notice->notice = TRUE;
			g_queue_push_head(&P(self)->pending_notices, notice);
		}
		P(self)->speech->discarded = TRUE;
		g_cancellable_cancel(P(self)->speech->cancel);
	}
	if (P(self)->transport != NULL)
		ai_audio_transport_flush(P(self)->transport);
	g_queue_clear_full(&P(self)->lines, line_free);
	g_string_truncate(P(self)->pending_text, 0);
	P(self)->generation++;
}
/* A tool that ran has run, even in a turn since interrupted: report it for
 * any generation. */
static void
report_tool(AiVoiceSession *self, AiEvent *event)
{
	AiToolUse *use = ai_event_get_tool_use(event);
	AiToolResult *result = ai_event_get_tool_result(event);
	JsonNode *input = use != NULL ? ai_tool_use_get_input(use) : NULL;
	g_autofree gchar *arguments = input != NULL ? json_to_string(input, FALSE) : NULL;
	const gchar *name = use != NULL ? ai_tool_use_get_name(use) : NULL;
	const gchar *content = result != NULL ? ai_tool_result_get_content(result) : NULL;
	if (result == NULL)
		return;
	g_signal_emit_by_name(self, "tool", name != NULL ? name : "",
						  arguments != NULL ? arguments : "{}",
						  content != NULL ? content : "",
						  ai_tool_result_get_is_error(result));
}
static void
worker_mail(GObject *object, AiVoiceMailKind kind, guint64 generation, AiEvent *event,
			const gchar *text, const GError *error)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	if (P(self)->stopped)
		return;
	if (kind == AI_VOICE_MAIL_EVENT && event != NULL &&
		ai_event_get_kind(event) == AI_EVENT_TOOL_FINISHED)
		report_tool(self, event);
	if (kind == AI_VOICE_MAIL_AGENT) {
		/* A finished agent becomes a turn for the model, queued behind any turn
		 * under way, so it fetches the result and says it unprompted. */
		if (!g_hash_table_contains(P(self)->notices, text)) {
			g_hash_table_add(P(self)->notices, g_strdup(text));
			g_queue_push_tail(&P(self)->turns, g_strdup(text));
		}
	} else if (kind == AI_VOICE_MAIL_DONE) {
		if (generation == P(self)->provider_generation) {
			clear_progress(self);
			clear_hold(self);
			P(self)->provider_pending = FALSE;
			clear_deadline(self);
			P(self)->paused_deadline_us = 0;
		}
		if (generation == P(self)->generation && P(self)->runaway) {
			/* Stopped on purpose; the cancellation is not an error to speak. */
		} else if (generation == P(self)->generation) {
			if (error != NULL) {
				g_autofree gchar *line =
					g_strdup_printf("There was an error: %s", error->message);
				report(self, error);
				queue_line(self, line, FALSE);
			} else {
				/* Streamed replies arrive with no text here, but the end
				 * still has to close a span or a backtick left open. */
				append_reply(self, text != NULL ? text : "", TRUE);
				segment(self, TRUE);
				/* No text, no tool error, nothing pronounceable: a model that
				 * returns an empty turn, or a tool call a provider failed to
				 * read, would otherwise end in silence the caller cannot tell
				 * from a dropped call. */
				if (P(self)->turn_lines == 0) {
					g_log("ai-glib", G_LOG_LEVEL_INFO,
						  "Provider turn ended with nothing to say; speaking the "
						  "empty-reply message");
					queue_line(self, P(self)->empty_reply_message, FALSE);
				}
			}
		}
	} else if (generation == P(self)->generation) {
		AiEventKind k = ai_event_get_kind(event);
		if (k == AI_EVENT_TOOL_STARTED) {
			/* A tool that never finishes never reaches the transcript: log its
			 * start, once per call, so a stuck turn names what it is stuck in. */
			AiToolUse *use = ai_event_get_tool_use(event);
			const gchar *id = use != NULL ? ai_tool_use_get_id(use) : NULL;
			if (use != NULL && g_strcmp0(id, P(self)->logged_tool) != 0) {
				g_free(P(self)->logged_tool);
				P(self)->logged_tool = g_strdup(id);
				g_log("ai-glib", G_LOG_LEVEL_INFO, "Tool started: %s",
					  ai_tool_use_get_name(use) != NULL ? ai_tool_use_get_name(use) : "?");
			}
			tool_started(self);
		}
		else if (k == AI_EVENT_TEXT_DELTA) {
			if (P(self)->runaway)
				;
			else if (P(self)->pending_text->len < 65536)
				append_reply(self, ai_event_get_text(event), FALSE);
			segment(self, FALSE);
			update_hold(self);
		} else if (k == AI_EVENT_TOOL_FINISHED) {
			AiToolResult *result = ai_event_get_tool_result(event);
			AiToolUse *use = ai_event_get_tool_use(event);
			const gchar *tool = use != NULL ? ai_tool_use_get_name(use) : NULL;
			/* Checking on an agent too early ("still running") is the model's
			 * business; it is never read to the caller. */
			if (result != NULL && ai_tool_result_get_is_error(result) &&
				g_strcmp0(tool, "agent_result") != 0 && g_strcmp0(tool, "agent_status") != 0) {
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
	if (P(self)->stopped || g_hash_table_contains(P(self)->participants, speaker))
		return;
	p = g_new0(Participant, 1);
	p->name = g_strdup(name != NULL && *name != '\0' ? name : speaker);
	p->frames = g_byte_array_new();
	p->onset = g_byte_array_new();
	p->preroll = g_byte_array_new();
	p->utterance = g_byte_array_new();
	g_hash_table_insert(P(self)->participants, g_strdup(speaker), p);
}
static void
left(AiAudioTransport *transport, const gchar *speaker, gpointer data)
{
	AiVoiceSession *self = data;
	ai_speech_recognizer_cancel(P(self)->recognizer, speaker);
	ai_voice_activity_reset(P(self)->activity, speaker);
	g_hash_table_remove(P(self)->participants, speaker);
	pump(self);
}
static void
audio_in(AiAudioTransport *transport, const gchar *speaker, GBytes *pcm, gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p = g_hash_table_lookup(P(self)->participants, speaker);
	gsize n;
	const guint8 *raw = g_bytes_get_data(pcm, &n);
	if (P(self)->stopped || p == NULL || n % 2 != 0 || n > 32000)
		return;
	g_byte_array_append(p->frames, raw, n);
	while (p->frames->len >= 320) {
		g_autoptr(GBytes) frame = g_bytes_new(p->frames->data, 320);
		g_autoptr(GError) error = NULL;
		gint activity;
		gboolean fed_onset = FALSE;
		g_byte_array_remove_range(p->frames, 0, 320);
		activity = ai_voice_activity_process(P(self)->activity, speaker, frame, &error);
		if (activity < 0) {
			p->speech_ms = 0;
			g_byte_array_set_size(p->onset, 0);
			report(self, error);
			return;
		}
		if (activity & AI_VOICE_ACTIVITY_SPEECH) {
			p->speech_ms = MIN(p->speech_ms + 10, 5000);
			if (!p->recognizing || p->ended) {
				gsize size;
				const guint8 *samples = g_bytes_get_data(frame, &size);
				if (p->onset->len == 0)
					g_byte_array_append(p->onset, p->preroll->data, p->preroll->len);
				g_byte_array_append(p->onset, samples, size);
			}
			if (p->speech_ms >= P(self)->barge_in_ms) {
				/* While we are talking, sound alone may be our own voice
				 * returning through the caller's speaker. Listen, and let
				 * words decide whether this is an interruption. */
				/* Muted, the caller is not heard, so cannot interrupt: only
				 * the words "unmute" or "hang up" reach us, as commands. */
				if (P(self)->muted) {
				} else if (P(self)->state == AI_VOICE_SPEAKING && P(self)->barge_in_confirm) {
					if (!p->pending_barge)
						g_log("ai-glib", G_LOG_LEVEL_INFO,
							  "possible barge-in by %s after %u ms of speech, waiting for words",
							  p->name, p->speech_ms);
					p->pending_barge = TRUE;
				} else if (P(self)->state == AI_VOICE_SPEAKING ||
						   P(self)->state == AI_VOICE_THINKING) {
					g_log("ai-glib", G_LOG_LEVEL_INFO,
						  "barge-in by %s after %u ms of speech, cancelling %s", p->name,
						  p->speech_ms,
						  P(self)->state == AI_VOICE_SPEAKING ? "speaking" : "thinking");
					interrupt_turn(self);
				}
				if (p->ended) {
					ai_speech_recognizer_cancel(P(self)->recognizer, speaker);
					p->recognizing = FALSE;
					p->ended = FALSE;
				}
				if (!p->recognizing) {
					guint offset;
					gboolean recognizing;
					g_autoptr(GBytes) onset = g_bytes_new(p->onset->data, p->onset->len);
					gsize size;
					const guint8 *samples = g_bytes_get_data(onset, &size);
					g_byte_array_set_size(p->onset, 0);
					g_byte_array_set_size(p->utterance, 0);
					g_byte_array_append(p->utterance, samples, size);
					recognizing =
						ai_speech_recognizer_begin(P(self)->recognizer, speaker, &error);
					if (P(self)->stopped ||
						g_hash_table_lookup(P(self)->participants, speaker) != p)
						return;
					p->recognizing = recognizing;
					/* Preserve the confirmed onset rather than clipping initial words.
					 * Feed ordinary 10 ms frames, independent of backend size limits. */
					for (offset = 0; p->recognizing && error == NULL && offset < size;
						 offset += 320) {
						g_autoptr(GBytes) buffered = g_bytes_new(samples + offset, 320);
						ai_speech_recognizer_feed(P(self)->recognizer, speaker, buffered,
												  &error);
						if (P(self)->stopped ||
							g_hash_table_lookup(P(self)->participants, speaker) != p)
							return;
					}
					fed_onset = TRUE;
				}
				if (!p->pending_barge)
					state(self, AI_VOICE_TRANSCRIBING);
				if (P(self)->stopped ||
					g_hash_table_lookup(P(self)->participants, speaker) != p)
					return;
			}
		} else {
			p->speech_ms = 0;
			g_byte_array_set_size(p->onset, 0);
		}
		g_byte_array_append(p->preroll, g_bytes_get_data(frame, NULL), 320);
		if (p->preroll->len > PREROLL_BYTES)
			g_byte_array_remove_range(p->preroll, 0, p->preroll->len - PREROLL_BYTES);
		if (p->recognizing && !p->ended && error == NULL && !fed_onset) {
			if (p->utterance->len < UTTERANCE_BYTES)
				g_byte_array_append(p->utterance, g_bytes_get_data(frame, NULL), 320);
			ai_speech_recognizer_feed(P(self)->recognizer, speaker, frame, &error);
		}
		if (P(self)->stopped || g_hash_table_lookup(P(self)->participants, speaker) != p)
			return;
		if (error != NULL) {
			ai_speech_recognizer_cancel(P(self)->recognizer, speaker);
			stt_error(P(self)->recognizer, speaker, error, self);
			return;
		}
		if ((activity & AI_VOICE_ACTIVITY_END) && p->recognizing && !p->ended) {
			p->ended = TRUE;
			ai_speech_recognizer_end(P(self)->recognizer, speaker);
			if (P(self)->stopped || g_hash_table_lookup(P(self)->participants, speaker) != p)
				return;
		}
	}
}
/* Spoken controls. A command is the whole utterance — "stop listening", or
 * "Ada, stop listening, please" — never a phrase inside a sentence, so "I want
 * to stop listening to that podcast" is still something to answer. The table
 * is the registration: a new command is one row and one case in
 * run_command(). */
typedef struct {
	const gchar *name;
	const gchar *const *phrases;
} VoiceCommand;
static const gchar *const stop_phrases[] = {
	"stop", "stop talking", "stop it", "shush", "hush", "shut up", "be quiet", "quiet",
	"enough", "that's enough", "cancel", "cancel that", "never mind", "nevermind", NULL};
static const gchar *const mute_phrases[] = {
	"mute", "mute yourself", "stop listening", "don't listen", "go to sleep",
	"pause listening", NULL};
static const gchar *const unmute_phrases[] = {
	"unmute", "start listening", "wake up", "resume listening", "you can listen",
	"i'm back", "listen", NULL};
static const gchar *const hangup_phrases[] = {"hang up", "end call", "end the call", NULL};
static const VoiceCommand voice_command_table[] = {{"stop", stop_phrases},
												   {"mute", mute_phrases},
												   {"unmute", unmute_phrases},
												   {"hangup", hangup_phrases}};
/* Politeness around a command is still the command: "go ahead and hang up". */
static const gchar *const lead_words[] = {"hey",	  "hi",		"ok",		  "okay",
										  "yo",		  "please", "just",		  "go ahead and",
										  "can you", "could you", "would you", NULL};
static const gchar *const tail_words[] = {"please", "now", "thanks", "for me", NULL};
/* Lower-case words separated by single spaces; a curly apostrophe is a straight
 * one, anything else that is not a letter or digit separates words. */
static gchar *
command_words(const gchar *text)
{
	GString *out = g_string_new(NULL);
	const gchar *p;
	for (p = text; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		if (c == 0x2019)
			c = '\'';
		if (g_unichar_isalnum(c) || c == '\'')
			g_string_append_unichar(out, g_unichar_tolower(c));
		else if (out->len > 0 && out->str[out->len - 1] != ' ')
			g_string_append_c(out, ' ');
	}
	while (out->len > 0 && out->str[out->len - 1] == ' ')
		g_string_truncate(out, out->len - 1);
	return g_string_free(out, FALSE);
}
static gboolean
strip_lead(gchar **words, const gchar *lead)
{
	gsize n = strlen(lead);
	if (n == 0 || strncmp(*words, lead, n) != 0 || ((*words)[n] != ' ' && (*words)[n] != '\0'))
		return FALSE;
	memmove(*words, *words + n + ((*words)[n] == ' '), strlen(*words + n) + 1 - ((*words)[n] == ' '));
	return TRUE;
}
static gboolean
strip_tail(gchar *words, const gchar *tail)
{
	gsize n = strlen(words), t = strlen(tail);
	if (t == 0 || n <= t || words[n - t - 1] != ' ' || strcmp(words + n - t, tail) != 0)
		return FALSE;
	words[n - t - 1] = '\0';
	return TRUE;
}
static const gchar *
match_command(AiVoiceSession *self, const gchar *text)
{
	g_autofree gchar *words = command_words(text);
	gboolean again = TRUE;
	guint i, j;
	while (again) {
		again = FALSE;
		for (i = 0; lead_words[i] != NULL; i++)
			again |= strip_lead(&words, lead_words[i]);
		for (i = 0; P(self)->assistant_names != NULL && P(self)->assistant_names[i] != NULL; i++) {
			g_autofree gchar *name = command_words(P(self)->assistant_names[i]);
			again |= strip_lead(&words, name);
		}
		for (i = 0; tail_words[i] != NULL; i++)
			again |= strip_tail(words, tail_words[i]);
	}
	for (i = 0; i < G_N_ELEMENTS(voice_command_table); i++)
		for (j = 0; voice_command_table[i].phrases[j] != NULL; j++)
			if (strcmp(words, voice_command_table[i].phrases[j]) == 0)
				return voice_command_table[i].name;
	return NULL;
}
static void
cancel_scoring(AiVoiceSession *self);
static void
drop_pending_namings(AiVoiceSession *self);
static void
set_muted(AiVoiceSession *self, gboolean muted)
{
	if (P(self)->muted == muted)
		return;
	P(self)->muted = muted;
	/* A score on its way was asked before the mute; it must not chime. */
	if (muted)
		cancel_scoring(self);
	g_object_notify(G_OBJECT(self), "muted");
}
static void
run_command(AiVoiceSession *self, const gchar *name)
{
	g_log("ai-glib", G_LOG_LEVEL_INFO, "Voice command: %s", name);
	g_signal_emit_by_name(self, "command", name);
	if (P(self)->stopped)
		return;
	if (g_str_equal(name, "stop") || g_str_equal(name, "mute")) {
		/* A cut-off notice goes back in the queue after a barge-in, so the
		 * caller's turn goes first. After "stop" there is no turn: it would
		 * be replayed at once. */
		if (P(self)->speech != NULL)
			P(self)->speech->notice = FALSE;
		interrupt_turn(self);
		g_queue_clear_full(&P(self)->turns, g_free);
		/* Finals said before the command and still being named must not
		 * become turns after it: "stop" would be followed by an answer. */
		drop_pending_namings(self);
	}
	if (g_str_equal(name, "mute")) {
		set_muted(self, TRUE);
		if (P(self)->mute_message != NULL && *P(self)->mute_message != '\0')
			queue_line(self, P(self)->mute_message, FALSE);
	} else if (g_str_equal(name, "unmute")) {
		set_muted(self, FALSE);
		if (P(self)->unmute_message != NULL && *P(self)->unmute_message != '\0')
			queue_line(self, P(self)->unmute_message, FALSE);
	}
}
/* ---- giving a guest a lasting name, by voice ----
 *
 * "Ada, guest two is Kim", "call guest 2 Kim", "guest number two's name is
 * Kim". The identifier saves the guest's voice as Kim's, but only as a new
 * voiceprint: a name already enrolled is refused, so nobody can take over
 * someone else's voice by saying so. An application may veto any promotion
 * through AiVoiceSession::allow-promotion. */
static const gchar *const number_words[] = {"zero",	   "one",	  "two",	 "three",
											"four",	   "five",	  "six",	 "seven",
											"eight",   "nine",	  "ten",	 "eleven",
											"twelve",  "thirteen", "fourteen", "fifteen",
											"sixteen", "seventeen", "eighteen", "nineteen",
											"twenty",  NULL};
/* "two" or "2" -> 2; 0 when WORD is not a number up to 99. */
static guint
number_of(const gchar *word)
{
	guint i;
	if (g_ascii_isdigit(*word)) {
		guint64 n = g_ascii_strtoull(word, NULL, 10);
		return n <= 99 ? (guint)n : 0;
	}
	for (i = 1; number_words[i] != NULL; i++)
		if (g_str_equal(word, number_words[i]))
			return i;
	/* "to" and "too" are how recognition often writes "two". */
	return g_str_equal(word, "to") || g_str_equal(word, "too") ? 2 : 0;
}
/* Words of a name, each capitalized: "kim lee" -> "Kim Lee". */
static gchar *
title_case(gchar **words)
{
	GString *out = g_string_new(NULL);
	guint i;
	for (i = 0; words[i] != NULL; i++) {
		const gchar *w = words[i];
		if (*w == '\0')
			continue;
		if (out->len > 0)
			g_string_append_c(out, ' ');
		g_string_append_unichar(out, g_unichar_totitle(g_utf8_get_char(w)));
		g_string_append(out, g_utf8_next_char(w));
	}
	return g_string_free(out, FALSE);
}
static gboolean
match_promotion(AiVoiceSession *self, const gchar *text, gchar **guest, gchar **name)
{
	g_autofree gchar *words = command_words(text);
	g_auto(GStrv) parts = NULL;
	gboolean again = TRUE;
	guint i, n = 0, at, number;
	while (again) {
		again = FALSE;
		for (i = 0; lead_words[i] != NULL; i++)
			again |= strip_lead(&words, lead_words[i]);
		for (i = 0; P(self)->assistant_names != NULL && P(self)->assistant_names[i] != NULL; i++) {
			g_autofree gchar *assistant = command_words(P(self)->assistant_names[i]);
			again |= strip_lead(&words, assistant);
		}
		for (i = 0; tail_words[i] != NULL; i++)
			again |= strip_tail(words, tail_words[i]);
	}
	parts = g_strsplit(words, " ", -1);
	n = g_strv_length(parts);
	at = 0;
	if (at < n && (g_str_equal(parts[at], "call") || g_str_equal(parts[at], "name")))
		at++;
	if (at >= n || !g_str_equal(parts[at], "guest"))
		return FALSE;
	at++;
	if (at < n && g_str_equal(parts[at], "number"))
		at++;
	if (at >= n)
		return FALSE;
	/* "guest two's name is", "guest two is", "guest 2 Kim" after "call". */
	if (g_str_has_suffix(parts[at], "'s")) {
		parts[at][strlen(parts[at]) - 2] = '\0';
		if ((number = number_of(parts[at])) == 0)
			return FALSE;
		at++;
		if (at < n && g_str_equal(parts[at], "name"))
			at++;
	} else if ((number = number_of(parts[at++])) == 0)
		return FALSE;
	if (at < n && (g_str_equal(parts[at], "is") || g_str_equal(parts[at], "as")))
		at++;
	/* A name of one to three words, and nothing that reads as a sentence. */
	if (at >= n || n - at > 3)
		return FALSE;
	for (i = at; i < n; i++)
		if (g_str_equal(parts[i], "not") || g_str_equal(parts[i], "the") ||
			g_str_equal(parts[i], "a") || g_str_equal(parts[i], "here") ||
			g_str_equal(parts[i], "gone") || g_str_equal(parts[i], "back"))
			return FALSE;
	*guest = g_strdup_printf("Guest %u", number);
	*name = title_case(parts + at);
	if (ai_voiceprint_name_is_valid(*name))
		return TRUE;
	g_clear_pointer(guest, g_free);
	g_clear_pointer(name, g_free);
	return FALSE;
}
/* TEMPLATE with {name}, {guest} and {reason} replaced. */
static gchar *
fill_template(const gchar *template, const gchar *name, const gchar *guest,
			  const gchar *reason)
{
	GString *out = g_string_new(template);
	g_string_replace(out, "{name}", name, 0);
	g_string_replace(out, "{guest}", guest, 0);
	g_string_replace(out, "{reason}", reason != NULL ? reason : "", 0);
	return g_string_free(out, FALSE);
}
static void
run_promotion(AiVoiceSession *self, const gchar *guest, const gchar *name)
{
	g_autoptr(GError) error = NULL;
	gboolean allowed = TRUE;
	GHashTableIter iter;
	gpointer value;
	g_log("ai-glib", G_LOG_LEVEL_INFO, "Voice command: promote %s to %s", guest, name);
	g_signal_emit_by_name(self, "command", "promote");
	g_signal_emit_by_name(self, "allow-promotion", guest, name, &allowed);
	if (!allowed)
		error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
									"that is not allowed here");
	else if (!ai_speaker_identifier_promote(AI_SPEAKER_IDENTIFIER(P(self)->identifier), guest, name,
											FALSE, &error)) {
		/* Spoken to the room, so said plainly. */
		const gchar *why = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_EXISTS)
							   ? "someone by that name is already enrolled"
						   : g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
							   ? "I haven't heard that guest"
							   : error->message;
		g_autofree gchar *reason = g_strdup(why);
		g_clear_error(&error);
		error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, reason);
	}
	if (error != NULL) {
		g_autofree gchar *line = NULL;
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Not promoting %s to %s: %s", guest, name,
			  error->message);
		if (P(self)->promotion_refused_message != NULL && *P(self)->promotion_refused_message != '\0') {
			line = fill_template(P(self)->promotion_refused_message, name, guest, error->message);
			queue_line(self, line, FALSE);
		}
		return;
	}
	/* From now on that voice is NAME: in the room's memory, and for whoever
	 * a short utterance inherits its name from. */
	ai_voice_room_rename_speaker(P(self)->room, guest, name);
	g_hash_table_iter_init(&iter, P(self)->participants);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		Participant *p = value;
		if (g_strcmp0(p->last_named, guest) == 0) {
			g_free(p->last_named);
			p->last_named = g_strdup(name);
		}
	}
	g_signal_emit_by_name(self, "guest-promoted", guest, name);
	if (P(self)->promoted_message != NULL && *P(self)->promoted_message != '\0') {
		g_autofree gchar *line = fill_template(P(self)->promoted_message, name, guest, NULL);
		queue_line(self, line, FALSE);
	}
}

/* ---- room mode ----
 *
 * With several people in a room, most of what is said is not for the
 * assistant. The AiVoiceRoom keeps it, and hands it to the model with the
 * next turn so a reply knows what was said; a turn happens when the address
 * detector says an utterance is for the assistant, or when the relevance
 * judge scores what was said at chime-in-threshold or above.
 *
 * One score is in flight at a time. Talk heard while it is on its way does
 * not cancel it -- in a busy room that would mean no score ever finished --
 * but marks it stale: when it comes back, the latest talk is scored instead. */
static AiAddressDetector *
detector_of(AiVoiceSession *self)
{
	return P(self)->detector != NULL ? P(self)->detector : AI_ADDRESS_DETECTOR(P(self)->name_detector);
}
static gboolean
addressed(AiVoiceSession *self, const gchar *speaker, const gchar *text)
{
	return ai_address_detector_is_addressed(detector_of(self), speaker, text);
}
static void
cancel_scoring(AiVoiceSession *self)
{
	if (P(self)->scoring != NULL)
		g_cancellable_cancel(P(self)->scoring);
	g_clear_object(&P(self)->scoring);
	if (P(self)->debounce != NULL) {
		g_source_destroy(P(self)->debounce);
		g_clear_pointer(&P(self)->debounce, g_source_unref);
	}
	P(self)->rescore = FALSE;
}
typedef struct {
	AiVoiceSession *session;
	GCancellable *cancel;
	guint64 revision;
} Scoring;
static void
scoring_free(Scoring *sc)
{
	g_object_unref(sc->session);
	g_object_unref(sc->cancel);
	g_free(sc);
}
static gboolean
recognizing(AiVoiceSession *self);
static void
maybe_score(AiVoiceSession *self);
static gboolean
can_score(AiVoiceSession *self)
{
	return P(self)->judge != NULL && P(self)->chime_threshold > 0 && !P(self)->stopped && !P(self)->muted &&
		   ai_voice_room_get_n_lines(P(self)->room) > 0 &&
		   (P(self)->last_turn_us == 0 ||
			g_get_monotonic_time() - P(self)->last_turn_us >= (gint64)P(self)->chime_cooldown_ms * 1000);
}
static void
scored(GObject *source, GAsyncResult *result, gpointer data)
{
	Scoring *sc = data;
	AiVoiceSession *self = sc->session;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *reason = NULL;
	gint score;
	gboolean chime;
	score = ai_relevance_judge_judge_finish(AI_RELEVANCE_JUDGE(source), result, &reason, &error);
	if (P(self)->scoring == sc->cancel)
		g_clear_object(&P(self)->scoring);
	if (P(self)->stopped || g_cancellable_is_cancelled(sc->cancel)) {
		scoring_free(sc);
		return;
	}
	if (score < 0) {
		g_debug("voice: relevance not scored: %s",
				error != NULL ? error->message : "the judge returned nothing");
		scoring_free(sc);
		return;
	}
	/* Stale: someone spoke since. Score what is there now instead. */
	if (sc->revision != ai_voice_room_get_revision(P(self)->room)) {
		P(self)->rescore = FALSE;
		maybe_score(self);
		scoring_free(sc);
		return;
	}
	/* Never over anyone -- not even someone whose words are still being
	 * named -- never while muted, and never on top of a turn under way. */
	chime = (guint)score >= P(self)->chime_threshold && !P(self)->provider_pending && !P(self)->muted &&
			P(self)->speech == NULL && g_queue_is_empty(&P(self)->turns) &&
			g_queue_is_empty(&P(self)->naming) && !recognizing(self) &&
			ai_voice_room_get_n_lines(P(self)->room) > 0;
	g_log("ai-glib", G_LOG_LEVEL_INFO, "Relevance %d/10 (threshold %u)%s: %s", score,
		  P(self)->chime_threshold, chime ? ", chiming in" : "", reason);
	g_signal_emit_by_name(self, "relevance", (guint)score, reason, chime);
	if (chime && !P(self)->stopped) {
		g_autofree gchar *note = g_strdup_printf(
			"[Note]: Nobody addressed you. You are chiming in because: %s. Say it "
			"in one or two sentences, to whoever it is for.",
			*reason != '\0' ? reason : "it seemed useful");
		g_queue_push_tail(&P(self)->turns, ai_voice_room_take_turn(P(self)->room, NULL, note));
		pump(self);
	}
	scoring_free(sc);
}
/* Ask the judge about what was said since the assistant last spoke. */
static void
start_score(AiVoiceSession *self)
{
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_free);
	g_auto(GStrv) heard = NULL;
	Scoring *sc;
	GList *l;
	guint i;
	if (!can_score(self))
		return;
	if (P(self)->scoring != NULL) {
		P(self)->rescore = TRUE;
		return;
	}
	/* What the assistant said last, so a follow-up to it reads as one. */
	for (l = P(self)->said.head; l != NULL; l = l->next) {
		const Said *said = l->data;
		if (said->at >= P(self)->last_turn_us &&
			g_get_monotonic_time() - said->at < 180 * G_USEC_PER_SEC)
			g_ptr_array_add(lines, g_strdup_printf("[Assistant]: %s", said->text));
	}
	heard = ai_voice_room_get_lines(P(self)->room);
	for (i = 0; heard[i] != NULL; i++)
		g_ptr_array_add(lines, g_strdup(heard[i]));
	g_ptr_array_add(lines, NULL);
	sc = g_new0(Scoring, 1);
	sc->session = g_object_ref(self);
	sc->cancel = g_cancellable_new();
	sc->revision = ai_voice_room_get_revision(P(self)->room);
	P(self)->scoring = g_object_ref(sc->cancel);
	P(self)->rescore = FALSE;
	ai_relevance_judge_judge_async(P(self)->judge,
								   P(self)->assistant_names != NULL &&
										   P(self)->assistant_names[0] != NULL
									   ? P(self)->assistant_names[0]
									   : "the assistant",
								   (const gchar *const *)lines->pdata, sc->cancel, scored, sc);
}
static gboolean
debounce_fire(gpointer data)
{
	AiVoiceSession *self = data;
	g_clear_pointer(&P(self)->debounce, g_source_unref);
	start_score(self);
	return G_SOURCE_REMOVE;
}
/* Score the room, after relevance-debounce-ms of quiet when that is set.
 * Nothing is scored in the cooldown after a turn. */
static void
maybe_score(AiVoiceSession *self)
{
	if (!can_score(self))
		return;
	if (P(self)->relevance_debounce_ms == 0) {
		start_score(self);
		return;
	}
	if (P(self)->debounce != NULL) {
		g_source_destroy(P(self)->debounce);
		g_clear_pointer(&P(self)->debounce, g_source_unref);
	}
	P(self)->debounce = g_timeout_source_new(P(self)->relevance_debounce_ms);
	g_source_set_callback(P(self)->debounce, debounce_fire, self, NULL);
	g_source_attach(P(self)->debounce, P(self)->context);
}
static void
confirm_barge(AiVoiceSession *self, Participant *p)
{
	g_log("ai-glib", G_LOG_LEVEL_INFO, "barge-in by %s confirmed by speech, cancelling %s",
		  p->name, P(self)->state == AI_VOICE_SPEAKING ? "speaking" : "thinking");
	p->pending_barge = FALSE;
	interrupt_turn(self);
	state(self, AI_VOICE_TRANSCRIBING);
}
/* A final transcript, attributed: a turn, or in room mode perhaps only heard. */
static void
deliver(AiVoiceSession *self, const gchar *name, const gchar *text)
{
	if (P(self)->stopped)
		return;
	/* Muted while this final was being named: it was said to a deaf session. */
	if (P(self)->muted) {
		g_debug("voice: muted; not hearing %s", name);
		pump(self);
		return;
	}
	if (P(self)->room_mode && *text != '\0' && !addressed(self, name, text)) {
		g_signal_emit_by_name(self, "transcript", name, text, TRUE);
		ai_voice_room_hear(P(self)->room, name, text);
		maybe_score(self);
		pump(self);
		return;
	}
	if (*text != '\0' && g_queue_get_length(&P(self)->turns) < 32) {
		cancel_scoring(self);
		g_queue_push_tail(&P(self)->turns, ai_voice_room_take_turn(P(self)->room, name, text));
	}
	if (*text != '\0')
		g_signal_emit_by_name(self, "transcript", name, text, TRUE);
	pump(self);
}

/* ---- naming speakers who share a microphone ----
 *
 * With a speaker identifier, each final waits for its utterance's audio to be
 * named before it is delivered. Namings finish in any order; finals are
 * delivered in the order they were heard. A final too short to tell is
 * whoever that participant was last named, for a while; a voice that matches
 * nobody is unknown-speaker-name, or the participant's own name. */
#define NAME_MEMORY_US (120 * G_USEC_PER_SEC)
/*
 * One final waiting for its name. The queue holds one reference and an
 * identification in flight holds another, so a reply that arrives after the
 * naming deadline, or after the session dropped the queue, still has
 * somewhere to land. Only the in-flight reference keeps the session alive:
 * a queued naming must not, or a session whose identifier never answers
 * could never be freed.
 */
typedef struct {
	gint refs;
	AiVoiceSession *session; /* borrowed; owned while identification is in flight */
	gchar *speaker, *text, *name;
	gboolean done, too_short, failed, dropped;
	AiSpeakerMatch *match; /* the identifier's answer, when it gave one */
	GCancellable *cancel;
	GSource *timeout;
} Naming;
static Naming *
naming_ref(Naming *n)
{
	n->refs++;
	return n;
}
static void
naming_unref(gpointer data)
{
	Naming *n = data;
	if (--n->refs > 0)
		return;
	if (n->timeout != NULL) {
		g_source_destroy(n->timeout);
		g_source_unref(n->timeout);
	}
	g_clear_object(&n->cancel);
	g_free(n->speaker);
	g_free(n->text);
	g_clear_object(&n->match);
	g_free(n);
}
/* Named, or given up on: the deadline is no longer needed. */
static void
naming_settle(Naming *n)
{
	n->done = TRUE;
	if (n->timeout != NULL) {
		g_source_destroy(n->timeout);
		g_clear_pointer(&n->timeout, g_source_unref);
	}
}
/* Names are settled here, in the order things were said, so "too short to
 * tell" means whoever spoke just before -- not whoever was named first. */
static void
deliver_named(AiVoiceSession *self)
{
	Naming *n;
	while ((n = g_queue_peek_head(&P(self)->naming)) != NULL && n->done) {
		Participant *p = g_hash_table_lookup(P(self)->participants, n->speaker);
		gint64 now = g_get_monotonic_time();
		const gchar *named_as = n->match != NULL ? ai_speaker_match_get_name(n->match) : NULL;
		g_autoptr(AiSpeakerMatch) final = NULL;
		const gchar *name;
		g_queue_pop_head(&P(self)->naming);
		if (named_as != NULL) {
			final = g_object_ref(n->match);
			if (p != NULL) {
				g_free(p->last_named);
				p->last_named = g_strdup(named_as);
				p->last_named_us = now;
			}
		} else if ((n->too_short || n->failed) && p != NULL && p->last_named != NULL &&
				   now - p->last_named_us < NAME_MEMORY_US)
			final = ai_speaker_match_new(AI_SPEAKER_MATCH_INHERITED, p->last_named, 0);
		else if (n->match != NULL) {
			/* A stranger: unknown-speaker-name, or the participant. */
			final = ai_speaker_match_new(AI_SPEAKER_MATCH_UNKNOWN,
										 P(self)->unknown_speaker_name != NULL
											 ? P(self)->unknown_speaker_name
											 : p != NULL ? p->name : n->speaker,
										 ai_speaker_match_get_score(n->match));
			ai_speaker_match_set_voiceprint(final, ai_speaker_match_get_voiceprint(n->match));
		} else
			final = ai_speaker_match_new(AI_SPEAKER_MATCH_FALLBACK,
										 p != NULL ? p->name : n->speaker, 0);
		name = ai_speaker_match_get_name(final);
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Speaker: %s (%.2f)", name,
			  ai_speaker_match_get_score(final));
		if (!P(self)->stopped)
			g_signal_emit_by_name(self, "speaker-named", n->speaker, final);
		if (n->dropped) {
			/* Said before a stop or a mute: on the record, never a turn. */
			if (!P(self)->stopped)
				g_signal_emit_by_name(self, "transcript", name, n->text, TRUE);
		} else if (!P(self)->stopped)
			deliver(self, name, n->text);
		naming_unref(n);
	}
}
/* Stop or mute: what is still being named is heard but not answered, and
 * its identification is no longer worth waiting for. */
static void
drop_pending_namings(AiVoiceSession *self)
{
	GList *l;
	for (l = P(self)->naming.head; l != NULL; l = l->next) {
		Naming *n = l->data;
		n->dropped = TRUE;
		if (!n->done) {
			n->failed = TRUE;
			naming_settle(n);
			g_cancellable_cancel(n->cancel);
		}
	}
	deliver_named(self);
}
static void
named(GObject *source, GAsyncResult *result, gpointer data)
{
	Naming *n = data;
	AiVoiceSession *self = n->session;
	g_autoptr(GError) error = NULL;
	AiSpeakerMatch *match;
	match = ai_speaker_identifier_identify_finish(AI_SPEAKER_IDENTIFIER(source), result, &error);
	/* Settled already -- by its deadline, a stop or a mute: too late to count. */
	if (n->done) {
		g_clear_object(&match);
		naming_unref(n);
		g_object_unref(self);
		return;
	}
	n->match = match;
	n->too_short = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	/* The service down or refusing: the participant, not "a stranger". */
	n->failed = error != NULL && !n->too_short;
	if (n->failed)
		g_debug("voice: speaker not identified: %s", error->message);
	naming_settle(n);
	deliver_named(self);
	naming_unref(n);
	g_object_unref(self);
}
/* An identifier that never answers must not hold every later final. */
static gboolean
naming_expired(gpointer data)
{
	Naming *n = data;
	AiVoiceSession *self = n->session;
	g_log("ai-glib", G_LOG_LEVEL_INFO,
		  "Speaker not named within %u ms; using who spoke last", P(self)->naming_timeout_ms);
	g_clear_pointer(&n->timeout, g_source_unref);
	n->failed = TRUE;
	n->done = TRUE;
	g_cancellable_cancel(n->cancel);
	deliver_named(self);
	return G_SOURCE_REMOVE;
}
static void
name_then_deliver(AiVoiceSession *self, const gchar *speaker, Participant *p,
				  const gchar *text)
{
	Naming *n = g_new0(Naming, 1);
	g_autoptr(GBytes) pcm = g_bytes_new(p->utterance->data, p->utterance->len);
	g_byte_array_set_size(p->utterance, 0);
	n->refs = 1;
	n->session = self;
	n->speaker = g_strdup(speaker);
	n->text = g_strdup(text);
	n->cancel = g_cancellable_new();
	g_queue_push_tail(&P(self)->naming, n);
	if (P(self)->naming_timeout_ms > 0) {
		n->timeout = g_timeout_source_new(P(self)->naming_timeout_ms);
		g_source_set_callback(n->timeout, naming_expired, n, NULL);
		g_source_attach(n->timeout, P(self)->context);
	}
	g_object_ref(self);
	ai_speaker_identifier_identify_async(AI_SPEAKER_IDENTIFIER(P(self)->identifier), pcm, n->cancel,
										 named, naming_ref(n));
}

static void
transcript(AiSpeechRecognizer *recognizer, const gchar *speaker, const gchar *text,
		   gboolean final, gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p = g_hash_table_lookup(P(self)->participants, speaker);
	g_autofree gchar *trimmed = NULL;
	gboolean pending;
	if (P(self)->stopped || p == NULL)
		return;
	if (!final && P(self)->muted)
		return;
	if (!final) {
		g_autofree gchar *words = strip_annotations(text);
		if (p->pending_barge && has_words(words) && !is_echo(self, words))
			confirm_barge(self, p);
		g_signal_emit_by_name(self, "transcript", p->name, words, FALSE);
		return;
	}
	trimmed = strip_annotations(text);
	text = g_strstrip(trimmed);
	pending = p->pending_barge;
	p->recognizing = FALSE;
	p->ended = FALSE;
	p->pending_barge = FALSE;
	p->speech_ms = 0;
	g_byte_array_set_size(p->onset, 0);
	ai_voice_activity_reset(P(self)->activity, speaker);
	if (*text != '\0' && (pending || P(self)->state == AI_VOICE_SPEAKING) &&
		is_echo(self, text)) {
		g_log("ai-glib", G_LOG_LEVEL_INFO,
			  "ignoring transcript from %s: it repeats our own recent speech", p->name);
		pump(self);
		return;
	}
	if (pending && has_words(text) && !P(self)->muted)
		confirm_barge(self, p);
	if (P(self)->voice_commands && has_words(text)) {
		const gchar *command = match_command(self, text);
		/* Muted, only a way back is heard: nothing else reaches the model or
		 * is reported as said. */
		if (command != NULL &&
			(!P(self)->muted || g_str_equal(command, "unmute") || g_str_equal(command, "hangup"))) {
			run_command(self, command);
			pump(self);
			return;
		}
		/* "Guest two is Kim": only with an identifier that knows guests. */
		if (!P(self)->muted && P(self)->identifier != NULL) {
			g_autofree gchar *guest = NULL, *name = NULL;
			if (match_promotion(self, text, &guest, &name)) {
				run_promotion(self, guest, name);
				pump(self);
				return;
			}
		}
	}
	if (P(self)->muted) {
		g_debug("voice: muted; not hearing %s", p->name);
		pump(self);
		return;
	}
	if (P(self)->identifier != NULL && *text != '\0') {
		name_then_deliver(self, speaker, p, text);
		return;
	}
	deliver(self, p->name, text);
}
static void
stt_error(AiSpeechRecognizer *recognizer, const gchar *speaker, GError *error,
		  gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p = g_hash_table_lookup(P(self)->participants, speaker);
	if (P(self)->stopped)
		return;
	ai_voice_activity_reset(P(self)->activity, speaker);
	if (p != NULL) {
		p->recognizing = FALSE;
		p->ended = FALSE;
		p->pending_barge = FALSE;
		p->speech_ms = 0;
		g_byte_array_set_size(p->onset, 0);
	}
	report(self, error);
	queue_line(self, P(self)->transcription_error_message, FALSE);
	pump(self);
}
static void
transport_reconnecting(AiAudioTransport *transport, gpointer data)
{
	AiVoiceSession *self = data;
	if (P(self)->stopped || P(self)->media_recovering)
		return;
	P(self)->media_recovering = TRUE;
	if (P(self)->deadline != NULL) {
		P(self)->paused_deadline_us = MAX(
			(gint64)1, g_source_get_ready_time(P(self)->deadline) - g_get_monotonic_time());
		clear_deadline(self);
	}
}
static void
transport_reconnected(AiAudioTransport *transport, gpointer data)
{
	AiVoiceSession *self = data;
	P(self)->media_recovering = FALSE;
	if (!P(self)->stopped && P(self)->provider_pending && P(self)->paused_deadline_us > 0) {
		P(self)->deadline =
			g_timeout_source_new((guint)((P(self)->paused_deadline_us + 999) / 1000));
		g_source_set_callback(P(self)->deadline, deadline, self, NULL);
		g_source_attach(P(self)->deadline, P(self)->context);
	}
	P(self)->paused_deadline_us = 0;
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
	Naming *n;
	g_return_if_fail(AI_IS_VOICE_SESSION(self));
	if (P(self)->stopped)
		return;
	P(self)->stopped = TRUE;
	P(self)->media_recovering = TRUE;
	cancel_scoring(self);
	/* Settle every naming before dropping the queue, so no deadline fires
	 * later on a session that has stopped; a reply still on its way finds
	 * its naming done and is ignored. */
	while ((n = g_queue_pop_head(&P(self)->naming)) != NULL) {
		naming_settle(n);
		g_cancellable_cancel(n->cancel);
		naming_unref(n);
	}
	interrupt_turn(self);
	g_hash_table_iter_init(&iter, P(self)->participants);
	while (g_hash_table_iter_next(&iter, &key, NULL)) {
		ai_speech_recognizer_cancel(P(self)->recognizer, key);
		ai_voice_activity_reset(P(self)->activity, key);
	}
	g_hash_table_remove_all(P(self)->participants);
	if (P(self)->worker != NULL) {
		ai_voice_worker_stop(P(self)->worker);
		P(self)->worker = NULL;
	}
	state(self, AI_VOICE_LISTENING);
}
static void
dispose(GObject *object)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	ai_voice_session_stop(self);
	if (P(self)->transport != NULL)
		g_signal_handlers_disconnect_by_data(P(self)->transport, self);
	if (P(self)->recognizer != NULL)
		g_signal_handlers_disconnect_by_data(P(self)->recognizer, self);
	if (P(self)->synthesizer != NULL)
		g_signal_handlers_disconnect_by_data(P(self)->synthesizer, self);
	g_clear_object(&P(self)->transport);
	g_clear_object(&P(self)->recognizer);
	g_clear_object(&P(self)->synthesizer);
	g_clear_object(&P(self)->activity);
	g_clear_object(&P(self)->conversation);
	g_clear_object(&P(self)->turn_cancel);
	g_clear_object(&P(self)->judge);
	g_clear_object(&P(self)->detector);
	g_clear_object(&P(self)->name_detector);
	g_clear_object(&P(self)->room);
	g_clear_object(&P(self)->identifier);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	g_hash_table_unref(P(self)->participants);
	g_hash_table_unref(P(self)->notices);
	g_hash_table_unref(P(self)->repeats);
	g_string_free(P(self)->span, TRUE);
	g_strfreev(P(self)->assistant_names);
	g_free(P(self)->promoted_message);
	g_free(P(self)->promotion_refused_message);
	g_free(P(self)->unknown_speaker_name);
	g_free(P(self)->logged_tool);
	g_free(P(self)->mute_message);
	g_free(P(self)->unmute_message);
	g_free(P(self)->repeat_message);
	g_main_context_unref(P(self)->context);
	g_queue_clear_full(&P(self)->lines, line_free);
	g_queue_clear_full(&P(self)->turns, g_free);
	g_queue_clear_full(&P(self)->pending_notices, line_free);
	g_queue_clear_full(&P(self)->said, said_free);
	g_string_free(P(self)->pending_text, TRUE);
	g_string_free(P(self)->spoken, TRUE);
	g_free(P(self)->deadline_message);
	g_free(P(self)->transcription_error_message);
	g_free(P(self)->synthesis_error_message);
	g_free(P(self)->empty_reply_message);
	g_free(P(self)->tool_progress_message);
	g_clear_pointer(&P(self)->fallback_pcm, g_bytes_unref);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->finalize(object);
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	if (id == PROP_TURN_DEADLINE)
		g_value_set_uint(value, P(self)->deadline_ms);
	else if (id == PROP_BARGE_IN)
		g_value_set_uint(value, P(self)->barge_in_ms);
	else if (id == PROP_BARGE_IN_CONFIRM)
		g_value_set_boolean(value, P(self)->barge_in_confirm);
	else if (id == PROP_STATE)
		g_value_set_enum(value, P(self)->state);
	else if (id == PROP_TRANSPORT)
		g_value_set_object(value, P(self)->transport);
	else if (id == PROP_RECOGNIZER)
		g_value_set_object(value, P(self)->recognizer);
	else if (id == PROP_SYNTHESIZER)
		g_value_set_object(value, P(self)->synthesizer);
	else if (id == PROP_SYNTHESIS_ERROR_MESSAGE)
		g_value_set_string(value, P(self)->synthesis_error_message);
	else if (id == PROP_EMPTY_REPLY_MESSAGE)
		g_value_set_string(value, P(self)->empty_reply_message);
	else if (id == PROP_TOOL_PROGRESS_MESSAGE)
		g_value_set_string(value, P(self)->tool_progress_message);
	else if (id == PROP_TOOL_PROGRESS_DELAY)
		g_value_set_uint(value, P(self)->tool_progress_delay_ms);
	else if (id == PROP_TRIM_AFTER)
		g_value_set_uint(value, P(self)->trim_after);
	else if (id == PROP_SPEAK_CODE)
		g_value_set_boolean(value, P(self)->speak_code);
	else if (id == PROP_REPEAT_LIMIT)
		g_value_set_uint(value, P(self)->repeat_limit);
	else if (id == PROP_REPEAT_MESSAGE)
		g_value_set_string(value, P(self)->repeat_message);
	else if (id == PROP_VOICE_COMMANDS)
		g_value_set_boolean(value, P(self)->voice_commands);
	else if (id == PROP_ASSISTANT_NAMES) {
		g_autofree gchar *joined =
			P(self)->assistant_names != NULL ? g_strjoinv(",", P(self)->assistant_names) : NULL;
		g_value_set_string(value, joined);
	} else if (id == PROP_MUTED)
		g_value_set_boolean(value, P(self)->muted);
	else if (id == PROP_MUTE_MESSAGE)
		g_value_set_string(value, P(self)->mute_message);
	else if (id == PROP_UNMUTE_MESSAGE)
		g_value_set_string(value, P(self)->unmute_message);
	else if (id == PROP_ROOM_MODE)
		g_value_set_boolean(value, P(self)->room_mode);
	else if (id == PROP_RELEVANCE_PROVIDER) {
		g_autoptr(AiProvider) provider = NULL;
		if (AI_IS_PROVIDER_RELEVANCE_JUDGE(P(self)->judge))
			g_object_get(P(self)->judge, "provider", &provider, NULL);
		g_value_set_object(value, provider);
	} else if (id == PROP_RELEVANCE_JUDGE)
		g_value_set_object(value, P(self)->judge);
	else if (id == PROP_RELEVANCE_DEBOUNCE)
		g_value_set_uint(value, P(self)->relevance_debounce_ms);
	else if (id == PROP_ROOM)
		g_value_set_object(value, P(self)->room);
	else if (id == PROP_ADDRESS_DETECTOR)
		g_value_set_object(value, detector_of(self));
	else if (id == PROP_PROMOTED_MESSAGE)
		g_value_set_string(value, P(self)->promoted_message);
	else if (id == PROP_PROMOTION_REFUSED_MESSAGE)
		g_value_set_string(value, P(self)->promotion_refused_message);
	else if (id == PROP_CHIME_IN_THRESHOLD)
		g_value_set_uint(value, P(self)->chime_threshold);
	else if (id == PROP_CHIME_IN_COOLDOWN)
		g_value_set_uint(value, P(self)->chime_cooldown_ms);
	else if (id == PROP_SPEAKER_IDENTIFIER)
		g_value_set_object(value, P(self)->identifier);
	else if (id == PROP_NAMING_TIMEOUT)
		g_value_set_uint(value, P(self)->naming_timeout_ms);
	else if (id == PROP_UNKNOWN_SPEAKER_NAME)
		g_value_set_string(value, P(self)->unknown_speaker_name);
	else if (id == PROP_ASSISTANT_SOUNDALIKES)
		g_object_get_property(G_OBJECT(P(self)->name_detector), "soundalikes", value);
	else if (id == PROP_FALLBACK_PCM)
		g_value_set_boxed(value, P(self)->fallback_pcm);
	else if (id == PROP_FALLBACK_SAMPLE_RATE)
		g_value_set_uint(value, P(self)->fallback_sample_rate);
	else if (id == PROP_ACTIVITY)
		g_value_set_object(value, P(self)->activity);
	else if (id == PROP_CONVERSATION)
		g_value_set_object(value, P(self)->conversation);
	else if (id == PROP_DEADLINE_MESSAGE)
		g_value_set_string(value, P(self)->deadline_message);
	else if (id == PROP_TRANSCRIPTION_ERROR_MESSAGE)
		g_value_set_string(value, P(self)->transcription_error_message);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	if (id == PROP_TURN_DEADLINE)
		P(self)->deadline_ms = g_value_get_uint(value);
	else if (id == PROP_BARGE_IN)
		P(self)->barge_in_ms = g_value_get_uint(value);
	else if (id == PROP_BARGE_IN_CONFIRM)
		P(self)->barge_in_confirm = g_value_get_boolean(value);
	else if (id == PROP_TRANSPORT)
		g_set_object(&P(self)->transport, g_value_get_object(value));
	else if (id == PROP_RECOGNIZER)
		g_set_object(&P(self)->recognizer, g_value_get_object(value));
	else if (id == PROP_SYNTHESIZER)
		g_set_object(&P(self)->synthesizer, g_value_get_object(value));
	else if (id == PROP_FALLBACK_PCM) {
		GBytes *pcm = g_value_get_boxed(value);
		g_return_if_fail(pcm == NULL || (g_bytes_get_size(pcm) > 0 &&
										 g_bytes_get_size(pcm) <= 4 * 1024 * 1024 &&
										 g_bytes_get_size(pcm) % 2 == 0));
		g_clear_pointer(&P(self)->fallback_pcm, g_bytes_unref);
		P(self)->fallback_pcm = pcm != NULL ? g_bytes_ref(pcm) : NULL;
	} else if (id == PROP_FALLBACK_SAMPLE_RATE)
		P(self)->fallback_sample_rate = g_value_get_uint(value);
	else if (id == PROP_TOOL_PROGRESS_MESSAGE) {
		g_free(P(self)->tool_progress_message);
		P(self)->tool_progress_message = g_value_dup_string(value);
	} else if (id == PROP_TOOL_PROGRESS_DELAY)
		P(self)->tool_progress_delay_ms = g_value_get_uint(value);
	else if (id == PROP_SPEAK_CODE)
		P(self)->speak_code = g_value_get_boolean(value);
	else if (id == PROP_REPEAT_LIMIT)
		P(self)->repeat_limit = g_value_get_uint(value);
	else if (id == PROP_REPEAT_MESSAGE) {
		g_free(P(self)->repeat_message);
		P(self)->repeat_message = g_value_dup_string(value);
	} else if (id == PROP_VOICE_COMMANDS)
		P(self)->voice_commands = g_value_get_boolean(value);
	else if (id == PROP_ASSISTANT_NAMES) {
		const gchar *names = g_value_get_string(value);
		g_clear_pointer(&P(self)->assistant_names, g_strfreev);
		if (names != NULL && *names != '\0') {
			guint i;
			P(self)->assistant_names = g_strsplit(names, ",", -1);
			for (i = 0; P(self)->assistant_names[i] != NULL; i++)
				g_strstrip(P(self)->assistant_names[i]);
		}
		/* The names also tell the default detector what to listen for. */
		g_object_set(P(self)->name_detector, "names", names, NULL);
	} else if (id == PROP_MUTED)
		set_muted(self, g_value_get_boolean(value));
	else if (id == PROP_MUTE_MESSAGE) {
		g_free(P(self)->mute_message);
		P(self)->mute_message = g_value_dup_string(value);
	} else if (id == PROP_UNMUTE_MESSAGE) {
		g_free(P(self)->unmute_message);
		P(self)->unmute_message = g_value_dup_string(value);
	} else if (id == PROP_ROOM_MODE)
		P(self)->room_mode = g_value_get_boolean(value);
	else if (id == PROP_RELEVANCE_PROVIDER) {
		AiProvider *provider = g_value_get_object(value);
		cancel_scoring(self);
		g_clear_object(&P(self)->judge);
		if (provider != NULL)
			P(self)->judge = AI_RELEVANCE_JUDGE(ai_provider_relevance_judge_new(provider));
	} else if (id == PROP_RELEVANCE_JUDGE) {
		cancel_scoring(self);
		g_set_object(&P(self)->judge, g_value_get_object(value));
	} else if (id == PROP_RELEVANCE_DEBOUNCE)
		P(self)->relevance_debounce_ms = g_value_get_uint(value);
	else if (id == PROP_ROOM) {
		AiVoiceRoom *room = g_value_get_object(value);
		/* NULL keeps the room there is: a session always has one. */
		if (room != NULL)
			g_set_object(&P(self)->room, room);
	} else if (id == PROP_ADDRESS_DETECTOR)
		g_set_object(&P(self)->detector, g_value_get_object(value));
	else if (id == PROP_PROMOTED_MESSAGE) {
		g_free(P(self)->promoted_message);
		P(self)->promoted_message = g_value_dup_string(value);
	} else if (id == PROP_PROMOTION_REFUSED_MESSAGE) {
		g_free(P(self)->promotion_refused_message);
		P(self)->promotion_refused_message = g_value_dup_string(value);
	} else if (id == PROP_CHIME_IN_THRESHOLD)
		P(self)->chime_threshold = g_value_get_uint(value);
	else if (id == PROP_CHIME_IN_COOLDOWN)
		P(self)->chime_cooldown_ms = g_value_get_uint(value);
	else if (id == PROP_SPEAKER_IDENTIFIER)
		g_set_object(&P(self)->identifier, g_value_get_object(value));
	else if (id == PROP_NAMING_TIMEOUT)
		P(self)->naming_timeout_ms = g_value_get_uint(value);
	else if (id == PROP_ASSISTANT_SOUNDALIKES)
		g_object_set_property(G_OBJECT(P(self)->name_detector), "soundalikes", value);
	else if (id == PROP_UNKNOWN_SPEAKER_NAME) {
		const gchar *name = g_value_get_string(value);
		g_free(P(self)->unknown_speaker_name);
		P(self)->unknown_speaker_name = name != NULL && *name != '\0' ? g_strdup(name) : NULL;
	} else if (id == PROP_TRIM_AFTER) {
		P(self)->trim_after = g_value_get_uint(value);
		if (P(self)->worker != NULL)
			ai_voice_worker_set_trim_after(P(self)->worker, P(self)->trim_after);
	}
	else if (id == PROP_EMPTY_REPLY_MESSAGE) {
		g_free(P(self)->empty_reply_message);
		P(self)->empty_reply_message = g_value_dup_string(value);
	} else if (id == PROP_SYNTHESIS_ERROR_MESSAGE) {
		g_free(P(self)->synthesis_error_message);
		P(self)->synthesis_error_message = g_value_dup_string(value);
	} else if (id == PROP_ACTIVITY)
		g_set_object(&P(self)->activity, g_value_get_object(value));
	else if (id == PROP_CONVERSATION)
		g_set_object(&P(self)->conversation, g_value_get_object(value));
	else if (id == PROP_DEADLINE_MESSAGE) {
		g_free(P(self)->deadline_message);
		P(self)->deadline_message = g_value_dup_string(value);
	} else if (id == PROP_TRANSCRIPTION_ERROR_MESSAGE) {
		g_free(P(self)->transcription_error_message);
		P(self)->transcription_error_message = g_value_dup_string(value);
	} else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
constructed(GObject *object)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->constructed(object);
	g_return_if_fail(P(self)->transport != NULL && P(self)->recognizer != NULL &&
					 P(self)->synthesizer != NULL && P(self)->activity != NULL &&
					 P(self)->conversation != NULL);
	g_signal_connect(P(self)->transport, "audio", G_CALLBACK(audio_in), self);
	g_signal_connect(P(self)->transport, "participant-joined", G_CALLBACK(joined), self);
	g_signal_connect(P(self)->transport, "participant-left", G_CALLBACK(left), self);
	g_signal_connect(P(self)->transport, "reconnecting", G_CALLBACK(transport_reconnecting),
					 self);
	g_signal_connect(P(self)->transport, "reconnected", G_CALLBACK(transport_reconnected),
					 self);
	g_signal_connect(P(self)->transport, "error", G_CALLBACK(transport_error), self);
	g_signal_connect(P(self)->recognizer, "transcript", G_CALLBACK(transcript), self);
	g_signal_connect(P(self)->recognizer, "error", G_CALLBACK(stt_error), self);
	g_signal_connect(P(self)->synthesizer, "audio", G_CALLBACK(audio_out), self);
	P(self)->worker = ai_voice_worker_new(P(self)->conversation, G_OBJECT(self), P(self)->context,
									   worker_mail);
	ai_voice_worker_set_trim_after(P(self)->worker, P(self)->trim_after);
}
/* Every handler must say yes: the first FALSE stops the emission. */
static gboolean
all_agree(GSignalInvocationHint *hint, GValue *accumulated, const GValue *handler_return,
		  gpointer data)
{
	gboolean yes = g_value_get_boolean(handler_return);
	g_value_set_boolean(accumulated, yes);
	return yes;
}
static gboolean
default_allow_promotion(AiVoiceSession *self, const gchar *guest, const gchar *name)
{
	return TRUE;
}
static void
ai_voice_session_class_init(AiVoiceSessionClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	klass->allow_promotion = default_allow_promotion;
	oc->dispose = dispose;
	oc->constructed = constructed;
	oc->finalize = finalize;
	oc->get_property = get_property;
	oc->set_property = set_property;
	g_object_class_install_property(
		oc, PROP_FALLBACK_PCM,
		g_param_spec_boxed(
			"fallback-pcm", "Fallback PCM",
			"Optional in-memory S16LE mono spoken synthesis failure message",
			G_TYPE_BYTES, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_FALLBACK_SAMPLE_RATE,
		g_param_spec_uint("fallback-sample-rate", "Fallback sample rate",
						  "Native rate of fallback PCM", 8000, 192000, 16000,
						  G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
							  G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_SYNTHESIS_ERROR_MESSAGE,
		g_param_spec_string("synthesis-error-message", "Synthesis error message",
							"Spoken fallback after synthesis failure",
							"Sorry, the speech service is unavailable. Please try again.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
								G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_BARGE_IN,
		g_param_spec_uint(
			"barge-in-ms", "Barge-in debounce",
			"Consecutive speech required before recognition or interruption", 10, 5000,
			250, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_SPEAK_CODE,
		g_param_spec_boolean("speak-code", "Speak code",
							 "Read markdown code spans and fenced blocks aloud; when off "
							 "they are left out of speech",
							 TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_REPEAT_LIMIT,
		g_param_spec_uint("repeat-limit", "Repeat limit",
						  "Stop a reply once one line has been spoken this many times; "
						  "0 never stops one",
						  0, 100, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_REPEAT_MESSAGE,
		g_param_spec_string("repeat-message", "Repeat message",
							"Spoken when repeat-limit stops a reply; empty says nothing",
							"I'm going in circles, so I'll stop there.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_VOICE_COMMANDS,
		g_param_spec_boolean("voice-commands", "Voice commands",
							 "Treat a whole utterance of stop, mute, unmute or hang up as a "
							 "command instead of something to answer",
							 FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_ASSISTANT_NAMES,
		g_param_spec_string("assistant-names", "Assistant names",
							"Comma-separated names a command may start with, as in "
							"\"Name, stop listening\"",
							NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_MUTED,
		g_param_spec_boolean("muted", "Muted",
							 "The caller is not heard: nothing they say reaches the model "
							 "or is reported, except unmute and hang up",
							 FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY |
										G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_MUTE_MESSAGE,
		g_param_spec_string("mute-message", "Mute message",
							"Spoken when the caller mutes the assistant; empty says nothing",
							"Okay, I'm not listening. Say unmute when you need me.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_UNMUTE_MESSAGE,
		g_param_spec_string("unmute-message", "Unmute message",
							"Spoken when the caller unmutes the assistant; empty says nothing",
							"I'm listening.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:room-mode:
	 *
	 * Several people share the audio. What does not name the assistant (one of
	 * #AiVoiceSession:assistant-names, anywhere in the utterance) is heard but
	 * not answered, and is given to the model with the next turn. Without
	 * assistant names, every utterance names it.
	 */
	g_object_class_install_property(
		oc, PROP_ROOM_MODE,
		g_param_spec_boolean("room-mode", "Room mode",
							 "Answer only utterances that name the assistant; hear the rest",
							 FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:relevance-provider:
	 *
	 * An #AiProvider that scores, from 0 to 10, how much the assistant should
	 * speak up about talk it was not addressed in. A small fast model is
	 * enough. It must not be the conversation's own provider object, which
	 * the session's worker thread is using. %NULL never chimes in.
	 */
	g_object_class_install_property(
		oc, PROP_RELEVANCE_PROVIDER,
		g_param_spec_object("relevance-provider", "Relevance provider",
							"Provider that scores unaddressed room talk for chiming in",
							AI_TYPE_PROVIDER, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:chime-in-threshold:
	 *
	 * In room mode, a relevance score at or above this takes a turn unasked.
	 * 0 never does.
	 */
	g_object_class_install_property(
		oc, PROP_CHIME_IN_THRESHOLD,
		g_param_spec_uint("chime-in-threshold", "Chime-in threshold",
						  "Relevance (1-10) at which the assistant speaks up unasked; 0 never",
						  0, 10, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:chime-in-cooldown-ms:
	 *
	 * After any turn, room talk is not scored for this long.
	 */
	g_object_class_install_property(
		oc, PROP_CHIME_IN_COOLDOWN,
		g_param_spec_uint("chime-in-cooldown-ms", "Chime-in cooldown",
						  "Time after a turn before room talk is scored again",
						  0, 3600000, 60000,
						  G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:speaker-identifier: (type AiSpeakerIdentifier)
	 *
	 * Names the speaker of each final transcript from its audio, for several
	 * people sharing one participant's microphone. Finals are delivered in
	 * the order they were heard, each once it is named. %NULL: every
	 * utterance is its participant's.
	 */
	g_object_class_install_property(
		oc, PROP_SPEAKER_IDENTIFIER,
		g_param_spec_object("speaker-identifier", "Speaker identifier",
							"Names who spoke each utterance from its audio",
							AI_TYPE_SPEAKER_IDENTIFIER,
							G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:room:
	 *
	 * What the session remembers of the room between turns: the talk that
	 * was not a turn, and who has been speaking. Replace it with a
	 * configured or subclassed #AiVoiceRoom to change the limits or how
	 * talk reaches the model.
	 */
	g_object_class_install_property(
		oc, PROP_ROOM,
		g_param_spec_object("room", "Room", "What is remembered of the room between turns",
							AI_TYPE_VOICE_ROOM, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:address-detector:
	 *
	 * In room mode, decides which utterances are said to the assistant.
	 * Unset (or set to %NULL), an #AiNameAddressDetector built from
	 * #AiVoiceSession:assistant-names and #AiVoiceSession:assistant-soundalikes.
	 */
	g_object_class_install_property(
		oc, PROP_ADDRESS_DETECTOR,
		g_param_spec_object("address-detector", "Address detector",
							"Decides which utterances are said to the assistant",
							AI_TYPE_ADDRESS_DETECTOR,
							G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:relevance-judge:
	 *
	 * Scores unaddressed room talk for chiming in. Setting
	 * #AiVoiceSession:relevance-provider replaces it with an
	 * #AiProviderRelevanceJudge for that provider. %NULL never chimes in.
	 */
	g_object_class_install_property(
		oc, PROP_RELEVANCE_JUDGE,
		g_param_spec_object("relevance-judge", "Relevance judge",
							"Scores unaddressed room talk for chiming in",
							AI_TYPE_RELEVANCE_JUDGE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:relevance-debounce-ms:
	 *
	 * Wait this long after the last unaddressed line before scoring, so a
	 * burst of talk is scored once. 0 scores at once; a score in flight is
	 * never cancelled by new talk, only redone with it.
	 */
	g_object_class_install_property(
		oc, PROP_RELEVANCE_DEBOUNCE,
		g_param_spec_uint("relevance-debounce-ms", "Relevance debounce",
						  "Quiet before room talk is scored", 0, 60000, 0,
						  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:promoted-message:
	 *
	 * Spoken when a voice command saves a guest's voice under a name.
	 * "{name}" and "{guest}" are replaced. Empty says nothing.
	 */
	g_object_class_install_property(
		oc, PROP_PROMOTED_MESSAGE,
		g_param_spec_string("promoted-message", "Promoted message",
							"Spoken when a guest's voice is saved under a name",
							"Okay, I'll remember {name}.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:promotion-refused-message:
	 *
	 * Spoken when a guest's voice cannot be saved under a name. "{name}",
	 * "{guest}" and "{reason}" are replaced. Empty says nothing.
	 */
	g_object_class_install_property(
		oc, PROP_PROMOTION_REFUSED_MESSAGE,
		g_param_spec_string("promotion-refused-message", "Promotion refused message",
							"Spoken when a guest's voice cannot be saved under a name",
							"I can't save that voice as {name}: {reason}.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:naming-timeout-ms:
	 *
	 * The longest a final transcript waits for #AiVoiceSession:speaker-identifier
	 * to name it. Past it the final goes on as whoever that microphone was
	 * last named, or as its participant, and a late name is ignored. 0 waits
	 * as long as the identifier takes.
	 */
	g_object_class_install_property(
		oc, PROP_NAMING_TIMEOUT,
		g_param_spec_uint("naming-timeout-ms", "Naming timeout",
						  "Longest a final waits to be named; 0 waits for the identifier",
						  0, 600000, 8000,
						  G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:assistant-soundalikes:
	 *
	 * Comma-separated words speech recognition hears in place of an assistant
	 * name ("say" for "Kay"). In room mode one counts as naming the assistant
	 * only where a name would be: after a greeting, before a comma that opens
	 * the utterance, or after a comma that ends it.
	 */
	g_object_class_install_property(
		oc, PROP_ASSISTANT_SOUNDALIKES,
		g_param_spec_string("assistant-soundalikes", "Assistant sound-alikes",
							"Words heard for an assistant name, counted only in a name's place",
							NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	/**
	 * AiVoiceSession:unknown-speaker-name:
	 *
	 * With a speaker identifier, the name for a voice that matches nobody.
	 * %NULL or empty: the participant's own name.
	 */
	g_object_class_install_property(
		oc, PROP_UNKNOWN_SPEAKER_NAME,
		g_param_spec_string("unknown-speaker-name", "Unknown speaker name",
							"Name for a voice the identifier does not know", NULL,
							G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_TRIM_AFTER,
		g_param_spec_uint("trim-tool-results-after", "Trim tool results after",
						  "Keep tool output in full for this many recent turns and replace "
						  "older output with a short note; 0 keeps everything",
						  0, 1000, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_TOOL_PROGRESS_MESSAGE,
		g_param_spec_string("tool-progress-message", "Tool progress message",
							"Spoken once per turn when a tool has run for "
							"tool-progress-delay-ms with nothing said yet. Empty disables it",
							"One moment, let me check.",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_TOOL_PROGRESS_DELAY,
		g_param_spec_uint("tool-progress-delay-ms", "Tool progress delay",
						  "How long a tool must run in silence before the progress line",
						  0, 60000, 1500,
						  G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_EMPTY_REPLY_MESSAGE,
		g_param_spec_string("empty-reply-message", "Empty reply message",
							"Spoken when a provider turn ends with nothing to say. Empty "
							"disables it",
							"Sorry, I didn't come up with an answer to that. Could you "
							"ask me again?",
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
		oc, PROP_BARGE_IN_CONFIRM,
		g_param_spec_boolean(
			"barge-in-confirm", "Confirm barge-in",
			"While speaking, interrupt only once recognized words arrive that are not an "
			"echo of our own recent speech. FALSE interrupts on voice activity alone",
			TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
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
	g_signal_new("transcript", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET(AiVoiceSessionClass, transcript), NULL, NULL,
				 NULL, G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
	/**
	 * AiVoiceSession::reply:
	 * @self: the session
	 * @text: a segment submitted for synthesis
	 */
	g_signal_new("reply", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET(AiVoiceSessionClass, reply), NULL, NULL,
				 NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
	/**
	 * AiVoiceSession::tool:
	 * @self: the session
	 * @name: the tool's name
	 * @arguments: its input, as JSON text
	 * @result: what it returned, or its error message
	 * @is_error: whether it failed
	 *
	 * Emitted once per tool call that finished, including calls from a turn
	 * that was since interrupted: the tool ran either way.
	 */
	g_signal_new("tool", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
				 G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
				 G_TYPE_BOOLEAN);
	/**
	 * AiVoiceSession::command:
	 * @session: the session
	 * @name: "stop", "mute", "unmute", "hangup" or "promote"
	 *
	 * A caller spoke a command while #AiVoiceSession:voice-commands is set.
	 * The session has already acted on stop, mute and unmute; hang-up is the
	 * application's to carry out.
	 */
	g_signal_new("command", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET(AiVoiceSessionClass, command), NULL, NULL, NULL,
				 G_TYPE_NONE, 1, G_TYPE_STRING);
	/**
	 * AiVoiceSession::spoken:
	 * @self: the session
	 * @text: a segment of which at least some audio was played
	 * @complete: FALSE when playback was cut short, by barge-in or failure
	 *
	 * Emitted once per segment after its playback ends, in order. A segment
	 * that produced no audio at all is not reported: nobody heard it.
	 */
	g_signal_new("spoken", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
	/**
	 * AiVoiceSession::relevance:
	 * @session: the session
	 * @score: 0 to 10
	 * @reason: the scorer's one-line reason, possibly empty
	 * @chimed: whether the assistant took a turn because of it
	 *
	 * Room talk was scored. Emitted for every score, above the threshold or
	 * not, so a host can log them and tune #AiVoiceSession:chime-in-threshold.
	 */
	g_signal_new("relevance", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET(AiVoiceSessionClass, relevance), NULL, NULL,
				 NULL, G_TYPE_NONE, 3, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_BOOLEAN);
	/**
	 * AiVoiceSession::speaker-named:
	 * @session: the session
	 * @participant: the transport's participant whose microphone heard it
	 * @match: who said it, and how that was decided
	 *
	 * With #AiVoiceSession:speaker-identifier, emitted for each final once it
	 * is named, in the order heard, before it is delivered. A match of kind
	 * %AI_SPEAKER_MATCH_INHERITED or %AI_SPEAKER_MATCH_FALLBACK is the
	 * session's own decision when the identifier could not say.
	 */
	g_signal_new("speaker-named", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET(AiVoiceSessionClass, speaker_named), NULL, NULL, NULL,
				 G_TYPE_NONE, 2, G_TYPE_STRING, AI_TYPE_SPEAKER_MATCH);
	/**
	 * AiVoiceSession::allow-promotion:
	 * @session: the session
	 * @guest: the guest's name ("Guest 2")
	 * @name: the name asked for
	 *
	 * Someone asked, by voice, for a guest's voice to be saved as @name.
	 * Return %FALSE to refuse. Every handler must agree; the first %FALSE
	 * wins. The identifier itself still refuses a name already enrolled.
	 *
	 * Returns: whether the promotion may go ahead
	 */
	g_signal_new("allow-promotion", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET(AiVoiceSessionClass, allow_promotion), all_agree, NULL, NULL,
				 G_TYPE_BOOLEAN, 2, G_TYPE_STRING, G_TYPE_STRING);
	/**
	 * AiVoiceSession::guest-promoted:
	 * @session: the session
	 * @guest: the guest's name as it was
	 * @name: the name the voice now has
	 *
	 * A guest's voice was saved under a lasting name by voice command.
	 */
	g_signal_new("guest-promoted", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
				 G_STRUCT_OFFSET(AiVoiceSessionClass, guest_promoted), NULL, NULL, NULL,
				 G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
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
	P(self)->context = g_main_context_ref_thread_default();
	P(self)->participants =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, participant_free);
	P(self)->notices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	P(self)->repeats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	P(self)->speak_code = TRUE;
	P(self)->span = g_string_new(NULL);
	P(self)->pending_text = g_string_new(NULL);
	P(self)->spoken = g_string_new(NULL);
	P(self)->deadline_ms = 20000;
	P(self)->barge_in_ms = 250;
	P(self)->barge_in_confirm = TRUE;
	P(self)->room = ai_voice_room_new();
	P(self)->name_detector = ai_name_address_detector_new(NULL, NULL);
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
	return P(self)->state;
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
