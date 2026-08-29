/*
 * ai-conversation.c - Drives a provider and folds what happens into blocks
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * The join between the event stream and the transcript. Everything a
 * frontend needs is reachable from here: send text, watch the transcript,
 * answer approval requests.
 *
 * The folding rules in fold_event() are what produce the rhythm of a
 * claude-code session -- narration, a collapsed tool group, more narration
 * -- and they are three lines of "does this continue the last block or
 * start a new one".
 */

#include "config.h"

#include "view/ai-conversation.h"

#include "agent/ai-local-worker.h"
#include "harness/ai-mention.h"
#include "view/ai-view-blocks.h"
#include "view/ai-view-tool-block.h"
#include "view/ai-tool-style.h"
#include "core/ai-cli-client.h"
#include "core/ai-cli-client-private.h"
#include "core/ai-client.h"
#include "core/ai-error.h"
#include "core/ai-event-source.h"
#include "core/ai-streamable.h"
#include "model/ai-message.h"

struct _AiConversation
{
    GObject         parent_instance;

    GObject        *provider;
    AiTranscript   *transcript;
    AiToolExecutor *executor;
    GList          *messages;         /* AiMessage, owned */

    gchar          *system_prompt;
    gint            max_tokens;
    gboolean        stream;
    gboolean        local_tools;
    gboolean        busy;

    GCancellable   *cancellable;      /* live only during a turn */
    GTask          *task;             /* borrowed; the in-flight send */

    /*
     * The blocks currently being appended to. Cleared when something else
     * interrupts them, which is exactly how a tool group ends a paragraph
     * and starts a new one after it.
     */
    AiViewBlock    *open_text;
    AiViewBlock    *open_thinking;
    AiViewBlock    *open_tools;

    /*
     * The todo block, if the model has written one. Kept so an update
     * mutates it in place rather than appending a ninth copy of a
     * nine-line list.
     */
    AiViewBlock    *todo_block;

    /*
     * The background-agent panel, on the same terms as the todo block:
     * one block that keeps being rewritten, because an agent changes
     * state several times and a transcript is not a log file.
     */
    AiViewBlock    *agent_block;
    AiBrigade      *brigade;

    /* The input pipeline. NULL command_set means slash commands are not
     * resolved here at all, which is what an embedder that only wants a
     * transcript gets. */
    AiCommandSet   *command_set;
    gchar          *working_directory;
    AiAgentEndpoint *tool_endpoint;   /* owned, may be NULL */
    gboolean        passthrough_set;      /* has the caller decided? */
    gboolean        passthrough;

    /*
     * What the turn is doing right now, in words --- "Thinking",
     * "Running bash". NULL when idle. This is model state, not
     * decoration: a frontend spins its own glyph, but neither ncurses nor
     * Emacs should have to work out what the events mean.
     */
    gchar          *activity;
    gint64          activity_started_us;

    gulong          event_id;
    gulong          todos_id;
    gulong          agent_state_id;
    gulong          agent_finished_id;
};

G_DEFINE_TYPE(AiConversation, ai_conversation, G_TYPE_OBJECT)

enum
{
    PROP_0,
    PROP_PROVIDER,
    PROP_SYSTEM_PROMPT,
    PROP_MAX_TOKENS,
    PROP_STREAM,
    PROP_LOCAL_TOOLS,
    PROP_BUSY,
    PROP_TRANSCRIPT,
    PROP_COMMAND_SET,
    PROP_WORKING_DIRECTORY,
    PROP_PASSTHROUGH_COMMANDS,
    PROP_ACTIVITY,
    PROP_BRIGADE,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
    SIGNAL_APPROVAL_REQUESTED,
    SIGNAL_AGENT_FINISHED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void conversation_finish_turn(AiConversation *self, GError *error);
static void push_working_directory_to_provider(AiConversation *self,
                                               const gchar    *path,
                                               gboolean        moved);

/* ================================================================
 * Folding events into blocks
 * ================================================================ */

/*
 * Whatever prose or reasoning was being written has been interrupted.
 *
 * Forgetting the open text block is what makes the next delta start a new
 * paragraph, which is the whole reason a tool group visually separates the
 * narration before it from the narration after it.
 */
static void
close_open_blocks(AiConversation *self)
{
    if (self->open_text != NULL)
    {
        ai_view_block_set_complete(self->open_text, TRUE);
        self->open_text = NULL;
    }

    if (self->open_thinking != NULL)
    {
        ai_view_block_set_complete(self->open_thinking, TRUE);
        self->open_thinking = NULL;
    }
}

static AiViewBlock *
ensure_text_block(AiConversation *self)
{
    if (self->open_text == NULL)
    {
        g_autoptr(AiViewBlock) block = ai_view_text_block_new();

        self->open_text = block;
        ai_transcript_append(self->transcript, block);

        /* A tool group cannot continue past new prose either. */
        self->open_tools = NULL;
        self->open_thinking = NULL;
    }

    return self->open_text;
}

static AiViewBlock *
ensure_thinking_block(AiConversation *self)
{
    if (self->open_thinking == NULL)
    {
        g_autoptr(AiViewBlock) block = ai_view_thinking_block_new();

        self->open_thinking = block;
        ai_transcript_append(self->transcript, block);

        self->open_text = NULL;
        self->open_tools = NULL;
    }

    return self->open_thinking;
}

/*
 * The grouping.
 *
 * Consecutive tool calls join the block that is already open, so five calls
 * become one summary line rather than five. Anything else -- prose,
 * reasoning, the end of a turn -- closes it, so the next call after that
 * starts a fresh group.
 */
static AiViewToolBlock *
ensure_tool_block(AiConversation *self)
{
    if (self->open_tools == NULL)
    {
        g_autoptr(AiViewBlock) block = ai_view_tool_block_new();

        self->open_tools = block;
        ai_transcript_append(self->transcript, block);

        close_open_blocks(self);
    }

    return AI_VIEW_TOOL_BLOCK(self->open_tools);
}

/*
 * Find the call an AI_EVENT_TOOL_FINISHED answers.
 *
 * The open group first, then earlier ones: a result can arrive after prose
 * has already started a new paragraph, and the call it belongs to is then
 * in a group that is no longer open.
 */
static AiToolCall *
find_call_for_result(
    AiConversation *self,
    const gchar    *tool_use_id
){
    guint n;
    guint i;

    if (tool_use_id == NULL || tool_use_id[0] == '\0')
    {
        return NULL;
    }

    if (self->open_tools != NULL)
    {
        AiToolCall *call = ai_view_tool_block_find_call(
            AI_VIEW_TOOL_BLOCK(self->open_tools), tool_use_id);

        if (call != NULL)
        {
            return call;
        }
    }

    n = ai_transcript_get_n_blocks(self->transcript);

    for (i = n; i > 0; i--)
    {
        AiViewBlock *block = ai_transcript_get_block(self->transcript, i - 1);

        if (AI_IS_VIEW_TOOL_BLOCK(block))
        {
            AiToolCall *call = ai_view_tool_block_find_call(
                AI_VIEW_TOOL_BLOCK(block), tool_use_id);

            if (call != NULL)
            {
                return call;
            }
        }
    }

    return NULL;
}

/*
 * Say what the turn is doing now.
 *
 * Idempotent: the same phrase twice is one notification, because the
 * events arrive far faster than anything wants to redraw --- a streamed
 * answer is one TEXT_DELTA per token, and every one of them would
 * otherwise wake the frontend to say "still responding".
 */
static void
set_activity(
    AiConversation *self,
    const gchar    *activity
){
    if (g_strcmp0(self->activity, activity) == 0)
    {
        return;
    }

    g_free(self->activity);
    self->activity = g_strdup(activity);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_ACTIVITY]);
}

/*
 * "Running bash", "Reading ai-event.c".
 *
 * The target when there is one, because "Reading" alone is less useful
 * than the file name and a tool call almost always has one.
 */
