/*
 * ai-grok-home-overlay.h - A throwaway GROK_HOME with extra config
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private: in LIB_SOURCES but not PUBLIC_HEADERS, like
 * ai-openai-shared.h.  Deliberately free of GObject so the directory
 * logic can be tested without a client.
 *
 * Why this exists.  The grok CLI has no per-invocation flag for extra
 * MCP servers on the code path ai-glib drives.  Its config discovery is
 * $GROK_HOME (or ~/.grok), then <repo>/.grok, then <cwd>/.grok, plus
 * the claude-compatible ~/.claude.json and .mcp.json.  Everything but
 * GROK_HOME is a file inside the user's home or repository, and writing
 * a tool config there is doubly wrong: it is not ours to edit, and it
 * outlives the process whose socket it names, so every later `grok` the
 * user runs by hand starts by failing to reach a dead server.
 *
 * GROK_HOME alone is per-invocation, but it relocates auth, sessions and
 * plugins along with the config.  The overlay keeps those: a temporary
 * directory of symlinks back to the real home, with config.toml replaced
 * by the user's own plus an appended fragment.  Directory symlinks are
 * followed on path resolution, so new session files still land in the
 * real sessions/ and an auth.json write still reaches the real file.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-grok-home-overlay.h is an internal header"
#endif

#include <glib.h>

G_BEGIN_DECLS

/*
 * Builds an overlay of @source_home whose config.toml has the contents
 * of @config_fragment_path appended.
 *
 * @source_home: (nullable): the real grok home; NULL means $GROK_HOME,
 *   else ~/.grok.
 * @config_fragment_path: (nullable): a TOML fragment to append; NULL
 *   copies the config unchanged.
 *
 * Appending a fragment that starts with a [table] header is always
 * syntactically safe, because a table header ends whatever table
 * preceded it.  The one hazard is a duplicate table name, which is the
 * caller's to avoid by naming its server with a nonce.
 *
 * Returns: (transfer full) (nullable): the overlay directory, or NULL
 *   with @error set.
 */
gchar *
ai_grok_home_overlay_create(
    const gchar  *source_home,
    const gchar  *config_fragment_path,
    GError      **error
);

/*
 * Removes an overlay created by ai_grok_home_overlay_create().
 *
 * Unlinks only the overlay's own entries and never descends through a
 * symlink, so the real home cannot be damaged by this even if the
 * overlay is malformed.  A NULL or non-overlay path is ignored.
 */
void
ai_grok_home_overlay_destroy(const gchar *overlay_path);

G_END_DECLS
