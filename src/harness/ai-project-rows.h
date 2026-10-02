/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <ai-glib.h>

/* Shared dashboard projection. Recent projects stay openable after their last
 * process exits. A placeholder is presentation data, never a saved session. */
static inline GPtrArray *
ai_project_rows(const gchar *directory, GError **error)
{
	g_autofree gchar *fallback = directory == NULL ? ai_work_session_default_directory() : NULL;
	g_autolist(AiProject) projects = NULL;
	g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_object_unref);
	GList *l;
	if (directory == NULL) directory = fallback;
	ai_work_session_prune(directory, error);
	if (error != NULL && *error != NULL) return NULL;
	projects = ai_project_list(directory, error);
	if (error != NULL && *error != NULL) return NULL;
	for (l = projects; l != NULL; l = l->next) {
		AiProject *project = l->data;
		g_autoptr(GPtrArray) sessions = ai_project_dup_sessions(project);
		guint i;
		if (sessions->len == 0) {
			g_autoptr(AiWorkSession) row = g_object_new(AI_TYPE_WORK_SESSION,
				"project", ai_project_get_id(project), "directory", ai_project_get_root(project),
				"status", "PROJECT", "title", "Open project", "activity", "No active conversations. Open to start working.", NULL);
			g_object_set_data(G_OBJECT(row), "ai-project-placeholder", GINT_TO_POINTER(1));
			g_ptr_array_add(rows, g_steal_pointer(&row));
		}
		for (i = 0; i < sessions->len; i++)
			g_ptr_array_add(rows, g_object_ref(g_ptr_array_index(sessions, i)));
	}
	return g_steal_pointer(&rows);
}

/* Placeholders have no persisted UUID; the project identity keeps selection
 * stable across refreshes of the recent-project list. */
static inline const gchar *
ai_project_row_key(AiWorkSession *row)
{
	return g_object_get_data(G_OBJECT(row), "ai-project-placeholder") != NULL
		? ai_work_session_get_field(row, "project") : ai_work_session_get_id(row);
}