static gchar *
describe_tool_call(AiToolUse *tool_use)
{
    const AiToolStyle *style;
    const gchar       *name;
    g_autoptr(AiToolCall) call = NULL;
    const gchar       *target;

    name = (tool_use != NULL) ? ai_tool_use_get_name(tool_use) : NULL;

    if (name == NULL)
    {
        return g_strdup("Working");
    }

    style = ai_tool_style_lookup(name);
    call = ai_tool_call_new(tool_use);
    target = ai_tool_call_get_target(call);

    if (target == NULL || target[0] == '\0')
    {
        return g_strdup_printf("%s %s",
                               ai_tool_category_gerund(
                                   style != NULL ? style->category
                                                 : AI_TOOL_CATEGORY_OTHER),
                               name);
    }

    return g_strdup_printf("%s %s",
                           ai_tool_category_gerund(
                               style != NULL ? style->category
                                             : AI_TOOL_CATEGORY_OTHER),
                           target);
}

static void
fold_event(
    AiConversation *self,
    AiEvent        *event
){
    switch (ai_event_get_kind(event))
    {
        case AI_EVENT_TEXT_DELTA:
        {
            AiViewBlock *block = ensure_text_block(self);

            ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block),
                                      ai_event_get_text(event));
            set_activity(self, "Responding");
            break;
        }

        case AI_EVENT_THINKING_DELTA:
        {
            AiViewBlock *block = ensure_thinking_block(self);

            ai_view_thinking_block_append(AI_VIEW_THINKING_BLOCK(block),
                                          ai_event_get_text(event));
            set_activity(self, "Thinking");
            break;
        }

        case AI_EVENT_TOOL_STARTED:
        {
            AiViewToolBlock  *block = ensure_tool_block(self);
            g_autofree gchar *doing =
                describe_tool_call(ai_event_get_tool_use(event));

            ai_view_tool_block_add_call(block, ai_event_get_tool_use(event));
            set_activity(self, doing);
            break;
        }

        case AI_EVENT_TOOL_FINISHED:
        {
            AiToolCall *call =
                find_call_for_result(self, ai_event_get_tool_use_id(event));

            if (call == NULL)
            {
                /*
                 * A result for a call nobody announced. Rather than drop it
                 * -- which would leave the work invisible -- open a group
                 * for it, so the transcript still says something happened.
                 */
                AiViewToolBlock *block = ensure_tool_block(self);

                call = ai_view_tool_block_add_call(block,
                                                   ai_event_get_tool_use(event));
            }

            ai_tool_call_finish(call, ai_event_get_tool_result(event));

            if (self->open_tools != NULL)
            {
                ai_view_tool_block_call_changed(
                    AI_VIEW_TOOL_BLOCK(self->open_tools));
            }

            /* Back to the model, which is where the wait now is. */
            set_activity(self, "Waiting for the model");
            break;
        }

        case AI_EVENT_USAGE:
        {
            g_autoptr(AiViewBlock) block = ai_view_status_block_new_usage(
                ai_event_get_usage(event),
                ai_event_get_cost_micros(event));

            close_open_blocks(self);
            self->open_tools = NULL;
            ai_transcript_append(self->transcript, block);
            break;
        }

        case AI_EVENT_STATUS:
        {
            g_autoptr(AiViewBlock) block = ai_view_status_block_new(
                AI_VIEW_STATUS_INFO, ai_event_get_text(event));

            ai_transcript_append(self->transcript, block);
            break;
        }

        case AI_EVENT_STREAM_START:
            set_activity(self, "Waiting for the model");
            break;

        case AI_EVENT_ERROR:
        {
            g_autoptr(AiViewBlock) block = ai_view_status_block_new(
                AI_VIEW_STATUS_ERROR, ai_event_get_text(event));

            close_open_blocks(self);
            self->open_tools = NULL;
            ai_transcript_append(self->transcript, block);
            break;
        }

        case AI_EVENT_STREAM_END:
            close_open_blocks(self);
            self->open_tools = NULL;
            break;

        case AI_EVENT_TOOL_INPUT_DELTA:
        default:
            /*
             * Nothing to fold. The argument fragments are already reflected
             * by the second TOOL_STARTED that carries the assembled input,
             * so a consumer that wanted only whole calls loses nothing.
             */
            break;
    }
}

static void
on_event(
    AiEventSource *source,
    AiEvent       *event,
    gpointer       user_data
){
    (void)source;

    fold_event(AI_CONVERSATION(user_data), event);
}

/* ================================================================
 * Sending
 * ================================================================ */

/*
 * Build blocks from a finished response.
 *
 * The path for a provider that is not an AiStreamable -- claude-tmux, which
 * drives claude through a tmux session and reads the finished transcript.
 * It produces its answer in one piece, so there is nothing to stream, but a
 * transcript should still show it rather than showing nothing.
 */
static void
fold_response(
    AiConversation *self,
    AiResponse     *response
){
    g_autofree gchar *text = NULL;
    GList *tool_uses;
    GList *iter;

    if (response == NULL)
    {
        return;
    }

    tool_uses = ai_response_get_tool_uses(response);

    for (iter = tool_uses; iter != NULL; iter = iter->next)
    {
        AiViewToolBlock *block = ensure_tool_block(self);

        ai_view_tool_block_add_call(block, iter->data);
    }

    g_list_free(tool_uses);

    text = ai_response_get_text(response);

    if (text != NULL && text[0] != '\0')
    {
        AiViewBlock *block = ensure_text_block(self);

        ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block), text);
    }

    if (ai_response_get_usage(response) != NULL)
    {
        g_autoptr(AiViewBlock) block =
            ai_view_status_block_new_usage(ai_response_get_usage(response), -1);

        close_open_blocks(self);
        self->open_tools = NULL;
        ai_transcript_append(self->transcript, block);
    }
}

static void
conversation_finish_turn(
    AiConversation *self,
    GError         *error
){
    GTask *task = self->task;

    close_open_blocks(self);
    self->open_tools = NULL;

    self->task = NULL;
    g_clear_object(&self->cancellable);

    if (self->busy)
    {
        self->busy = FALSE;
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BUSY]);
    }

    set_activity(self, NULL);

    if (task == NULL)
    {
        g_clear_error(&error);
        return;
    }

    if (error != NULL)
    {
        g_task_return_error(task, error);
    }
    else
    {
        g_task_return_boolean(task, TRUE);
    }

    g_object_unref(task);
}

static void
on_executor_done(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    AiConversation *self = user_data;
    g_autofree gchar *answer = NULL;
    g_autoptr(GError) error = NULL;
    GList *new_messages = NULL;

    answer = ai_tool_executor_run_full_finish(AI_TOOL_EXECUTOR(source), result,
                                              &new_messages, &error);

    if (answer == NULL && error != NULL)
    {
        g_autoptr(AiViewBlock) block = ai_view_status_block_new(
            AI_VIEW_STATUS_ERROR, error->message);

        close_open_blocks(self);
        self->open_tools = NULL;
        ai_transcript_append(self->transcript, block);

        conversation_finish_turn(self, g_steal_pointer(&error));
        return;
    }

    /*
     * The executor's events already built the blocks when streaming. When
     * it was not streaming, nothing has been folded yet and the answer is
     * all there is.
     */
    if (!self->stream && answer != NULL && answer[0] != '\0')
    {
        AiViewBlock *block = ensure_text_block(self);

        ai_view_text_block_append(AI_VIEW_TEXT_BLOCK(block), answer);
    }

    self->messages = g_list_concat(self->messages, new_messages);

    conversation_finish_turn(self, NULL);
}

static void
on_provider_done(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    AiConversation *self = user_data;
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;

    if (AI_IS_STREAMABLE(source) && self->stream)
    {
        response = ai_streamable_chat_stream_finish(AI_STREAMABLE(source),
                                                    result, &error);
    }
    else
    {
        response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);
    }

    if (response == NULL)
    {
        g_autoptr(AiViewBlock) block = ai_view_status_block_new(
            AI_VIEW_STATUS_ERROR,
            error != NULL ? error->message : "the turn produced no response");

        close_open_blocks(self);
        self->open_tools = NULL;
        ai_transcript_append(self->transcript, block);

        conversation_finish_turn(self,
            error != NULL
                ? g_steal_pointer(&error)
                : g_error_new_literal(AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                      "the turn produced no response"));
        return;
    }

    /*
     * A streaming provider already folded its own events. A non-streaming
     * one -- or claude-tmux, which cannot stream at all -- has told us
     * nothing yet, so the finished response is where the blocks come from.
     */
    if (!self->stream || !AI_IS_STREAMABLE(source))
    {
        fold_response(self, response);
    }

    if (ai_response_get_content_blocks(response) != NULL)
    {
        self->messages = g_list_append(
            self->messages,
            ai_message_new_from_response(response));
    }

    conversation_finish_turn(self, NULL);
}

