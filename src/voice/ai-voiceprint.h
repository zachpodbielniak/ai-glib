/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <glib-object.h>
#include <json-glib/json-glib.h>
G_BEGIN_DECLS
#define AI_TYPE_VOICEPRINT (ai_voiceprint_get_type())
G_DECLARE_DERIVABLE_TYPE(AiVoiceprint, ai_voiceprint, AI, VOICEPRINT, GObject)
/**
 * AiVoiceprintClass:
 * @parent_class: the parent class
 * @similarity: how alike two voiceprints of the same model are, -1 to 1;
 *   cosine similarity by default
 *
 * A subclass may carry more than the vector (say, a database row id) or
 * measure similarity differently.
 */
struct _AiVoiceprintClass {
	GObjectClass parent_class;
	gdouble (*similarity)(AiVoiceprint *self, AiVoiceprint *other);
	gpointer _reserved[8];
};

/**
 * AI_VOICEPRINT_NAME_MAX:
 *
 * The longest name a voiceprint may have, in bytes.
 */
#define AI_VOICEPRINT_NAME_MAX (64)

/**
 * AI_VOICEPRINT_MAX_DIMENSIONS:
 *
 * The most elements a voiceprint's embedding may have.
 */
#define AI_VOICEPRINT_MAX_DIMENSIONS (8192)

AiVoiceprint *
ai_voiceprint_new(const gchar *name, const gchar *model, const gfloat *embedding,
				  gsize dimensions, GError **error);
AiVoiceprint *
ai_voiceprint_new_from_json(JsonNode *node, GError **error);
JsonNode *
ai_voiceprint_to_json(AiVoiceprint *self);
AiVoiceprint *
ai_voiceprint_copy(AiVoiceprint *self, const gchar *name);

const gchar *
ai_voiceprint_get_name(AiVoiceprint *self);
const gchar *
ai_voiceprint_get_model(AiVoiceprint *self);
const gfloat *
ai_voiceprint_get_embedding(AiVoiceprint *self, gsize *dimensions);
guint
ai_voiceprint_get_count(AiVoiceprint *self);
void
ai_voiceprint_set_count(AiVoiceprint *self, guint count);
gint64
ai_voiceprint_get_updated(AiVoiceprint *self);

gboolean
ai_voiceprint_is_comparable(AiVoiceprint *self, AiVoiceprint *other);
gdouble
ai_voiceprint_similarity(AiVoiceprint *self, AiVoiceprint *other);
void
ai_voiceprint_absorb(AiVoiceprint *self, AiVoiceprint *other, guint max_count);

gboolean
ai_voiceprint_name_is_valid(const gchar *name);
gboolean
ai_voiceprint_name_is_guest(const gchar *name, guint *number);
G_END_DECLS
