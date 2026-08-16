/*
 * ai-command.h - Slash commands, from disk and from the library
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

#include <glib-object.h>
#include <gio/gio.h>

#include "harness/ai-resource.h"
#include "harness/ai-resource-registry.h"

G_BEGIN_DECLS

/**
 * AiCommandKind:
 * @AI_COMMAND_BUILTIN: the frontend acts; nothing is sent to the model
 * @AI_COMMAND_PROMPT: expands to text, which is sent
 * @AI_COMMAND_AGENT: dispatches a subagent
 *
 * What invoking a command does.
 */
typedef enum
{
    AI_COMMAND_BUILTIN = 0,
    AI_COMMAND_PROMPT,
    AI_COMMAND_AGENT
} AiCommandKind;

/**
 * AiCommandShellPolicy:
 * @AI_COMMAND_SHELL_OPT_IN: run only when the file declares `shell: true`
 * @AI_COMMAND_SHELL_NEVER: never run; the backticks stay literal
 * @AI_COMMAND_SHELL_ALWAYS: run in every command body
 *
 * Whether a command body's `` !`cmd` `` may execute.
 *
 * The default is %AI_COMMAND_SHELL_OPT_IN. Scanning a directory must not
 * be enough to make the files in it executable, and these directories are
 * shared with other tools --- anything that can write a file into
 * `~/.claude/commands` would otherwise be able to run code the next time
 * a listing is built.
 */
typedef enum
{
    AI_COMMAND_SHELL_OPT_IN = 0,
    AI_COMMAND_SHELL_NEVER,
    AI_COMMAND_SHELL_ALWAYS
} AiCommandShellPolicy;

/**
 * AiCommandOutcome:
 * @AI_COMMAND_OUTCOME_NOT_A_COMMAND: the line is an ordinary prompt
 * @AI_COMMAND_OUTCOME_BUILTIN: the frontend must handle it
 * @AI_COMMAND_OUTCOME_PROMPT: send the expanded text to the model
 * @AI_COMMAND_OUTCOME_AGENT: run the named agent with the expanded text
 *
 * What a caller should do with a resolved line.
 */
typedef enum
{
    AI_COMMAND_OUTCOME_NOT_A_COMMAND = 0,
    AI_COMMAND_OUTCOME_BUILTIN,
    AI_COMMAND_OUTCOME_PROMPT,
    AI_COMMAND_OUTCOME_AGENT
} AiCommandOutcome;

/* ---- AiCommand ---- */

#define AI_TYPE_COMMAND (ai_command_get_type())

G_DECLARE_FINAL_TYPE(AiCommand, ai_command, AI, COMMAND, GObject)

AiCommand *
ai_command_new_builtin(
    const gchar *name,
    const gchar *description,
    const gchar *argument_hint
);

AiCommand *
ai_command_new_for_resource(AiResource *resource);

const gchar *
ai_command_get_name(AiCommand *self);

const gchar *
ai_command_get_description(AiCommand *self);

const gchar *
ai_command_get_argument_hint(AiCommand *self);

AiCommandKind
ai_command_get_kind(AiCommand *self);

AiResource *
ai_command_get_resource(AiCommand *self);

const gchar *
ai_command_get_origin(AiCommand *self);

/* ---- AiCommandResult ---- */

#define AI_TYPE_COMMAND_RESULT (ai_command_result_get_type())

G_DECLARE_FINAL_TYPE(AiCommandResult, ai_command_result,
                     AI, COMMAND_RESULT, GObject)

AiCommandOutcome
ai_command_result_get_outcome(AiCommandResult *self);

AiCommand *
ai_command_result_get_command(AiCommandResult *self);

const gchar *
ai_command_result_get_name(AiCommandResult *self);

const gchar *
ai_command_result_get_arguments(AiCommandResult *self);

const gchar *
ai_command_result_get_prompt(AiCommandResult *self);

/* ---- AiCommandSet ---- */

#define AI_TYPE_COMMAND_SET (ai_command_set_get_type())

G_DECLARE_FINAL_TYPE(AiCommandSet, ai_command_set, AI, COMMAND_SET, GObject)

AiCommandSet *
ai_command_set_new(AiResourceRegistry *registry);

AiResourceRegistry *
ai_command_set_get_registry(AiCommandSet *self);

void
ai_command_set_set_shell_policy(
    AiCommandSet         *self,
    AiCommandShellPolicy  policy
);

AiCommandShellPolicy
ai_command_set_get_shell_policy(AiCommandSet *self);

AiCommand *
ai_command_set_lookup(
    AiCommandSet *self,
    const gchar  *name
);

GList *
ai_command_set_list(AiCommandSet *self);

gboolean
ai_command_set_is_command_line(const gchar *line);

AiCommandResult *
ai_command_set_resolve(
    AiCommandSet  *self,
    const gchar   *line,
    const gchar   *cwd,
    GCancellable  *cancellable,
    GError       **error
);

gchar **
ai_command_split_arguments(const gchar *arguments);

gchar *
ai_command_substitute(
    const gchar        *body,
    const gchar        *arguments,
    const gchar *const *argv
);

G_END_DECLS