/**
 * ai_conversation_get_activity:
 * @self: an #AiConversation
 *
 * What the turn in flight is doing, in words, or %NULL when idle.
 *
 * Pair it with a spinner of your own; see
 * #AiConversation:activity for why the words are here and the animation
 * is not.
 *
 * Returns: (transfer none) (nullable): the phrase, or %NULL
 */
const gchar *
ai_conversation_get_activity(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->activity;
}

/**
 * ai_conversation_get_activity_elapsed:
 * @self: an #AiConversation
 *
 * How long the turn in flight has been running, in microseconds.
 *
 * Measured monotonically from the moment it was sent, so it is unaffected
 * by the clock being set. Zero when idle.
 *
 * Returns: microseconds since the turn started
 */
gint64
ai_conversation_get_activity_elapsed(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), 0);

    if (!self->busy || self->activity_started_us == 0)
    {
        return 0;
    }

    return g_get_monotonic_time() - self->activity_started_us;
}

/**
 * ai_conversation_send_full_async:
 * @self: an #AiConversation
 * @display_text: (nullable): what to show in the transcript, or %NULL to
 *   show @text
 * @text: what to send to the model
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the turn finishes
 * @user_data: data for @callback
 *
 * Sends a turn whose transcript entry differs from its message.
 *
 * The two come apart whenever input is expanded: a `@path` mention or a
 * slash command produces text far longer than what was typed, and the
 * reader wants their own line back while the model needs the expansion.
 *
 * Complete with ai_conversation_send_finish().
 */
void
ai_conversation_send_full_async(
    AiConversation      *self,
    const gchar         *display_text,
    const gchar         *text,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    GTask *task;
    g_autoptr(AiViewBlock) turn = NULL;

    g_return_if_fail(AI_IS_CONVERSATION(self));

    task = g_task_new(self, cancellable, callback, user_data);

    if (self->busy)
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "a turn is already in flight");
        g_object_unref(task);
        return;
    }

    if (text == NULL || text[0] == '\0')
    {
        g_task_return_new_error(task, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                "nothing to send");
        g_object_unref(task);
        return;
    }

    /*
     * The transcript shows what the user typed; the model receives the
     * expansion. Showing the expansion instead would bury a one-line
     * question under the nine hundred lines it pulled in, and hiding what
     * was actually sent would make a surprising answer impossible to
     * explain.
     */
    turn = ai_view_turn_block_new(display_text != NULL ? display_text : text);
    ai_transcript_append(self->transcript, turn);

    close_open_blocks(self);
    self->open_tools = NULL;

    self->messages = g_list_append(self->messages, ai_message_new_user(text));

    self->task = task;
    self->cancellable = cancellable != NULL
        ? g_object_ref(cancellable)
        : g_cancellable_new();

    self->busy = TRUE;
    self->activity_started_us = g_get_monotonic_time();
    set_activity(self, "Waiting for the model");
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BUSY]);

    if (self->local_tools)
    {
        ai_tool_executor_set_stream(self->executor, self->stream);
        ai_tool_executor_run_full_async(self->executor,
                                        AI_PROVIDER(self->provider),
                                        self->messages,
                                        self->system_prompt,
                                        self->max_tokens,
                                        0,   /* the executor's own default */
                                        self->cancellable,
                                        on_executor_done,
                                        self);
        return;
    }

    if (self->stream && AI_IS_STREAMABLE(self->provider))
    {
        ai_streamable_chat_stream_async(AI_STREAMABLE(self->provider),
                                        self->messages,
                                        self->system_prompt,
                                        self->max_tokens,
                                        NULL,
                                        self->cancellable,
                                        on_provider_done,
                                        self);
        return;
    }

    ai_provider_chat_async(AI_PROVIDER(self->provider),
                           self->messages,
                           self->system_prompt,
                           self->max_tokens,
                           NULL,
                           self->cancellable,
                           on_provider_done,
                           self);
}

/**
 * ai_conversation_send_full_finish:
 * @self: an #AiConversation
 * @result: the #GAsyncResult
 * @error: (nullable): return location for a #GError
 *
 * Completes ai_conversation_send_full_async().
 *
 * Identical to ai_conversation_send_finish(); both exist because every
 * async function needs the matching name.
 *
 * Returns: %TRUE unless @error is set
 */
gboolean
ai_conversation_send_full_finish(
    AiConversation  *self,
    GAsyncResult    *result,
    GError         **error
){
    return ai_conversation_send_finish(self, result, error);
}

/**
 * ai_conversation_send_async:
 * @self: an #AiConversation
 * @text: the message to send
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the turn finishes
 * @user_data: data for @callback
 *
 * Sends @text as typed, with no command resolution and no mention
 * expansion.
 *
 * For a caller that has already done its own expansion, or wants none.
 * ai_conversation_send_input_async() is the one that runs the pipeline.
 */
void
ai_conversation_send_async(
    AiConversation      *self,
    const gchar         *text,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    ai_conversation_send_full_async(self, NULL, text, cancellable, callback,
                                    user_data);
}

/**
 * ai_conversation_send_finish:
 * @self: an #AiConversation
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes an ai_conversation_send_async() call.
 *
 * The transcript already holds whatever the turn produced, including a
 * status block describing the failure --- this reports the outcome for a
 * caller that wants to branch on it.
 *
 * Returns: %TRUE if the turn completed
 */
