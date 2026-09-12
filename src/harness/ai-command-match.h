/*
 * ai-command-match.h - Fuzzy matching for slash-command names
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API. Shared
 * by command completion and the unknown-command suggestion list so a
 * fragment like "git" can find skill-git-worktree in both places.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-command-match.h is an internal header"
#endif

#include <string.h>
#include <glib.h>

/* Lower is a better match. AI_COMMAND_FUZZY_NONE means no match. */
#define AI_COMMAND_FUZZY_NONE        (-1)
#define AI_COMMAND_FUZZY_EXACT         (0)
#define AI_COMMAND_FUZZY_PREFIX        (1)
#define AI_COMMAND_FUZZY_SEGMENT       (2)
#define AI_COMMAND_FUZZY_SUBSTRING     (3)
#define AI_COMMAND_FUZZY_SUBSEQUENCE   (4)

static inline gboolean
ai_command_is_segment_char(gchar c)
{
	return c == '-' || c == '_' || c == ':';
}

/* Every character of @needle appears in @name, in order, not necessarily
 * adjacent. That is what makes `/gwt` find `skill-git-worktree`. */
static inline gboolean
ai_command_is_subsequence(const gchar *name, const gchar *needle)
{
	const gchar *n;
	const gchar *p;

	n = name;

	for (p = needle; *p != '\0'; p++)
	{
		n = strchr(n, *p);

		if (n == NULL)
		{
			return FALSE;
		}

		n++;
	}

	return TRUE;
}

/**
 * ai_command_fuzzy_score:
 * @name: a command, skill or agent name, without the slash
 * @needle: what the user typed after `/`
 *
 * How well @needle matches @name. Case-sensitive, like lookup.
 *
 * An empty needle matches everything --- a bare `/` is "show me the
 * list", not "match the empty string".
 *
 * Returns: a rank from %AI_COMMAND_FUZZY_EXACT to
 *   %AI_COMMAND_FUZZY_SUBSEQUENCE, or %AI_COMMAND_FUZZY_NONE
 */
static inline gint
ai_command_fuzzy_score(const gchar *name, const gchar *needle)
{
	const gchar *found;

	if (name == NULL || needle == NULL)
	{
		return AI_COMMAND_FUZZY_NONE;
	}

	if (needle[0] == '\0')
	{
		return AI_COMMAND_FUZZY_PREFIX;
	}

	if (g_strcmp0(name, needle) == 0)
	{
		return AI_COMMAND_FUZZY_EXACT;
	}

	if (g_str_has_prefix(name, needle))
	{
		return AI_COMMAND_FUZZY_PREFIX;
	}

	found = strstr(name, needle);

	if (found != NULL)
	{
		/* A hit at a hyphen, underscore or colon is the usual shape of
		 * a skill name: `/git` against `skill-git-worktree`. */
		if (found == name || ai_command_is_segment_char(*(found - 1)))
		{
			return AI_COMMAND_FUZZY_SEGMENT;
		}

		return AI_COMMAND_FUZZY_SUBSTRING;
	}

	if (ai_command_is_subsequence(name, needle))
	{
		return AI_COMMAND_FUZZY_SUBSEQUENCE;
	}

	return AI_COMMAND_FUZZY_NONE;
}
