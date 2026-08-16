/*
 * ai-event.c - One normalized thing that happened during a turn
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "core/ai-event.h"

/*
 * Private structure for the AiEvent boxed type.
 *
 * Every kind uses a subset of these fields and leaves the rest zeroed, which
 * is why the getters are safe to call on any event: asking a text delta for
 * its tool use is a NULL, not undefined behaviour.  A consumer that switches
 * on the kind never reaches them; one that logs everything generically does,
 * and should not have to guard each access.
 */
struct _AiEvent
{
    AiEventKind   kind;
    gchar        *text;
    gchar        *tool_use_id;
    AiToolUse    *tool_use;
    AiToolResult *tool_result;
    AiUsage      *usage;
    gint64        cost_micros;
    GError       *error;
    gchar        *source;
    gint64        timestamp;

    gint          ref_count;
};

static AiEvent *
ai_event_alloc(AiEventKind kind);

/*
 * ai_event_get_type:
 *
 * Registers AiEvent as a refcounted boxed type.  Copy is ref: an event is
 * immutable once published, so handing a second subscriber its own deep copy
 * would allocate for no reason.
 */
G_DEFINE_BOXED_TYPE(AiEvent, ai_event, ai_event_ref, ai_event_unref)

/*
 * Allocates a zeroed event of the given kind with one reference and a
 * timestamp.  Every public constructor funnels through here so no field is
 * ever left uninitialised and the timestamp is never forgotten.
 */
static AiEvent *
ai_event_alloc(AiEventKind kind)
{
    AiEvent *self;

    self = g_slice_new0(AiEvent);
    self->kind = kind;
    self->cost_micros = -1;
    self->timestamp = g_get_monotonic_time();
    self->ref_count = 1;

    return self;
}

/**
 * ai_event_new:
 * @kind: the kind of event
 *
 * Creates an event carrying nothing but its kind.
 *
 * Useful for the kinds that have no payload --- %AI_EVENT_STREAM_START and
 * %AI_EVENT_STREAM_END.  Prefer the kind-specific constructors otherwise.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new(AiEventKind kind)
{
    return ai_event_alloc(kind);
}

/**
 * ai_event_new_text_delta:
 * @text: (nullable): the chunk of assistant prose
 *
 * Creates an %AI_EVENT_TEXT_DELTA event.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_text_delta(const gchar *text)
{
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_TEXT_DELTA);
    self->text = g_strdup(text);

    return self;
}

/**
 * ai_event_new_thinking_delta:
 * @text: (nullable): the chunk of reasoning
 *
 * Creates an %AI_EVENT_THINKING_DELTA event.
 *
 * Reasoning is kept distinct from prose rather than folded into it because a
 * frontend usually wants to collapse it, and a caller assembling the final
 * answer must not include it.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_thinking_delta(const gchar *text)
{
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_THINKING_DELTA);
    self->text = g_strdup(text);

    return self;
}

/**
 * ai_event_new_tool_started:
 * @tool_use: (transfer none) (nullable): the request from the model
 *
 * Creates an %AI_EVENT_TOOL_STARTED event.
 *
 * The tool use id is copied out of @tool_use so that
 * ai_event_get_tool_use_id() answers uniformly across the three tool kinds,
 * including %AI_EVENT_TOOL_INPUT_DELTA which has no #AiToolUse to offer.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_tool_started(AiToolUse *tool_use)
{
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_TOOL_STARTED);

    if (tool_use != NULL)
    {
        self->tool_use = g_object_ref(tool_use);
        self->tool_use_id = g_strdup(ai_tool_use_get_id(tool_use));
    }

    return self;
}

/**
 * ai_event_new_tool_input_delta:
 * @tool_use_id: (nullable): which pending call this fragment belongs to
 * @fragment: (nullable): a piece of the JSON arguments
 *
 * Creates an %AI_EVENT_TOOL_INPUT_DELTA event.
 *
 * Providers that stream tool arguments emit them in fragments that are not
 * individually valid JSON --- only the concatenation parses.  This event
 * exists so a frontend can show "running bash..." before the arguments are
 * complete; a consumer that only wants whole calls can ignore it and wait
 * for %AI_EVENT_TOOL_STARTED.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_tool_input_delta(
    const gchar *tool_use_id,
    const gchar *fragment
){
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_TOOL_INPUT_DELTA);
    self->tool_use_id = g_strdup(tool_use_id);
    self->text = g_strdup(fragment);

    return self;
}

/**
 * ai_event_new_tool_finished:
 * @tool_use: (transfer none) (nullable): the request this answers
 * @tool_result: (transfer none) (nullable): what it produced
 *
 * Creates an %AI_EVENT_TOOL_FINISHED event.
 *
 * The id is taken from @tool_use when present and from @tool_result
 * otherwise, because several providers report a result without repeating the
 * request --- and a result nobody can match to a call is not much use.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_tool_finished(
    AiToolUse    *tool_use,
    AiToolResult *tool_result
){
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_TOOL_FINISHED);

    if (tool_use != NULL)
    {
        self->tool_use = g_object_ref(tool_use);
        self->tool_use_id = g_strdup(ai_tool_use_get_id(tool_use));
    }

    if (tool_result != NULL)
    {
        self->tool_result = g_object_ref(tool_result);

        if (self->tool_use_id == NULL)
        {
            self->tool_use_id =
                g_strdup(ai_tool_result_get_tool_use_id(tool_result));
        }
    }

    return self;
}

/**
 * ai_event_new_usage:
 * @usage: (transfer none) (nullable): the token counts
 * @cost_micros: cost in micro-dollars, or -1 when the provider does not say
 *
 * Creates an %AI_EVENT_USAGE event.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_usage(
    AiUsage *usage,
    gint64   cost_micros
){
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_USAGE);
    self->usage = ai_usage_copy(usage);
    self->cost_micros = cost_micros;

    return self;
}

/**
 * ai_event_new_status:
 * @text: (nullable): something worth telling a human
 *
 * Creates an %AI_EVENT_STATUS event.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_status(const gchar *text)
{
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_STATUS);
    self->text = g_strdup(text);

    return self;
}

/**
 * ai_event_new_error:
 * @error: (nullable): what went wrong
 *
 * Creates an %AI_EVENT_ERROR event holding a copy of @error.
 *
 * A copy rather than a reference because the emitter almost always owns its
 * #GError inside a `g_autoptr` and will drop it as the frame unwinds, while
 * subscribers may keep the event for as long as they like.
 *
 * The event's text is set to the error message as well, so a frontend that
 * only renders text does not have to special-case this kind.
 *
 * Returns: (transfer full): a new #AiEvent
 */
