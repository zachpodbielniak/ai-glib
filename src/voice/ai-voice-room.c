/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voice-room.h"
#include <string.h>

/*
 * The room's memory. In room mode most of what is said is not for the
 * assistant; it is kept here and handed to the model with the next turn, so
 * a reply knows what was said before it was asked. Three limits keep that
 * from growing into a transcript of the whole afternoon: a number of lines,
 * a number of characters, and an age. Oldest goes first.
 *
 * It also keeps a roster: who has spoken recently, addressed or not. With
 * announce-roster set, each turn starts with "[Present: Sam, Kim, Guest 1]",
 * so the model can answer people by name and knows who it is talking in
 * front of.
 */
typedef struct {
	gchar *speaker, *text;
	gint64 at;
} Heard;
typedef struct {
	gchar *name;
	gint64 at;
} Seen;
typedef struct {
	GQueue heard;	/* Heard*, oldest first */
	gsize chars;	/* bytes of text in heard */
	GQueue roster;	/* Seen*, least recently heard first */
	guint64 revision;
	guint max_lines, max_chars, max_age_ms, roster_window_ms;
	gboolean announce_roster;
} AiVoiceRoomPrivate;

enum {
	PROP_0,
	PROP_MAX_LINES,
	PROP_MAX_CHARS,
	PROP_MAX_AGE,
	PROP_ROSTER_WINDOW,
	PROP_ANNOUNCE_ROSTER,
	N_PROPS
};
static GParamSpec *props[N_PROPS];

G_DEFINE_TYPE_WITH_PRIVATE(AiVoiceRoom, ai_voice_room, G_TYPE_OBJECT)

static AiVoiceRoomPrivate *
private_of(AiVoiceRoom *self)
{
	return ai_voice_room_get_instance_private(self);
}
static void
heard_free(gpointer data)
{
	Heard *h = data;
	g_free(h->speaker);
	g_free(h->text);
	g_free(h);
}
static void
seen_free(gpointer data)
{
	Seen *s = data;
	g_free(s->name);
	g_free(s);
}
/* Drop the oldest lines until every limit holds. */
static void
trim(AiVoiceRoom *self)
{
	AiVoiceRoomPrivate *priv = private_of(self);
	gint64 now = g_get_monotonic_time();
	Heard *h;
	while ((h = g_queue_peek_head(&priv->heard)) != NULL &&
		   (g_queue_get_length(&priv->heard) > priv->max_lines ||
			(priv->max_chars > 0 && priv->chars > priv->max_chars) ||
			(priv->max_age_ms > 0 && now - h->at > (gint64)priv->max_age_ms * 1000))) {
		g_queue_pop_head(&priv->heard);
		priv->chars -= strlen(h->text);
		heard_free(h);
	}
}
static gchar *
default_format_line(AiVoiceRoom *self, const gchar *speaker, const gchar *text)
{
	return g_strdup_printf("[%s]: %s", speaker != NULL ? speaker : "Someone", text);
}
static gchar *
default_format_turn(AiVoiceRoom *self, const gchar *const *present, const gchar *const *heard,
					const gchar *line)
{
	GString *turn = g_string_new(NULL);
	guint i;
	if (present != NULL && present[0] != NULL) {
		g_autofree gchar *names = g_strjoinv(", ", (gchar **)present);
		g_string_append_printf(turn, "[Present: %s]\n", names);
	}
	for (i = 0; heard != NULL && heard[i] != NULL; i++) {
		g_string_append(turn, heard[i]);
		g_string_append_c(turn, '\n');
	}
	g_string_append(turn, line);
	return g_string_free(turn, FALSE);
}

/**
 * ai_voice_room_note_speaker:
 * @self: a room
 * @speaker: who just spoke
 *
 * Puts @speaker on the roster as heard now.
 */
void
ai_voice_room_note_speaker(AiVoiceRoom *self, const gchar *speaker)
{
	AiVoiceRoomPrivate *priv;
	GList *l;
	Seen *s = NULL;
	g_return_if_fail(AI_IS_VOICE_ROOM(self));
	if (speaker == NULL || *speaker == '\0')
		return;
	priv = private_of(self);
	for (l = priv->roster.head; l != NULL; l = l->next)
		if (g_str_equal(((Seen *)l->data)->name, speaker)) {
			s = l->data;
			g_queue_delete_link(&priv->roster, l);
			break;
		}
	if (s == NULL) {
		s = g_new0(Seen, 1);
		s->name = g_strdup(speaker);
	}
	s->at = g_get_monotonic_time();
	g_queue_push_tail(&priv->roster, s);
	/* Nobody keeps a roster of hundreds. */
	while (g_queue_get_length(&priv->roster) > 64)
		seen_free(g_queue_pop_head(&priv->roster));
}

/**
 * ai_voice_room_rename_speaker:
 * @self: a room
 * @from: a name heard
 * @to: what to call them now
 *
 * After a guest is given a real name, the roster and what was heard say so.
 */
