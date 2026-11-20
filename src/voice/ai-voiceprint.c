/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voiceprint.h"
#include "model/ai-embedding.h"
#include "core/ai-json-util.h"
#include <gio/gio.h>
#include <math.h>
#include <string.h>

/*
 * A voiceprint: a person's name, the model that made it, and a unit-length
 * vector. The model travels with every voiceprint because vectors from two
 * models are not comparable -- the cosine between them reads like a weak
 * match rather than like an error -- so is_comparable() is checked before
 * every similarity.
 *
 * The vector is validated once, here: every element finite, at least one of
 * them, not all zero. Everything downstream (stores, identifiers, servers)
 * can then trust it.
 */
typedef struct {
	gchar *name, *model;
	gfloat *vector;
	gsize dims;
	guint count;   /* how many recordings were averaged into it */
	gint64 updated; /* unix seconds of the last change */
} AiVoiceprintPrivate;

enum { PROP_0, PROP_NAME, PROP_MODEL, PROP_DIMENSIONS, PROP_COUNT, PROP_UPDATED, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_TYPE_WITH_PRIVATE(AiVoiceprint, ai_voiceprint, G_TYPE_OBJECT)

static gdouble
default_similarity(AiVoiceprint *self, AiVoiceprint *other)
{
	AiVoiceprintPrivate *a = ai_voiceprint_get_instance_private(self);
	AiVoiceprintPrivate *b = ai_voiceprint_get_instance_private(other);
	return ai_embedding_cosine(a->vector, b->vector, a->dims);
}
static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
	AiVoiceprintPrivate *priv = ai_voiceprint_get_instance_private(AI_VOICEPRINT(object));
	switch (id) {
	case PROP_NAME:
		g_value_set_string(value, priv->name);
		break;
	case PROP_MODEL:
		g_value_set_string(value, priv->model);
		break;
	case PROP_DIMENSIONS:
		g_value_set_uint(value, (guint)priv->dims);
		break;
	case PROP_COUNT:
		g_value_set_uint(value, priv->count);
		break;
	case PROP_UPDATED:
		g_value_set_int64(value, priv->updated);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
	AiVoiceprintPrivate *priv = ai_voiceprint_get_instance_private(AI_VOICEPRINT(object));
	switch (id) {
	case PROP_NAME:
		g_free(priv->name);
		priv->name = g_value_dup_string(value);
		break;
	case PROP_MODEL:
		g_free(priv->model);
		priv->model = g_value_dup_string(value);
		break;
	case PROP_COUNT:
		priv->count = MAX(1u, g_value_get_uint(value));
		break;
	case PROP_UPDATED:
		priv->updated = g_value_get_int64(value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
	}
}
static void
finalize(GObject *object)
{
	AiVoiceprintPrivate *priv = ai_voiceprint_get_instance_private(AI_VOICEPRINT(object));
	g_free(priv->name);
	g_free(priv->model);
	g_free(priv->vector);
	G_OBJECT_CLASS(ai_voiceprint_parent_class)->finalize(object);
}
static void
ai_voiceprint_class_init(AiVoiceprintClass *klass)
{
	GObjectClass *oc = G_OBJECT_CLASS(klass);
	oc->get_property = get_property;
	oc->set_property = set_property;
	oc->finalize = finalize;
	klass->similarity = default_similarity;
	/**
	 * AiVoiceprint:name:
	 *
	 * Whose voice it is; %NULL for an utterance not yet named.
	 */
	props[PROP_NAME] = g_param_spec_string("name", "Name", "Whose voice it is", NULL,
										   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprint:model:
	 *
	 * The embedding model that made the vector. Voiceprints are compared
	 * only with others from the same model.
	 */
	props[PROP_MODEL] =
		g_param_spec_string("model", "Model", "Embedding model that made it", NULL,
							G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprint:dimensions:
	 *
	 * The vector's length.
	 */
	props[PROP_DIMENSIONS] =
		g_param_spec_uint("dimensions", "Dimensions", "Vector length", 0,
						  AI_VOICEPRINT_MAX_DIMENSIONS, 0, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprint:count:
	 *
	 * How many recordings were averaged into it: the weight the vector
	 * carries when another is absorbed.
	 */
	props[PROP_COUNT] = g_param_spec_uint("count", "Count", "Recordings averaged into it", 1,
										  G_MAXUINT, 1,
										  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	/**
	 * AiVoiceprint:updated:
	 *
	 * When it last changed, in seconds since the Unix epoch; 0 unknown.
	 */
	props[PROP_UPDATED] = g_param_spec_int64("updated", "Updated", "Last change, Unix seconds",
											 0, G_MAXINT64, 0,
											 G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	g_object_class_install_properties(oc, N_PROPS, props);
}
static void
ai_voiceprint_init(AiVoiceprint *self)
{
	AiVoiceprintPrivate *priv = ai_voiceprint_get_instance_private(self);
	priv->count = 1;
}

/* Every element finite and small enough for a float, at least one of them,
 * and not all zero; then scaled to unit length. A zero or non-finite vector
 * has no direction: it matches nobody, and averaged into a voiceprint it
 * poisons it. */
static gfloat *
checked_vector(const gfloat *embedding, gsize dims, GError **error)
{
	gdouble norm = 0;
	gfloat *vector;
	gsize i;
	if (embedding == NULL || dims == 0 || dims > AI_VOICEPRINT_MAX_DIMENSIONS) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"A voiceprint needs 1 to %u values, not %" G_GSIZE_FORMAT,
					(guint)AI_VOICEPRINT_MAX_DIMENSIONS, dims);
		return NULL;
	}
	for (i = 0; i < dims; i++) {
		if (!isfinite(embedding[i])) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
								"A voiceprint value is not a finite number");
			return NULL;
		}
		norm += (gdouble)embedding[i] * (gdouble)embedding[i];
	}
	if (!(norm > 0) || !isfinite(norm)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"A voiceprint of all zeros has no direction");
		return NULL;
	}
	vector = g_memdup2(embedding, dims * sizeof(gfloat));
	ai_embedding_normalize(vector, dims);
	return vector;
}

/**
 * ai_voiceprint_new:
 * @name: (nullable): whose voice; %NULL for an utterance not yet named
 * @model: the embedding model that made @embedding
 * @embedding: (array length=dimensions): the vector
 * @dimensions: its length
 * @error: return location for an error
 *
 * The vector is copied and scaled to unit length. A non-finite value, an
 * all-zero vector, an empty or oversized one, an invalid @name, or a missing
 * @model is %G_IO_ERROR_INVALID_DATA.
 *
 * Returns: (transfer full) (nullable): a new voiceprint
 */
AiVoiceprint *
ai_voiceprint_new(const gchar *name, const gchar *model, const gfloat *embedding,
				  gsize dimensions, GError **error)
{
	AiVoiceprint *self;
	AiVoiceprintPrivate *priv;
	gfloat *vector;
	if (model == NULL || *model == '\0' || !g_utf8_validate(model, -1, NULL)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"A voiceprint needs the model that made it");
		return NULL;
	}
	if (name != NULL && !ai_voiceprint_name_is_valid(name)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
					"Not a usable voiceprint name: \"%s\"", name);
		return NULL;
	}
	if ((vector = checked_vector(embedding, dimensions, error)) == NULL)
		return NULL;
	self = g_object_new(AI_TYPE_VOICEPRINT, "name", name, "model", model, NULL);
	priv = ai_voiceprint_get_instance_private(self);
	priv->vector = vector;
	priv->dims = dimensions;
	priv->updated = g_get_real_time() / G_USEC_PER_SEC;
	return self;
}

