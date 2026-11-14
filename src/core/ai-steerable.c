/*
 * ai-steerable.c - Words into a running turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-steerable.h"

/**
 * SECTION:ai-steerable
 * @title: AiSteerable
 * @short_description: Words into a running turn
 *
 * Today a message for a turn that is running waits and becomes the next
 * turn.  #AiSteerable hands it to the running turn instead, at the next
 * point the engine itself accepts input: before the next model call, at
 * the top of the next round of a tool loop.  Nothing is killed or
 * restarted to make room for it.
 *
 * The answer is a tri-state, and the third state is the important one:
 *
 * - %AI_STEER_REFUSED means *provably not delivered*.  The caller may
 *   queue the words for the next turn.
 * - %AI_STEER_DELIVERED means the running turn took them.
 * - %AI_STEER_INDETERMINATE means they went out and nothing confirmed
 *   them.  It counts as delivered: the words may already be running, and
 *   running them twice is worse than running them late.
 *
 * Whether a class can steer at all is a class datum,
 * #AiSteerableInterface.can_steer, so a caller asks one probe client
 * configured the way the real one will be (a class whose instances
 * differ -- #AiCodexCliClient, by transport -- answers per instance
 * through #AiSteerableInterface.is_steerable):
 *
 * |[<!-- language="C" -->
 * if (AI_IS_STEERABLE (client) &&
 *     ai_steerable_can_steer (AI_STEERABLE (client)))
 *   ai_steerable_steer_async (AI_STEERABLE (client), NULL, text,
 *                             NULL, on_steered, self);
 * ]|
 *
 * A steer targets a *running* turn.  ai_steerable_dup_turn_id() names
 * the one running now, and passing that id back makes the steer refuse
 * with %AI_STEER_ERROR_TURN_MISMATCH rather than land in a later turn.
 */

G_DEFINE_INTERFACE(AiSteerable, ai_steerable, G_TYPE_OBJECT)

/**
 * ai_steer_error_quark:
 *
 * Gets the error quark for steering refusals.
 *
 * Returns: the #AI_STEER_ERROR quark
 */
GQuark
ai_steer_error_quark(void)
{
    return g_quark_from_static_string("ai-steer-error-quark");
}

/**
 * ai_steer_result_get_type:
 *
 * Gets the #GType for #AiSteerResult.
 *
 * Returns: the #GType for #AiSteerResult
 */
GType
ai_steer_result_get_type(void)
{
    static GType result_type = 0;

    if (g_once_init_enter(&result_type))
    {
        static const GEnumValue values[] = {
            { AI_STEER_REFUSED, "AI_STEER_REFUSED", "refused" },
            { AI_STEER_DELIVERED, "AI_STEER_DELIVERED", "delivered" },
            { AI_STEER_INDETERMINATE, "AI_STEER_INDETERMINATE", "indeterminate" },
            { 0, NULL, NULL }
        };
        GType type = g_enum_register_static("AiSteerResult", values);

        g_once_init_leave(&result_type, type);
    }

    return result_type;
}

/**
 * ai_steer_error_get_type:
 *
 * Gets the #GType for #AiSteerError.
 *
 * Returns: the #GType for #AiSteerError
 */
GType
ai_steer_error_get_type(void)
{
    static GType error_type = 0;

    if (g_once_init_enter(&error_type))
    {
        static const GEnumValue values[] = {
            { AI_STEER_ERROR_NOT_SUPPORTED, "AI_STEER_ERROR_NOT_SUPPORTED", "not-supported" },
            { AI_STEER_ERROR_NO_ACTIVE_TURN, "AI_STEER_ERROR_NO_ACTIVE_TURN", "no-active-turn" },
            { AI_STEER_ERROR_TURN_MISMATCH, "AI_STEER_ERROR_TURN_MISMATCH", "turn-mismatch" },
            { AI_STEER_ERROR_AMBIGUOUS, "AI_STEER_ERROR_AMBIGUOUS", "ambiguous" },
            { AI_STEER_ERROR_NOT_STEERABLE, "AI_STEER_ERROR_NOT_STEERABLE", "not-steerable" },
            { AI_STEER_ERROR_SEND_FAILED, "AI_STEER_ERROR_SEND_FAILED", "send-failed" },
            { AI_STEER_ERROR_CANCELLED, "AI_STEER_ERROR_CANCELLED", "cancelled" },
            { 0, NULL, NULL }
        };
        GType type = g_enum_register_static("AiSteerError", values);

        g_once_init_leave(&error_type, type);
    }

    return error_type;
}

/**
 * ai_steer_result_to_string:
 * @result: an #AiSteerResult
 *
 * Gets the wire name of @result: "refused", "delivered" or
 * "indeterminate".  These are the names a caller reports onward, so they
 * are stable.
 *
 * Returns: (transfer none): the name, never %NULL
 */
const gchar *
ai_steer_result_to_string(AiSteerResult result)
{
    switch (result)
    {
        case AI_STEER_DELIVERED:
            return "delivered";
        case AI_STEER_INDETERMINATE:
            return "indeterminate";
        case AI_STEER_REFUSED:
        default:
            return "refused";
    }
}

static void
ai_steerable_default_init(AiSteerableInterface *iface)
{
    /* FALSE until a class says otherwise: refusing is always safe. */
    iface->can_steer = FALSE;
}

/**
 * ai_steerable_can_steer:
 * @self: an #AiSteerable
 *
 * Whether this client can put words into a running turn at all.
 *
 * This is a property of the class -- and, for a class that sets
 * #AiSteerableInterface.is_steerable, of how this instance is
 * configured -- not of the moment: %TRUE does not mean a turn is running
 * now, only that a running turn of this kind can be steered.  A steer
 * can still be refused.
 *
 * Returns: %TRUE when the client can steer
 */