gboolean
ai_conversation_send_finish(
    AiConversation  *self,
    GAsyncResult    *result,
    GError         **error
){
    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

/**
 * ai_conversation_cancel:
 * @self: an #AiConversation
 *
 * Stops the turn in flight, if there is one.
 *
 * Always allowed: a user who wants to stop must never be refused. The
 * blocks produced so far stay in the transcript, because what already
 * happened did happen.
 */
void
ai_conversation_cancel(AiConversation *self)
{
    g_return_if_fail(AI_IS_CONVERSATION(self));

    if (self->cancellable != NULL)
    {
        g_cancellable_cancel(self->cancellable);
    }
}

/**
 * ai_conversation_clear:
 * @self: an #AiConversation
 *
 * Empties the transcript and forgets the history.
 *
 * A provider holding a session of its own is not affected --- the CLI
 * wrappers keep their own state, and clearing the local view does not
 * un-say what was said to them. Set their session-id to %NULL for that.
 */
void
ai_conversation_clear(AiConversation *self)
{
    g_return_if_fail(AI_IS_CONVERSATION(self));

    ai_transcript_clear(self->transcript);

    g_list_free_full(self->messages, g_object_unref);
    self->messages = NULL;

    self->open_text = NULL;
    self->open_thinking = NULL;
    self->open_tools = NULL;

    /* The todo list belongs to this conversation too; leaving it behind
     * a /clear would be the one thing on screen that did not go. */
    self->todo_block = NULL;
    ai_tool_executor_clear_todos(self->executor);

    /*
     * The panel goes; the agents do not. Clearing a transcript is an
     * instruction about the display, and killing work somebody started
     * because they wanted a clean screen would be a surprising way to
     * lose an hour of it. ai_brigade_cancel_all() is how you stop them.
     */
    self->agent_block = NULL;
}

/* ================================================================
 * Boilerplate
 * ================================================================ */

static gint
on_executor_approval(
    AiToolExecutor *executor,
    AiToolUse      *tool_use,
    gpointer        user_data
){
    AiConversation *self = user_data;
    gint answer = AI_TOOL_APPROVAL_DEFAULT;

    (void)executor;

    /*
     * Forwarded so a frontend connects to one object. The accumulator on
     * the far side means an unanswered request still falls through to the
     * executor's policy.
     */
    g_signal_emit(self, signals[SIGNAL_APPROVAL_REQUESTED], 0, tool_use,
                  &answer);

    return answer;
}

/*
 * The model rewrote its plan.
 *
 * One block, updated in place. A model revises its todo list eight times
 * over a long task, and eight copies of a nine-line list is not a
 * transcript anybody can read --- so this reuses the block if there is
 * one, which the transcript reports as ::block-changed rather than
 * ::items-changed.
 */
static void
on_executor_todos_changed(
    AiToolExecutor *executor,
    gpointer        user_data
){
    AiConversation *self = user_data;

    if (self->todo_block == NULL)
    {
        g_autoptr(AiViewTodoBlock) block = NULL;

        /* An empty list is not worth a row. This is also what keeps
         * ai_conversation_clear() from putting a fresh empty block back
         * the moment it empties the executor's list. */
        if (ai_tool_executor_get_n_todos(executor) == 0)
        {
            return;
        }

        block = ai_view_todo_block_new();
        self->todo_block = AI_VIEW_BLOCK(block);
        ai_transcript_append(self->transcript, AI_VIEW_BLOCK(block));
    }

    ai_view_todo_block_set_todos(AI_VIEW_TODO_BLOCK(self->todo_block),
                                 ai_tool_executor_get_todos(executor));
}

/*
 * A background agent changed state.
 *
 * The panel is rebuilt from the brigade rather than patched, because a
 * reap removes an agent and there is no state-change event for "gone".
 * Rebuilding is a handful of strdups over a list that is never long.
 */
static void
conversation_refresh_agents(AiConversation *self)
{
    g_autoptr(GList) agents = NULL;

    if (self->brigade == NULL) return;

    agents = ai_brigade_list(self->brigade);

    if (self->agent_block == NULL)
    {
        g_autoptr(AiViewAgentBlock) block = NULL;

        /* Nothing has ever been spawned: no panel.  Same rule as the
         * todo block, and it is what stops ai_conversation_clear() from
         * putting an empty one back. */
        if (agents == NULL) return;

        block = ai_view_agent_block_new();
        self->agent_block = AI_VIEW_BLOCK(block);
        ai_transcript_append(self->transcript, AI_VIEW_BLOCK(block));
    }

    ai_view_agent_block_set_agents(AI_VIEW_AGENT_BLOCK(self->agent_block),
                                   agents);
}

static void
on_brigade_agent_state_changed(
    AiBrigade   *brigade,
    const gchar *agent_id,
    gint         state,
    gpointer     user_data
){
    (void)brigade;
    (void)agent_id;
    (void)state;

    conversation_refresh_agents(user_data);
}

/*
 * Republished so a frontend can react without knowing what a brigade is.
 *
 * The model is told separately, by #AiToolExecutor, at the next turn
 * boundary --- it is only awake between turns. This signal is for the
 * person watching, who is not.
 */
static void
on_brigade_agent_finished(
    AiBrigade   *brigade,
    const gchar *agent_id,
    gint         state,
    gpointer     user_data
){
    AiConversation *self = user_data;

    (void)brigade;

    conversation_refresh_agents(self);

    g_signal_emit(self, signals[SIGNAL_AGENT_FINISHED], 0, agent_id, state);
}

/**
 * ai_conversation_set_brigade:
 * @self: an #AiConversation
 * @brigade: (nullable) (transfer none): where background agents run
 *
 * Lets this conversation start background agents, and shows them.
 *
 * Passing a brigade registers the `agent_*` tools on the executor and
 * adds a panel to the transcript that tracks what is running. Passing
 * %NULL takes both away. An application that never calls this is
 * unaffected --- which is the point, since a model able to start
 * unattended work that outlives the turn is a grant an embedder should
 * make deliberately.
 *
 * The brigade needs a worker to run anything; see ai_local_worker_new().
 */
void
ai_conversation_set_brigade(
    AiConversation *self,
    AiBrigade      *brigade
){
    g_return_if_fail(AI_IS_CONVERSATION(self));
    g_return_if_fail(brigade == NULL || AI_IS_BRIGADE(brigade));

    if (self->brigade == brigade) return;

    if (self->brigade != NULL)
    {
        if (self->agent_state_id != 0)
            g_signal_handler_disconnect(self->brigade, self->agent_state_id);
        if (self->agent_finished_id != 0)
            g_signal_handler_disconnect(self->brigade, self->agent_finished_id);
    }

    self->agent_state_id = 0;
    self->agent_finished_id = 0;

    g_set_object(&self->brigade, brigade);
    ai_tool_executor_set_brigade(self->executor, brigade);

    if (brigade != NULL)
    {
        self->agent_state_id =
            g_signal_connect(brigade, "agent-state-changed",
                             G_CALLBACK(on_brigade_agent_state_changed), self);
        self->agent_finished_id =
            g_signal_connect(brigade, "agent-finished",
                             G_CALLBACK(on_brigade_agent_finished), self);

        conversation_refresh_agents(self);
    }

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BRIGADE]);
}

/**
 * ai_conversation_get_brigade:
 * @self: an #AiConversation
 *
 * Returns: (transfer none) (nullable): the brigade, or %NULL
 */
AiBrigade *
ai_conversation_get_brigade(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);
    return self->brigade;
}

AiBrigade *
ai_conversation_enable_background_agents(
    AiConversation *self,
    guint           max_concurrent
){
    g_autoptr(AiBrigade)     brigade = NULL;
    g_autoptr(AiLocalWorker) worker  = NULL;

    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    if (self->brigade != NULL) return self->brigade;

    brigade = ai_brigade_new();
    worker  = ai_local_worker_new();

    ai_brigade_set_worker(brigade, AI_AGENT_WORKER(worker));
    ai_brigade_set_max_concurrent(brigade, max_concurrent);

    ai_conversation_set_brigade(self, brigade);

    return self->brigade;
}

static void
ai_conversation_finalize(GObject *object)
{
    AiConversation *self = AI_CONVERSATION(object);

    if (self->event_id != 0 && self->provider != NULL)
    {
        g_signal_handler_disconnect(self->provider, self->event_id);
    }

    if (self->todos_id != 0 && self->executor != NULL)
    {
        g_signal_handler_disconnect(self->executor, self->todos_id);
    }

    if (self->brigade != NULL)
    {
        if (self->agent_state_id != 0)
            g_signal_handler_disconnect(self->brigade, self->agent_state_id);
        if (self->agent_finished_id != 0)
            g_signal_handler_disconnect(self->brigade, self->agent_finished_id);
    }

    g_clear_object(&self->brigade);
    g_clear_object(&self->provider);
    g_clear_object(&self->transcript);
    g_clear_object(&self->executor);
    g_clear_object(&self->cancellable);
    g_clear_object(&self->command_set);
    g_clear_pointer(&self->system_prompt, g_free);
    g_clear_pointer(&self->working_directory, g_free);
    g_clear_pointer(&self->tool_endpoint, ai_agent_endpoint_free);
    g_clear_pointer(&self->activity, g_free);
    g_list_free_full(self->messages, g_object_unref);

    G_OBJECT_CLASS(ai_conversation_parent_class)->finalize(object);
}