/**
 * ai_voiceprint_new_from_json:
 * @node: an object with "model", "embedding" (an array of numbers), and
 *   optionally "name", "count" and "updated"
 * @error: return location for an error
 *
 * Reads what ai_voiceprint_to_json() writes, and also a voiceprint service's
 * reply, which has no name. Anything malformed is %G_IO_ERROR_INVALID_DATA.
 *
 * Returns: (transfer full) (nullable): the voiceprint
 */
AiVoiceprint *
ai_voiceprint_new_from_json(JsonNode *node, GError **error)
{
	JsonObject *o = node != NULL && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node)
																 : NULL;
	JsonArray *array = o != NULL ? ai_json_get_array(o, "embedding") : NULL;
	g_autofree gfloat *vector = NULL;
	AiVoiceprint *self;
	guint i, n;
	if (o == NULL || array == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
							"Not a voiceprint: no embedding");
		return NULL;
	}
	n = json_array_get_length(array);
	if (n == 0 || n > AI_VOICEPRINT_MAX_DIMENSIONS) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
					"A voiceprint needs 1 to %u values, not %u",
					(guint)AI_VOICEPRINT_MAX_DIMENSIONS, n);
		return NULL;
	}
	vector = g_new(gfloat, n);
	for (i = 0; i < n; i++) {
		JsonNode *element = json_array_get_element(array, i);
		GType type = JSON_NODE_HOLDS_VALUE(element) ? json_node_get_value_type(element)
													: G_TYPE_INVALID;
		gdouble value;
		if (type == G_TYPE_DOUBLE)
			value = json_node_get_double(element);
		else if (type == G_TYPE_INT64)
			value = (gdouble)json_node_get_int(element);
		else {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
								"A voiceprint value is not a number");
			return NULL;
		}
		/* A double beyond a float's range would become infinity. */
		if (!isfinite(value) || fabs(value) > 3.0e38) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
								"A voiceprint value is out of range");
			return NULL;
		}
		vector[i] = (gfloat)value;
	}
	self = ai_voiceprint_new(ai_json_get_string(o, "name", NULL),
							 ai_json_get_string(o, "model", NULL), vector, n, error);
	if (self != NULL) {
		AiVoiceprintPrivate *priv = ai_voiceprint_get_instance_private(self);
		priv->count = (guint)CLAMP(ai_json_get_int(o, "count", 1), 1, G_MAXINT);
		priv->updated = MAX((gint64)0, ai_json_get_int(o, "updated", 0));
	}
	return self;
}