AiEvent *
ai_event_new_error(const GError *error)
{
    AiEvent *self;

    self = ai_event_alloc(AI_EVENT_ERROR);

    if (error != NULL)
    {
        self->error = g_error_copy(error);
        self->text = g_strdup(error->message);
    }

    return self;
}

/**
 * ai_event_ref:
 * @self: (nullable): an #AiEvent
 *
 * Adds a reference to @self.
 *
 * Returns: (transfer full) (nullable): @self
 */
AiEvent *
ai_event_ref(AiEvent *self)
{
    if (self == NULL)
    {
        return NULL;
    }

    g_atomic_int_inc(&self->ref_count);

    return self;
}

/**
 * ai_event_unref:
 * @self: (nullable): an #AiEvent
 *
 * Drops a reference to @self, freeing it when the last one goes.
 * If @self is %NULL, this function does nothing.
 */
void
ai_event_unref(AiEvent *self)
{
    if (self == NULL)
    {
        return;
    }

    if (!g_atomic_int_dec_and_test(&self->ref_count))
    {
        return;
    }

    g_free(self->text);
    g_free(self->tool_use_id);
    g_free(self->source);
    g_clear_object(&self->tool_use);
    g_clear_object(&self->tool_result);
    g_clear_pointer(&self->usage, ai_usage_free);
    g_clear_error(&self->error);

    g_slice_free(AiEvent, self);
}

/**
 * ai_event_get_kind:
 * @self: an #AiEvent
 *
 * Returns: what @self describes
 */
AiEventKind
ai_event_get_kind(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, AI_EVENT_STATUS);

    return self->kind;
}

/**
 * ai_event_get_text:
 * @self: an #AiEvent
 *
 * The text payload: prose for %AI_EVENT_TEXT_DELTA, reasoning for
 * %AI_EVENT_THINKING_DELTA, the argument fragment for
 * %AI_EVENT_TOOL_INPUT_DELTA, the message for %AI_EVENT_STATUS and
 * %AI_EVENT_ERROR.
 *
 * Returns: (transfer none) (nullable): the text, or %NULL
 */