static void
ai_conversation_get_property(
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiConversation *self = AI_CONVERSATION(object);

    switch (prop_id)
    {
        case PROP_PROVIDER:
            g_value_set_object(value, self->provider);
            break;
        case PROP_SYSTEM_PROMPT:
            g_value_set_string(value, self->system_prompt);
            break;
        case PROP_MAX_TOKENS:
            g_value_set_int(value, self->max_tokens);
            break;
        case PROP_STREAM:
            g_value_set_boolean(value, self->stream);
            break;
        case PROP_LOCAL_TOOLS:
            g_value_set_boolean(value, self->local_tools);
            break;
        case PROP_BUSY:
            g_value_set_boolean(value, self->busy);
            break;
        case PROP_TRANSCRIPT:
            g_value_set_object(value, self->transcript);
            break;
        case PROP_COMMAND_SET:
            g_value_set_object(value, self->command_set);
            break;
        case PROP_WORKING_DIRECTORY:
            g_value_set_string(value, self->working_directory);
            break;
        case PROP_PASSTHROUGH_COMMANDS:
            g_value_set_boolean(value,
                                ai_conversation_get_passthrough_commands(self));
            break;
        case PROP_ACTIVITY:
            g_value_set_string(value, self->activity);
            break;
        case PROP_BRIGADE:
            g_value_set_object(value, self->brigade);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

static void
ai_conversation_set_property(
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiConversation *self = AI_CONVERSATION(object);

    switch (prop_id)
    {
        case PROP_SYSTEM_PROMPT:
            ai_conversation_set_system_prompt(self, g_value_get_string(value));
            break;
        case PROP_MAX_TOKENS:
            ai_conversation_set_max_tokens(self, g_value_get_int(value));
            break;
        case PROP_STREAM:
            ai_conversation_set_stream(self, g_value_get_boolean(value));
            break;
        case PROP_LOCAL_TOOLS:
            ai_conversation_set_local_tools(self, g_value_get_boolean(value));
            break;
        case PROP_COMMAND_SET:
            ai_conversation_set_command_set(self, g_value_get_object(value));
            break;
        case PROP_WORKING_DIRECTORY:
            ai_conversation_set_working_directory(self,
                                                  g_value_get_string(value));
            break;
        case PROP_PASSTHROUGH_COMMANDS:
            ai_conversation_set_passthrough_commands(
                self, g_value_get_boolean(value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
            break;
    }
}

/*
 * The same accumulator AiToolExecutor uses: keep asking while the answer is
 * DEFAULT, stop at the first real decision. See that class for why
 * g_signal_accumulator_first_wins is the wrong one here.
 */
static gboolean
approval_accumulator(
    GSignalInvocationHint *hint,
    GValue                *return_accu,
    const GValue          *handler_return,
    gpointer               data
){
    gint answer;

    (void)hint;
    (void)data;

    answer = g_value_get_int(handler_return);
    g_value_set_int(return_accu, answer);

    return answer == AI_TOOL_APPROVAL_DEFAULT;
}

static void
ai_conversation_class_init(AiConversationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->finalize = ai_conversation_finalize;
    object_class->get_property = ai_conversation_get_property;
    object_class->set_property = ai_conversation_set_property;

    /**
     * AiConversation:provider:
     *
     * The provider used for the next turn.
     *
     * Read-only as a property because changing it can fail while a turn is in
     * flight or while applying a tool endpoint. Use
     * ai_conversation_set_provider() to switch it.
     */
    properties[PROP_PROVIDER] =
        g_param_spec_object("provider", "Provider",
                            "The provider used for the next turn",
                            G_TYPE_OBJECT,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    properties[PROP_SYSTEM_PROMPT] =
        g_param_spec_string("system-prompt", "System Prompt",
                            "Instructions sent with every turn", NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_MAX_TOKENS] =
        g_param_spec_int("max-tokens", "Max Tokens",
                         "Maximum tokens per response", 0, G_MAXINT, 4096,
                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_STREAM] =
        g_param_spec_boolean("stream", "Stream",
                             "Stream each turn when the provider supports it",
                             TRUE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiConversation:local-tools:
     *
     * Whether to run tools in this process via #AiToolExecutor.
     *
     * Defaults to %FALSE and is refused for the CLI wrapper providers,
     * which ignore the tools argument entirely and run their own --- an
     * executor pointed at one would advertise tools the CLI never sees, and
     * the model would be told it had capabilities that do not exist.
     */
    properties[PROP_LOCAL_TOOLS] =
        g_param_spec_boolean("local-tools", "Local Tools",
                             "Run tools in this process", FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_BUSY] =
        g_param_spec_boolean("busy", "Busy", "Whether a turn is in flight",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    properties[PROP_TRANSCRIPT] =
        g_param_spec_object("transcript", "Transcript",
                            "The blocks this conversation has produced",
                            AI_TYPE_TRANSCRIPT,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiConversation:command-set:
     *
     * How ai_conversation_send_input_async() resolves a slash command.
     *
     * %NULL means it does not: every line goes to the model as typed,
     * which is what an embedder that only wants a transcript gets.
     */
    properties[PROP_COMMAND_SET] =
        g_param_spec_object("command-set", NULL, NULL, AI_TYPE_COMMAND_SET,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiConversation:working-directory:
     *
     * What `@path` mentions and shell substitutions resolve against.
     *
     * Also set on the conversation's #AiToolExecutor, so the model's
     * tools and the user's mentions agree about where they are.
     */
    properties[PROP_WORKING_DIRECTORY] =
        g_param_spec_string("working-directory", NULL, NULL, NULL,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiConversation:passthrough-commands:
     *
     * Whether a line reaches the provider exactly as typed.
     *
     * Defaults to %TRUE for a CLI provider and %FALSE for an HTTP one,
     * and that default is the whole point: claude-code, opencode and
     * grok already resolve `@`, `/` and their own skills, so expanding
     * first would fight them and would stop `/compact` from ever
     * arriving. Setting it explicitly overrides the guess in either
     * direction.
     *
     * Purely local built-ins --- `/quit`, `/clear` --- still run locally
     * even under passthrough, because the wrapped CLI has no opinion
     * about this program's transcript.
     */
    properties[PROP_PASSTHROUGH_COMMANDS] =
        g_param_spec_boolean("passthrough-commands", NULL, NULL, FALSE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiConversation:activity:
     *
     * What the turn in flight is doing, in words --- "Thinking",
     * "Running bash", "Waiting for the model" --- or %NULL when idle.
     *
     * Folded out of the event stream alongside the transcript, so a
     * frontend showing progress does not have to interpret events a
     * second time. The animation is the frontend's: this says what to put
     * next to the spinner, not how to spin it.
     *
     * A provider that emits no events (claude-tmux reads a finished
     * transcript rather than a stream) reports "Waiting for the model"
     * for the whole turn, which is honest and is also the case where a
     * progress indicator matters most.
     */
    properties[PROP_ACTIVITY] =
        g_param_spec_string("activity", NULL, NULL, NULL,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /**
     * AiConversation:brigade:
     *
     * Where background agents run, or %NULL.
     *
     * Setting one is what gives the model the `agent_*` tools and adds
     * the panel that tracks them. See ai_conversation_set_brigade().
     */
    properties[PROP_BRIGADE] =
        g_param_spec_object("brigade", NULL, NULL, AI_TYPE_BRIGADE,
                            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties(object_class, N_PROPS, properties);

    /**
     * AiConversation::approval-requested:
     * @self: the conversation
     * @tool_use: the call the model wants to make
     *
     * Forwarded from the conversation's #AiToolExecutor, so a frontend
     * connects to one object rather than reaching inside for another.
     *
     * Returns an #AiToolApproval; see
     * #AiToolExecutor::approval-requested for what the values mean and for
     * the nested-loop rule a handler that asks a human must follow.
     */
    signals[SIGNAL_APPROVAL_REQUESTED] =
        g_signal_new("approval-requested",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0,
                     approval_accumulator, NULL,
                     NULL,
                     G_TYPE_INT, 1,
                     AI_TYPE_TOOL_USE);

    /**
     * AiConversation::agent-finished:
     * @self: the conversation
     * @agent_id: which agent
     * @state: the terminal #AiAgentState it reached
     *
     * A background agent stopped working.
     *
     * Forwarded from the brigade so a frontend connects to one object.
     * The model is told separately and later --- at the next turn
     * boundary, since that is the only moment it exists to be told
     * anything. This signal is for the person watching, who does not
     * have to wait for a turn.
     *
     * The answer is not carried here. Collect it with ai_brigade_reap(),
     * bearing in mind the model's `agent_result` tool reaps too, so
     * whichever asks first gets it.
     */
    signals[SIGNAL_AGENT_FINISHED] =
        g_signal_new("agent-finished",
                     G_TYPE_FROM_CLASS(klass),
                     G_SIGNAL_RUN_LAST,
                     0, NULL, NULL, NULL,
                     G_TYPE_NONE, 2,
                     G_TYPE_STRING, G_TYPE_INT);
}

static void
ai_conversation_init(AiConversation *self)
{
    self->transcript = ai_transcript_new();
    self->executor = ai_tool_executor_new();
    self->max_tokens = 4096;
    self->stream = TRUE;
    self->local_tools = FALSE;

    g_signal_connect(self->executor, "approval-requested",
                     G_CALLBACK(on_executor_approval), self);
    self->todos_id = g_signal_connect(self->executor, "todos-changed",
                                      G_CALLBACK(on_executor_todos_changed),
                                      self);
}

/**
 * ai_conversation_new:
 * @provider: (transfer none): an #AiClient or #AiCliClient
 *
 * Creates a conversation driving @provider.
 *
 * Takes a #GObject rather than an #AiProvider because the two client base
 * classes share no ancestor, and a caller should not have to know which one
 * it is holding.
 *
 * Returns: (transfer full): a new #AiConversation
 */
AiConversation *
ai_conversation_new(GObject *provider)
{
    AiConversation *self;

    g_return_val_if_fail(G_IS_OBJECT(provider), NULL);
    g_return_val_if_fail(AI_IS_PROVIDER(provider), NULL);

    self = g_object_new(AI_TYPE_CONVERSATION, NULL);
    self->provider = g_object_ref(provider);

    /* A construct-time "working-directory" property runs through the
     * setter before there is a provider to push it to, so re-push it
     * here.  Not a move: this is the first time the provider has heard
     * a directory at all, and clearing a session id the caller set
     * alongside it would be a surprise. */
    if (self->working_directory != NULL)
    {
        push_working_directory_to_provider(self, self->working_directory,
                                           FALSE);
    }

    if (AI_IS_EVENT_SOURCE(provider))
    {
        self->event_id = g_signal_connect(provider, "event",
                                          G_CALLBACK(on_event), self);
    }

    return self;
}

/**
 * ai_conversation_get_transcript:
 * @self: an #AiConversation
 *
 * Returns: (transfer none): the blocks this conversation has produced
 */
AiTranscript *
ai_conversation_get_transcript(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->transcript;
}

/**
 * ai_conversation_get_executor:
 * @self: an #AiConversation
 *
 * The executor used when #AiConversation:local-tools is set.
 *
 * Exposed so a host can restrict what the model may call ---
 * ai_tool_executor_unregister(), or a fresh
 * ai_tool_executor_new_empty() plus its own registrations.
 *
 * Returns: (transfer none): the executor
 */
AiToolExecutor *
ai_conversation_get_executor(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->executor;
}

/**
 * ai_conversation_get_provider:
 * @self: an #AiConversation
 *
 * Returns: (transfer none): the provider being driven
 */
GObject *
ai_conversation_get_provider(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->provider;
}

/**
 * ai_conversation_set_provider:
 * @self: an #AiConversation
 * @provider: (transfer none): an #AiProvider implementation
 * @error: (out) (optional): return location for a #GError
 *
 * Switches the provider used by subsequent turns without clearing history.
 *
 * The transcript, completed #AiMessage history, system prompt, working
 * directory, and other conversation settings stay in place. A CLI target
 * starts a fresh native session: its session ID and `continue-session`
 * property are cleared so provider-private history cannot duplicate or
 * contradict the canonical in-process messages.
 *
 * A switch is refused while a turn is in flight. If the conversation has a
 * tool endpoint, @provider must accept it; failure leaves the current provider
 * untouched. Local tools are disabled when the target is a CLI wrapper,
 * because those providers run tools in their own process.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_conversation_set_provider(
    AiConversation  *self,
    GObject         *provider,
    GError         **error
){
    gboolean old_passthrough;
    gboolean new_passthrough;

    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);
    g_return_val_if_fail(G_IS_OBJECT(provider), FALSE);
    g_return_val_if_fail(AI_IS_PROVIDER(provider), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (self->busy)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "cannot switch provider while a turn is in flight");
        return FALSE;
    }

    if (self->provider == provider)
    {
        return TRUE;
    }

    if (self->tool_endpoint != NULL)
    {
        if (!AI_IS_TOOL_ENDPOINT_CONSUMER(provider))
        {
            g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                        "%s cannot use the conversation's tool endpoint",
                        G_OBJECT_TYPE_NAME(provider));
            return FALSE;
        }

        if (!ai_tool_endpoint_consumer_apply(
                AI_TOOL_ENDPOINT_CONSUMER(provider),
                self->tool_endpoint,
                error))
        {
            return FALSE;
        }
    }

    if (AI_IS_CLI_CLIENT(provider))
    {
        GParamSpec *continue_spec;

        ai_cli_client_set_working_directory(AI_CLI_CLIENT(provider),
                                            self->working_directory);
        ai_cli_client_set_session_id(AI_CLI_CLIENT(provider), NULL);
        ai_cli_client_mark_portable_context(AI_CLI_CLIENT(provider));

        continue_spec = g_object_class_find_property(
            G_OBJECT_GET_CLASS(provider), "continue-session");
        if (continue_spec != NULL
            && (continue_spec->flags & G_PARAM_WRITABLE) != 0)
        {
            g_object_set(provider, "continue-session", FALSE, NULL);
        }
    }

    old_passthrough = ai_conversation_get_passthrough_commands(self);

    if (self->event_id != 0)
    {
        g_signal_handler_disconnect(self->provider, self->event_id);
        self->event_id = 0;
    }

    g_set_object(&self->provider, provider);

    if (AI_IS_EVENT_SOURCE(provider))
    {
        self->event_id = g_signal_connect(provider, "event",
                                          G_CALLBACK(on_event), self);
    }

    if (self->local_tools && AI_IS_CLI_CLIENT(provider))
    {
        self->local_tools = FALSE;
        g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LOCAL_TOOLS]);
    }

    new_passthrough = ai_conversation_get_passthrough_commands(self);
    if (!self->passthrough_set && old_passthrough != new_passthrough)
    {
        g_object_notify_by_pspec(
            G_OBJECT(self), properties[PROP_PASSTHROUGH_COMMANDS]);
    }

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PROVIDER]);

    return TRUE;
}

/**
 * ai_conversation_get_messages:
 * @self: an #AiConversation
 *
 * Returns: (transfer none) (element-type AiMessage): the history so far
 */
GList *
ai_conversation_get_messages(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->messages;
}

/**
 * ai_conversation_get_system_prompt:
 * @self: an #AiConversation
 *
 * Returns: (transfer none) (nullable): the system prompt
 */
const gchar *
ai_conversation_get_system_prompt(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->system_prompt;
}

/**
 * ai_conversation_set_system_prompt:
 * @self: an #AiConversation
 * @prompt: (nullable): the instructions
 *
 * Sets the system prompt sent with every turn.
 */
void
ai_conversation_set_system_prompt(
    AiConversation *self,
    const gchar    *prompt
){
    g_return_if_fail(AI_IS_CONVERSATION(self));

    if (g_strcmp0(self->system_prompt, prompt) == 0)
    {
        return;
    }

    g_free(self->system_prompt);
    self->system_prompt = g_strdup(prompt);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_SYSTEM_PROMPT]);
}

/**
 * ai_conversation_get_max_tokens:
 * @self: an #AiConversation
 *
 * Returns: the per-response token cap
 */
gint
ai_conversation_get_max_tokens(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), 0);

    return self->max_tokens;
}

/**
 * ai_conversation_set_max_tokens:
 * @self: an #AiConversation
 * @max_tokens: the cap
 *
 * Sets the per-response token cap.
 */
void
ai_conversation_set_max_tokens(
    AiConversation *self,
    gint            max_tokens
){
    g_return_if_fail(AI_IS_CONVERSATION(self));

    if (self->max_tokens == max_tokens)
    {
        return;
    }

    self->max_tokens = max_tokens;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_MAX_TOKENS]);
}

/**
 * ai_conversation_get_stream:
 * @self: an #AiConversation
 *
 * Returns: whether turns are streamed
 */
gboolean
ai_conversation_get_stream(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);

    return self->stream;
}

/**
 * ai_conversation_set_stream:
 * @self: an #AiConversation
 * @stream: %TRUE to stream
 *
 * Sets whether to stream turns when the provider supports it.
 */
void
ai_conversation_set_stream(
    AiConversation *self,
    gboolean        stream
){
    g_return_if_fail(AI_IS_CONVERSATION(self));

    stream = !!stream;

    if (self->stream == stream)
    {
        return;
    }

    self->stream = stream;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STREAM]);
}

/**
 * ai_conversation_get_local_tools:
 * @self: an #AiConversation
 *
 * Returns: whether tools run in this process
 */
gboolean
ai_conversation_get_local_tools(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);

    return self->local_tools;
}

/**
 * ai_conversation_set_local_tools:
 * @self: an #AiConversation
 * @local_tools: %TRUE to run tools here
 *
 * Sets whether to run tools in this process.
 *
 * Silently stays %FALSE for a CLI wrapper provider. Those ignore the tools
 * argument and run their own inside their own process, so an executor
 * pointed at one would advertise tools that never reach the model. Refusing
 * here rather than failing later is the difference between a knob that does
 * nothing and a conversation that quietly lies about what it can do.
 */
void
ai_conversation_set_local_tools(
    AiConversation *self,
    gboolean        local_tools
){
    g_return_if_fail(AI_IS_CONVERSATION(self));

    local_tools = !!local_tools;

    if (local_tools && AI_IS_CLI_CLIENT(self->provider))
    {
        g_debug("local-tools ignored: %s runs its own tools",
                G_OBJECT_TYPE_NAME(self->provider));
        local_tools = FALSE;
    }

    if (self->local_tools == local_tools)
    {
        return;
    }

    self->local_tools = local_tools;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LOCAL_TOOLS]);
}

/**
 * ai_conversation_get_busy:
 * @self: an #AiConversation
 *
 * Returns: whether a turn is in flight
 */
gboolean
ai_conversation_get_busy(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);

    return self->busy;
}

