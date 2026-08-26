/*
 * ai-session-limit.h - Recognising an account's session usage limit
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#ifndef AI_SESSION_LIMIT_H
#define AI_SESSION_LIMIT_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * AI_SESSION_LIMIT_SYNTHETIC_MODEL:
 *
 * The model name a CLI reports when it answered without a model.
 *
 * A usage limit is not a failed request: the CLI never contacts the API
 * at all.  It writes one assistant message of its own and exits, and
 * that message names this instead of a real model.
 */
#define AI_SESSION_LIMIT_SYNTHETIC_MODEL "<synthetic>"

/**
 * ai_session_limit_looks_synthetic:
 * @model: (nullable): the model the message names
 * @input_tokens: tokens the message reports consuming
 * @output_tokens: tokens the message reports producing
 * @cache_creation_tokens: cache tokens written
 * @cache_read_tokens: cache tokens read
 *
 * Whether an assistant message was produced by the CLI itself rather
 * than by a model.
 *
 * Two independent facts, and both are required.  The model name alone
 * would misread anything else a CLI ever labels synthetically, and zero
 * usage alone is reachable by a genuine completion that was cut off
 * before it billed anything.  Together they describe a message that
 * cost nothing because nothing was asked.
 *
 * Every cache counter is included because a CLI bills cache reads and
 * cache writes separately from input: a turn that touched only cache
 * would report zero input and output while having really run.
 *
 * Returns: %TRUE if the CLI wrote this message without a model
 */
gboolean ai_session_limit_looks_synthetic(const gchar *model,
                                          gint64       input_tokens,
                                          gint64       output_tokens,
                                          gint64       cache_creation_tokens,
                                          gint64       cache_read_tokens);

/**
 * ai_session_limit_parse_reset:
 * @text: (nullable): the message the CLI wrote
 * @now: the current time, as a Unix timestamp in seconds
 * @reset_out: (out) (optional): when the limit resets, as a Unix
 *   timestamp in seconds
 *
 * Reads the reset time out of a session-limit message.
 *
 * The text is of the form "You've hit your session limit · resets 6:50pm
 * (America/New_York)": a wall-clock time and an IANA zone, with **no
 * date**.  So the answer is the next occurrence of that time in that
 * zone, which is why @now is a parameter rather than read from the clock
 * -- a function that reads the clock cannot be tested across a day
 * boundary, and the day boundary is the case that matters.
 *
 * A zone the host does not know falls back to the local one rather than
 * failing: being an hour out is a far better answer than treating a
 * limit as though it had no reset at all, which is what a refusal here
 * would mean downstream.
 *
 * Returns: %TRUE if a reset time was found and @reset_out was set
 */
gboolean ai_session_limit_parse_reset(const gchar *text,
                                      gint64       now,
                                      gint64      *reset_out);

/**
 * ai_session_limit_format:
 * @reset: when the limit resets, as a Unix timestamp, or 0 if unknown
 *
 * A one-line description of the pause, for a log an operator reads.
 *
 * One spelling, because three layers report this condition -- the CLI
 * adapter that detects it, the agent that declines to retry it and the
 * supervisor that parks on it -- and three descriptions of one fact is
 * how an operator ends up believing they are looking at three problems.
 *
 * Returns: (transfer full): the sentence
 */
gchar *ai_session_limit_format(gint64 reset);

/**
 * AI_SESSION_LIMIT_NOTICE_PREFIX:
 *
 * The token that marks a machine-readable session-limit notice.
 *
 * A supervisor watching an agent's output needs to know two things: that
 * the agent is paused, and until when.  Recovering that from the
 * human sentence would mean parsing a localised date with no year in
 * it, so the notice is emitted separately and carries the reset as a
 * Unix timestamp.
 *
 * Deliberately not a word anything else would write: it is matched
 * against every line an agent logs.
 */
#define AI_SESSION_LIMIT_NOTICE_PREFIX "ai-glib-session-limit:"

/**
 * ai_session_limit_notice_new:
 * @reset: when the limit resets, as a Unix timestamp, or 0 if unknown
 *
 * The machine-readable notice an agent emits so its supervisor can see
 * the pause.
 *
 * Returns: (transfer full): the line, with no trailing newline
 */
gchar *ai_session_limit_notice_new(gint64 reset);

/**
 * ai_session_limit_notice_parse:
 * @line: (nullable): one line of an agent's output
 * @reset_out: (out) (optional): the reset time, or 0 if the notice
 *   carried none
 *
 * Reads a notice written by ai_session_limit_notice_new().
 *
 * The inverse of that function and tested as a round trip, because two
 * halves of one format written apart is exactly how a supervisor comes
 * to silently stop noticing the thing it was built to notice.
 *
 * Returns: %TRUE if @line is such a notice
 */
gboolean ai_session_limit_notice_parse(const gchar *line, gint64 *reset_out);

G_END_DECLS

#endif /* AI_SESSION_LIMIT_H */
