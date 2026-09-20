/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
#include <gio/gio.h>
G_BEGIN_DECLS
#define AI_TYPE_WORK_SESSION (ai_work_session_get_type())
G_DECLARE_FINAL_TYPE(AiWorkSession, ai_work_session, AI, WORK_SESSION, GObject)
AiWorkSession *ai_work_session_new(const gchar *directory);
const gchar *ai_work_session_get_id(AiWorkSession *self);
const gchar *ai_work_session_get_field(AiWorkSession *self, const gchar *field);
void ai_work_session_update(AiWorkSession *self, gboolean busy, gboolean attention,
						   guint background, const gchar *outcome);
gboolean ai_work_session_add_link(AiWorkSession *self, const gchar *url, GError **error);
gboolean ai_work_session_remove_link(AiWorkSession *self, const gchar *url);
gchar **ai_work_session_dup_links(AiWorkSession *self);
gboolean ai_work_session_save(AiWorkSession *self, const gchar *directory, gboolean live, GError **error);
GList *ai_work_session_list(const gchar *directory, GError **error);
gchar *ai_work_session_default_directory(void);
void ai_work_session_refresh_link_async(AiWorkSession *self, const gchar *url,
	GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
gchar *ai_work_session_refresh_link_finish(AiWorkSession *self, GAsyncResult *result, GError **error);
const gchar *ai_work_session_get_link_title(AiWorkSession *self, const gchar *url);
const gchar *ai_work_session_get_link_state(AiWorkSession *self, const gchar *url);
G_END_DECLS