/* ================================================================
 * The input pipeline
 * ================================================================ */

/**
 * ai_conversation_set_command_set:
 * @self: an #AiConversation
 * @commands: (nullable) (transfer none): how to resolve a slash command
 *
 * Sets what ai_conversation_send_input_async() resolves commands with.
 *
 * The set's registry, if it has one, is also handed to the conversation's
 * tool executor, which is what makes the `task` and `skill` tools
 * available to the model. One assignment, because a user who can type
 * `/reviewer` expects the model to be able to reach the same agent.
 */
void
ai_conversation_set_command_set(
    AiConversation *self,
    AiCommandSet   *commands
){
    g_return_if_fail(AI_IS_CONVERSATION(self));

    if (!g_set_object(&self->command_set, commands))
    {
        return;
    }

    ai_tool_executor_set_resource_registry(
        self->executor,
        commands != NULL ? ai_command_set_get_registry(commands) : NULL);

    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_COMMAND_SET]);
}

/**
 * ai_conversation_get_command_set:
 * @self: an #AiConversation
 *
 * Returns: (transfer none) (nullable): the command set, or %NULL
 */
AiCommandSet *
ai_conversation_get_command_set(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->command_set;
}

/*
 * push_working_directory_to_provider: @self, @path, and @moved -- whether
 * this replaces a directory that was already set.  A plain comment, not
 * gtk-doc: the function is static, and g-ir-scanner reads a doc-comment
 * block even here, then warns about parameters on a symbol it cannot
 * find.  Opening this one with two stars fails the GIR-clean gate.
 *
 * A CLI provider runs in its own process, and that process's cwd comes
 * from the client's own property -- ai_cli_client_real_spawn feeds it to
 * g_subprocess_launcher_set_cwd, and claude-tmux's resolve_session_cwd
 * falls back to g_get_current_dir() without it.  Setting the executor
 * and the registry alone leaves the agent running wherever the embedding
 * process happened to start, which for a daemon is $HOME.
 *
 * The executor hop is not a substitute: ai_conversation_set_local_tools
 * refuses local tools for a CLI provider, so it is inert for exactly the
 * providers whose cwd matters.
 *
 * A CLI session is keyed to its directory -- claude-tmux derives
 * ~/.claude/projects/<encoded-cwd>/<id>.jsonl from it and claude-code's
 * --resume is cwd-relative -- so carrying a session id across a move
 * would resume a transcript that lives under the old project.  Hence
 * @moved: clear the id when the directory actually changes, but not on
 * the first NULL -> path set, which would discard an id the embedder
 * deliberately pinned before the directory was known.
 */