void
ai_voice_room_rename_speaker(AiVoiceRoom *self, const gchar *from, const gchar *to)
{
	AiVoiceRoomPrivate *priv;
	GList *l;
	g_return_if_fail(AI_IS_VOICE_ROOM(self));
	g_return_if_fail(from != NULL && to != NULL);
	priv = private_of(self);
	for (l = priv->roster.head; l != NULL; l = l->next) {
		Seen *s = l->data;
		if (g_str_equal(s->name, from)) {
			g_free(s->name);
			s->name = g_strdup(to);
		}
	}
	for (l = priv->heard.head; l != NULL; l = l->next) {
		Heard *h = l->data;
		if (g_strcmp0(h->speaker, from) == 0) {
			g_free(h->speaker);
			h->speaker = g_strdup(to);
		}
	}
}

/**
 * ai_voice_room_hear:
 * @self: a room
 * @speaker: who said it
 * @text: what was said, not to the assistant
 *
 * Keeps a line of talk for the next turn, and puts @speaker on the roster.
 */
void
ai_voice_room_hear(AiVoiceRoom *self, const gchar *speaker, const gchar *text)
{
	AiVoiceRoomPrivate *priv;
	Heard *h;
	g_return_if_fail(AI_IS_VOICE_ROOM(self));
	g_return_if_fail(text != NULL);
	priv = private_of(self);
	h = g_new0(Heard, 1);
	h->speaker = g_strdup(speaker);
	h->text = g_strdup(text);
	h->at = g_get_monotonic_time();
	g_queue_push_tail(&priv->heard, h);
	priv->chars += strlen(text);
	priv->revision++;
	ai_voice_room_note_speaker(self, speaker);
	trim(self);
}

/**
 * ai_voice_room_get_lines:
 * @self: a room
 *
 * Returns: (transfer full) (array zero-terminated=1): what was heard since
 *   the last turn, oldest first, as the model sees it
 */
