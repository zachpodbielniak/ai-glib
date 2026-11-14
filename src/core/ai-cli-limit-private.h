/*
 * ai-cli-limit-private.h - Classifying what a wrapped CLI says when it is
 *                          limited, throttled or signed out
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "ai-cli-limit-private.h is an internal header"
#endif

#include <glib-object.h>

#include "core/ai-cli-client.h"
#include "core/ai-error.h"

G_BEGIN_DECLS

/*
 * One phrase a CLI prints and the error it means.
 *
 * A CLI's own text is the only signal: every one of them reports a usage
 * limit, a 429 and an expired login as the same failed run (a non-zero
 * exit, an error event), so without this a caller sees
 * AI_ERROR_CLI_EXECUTION for all three and has to guess from the words --
 * which is what libreclaw's failover did.  The guessing belongs here,
 * next to the provider that knows its CLI, with the source of each
 * phrase cited where the table is defined.
 *
 * @needle is matched case-insensitively anywhere in the error message.
 * Tables are searched in order and end with a { 0, NULL } entry, so a
 * more specific phrase ("usage limit") goes before a looser one that the
 * same message also contains ("try again").
 */
typedef struct
{
    AiError      code;
    const gchar *needle;
} AiCliLimitPattern;

/*
 * Associates a pattern table with a CLI class, from its class_init.
 * @cli_name names the CLI in the error ("codex", "grok").  Both are
 * static and must outlive the type, which every class datum does.
 *
 * Stored as type qdata rather than a field so AiCliClientClass keeps its
 * size; looked up through the parent chain, so a subclass of a
 * registered class inherits its table.
 */
void ai_cli_limit_register(GType                    type,
                           const gchar             *cli_name,
                           const AiCliLimitPattern *patterns);

/*
 * The error @text means, if any.
 *
 * Returns TRUE and sets @error when a pattern matches.  SESSION_LIMIT
 * is reported in claude-code's shape -- ai_session_limit_format()'s
 * sentence, then ai_session_limit_notice_new()'s notice -- followed by
 * the CLI's own words.  RATE_LIMITED carries the same notice when the
 * text says when to retry, so a supervisor can wait it out.
 * INVALID_API_KEY and INSUFFICIENT_QUOTA carry the CLI's words.
 *
 * @now is a parameter for the reason ai_session_limit_parse_reset()'s
 * is: a reset given as a clock time cannot be tested otherwise.
 */
gboolean ai_cli_limit_classify(const AiCliLimitPattern  *patterns,
                               const gchar              *cli_name,
                               const gchar              *text,
                               gint64                    now,
                               GError                  **error);

/*
 * Replaces *@error with what it means for @self's CLI, if its class
 * registered a table and the error is the generic kind a CLI failure
 * produces (AI_ERROR_CLI_EXECUTION or AI_ERROR_CLI_PARSE_ERROR).  Every
 * other error, and an error no pattern matches, is left alone.
 *
 * Called on every path a CLI's failure leaves by: the base's
 * ai_cli_client_chat_sync() and ai_cli_client_stream_run_finish(), and
 * each provider's own chat_finish.  Reclassifying twice is harmless --
 * the first replaces the code, and the second then leaves it alone.
 */
void ai_cli_limit_reclassify(AiCliClient  *self,
                             GError      **error);

G_END_DECLS
