/*
 * ai-cli-steer-private.h - The open stdin of a steerable CLI stream
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * A CLI that can be steered keeps its stdin open for the length of the
 * turn.  The prompt goes in first, then any words a caller steers in, and
 * the pipe closes when the engine says the turn is over.  The channel is
 * that open pipe plus the bookkeeping the tri-state needs: which steers
 * are still queued (refusable), which are on the wire (indeterminate if
 * the run dies) and which are waiting for the engine to acknowledge them.
 *
 * Threads.  A channel lives on the main context its stream run was
 * started on -- for libreclaw, a private context on a worker thread.
 * Everything except the client's registry is touched only there; a steer
 * from another thread finds the channel under the registry lock and hops
 * onto that context with g_main_context_invoke_full().
 */

#pragma once

#if !defined(AI_GLIB_COMPILATION)
#error "This is a private ai-glib header."
#endif

#include "core/ai-cli-client.h"
#include "core/ai-steerable.h"

G_BEGIN_DECLS

typedef struct _AiCliSteerChannel AiCliSteerChannel;

/*
 * Builds the bytes that put @text into the running turn, newline
 * included.  @ack_key, when set, is the key the engine will echo back to
 * confirm it took the words (ai_cli_steer_channel_resolve()); left NULL,
 * a completed write is the confirmation.  Runs on the channel's context.
 *
 * Returning NULL refuses the steer with @error, which should be in the
 * AI_STEER_ERROR domain (NOT_STEERABLE, typically).
 */
typedef gchar * (*AiCliSteerBuildFunc) (AiCliClient        *client,
                                        AiCliSteerChannel  *channel,
                                        const gchar        *text,
                                        gchar             **ack_key,
                                        GError            **error);

/* --- used by the shared stream reader (ai-cli-client.c) ------------- */

/*
 * Whether a stream run of @client keeps stdin open: the class can steer
 * and the instance asked for it with ai_cli_steer_set_open_input().  Both,
 * because an open stdin changes the CLI's contract (a stand-in that reads
 * to EOF would hang), so it is opt-in per client.
 */
gboolean
ai_cli_steer_wants_open_input(AiCliClient *client);

void
ai_cli_steer_set_open_input(AiCliClient *client, gboolean open_input);

/*
 * Called once if the prompt itself cannot be written, so the run fails
 * as it did when it wrote stdin alone.  A failed *steer* never calls it:
 * that is the steer's refusal, not the run's.  Never called after
 * ai_cli_steer_channel_end(), so @user_data need only live until then.
 */
typedef void (*AiCliSteerInputFailed) (GError   *error,
                                       gpointer  user_data);

AiCliSteerChannel *
ai_cli_steer_channel_new(
    AiCliClient           *client,
    GOutputStream         *input,
    const gchar           *initial_data,
    AiCliSteerInputFailed  input_failed,
    gpointer               user_data
);

/* The run is over.  Resolves everything still pending and closes stdin. */
void
ai_cli_steer_channel_end(AiCliSteerChannel *channel);

void
ai_cli_steer_channel_unref(AiCliSteerChannel *channel);

/* Brackets one parse call so a parser can find its run's channel. */
void
ai_cli_steer_set_current(AiCliSteerChannel *channel);

/* --- used by parsers (parse_stream_events) -------------------------- */

/*
 * The channel of the line being parsed, or NULL when the line is not
 * from a steerable stream (the synchronous path, a non-steering class).
 */
AiCliSteerChannel *
ai_cli_steer_current(void);

/*
 * The engine finished a turn.  Steers not yet sent are refused, and
 * stdin closes as soon as nothing is in flight or awaiting an answer.
 * New steers are refused from here on.
 */
void
ai_cli_steer_channel_settle(AiCliSteerChannel *channel);

/* The engine answered for the steer sent under @ack_key.  Takes @error. */
void
ai_cli_steer_channel_resolve(
    AiCliSteerChannel *channel,
    const gchar       *ack_key,
    AiSteerResult      result,
    GError            *error
);

/* A line of the parser's own (a protocol request), not a steer. */
void
ai_cli_steer_channel_write(
    AiCliSteerChannel *channel,
    const gchar       *line
);

/* Per-run state a subclass keeps on its channel. */
gpointer
ai_cli_steer_channel_get_data(AiCliSteerChannel *channel);

void
ai_cli_steer_channel_set_data(
    AiCliSteerChannel *channel,
    gpointer           data,
    GDestroyNotify     destroy
);

const gchar *
ai_cli_steer_channel_get_turn_id(AiCliSteerChannel *channel);

/* --- used by AiSteerable implementations ---------------------------- */

gchar *
ai_cli_steer_dup_turn_id(AiCliClient *client);

/*
 * The whole steer, given how to say it.  @ack_timeout_ms bounds the wait
 * for an acknowledgement (INDETERMINATE when it runs out); 0 waits until
 * the run ends.
 */
void
ai_cli_steer_async(
    AiCliClient         *client,
    const gchar         *turn_id,
    const gchar         *text,
    AiCliSteerBuildFunc  build,
    guint                ack_timeout_ms,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

AiSteerResult
ai_cli_steer_finish(
    AiCliClient   *client,
    GAsyncResult  *result,
    GError       **error
);

G_END_DECLS
