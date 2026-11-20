/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "voice/ai-voiceprint-store.h"

/*
 * The store interface, and the simplest store: voiceprints in a hash table
 * for the life of the process. The wrappers validate names and argument
 * shape once, so an implementation only ever sees a valid name and a named
 * voiceprint.
 */
G_DEFINE_INTERFACE(AiVoiceprintStore, ai_voiceprint_store, G_TYPE_OBJECT)
static void
ai_voiceprint_store_default_init(AiVoiceprintStoreInterface *iface)
{
	/**
	 * AiVoiceprintStore::changed:
	 * @self: the store
	 *
	 * Voiceprints were saved, removed or renamed, by this process or
	 * another. An identifier reloads on it.
	 */
	g_signal_new("changed", G_TYPE_FROM_INTERFACE(iface), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
				 NULL, G_TYPE_NONE, 0);
}

static gboolean
check_name(const gchar *name, GError **error)
{
	if (ai_voiceprint_name_is_valid(name))
		return TRUE;
	g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				"Not a usable voiceprint name: \"%s\"", name != NULL ? name : "");
	return FALSE;
}

/**
 * ai_voiceprint_store_list:
 * @self: a store
 *
 * Returns: (transfer container) (element-type AiVoiceprint): every enrolled
 *   voiceprint, sorted by name
 */
GPtrArray *
ai_voiceprint_store_list(AiVoiceprintStore *self)
{
	AiVoiceprintStoreInterface *iface;
	g_return_val_if_fail(AI_IS_VOICEPRINT_STORE(self), NULL);
	iface = AI_VOICEPRINT_STORE_GET_IFACE(self);
	if (iface->list == NULL)
		return g_ptr_array_new_with_free_func(g_object_unref);
	return iface->list(self);
}

/**
 * ai_voiceprint_store_lookup:
 * @self: a store
 * @name: a name
 *
 * Returns: (transfer full) (nullable): @name's voiceprint
 */
AiVoiceprint *
ai_voiceprint_store_lookup(AiVoiceprintStore *self, const gchar *name)
{
	AiVoiceprintStoreInterface *iface;
	g_return_val_if_fail(AI_IS_VOICEPRINT_STORE(self), NULL);
	if (!ai_voiceprint_name_is_valid(name))
		return NULL;
	iface = AI_VOICEPRINT_STORE_GET_IFACE(self);
	return iface->lookup != NULL ? iface->lookup(self, name) : NULL;
}

/**
 * ai_voiceprint_store_save:
 * @self: a store
 * @voiceprint: a named voiceprint
 * @error: return location for an error
 *
 * Stores @voiceprint under its name, replacing any voiceprint of that name.
 *
 * Returns: whether it was saved
 */
gboolean
ai_voiceprint_store_save(AiVoiceprintStore *self, AiVoiceprint *voiceprint, GError **error)
{
	AiVoiceprintStoreInterface *iface;
	g_return_val_if_fail(AI_IS_VOICEPRINT_STORE(self), FALSE);
	g_return_val_if_fail(AI_IS_VOICEPRINT(voiceprint), FALSE);
	if (!check_name(ai_voiceprint_get_name(voiceprint), error))
		return FALSE;
	iface = AI_VOICEPRINT_STORE_GET_IFACE(self);
	if (iface->save == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY,
							"This voiceprint store is read-only");
		return FALSE;
	}
	return iface->save(self, voiceprint, error);
}

/**
 * ai_voiceprint_store_remove:
 * @self: a store
 * @name: whom to forget
 * @error: return location for an error
 *
 * A name that is not enrolled is %G_IO_ERROR_NOT_FOUND.
 *
 * Returns: whether @name was forgotten
 */
gboolean
ai_voiceprint_store_remove(AiVoiceprintStore *self, const gchar *name, GError **error)
{
	AiVoiceprintStoreInterface *iface;
	g_return_val_if_fail(AI_IS_VOICEPRINT_STORE(self), FALSE);
	if (!check_name(name, error))
		return FALSE;
	iface = AI_VOICEPRINT_STORE_GET_IFACE(self);
	if (iface->remove == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY,
							"This voiceprint store is read-only");
		return FALSE;
	}
	return iface->remove(self, name, error);
}

/**
 * ai_voiceprint_store_rename:
 * @self: a store
 * @from: an enrolled name
 * @to: a name not enrolled
 * @error: return location for an error
 *
 * An unknown @from is %G_IO_ERROR_NOT_FOUND; a @to already enrolled is
 * %G_IO_ERROR_EXISTS.
 *
 * Returns: whether the voiceprint was renamed
 */
gboolean
ai_voiceprint_store_rename(AiVoiceprintStore *self, const gchar *from, const gchar *to,
						   GError **error)
{
	AiVoiceprintStoreInterface *iface;
	g_return_val_if_fail(AI_IS_VOICEPRINT_STORE(self), FALSE);
	if (!check_name(from, error) || !check_name(to, error))
		return FALSE;
	if (g_str_equal(from, to))
		return TRUE;
	iface = AI_VOICEPRINT_STORE_GET_IFACE(self);
	if (iface->rename == NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_READ_ONLY,
							"This voiceprint store is read-only");
		return FALSE;
	}
	return iface->rename(self, from, to, error);
}

