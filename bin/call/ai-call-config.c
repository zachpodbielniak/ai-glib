/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ai-call-config.h"
#include <yaml.h>
#include <errno.h>
#include <string.h>

typedef struct {
	const gchar *name, *environment, *text;
	guint number, minimum, maximum;
} Setting;
/* One schema supplies properties, defaults, validation, YAML and environment. */
static const Setting settings[] = {
	{"credentials", "AI_CALL_CREDENTIALS", NULL, 0, 0, 0},
	{"device", "AI_CALL_DEVICE", "AIVOICE01", 0, 0, 0},
	{"jwt-url", "AI_CALL_JWT_URL", NULL, 0, 0, 0},
	{"focus-url", "AI_CALL_FOCUS_URL", NULL, 0, 0, 0},
	{"outbound-path", "AI_CALL_OUTBOUND_PATH", NULL, 0, 0, 0},
	{"greeting", "AI_CALL_GREETING", "Hello. How can I help?", 0, 0, 0},
	{"outbound-greeting", "AI_CALL_OUTBOUND_GREETING", "Hello. Is now a good time?", 0, 0,
	 0},
	{"turn-deadline-ms", "AI_VOICE_TURN_DEADLINE_MS", NULL, 20000, 1, 3600000},
	{"trailing-silence-ms", "AI_VOICE_TRAILING_SILENCE_MS", NULL, 600, 10, 10000},
	{"barge-in-ms", "AI_VOICE_BARGE_IN_MS", NULL, 250, 10, 5000},
	{"vad-mode", "AI_VOICE_VAD_MODE", NULL, 2, 0, 3},
	{"stt-timeout-ms", "AI_VOICE_STT_TIMEOUT_MS", NULL, 30000, 1, 300000},
	{"max-agents", "AI_VOICE_MAX_AGENTS", NULL, 4, 1, 128},
	{"deadline-message", "AI_VOICE_DEADLINE_MESSAGE",
	 "Sorry, that is taking too long. Please try again.", 0, 0, 0},
	{"transcription-error-message", "AI_VOICE_TRANSCRIPTION_ERROR_MESSAGE",
	 "Sorry, I could not transcribe that. Please try again.", 0, 0, 0}};
