/*
 * ai-gui-util.h - Small shared helpers
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define AI_GUI_AGENT_MAX 4

/**
 * AiGuiOptions:
 * @provider: (nullable): provider name from the command line
 * @model: (nullable): model id from the command line
 * @system: (nullable): system prompt
 * @effort: (nullable): reasoning effort for a CLI provider
 * @working_directory: (nullable): where mentions and tools resolve
 * @sets: (nullable): `PROP=VALUE` provider property overrides
 * @max_tokens: response bound
 * @stream: whether to stream a turn
 * @continue_session: resume the provider's most recent session
 * @skip_permissions: let a wrapped CLI run its tools unattended
 * @local_tools: run tools in this process (HTTP providers only)
 * @approve_all: approve every local tool call without asking
 * @expand: resolve `@` mentions and `/` commands
 * @agents: let the model start background agents
 *
 * The command line, in one struct.
 *
 * Every new session in the window starts from this, so a flag given once
 * at startup keeps applying to sessions created later — which is what a
 * person means by passing it.  The preferences dialog writes back into
 * the same struct, so the two cannot disagree.
 */
typedef struct
{
	gchar    *provider;
	gchar    *model;
	gchar    *system;
	gchar    *effort;
	gchar    *working_directory;
	gchar   **sets;
	gint      max_tokens;
	gboolean  stream;
	gboolean  continue_session;
	gboolean  skip_permissions;
	gboolean  local_tools;
	gboolean  approve_all;
	gboolean  expand;
	gboolean  agents;
} AiGuiOptions;

AiGuiOptions *
ai_gui_options_copy(const AiGuiOptions *self);

void
ai_gui_options_free(AiGuiOptions *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(AiGuiOptions, ai_gui_options_free)

/**
 * ai_gui_value_from_string:
 * @value: (out caller-allocates): an uninitialised #GValue
 * @pspec: the property being written
 * @text: the text to parse
 *
 * Parses @text into @value according to @pspec's type.
 *
 * The same vocabulary `ai --set` accepts, on purpose: a person who has
 * learned that `--set effort-level=high` works should not have to learn a
 * second spelling to type it into the preferences dialog.
 *
 * Returns: %TRUE if @text parsed, %FALSE if it did not
 */
gboolean
ai_gui_value_from_string(
	GValue      *value,
	GParamSpec  *pspec,
	const gchar *text
);

/**
 * ai_gui_value_to_string:
 * @object: the object holding the property
 * @pspec: the property to read
 *
 * Returns: (transfer full): @pspec's current value, spelled the way
 *   ai_gui_value_from_string() would read it back
 */
gchar *
ai_gui_value_to_string(
	GObject    *object,
	GParamSpec *pspec
);

/**
 * ai_gui_format_relative_time:
 * @unix_time: seconds since the epoch, or 0
 *
 * Returns: (transfer full): "just now", "12m ago", "3d ago", or a date
 */
gchar *
ai_gui_format_relative_time(gint64 unix_time);

/**
 * ai_gui_summarise_prompt:
 * @text: (nullable): what the user typed
 *
 * Turns a prompt into something that fits in a sidebar row.
 *
 * Returns: (transfer full): a single line, elided, never %NULL
 */
gchar *
ai_gui_summarise_prompt(const gchar *text);

G_END_DECLS
