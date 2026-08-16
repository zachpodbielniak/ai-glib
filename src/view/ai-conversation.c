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
#include "view/ai-view-blocks.h"
#include "view/ai-view-tool-block.h"
#include "core/ai-cli-client.h"
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

    gulong          event_id;
};

G_DEFINE_TYPE(AiConversation, ai_conversation, G_TYPE_OBJECT)

enum
{
    PROP_0,
    PROP_SYSTEM_PROMPT,
    PROP_MAX_TOKENS,
    PROP_STREAM,
    PROP_LOCAL_TOOLS,
    PROP_BUSY,
    PROP_TRANSCRIPT,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
    SIGNAL_APPROVAL_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static void conversation_finish_turn(AiConversation *self, GError *error);

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
            break;
        }

        case AI_EVENT_THINKING_DELTA:
        {
            AiViewBlock *block = ensure_thinking_block(self);

            ai_view_thinking_block_append(AI_VIEW_THINKING_BLOCK(block),
                                          ai_event_get_text(event));
            break;
        }

        case AI_EVENT_TOOL_STARTED:
        {
            AiViewToolBlock *block = ensure_tool_block(self);

            ai_view_tool_block_add_call(block, ai_event_get_tool_use(event));
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

        case AI_EVENT_STREAM_START:
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

    answer = ai_tool_executor_run_finish(AI_TOOL_EXECUTOR(source), result,
                                         &error);

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

    if (answer != NULL)
    {
        self->messages = g_list_append(self->messages,
                                       ai_message_new_assistant(answer));
    }

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

    {
        g_autofree gchar *text = ai_response_get_text(response);

        if (text != NULL)
        {
            self->messages = g_list_append(self->messages,
                                           ai_message_new_assistant(text));
        }
    }

    conversation_finish_turn(self, NULL);
}

/**
 * ai_conversation_send_async:
 * @self: an #AiConversation
 * @text: what to say
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the turn ends
 * @user_data: user data for @callback
 *
 * Sends @text and drives the turn, folding what happens into the transcript.
 *
 * One turn at a time: sending while #AiConversation:busy fails rather than
 * interleaving two turns into one transcript, which would produce a
 * transcript describing neither.
 */
void
ai_conversation_send_async(
    AiConversation      *self,
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

    turn = ai_view_turn_block_new(text);
    ai_transcript_append(self->transcript, turn);

    close_open_blocks(self);
    self->open_tools = NULL;

    self->messages = g_list_append(self->messages, ai_message_new_user(text));

    self->task = task;
    self->cancellable = cancellable != NULL
        ? g_object_ref(cancellable)
        : g_cancellable_new();

    self->busy = TRUE;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BUSY]);

    if (self->local_tools)
    {
        ai_tool_executor_set_stream(self->executor, self->stream);
        ai_tool_executor_run_async(self->executor,
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

static void
ai_conversation_finalize(GObject *object)
{
    AiConversation *self = AI_CONVERSATION(object);

    if (self->event_id != 0 && self->provider != NULL)
    {
        g_signal_handler_disconnect(self->provider, self->event_id);
    }

    g_clear_object(&self->provider);
    g_clear_object(&self->transcript);
    g_clear_object(&self->executor);
    g_clear_object(&self->cancellable);
    g_clear_pointer(&self->system_prompt, g_free);
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
