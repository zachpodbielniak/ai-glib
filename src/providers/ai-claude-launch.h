/*
 * ai-claude-launch.h - Ollama-as-transport launcher helpers for the
 *                      Claude Code CLI providers
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API. Shared
 * between the claude-code (--print) and claude-tmux providers so the
 * "ollama/<model>" transport rule lives in exactly one place.
 *
 * Ollama can drive the Claude Code CLI as its transport:
 *
 *     ollama launch claude --model <ollama_model> -- <claude args>
 *
 * When a CLI provider's model string starts with "ollama/", these helpers
 * rewrite the spawn so that instead of running `claude <args>` directly we
 * run `ollama launch claude --model <suffix> -- <args>`. The claude binary
 * is still required on PATH (ollama execs it); only the launcher changes.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-claude-launch.h is an internal header"
#endif

#include <glib.h>

G_BEGIN_DECLS

/*
 * Returns TRUE iff @model selects the Ollama transport, i.e. it is
 * non-NULL, begins with the literal prefix "ollama/" AND has at least one
 * character after the slash. Case-sensitive; no whitespace trimming:
 *   "ollama"      -> FALSE (no slash)
 *   "ollama/"     -> FALSE (empty suffix)
 *   "ollama/x"    -> TRUE
 *   "Ollama/x"    -> FALSE (case-sensitive)
 *   "x-ollama/y"  -> FALSE (must be a true prefix, not a substring)
 */
gboolean
ai_claude_launch_model_is_ollama(const gchar *model);

/*
 * Returns a borrowed pointer into @model just past the "ollama/" prefix
 * (the Ollama model id passed to `ollama launch --model`), or NULL when
 * @model is not an Ollama-transport model. (transfer none)
 */
const gchar *
ai_claude_launch_ollama_model(const gchar *model);

/*
 * Returns the executable NAME (not a resolved path) that should be spawned
 * for @model:
 *   - Ollama model -> the OLLAMA_PATH env var if set & non-empty, else "ollama"
 *   - otherwise    -> the CLAUDE_CODE_PATH env var if set & non-empty, else "claude"
 * (transfer full) -- caller frees with g_free().
 */
gchar *
ai_claude_launch_executable_name(const gchar *model);

/*
 * Returns TRUE when the caller should emit its own claude "--model <model>"
 * argument. FALSE in Ollama mode, where the model is carried solely by
 * `ollama launch --model <suffix>` and re-passing it to claude would be
 * wrong. Equivalent to !ai_claude_launch_model_is_ollama(@model); named for
 * self-documenting call sites.
 */
gboolean
ai_claude_launch_should_emit_claude_model(const gchar *model);

/*
 * Appends the launcher prefix tokens (each independently g_strdup'd) to
 * @argv, a GPtrArray of strings. @program is the token placed at the
 * program position (argv element to be exec'd, or, in the base CLI
 * pipeline, the placeholder later overwritten with the resolved path).
 *
 *   claude mode -> [program]
 *   ollama mode -> [program, "launch", "claude", "--model", <suffix>, "--"]
 *
 * Call this FIRST when building an argv, then append the claude-arg tail
 * (omitting claude's own "--model" when in Ollama mode).
 */
void
ai_claude_launch_emit_tokens(
    GPtrArray   *argv,
    const gchar *model,
    const gchar *program
);

G_END_DECLS