/**
 * ai_voiceprint_to_json:
 * @self: a voiceprint
 *
 * Returns: (transfer full): an object with "name" (when set), "model",
 *   "count", "updated" and "embedding"
 */
JsonNode *
ai_voiceprint_to_json(AiVoiceprint *self)
{
	AiVoiceprintPrivate *priv;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	gsize i;
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), NULL);
	priv = ai_voiceprint_get_instance_private(self);
	json_builder_begin_object(builder);
	if (priv->name != NULL) {
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, priv->name);
	}
	json_builder_set_member_name(builder, "model");
	json_builder_add_string_value(builder, priv->model);
	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, priv->count);
	json_builder_set_member_name(builder, "updated");
	json_builder_add_int_value(builder, priv->updated);
	json_builder_set_member_name(builder, "embedding");
	json_builder_begin_array(builder);
	for (i = 0; i < priv->dims; i++)
		json_builder_add_double_value(builder, priv->vector[i]);
	json_builder_end_array(builder);
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

/**
 * ai_voiceprint_copy:
 * @self: a voiceprint
 * @name: (nullable): the copy's name; %NULL keeps @self's
 *
 * Returns: (transfer full): an independent copy of the same class
 */
AiVoiceprint *
ai_voiceprint_copy(AiVoiceprint *self, const gchar *name)
{
	AiVoiceprintPrivate *priv, *cp;
	AiVoiceprint *copy;
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), NULL);
	priv = ai_voiceprint_get_instance_private(self);
	copy = g_object_new(G_OBJECT_TYPE(self), "name", name != NULL ? name : priv->name, "model",
						priv->model, NULL);
	cp = ai_voiceprint_get_instance_private(copy);
	cp->vector = g_memdup2(priv->vector, priv->dims * sizeof(gfloat));
	cp->dims = priv->dims;
	cp->count = priv->count;
	cp->updated = priv->updated;
	return copy;
}