struct _AiCallConfig {
	GObject parent;
	GValue values[G_N_ELEMENTS(settings)];
};
G_DEFINE_TYPE(AiCallConfig, ai_call_config, G_TYPE_OBJECT)
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	g_value_copy(&AI_CALL_CONFIG(object)->values[id - 1], value);
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	GValue *dest = &AI_CALL_CONFIG(object)->values[id - 1];
	g_value_reset(dest);
	g_value_copy(value, dest);
}
static void
finalize(GObject *object)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(settings); i++)
		g_value_unset(&AI_CALL_CONFIG(object)->values[i]);
	G_OBJECT_CLASS(ai_call_config_parent_class)->finalize(object);
}
static void
ai_call_config_class_init(AiCallConfigClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	guint i;
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		const Setting *s = &settings[i];
		GParamSpec *p =
			s->maximum ? g_param_spec_uint(s->name, s->name, "Call setting", s->minimum,
										   s->maximum, s->number,
										   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
					   : g_param_spec_string(s->name, s->name, "Call setting", s->text,
											 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
		g_object_class_install_property(oc, i + 1, p);
	}
}
static void
ai_call_config_init(AiCallConfig *self)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		GParamSpec *p =
			g_object_class_find_property(G_OBJECT_GET_CLASS(self), settings[i].name);
		g_value_init(&self->values[i], G_PARAM_SPEC_VALUE_TYPE(p));
		g_param_value_set_default(p, &self->values[i]);
	}
}
AiCallConfig *
ai_call_config_new(void)
{
	g_autoptr(AiCallConfig) self = g_object_new(AI_TYPE_CALL_CONFIG, NULL);
	return g_steal_pointer(&self);
}
gboolean
ai_call_config_set_text(AiCallConfig *self, const gchar *name, const gchar *text,
						GError **error)
{
	GParamSpec *p = g_object_class_find_property(G_OBJECT_GET_CLASS(self), name);
	GValue value = G_VALUE_INIT;
	gboolean valid = FALSE;
	if (p == NULL)
		goto bad;
	g_value_init(&value, G_PARAM_SPEC_VALUE_TYPE(p));
	if (G_IS_PARAM_SPEC_STRING(p)) {
		g_value_set_string(&value, text);
		valid = TRUE;
	} else {
		gchar *end;
		guint64 number;
		errno = 0;
		number = g_ascii_strtoull(text, &end, 10);
		if (errno == 0 && text[0] >= '0' && text[0] <= '9' && *end == '\0' &&
			number <= G_MAXUINT) {
			g_value_set_uint(&value, number);
			valid = !g_param_value_validate(p, &value);
		}
	}
	if (valid)
		g_object_set_property(G_OBJECT(self), name, &value);
	g_value_unset(&value);
	if (valid)
		return TRUE;
bad:
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
				"Unknown or invalid ai_call setting: %s", name);
	return FALSE;
}
static void
copy_values(AiCallConfig *to, AiCallConfig *from)
{
	guint i;
	for (i = 0; i < G_N_ELEMENTS(settings); i++)
		g_object_set_property(G_OBJECT(to), settings[i].name, &from->values[i]);
}
gboolean
ai_call_config_load(AiCallConfig *self, const gchar *path, GError **error)
{
	g_autofree gchar *contents = NULL;
	g_autoptr(AiCallConfig) candidate = ai_call_config_new();
	gsize size;
	yaml_parser_t parser;
	yaml_document_t document;
	yaml_node_t *root;
	yaml_node_pair_t *pair;
	gboolean valid = FALSE;
	if (!g_file_get_contents(path, &contents, &size, error))
		return FALSE;
	copy_values(candidate, self);
	if (!yaml_parser_initialize(&parser)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
							"Cannot initialize YAML parser");
		return FALSE;
	}
	yaml_parser_set_input_string(&parser, (const unsigned char *)contents, size);
	if (!yaml_parser_load(&parser, &document)) {
		yaml_parser_delete(&parser);
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Invalid call configuration YAML");
		return FALSE;
	}
	{
		yaml_document_t extra;
		if (!yaml_parser_load(&parser, &extra))
			goto done;
		valid = yaml_document_get_root_node(&extra) == NULL;
		yaml_document_delete(&extra);
		if (!valid)
			goto done;
	}
	valid = FALSE;
	root = yaml_document_get_root_node(&document);
	if (root == NULL || root->type != YAML_MAPPING_NODE)
		goto done;
	valid = TRUE;
	for (pair = root->data.mapping.pairs.start; pair < root->data.mapping.pairs.top;
		 pair++) {
		yaml_node_t *key = yaml_document_get_node(&document, pair->key);
		yaml_node_t *map = yaml_document_get_node(&document, pair->value);
		yaml_node_pair_t *entry;
		if (key->type != YAML_SCALAR_NODE ||
			strcmp((gchar *)key->data.scalar.value, "ai_call") != 0)
			continue;
		if (map->type != YAML_MAPPING_NODE) {
			valid = FALSE;
			break;
		}
		for (entry = map->data.mapping.pairs.start; entry < map->data.mapping.pairs.top;
			 entry++) {
			yaml_node_t *k = yaml_document_get_node(&document, entry->key);
			yaml_node_t *v = yaml_document_get_node(&document, entry->value);
			if (k->type != YAML_SCALAR_NODE || v->type != YAML_SCALAR_NODE ||
				!ai_call_config_set_text(candidate, (gchar *)k->data.scalar.value,
										 (gchar *)v->data.scalar.value, error)) {
				valid = FALSE;
				break;
			}
		}
		if (!valid)
			break;
	}
done:
	yaml_document_delete(&document);
	yaml_parser_delete(&parser);
	if (valid)
		copy_values(self, candidate);
	else if (error != NULL && *error == NULL)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"ai_call must be a mapping of scalar settings");
	return valid;
}
gboolean
ai_call_config_apply_environment(AiCallConfig *self, GError **error)
{
	g_autoptr(AiCallConfig) candidate = ai_call_config_new();
	guint i;
	copy_values(candidate, self);
	for (i = 0; i < G_N_ELEMENTS(settings); i++) {
		const gchar *value = g_getenv(settings[i].environment);
		if (value != NULL &&
			!ai_call_config_set_text(candidate, settings[i].name, value, error))
			return FALSE;
	}
	copy_values(self, candidate);
	return TRUE;
}
