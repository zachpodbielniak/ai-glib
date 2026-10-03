/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voice-session.h"
#include "voice/ai-voice-worker-private.h"
#include "model/ai-tool-result.h"
#include "core/ai-provider.h"
#include "voice/ai-speaker-identifier.h"
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

struct _AiVoiceSession {
	GObject parent_instance;
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
	gchar **soundalikes; /* heard for a name, counted only where a name would be */
	gchar *mute_message, *unmute_message;
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
	/* room-mode: talk that does not name the assistant is heard, not answered */
	gboolean room_mode;
	GQueue room;           /* "[Name]: text" heard since the assistant last took a turn */
	guint64 room_revision; /* bumped by every line heard; a score for an older one is stale */
	GObject *relevance_provider;
	guint chime_threshold, chime_cooldown_ms;
	GCancellable *scoring;
	gint64 last_turn_us;
	gchar *logged_tool; /* id of the last tool whose start was logged */
	/* speaker-identifier: finals wait here, in order, until each is named */
	GObject *identifier;
	gchar *unknown_speaker_name;
	GQueue naming;
	GCancellable *naming_cancel;
};
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
	PROP_ASSISTANT_SOUNDALIKES
};
G_DEFINE_TYPE(AiVoiceSession, ai_voice_session, G_TYPE_OBJECT)
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
	g_queue_push_tail(&self->said, said);
	while (g_queue_get_length(&self->said) > 16)
		said_free(g_queue_pop_head(&self->said));
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
	for (l = self->said.head; l != NULL; l = l->next) {
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
	if (self->state == next)
		return;
	self->state = next;
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
	if (self->deadline == NULL)
		return;
	g_source_destroy(self->deadline);
	g_clear_pointer(&self->deadline, g_source_unref);
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
	if (text == NULL || self->stopped)
		return;
	if (g_queue_get_length(&self->lines) >= 128)
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
	g_queue_push_tail(&self->lines, line);
	if (counted)
		self->turn_lines++;
}
static void
queue_line(AiVoiceSession *self, const gchar *text, gboolean remember)
{
	queue_line_full(self, text, remember, TRUE);
}
static void
clear_progress(AiVoiceSession *self)
{
	if (self->progress == NULL)
		return;
	g_source_destroy(self->progress);
	g_clear_pointer(&self->progress, g_source_unref);
}
static void
pump(AiVoiceSession *self);
/* Fires only if the tool is still running and nothing has been said: a
 * quick lookup, or a model that already said "let me look", hears nothing. */