/**
 * ai_voiceprint_get_name:
 * @self: a voiceprint
 *
 * Returns: (nullable): whose voice it is
 */
const gchar *
ai_voiceprint_get_name(AiVoiceprint *self)
{
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), NULL);
	return ((AiVoiceprintPrivate *)ai_voiceprint_get_instance_private(self))->name;
}

/**
 * ai_voiceprint_get_model:
 * @self: a voiceprint
 *
 * Returns: the embedding model that made it
 */
const gchar *
ai_voiceprint_get_model(AiVoiceprint *self)
{
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), NULL);
	return ((AiVoiceprintPrivate *)ai_voiceprint_get_instance_private(self))->model;
}

/**
 * ai_voiceprint_get_embedding:
 * @self: a voiceprint
 * @dimensions: (out) (optional): the vector's length
 *
 * Returns: (array length=dimensions) (transfer none): the unit-length vector
 */
const gfloat *
ai_voiceprint_get_embedding(AiVoiceprint *self, gsize *dimensions)
{
	AiVoiceprintPrivate *priv;
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), NULL);
	priv = ai_voiceprint_get_instance_private(self);
	if (dimensions != NULL)
		*dimensions = priv->dims;
	return priv->vector;
}

/**
 * ai_voiceprint_get_count:
 * @self: a voiceprint
 *
 * Returns: how many recordings were averaged into it
 */
guint
ai_voiceprint_get_count(AiVoiceprint *self)
{
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), 0);
	return ((AiVoiceprintPrivate *)ai_voiceprint_get_instance_private(self))->count;
}

/**
 * ai_voiceprint_set_count:
 * @self: a voiceprint
 * @count: how many recordings it stands for, at least 1
 */
void
ai_voiceprint_set_count(AiVoiceprint *self, guint count)
{
	g_return_if_fail(AI_IS_VOICEPRINT(self));
	g_object_set(self, "count", MAX(1u, count), NULL);
}

/**
 * ai_voiceprint_get_updated:
 * @self: a voiceprint
 *
 * Returns: when it last changed, in Unix seconds; 0 unknown
 */
gint64
ai_voiceprint_get_updated(AiVoiceprint *self)
{
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), 0);
	return ((AiVoiceprintPrivate *)ai_voiceprint_get_instance_private(self))->updated;
}

/**
 * ai_voiceprint_is_comparable:
 * @self: a voiceprint
 * @other: another
 *
 * Returns: whether both come from the same model and have the same length
 */
gboolean
ai_voiceprint_is_comparable(AiVoiceprint *self, AiVoiceprint *other)
{
	AiVoiceprintPrivate *a, *b;
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), FALSE);
	g_return_val_if_fail(AI_IS_VOICEPRINT(other), FALSE);
	a = ai_voiceprint_get_instance_private(self);
	b = ai_voiceprint_get_instance_private(other);
	return a->dims == b->dims && g_strcmp0(a->model, b->model) == 0;
}

/**
 * ai_voiceprint_similarity:
 * @self: a voiceprint
 * @other: another from the same model
 *
 * Returns: -1 to 1, higher more alike; -1 when the two are not comparable
 */
gdouble
ai_voiceprint_similarity(AiVoiceprint *self, AiVoiceprint *other)
{
	gdouble score;
	g_return_val_if_fail(AI_IS_VOICEPRINT(self), -1);
	g_return_val_if_fail(AI_IS_VOICEPRINT(other), -1);
	if (!ai_voiceprint_is_comparable(self, other))
		return -1;
	score = AI_VOICEPRINT_GET_CLASS(self)->similarity(self, other);
	return isfinite(score) ? CLAMP(score, -1.0, 1.0) : -1;
}

