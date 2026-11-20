/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-speaker-match.h"

/*
 * Who an utterance was said by, and how sure: the result of naming one
 * utterance. The identifier fills in an enrolled or guest match; the
 * session reports inherited and fallback names the same way, so an
 * application sees one shape whatever happened.
 */
typedef struct {
	AiSpeakerMatchKind kind;
	gchar *name;
	gdouble score;
	AiVoiceprint *voiceprint; /* the utterance's own, when there was one */
} AiSpeakerMatchPrivate;

enum { PROP_0, PROP_KIND, PROP_NAME, PROP_SCORE, PROP_VOICEPRINT, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_TYPE_WITH_PRIVATE(AiSpeakerMatch, ai_speaker_match, G_TYPE_OBJECT)

/**
 * ai_speaker_match_kind_get_type:
 *
 * Returns: the registered #AiSpeakerMatchKind type
 */
GType
ai_speaker_match_kind_get_type(void)
{
	static gsize type_id;
	if (g_once_init_enter(&type_id)) {
		static const GEnumValue values[] = {
			{AI_SPEAKER_MATCH_ENROLLED, "AI_SPEAKER_MATCH_ENROLLED", "enrolled"},
			{AI_SPEAKER_MATCH_GUEST, "AI_SPEAKER_MATCH_GUEST", "guest"},
			{AI_SPEAKER_MATCH_UNKNOWN, "AI_SPEAKER_MATCH_UNKNOWN", "unknown"},
			{AI_SPEAKER_MATCH_INHERITED, "AI_SPEAKER_MATCH_INHERITED", "inherited"},
			{AI_SPEAKER_MATCH_FALLBACK, "AI_SPEAKER_MATCH_FALLBACK", "fallback"},
			{0, NULL, NULL}};
		GType registered = g_enum_register_static("AiSpeakerMatchKind", values);
		g_once_init_leave(&type_id, registered);
	}
	return type_id;
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiSpeakerMatchPrivate *priv = ai_speaker_match_get_instance_private(AI_SPEAKER_MATCH(object));
	switch (id) {
	case PROP_KIND:
		g_value_set_enum(value, priv->kind);
		break;
	case PROP_NAME:
		g_value_set_string(value, priv->name);
		break;
	case PROP_SCORE:
		g_value_set_double(value, priv->score);
		break;
	case PROP_VOICEPRINT:
		g_value_set_object(value, priv->voiceprint);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiSpeakerMatchPrivate *priv = ai_speaker_match_get_instance_private(AI_SPEAKER_MATCH(object));
	switch (id) {
	case PROP_KIND:
		priv->kind = g_value_get_enum(value);
		break;
	case PROP_NAME:
		g_free(priv->name);
		priv->name = g_value_dup_string(value);
		break;
	case PROP_SCORE:
		priv->score = g_value_get_double(value);
		break;
	case PROP_VOICEPRINT:
		g_set_object(&priv->voiceprint, g_value_get_object(value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
finalize(GObject *object)
{
	AiSpeakerMatchPrivate *priv = ai_speaker_match_get_instance_private(AI_SPEAKER_MATCH(object));
	g_free(priv->name);
	g_clear_object(&priv->voiceprint);
	G_OBJECT_CLASS(ai_speaker_match_parent_class)->finalize(object);
}
static void
ai_speaker_match_class_init(AiSpeakerMatchClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	/**
	 * AiSpeakerMatch:kind:
	 *
	 * How the speaker came to be named.
	 */
	props[PROP_KIND] = g_param_spec_enum("kind", "Kind", "How the speaker was named",
										 AI_TYPE_SPEAKER_MATCH_KIND, AI_SPEAKER_MATCH_UNKNOWN,
										 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiSpeakerMatch:name:
	 *
	 * The speaker's name; %NULL for an unknown voice.
	 */
	props[PROP_NAME] = g_param_spec_string("name", "Name", "The speaker's name", NULL,
										   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiSpeakerMatch:score:
	 *
	 * Similarity to the voice named, -1 to 1; 0 when there was nothing to
	 * compare.
	 */
	props[PROP_SCORE] = g_param_spec_double("score", "Score", "Similarity to the voice named",
											-1.0, 1.0, 0.0,
											G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiSpeakerMatch:voiceprint:
	 *
	 * The utterance's own voiceprint, when one was made.
	 */
	props[PROP_VOICEPRINT] =
		g_param_spec_object("voiceprint", "Voiceprint", "The utterance's own voiceprint",
							AI_TYPE_VOICEPRINT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
}
static void
ai_speaker_match_init(AiSpeakerMatch *self)
{
	AiSpeakerMatchPrivate *priv = ai_speaker_match_get_instance_private(self);
	priv->kind = AI_SPEAKER_MATCH_UNKNOWN;
}

/**
 * ai_speaker_match_new:
 * @kind: how the speaker was named
 * @name: (nullable): the name
 * @score: similarity to the voice named, -1 to 1
 *
 * Returns: (transfer full): a new match
 */
AiSpeakerMatch *
ai_speaker_match_new(AiSpeakerMatchKind kind, const gchar *name, gdouble score)
{
	return g_object_new(AI_TYPE_SPEAKER_MATCH, "kind", kind, "name", name, "score",
						CLAMP(score, -1.0, 1.0), NULL);
}

/**
 * ai_speaker_match_get_kind:
 * @self: a match
 *
 * Returns: how the speaker was named
 */
AiSpeakerMatchKind
ai_speaker_match_get_kind(AiSpeakerMatch *self)
{
	g_return_val_if_fail(AI_IS_SPEAKER_MATCH(self), AI_SPEAKER_MATCH_UNKNOWN);
	return ((AiSpeakerMatchPrivate *)ai_speaker_match_get_instance_private(self))->kind;
}

/**
 * ai_speaker_match_get_name:
 * @self: a match
 *
 * Returns: (nullable): the speaker's name; %NULL for an unknown voice
 */
const gchar *
ai_speaker_match_get_name(AiSpeakerMatch *self)
{
	g_return_val_if_fail(AI_IS_SPEAKER_MATCH(self), NULL);
	return ((AiSpeakerMatchPrivate *)ai_speaker_match_get_instance_private(self))->name;
}

/**
 * ai_speaker_match_get_score:
 * @self: a match
 *
 * Returns: similarity to the voice named, -1 to 1
 */
gdouble
ai_speaker_match_get_score(AiSpeakerMatch *self)
{
	g_return_val_if_fail(AI_IS_SPEAKER_MATCH(self), 0);
	return ((AiSpeakerMatchPrivate *)ai_speaker_match_get_instance_private(self))->score;
}

/**
 * ai_speaker_match_get_guest_number:
 * @self: a match
 *
 * Returns: N for a match named "Guest N", else 0
 */
guint
ai_speaker_match_get_guest_number(AiSpeakerMatch *self)
{
	guint n = 0;
	g_return_val_if_fail(AI_IS_SPEAKER_MATCH(self), 0);
	return ai_voiceprint_name_is_guest(ai_speaker_match_get_name(self), &n) ? n : 0;
}

/**
 * ai_speaker_match_get_voiceprint:
 * @self: a match
 *
 * Returns: (transfer none) (nullable): the utterance's own voiceprint
 */
AiVoiceprint *
ai_speaker_match_get_voiceprint(AiSpeakerMatch *self)
{
	g_return_val_if_fail(AI_IS_SPEAKER_MATCH(self), NULL);
	return ((AiSpeakerMatchPrivate *)ai_speaker_match_get_instance_private(self))->voiceprint;
}

/**
 * ai_speaker_match_set_voiceprint:
 * @self: a match
 * @voiceprint: (nullable): the utterance's own voiceprint
 */
void
ai_speaker_match_set_voiceprint(AiSpeakerMatch *self, AiVoiceprint *voiceprint)
{
	g_return_if_fail(AI_IS_SPEAKER_MATCH(self));
	g_object_set(self, "voiceprint", voiceprint, NULL);
}