static gboolean
progress_fire(gpointer data)
{
	AiVoiceSession *self = data;
	g_clear_pointer(&self->progress, g_source_unref);
	if (self->stopped || !self->provider_pending ||
		self->progress_generation != self->generation || self->turn_lines != 0)
		return G_SOURCE_REMOVE;
	/* Not counted: it is not a reply, so an empty turn still says so. */
	queue_line_full(self, self->tool_progress_message, FALSE, FALSE);
	pump(self);
	return G_SOURCE_REMOVE;
}
static void
tool_started(AiVoiceSession *self)
{
	if (self->progress_used || self->tool_progress_message == NULL ||
		*self->tool_progress_message == '\0' || self->turn_lines != 0)
		return;
	self->progress_used = TRUE;
	self->progress_generation = self->generation;
	self->progress = g_timeout_source_new(self->tool_progress_delay_ms);
	g_source_set_callback(self->progress, progress_fire, self, NULL);
	g_source_attach(self->progress, self->context);
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
	if (self->hold == NULL)
		return;
	g_source_destroy(self->hold);
	g_clear_pointer(&self->hold, g_source_unref);
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
	g_clear_pointer(&self->hold, g_source_unref);
	if (self->stopped)
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
	const gchar *end = self->pending_text->str + self->pending_text->len;
	gboolean waiting = FALSE;
	while (end > self->pending_text->str && closing_len(end - 1, 1) != 0)
		end--;
	while (end > self->pending_text->str && terminator(end[-1])) {
		if (end[-1] == '.')
			waiting = TRUE;
		end--;
	}
	if (!waiting) {
		clear_hold(self);
		return;
	}
	if (self->hold != NULL)
		return;
	self->hold = g_timeout_source_new(PERIOD_HOLD_MS);
	g_source_set_callback(self->hold, hold_fire, self, NULL);
	g_source_attach(self->hold, self->context);
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
		  "Reply repeated a line more than %u times; stopping it", self->repeat_limit);
	self->runaway = TRUE;
	g_string_truncate(self->pending_text, 0);
	clear_hold(self);
	clear_progress(self);
	if (self->turn_cancel != NULL)
		g_cancellable_cancel(self->turn_cancel);
	if (self->repeat_message != NULL && *self->repeat_message != '\0')
		queue_line(self, self->repeat_message, FALSE);
}
static void
offer_line(AiVoiceSession *self, const gchar *text)
{
	if (self->runaway)
		return;
	if (self->repeat_limit > 0) {
		g_autofree gchar *key = repeat_key(text);
		if (*key != '\0') {
			guint seen = GPOINTER_TO_UINT(g_hash_table_lookup(self->repeats, key)) + 1;
			g_hash_table_insert(self->repeats, g_steal_pointer(&key), GUINT_TO_POINTER(seen));
			if (seen > self->repeat_limit) {
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
	while (!self->runaway && (end = sentence_end(self->pending_text->str,
												 self->pending_text->len, final)) != 0) {
		g_autofree gchar *text = g_strndup(self->pending_text->str, end);
		g_string_erase(self->pending_text, 0, end);
		offer_line(self, text);
	}
	if (final && self->pending_text->len != 0) {
		g_autofree gchar *text = g_strdup(self->pending_text->str);
		g_string_truncate(self->pending_text, 0);
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
	for (p = self->span->str; *p != '\0'; p++)
		if (g_ascii_isspace(*p))
			break;
	if (*p == '\0')
		g_string_append_len(self->pending_text, self->span->str, self->span->len);
	g_string_truncate(self->span, 0);
	self->in_inline = FALSE;
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
	if (self->speak_code) {
		g_string_append(self->pending_text, text);
		return;
	}
	for (p = text; *p != '\0'; p++) {
		if (*p == '`') {
			self->ticks++;
			continue;
		}
		if (self->ticks >= 3) {
			self->in_fence = !self->in_fence;
			self->in_inline = FALSE;
			g_string_truncate(self->span, 0);
			g_string_append_c(self->pending_text, '\n');
		} else if (self->ticks > 0 && !self->in_fence) {
			if (self->in_inline)
				close_span(self);
			else
				self->in_inline = TRUE;
		}
		self->ticks = 0;
		if (self->in_fence)
			continue;
		if (self->in_inline) {
			/* An unclosed span ends with its line rather than eating the reply. */
			if (*p == '\n') {
				close_span(self);
				g_string_append_c(self->pending_text, '\n');
			} else
				g_string_append_c(self->span, *p);
			continue;
		}
		g_string_append_c(self->pending_text, *p);
	}
	if (final) {
		if (self->in_inline)
			close_span(self);
		self->ticks = 0;
	}
}
static void
speech_complete(Speech *s)
{
	AiVoiceSession *self = s->session;
	if (!s->synthesized || s->pending != 0)
		return;
	self->speech = NULL;
	/* Only speech that reached the caller was said; cut short is still said. */
	if (s->heard)
		g_signal_emit_by_name(self, "spoken", s->text, !s->discarded);
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
	ai_audio_transport_write_pcm_async(self->transport, pcm, sample_rate, s->cancel,
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
		if (!s->fallback && !s->session->stopped &&
			s->generation == s->session->generation &&
			!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
			/* A failed notice is not a barge-in: do not retry it indefinitely. */
			s->notice = FALSE;
			interrupt_turn(s->session);
			if (s->session->synthesis_error_message != NULL &&
			    *s->session->synthesis_error_message != '\0') {
				Line *fallback = g_new0(Line, 1);
				fallback->text = g_strdup(s->session->synthesis_error_message);
				fallback->fallback = TRUE;
				g_queue_push_head(&s->session->lines, fallback);
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
	g_clear_pointer(&self->deadline, g_source_unref);
	interrupt_turn(self);
	queue_line(self, self->deadline_message, FALSE);
	pump(self);
	return G_SOURCE_REMOVE;
}
static void
start_turn(AiVoiceSession *self, const gchar *text)
{
	self->last_turn_us = g_get_monotonic_time();
	self->generation++;
	self->provider_generation = self->generation;
	self->provider_pending = TRUE;
	g_clear_object(&self->turn_cancel);
	self->turn_cancel = g_cancellable_new();
	g_string_truncate(self->spoken, 0);
	self->turn_lines = 0;
	self->progress_used = FALSE;
	self->in_fence = self->in_inline = self->runaway = FALSE;
	self->ticks = 0;
	g_string_truncate(self->span, 0);
	g_hash_table_remove_all(self->repeats);
	clear_progress(self);
	clear_hold(self);
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
	if (self->stopped || self->media_recovering || self->speech != NULL)
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
		s->fallback = line->fallback;
		s->cancel = g_cancellable_new();
		s->generation = self->generation;
		self->speech = s;
		line_free(line);
		state(self, AI_VOICE_SPEAKING);
		remember_said(self, s->text);
		g_signal_emit_by_name(self, "reply", s->text);
		if (s->fallback && self->fallback_pcm != NULL) {
			s->synthesized = TRUE;
			audio_out(self->synthesizer, self->fallback_pcm, self->fallback_sample_rate,
					  self);
			speech_complete(s);
		} else {
			ai_speech_synthesizer_synthesize_async(self->synthesizer, s->text, s->cancel,
												   synthesized, s);
		}
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
	clear_progress(self);
	clear_hold(self);
	self->paused_deadline_us = 0;
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
	if (self->stopped)
		return;
	if (kind == AI_VOICE_MAIL_EVENT && event != NULL &&
		ai_event_get_kind(event) == AI_EVENT_TOOL_FINISHED)
		report_tool(self, event);
	if (kind == AI_VOICE_MAIL_AGENT) {
		/* A finished agent becomes a turn for the model, queued behind any turn
		 * under way, so it fetches the result and says it unprompted. */
		if (!g_hash_table_contains(self->notices, text)) {
			g_hash_table_add(self->notices, g_strdup(text));
			g_queue_push_tail(&self->turns, g_strdup(text));
		}
	} else if (kind == AI_VOICE_MAIL_DONE) {
		if (generation == self->provider_generation) {
			clear_progress(self);
			clear_hold(self);
			self->provider_pending = FALSE;
			clear_deadline(self);
			self->paused_deadline_us = 0;
		}
		if (generation == self->generation && self->runaway) {
			/* Stopped on purpose; the cancellation is not an error to speak. */
		} else if (generation == self->generation) {
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
				if (self->turn_lines == 0) {
					g_log("ai-glib", G_LOG_LEVEL_INFO,
						  "Provider turn ended with nothing to say; speaking the "
						  "empty-reply message");
					queue_line(self, self->empty_reply_message, FALSE);
				}
			}
		}
	} else if (generation == self->generation) {
		AiEventKind k = ai_event_get_kind(event);
		if (k == AI_EVENT_TOOL_STARTED) {
			/* A tool that never finishes never reaches the transcript: log its
			 * start, once per call, so a stuck turn names what it is stuck in. */
			AiToolUse *use = ai_event_get_tool_use(event);
			const gchar *id = use != NULL ? ai_tool_use_get_id(use) : NULL;
			if (use != NULL && g_strcmp0(id, self->logged_tool) != 0) {
				g_free(self->logged_tool);
				self->logged_tool = g_strdup(id);
				g_log("ai-glib", G_LOG_LEVEL_INFO, "Tool started: %s",
					  ai_tool_use_get_name(use) != NULL ? ai_tool_use_get_name(use) : "?");
			}
			tool_started(self);
		}
		else if (k == AI_EVENT_TEXT_DELTA) {
			if (self->runaway)
				;
			else if (self->pending_text->len < 65536)
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
	if (self->stopped || g_hash_table_contains(self->participants, speaker))
		return;
	p = g_new0(Participant, 1);
	p->name = g_strdup(name != NULL && *name != '\0' ? name : speaker);
	p->frames = g_byte_array_new();
	p->onset = g_byte_array_new();
	p->preroll = g_byte_array_new();
	p->utterance = g_byte_array_new();
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
		gboolean fed_onset = FALSE;
		g_byte_array_remove_range(p->frames, 0, 320);
		activity = ai_voice_activity_process(self->activity, speaker, frame, &error);
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
			if (p->speech_ms >= self->barge_in_ms) {
				/* While we are talking, sound alone may be our own voice
				 * returning through the caller's speaker. Listen, and let
				 * words decide whether this is an interruption. */
				/* Muted, the caller is not heard, so cannot interrupt: only
				 * the words "unmute" or "hang up" reach us, as commands. */
				if (self->muted) {
				} else if (self->state == AI_VOICE_SPEAKING && self->barge_in_confirm) {
					if (!p->pending_barge)
						g_log("ai-glib", G_LOG_LEVEL_INFO,
							  "possible barge-in by %s after %u ms of speech, waiting for words",
							  p->name, p->speech_ms);
					p->pending_barge = TRUE;
				} else if (self->state == AI_VOICE_SPEAKING ||
						   self->state == AI_VOICE_THINKING) {
					g_log("ai-glib", G_LOG_LEVEL_INFO,
						  "barge-in by %s after %u ms of speech, cancelling %s", p->name,
						  p->speech_ms,
						  self->state == AI_VOICE_SPEAKING ? "speaking" : "thinking");
					interrupt_turn(self);
				}
				if (p->ended) {
					ai_speech_recognizer_cancel(self->recognizer, speaker);
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
						ai_speech_recognizer_begin(self->recognizer, speaker, &error);
					if (self->stopped ||
						g_hash_table_lookup(self->participants, speaker) != p)
						return;
					p->recognizing = recognizing;
					/* Preserve the confirmed onset rather than clipping initial words.
					 * Feed ordinary 10 ms frames, independent of backend size limits. */
					for (offset = 0; p->recognizing && error == NULL && offset < size;
						 offset += 320) {
						g_autoptr(GBytes) buffered = g_bytes_new(samples + offset, 320);
						ai_speech_recognizer_feed(self->recognizer, speaker, buffered,
												  &error);
						if (self->stopped ||
							g_hash_table_lookup(self->participants, speaker) != p)
							return;
					}
					fed_onset = TRUE;
				}
				if (!p->pending_barge)
					state(self, AI_VOICE_TRANSCRIBING);
				if (self->stopped ||
					g_hash_table_lookup(self->participants, speaker) != p)
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
			ai_speech_recognizer_feed(self->recognizer, speaker, frame, &error);
		}
		if (self->stopped || g_hash_table_lookup(self->participants, speaker) != p)
			return;
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
		for (i = 0; self->assistant_names != NULL && self->assistant_names[i] != NULL; i++) {
			g_autofree gchar *name = command_words(self->assistant_names[i]);
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
set_muted(AiVoiceSession *self, gboolean muted)
{
	if (self->muted == muted)
		return;
	self->muted = muted;
	g_object_notify(G_OBJECT(self), "muted");
}
static void
run_command(AiVoiceSession *self, const gchar *name)
{
	g_log("ai-glib", G_LOG_LEVEL_INFO, "Voice command: %s", name);
	g_signal_emit_by_name(self, "command", name);
	if (self->stopped)
		return;
	if (g_str_equal(name, "stop") || g_str_equal(name, "mute")) {
		/* A cut-off notice goes back in the queue after a barge-in, so the
		 * caller's turn goes first. After "stop" there is no turn: it would
		 * be replayed at once. */
		if (self->speech != NULL)
			self->speech->notice = FALSE;
		interrupt_turn(self);
		g_queue_clear_full(&self->turns, g_free);
	}
	if (g_str_equal(name, "mute")) {
		set_muted(self, TRUE);
		if (self->mute_message != NULL && *self->mute_message != '\0')
			queue_line(self, self->mute_message, FALSE);
	} else if (g_str_equal(name, "unmute")) {
		set_muted(self, FALSE);
		if (self->unmute_message != NULL && *self->unmute_message != '\0')
			queue_line(self, self->unmute_message, FALSE);
	}
}
/* ---- room mode ----
 *
 * With several people in a room, most of what is said is not for the
 * assistant. It is kept, and handed to the model with the next turn so a
 * reply knows what was said; a turn happens when someone names the assistant,
 * or when a relevance score of what was said clears chime-in-threshold. */
#define ROOM_LINES 40
static const gchar relevance_prompt[] =
	"You are listening to a live conversation between people in a room. An AI "
	"assistant named %s is present and hears everything, but nobody addressed it. "
	"Decide how much the assistant should speak up right now, from 0 (stay quiet) "
	"to 10 (clearly should speak). Lines marked [Assistant] are what the "
	"assistant itself said last; talk that follows up on them -- a next question, "
	"a correction, another sum -- is usually meant for it.\n"
	"Score high when: a question was asked that nobody answered; someone states "
	"something that is wrong; the group is stuck or looking for information the "
	"assistant is likely to have; someone seems to be waiting for the assistant.\n"
	"Score low when: people are chatting, joking or talking about personal "
	"matters; the conversation is going fine without help; the point is already "
	"settled.\n"
	"Answer with exactly two lines and nothing else:\n"
	"SCORE: <0-10>\n"
	"REASON: <one short sentence>";
/* A sound-alike in a name's place: after a greeting ("Hi, say."), before a
 * comma at the start ("Say, could you..."), after a comma at the end ("What
 * do you think, say?"). Anywhere else it is the ordinary word. */
static gboolean
soundalike_addressed(AiVoiceSession *self, const gchar *text)
{
	guint i;
	for (i = 0; self->soundalikes != NULL && self->soundalikes[i] != NULL; i++) {
		g_autofree gchar *word = g_regex_escape_string(self->soundalikes[i], -1);
		g_autofree gchar *pattern = NULL;
		if (*word == '\0')
			continue;
		pattern = g_strdup_printf("^\\W*(?:hi|hey|hello|ok|okay|yo)\\W+%s\\s*(?:[,.!?]|$)"
								  "|^\\W*%s\\s*,"
								  "|,\\s*%s\\W*$",
								  word, word, word);
		if (g_regex_match_simple(pattern, text, G_REGEX_CASELESS, 0))
			return TRUE;
	}
	return FALSE;
}
/* Named anywhere in the utterance, as whole words: "Ada, what do you think?"
 * and "what does Ada think?" both are. With no names, everything is. */
static gboolean
addressed(AiVoiceSession *self, const gchar *text)
{
	g_autofree gchar *words = NULL, *padded = NULL;
	guint i;
	if (self->assistant_names == NULL || self->assistant_names[0] == NULL)
		return TRUE;
	if (soundalike_addressed(self, text))
		return TRUE;
	words = command_words(text);
	padded = g_strconcat(" ", words, " ", NULL);
	for (i = 0; self->assistant_names[i] != NULL; i++) {
		g_autofree gchar *name = command_words(self->assistant_names[i]);
		g_autofree gchar *needle = g_strconcat(" ", name, " ", NULL);
		if (*name != '\0' && strstr(padded, needle) != NULL)
			return TRUE;
	}
	return FALSE;
}
/* What was said in the room, then LINE, as one turn; the room starts over. */
static gchar *
room_turn(AiVoiceSession *self, const gchar *line)
{
	GString *turn = g_string_new(NULL);
	gchar *heard;
	while ((heard = g_queue_pop_head(&self->room)) != NULL) {
		g_string_append(turn, heard);
		g_string_append_c(turn, '\n');
		g_free(heard);
	}
	g_string_append(turn, line);
	return g_string_free(turn, FALSE);
}
static void
cancel_scoring(AiVoiceSession *self)
{
	if (self->scoring != NULL)
		g_cancellable_cancel(self->scoring);
	g_clear_object(&self->scoring);
}
typedef struct {
	AiVoiceSession *session;
	GCancellable *cancel;
	GList *messages;
	guint64 revision;
} Scoring;
static void
scoring_free(Scoring *sc)
{
	g_object_unref(sc->session);
	g_object_unref(sc->cancel);
	g_list_free_full(sc->messages, g_object_unref);
	g_free(sc);
}
/* "SCORE: 7" anywhere, then "REASON: ..." to the end of its line. Anything
 * unreadable is a zero: a judge that cannot answer is not a reason to speak. */
static guint
parse_relevance(const gchar *text, gchar **reason)
{
	g_autofree gchar *lower = g_ascii_strdown(text != NULL ? text : "", -1);
	const gchar *at = strstr(lower, "score"), *why = strstr(lower, "reason:");
	guint score = 0;
	*reason = g_strdup("");
	if (at == NULL)
		return 0;
	for (at += 5; *at == ':' || *at == ' ' || *at == '*'; at++)
		;
	if (!g_ascii_isdigit(*at))
		return 0;
	score = (guint)MIN(g_ascii_strtoull(at, NULL, 10), 10);
	if (why != NULL) {
		const gchar *start = text + (why - lower) + 7;
		const gchar *end = strchr(start, '\n');
		g_free(*reason);
		*reason = g_strstrip(end != NULL ? g_strndup(start, end - start) : g_strdup(start));
	}
	return score;
}
static gboolean
recognizing(AiVoiceSession *self);
static void
scored(GObject *source, GAsyncResult *result, gpointer data)
{
	Scoring *sc = data;
	AiVoiceSession *self = sc->session;
	g_autoptr(GError) error = NULL;
	g_autoptr(AiResponse) response =
		ai_provider_chat_finish(AI_PROVIDER(source), result, &error);
	g_autofree gchar *text = NULL, *reason = NULL;
	guint score;
	gboolean chime;
	if (self->scoring == sc->cancel)
		g_clear_object(&self->scoring);
	/* Stale: someone spoke since, and that talk has its own score coming. */
	if (self->stopped || g_cancellable_is_cancelled(sc->cancel) ||
		sc->revision != self->room_revision) {
		scoring_free(sc);
		return;
	}
	if (response == NULL) {
		g_debug("voice: relevance not scored: %s", error->message);
		scoring_free(sc);
		return;
	}
	text = ai_response_get_text(response);
	score = parse_relevance(text, &reason);
	/* Never over anyone, and never on top of a turn already under way. */
	chime = score >= self->chime_threshold && !self->provider_pending &&
			self->speech == NULL && g_queue_is_empty(&self->turns) &&
			!recognizing(self) && !g_queue_is_empty(&self->room);
	g_log("ai-glib", G_LOG_LEVEL_INFO, "Relevance %u/10 (threshold %u)%s: %s", score,
		  self->chime_threshold, chime ? ", chiming in" : "", reason);
	g_signal_emit_by_name(self, "relevance", score, reason, chime);
	if (chime && !self->stopped) {
		g_autofree gchar *note = g_strdup_printf(
			"[Note]: Nobody addressed you. You are chiming in because: %s. Say it "
			"in one or two sentences, to whoever it is for.",
			*reason != '\0' ? reason : "it seemed useful");
		g_queue_push_tail(&self->turns, room_turn(self, note));
		pump(self);
	}
	scoring_free(sc);
}
/* Score what was said since the assistant last spoke, superseding any score
 * still on its way. Nothing is scored in the cooldown after a turn. */
static void
maybe_score(AiVoiceSession *self)
{
	GString *talk = g_string_new(NULL);
	g_autofree gchar *system = NULL;
	Scoring *sc;
	GList *l;
	cancel_scoring(self);
	if (self->relevance_provider == NULL || self->chime_threshold == 0 || self->stopped ||
		self->muted || g_queue_is_empty(&self->room) ||
		(self->last_turn_us != 0 &&
		 g_get_monotonic_time() - self->last_turn_us < (gint64)self->chime_cooldown_ms * 1000)) {
		g_string_free(talk, TRUE);
		return;
	}
	/* What the assistant said last, so a follow-up to it reads as one. */
	for (l = self->said.head; l != NULL; l = l->next) {
		const Said *said = l->data;
		if (said->at >= self->last_turn_us &&
			g_get_monotonic_time() - said->at < 180 * G_USEC_PER_SEC)
			g_string_append_printf(talk, "[Assistant]: %s\n", said->text);
	}
	for (l = self->room.head; l != NULL; l = l->next) {
		g_string_append(talk, l->data);
		g_string_append_c(talk, '\n');
	}
	system = g_strdup_printf(relevance_prompt,
							 self->assistant_names != NULL && self->assistant_names[0] != NULL
								 ? self->assistant_names[0]
								 : "the assistant");
	sc = g_new0(Scoring, 1);
	sc->session = g_object_ref(self);
	sc->cancel = g_cancellable_new();
	sc->revision = self->room_revision;
	sc->messages = g_list_append(NULL, ai_message_new_user(talk->str));
	g_string_free(talk, TRUE);
	self->scoring = g_object_ref(sc->cancel);
	ai_provider_chat_async(AI_PROVIDER(self->relevance_provider), sc->messages, system, 200,
						   NULL, sc->cancel, scored, sc);
}
static void
hear_room(AiVoiceSession *self, const gchar *name, const gchar *text)
{
	g_queue_push_tail(&self->room, g_strdup_printf("[%s]: %s", name, text));
	while (g_queue_get_length(&self->room) > ROOM_LINES)
		g_free(g_queue_pop_head(&self->room));
	self->room_revision++;
	maybe_score(self);
}
static void
confirm_barge(AiVoiceSession *self, Participant *p)
{
	g_log("ai-glib", G_LOG_LEVEL_INFO, "barge-in by %s confirmed by speech, cancelling %s",
		  p->name, self->state == AI_VOICE_SPEAKING ? "speaking" : "thinking");
	p->pending_barge = FALSE;
	interrupt_turn(self);
	state(self, AI_VOICE_TRANSCRIBING);
}
/* A final transcript, attributed: a turn, or in room mode perhaps only heard. */
static void
deliver(AiVoiceSession *self, const gchar *name, const gchar *text)
{
	if (self->room_mode && *text != '\0' && !addressed(self, text)) {
		g_signal_emit_by_name(self, "transcript", name, text, TRUE);
		hear_room(self, name, text);
		pump(self);
		return;
	}
	if (*text != '\0' && g_queue_get_length(&self->turns) < 32) {
		g_autofree gchar *line = g_strdup_printf("[%s]: %s", name, text);
		cancel_scoring(self);
		self->room_revision++;
		g_queue_push_tail(&self->turns, room_turn(self, line));
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
typedef struct {
	AiVoiceSession *session;
	gchar *speaker, *text, *name;
	gboolean done, too_short, failed;
	gdouble score;
} Naming;
static void
naming_free(gpointer data)
{
	Naming *n = data;
	g_object_unref(n->session);
	g_free(n->speaker);
	g_free(n->text);
	g_free(n->name);
	g_free(n);
}
/* Names are settled here, in the order things were said, so "too short to
 * tell" means whoever spoke just before -- not whoever was named first. */
static void
deliver_named(AiVoiceSession *self)
{
	Naming *n;
	while ((n = g_queue_peek_head(&self->naming)) != NULL && n->done) {
		Participant *p = g_hash_table_lookup(self->participants, n->speaker);
		gint64 now = g_get_monotonic_time();
		const gchar *name;
		g_queue_pop_head(&self->naming);
		if (n->name != NULL) {
			name = n->name;
			if (p != NULL) {
				g_free(p->last_named);
				p->last_named = g_strdup(n->name);
				p->last_named_us = now;
			}
		} else if (n->too_short && p != NULL && p->last_named != NULL &&
				   now - p->last_named_us < NAME_MEMORY_US)
			name = p->last_named;
		else if (!n->failed && !n->too_short && self->unknown_speaker_name != NULL)
			name = self->unknown_speaker_name;
		else
			name = p != NULL ? p->name : n->speaker;
		g_log("ai-glib", G_LOG_LEVEL_INFO, "Speaker: %s (%.2f)", name, n->score);
		if (!self->stopped)
			deliver(self, name, n->text);
		naming_free(n);
	}
}
static void
named(GObject *source, GAsyncResult *result, gpointer data)
{
	Naming *n = data;
	AiVoiceSession *self = n->session;
	g_autoptr(GError) error = NULL;
	n->name = ai_speaker_identifier_identify_finish(AI_SPEAKER_IDENTIFIER(source), result,
													&n->score, &error);
	n->too_short = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
	/* The service down or refusing: the participant, not "a stranger". */
	n->failed = error != NULL && !n->too_short;
	if (n->failed)
		g_debug("voice: speaker not identified: %s", error->message);
	n->done = TRUE;
	deliver_named(self);
}
static void
name_then_deliver(AiVoiceSession *self, const gchar *speaker, Participant *p,
				  const gchar *text)
{
	Naming *n = g_new0(Naming, 1);
	g_autoptr(GBytes) pcm = g_bytes_new(p->utterance->data, p->utterance->len);
	g_byte_array_set_size(p->utterance, 0);
	n->session = g_object_ref(self);
	n->speaker = g_strdup(speaker);
	n->text = g_strdup(text);
	g_queue_push_tail(&self->naming, n);
	if (self->naming_cancel == NULL)
		self->naming_cancel = g_cancellable_new();
	ai_speaker_identifier_identify_async(AI_SPEAKER_IDENTIFIER(self->identifier), pcm,
										 self->naming_cancel, named, n);
}

static void
transcript(AiSpeechRecognizer *recognizer, const gchar *speaker, const gchar *text,
		   gboolean final, gpointer data)
{
	AiVoiceSession *self = data;
	Participant *p = g_hash_table_lookup(self->participants, speaker);
	g_autofree gchar *trimmed = NULL;
	gboolean pending;
	if (self->stopped || p == NULL)
		return;
	if (!final && self->muted)
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
	ai_voice_activity_reset(self->activity, speaker);
	if (*text != '\0' && (pending || self->state == AI_VOICE_SPEAKING) &&
		is_echo(self, text)) {
		g_log("ai-glib", G_LOG_LEVEL_INFO,
			  "ignoring transcript from %s: it repeats our own recent speech", p->name);
		pump(self);
		return;
	}
	if (pending && has_words(text) && !self->muted)
		confirm_barge(self, p);
	if (self->voice_commands && has_words(text)) {
		const gchar *command = match_command(self, text);
		/* Muted, only a way back is heard: nothing else reaches the model or
		 * is reported as said. */
		if (command != NULL &&
			(!self->muted || g_str_equal(command, "unmute") || g_str_equal(command, "hangup"))) {
			run_command(self, command);
			pump(self);
			return;
		}
	}
	if (self->muted) {
		g_debug("voice: muted; not hearing %s", p->name);
		pump(self);
		return;
	}
	if (self->identifier != NULL && *text != '\0') {
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
	Participant *p = g_hash_table_lookup(self->participants, speaker);
	if (self->stopped)
		return;
	ai_voice_activity_reset(self->activity, speaker);
	if (p != NULL) {
		p->recognizing = FALSE;
		p->ended = FALSE;
		p->pending_barge = FALSE;
		p->speech_ms = 0;
		g_byte_array_set_size(p->onset, 0);
	}
	report(self, error);
	queue_line(self, self->transcription_error_message, FALSE);
	pump(self);
}
static void
transport_reconnecting(AiAudioTransport *transport, gpointer data)
{
	AiVoiceSession *self = data;
	if (self->stopped || self->media_recovering)
		return;
	self->media_recovering = TRUE;
	if (self->deadline != NULL) {
		self->paused_deadline_us = MAX(
			(gint64)1, g_source_get_ready_time(self->deadline) - g_get_monotonic_time());
		clear_deadline(self);
	}
}
static void
transport_reconnected(AiAudioTransport *transport, gpointer data)
{
	AiVoiceSession *self = data;
	self->media_recovering = FALSE;
	if (!self->stopped && self->provider_pending && self->paused_deadline_us > 0) {
		self->deadline =
			g_timeout_source_new((guint)((self->paused_deadline_us + 999) / 1000));
		g_source_set_callback(self->deadline, deadline, self, NULL);
		g_source_attach(self->deadline, self->context);
	}
	self->paused_deadline_us = 0;
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
	self->media_recovering = TRUE;
	cancel_scoring(self);
	if (self->naming_cancel != NULL)
		g_cancellable_cancel(self->naming_cancel);
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
	g_clear_object(&self->relevance_provider);
	g_clear_object(&self->identifier);
	g_clear_object(&self->naming_cancel);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->dispose(object);
}
static void
finalize(GObject *object)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	g_hash_table_unref(self->participants);
	g_hash_table_unref(self->notices);
	g_hash_table_unref(self->repeats);
	g_string_free(self->span, TRUE);
	g_strfreev(self->assistant_names);
	g_strfreev(self->soundalikes);
	g_queue_clear_full(&self->room, g_free);
	g_free(self->unknown_speaker_name);
	g_free(self->logged_tool);
	g_free(self->mute_message);
	g_free(self->unmute_message);
	g_free(self->repeat_message);
	g_main_context_unref(self->context);
	g_queue_clear_full(&self->lines, line_free);
	g_queue_clear_full(&self->turns, g_free);
	g_queue_clear_full(&self->pending_notices, line_free);
	g_queue_clear_full(&self->said, said_free);
	g_string_free(self->pending_text, TRUE);
	g_string_free(self->spoken, TRUE);
	g_free(self->deadline_message);
	g_free(self->transcription_error_message);
	g_free(self->synthesis_error_message);
	g_free(self->empty_reply_message);
	g_free(self->tool_progress_message);
	g_clear_pointer(&self->fallback_pcm, g_bytes_unref);
	G_OBJECT_CLASS(ai_voice_session_parent_class)->finalize(object);
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiVoiceSession *self = AI_VOICE_SESSION(object);
	if (id == PROP_TURN_DEADLINE)
		g_value_set_uint(value, self->deadline_ms);
	else if (id == PROP_BARGE_IN)
		g_value_set_uint(value, self->barge_in_ms);
	else if (id == PROP_BARGE_IN_CONFIRM)
		g_value_set_boolean(value, self->barge_in_confirm);
	else if (id == PROP_STATE)
		g_value_set_enum(value, self->state);
	else if (id == PROP_TRANSPORT)
		g_value_set_object(value, self->transport);
	else if (id == PROP_RECOGNIZER)
		g_value_set_object(value, self->recognizer);
	else if (id == PROP_SYNTHESIZER)
		g_value_set_object(value, self->synthesizer);
	else if (id == PROP_SYNTHESIS_ERROR_MESSAGE)
		g_value_set_string(value, self->synthesis_error_message);
	else if (id == PROP_EMPTY_REPLY_MESSAGE)
		g_value_set_string(value, self->empty_reply_message);
	else if (id == PROP_TOOL_PROGRESS_MESSAGE)
		g_value_set_string(value, self->tool_progress_message);
	else if (id == PROP_TOOL_PROGRESS_DELAY)
		g_value_set_uint(value, self->tool_progress_delay_ms);
	else if (id == PROP_TRIM_AFTER)
		g_value_set_uint(value, self->trim_after);
	else if (id == PROP_SPEAK_CODE)
		g_value_set_boolean(value, self->speak_code);
	else if (id == PROP_REPEAT_LIMIT)
		g_value_set_uint(value, self->repeat_limit);
	else if (id == PROP_REPEAT_MESSAGE)
		g_value_set_string(value, self->repeat_message);
	else if (id == PROP_VOICE_COMMANDS)
		g_value_set_boolean(value, self->voice_commands);
	else if (id == PROP_ASSISTANT_NAMES) {
		g_autofree gchar *joined =
			self->assistant_names != NULL ? g_strjoinv(",", self->assistant_names) : NULL;
		g_value_set_string(value, joined);
	} else if (id == PROP_MUTED)
		g_value_set_boolean(value, self->muted);
	else if (id == PROP_MUTE_MESSAGE)
		g_value_set_string(value, self->mute_message);
	else if (id == PROP_UNMUTE_MESSAGE)
		g_value_set_string(value, self->unmute_message);
	else if (id == PROP_ROOM_MODE)
		g_value_set_boolean(value, self->room_mode);
	else if (id == PROP_RELEVANCE_PROVIDER)
		g_value_set_object(value, self->relevance_provider);
	else if (id == PROP_CHIME_IN_THRESHOLD)
		g_value_set_uint(value, self->chime_threshold);
	else if (id == PROP_CHIME_IN_COOLDOWN)
		g_value_set_uint(value, self->chime_cooldown_ms);
	else if (id == PROP_SPEAKER_IDENTIFIER)
		g_value_set_object(value, self->identifier);
	else if (id == PROP_UNKNOWN_SPEAKER_NAME)
		g_value_set_string(value, self->unknown_speaker_name);
	else if (id == PROP_ASSISTANT_SOUNDALIKES) {
		g_autofree gchar *joined =
			self->soundalikes != NULL ? g_strjoinv(",", self->soundalikes) : NULL;
		g_value_set_string(value, joined);
	}
	else if (id == PROP_FALLBACK_PCM)
		g_value_set_boxed(value, self->fallback_pcm);
	else if (id == PROP_FALLBACK_SAMPLE_RATE)
		g_value_set_uint(value, self->fallback_sample_rate);
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
	else if (id == PROP_BARGE_IN)
		self->barge_in_ms = g_value_get_uint(value);
	else if (id == PROP_BARGE_IN_CONFIRM)
		self->barge_in_confirm = g_value_get_boolean(value);
	else if (id == PROP_TRANSPORT)
		g_set_object(&self->transport, g_value_get_object(value));
	else if (id == PROP_RECOGNIZER)
		g_set_object(&self->recognizer, g_value_get_object(value));
	else if (id == PROP_SYNTHESIZER)
		g_set_object(&self->synthesizer, g_value_get_object(value));
	else if (id == PROP_FALLBACK_PCM) {
		GBytes *pcm = g_value_get_boxed(value);
		g_return_if_fail(pcm == NULL || (g_bytes_get_size(pcm) > 0 &&
										 g_bytes_get_size(pcm) <= 4 * 1024 * 1024 &&
										 g_bytes_get_size(pcm) % 2 == 0));
		g_clear_pointer(&self->fallback_pcm, g_bytes_unref);
		self->fallback_pcm = pcm != NULL ? g_bytes_ref(pcm) : NULL;
	} else if (id == PROP_FALLBACK_SAMPLE_RATE)
		self->fallback_sample_rate = g_value_get_uint(value);
	else if (id == PROP_TOOL_PROGRESS_MESSAGE) {
		g_free(self->tool_progress_message);
		self->tool_progress_message = g_value_dup_string(value);
	} else if (id == PROP_TOOL_PROGRESS_DELAY)
		self->tool_progress_delay_ms = g_value_get_uint(value);
	else if (id == PROP_SPEAK_CODE)
		self->speak_code = g_value_get_boolean(value);
	else if (id == PROP_REPEAT_LIMIT)
		self->repeat_limit = g_value_get_uint(value);
	else if (id == PROP_REPEAT_MESSAGE) {
		g_free(self->repeat_message);
		self->repeat_message = g_value_dup_string(value);
	} else if (id == PROP_VOICE_COMMANDS)
		self->voice_commands = g_value_get_boolean(value);
	else if (id == PROP_ASSISTANT_NAMES) {
		const gchar *names = g_value_get_string(value);
		g_clear_pointer(&self->assistant_names, g_strfreev);
		if (names != NULL && *names != '\0') {
			guint i;
			self->assistant_names = g_strsplit(names, ",", -1);
			for (i = 0; self->assistant_names[i] != NULL; i++)
				g_strstrip(self->assistant_names[i]);
		}
	} else if (id == PROP_MUTED)
		set_muted(self, g_value_get_boolean(value));
	else if (id == PROP_MUTE_MESSAGE) {
		g_free(self->mute_message);
		self->mute_message = g_value_dup_string(value);
	} else if (id == PROP_UNMUTE_MESSAGE) {
		g_free(self->unmute_message);
		self->unmute_message = g_value_dup_string(value);
	} else if (id == PROP_ROOM_MODE)
		self->room_mode = g_value_get_boolean(value);
	else if (id == PROP_RELEVANCE_PROVIDER) {
		GObject *provider = g_value_get_object(value);
		cancel_scoring(self);
		if (provider != NULL && !AI_IS_PROVIDER(provider)) {
			g_message("relevance-provider: a %s is not an AiProvider; ignored",
					  G_OBJECT_TYPE_NAME(provider));
			provider = NULL;
		}
		g_set_object(&self->relevance_provider, provider);
	} else if (id == PROP_CHIME_IN_THRESHOLD)
		self->chime_threshold = g_value_get_uint(value);
	else if (id == PROP_CHIME_IN_COOLDOWN)
		self->chime_cooldown_ms = g_value_get_uint(value);
	else if (id == PROP_SPEAKER_IDENTIFIER)
		g_set_object(&self->identifier, g_value_get_object(value));
	else if (id == PROP_ASSISTANT_SOUNDALIKES) {
		const gchar *words = g_value_get_string(value);
		g_clear_pointer(&self->soundalikes, g_strfreev);
		if (words != NULL && *words != '\0') {
			guint i;
			self->soundalikes = g_strsplit(words, ",", -1);
			for (i = 0; self->soundalikes[i] != NULL; i++)
				g_strstrip(self->soundalikes[i]);
		}
	} else if (id == PROP_UNKNOWN_SPEAKER_NAME) {
		const gchar *name = g_value_get_string(value);
		g_free(self->unknown_speaker_name);
		self->unknown_speaker_name = name != NULL && *name != '\0' ? g_strdup(name) : NULL;
	} else if (id == PROP_TRIM_AFTER) {
		self->trim_after = g_value_get_uint(value);
		if (self->worker != NULL)
			ai_voice_worker_set_trim_after(self->worker, self->trim_after);
	}
	else if (id == PROP_EMPTY_REPLY_MESSAGE) {
		g_free(self->empty_reply_message);
		self->empty_reply_message = g_value_dup_string(value);
	} else if (id == PROP_SYNTHESIS_ERROR_MESSAGE) {
		g_free(self->synthesis_error_message);
		self->synthesis_error_message = g_value_dup_string(value);
	} else if (id == PROP_ACTIVITY)
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
	g_signal_connect(self->transport, "reconnecting", G_CALLBACK(transport_reconnecting),
					 self);
	g_signal_connect(self->transport, "reconnected", G_CALLBACK(transport_reconnected),
					 self);
	g_signal_connect(self->transport, "error", G_CALLBACK(transport_error), self);
	g_signal_connect(self->recognizer, "transcript", G_CALLBACK(transcript), self);
	g_signal_connect(self->recognizer, "error", G_CALLBACK(stt_error), self);
	g_signal_connect(self->synthesizer, "audio", G_CALLBACK(audio_out), self);
	self->worker = ai_voice_worker_new(self->conversation, G_OBJECT(self), self->context,
									   worker_mail);
	ai_voice_worker_set_trim_after(self->worker, self->trim_after);
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
	 * AiVoiceSession:relevance-provider: (type GObject)
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
							G_TYPE_OBJECT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
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
	 * @name: "stop", "mute", "unmute" or "hangup"
	 *
	 * A caller spoke a command while #AiVoiceSession:voice-commands is set.
	 * The session has already acted on stop, mute and unmute; hang-up is the
	 * application's to carry out.
	 */
	g_signal_new("command", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
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
	g_signal_new("relevance", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 3, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_BOOLEAN);
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
	self->repeats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	self->speak_code = TRUE;
	self->span = g_string_new(NULL);
	self->pending_text = g_string_new(NULL);
	self->spoken = g_string_new(NULL);
	self->deadline_ms = 20000;
	self->barge_in_ms = 250;
	self->barge_in_confirm = TRUE;
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
