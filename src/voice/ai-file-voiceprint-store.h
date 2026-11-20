/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include "voice/ai-voiceprint-store.h"
G_BEGIN_DECLS
#define AI_TYPE_FILE_VOICEPRINT_STORE (ai_file_voiceprint_store_get_type())
G_DECLARE_DERIVABLE_TYPE(AiFileVoiceprintStore, ai_file_voiceprint_store, AI,
						 FILE_VOICEPRINT_STORE, GObject)
/**
 * AiFileVoiceprintStoreClass:
 * @parent_class: the parent class
 */
struct _AiFileVoiceprintStoreClass {
	GObjectClass parent_class;
	gpointer _reserved[8];
};

/**
 * AI_FILE_VOICEPRINT_STORE_MAX_FILE:
 *
 * The largest voiceprint file read, in bytes.
 */
#define AI_FILE_VOICEPRINT_STORE_MAX_FILE ((gsize)256 * 1024)

AiFileVoiceprintStore *
ai_file_voiceprint_store_new(const gchar *directory);
const gchar *
ai_file_voiceprint_store_get_directory(AiFileVoiceprintStore *self);
G_END_DECLS
