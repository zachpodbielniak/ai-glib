/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once

#include <glib.h>

/* Unknown tools, shell commands, skills and delegation cannot inherit a
 * read-only grant merely because their current arguments look harmless. */
static inline gboolean
ai_plan_tool_is_read_only(const gchar *name)
{
	static const gchar * const allowed[] = {
		"read", "ls", "glob", "grep", "web_fetch", "web_search", NULL
	};
	return name != NULL && g_strv_contains(allowed, name);
}
