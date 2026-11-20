/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
#include "voice/ai-voiceprint.h"
G_BEGIN_DECLS
#define AI_TYPE_VOICEPRINT_STORE (ai_voiceprint_store_get_type())
G_DECLARE_INTERFACE(AiVoiceprintStore, ai_voiceprint_store, AI, VOICEPRINT_STORE, GObject)
/**
 * AiVoiceprintStoreInterface:
 * @parent_iface: the parent interface
 * @list: every enrolled voiceprint
 * @lookup: one voiceprint by name, or %NULL
 * @save: store a named voiceprint, replacing any of that name
 * @remove: forget a name
 * @rename: move a voiceprint to a new name, which must be free
 *
 * Where enrolled voiceprints live. #AiFileVoiceprintStore keeps one file
 * per person; #AiMemoryVoiceprintStore keeps them for the process. An
 * application can keep them anywhere (a database, a keyring) by
 * implementing this. Implementations emit #AiVoiceprintStore::changed after
 * any change, including one made by another process.
 *
 * Voiceprints are biometric data: an implementation should keep them
 * readable by their owner only.
 */
struct _AiVoiceprintStoreInterface {
	GTypeInterface parent_iface;
	GPtrArray *(*list)(AiVoiceprintStore *self);
	AiVoiceprint *(*lookup)(AiVoiceprintStore *self, const gchar *name);
	gboolean (*save)(AiVoiceprintStore *self, AiVoiceprint *voiceprint, GError **error);
	gboolean (*remove)(AiVoiceprintStore *self, const gchar *name, GError **error);
	gboolean (*rename)(AiVoiceprintStore *self, const gchar *from, const gchar *to,
					   GError **error);
	gpointer _reserved[8];
};

GPtrArray *
ai_voiceprint_store_list(AiVoiceprintStore *self);
AiVoiceprint *
ai_voiceprint_store_lookup(AiVoiceprintStore *self, const gchar *name);
gboolean
ai_voiceprint_store_save(AiVoiceprintStore *self, AiVoiceprint *voiceprint, GError **error);
gboolean
ai_voiceprint_store_remove(AiVoiceprintStore *self, const gchar *name, GError **error);
gboolean
ai_voiceprint_store_rename(AiVoiceprintStore *self, const gchar *from, const gchar *to,
						   GError **error);
void
ai_voiceprint_store_emit_changed(AiVoiceprintStore *self);

#define AI_TYPE_MEMORY_VOICEPRINT_STORE (ai_memory_voiceprint_store_get_type())
G_DECLARE_DERIVABLE_TYPE(AiMemoryVoiceprintStore, ai_memory_voiceprint_store, AI,
						 MEMORY_VOICEPRINT_STORE, GObject)
/**
 * AiMemoryVoiceprintStoreClass:
 * @parent_class: the parent class
 */
struct _AiMemoryVoiceprintStoreClass {
	GObjectClass parent_class;
	gpointer _reserved[8];
};
AiMemoryVoiceprintStore *
ai_memory_voiceprint_store_new(void);
G_END_DECLS