static void
push_working_directory_to_provider(
    AiConversation *self,
    const gchar    *path,
    gboolean        moved
){
    if (self->provider == NULL || !AI_IS_CLI_CLIENT(self->provider))
    {
        return;
    }

    ai_cli_client_set_working_directory(AI_CLI_CLIENT(self->provider), path);

    if (moved)
    {
        ai_cli_client_set_session_id(AI_CLI_CLIENT(self->provider), NULL);
    }
}

/**
 * ai_conversation_set_working_directory:
 * @self: an #AiConversation
 * @path: (nullable): the directory, or %NULL for the process's own
 *
 * Sets what mentions, commands and tools resolve paths against.
 */
void
ai_conversation_set_working_directory(
    AiConversation *self,
    const gchar    *path
){
    gboolean moved;

    g_return_if_fail(AI_IS_CONVERSATION(self));

    if (g_strcmp0(self->working_directory, path) == 0)
    {
        return;
    }

    moved = self->working_directory != NULL
        && self->working_directory[0] != '\0';

    g_free(self->working_directory);
    self->working_directory = g_strdup(path);

    /* The model's tools and the user's mentions must agree about where
     * they are; two settings that could disagree would be a bug waiting
     * to be filed. */
    ai_tool_executor_set_working_directory(self->executor, path);

    if (self->command_set != NULL)
    {
        AiResourceRegistry *registry =
            ai_command_set_get_registry(self->command_set);

        if (registry != NULL)
        {
            ai_resource_registry_set_working_directory(registry, path);
        }
    }

    push_working_directory_to_provider(self, path, moved);

    g_object_notify_by_pspec(G_OBJECT(self),
                             properties[PROP_WORKING_DIRECTORY]);
}


/**
 * ai_conversation_set_tool_endpoint:
 * @self: an #AiConversation
 * @endpoint: (nullable): where the extra tools live, or %NULL to revoke
 * @error: return location for a #GError
 *
 * Points the provider at tools it does not host itself.
 *
 * This is the CLI half of giving a conversation tools, and it is a
 * separate mechanism from #AiConversation:local-tools rather than an
 * alternative spelling of it.  A CLI wrapper runs its own tools in its
 * own process and ignores the tools argument entirely, which is why
 * set_local_tools refuses one; what it does take is a config file, an
 * environment variable or a directory saying where to find more.
 *
 * A provider that is not an #AiToolEndpointConsumer -- every HTTP client
 * -- fails with %AI_ERROR_INVALID_REQUEST rather than succeeding
 * quietly, because a caller that believes it granted tools and did not
 * is the failure this whole path exists to prevent.
 *
 * Returns: %TRUE on success
 */
gboolean
ai_conversation_set_tool_endpoint(
    AiConversation        *self,
    const AiAgentEndpoint *endpoint,
    GError               **error
){
    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    if (self->provider == NULL
        || !AI_IS_TOOL_ENDPOINT_CONSUMER(self->provider))
    {
        g_set_error(error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                    "%s cannot be handed a tool endpoint; it runs no "
                    "tools of its own",
                    self->provider != NULL
                        ? G_OBJECT_TYPE_NAME(self->provider)
                        : "this conversation");
        return FALSE;
    }

    if (!ai_tool_endpoint_consumer_apply(
            AI_TOOL_ENDPOINT_CONSUMER(self->provider), endpoint, error))
    {
        return FALSE;
    }

    g_clear_pointer(&self->tool_endpoint, ai_agent_endpoint_free);
    if (endpoint != NULL)
    {
        self->tool_endpoint = ai_agent_endpoint_copy(endpoint);
    }

    return TRUE;
}

/**
 * ai_conversation_get_tool_endpoint:
 * @self: an #AiConversation
 *
 * Returns: (transfer none) (nullable): the endpoint in force, or %NULL
 */
const AiAgentEndpoint *
ai_conversation_get_tool_endpoint(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->tool_endpoint;
}