gchar **
ai_voice_room_get_lines(AiVoiceRoom *self)
{
	AiVoiceRoomPrivate *priv;
	GPtrArray *out;
	GList *l;
	g_return_val_if_fail(AI_IS_VOICE_ROOM(self), NULL);
	priv = private_of(self);
	trim(self);
	out = g_ptr_array_new();
	for (l = priv->heard.head; l != NULL; l = l->next) {
		Heard *h = l->data;
		g_ptr_array_add(out, AI_VOICE_ROOM_GET_CLASS(self)->format_line(self, h->speaker, h->text));
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/**
 * ai_voice_room_get_n_lines:
 * @self: a room
 *
 * Returns: how many lines were heard since the last turn
 */
guint
ai_voice_room_get_n_lines(AiVoiceRoom *self)
{
	g_return_val_if_fail(AI_IS_VOICE_ROOM(self), 0);
	trim(self);
	return g_queue_get_length(&private_of(self)->heard);
}

/**
 * ai_voice_room_get_present:
 * @self: a room
 *
 * Returns: (transfer full) (array zero-terminated=1): who spoke within
 *   #AiVoiceRoom:roster-window-ms, least recently heard first
 */
gchar **
ai_voice_room_get_present(AiVoiceRoom *self)
{
	AiVoiceRoomPrivate *priv;
	GPtrArray *out;
	gint64 now;
	GList *l;
	g_return_val_if_fail(AI_IS_VOICE_ROOM(self), NULL);
	priv = private_of(self);
	now = g_get_monotonic_time();
	out = g_ptr_array_new();
	for (l = priv->roster.head; l != NULL; l = l->next) {
		Seen *s = l->data;
		if (priv->roster_window_ms == 0 || now - s->at <= (gint64)priv->roster_window_ms * 1000)
			g_ptr_array_add(out, g_strdup(s->name));
	}
	g_ptr_array_add(out, NULL);
	return (gchar **)g_ptr_array_free(out, FALSE);
}

/**
 * ai_voice_room_get_revision:
 * @self: a room
 *
 * Returns: a number that changes with every line heard and every turn taken:
 *   a score of the talk at an older revision is stale
 */
guint64
ai_voice_room_get_revision(AiVoiceRoom *self)
{
	g_return_val_if_fail(AI_IS_VOICE_ROOM(self), 0);
	return private_of(self)->revision;
}

/**
 * ai_voice_room_take_turn:
 * @self: a room
 * @speaker: (nullable): who made the turn; %NULL when @text is a note of
 *   the session's own (chiming in)
 * @text: what they said, or the note
 *
 * The text of a turn: who is present when announced, what was heard, then
 * @text. The room's heard lines start over.
 *
 * Returns: (transfer full): the turn's text
 */
gchar *
ai_voice_room_take_turn(AiVoiceRoom *self, const gchar *speaker, const gchar *text)
{
	AiVoiceRoomPrivate *priv;
	AiVoiceRoomClass *klass;
	g_auto(GStrv) heard = NULL;
	g_auto(GStrv) present = NULL;
	g_autofree gchar *line = NULL;
	gchar *turn;
	g_return_val_if_fail(AI_IS_VOICE_ROOM(self), NULL);
	g_return_val_if_fail(text != NULL, NULL);
	priv = private_of(self);
	klass = AI_VOICE_ROOM_GET_CLASS(self);
	if (speaker != NULL)
		ai_voice_room_note_speaker(self, speaker);
	heard = ai_voice_room_get_lines(self);
	present = priv->announce_roster ? ai_voice_room_get_present(self) : NULL;
	line = speaker != NULL ? klass->format_line(self, speaker, text) : g_strdup(text);
	turn = klass->format_turn(self, (const gchar *const *)present, (const gchar *const *)heard,
							  line);
	ai_voice_room_clear(self);
	return turn;
}

/**
 * ai_voice_room_clear:
 * @self: a room
 *
 * Forgets what was heard; the roster stays.
 */
void
ai_voice_room_clear(AiVoiceRoom *self)
{
	AiVoiceRoomPrivate *priv;
	g_return_if_fail(AI_IS_VOICE_ROOM(self));
	priv = private_of(self);
	g_queue_clear_full(&priv->heard, heard_free);
	priv->chars = 0;
	priv->revision++;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiVoiceRoomPrivate *priv = private_of(AI_VOICE_ROOM(object));
	switch (id) {
	case PROP_MAX_LINES:
		g_value_set_uint(value, priv->max_lines);
		break;
	case PROP_MAX_CHARS:
		g_value_set_uint(value, priv->max_chars);
		break;
	case PROP_MAX_AGE:
		g_value_set_uint(value, priv->max_age_ms);
		break;
	case PROP_ROSTER_WINDOW:
		g_value_set_uint(value, priv->roster_window_ms);
		break;
	case PROP_ANNOUNCE_ROSTER:
		g_value_set_boolean(value, priv->announce_roster);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiVoiceRoomPrivate *priv = private_of(AI_VOICE_ROOM(object));
	switch (id) {
	case PROP_MAX_LINES:
		priv->max_lines = g_value_get_uint(value);
		break;
	case PROP_MAX_CHARS:
		priv->max_chars = g_value_get_uint(value);
		break;
	case PROP_MAX_AGE:
		priv->max_age_ms = g_value_get_uint(value);
		break;
	case PROP_ROSTER_WINDOW:
		priv->roster_window_ms = g_value_get_uint(value);
		break;
	case PROP_ANNOUNCE_ROSTER:
		priv->announce_roster = g_value_get_boolean(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
		return;
	}
	trim(AI_VOICE_ROOM(object));
}
static void
finalize(GObject *object)
{
	AiVoiceRoomPrivate *priv = private_of(AI_VOICE_ROOM(object));
	g_queue_clear_full(&priv->heard, heard_free);
	g_queue_clear_full(&priv->roster, seen_free);
	G_OBJECT_CLASS(ai_voice_room_parent_class)->finalize(object);
}
static void
ai_voice_room_class_init(AiVoiceRoomClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	klass->format_line = default_format_line;
	klass->format_turn = default_format_turn;
	/**
	 * AiVoiceRoom:max-lines:
	 *
	 * The most lines of unaddressed talk kept for the next turn.
	 */
	props[PROP_MAX_LINES] = g_param_spec_uint("max-lines", "Maximum lines",
											  "Most lines of talk kept for the next turn", 1,
											  10000, 40,
											  G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
												  G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceRoom:max-chars:
	 *
	 * The most bytes of unaddressed talk kept; 0 no limit.
	 */
	props[PROP_MAX_CHARS] = g_param_spec_uint("max-chars", "Maximum characters",
											  "Most bytes of talk kept; 0 no limit", 0,
											  G_MAXINT, 6000,
											  G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
												  G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceRoom:max-age-ms:
	 *
	 * Talk older than this is dropped; 0 keeps it however old.
	 */
	props[PROP_MAX_AGE] = g_param_spec_uint("max-age-ms", "Maximum age",
											"Oldest talk kept; 0 no limit", 0, G_MAXINT, 900000,
											G_PARAM_READWRITE | G_PARAM_CONSTRUCT |
												G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceRoom:roster-window-ms:
	 *
	 * Someone heard within this long is present; 0 keeps everyone heard.
	 */
	props[PROP_ROSTER_WINDOW] = g_param_spec_uint(
		"roster-window-ms", "Roster window", "How recently someone spoke to be present", 0,
		G_MAXINT, 600000, G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceRoom:announce-roster:
	 *
	 * Start every turn with "[Present: ...]", the people heard recently.
	 */
	props[PROP_ANNOUNCE_ROSTER] = g_param_spec_boolean(
		"announce-roster", "Announce roster", "Tell the model who is present with each turn",
		FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
}
static void
ai_voice_room_init(AiVoiceRoom *self)
{
	AiVoiceRoomPrivate *priv = private_of(self);
	g_queue_init(&priv->heard);
	g_queue_init(&priv->roster);
}

/**
 * ai_voice_room_new:
 *
 * Returns: (transfer full): a room with the default limits
 */
AiVoiceRoom *
ai_voice_room_new(void)
{
	return g_object_new(AI_TYPE_VOICE_ROOM, NULL);
}
