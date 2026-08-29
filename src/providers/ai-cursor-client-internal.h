/*
 * ai-cursor-client-internal.h - test/introspection seam for the
 *                               Cursor Agent CLI client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * Private header. NOT installed and NOT part of the public API. Exposes
 * the argv builder so unit tests (and the `ai` CLI's --dry-run) can assert
 * the exact command line that would be spawned without actually launching
 * a subprocess.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-cursor-client-internal.h is an internal header"
#endif

#include <glib.h>

#include "core/ai-cli-client.h"
#include "model/ai-message.h"

G_BEGIN_DECLS

/*
 * Build the argv for cursor-agent. The returned array is NULL-terminated;
 * free with g_strfreev().
 *
 * @messages: (element-type AiMessage): conversation messages (used only
 *   for shape; the prompt itself is piped via stdin, not passed in argv).
 * @system_prompt: (nullable): system prompt for a fresh session.
 * @streaming: TRUE to request stream-json plus --stream-partial-output.
 *
 * Element-0 is an executable placeholder that the base CLI pipeline
 * overwrites with the resolved path at spawn time.
 */
gchar **
ai_cursor_client_build_argv(
    AiCliClient *self,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    gboolean     streaming
);

G_END_DECLS