/**
 * ai_conversation_get_working_directory:
 * @self: an #AiConversation
 *
 * Returns: (transfer none) (nullable): the directory, or %NULL
 */
const gchar *
ai_conversation_get_working_directory(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);

    return self->working_directory;
}

/**
 * ai_conversation_set_passthrough_commands:
 * @self: an #AiConversation
 * @passthrough: whether to send lines exactly as typed
 *
 * Overrides the by-provider default.
 *
 * Once set, it stays set: the guess is only made for a caller who has
 * not expressed a preference.
 */
void
ai_conversation_set_passthrough_commands(
    AiConversation *self,
    gboolean        passthrough
){
    g_return_if_fail(AI_IS_CONVERSATION(self));

    passthrough = !!passthrough;

    if (self->passthrough_set && self->passthrough == passthrough)
    {
        return;
    }

    self->passthrough_set = TRUE;
    self->passthrough = passthrough;

    g_object_notify_by_pspec(G_OBJECT(self),
                             properties[PROP_PASSTHROUGH_COMMANDS]);
}

/**
 * ai_conversation_get_passthrough_commands:
 * @self: an #AiConversation
 *
 * Whether input reaches the provider exactly as typed.
 *
 * Unless set explicitly this is %TRUE for a CLI provider and %FALSE for
 * an HTTP one. claude-code, opencode and grok resolve `@`, `/` and their
 * own skills themselves; expanding first would fight them and would stop
 * `/compact` from ever arriving.
 *
 * Returns: whether input is passed through untouched
 */
gboolean
ai_conversation_get_passthrough_commands(AiConversation *self)
{
    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);

    if (self->passthrough_set)
    {
        return self->passthrough;
    }

    return AI_IS_CLI_CLIENT(self->provider);
}

/*
 * Is this built-in one the frontend must handle even under passthrough?
 *
 * All of them, as it happens: every built-in acts on this program --- its
 * transcript, its model, its listings --- and a wrapped CLI has no
 * opinion about any of that. The function exists so the rule is written
 * down rather than implied by its absence.
 */
static gboolean
builtin_is_local(const gchar *name)
{
    (void)name;

    return TRUE;
}

/**
 * ai_conversation_resolve_input:
 * @self: an #AiConversation
 * @line: what the user typed
 * @cancellable: (nullable): interrupts a shell substitution
 * @error: (nullable): return location for a #GError
 *
 * Works out what a line of input means, without sending anything.
 *
 * This is the first half of ai_conversation_send_input_async(), exposed
 * so a frontend can implement `/expand` --- and so the decision can be
 * inspected in a test without a provider.
 *
 * Under passthrough, an unknown `/name` resolves to
 * %AI_COMMAND_OUTCOME_NOT_A_COMMAND rather than an error: it is meant for
 * the wrapped CLI, not for us.
 *
 * Returns: (transfer full) (nullable): what to do, or %NULL on error
 */
AiCommandResult *
ai_conversation_resolve_input(
    AiConversation  *self,
    const gchar     *line,
    GCancellable    *cancellable,
    GError         **error
){
    g_autoptr(AiCommandResult) result = NULL;
    g_autoptr(GError)          local_error = NULL;

    g_return_val_if_fail(AI_IS_CONVERSATION(self), NULL);
    g_return_val_if_fail(line != NULL, NULL);

    if (self->command_set == NULL)
    {
        return NULL;
    }

    result = ai_command_set_resolve(self->command_set, line,
                                    self->working_directory, cancellable,
                                    &local_error);

    if (result != NULL)
    {
        return (AiCommandResult *)g_steal_pointer(&result);
    }

    /*
     * An unknown command under passthrough is not an error --- `/compact`
     * means something to claude and nothing here, and refusing it would
     * take away a feature the wrapped CLI has.
     */
    if (ai_conversation_get_passthrough_commands(self))
    {
        return NULL;
    }

    g_propagate_error(error, g_steal_pointer(&local_error));

    return NULL;
}

/**
 * ai_conversation_send_input_async:
 * @self: an #AiConversation
 * @line: what the user typed
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the turn finishes
 * @user_data: data for @callback
 *
 * Runs a line of input through the whole pipeline and sends it.
 *
 * In order: resolve a slash command, expand `@path` mentions, record what
 * the user typed in the transcript, and send the expansion to the model.
 * It lives here rather than in each frontend so `ai`, `ai-tui` and an
 * Emacs client cannot disagree about that order --- which they would,
 * because the order is not obvious and two of the steps are easy to swap.
 *
 * A line that resolves to a built-in is *not* sent. The call completes
 * immediately and ai_conversation_send_input_finish() reports it through
 * @out_command, so the frontend can act on it.
 *
 * Under passthrough (a CLI provider, by default) the line reaches the
 * provider byte-for-byte as typed, except for built-ins, which always run
 * locally.
 */
void
ai_conversation_send_input_async(
    AiConversation      *self,
    const gchar         *line,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    g_autoptr(AiCommandResult) resolved = NULL;
    g_autoptr(GError)          local_error = NULL;
    g_autofree gchar          *expanded = NULL;
    const gchar               *to_send;

    g_return_if_fail(AI_IS_CONVERSATION(self));
    g_return_if_fail(line != NULL);

    resolved = ai_conversation_resolve_input(self, line, cancellable,
                                             &local_error);

    if (local_error != NULL)
    {
        GTask *task = g_task_new(self, cancellable, callback, user_data);

        g_task_return_error(task, g_steal_pointer(&local_error));
        g_object_unref(task);
        return;
    }

    if (resolved != NULL)
    {
        switch (ai_command_result_get_outcome(resolved))
        {
            case AI_COMMAND_OUTCOME_BUILTIN:
                if (builtin_is_local(ai_command_result_get_name(resolved)))
                {
                    GTask *task = g_task_new(self, cancellable, callback,
                                             user_data);

                    g_task_set_task_data(task,
                                         g_object_ref(resolved),
                                         g_object_unref);
                    g_task_return_boolean(task, TRUE);
                    g_object_unref(task);
                    return;
                }

                break;

            case AI_COMMAND_OUTCOME_PROMPT:
            case AI_COMMAND_OUTCOME_AGENT:
                to_send = ai_command_result_get_prompt(resolved);
                expanded = ai_mention_expand(to_send != NULL ? to_send : "",
                                             self->working_directory, 0,
                                             NULL);
                ai_conversation_send_full_async(self, line, expanded,
                                                cancellable, callback,
                                                user_data);
                return;

            case AI_COMMAND_OUTCOME_NOT_A_COMMAND:
            default:
                break;
        }
    }

    if (ai_conversation_get_passthrough_commands(self))
    {
        /* Byte-for-byte. The wrapped CLI resolves its own mentions and
         * its own commands, and doing it twice would be worse than not
         * at all. */
        ai_conversation_send_async(self, line, cancellable, callback,
                                   user_data);
        return;
    }

    expanded = ai_mention_expand(line, self->working_directory, 0, NULL);

    ai_conversation_send_full_async(self, line, expanded, cancellable,
                                    callback, user_data);
}

/**
 * ai_conversation_send_input_finish:
 * @self: an #AiConversation
 * @result: the #GAsyncResult
 * @out_command: (out) (optional) (transfer full) (nullable): the built-in
 *   the frontend must handle, or %NULL if a turn was sent
 * @error: (nullable): return location for a #GError
 *
 * Completes ai_conversation_send_input_async().
 *
 * When @out_command is non-%NULL on return, nothing was sent: the line
 * was a built-in, and its name and arguments are on the result.
 *
 * Returns: %TRUE unless @error is set
 */
gboolean
ai_conversation_send_input_finish(
    AiConversation   *self,
    GAsyncResult     *result,
    AiCommandResult **out_command,
    GError          **error
){
    g_return_val_if_fail(AI_IS_CONVERSATION(self), FALSE);
    g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

    if (out_command != NULL)
    {
        gpointer data = g_task_get_task_data(G_TASK(result));

        *out_command = (data != NULL) ? g_object_ref(data) : NULL;
    }

    return g_task_propagate_boolean(G_TASK(result), error);
}
