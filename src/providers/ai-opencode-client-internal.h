/*
 * ai-opencode-client-internal.h - test/introspection seam for the
 *                                 OpenCode CLI client
 *
 * Copyright (C) 2025
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
#error "ai-opencode-client-internal.h is an internal header"
#endif

#include <glib.h>

#include "core/ai-cli-client.h"
#include "model/ai-message.h"

G_BEGIN_DECLS

/*
 * Build the argv for the opencode CLI. The returned array is
 * NULL-terminated; free with g_strfreev().
 *
 * @messages: (element-type AiMessage): conversation messages (used only
 *   for shape; the prompt itself is piped via stdin, not passed in argv).
 * @system_prompt: (nullable): retained for build_stdin -- opencode has no
 *   system-prompt flag, so a fresh session inlines it into the piped prompt.
 * @streaming: ignored -- opencode's only machine-readable format is
 *   `--format json`, which streams as NDJSON either way.
 *
 * Element-0 is an executable placeholder that the base CLI pipeline
 * overwrites with the resolved path at spawn time.
 */
gchar **
ai_opencode_client_build_argv(
    AiCliClient *self,
    GList       *messages,
    const gchar *system_prompt,
    gint         max_tokens,
    gboolean     streaming
);

G_END_DECLS
