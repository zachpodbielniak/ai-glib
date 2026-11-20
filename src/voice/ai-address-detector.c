/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-address-detector.h"
#include <string.h>

/*
 * Whether an utterance is said to the assistant. The interface is one
 * question; AiNameAddressDetector answers it by listening for a name.
 */
G_DEFINE_INTERFACE(AiAddressDetector, ai_address_detector, G_TYPE_OBJECT)
static void
ai_address_detector_default_init(AiAddressDetectorInterface *iface)
{
}

/**
 * ai_address_detector_is_addressed:
 * @self: a detector
 * @speaker: (nullable): who said it, as named
 * @text: what was said
 *
 * Returns: whether @text is said to the assistant
 */
gboolean
ai_address_detector_is_addressed(AiAddressDetector *self, const gchar *speaker,
								 const gchar *text)
{
	AiAddressDetectorInterface *iface;
	g_return_val_if_fail(AI_IS_ADDRESS_DETECTOR(self), TRUE);
	iface = AI_ADDRESS_DETECTOR_GET_IFACE(self);
	return iface->is_addressed == NULL || iface->is_addressed(self, speaker, text != NULL ? text : "");
}

/* ---- AiNameAddressDetector ----
 *
 * Named anywhere in the utterance, as whole words: "Ada, what do you think?"
 * and "what does Ada think?" both are. Recognition hears a short name as a
 * common word ("Kay" as "say"), so a sound-alike counts too -- but only
 * where a name would be: after a greeting ("Hi, say."), before a comma at
 * the start ("Say, could you..."), after a comma at the end ("What do you
 * think, say?"). Anywhere else it is the ordinary word. With no names at
 * all, everything is addressed. */
typedef struct {
	gchar **names, **soundalikes;
} AiNameAddressDetectorPrivate;

enum { PROP_0, PROP_NAMES, PROP_SOUNDALIKES, N_PROPS };
static GParamSpec *props[N_PROPS];

static void detector_iface_init(AiAddressDetectorInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiNameAddressDetector, ai_name_address_detector, G_TYPE_OBJECT,
						G_ADD_PRIVATE(AiNameAddressDetector)
							G_IMPLEMENT_INTERFACE(AI_TYPE_ADDRESS_DETECTOR, detector_iface_init))

/* Lower-case words separated by single spaces; anything not a letter, digit
 * or apostrophe separates words. */
static gchar *
words_of(const gchar *text)
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
soundalike_addressed(gchar **soundalikes, const gchar *text)
{
	guint i;
	for (i = 0; soundalikes != NULL && soundalikes[i] != NULL; i++) {
		g_autofree gchar *word = g_regex_escape_string(soundalikes[i], -1);
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
static gboolean
name_is_addressed(AiAddressDetector *detector, const gchar *speaker, const gchar *text)
{
	AiNameAddressDetectorPrivate *priv =
		ai_name_address_detector_get_instance_private(AI_NAME_ADDRESS_DETECTOR(detector));
	g_autofree gchar *words = NULL, *padded = NULL;
	guint i;
	if (priv->names == NULL || priv->names[0] == NULL)
		return TRUE;
	if (soundalike_addressed(priv->soundalikes, text))
		return TRUE;
	words = words_of(text);
	padded = g_strconcat(" ", words, " ", NULL);
	for (i = 0; priv->names[i] != NULL; i++) {
		g_autofree gchar *name = words_of(priv->names[i]);
		g_autofree gchar *needle = g_strconcat(" ", name, " ", NULL);
		if (*name != '\0' && strstr(padded, needle) != NULL)
			return TRUE;
	}
	return FALSE;
}
static void
detector_iface_init(AiAddressDetectorInterface *iface)
{
	iface->is_addressed = name_is_addressed;
}
/* "Ada, Ida" -> {"Ada", "Ida"}; empty -> NULL. */
static gchar **
split_list(const gchar *list)
{
	gchar **out;
	guint i;
	if (list == NULL || *list == '\0')
		return NULL;
	out = g_strsplit(list, ",", -1);
	for (i = 0; out[i] != NULL; i++)
		g_strstrip(out[i]);
	return out;
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiNameAddressDetectorPrivate *priv =
		ai_name_address_detector_get_instance_private(AI_NAME_ADDRESS_DETECTOR(object));
	gchar **list = id == PROP_NAMES ? priv->names : priv->soundalikes;
	if (id != PROP_NAMES && id != PROP_SOUNDALIKES) {
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
		return;
	}
	g_value_take_string(value, list != NULL ? g_strjoinv(",", list) : NULL);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiNameAddressDetectorPrivate *priv =
		ai_name_address_detector_get_instance_private(AI_NAME_ADDRESS_DETECTOR(object));
	if (id == PROP_NAMES) {
		g_strfreev(priv->names);
		priv->names = split_list(g_value_get_string(value));
	} else if (id == PROP_SOUNDALIKES) {
		g_strfreev(priv->soundalikes);
		priv->soundalikes = split_list(g_value_get_string(value));
	} else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
}
static void
finalize(GObject *object)
{
	AiNameAddressDetectorPrivate *priv =
		ai_name_address_detector_get_instance_private(AI_NAME_ADDRESS_DETECTOR(object));
	g_strfreev(priv->names);
	g_strfreev(priv->soundalikes);
	G_OBJECT_CLASS(ai_name_address_detector_parent_class)->finalize(object);
}
static void
ai_name_address_detector_class_init(AiNameAddressDetectorClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	/**
	 * AiNameAddressDetector:names:
	 *
	 * Comma-separated names of the assistant. With none, every utterance
	 * is addressed.
	 */
	props[PROP_NAMES] = g_param_spec_string("names", "Names", "Comma-separated assistant names",
											NULL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiNameAddressDetector:soundalikes:
	 *
	 * Comma-separated words recognition hears for a name, counted only
	 * where a name would be.
	 */
	props[PROP_SOUNDALIKES] =
		g_param_spec_string("soundalikes", "Sound-alikes",
							"Words heard for a name, counted only in a name's place", NULL,
							G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
}
static void
ai_name_address_detector_init(AiNameAddressDetector *self)
{
}

/**
 * ai_name_address_detector_new:
 * @names: (nullable): comma-separated assistant names
 * @soundalikes: (nullable): comma-separated sound-alike words
 *
 * Returns: (transfer full): a new detector
 */
AiNameAddressDetector *
ai_name_address_detector_new(const gchar *names, const gchar *soundalikes)
{
	return g_object_new(AI_TYPE_NAME_ADDRESS_DETECTOR, "names", names, "soundalikes",
						soundalikes, NULL);
}
