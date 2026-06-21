/*
 * ai-claude-tmux-client-internal.h - test/introspection seam for the
 *                                    Claude Code (tmux) client
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API. Exposes
 * the tmux `new-session` argv builder so unit tests (and the `ai` CLI's
 * --dry-run) can assert the exact command line that would be spawned,
 * including the Ollama transport rewrite, without launching tmux/claude.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-claude-tmux-client-internal.h is an internal header"
#endif

#include <glib.h>

G_BEGIN_DECLS

/*
 * Build the argv for
 *   tmux new-session -d -s NAME -c CWD -- <program> <claude args>
 *
 * For a normal model <program> is @claude_exec_path. For an
 * "ollama/<model>" transport model the program becomes the launcher and
 * the claude args are wrapped:
 *   tmux ... -- ollama launch claude --model <suffix> -- <claude args>
 * with claude's own "--model" omitted.
 *
 * @resuming_existing_session: TRUE selects "--resume", FALSE "--session-id".
 * @effort: (nullable): per-turn effort level, or NULL/"" to omit.
 *
 * Returns: (transfer full): a NULL-terminated #GPtrArray (free func g_free);
 *   free with g_ptr_array_unref().
 */
GPtrArray *
ai_claude_tmux_client_build_session_argv(
    const gchar *tmux_bin,
    const gchar *session_name,
    const gchar *cwd,
    const gchar *claude_exec_path,
    gboolean     resuming_existing_session,
    const gchar *session_id,
    const gchar *settings_path,
    const gchar *model,
    const gchar *effort,
    gboolean     skip_permissions
);

G_END_DECLS