gboolean
ai_steerable_can_steer(AiSteerable *self)
{
    AiSteerableInterface *iface;

    g_return_val_if_fail(AI_IS_STEERABLE(self), FALSE);

    iface = AI_STEERABLE_GET_IFACE(self);
    if (!iface->can_steer)
        return FALSE;

    /* The class can; ask the instance only when the class says it may differ. */
    return iface->is_steerable == NULL || iface->is_steerable(self);
}

/**
 * ai_steerable_dup_turn_id:
 * @self: an #AiSteerable
 *
 * Gets the id of the turn running now, the handle a caller holds to make
 * sure a later steer lands in *this* turn and not the next one.  With
 * more than one turn running, the newest.
 *
 * The id is opaque and local to this process; it is not the engine's
 * own turn id.
 *
 * Returns: (transfer full) (nullable): the turn id, or %NULL when no
 *   steerable turn is running
 */
gchar *
ai_steerable_dup_turn_id(AiSteerable *self)
{
    AiSteerableInterface *iface;

    g_return_val_if_fail(AI_IS_STEERABLE(self), NULL);

    iface = AI_STEERABLE_GET_IFACE(self);
    if (!ai_steerable_can_steer(self) || iface->dup_turn_id == NULL)
        return NULL;

    return iface->dup_turn_id(self);
}

/**
 * ai_steerable_steer_async:
 * @self: an #AiSteerable
 * @turn_id: (nullable): the turn the words are for, from
 *   ai_steerable_dup_turn_id(); %NULL for whichever turn is running
 * @text: the words
 * @cancellable: (nullable): a #GCancellable.  It is honoured only until
 *   the words are sent; after that the answer is whatever the engine
 *   says, since a cancel cannot take the words back.
 * @callback: (scope async): called with the outcome
 * @user_data: user data for @callback
 *
 * Puts @text into the running turn, at the next point the engine accepts
 * input.  Nothing is interrupted to make room.
 *
 * May be called from any thread; @callback runs in the thread-default
 * main context of the caller.  Finish with ai_steerable_steer_finish().
 */
void
ai_steerable_steer_async(
    AiSteerable         *self,
    const gchar         *turn_id,
    const gchar         *text,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    AiSteerableInterface *iface;

    g_return_if_fail(AI_IS_STEERABLE(self));
    g_return_if_fail(text != NULL);

    iface = AI_STEERABLE_GET_IFACE(self);

    /*
     * A class that cannot steer refuses here, so it needs no vfuncs of
     * its own -- implementing the interface with can_steer FALSE is the
     * whole job.  The task's source tag is what steer_finish recognises.
     */
    if (!ai_steerable_can_steer(self) || iface->steer_async == NULL ||
        iface->steer_finish == NULL)
    {
        g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);

        g_task_set_source_tag(task, ai_steerable_steer_async);
        g_task_return_new_error(task, AI_STEER_ERROR,
                                AI_STEER_ERROR_NOT_SUPPORTED,
                                "%s cannot steer a running turn",
                                G_OBJECT_TYPE_NAME(self));
        return;
    }

    iface->steer_async(self, turn_id, text, cancellable, callback, user_data);
}

/**
 * ai_steerable_steer_finish:
 * @self: an #AiSteerable
 * @result: the #GAsyncResult
 * @error: (out) (optional): set when, and only when, the result is
 *   %AI_STEER_REFUSED, saying why
 *
 * Finishes ai_steerable_steer_async().
 *
 * Only %AI_STEER_REFUSED lets the caller queue the words again.
 * %AI_STEER_INDETERMINATE counts as delivered.
 *
 * Returns: the outcome
 */
AiSteerResult
ai_steerable_steer_finish(
    AiSteerable   *self,
    GAsyncResult  *result,
    GError       **error
){
    AiSteerableInterface *iface;

    g_return_val_if_fail(AI_IS_STEERABLE(self), AI_STEER_REFUSED);

    if (g_async_result_is_tagged(result, ai_steerable_steer_async))
    {
        g_task_propagate_boolean(G_TASK(result), error);
        return AI_STEER_REFUSED;
    }

    iface = AI_STEERABLE_GET_IFACE(self);
    g_return_val_if_fail(iface->steer_finish != NULL, AI_STEER_REFUSED);

    return iface->steer_finish(self, result, error);
}

/*
 * Helpers for implementations; see ai-steerable-private.h.
 */
GTask *
ai_steer_task_new(
    gpointer             source_object,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task = g_task_new(source_object, cancellable, callback, user_data);

    g_task_set_check_cancellable(task, FALSE);
    return task;
}

void
ai_steer_task_return(
    GTask         *task,
    AiSteerResult  result,
    GError        *error
){
    if (result == AI_STEER_REFUSED)
    {
        /* A refusal always says why; invent a reason rather than none. */
        if (error == NULL)
            error = g_error_new_literal(AI_STEER_ERROR,
                                        AI_STEER_ERROR_NO_ACTIVE_TURN,
                                        "the words were not delivered");
        g_task_return_error(task, error);
        return;
    }

    g_clear_error(&error);
    g_task_return_int(task, (gssize)result);
}

AiSteerResult
ai_steer_task_propagate(
    GTask   *task,
    GError **error
){
    gssize value;
    GError *local = NULL;

    value = g_task_propagate_int(task, &local);
    if (local != NULL)
    {
        g_propagate_error(error, local);
        return AI_STEER_REFUSED;
    }

    return (AiSteerResult)value;
}