/**
 * ai_voiceprint_store_emit_changed:
 * @self: a store
 *
 * For implementations: announce that voiceprints changed.
 */
void
ai_voiceprint_store_emit_changed(AiVoiceprintStore *self)
{
	g_return_if_fail(AI_IS_VOICEPRINT_STORE(self));
	g_signal_emit_by_name(self, "changed");
}

/* ---- AiMemoryVoiceprintStore ---- */

typedef struct {
	GHashTable *voices; /* name -> AiVoiceprint */
} AiMemoryVoiceprintStorePrivate;

static void memory_store_iface_init(AiVoiceprintStoreInterface *iface);
G_DEFINE_TYPE_WITH_CODE(AiMemoryVoiceprintStore, ai_memory_voiceprint_store, G_TYPE_OBJECT,
						G_ADD_PRIVATE(AiMemoryVoiceprintStore)
							G_IMPLEMENT_INTERFACE(AI_TYPE_VOICEPRINT_STORE,
												  memory_store_iface_init))

static GHashTable *
voices_of(AiVoiceprintStore *store)
{
	AiMemoryVoiceprintStorePrivate *priv =
		ai_memory_voiceprint_store_get_instance_private(AI_MEMORY_VOICEPRINT_STORE(store));
	return priv->voices;
}
static gint
by_name(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(ai_voiceprint_get_name(*(AiVoiceprint *const *)a),
					 ai_voiceprint_get_name(*(AiVoiceprint *const *)b));
}
static GPtrArray *
memory_list(AiVoiceprintStore *store)
{
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	GHashTableIter iter;
	gpointer value;
	g_hash_table_iter_init(&iter, voices_of(store));
	while (g_hash_table_iter_next(&iter, NULL, &value))
		g_ptr_array_add(out, ai_voiceprint_copy(value, NULL));
	g_ptr_array_sort(out, by_name);
	return out;
}
static AiVoiceprint *
memory_lookup(AiVoiceprintStore *store, const gchar *name)
{
	AiVoiceprint *v = g_hash_table_lookup(voices_of(store), name);
	return v != NULL ? ai_voiceprint_copy(v, NULL) : NULL;
}
static gboolean
memory_save(AiVoiceprintStore *store, AiVoiceprint *voiceprint, GError **error)
{
	g_hash_table_replace(voices_of(store), g_strdup(ai_voiceprint_get_name(voiceprint)),
						 ai_voiceprint_copy(voiceprint, NULL));
	ai_voiceprint_store_emit_changed(store);
	return TRUE;
}
static gboolean
memory_remove(AiVoiceprintStore *store, const gchar *name, GError **error)
{
	if (!g_hash_table_remove(voices_of(store), name)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Nobody called %s is enrolled",
					name);
		return FALSE;
	}
	ai_voiceprint_store_emit_changed(store);
	return TRUE;
}
static gboolean
memory_rename(AiVoiceprintStore *store, const gchar *from, const gchar *to, GError **error)
{
	GHashTable *voices = voices_of(store);
	AiVoiceprint *v = g_hash_table_lookup(voices, from);
	if (v == NULL) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Nobody called %s is enrolled",
					from);
		return FALSE;
	}
	if (g_hash_table_contains(voices, to)) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "%s is already enrolled", to);
		return FALSE;
	}
	g_hash_table_replace(voices, g_strdup(to), ai_voiceprint_copy(v, to));
	g_hash_table_remove(voices, from);
	ai_voiceprint_store_emit_changed(store);
	return TRUE;
}
static void
memory_store_iface_init(AiVoiceprintStoreInterface *iface)
{
	iface->list = memory_list;
	iface->lookup = memory_lookup;
	iface->save = memory_save;
	iface->remove = memory_remove;
	iface->rename = memory_rename;
}
static void
memory_finalize(GObject *object)
{
	AiMemoryVoiceprintStorePrivate *priv =
		ai_memory_voiceprint_store_get_instance_private(AI_MEMORY_VOICEPRINT_STORE(object));
	g_hash_table_unref(priv->voices);
	G_OBJECT_CLASS(ai_memory_voiceprint_store_parent_class)->finalize(object);
}
static void
ai_memory_voiceprint_store_class_init(AiMemoryVoiceprintStoreClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = memory_finalize;
}
static void
ai_memory_voiceprint_store_init(AiMemoryVoiceprintStore *self)
{
	AiMemoryVoiceprintStorePrivate *priv = ai_memory_voiceprint_store_get_instance_private(self);
	priv->voices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}

/**
 * ai_memory_voiceprint_store_new:
 *
 * Returns: (transfer full): an empty store that keeps voiceprints in memory
 */
AiMemoryVoiceprintStore *
ai_memory_voiceprint_store_new(void)
{
	return g_object_new(AI_TYPE_MEMORY_VOICEPRINT_STORE, NULL);
}
