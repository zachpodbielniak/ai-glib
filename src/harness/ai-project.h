/*
 * ai-project.h - The work registry, grouped by project
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <gio/gio.h>

G_BEGIN_DECLS

#define AI_TYPE_PROJECT (ai_project_get_type())

G_DECLARE_FINAL_TYPE(AiProject, ai_project, AI, PROJECT, GObject)

gchar *
ai_project_label_for_path(
	const gchar *project
);

gint
ai_project_compare_paths(
	const gchar *a,
	const gchar *b
);

GList *
ai_project_list(
	const gchar  *directory,
	GError      **error
);

AiProject *
ai_project_find(
	GList        *projects,
	const gchar  *query,
	GError      **error
);

const gchar *
ai_project_get_id(
	AiProject *self
);

const gchar *
ai_project_get_name(
	AiProject *self
);

const gchar *
ai_project_get_root(
	AiProject *self
);

const gchar *
ai_project_get_status(
	AiProject *self
);

guint
ai_project_get_session_count(
	AiProject *self
);

guint
ai_project_get_live_count(
	AiProject *self
);

guint
ai_project_get_busy_count(
	AiProject *self
);

guint
ai_project_get_attention_count(
	AiProject *self
);

GPtrArray *
ai_project_dup_sessions(
	AiProject *self
);

G_END_DECLS
