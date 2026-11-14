/*
 * ai-steerable.h - Words into a running turn
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

G_BEGIN_DECLS

/**
 * AiSteerResult:
 * @AI_STEER_REFUSED: provably not delivered.  The words never reached the
 *   running turn, so the caller may queue them for the next one.  A
 *   #GError says why.
 * @AI_STEER_DELIVERED: the running turn took the words.
 * @AI_STEER_INDETERMINATE: the words were sent but nothing confirmed that
 *   the turn took them -- a timeout after the request went out, or a turn
 *   that ended while the answer was in flight.  This counts as delivered:
 *   the words may already be running, and running them twice is the one
 *   mistake steering must never make, so the caller must not queue them
 *   again.
 *
 * The outcome of ai_steerable_steer_async().
 *
 * %AI_STEER_REFUSED is zero, so a zero-initialised result never reads as
 * a delivery.
 */
typedef enum
{
    AI_STEER_REFUSED = 0,
    AI_STEER_DELIVERED,
    AI_STEER_INDETERMINATE
} AiSteerResult;

GType
ai_steer_result_get_type(void) G_GNUC_CONST;
#define AI_TYPE_STEER_RESULT (ai_steer_result_get_type())

const gchar *
ai_steer_result_to_string(AiSteerResult result);

/**
 * AI_STEER_ERROR:
 *
 * Error domain for steering refusals.  Errors in this domain will be from
 * the #AiSteerError enumeration.
 */
#define AI_STEER_ERROR (ai_steer_error_quark())

/**
 * AiSteerError:
 * @AI_STEER_ERROR_NOT_SUPPORTED: this client cannot steer at all
 *   (ai_steerable_can_steer() is %FALSE).
 * @AI_STEER_ERROR_NO_ACTIVE_TURN: there is no running turn to take the
 *   words, or it has already ended.
 * @AI_STEER_ERROR_TURN_MISMATCH: the turn named by the caller is not the
 *   one that is running.
 * @AI_STEER_ERROR_AMBIGUOUS: no turn was named and more than one is
 *   running.
 * @AI_STEER_ERROR_NOT_STEERABLE: a turn is running, but not one that
 *   accepts input (a review, a compaction, a turn still starting).
 * @AI_STEER_ERROR_SEND_FAILED: the words could not be sent; nothing
 *   reached the engine.
 * @AI_STEER_ERROR_CANCELLED: the caller cancelled before anything was
 *   sent.
 *
 * Why a steer was refused.  Every code here means the words were *not*
 * delivered.
 */
typedef enum
{
    AI_STEER_ERROR_NOT_SUPPORTED = 1,
    AI_STEER_ERROR_NO_ACTIVE_TURN,
    AI_STEER_ERROR_TURN_MISMATCH,
    AI_STEER_ERROR_AMBIGUOUS,
    AI_STEER_ERROR_NOT_STEERABLE,
    AI_STEER_ERROR_SEND_FAILED,
    AI_STEER_ERROR_CANCELLED
} AiSteerError;

GType
ai_steer_error_get_type(void) G_GNUC_CONST;
#define AI_TYPE_STEER_ERROR (ai_steer_error_get_type())

GQuark
ai_steer_error_quark(void);

#define AI_TYPE_STEERABLE (ai_steerable_get_type())

G_DECLARE_INTERFACE(AiSteerable, ai_steerable, AI, STEERABLE, GObject)

/**
 * AiSteerableInterface:
 * @parent_iface: the parent interface
 * @can_steer: whether this class can put words into a running turn at
 *   all.  A class datum, not a per-call answer: a caller asks one probe
 *   client rather than keeping a table of providers.
 * @dup_turn_id: returns the id of the turn that is running now, or %NULL
 * @steer_async: delivers words into a running turn
 * @steer_finish: finishes @steer_async
 * @_reserved: reserved for future expansion
 *
 * Words into a running turn.
 *
 * A class that implements this interface but leaves @can_steer %FALSE
 * answers every steer with %AI_STEER_REFUSED and
 * %AI_STEER_ERROR_NOT_SUPPORTED, without its vfuncs being called.
 */
struct _AiSteerableInterface
{
    GTypeInterface parent_iface;

    gboolean can_steer;

    gchar *       (*dup_turn_id)  (AiSteerable         *self);
    void          (*steer_async)  (AiSteerable         *self,
                                   const gchar         *turn_id,
                                   const gchar         *text,
                                   GCancellable        *cancellable,
                                   GAsyncReadyCallback  callback,
                                   gpointer             user_data);
    AiSteerResult (*steer_finish) (AiSteerable         *self,
                                   GAsyncResult        *result,
                                   GError             **error);

    /* Reserved for future expansion */
    gpointer _reserved[8];
};

gboolean
ai_steerable_can_steer(AiSteerable *self);

gchar *
ai_steerable_dup_turn_id(AiSteerable *self);

void
ai_steerable_steer_async(
    AiSteerable         *self,
    const gchar         *turn_id,
    const gchar         *text,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

AiSteerResult
ai_steerable_steer_finish(
    AiSteerable   *self,
    GAsyncResult  *result,
    GError       **error
);

G_END_DECLS