/**
 * ai_voiceprint_absorb:
 * @self: a voiceprint
 * @other: another from the same model, to average in
 * @max_count: the most weight @self's history may carry, 0 for no limit
 *
 * Folds @other into @self as a running mean weighted by both counts, then
 * renormalizes. A limit keeps a voiceprint adapting to a changing room
 * instead of freezing on what it heard first. Incomparable voiceprints are
 * left alone.
 */
void
ai_voiceprint_absorb(AiVoiceprint *self, AiVoiceprint *other, guint max_count)
{
	AiVoiceprintPrivate *a, *b;
	guint weight;
	gsize k;
	g_return_if_fail(AI_IS_VOICEPRINT(self));
	g_return_if_fail(AI_IS_VOICEPRINT(other));
	if (!ai_voiceprint_is_comparable(self, other))
		return;
	a = ai_voiceprint_get_instance_private(self);
	b = ai_voiceprint_get_instance_private(other);
	weight = max_count > 0 ? MIN(a->count, max_count) : a->count;
	for (k = 0; k < a->dims; k++)
		a->vector[k] = (gfloat)(((gdouble)a->vector[k] * weight + (gdouble)b->vector[k] * b->count) /
								(gdouble)(weight + b->count));
	ai_embedding_normalize(a->vector, a->dims);
	a->count = weight + b->count;
	if (max_count > 0)
		a->count = MIN(a->count, max_count);
	a->updated = g_get_real_time() / G_USEC_PER_SEC;
}

/**
 * ai_voiceprint_name_is_valid:
 * @name: (nullable): a candidate name
 *
 * A name becomes a file name and a prefix the model reads ("[Sam]: ..."),
 * so it is 1 to %AI_VOICEPRINT_NAME_MAX bytes of valid UTF-8, does not
 * start with a dot or a space, and holds no control character, path
 * separator, bracket or colon. It may not take the form "Guest N", which
 * belongs to voices nobody enrolled.
 *
 * Returns: whether @name may name a voiceprint
 */
gboolean
ai_voiceprint_name_is_valid(const gchar *name)
{
	const gchar *p;
	gsize len;
	if (name == NULL || *name == '\0' || *name == '.' || *name == ' ')
		return FALSE;
	len = strlen(name);
	if (len > AI_VOICEPRINT_NAME_MAX || !g_utf8_validate(name, (gssize)len, NULL))
		return FALSE;
	if (name[len - 1] == ' ')
		return FALSE;
	for (p = name; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);
		if (g_unichar_iscntrl(c) || c == '/' || c == '\\' || c == '[' || c == ']' || c == ':' ||
			g_unichar_type(c) == G_UNICODE_FORMAT || g_unichar_type(c) == G_UNICODE_LINE_SEPARATOR ||
			g_unichar_type(c) == G_UNICODE_PARAGRAPH_SEPARATOR)
			return FALSE;
	}
	return !ai_voiceprint_name_is_guest(name, NULL);
}

/**
 * ai_voiceprint_name_is_guest:
 * @name: (nullable): a name
 * @number: (out) (optional): the guest's number
 *
 * Returns: whether @name is "Guest N" (any case), the name of the Nth voice
 *   heard that nobody enrolled
 */
gboolean
ai_voiceprint_name_is_guest(const gchar *name, guint *number)
{
	guint64 n;
	gchar *end = NULL;
	if (name == NULL || g_ascii_strncasecmp(name, "guest ", 6) != 0 ||
		!g_ascii_isdigit(name[6]))
		return FALSE;
	n = g_ascii_strtoull(name + 6, &end, 10);
	if (end == NULL || *end != '\0' || n == 0 || n > G_MAXUINT)
		return FALSE;
	if (number != NULL)
		*number = (guint)n;
	return TRUE;
}