const gchar *
ai_event_get_text(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->text;
}

/**
 * ai_event_get_tool_use:
 * @self: an #AiEvent
 *
 * Returns: (transfer none) (nullable): the tool request, or %NULL
 */
AiToolUse *
ai_event_get_tool_use(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->tool_use;
}

/**
 * ai_event_get_tool_result:
 * @self: an #AiEvent
 *
 * Returns: (transfer none) (nullable): what the tool produced, or %NULL
 */
AiToolResult *
ai_event_get_tool_result(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->tool_result;
}

/**
 * ai_event_get_tool_use_id:
 * @self: an #AiEvent
 *
 * The id tying the three tool events together.  Available on every tool
 * event, including %AI_EVENT_TOOL_INPUT_DELTA which carries no #AiToolUse.
 *
 * Returns: (transfer none) (nullable): the id, or %NULL
 */
const gchar *
ai_event_get_tool_use_id(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->tool_use_id;
}

/**
 * ai_event_get_usage:
 * @self: an #AiEvent
 *
 * Returns: (transfer none) (nullable): the token counts, or %NULL
 */
AiUsage *
ai_event_get_usage(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->usage;
}

/**
 * ai_event_get_cost_micros:
 * @self: an #AiEvent
 *
 * Returns: the cost in micro-dollars, or -1 when unpriced
 */
gint64
ai_event_get_cost_micros(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, -1);

    return self->cost_micros;
}

/**
 * ai_event_get_error:
 * @self: an #AiEvent
 *
 * Returns: (transfer none) (nullable): why the turn failed, or %NULL
 */
const GError *
ai_event_get_error(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->error;
}

/**
 * ai_event_get_source:
 * @self: an #AiEvent
 *
 * Who emitted this --- a provider name, or "executor" for a locally run
 * tool.  A frontend that merges several streams into one transcript needs
 * this to label them apart.
 *
 * Returns: (transfer none) (nullable): the source, or %NULL
 */
const gchar *
ai_event_get_source(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->source;
}

/**
 * ai_event_set_source:
 * @self: an #AiEvent
 * @source: (nullable): who emitted it
 *
 * Sets the source label.
 *
 * This is the one mutator, and it is for the emitter to call between
 * constructing an event and publishing it --- ai_event_source_emit() does it
 * automatically.  Do not call it on an event you received: subscribers share
 * one instance, and changing it under them is a data race.
 */
void
ai_event_set_source(
    AiEvent     *self,
    const gchar *source
){
    g_return_if_fail(self != NULL);

    g_free(self->source);
    self->source = g_strdup(source);
}

/**
 * ai_event_get_timestamp:
 * @self: an #AiEvent
 *
 * When the event was constructed, from g_get_monotonic_time().  Monotonic
 * rather than wall-clock because the only thing anybody asks of it is how
 * long a tool took, and a clock adjustment mid-run must not answer that with
 * a negative number.
 *
 * Returns: microseconds on the monotonic clock
 */
gint64
ai_event_get_timestamp(const AiEvent *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->timestamp;
}

/**
 * ai_event_kind_to_string:
 * @kind: an #AiEventKind
 *
 * A stable lowercase name for @kind, for logging and for bindings that would
 * rather match on a string than an enum.
 *
 * Returns: (transfer none): the name, never %NULL
 */
const gchar *
ai_event_kind_to_string(AiEventKind kind)
{
    switch (kind)
    {
        case AI_EVENT_STREAM_START:     return "stream-start";
        case AI_EVENT_TEXT_DELTA:       return "text-delta";
        case AI_EVENT_THINKING_DELTA:   return "thinking-delta";
        case AI_EVENT_TOOL_STARTED:     return "tool-started";
        case AI_EVENT_TOOL_INPUT_DELTA: return "tool-input-delta";
        case AI_EVENT_TOOL_FINISHED:    return "tool-finished";
        case AI_EVENT_USAGE:            return "usage";
        case AI_EVENT_STATUS:           return "status";
        case AI_EVENT_ERROR:            return "error";
        case AI_EVENT_STREAM_END:       return "stream-end";
        default:                        return "unknown";
    }
}
