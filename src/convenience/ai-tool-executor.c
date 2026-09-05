/*
 * ai-tool-executor.c - Built-in tool executor for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "ai-glib.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>
#include <libxml/uri.h>

#include "convenience/ai-tool-executor.h"
#include "core/ai-event.h"
#include "core/ai-event-source.h"
#include "core/ai-streamable.h"
#include "core/ai-subprocess-util.h"
#include "model/ai-tool-result.h"
#include "convenience/ai-search-provider.h"
#include "core/ai-error.h"
#include "core/ai-enums.h"
#include "core/ai-json-util.h"
#include "core/ai-provider.h"
#include "model/ai-content-block.h"
#include "model/ai-message.h"
#include "model/ai-response.h"
#include "model/ai-tool.h"
#include "model/ai-tool-use.h"

#define MAX_TURNS          20
#define WEB_FETCH_MAX_BYTES (100 * 1024)  /* 100 KB */
#define DEFAULT_MAX_TOKENS  4096
#define WEB_FETCH_TIMEOUT_SECS 30
/* Self-cleaning cache TTL: 15 minutes, in microseconds (monotonic clock). */
#define WEB_FETCH_CACHE_TTL_US (15 * 60 * (gint64)G_USEC_PER_SEC)
#define WEB_FETCH_USER_AGENT \
    "cmacs-ai/1.0 (ai-glib web_fetch; +https://github.com/zachp/cmacs)"

/* web_search: formatted-result cache TTL (shorter than web_fetch's, since
 * search freshness matters more than a fetched page's). */
#define WEB_SEARCH_CACHE_TTL_US (5 * 60 * (gint64)G_USEC_PER_SEC)
/* Per-result page-content excerpt cap (bytes) for fetch_content enrichment. */
#define WEB_SEARCH_EXCERPT_MAX 1500
/* Timeout for each enrichment page fetch. */
#define WEB_SEARCH_FETCH_TIMEOUT 15

/* ================================================================
 * Struct definition — must precede any code accessing its fields
 * ================================================================ */

typedef struct
{
    AiToolCallback  fn;
    gpointer        user_data;
    GDestroyNotify  user_data_free;
} CallbackEntry;

static void
callback_entry_free (gpointer data)
{
    CallbackEntry *entry = data;

    if (entry == NULL)
        return;

    if (entry->user_data_free != NULL && entry->user_data != NULL)
        entry->user_data_free (entry->user_data);

    g_slice_free (CallbackEntry, entry);
}

struct _AiToolExecutor
{
    GObject           parent_instance;
    GList            *tools;           /* GList<AiTool>, owned */
    AiSearchProvider *search_provider; /* nullable, ref'd */
    GHashTable       *callbacks;       /* str -> CallbackEntry, owned */
    AiProvider       *active_provider; /* borrowed; set only during a run() */
    gchar            *working_directory; /* nullable, owned */

    AiToolApproval    approval_policy;   /* what DEFAULT resolves to */
    gboolean          stream;            /* prefer chat_stream_async */

    /* Tool names the user answered ALLOW_ALWAYS for, for this run only. */
    GHashTable       *always_allowed;

    /* Set when a handler answered DENY_ALL; unwinds the current turn. */
    gboolean          denied_all;

    /* Provider ::event handler, live only during a run. */
    gulong            provider_event_id;
    GObject          *provider_object;

    /* Where `task` and `skill` find what they run. NULL means neither
     * tool is offered at all. */
    AiResourceRegistry *registry;

    /* The current todo list, replaced wholesale by every todo_write. */
    GPtrArray        *todos;           /* AiTodo*, owned */

    /* How many `task` calls deep this executor already is. A subagent
     * that spawns a subagent that spawns a subagent is a runaway, not a
     * plan. */
    guint             task_depth;

    /* Where background agents live. NULL means the agent_* tools are not
     * offered at all, exactly as a NULL registry means `task` is not. */
    AiBrigade        *brigade;

    /* Which optional tool groups this executor is willing to offer. */
    AiToolFeatures    features;
};

static void
ai_tool_executor_event_source_init (AiEventSourceInterface *iface)
{
    (void)iface;
}

G_DEFINE_TYPE_WITH_CODE(AiToolExecutor, ai_tool_executor, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_EVENT_SOURCE,
                                              ai_tool_executor_event_source_init))

/*
 * Property and signal IDs.
 */
enum
{
    PROP_0,
    PROP_APPROVAL_POLICY,
    PROP_STREAM,
    PROP_WORKING_DIRECTORY,
    PROP_RESOURCE_REGISTRY,
    PROP_BRIGADE,
    PROP_FEATURES,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

enum
{
    SIGNAL_APPROVAL_REQUESTED,
    SIGNAL_TODOS_CHANGED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

/*
 * Keep asking until somebody has an opinion.
 *
 * g_signal_accumulator_first_wins() halts after the first handler whatever
 * it returns, which would let a handler answering DEFAULT ("no opinion")
 * silently veto every handler behind it. This continues while the answer is
 * DEFAULT and stops at the first real decision -- which is what makes
 * DEFAULT mean what its name says.
 */
static gboolean
ai_tool_approval_accumulator(
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

    /* TRUE continues the emission. */
    return answer == AI_TOOL_APPROVAL_DEFAULT;
}

/*
 * Ask whoever is watching whether this call may run.
 *
 * Resolution order: a handler's answer, then the approval-policy property,
 * and ALLOW if that is somehow unset. With no handler connected the signal
 * accumulates to zero -- DEFAULT -- so an unwatched executor behaves exactly
 * as it did before this existed.
 */
static AiToolApproval
ai_tool_executor_ask_approval(
    AiToolExecutor *self,
    AiToolUse      *tool_use
){
    const gchar *name = ai_tool_use_get_name(tool_use);
    gint answer = AI_TOOL_APPROVAL_DEFAULT;

    if (name != NULL && self->always_allowed != NULL &&
        g_hash_table_contains(self->always_allowed, name))
    {
        return AI_TOOL_APPROVAL_ALLOW;
    }

    g_signal_emit(self, signals[SIGNAL_APPROVAL_REQUESTED], 0, tool_use,
                  &answer);

    if (answer == AI_TOOL_APPROVAL_DEFAULT)
    {
        answer = self->approval_policy;
    }

    if (answer == AI_TOOL_APPROVAL_ALLOW_ALWAYS && name != NULL)
    {
        if (self->always_allowed == NULL)
        {
            self->always_allowed =
                g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        }

        g_hash_table_add(self->always_allowed, g_strdup(name));
    }

    return (AiToolApproval)answer;
}

/* ================================================================
 * Internal run context (async -> sync bridge)
 * ================================================================ */

typedef struct
{
    /* Exactly one of these is set.  `loop' means a synchronous caller is
     * blocked in ai_tool_executor_run(); `task' means an asynchronous
     * one is waiting on ai_tool_executor_run_finish().  Everything else
     * about the turn loop is identical, which is why they share one
     * context rather than one being reimplemented in terms of the
     * other. */
    GMainLoop      *loop;
    GTask          *task;

    AiToolExecutor *executor;
    AiProvider     *provider;
    GList          *messages;      /* owned, grows during loop */
    gchar          *system_prompt; /* owned in async mode, borrowed in sync */
    gint            max_tokens;
    gint            max_turns;
    GCancellable   *cancellable;
    gint            turn_count;
    guint           n_input;
    gboolean        return_messages;
    gchar          *result;        /* final text (transfer full to caller) */
    GError         *error;         /* propagated to caller */
} RunContext;

typedef struct
{
    gchar *text;
    GList *messages;               /* AiMessage, owned */
} RunResult;

static void run_context_finish (RunContext *ctx);
static void executor_unwatch_provider (AiToolExecutor *self);
static void ai_tool_executor_get_property (GObject *object, guint prop_id,
                                           GValue *value, GParamSpec *pspec);
static void ai_tool_executor_set_property (GObject *object, guint prop_id,
                                           const GValue *value,
                                           GParamSpec *pspec);
static void on_run_response_common (RunContext *ctx, AiResponse *response);
static gboolean executor_offers_tool (AiToolExecutor *self, const gchar *name);
static AiProvider *executor_resolve_agent_provider (AiToolExecutor *self,
                                                    const gchar *provider_name,
                                                    const gchar *model,
                                                    GError **error);
static gchar *executor_first_line (const gchar *text, glong max_chars);

/* Forward declaration */
static void run_context_send (RunContext *ctx);

static void
run_result_free (RunResult *result)
{
    if (result == NULL)
        return;

    g_free (result->text);
    g_list_free_full (result->messages, g_object_unref);
    g_free (result);
}

/*
 * Detach everything appended after the caller's original messages.
 *
 * The context owns one reference to every message. Drop those corresponding
 * to the borrowed input and transfer the rest to the returned list.
 */
static GList *
run_context_take_new_messages (RunContext *ctx)
{
    GList *iter;
    GList *tail = NULL;
    guint  i = 0;

    for (iter = ctx->messages; iter != NULL; iter = iter->next, i++)
    {
        if (i < ctx->n_input)
            g_object_unref (iter->data);
        else
            tail = g_list_append (tail, iter->data);
    }

    g_list_free (ctx->messages);
    ctx->messages = NULL;

    return tail;
}

/* End the run.  In synchronous mode the blocked caller resumes and does
 * its own teardown; in asynchronous mode nobody is waiting on the stack,
 * so the context owns itself and must clean up here. */
static void
run_context_finish (RunContext *ctx)
{
    if (ctx->loop != NULL)
    {
        g_main_loop_quit (ctx->loop);
        return;
    }

    if (ctx->error != NULL)
        g_task_return_error (ctx->task, g_steal_pointer (&ctx->error));
    else if (ctx->return_messages)
    {
        RunResult *result = g_new0 (RunResult, 1);

        result->text = g_steal_pointer (&ctx->result);
        result->messages = run_context_take_new_messages (ctx);
        g_task_return_pointer (ctx->task, result,
                               (GDestroyNotify)run_result_free);
    }
    else
        g_task_return_pointer (ctx->task, g_steal_pointer (&ctx->result),
                               g_free);

    /* g_task_return_* does not consume the reference from g_task_new. */
    g_clear_object (&ctx->task);
    g_clear_object (&ctx->executor);
    g_clear_object (&ctx->provider);
    g_clear_object (&ctx->cancellable);
    g_list_free_full (ctx->messages, g_object_unref);
    g_free (ctx->system_prompt);
    g_free (ctx->result);
    g_clear_error (&ctx->error);
    g_free (ctx);
}

/*
 * Tell the model about background agents that finished while it worked.
 *
 * This is the whole notification mechanism, and it is deliberately a
 * message rather than anything cleverer: the model is only ever awake
 * between turns, so a turn boundary is the one moment it can be told
 * anything at all. The brigade holds each finish until it is collected
 * here, so news that arrives mid-turn is not lost and is not repeated.
 *
 * What it is told is that an agent finished, not what the agent said.
 * Pasting a subagent's whole answer into a conversation that did not ask
 * for it yet is how a delegated task ends up costing more context than
 * doing the work inline would have; `agent_result` fetches it when the
 * model decides it wants it.
 *
 * Returns: %TRUE when something was appended.
 */
static gboolean
run_context_report_agents (RunContext *ctx)
{
    AiBrigade          *brigade = ctx->executor->brigade;
    g_autoptr(GString)  notice  = NULL;
    gchar              *id;

    if (brigade == NULL)
        return FALSE;

    while ((id = ai_brigade_take_finished (brigade)) != NULL)
    {
        g_autofree gchar *owned = id;
        AiAgent          *agent = ai_brigade_get (brigade, owned);

        /* Reaped already -- by agent_result, or by agent_wait, which
         * both collect and forget.  The model has the answer; telling it
         * again would be noise. */
        if (agent == NULL)
            continue;

        if (notice == NULL)
            notice = g_string_new (NULL);

        g_string_append_printf (notice, "Background agent '%s' %s", owned,
                                ai_agent_state_to_string (
                                    ai_agent_get_state (agent)));

        if (ai_agent_get_description (agent) != NULL)
        {
            g_autofree gchar *what =
                executor_first_line (ai_agent_get_description (agent), 60);

            g_string_append_printf (notice, ": %s", what);
        }

        g_string_append_c (notice, '\n');
    }

    if (notice == NULL)
        return FALSE;

    g_string_append (notice,
                     "\nCollect an answer with agent_result, or carry on if "
                     "it is no longer relevant.");

    ctx->messages = g_list_append (ctx->messages,
                                   ai_message_new_user (notice->str));

    return TRUE;
}

static void
on_run_response (
    GObject      *source,
    GAsyncResult *async_result,
    gpointer      user_data
){
    RunContext *ctx = user_data;
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError)     err      = NULL;

    response = ai_provider_chat_finish (AI_PROVIDER (source), async_result, &err);

    if (err != NULL)
    {
        ctx->error = g_steal_pointer (&err);
        run_context_finish (ctx);
        return;
    }

    on_run_response_common (ctx, response);
}

/*
 * One turn's response, however it arrived.
 *
 * Split out so the streaming and non-streaming paths share every decision
 * that follows -- whether tools ran, whether the turn limit is reached,
 * whether to go round again. Two copies of that would be two chances to
 * disagree about when a run is finished.
 */
static void
on_run_response_common (
    RunContext *ctx,
    AiResponse *response
){
    GList *iter;

    ctx->turn_count++;

    if (!ai_response_has_tool_use (response))
    {
        /* Final answer — grab text and quit.
         *
         * The assistant turn is appended to the message list on the way
         * out, even though nothing in this run will read it again.  A
         * caller using ai_tool_executor_run_full() to continue the
         * conversation needs it more than any of the intermediate ones:
         * without it the model's actual answer is the single thing
         * missing from the replayed history, so the next turn reads as a
         * follow-up to a question the model never appeared to answer. */
        ctx->result = ai_response_get_text (response);
        if (ai_response_get_content_blocks (response) != NULL)
        {
            AiMessage *final_msg = ai_message_new_from_response (response);
            ctx->messages = g_list_append (ctx->messages, final_msg);
        }

        /*
         * A background agent finished while the model was composing this.
         * It has not seen that yet, so this is not the end of the turn --
         * give it the news and let it decide. Bounded by the turn limit
         * like everything else, and each finish is reported once, so this
         * cannot cycle.
         */
        if (ctx->turn_count < ctx->max_turns && run_context_report_agents (ctx))
        {
            g_clear_pointer (&ctx->result, g_free);
            run_context_send (ctx);
            return;
        }

        run_context_finish (ctx);
        return;
    }

    /* Guard against infinite loops */
    if (ctx->turn_count >= ctx->max_turns)
    {
        g_set_error (&ctx->error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "ai_tool_executor_run: reached maximum turn limit (%d)",
                     ctx->max_turns);
        run_context_finish (ctx);
        return;
    }

    /*
     * Reconstruct the assistant message from the response content blocks.
     * We must include tool_use blocks (not just text) so the provider
     * can match them with our tool_result messages on the next turn.
     */
    {
        AiMessage *assistant_msg = ai_message_new_from_response (response);

        ctx->messages = g_list_append (ctx->messages, assistant_msg);
    }

    /* Execute each tool use and append result messages */
    {
        GList *tool_uses = ai_response_get_tool_uses (response);

        for (iter = tool_uses; iter != NULL; iter = iter->next)
        {
            AiToolUse       *tool_use   = iter->data;
            const gchar     *tool_id    = ai_tool_use_get_id (tool_use);
            const gchar     *tool_name  = ai_tool_use_get_name (tool_use);
            g_autofree gchar *tool_result = NULL;
            gboolean          is_error   = FALSE;
            AiMessage        *result_msg;

            AiToolApproval approval;

            /* g_debug, not g_warning: a tool call is the loop working
             * as designed.  As a warning it aborted any program run
             * with G_DEBUG=fatal-warnings -- including a GTest suite,
             * which is why the loop had no test until now. */
            g_debug ("ToolExecutor turn %d: calling tool '%s' (id=%s)",
                       ctx->turn_count, tool_name, tool_id);

            approval = ai_tool_executor_ask_approval (ctx->executor, tool_use);

            /*
             * Announce the call before running it, not after. A tool that
             * takes thirty seconds should show as running for thirty
             * seconds, not appear retroactively when it finishes.
             */
            {
                g_autoptr(AiEvent) event = ai_event_new_tool_started (tool_use);
                ai_event_source_emit (AI_EVENT_SOURCE (ctx->executor), event);
            }

            if (approval == AI_TOOL_APPROVAL_DENY ||
                approval == AI_TOOL_APPROVAL_DENY_ALL)
            {
                /*
                 * A refusal is reported to the model as the tool's result
                 * rather than as a hole in the conversation: it will
                 * usually acknowledge it and try something else, which is
                 * more useful than a turn that simply stops.
                 */
                tool_result = g_strdup (
                    "Error: the user denied permission to run this tool.");
                is_error = TRUE;

                if (approval == AI_TOOL_APPROVAL_DENY_ALL)
                {
                    ctx->executor->denied_all = TRUE;
                }
            }
            else
            {
                g_autoptr(GError) tool_error = NULL;

                tool_result = ai_tool_executor_execute (
                    ctx->executor, tool_use, ctx->cancellable, &tool_error);

                if (tool_result == NULL)
                {
                    /*
                     * The message, not a generic stand-in. A tool that
                     * says "no agent named 'reviewr'. Available:
                     * reviewer, auditor" gives the model something to do
                     * next; "tool execution failed" gives it nothing,
                     * and every carefully worded error in this file was
                     * being thrown away here.
                     */
                    tool_result = g_strdup_printf (
                        "Error: %s",
                        tool_error != NULL ? tool_error->message
                                           : "tool execution failed");
                    is_error = TRUE;
                }
            }

            {
                g_autoptr(AiToolResult) result_block =
                    ai_tool_result_new_with_name (tool_id, tool_name,
                                                  tool_result, is_error);
                g_autoptr(AiEvent) event =
                    ai_event_new_tool_finished (tool_use, result_block);

                ai_event_source_emit (AI_EVENT_SOURCE (ctx->executor), event);
            }

            /* Pass tool_name so providers like Gemini (whose functionResponse
             * is keyed by name, not id) round-trip correctly. */
            result_msg = ai_message_new_tool_result_with_name (
                tool_id, tool_name, tool_result, is_error);
            ctx->messages = g_list_append (ctx->messages, result_msg);
        }

        g_list_free (tool_uses);
    }

    /*
     * DENY_ALL means stop, not "deny this one". The results gathered so far
     * are kept and reported as the run's error, so a caller can still see
     * what happened before the refusal.
     */
    if (ctx->executor->denied_all)
    {
        ctx->error = g_error_new_literal (AI_ERROR, AI_ERROR_CANCELLED,
                                          "tool execution denied by the user");
        run_context_finish (ctx);
        return;
    }

    /* Anything that finished while those tools ran is reported before the
     * next turn, so the model reads it alongside the results it asked
     * for rather than a turn late. */
    run_context_report_agents (ctx);

    /* Continue conversation */
    run_context_send (ctx);
}

/*
 * Republish a provider's events on the executor's own stream.
 *
 * A frontend then subscribes to exactly one object and sees the whole turn
 * -- the model's prose from the provider and the tool activity from here --
 * in the order it happened. The source label is preserved so a transcript
 * can still tell the two apart.
 */
static void
on_provider_event (
    AiEventSource *source,
    AiEvent       *event,
    gpointer       user_data
){
    AiToolExecutor *self = user_data;

    (void)source;

    ai_event_source_emit (AI_EVENT_SOURCE (self), event);
}

/*
 * Streaming finished. The response is the same shape chat_async produces,
 * so the turn logic is shared -- on_run_response decides whether tools ran
 * and whether to go round again.
 */
/*
 * Subscribe to the provider for the duration of a run.
 *
 * Borrowed, not owned: the caller owns the provider and outlives the run.
 * The handler is disconnected when the run ends, so an executor reused
 * against a second provider never keeps forwarding the first one's events.
 */
static void
executor_watch_provider (
    AiToolExecutor *self,
    AiProvider     *provider
){
    if (self->provider_event_id != 0 && self->provider_object != NULL)
    {
        g_signal_handler_disconnect (self->provider_object,
                                     self->provider_event_id);
        self->provider_event_id = 0;
        self->provider_object = NULL;
    }

    if (provider == NULL || !AI_IS_EVENT_SOURCE (provider))
        return;

    self->provider_object = G_OBJECT (provider);
    self->provider_event_id = g_signal_connect (provider, "event",
                                                G_CALLBACK (on_provider_event),
                                                self);
}

static void
executor_unwatch_provider (AiToolExecutor *self)
{
    if (self->provider_event_id != 0 && self->provider_object != NULL)
    {
        g_signal_handler_disconnect (self->provider_object,
                                     self->provider_event_id);
    }

    self->provider_event_id = 0;
    self->provider_object = NULL;
}

static void
on_run_stream_done (
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    RunContext *ctx = user_data;
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;

    response = ai_streamable_chat_stream_finish (AI_STREAMABLE (source),
                                                 result, &error);

    if (response == NULL)
    {
        ctx->error = error != NULL
            ? g_steal_pointer (&error)
            : g_error_new_literal (AI_ERROR, AI_ERROR_INVALID_RESPONSE,
                                   "streaming produced no response");
        run_context_finish (ctx);
        return;
    }

    on_run_response_common (ctx, response);
}

static void
run_context_send (RunContext *ctx)
{
    /*
     * Streaming and the tool loop used to be mutually exclusive: this always
     * called chat_async, so a caller got live tokens or tool execution and
     * never both. The property defaults to FALSE, so every existing caller
     * takes exactly the path it took before.
     */
    if (ctx->executor->stream && AI_IS_STREAMABLE (ctx->provider))
    {
        ai_streamable_chat_stream_async (
            AI_STREAMABLE (ctx->provider),
            ctx->messages,
            ctx->system_prompt,
            ctx->max_tokens,
            ctx->executor->tools,
            ctx->cancellable,
            on_run_stream_done,
            ctx
        );
        return;
    }

    ai_provider_chat_async (
        ctx->provider,
        ctx->messages,
        ctx->system_prompt,
        ctx->max_tokens,
        ctx->executor->tools,
        ctx->cancellable,
        on_run_response,
        ctx
    );
}

/* ================================================================
 * Built-in tool implementations
 * ================================================================ */

/* Resolve PATH against the executor's working directory.
 *
 * An agent is given work to do somewhere -- a project, a checkout, a
 * notes tree -- and says "src/main.c", not an absolute path.  Without
 * this every relative path resolved against whatever directory the
 * host process happened to be in, which for an editor is wherever the
 * user last visited a file.  An absolute path is left alone, and with
 * no working directory set the behaviour is exactly as before. */
static gchar *
executor_resolve_path (
    AiToolExecutor  *self,
    const gchar     *path
){
    if (path == NULL)
        path = ".";

    if (self == NULL || self->working_directory == NULL
        || g_path_is_absolute (path))
        return g_strdup (path);

    return g_build_filename (self->working_directory, path, NULL);
}

static gchar *
tool_bash (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar                  *command;
    g_autoptr (GSubprocessLauncher) launcher = NULL;
    g_autoptr (GSubprocess)         proc     = NULL;
    g_autoptr (GBytes)              out      = NULL;
    g_autoptr (GError)              comm_error = NULL;
    gconstpointer                   data;
    gsize                           len      = 0;
    gchar                          *result;
    gint                            exit_status;

    command = ai_tool_use_get_input_string (tool_use, "command");
    if (command == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "bash: missing required parameter 'command'");
        return NULL;
    }

    if (g_cancellable_set_error_if_cancelled (cancellable, error))
        return NULL;

    /* GSubprocess rather than popen(): popen inherits the host
     * process's working directory with no way to override it, and
     * cannot be cancelled.  Both matter -- an agent is given work to do
     * in a particular tree, and a caller that gives up on a run should
     * not leave a build running. */
    launcher = g_subprocess_launcher_new (G_SUBPROCESS_FLAGS_STDOUT_PIPE
                                          | G_SUBPROCESS_FLAGS_STDERR_MERGE);
    if (self != NULL && self->working_directory != NULL)
        g_subprocess_launcher_set_cwd (launcher, self->working_directory);

    proc = g_subprocess_launcher_spawn (launcher, error,
                                        "/bin/sh", "-c", command, NULL);
    if (proc == NULL)
        return NULL;

    if (!ai_subprocess_communicate_bounded (proc, NULL, 0, cancellable,
                                           &out, NULL, &comm_error))
    {
        /* Preserve the tool's existing cancellation error domain. */
        if (g_error_matches (comm_error, AI_ERROR, AI_ERROR_CANCELLED))
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "bash: command cancelled");
        else
            g_propagate_error (error, g_steal_pointer (&comm_error));
        return NULL;
    }

    data = out ? g_bytes_get_data (out, &len) : NULL;
    /* Command output is arbitrary bytes -- a build log carries progress
     * bars and stray locale-encoded text -- and it is about to become a
     * JSON string, so it has to be valid UTF-8 first. */
    result = data ? g_utf8_make_valid ((const gchar *) data, (gssize) len)
                  : g_strdup ("");

    exit_status = g_subprocess_get_if_exited (proc)
                  ? g_subprocess_get_exit_status (proc) : -1;

    if (exit_status > 0)
    {
        gchar *prefixed = g_strdup_printf ("[exit code %d]\n%s",
                                           exit_status, result);
        g_free (result);
        result = prefixed;
    }

    return result;
}

static gchar *
tool_read (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar      *path;
    g_autofree gchar *contents = NULL;
    gsize             length;
    gint              offset;
    gint              limit;
    const gchar      *start;
    gsize             available;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path = ai_tool_use_get_input_string (tool_use, "path");
    if (path == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "read: missing required parameter 'path'");
        return NULL;
    }

    resolved = executor_resolve_path (self, path);
    path = resolved;

    if (!g_file_get_contents (path, &contents, &length, error))
        return NULL;

    offset = ai_tool_use_get_input_int (tool_use, "offset", 0);
    limit  = ai_tool_use_get_input_int (tool_use, "limit", -1);

    if (offset < 0)
        offset = 0;
    if ((gsize)offset >= length)
        return g_strdup ("");

    start     = contents + (gsize)offset;
    available = length   - (gsize)offset;

    if (limit > 0 && (gsize)limit < available)
        available = (gsize)limit;

    return g_strndup (start, available);
}

static gchar *
tool_write (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *path;
    const gchar *content;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path    = ai_tool_use_get_input_string (tool_use, "path");
    content = ai_tool_use_get_input_string (tool_use, "content");

    if (path == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "write: missing required parameter 'path'");
        return NULL;
    }

    resolved = executor_resolve_path (self, path);
    path = resolved;

    if (content == NULL)
        content = "";

    if (!g_file_set_contents (path, content, -1, error))
        return NULL;

    return g_strdup ("OK");
}

static gchar *
tool_edit (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar      *path;
    const gchar      *old_string;
    const gchar      *new_string;
    g_autofree gchar *contents = NULL;
    gsize             length;
    gchar            *found;
    GString          *rebuilt;
    gsize             prefix_len;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path       = ai_tool_use_get_input_string (tool_use, "path");
    old_string = ai_tool_use_get_input_string (tool_use, "old_string");
    new_string = ai_tool_use_get_input_string (tool_use, "new_string");

    if (path == NULL || old_string == NULL || new_string == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "edit: missing required parameter(s): "
                             "path, old_string, new_string");
        return NULL;
    }

    resolved = executor_resolve_path (self, path);
    path = resolved;

    if (!g_file_get_contents (path, &contents, &length, error))
        return NULL;

    /* String replacement cannot represent bytes past an embedded NUL. */
    if (memchr (contents, '\0', length) != NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                             "edit: file contains NUL bytes; no edits were applied");
        return NULL;
    }

    found = strstr (contents, old_string);
    if (found == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "edit: old_string not found in '%s'", path);
        return NULL;
    }

    prefix_len = (gsize)(found - contents);
    rebuilt    = g_string_new_len (contents, (gssize)prefix_len);
    g_string_append (rebuilt, new_string);
    g_string_append (rebuilt, found + strlen (old_string));

    if (!g_file_set_contents (path, rebuilt->str, (gssize)rebuilt->len, error))
    {
        g_string_free (rebuilt, TRUE);
        return NULL;
    }

    g_string_free (rebuilt, TRUE);
    return g_strdup ("OK");
}

/* ---- glob helpers ---- */

static void
glob_collect (
    const gchar  *base_dir,
    GPatternSpec *pattern,
    GString      *output
){
    g_autoptr(GError) dir_err = NULL;
    GDir        *dir;
    const gchar *name;

    dir = g_dir_open (base_dir, 0, &dir_err);
    if (dir == NULL)
        return;

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        g_autofree gchar *full = g_build_filename (base_dir, name, NULL);

        if (g_file_test (full, G_FILE_TEST_IS_DIR))
        {
            /* Follow an explicitly requested root, but not directory links
             * encountered during traversal: they can lead back to an ancestor. */
            if (!g_file_test (full, G_FILE_TEST_IS_SYMLINK))
                glob_collect (full, pattern, output);
        }
        else if (g_pattern_spec_match_string (pattern, name))
        {
            g_string_append (output, full);
            g_string_append_c (output, '\n');
        }
    }

    g_dir_close (dir);
}

static gchar *
tool_glob (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar          *pattern_str;
    const gchar          *path;
    g_autoptr(GPatternSpec) pattern = NULL;
    GString              *output;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;
    (void)error;

    pattern_str = ai_tool_use_get_input_string (tool_use, "pattern");
    if (pattern_str == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "glob: missing required parameter 'pattern'");
        return NULL;
    }

    path = ai_tool_use_get_input_string (tool_use, "path");
    resolved = executor_resolve_path (self, path);
    path = resolved;

    pattern = g_pattern_spec_new (pattern_str);
    output  = g_string_new (NULL);

    glob_collect (path, pattern, output);

    return g_string_free (output, FALSE);
}

/* ---- grep helpers ---- */

static void
grep_one_file (
    const gchar *filepath,
    GRegex      *regex,
    GString     *output
){
    g_autofree gchar  *contents = NULL;
    gsize              length;
    gchar            **lines;
    gint               i;
    gint               line_num;

    if (!g_file_get_contents (filepath, &contents, &length, NULL))
        return;

    lines = g_strsplit (contents, "\n", -1);

    for (i = 0, line_num = 1; lines[i] != NULL; i++, line_num++)
    {
        if (g_regex_match (regex, lines[i], 0, NULL))
        {
            g_string_append_printf (output, "%s:%d: %s\n",
                                    filepath, line_num, lines[i]);
        }
    }

    g_strfreev (lines);
}

static void
grep_dir_recurse (
    const gchar  *base_dir,
    GPatternSpec *file_pattern, /* nullable — match all files */
    GRegex       *regex,
    GString      *output
){
    g_autoptr(GError) dir_err = NULL;
    GDir        *dir;
    const gchar *name;

    dir = g_dir_open (base_dir, 0, &dir_err);
    if (dir == NULL)
        return;

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        g_autofree gchar *full = g_build_filename (base_dir, name, NULL);

        if (g_file_test (full, G_FILE_TEST_IS_DIR))
        {
            if (!g_file_test (full, G_FILE_TEST_IS_SYMLINK))
                grep_dir_recurse (full, file_pattern, regex, output);
        }
        else if (file_pattern == NULL
                 || g_pattern_spec_match_string (file_pattern, name))
        {
            grep_one_file (full, regex, output);
        }
    }

    g_dir_close (dir);
}

static gchar *
tool_grep (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar           *pattern_str;
    const gchar           *path;
    const gchar           *glob_str;
    g_autoptr(GRegex)      regex        = NULL;
    g_autoptr(GPatternSpec) file_pattern = NULL;
    GString               *output;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    pattern_str = ai_tool_use_get_input_string (tool_use, "pattern");
    if (pattern_str == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "grep: missing required parameter 'pattern'");
        return NULL;
    }

    regex = g_regex_new (pattern_str, 0, 0, error);
    if (regex == NULL)
        return NULL;

    path     = ai_tool_use_get_input_string (tool_use, "path");
    glob_str = ai_tool_use_get_input_string (tool_use, "glob");

    if (glob_str != NULL)
        file_pattern = g_pattern_spec_new (glob_str);

    resolved = executor_resolve_path (self, path);
    path = resolved;

    output = g_string_new (NULL);

    if (g_file_test (path, G_FILE_TEST_IS_DIR))
        grep_dir_recurse (path, file_pattern, regex, output);
    else
        grep_one_file (path, regex, output);

    return g_string_free (output, FALSE);
}

static gchar *
tool_ls (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *path;
    GDir        *dir;
    const gchar *name;
    GString     *output;
    g_autofree gchar *resolved = NULL;

    (void)cancellable;

    path = ai_tool_use_get_input_string (tool_use, "path");
    resolved = executor_resolve_path (self, path);
    path = resolved;

    dir = g_dir_open (path, 0, error);
    if (dir == NULL)
        return NULL;

    output = g_string_new (NULL);

    while ((name = g_dir_read_name (dir)) != NULL)
    {
        g_autofree gchar *full = g_build_filename (path, name, NULL);
        struct stat  st;
        const gchar *type;

        if (stat (full, &st) == 0)
        {
            type = S_ISDIR (st.st_mode) ? "d" : "-";
            g_string_append_printf (output, "%s  %10" G_GINT64_FORMAT "  %s\n",
                                    type, (gint64)st.st_size, name);
        }
        else
        {
            g_string_append_printf (output, "?  %s\n", name);
        }
    }

    g_dir_close (dir);

    return g_string_free (output, FALSE);
}

/* ---- web_fetch: response cache (URL -> converted text, 15 min TTL) ---- */

typedef struct
{
    gint64  stamp;   /* g_get_monotonic_time() at insertion (microseconds) */
    gchar  *text;    /* converted/cleaned body, owned */
} WebFetchCacheEntry;

static GHashTable *web_fetch_cache = NULL;     /* url -> WebFetchCacheEntry */
static GMutex      web_fetch_cache_lock;       /* static GMutex: zero-init OK */

static void
web_fetch_cache_entry_free (gpointer data)
{
    WebFetchCacheEntry *entry = data;

    if (entry == NULL)
        return;

    g_free (entry->text);
    g_slice_free (WebFetchCacheEntry, entry);
}

/* Drop every expired entry. Caller must hold web_fetch_cache_lock. */
static void
web_fetch_cache_sweep_locked (gint64 now)
{
    GHashTableIter iter;
    gpointer       key;
    gpointer       value;

    if (web_fetch_cache == NULL)
        return;

    g_hash_table_iter_init (&iter, web_fetch_cache);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        WebFetchCacheEntry *entry = value;

        if (now - entry->stamp > WEB_FETCH_CACHE_TTL_US)
            g_hash_table_iter_remove (&iter);
    }
}

/* Returns a fresh copy of the cached text for URL, or NULL on miss/expiry. */
static gchar *
web_fetch_cache_lookup (const gchar *url)
{
    gchar  *result = NULL;
    gint64  now;

    g_mutex_lock (&web_fetch_cache_lock);
    now = g_get_monotonic_time ();

    if (web_fetch_cache != NULL)
    {
        WebFetchCacheEntry *entry = g_hash_table_lookup (web_fetch_cache, url);

        if (entry != NULL)
        {
            if (now - entry->stamp <= WEB_FETCH_CACHE_TTL_US)
                result = g_strdup (entry->text);
            else
                g_hash_table_remove (web_fetch_cache, url);
        }

        web_fetch_cache_sweep_locked (now);
    }

    g_mutex_unlock (&web_fetch_cache_lock);
    return result;
}

static void
web_fetch_cache_store (const gchar *url, const gchar *text)
{
    WebFetchCacheEntry *entry;

    g_mutex_lock (&web_fetch_cache_lock);

    if (web_fetch_cache == NULL)
        web_fetch_cache = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, web_fetch_cache_entry_free);

    entry = g_slice_new0 (WebFetchCacheEntry);
    entry->stamp = g_get_monotonic_time ();
    entry->text  = g_strdup (text);

    g_hash_table_replace (web_fetch_cache, g_strdup (url), entry);

    g_mutex_unlock (&web_fetch_cache_lock);
}

/* ---- web_search: formatted-result cache (key -> formatted text, 5 min) ----
 * Reuses WebFetchCacheEntry. Keyed by provider + query + options so repeated
 * identical web_search calls within one agent run skip the network. */

static GHashTable *web_search_cache = NULL;     /* key -> WebFetchCacheEntry */
static GMutex      web_search_cache_lock;       /* static GMutex: zero-init OK */

static void
web_search_cache_sweep_locked (gint64 now)
{
    GHashTableIter iter;
    gpointer       key;
    gpointer       value;

    if (web_search_cache == NULL)
        return;

    g_hash_table_iter_init (&iter, web_search_cache);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        WebFetchCacheEntry *entry = value;

        if (now - entry->stamp > WEB_SEARCH_CACHE_TTL_US)
            g_hash_table_iter_remove (&iter);
    }
}

static gchar *
web_search_cache_lookup (const gchar *key)
{
    gchar  *result = NULL;
    gint64  now;

    g_mutex_lock (&web_search_cache_lock);
    now = g_get_monotonic_time ();

    if (web_search_cache != NULL)
    {
        WebFetchCacheEntry *entry = g_hash_table_lookup (web_search_cache, key);

        if (entry != NULL)
        {
            if (now - entry->stamp <= WEB_SEARCH_CACHE_TTL_US)
                result = g_strdup (entry->text);
            else
                g_hash_table_remove (web_search_cache, key);
        }

        web_search_cache_sweep_locked (now);
    }

    g_mutex_unlock (&web_search_cache_lock);
    return result;
}

static void
web_search_cache_store (const gchar *key, const gchar *text)
{
    WebFetchCacheEntry *entry;

    g_mutex_lock (&web_search_cache_lock);

    if (web_search_cache == NULL)
        web_search_cache = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, web_fetch_cache_entry_free);

    entry = g_slice_new0 (WebFetchCacheEntry);
    entry->stamp = g_get_monotonic_time ();
    entry->text  = g_strdup (text);

    g_hash_table_replace (web_search_cache, g_strdup (key), entry);

    g_mutex_unlock (&web_search_cache_lock);
}

/* ---- web_fetch: HTML -> readable markdown-ish text ---- */

static gboolean
node_name_is (xmlNode *node, const gchar *name)
{
    return node->name != NULL
        && g_ascii_strcasecmp ((const gchar *)node->name, name) == 0;
}

/* Find the first <base href> in the tree (HTML's per-document base override),
 * returning its href (caller frees with xmlFree) or NULL. */
static xmlChar *
html_find_base_href (xmlNode *node)
{
    xmlNode *cur;

    for (cur = node; cur != NULL; cur = cur->next)
    {
        if (cur->type != XML_ELEMENT_NODE)
            continue;

        if (node_name_is (cur, "base"))
        {
            xmlChar *href = xmlGetProp (cur, (const xmlChar *)"href");

            if (href != NULL && href[0] != '\0')
                return href;
            if (href != NULL)
                xmlFree (href);
        }

        {
            xmlChar *found = html_find_base_href (cur->children);

            if (found != NULL)
                return found;
        }
    }
    return NULL;
}

/* Walk the HTML tree appending markdown-ish text. BASE (nullable) is the
 * effective base URL used to resolve <img> srcs to absolute URLs. */
static void
html_walk (xmlNode *node, GString *out, const gchar *base)
{
    xmlNode *cur;

    for (cur = node; cur != NULL; cur = cur->next)
    {
        if (cur->type == XML_TEXT_NODE)
        {
            if (cur->content != NULL)
                g_string_append (out, (const gchar *)cur->content);
            continue;
        }

        if (cur->type != XML_ELEMENT_NODE)
            continue;

        /* Skip non-content subtrees entirely. */
        if (node_name_is (cur, "script")   || node_name_is (cur, "style")
            || node_name_is (cur, "head")  || node_name_is (cur, "noscript")
            || node_name_is (cur, "svg")   || node_name_is (cur, "template"))
            continue;

        if (node_name_is (cur, "br"))
        {
            g_string_append_c (out, '\n');
            continue;
        }

        /* Headings h1..h6 -> markdown '#' prefixes. */
        if (cur->name != NULL
            && (cur->name[0] == 'h' || cur->name[0] == 'H')
            && cur->name[1] >= '1' && cur->name[1] <= '6'
            && cur->name[2] == '\0')
        {
            gint level = cur->name[1] - '0';
            gint i;

            g_string_append_c (out, '\n');
            for (i = 0; i < level; i++)
                g_string_append_c (out, '#');
            g_string_append_c (out, ' ');
            html_walk (cur->children, out, base);
            g_string_append_c (out, '\n');
            continue;
        }

        /* Links -> [text](href). */
        if (node_name_is (cur, "a"))
        {
            xmlChar *href = xmlGetProp (cur, (const xmlChar *)"href");

            if (href != NULL && href[0] != '\0')
            {
                g_string_append_c (out, '[');
                html_walk (cur->children, out, base);
                g_string_append_printf (out, "](%s)", (const gchar *)href);
            }
            else
            {
                html_walk (cur->children, out, base);
            }

            if (href != NULL)
                xmlFree (href);
            continue;
        }

        /* Images -> ![alt](src).  The src is resolved to an absolute URL
         * against the document base (set from the page URL at parse time, or
         * a <base href>), so the model gets a directly-fetchable link rather
         * than a site-relative or protocol-relative path. */
        if (node_name_is (cur, "img"))
        {
            xmlChar *src = xmlGetProp (cur, (const xmlChar *)"src");

            if (src != NULL && src[0] != '\0')
            {
                xmlChar *alt = xmlGetProp (cur, (const xmlChar *)"alt");
                xmlChar *abs = xmlBuildURI (src, (const xmlChar *)base);
                const xmlChar *url = (abs != NULL) ? abs : src;

                g_string_append_printf (out, "![%s](%s)",
                                        (alt != NULL && alt[0] != '\0')
                                          ? (const gchar *)alt : "",
                                        (const gchar *)url);
                if (abs != NULL) xmlFree (abs);
                if (alt != NULL) xmlFree (alt);
            }
            if (src != NULL)
                xmlFree (src);
            continue;   /* void element: no children */
        }

        /* List items get a bullet; block elements get surrounding newlines. */
        if (node_name_is (cur, "li"))
        {
            g_string_append (out, "\n- ");
            html_walk (cur->children, out, base);
            g_string_append_c (out, '\n');
            continue;
        }

        {
            gboolean block =
                node_name_is (cur, "p")       || node_name_is (cur, "div")
                || node_name_is (cur, "tr")   || node_name_is (cur, "ul")
                || node_name_is (cur, "ol")   || node_name_is (cur, "table")
                || node_name_is (cur, "section") || node_name_is (cur, "article")
                || node_name_is (cur, "header")  || node_name_is (cur, "footer")
                || node_name_is (cur, "blockquote");

            if (block)
                g_string_append_c (out, '\n');

            html_walk (cur->children, out, base);

            if (block)
                g_string_append_c (out, '\n');
        }
    }
}

/* Parse HTML and return cleaned text (transfer full), or NULL on parse fail.
 * ENCODING is the charset hint (e.g. from the HTTP Content-Type); when NULL we
 * default to UTF-8 rather than libxml's legacy ISO-8859-1, since that is the
 * dominant encoding for modern pages that omit a charset declaration.
 * BASE_URL (nullable) is the page's URL; it becomes the document base so that
 * <img> srcs (and <base href>) resolve to absolute, fetchable URLs. */
static gchar *
html_to_text (const gchar *html, gsize len, const gchar *encoding,
              const gchar *base_url)
{
    htmlDocPtr   doc;
    xmlNode     *root;
    GString     *out;
    gchar       *raw;
    GString     *clean;
    const gchar *p;
    const gchar *enc;
    gint         nl_run;
    gboolean     space_pending;
    gboolean     at_line_start;

    enc = (encoding != NULL && encoding[0] != '\0') ? encoding : "UTF-8";
    doc = htmlReadMemory (html, (int)len, base_url, enc,
                          HTML_PARSE_RECOVER | HTML_PARSE_NOERROR
                          | HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (doc == NULL)
        return NULL;

    out  = g_string_new (NULL);
    root = xmlDocGetRootElement (doc);
    if (root != NULL)
    {
        g_autofree gchar *eff_base = NULL;
        xmlChar          *bhref    = html_find_base_href (root);

        if (bhref != NULL)
        {
            /* A <base href> can itself be relative to the page URL. */
            xmlChar *abs = (base_url != NULL)
                           ? xmlBuildURI (bhref, (const xmlChar *)base_url)
                           : NULL;
            eff_base = g_strdup ((const gchar *)(abs != NULL ? abs : bhref));
            if (abs != NULL)
                xmlFree (abs);
            xmlFree (bhref);
        }
        else if (base_url != NULL)
        {
            eff_base = g_strdup (base_url);
        }

        html_walk (root, out, eff_base);
    }
    xmlFreeDoc (doc);

    /* Whitespace cleanup: collapse spaces/tabs to one, drop leading spaces,
     * and limit consecutive blank lines to a single one. */
    raw           = g_string_free (out, FALSE);
    clean         = g_string_new (NULL);
    nl_run        = 0;
    space_pending = FALSE;
    at_line_start = TRUE;

    for (p = raw; *p != '\0'; p++)
    {
        gchar c = *p;

        if (c == '\r')
            continue;

        if (c == '\n')
        {
            nl_run++;
            space_pending = FALSE;
            continue;
        }

        if (c == ' ' || c == '\t')
        {
            space_pending = TRUE;
            continue;
        }

        if (nl_run > 0)
        {
            gint emit = (nl_run >= 2) ? 2 : 1;
            gint i;

            for (i = 0; i < emit; i++)
                g_string_append_c (clean, '\n');

            nl_run        = 0;
            at_line_start = TRUE;
            space_pending = FALSE;
        }

        if (space_pending && !at_line_start)
            g_string_append_c (clean, ' ');
        space_pending = FALSE;

        g_string_append_c (clean, c);
        at_line_start = FALSE;
    }

    g_free (raw);

    while (clean->len > 0
           && (clean->str[clean->len - 1] == '\n'
               || clean->str[clean->len - 1] == ' '))
        g_string_truncate (clean, clean->len - 1);

    return g_string_free (clean, FALSE);
}

/* Truncate BODY in place to at most MAX bytes on a valid UTF-8 boundary,
 * appending a marker when truncation occurred. No-op if already within MAX. */
static void
web_fetch_truncate (GString *body, gsize max)
{
    const gchar *valid_end;
    gsize        cut = max;

    if (body->len <= max)
        return;

    /* Never split a multi-byte UTF-8 sequence: if the prefix up to the cap
     * is not valid UTF-8, back the cut up to the last good boundary. */
    if (!g_utf8_validate (body->str, (gssize)cut, &valid_end))
        cut = (gsize)(valid_end - body->str);

    g_string_truncate (body, cut);
    g_string_append (body, "\n\n[truncated at 100 KB]");
}

/* Build the model-facing body (transfer full) from a fetched response.
 * Prepends a redirect notice when the final host differs from the requested
 * one, converts HTML to text (other text-ish types pass through, binary gets a
 * placeholder), and truncates to the size cap. Pure: no network or Soup types,
 * so it is unit-testable with synthetic inputs.
 *   CONTENT_TYPE / CHARSET : from the response (both nullable).
 *   REQ_HOST / FINAL_HOST  : requested vs final host (nullable; notice emitted
 *                            only when both are set and differ).
 *   FINAL_URL              : full final URL for the notice text (nullable). */
static gchar *
web_fetch_build_body (
    const gchar *data,
    gsize        size,
    const gchar *content_type,
    const gchar *charset,
    const gchar *req_host,
    const gchar *final_host,
    const gchar *final_url
){
    GString *body = g_string_new (NULL);

    if (req_host != NULL && final_host != NULL
        && g_ascii_strcasecmp (req_host, final_host) != 0)
    {
        g_string_append_printf (body, "[redirected to %s]\n\n",
                                final_url ? final_url : final_host);
    }

    if (content_type != NULL
        && (g_ascii_strcasecmp (content_type, "text/html") == 0
            || g_ascii_strcasecmp (content_type, "application/xhtml+xml") == 0))
    {
        g_autofree gchar *text = html_to_text (data, size, charset, final_url);

        if (text != NULL)
            g_string_append (body, text);
        else
            g_string_append_len (body, data, (gssize)size);
    }
    else if (content_type == NULL
             || g_str_has_prefix (content_type, "text/")
             || g_ascii_strcasecmp (content_type, "application/json") == 0
             || g_ascii_strcasecmp (content_type, "application/xml") == 0
             || g_ascii_strcasecmp (content_type, "application/javascript") == 0)
    {
        g_string_append_len (body, data, (gssize)size);
    }
    else
    {
        g_string_append_printf (body,
            "[binary content: %" G_GSIZE_FORMAT " bytes, type %s "
            "\xe2\x80\x94 not returned as text]", size, content_type);
    }

    web_fetch_truncate (body, WEB_FETCH_MAX_BYTES);

    return g_string_free (body, FALSE);
}

/* ---- web_fetch: optional prompt-based extraction via a sub-model ---- */

typedef struct
{
    GMainLoop *loop;
    gchar     *result;
    GError    *error;
} WebFetchExtractCtx;

static void
on_web_fetch_extract (GObject *source, GAsyncResult *res, gpointer user_data)
{
    WebFetchExtractCtx   *ec       = user_data;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish (AI_PROVIDER (source), res, &ec->error);
    if (response != NULL)
        ec->result = ai_response_get_text (response);

    g_main_loop_quit (ec->loop);
}

/* Run PROMPT over CONTENT with PROVIDER and return the model's text, or NULL
 * (with a warning) so the caller can fall back to the raw content. */
static gchar *
web_fetch_extract (
    AiProvider   *provider,
    const gchar  *url,
    const gchar  *content,
    const gchar  *prompt,
    GCancellable *cancellable
){
    WebFetchExtractCtx  ec        = { NULL, NULL, NULL };
    GList              *messages  = NULL;
    g_autofree gchar   *user_text = NULL;
    AiMessage          *msg;

    user_text = g_strdup_printf (
        "Content fetched from %s:\n\n%s\n\nTask: %s", url, content, prompt);
    msg      = ai_message_new_user (user_text);
    messages = g_list_append (NULL, msg);

    ec.loop = g_main_loop_new (g_main_context_get_thread_default (), FALSE);
    ai_provider_chat_async (
        provider, messages,
        "You extract and summarise fetched web content. Return only what the "
        "task asks for, concisely, with no preamble.",
        DEFAULT_MAX_TOKENS,
        NULL,            /* no tools for the sub-call */
        cancellable,
        on_web_fetch_extract, &ec);
    g_main_loop_run (ec.loop);
    g_main_loop_unref (ec.loop);

    g_list_free_full (messages, g_object_unref);

    if (ec.error != NULL)
    {
        g_warning ("web_fetch: prompt extraction failed: %s", ec.error->message);
        g_clear_error (&ec.error);
        return NULL;
    }

    return ec.result;   /* may be NULL if the model returned no text */
}

/* TRUE if an http:// URL should be speculatively upgraded to https://.
 * False for localhost and IP-literal hosts (dev/internal endpoints). */
static gboolean
web_fetch_should_upgrade (const gchar *url)
{
    g_autoptr(GUri)  uri  = NULL;
    const gchar     *host;

    uri = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
    if (uri == NULL)
        return FALSE;

    host = g_uri_get_host (uri);
    if (host == NULL || host[0] == '\0')
        return FALSE;

    if (g_ascii_strcasecmp (host, "localhost") == 0)
        return FALSE;

    if (g_hostname_is_ip_address (host))
        return FALSE;

    return TRUE;
}

/* GET URL with our headers; returns body bytes and (transfer full) *out_msg. */
static GBytes *
web_fetch_get (
    SoupSession   *session,
    const gchar   *url,
    SoupMessage  **out_msg,
    GCancellable  *cancellable,
    GError       **error
){
    SoupMessage        *msg;
    SoupMessageHeaders *req_headers;
    GBytes             *bytes;

    msg = soup_message_new ("GET", url);
    if (msg == NULL)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "web_fetch: invalid URL '%s'", url);
        return NULL;
    }

    req_headers = soup_message_get_request_headers (msg);
    soup_message_headers_replace (req_headers, "User-Agent",
                                  WEB_FETCH_USER_AGENT);
    soup_message_headers_replace (req_headers, "Accept",
        "text/html,application/xhtml+xml,text/plain,"
        "application/json;q=0.9,*/*;q=0.8");

    bytes = soup_session_send_and_read (session, msg, cancellable, error);
    if (bytes == NULL)
    {
        g_object_unref (msg);
        return NULL;
    }

    *out_msg = msg;
    return bytes;
}

static gchar *
tool_web_fetch (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar            *url;
    const gchar            *prompt;
    g_autofree gchar       *converted = NULL;

    url = ai_tool_use_get_input_string (tool_use, "url");
    if (url == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "web_fetch: missing required parameter 'url'");
        return NULL;
    }

    prompt = ai_tool_use_get_input_string (tool_use, "prompt");

    /* 1. Cache lookup (keyed on the original URL; stores converted text). */
    converted = web_fetch_cache_lookup (url);

    /* 2. On miss, fetch and convert. */
    if (converted == NULL)
    {
        g_autoptr(SoupSession)  session    = NULL;
        g_autoptr(SoupMessage)  msg        = NULL;
        g_autoptr(GBytes)       bytes      = NULL;
        g_autoptr(GError)       local_err  = NULL;
        g_autofree gchar       *fetch_url  = NULL;
        g_autoptr(GUri)         req_uri    = NULL;
        gboolean                upgraded   = FALSE;
        guint                   status_code;
        const gchar            *data;
        gsize                   size;
        const gchar            *content_type;
        GHashTable             *ct_params = NULL;
        const gchar            *charset;
        SoupMessageHeaders     *resp_headers;
        GUri                   *final_uri;
        const gchar            *req_host;
        const gchar            *final_host;
        gchar                  *final_str;

        session = soup_session_new ();
        g_object_set (session, "timeout", (guint)WEB_FETCH_TIMEOUT_SECS, NULL);

        /* http:// -> https:// upgrade, with fall-back to the original.
         * Skipped for localhost / IP-literal hosts: those are dev or internal
         * endpoints that are rarely TLS, and a speculative https probe there
         * just stalls until the timeout before falling back. */
        if (g_str_has_prefix (url, "http://") && web_fetch_should_upgrade (url))
        {
            fetch_url = g_strconcat ("https://",
                                     url + strlen ("http://"), NULL);
            upgraded = TRUE;
        }
        else
        {
            fetch_url = g_strdup (url);
        }

        bytes = web_fetch_get (session, fetch_url, &msg, cancellable, &local_err);
        if (bytes == NULL && upgraded)
        {
            g_clear_error (&local_err);
            bytes = web_fetch_get (session, url, &msg, cancellable, &local_err);
        }
        if (bytes == NULL)
        {
            g_propagate_error (error, g_steal_pointer (&local_err));
            return NULL;
        }

        status_code = soup_message_get_status (msg);
        if (status_code < 200 || status_code >= 300)
        {
            g_set_error (error, AI_ERROR, AI_ERROR_SERVER_ERROR,
                         "web_fetch: HTTP %u for '%s'", status_code, url);
            return NULL;
        }

        data         = g_bytes_get_data (bytes, &size);
        resp_headers = soup_message_get_response_headers (msg);
        content_type = soup_message_headers_get_content_type (resp_headers,
                                                              &ct_params);
        charset      = (ct_params != NULL)
                       ? g_hash_table_lookup (ct_params, "charset") : NULL;

        req_uri    = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
        req_host   = (req_uri != NULL) ? g_uri_get_host (req_uri) : NULL;
        final_uri  = soup_message_get_uri (msg);
        final_host = (final_uri != NULL) ? g_uri_get_host (final_uri) : NULL;
        final_str  = (final_uri != NULL) ? g_uri_to_string (final_uri) : NULL;

        converted = web_fetch_build_body (data, size, content_type, charset,
                                          req_host, final_host, final_str);
        g_free (final_str);
        g_clear_pointer (&ct_params, g_hash_table_unref);

        web_fetch_cache_store (url, converted);
    }

    /* 3. Optional prompt-based extraction via the active run's provider. */
    if (prompt != NULL && prompt[0] != '\0'
        && self != NULL && self->active_provider != NULL)
    {
        gchar *extracted = web_fetch_extract (self->active_provider, url,
                                              converted, prompt, cancellable);
        if (extracted != NULL)
            return extracted;
        /* On failure, fall through and return the converted content. */
    }

    return g_steal_pointer (&converted);
}

/* UTF-8-safe truncation of TEXT to at most MAX bytes, with an ellipsis when
 * cut. Used to keep fetch_content excerpts small in the model's context. */
static gchar *
web_search_excerpt (const gchar *text, gsize max)
{
    gsize len;
    gsize cut;

    if (text == NULL)
        return NULL;

    len = strlen (text);
    if (len <= max)
        return g_strdup (text);

    cut = max;
    /* Back off any UTF-8 continuation bytes so we cut on a char boundary. */
    while (cut > 0 && (((guchar) text[cut]) & 0xC0) == 0x80)
        cut--;

    return g_strdup_printf ("%.*s\xe2\x80\xa6", (int) cut, text);
}

/* Fetch URL and return a short readable excerpt of its page text, reusing the
 * web_fetch pipeline + cache. Returns NULL on any failure (best-effort). */
static gchar *
web_search_fetch_excerpt (const gchar *url, GCancellable *cancellable)
{
    g_autofree gchar *converted = NULL;

    converted = web_fetch_cache_lookup (url);

    if (converted == NULL)
    {
        g_autoptr(SoupSession)  session   = NULL;
        g_autoptr(SoupMessage)  msg       = NULL;
        g_autoptr(GBytes)       bytes     = NULL;
        g_autoptr(GError)       local_err = NULL;
        SoupMessageHeaders     *resp_headers;
        const gchar            *data;
        gsize                   size;
        const gchar            *content_type;
        GHashTable             *ct_params = NULL;
        const gchar            *charset;
        guint                   status;

        session = soup_session_new ();
        g_object_set (session, "timeout", (guint) WEB_SEARCH_FETCH_TIMEOUT, NULL);

        bytes = web_fetch_get (session, url, &msg, cancellable, &local_err);
        if (bytes == NULL)
            return NULL;

        status = soup_message_get_status (msg);
        if (status < 200 || status >= 300)
            return NULL;

        data         = g_bytes_get_data (bytes, &size);
        resp_headers = soup_message_get_response_headers (msg);
        content_type = soup_message_headers_get_content_type (resp_headers,
                                                              &ct_params);
        charset      = (ct_params != NULL)
                       ? g_hash_table_lookup (ct_params, "charset") : NULL;

        converted = web_fetch_build_body (data, size, content_type, charset,
                                          NULL, NULL, NULL);
        g_clear_pointer (&ct_params, g_hash_table_unref);

        if (converted != NULL)
            web_fetch_cache_store (url, converted);
    }

    if (converted == NULL)
        return NULL;

    return web_search_excerpt (converted, WEB_SEARCH_EXCERPT_MAX);
}

/* Build an AiSearchOptions from the web_search tool inputs. */
static AiSearchOptions *
web_search_build_options (AiToolUse *tool_use)
{
    AiSearchOptions *options = ai_search_options_new ();
    const gchar     *s;
    gint64           count;

    count = ai_tool_use_get_input_int (tool_use, "count", -1);
    if (count > 0)
        ai_search_options_set_count (options, (guint) count);

    s = ai_tool_use_get_input_string (tool_use, "freshness");
    if (s != NULL)
        ai_search_options_set_freshness (options,
                                         ai_search_freshness_from_string (s));

    s = ai_tool_use_get_input_string (tool_use, "safesearch");
    if (s != NULL)
        ai_search_options_set_safesearch (options,
                                          ai_search_safe_search_from_string (s));

    s = ai_tool_use_get_input_string (tool_use, "country");
    if (s != NULL)
        ai_search_options_set_country (options, s);

    s = ai_tool_use_get_input_string (tool_use, "language");
    if (s != NULL)
        ai_search_options_set_language (options, s);

    s = ai_tool_use_get_input_string (tool_use, "site");
    if (s != NULL)
        ai_search_options_set_site (options, s);

    ai_search_options_set_fetch_content (
        options, ai_tool_use_get_input_boolean (tool_use, "fetch_content",
                                                FALSE));

    return options;
}

/* Cache key: provider type + query + every option that affects the output. */
static gchar *
web_search_cache_key (
    AiSearchProvider *provider,
    const gchar      *query,
    AiSearchOptions  *options
){
    const gchar *country  = ai_search_options_get_country (options);
    const gchar *language = ai_search_options_get_language (options);
    const gchar *site     = ai_search_options_get_site (options);

    return g_strdup_printf (
        "%s|%s|c=%u|f=%d|s=%d|cc=%s|lang=%s|site=%s|off=%u|fc=%d|fn=%u",
        G_OBJECT_TYPE_NAME (provider), query,
        ai_search_options_get_count (options),
        (int) ai_search_options_get_freshness (options),
        (int) ai_search_options_get_safesearch (options),
        country  != NULL ? country  : "",
        language != NULL ? language : "",
        site     != NULL ? site     : "",
        ai_search_options_get_offset (options),
        ai_search_options_get_fetch_content (options) ? 1 : 0,
        ai_search_options_get_fetch_count (options));
}

static gchar *
tool_web_search (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar              *query;
    g_autoptr(AiSearchOptions) options   = NULL;
    g_autofree gchar         *cache_key  = NULL;
    g_autofree gchar         *cached     = NULL;
    g_autofree gchar         *formatted  = NULL;
    g_autoptr(GError)         local_err  = NULL;
    GList                    *results;
    gboolean                  fetch_content;

    if (self->search_provider == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "web_search: no search provider configured; "
                             "call ai_tool_executor_set_search_provider() first");
        return NULL;
    }

    query = ai_tool_use_get_input_string (tool_use, "query");
    if (query == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "web_search: missing required parameter 'query'");
        return NULL;
    }

    options       = web_search_build_options (tool_use);
    fetch_content = ai_search_options_get_fetch_content (options);

    /* Cache hit: identical query+options within the TTL skips the network. */
    cache_key = web_search_cache_key (self->search_provider, query, options);
    cached    = web_search_cache_lookup (cache_key);
    if (cached != NULL)
        return g_steal_pointer (&cached);

    results = ai_search_provider_search (self->search_provider, query, options,
                                         cancellable, &local_err);
    if (local_err != NULL)
    {
        g_propagate_error (error, g_steal_pointer (&local_err));
        return NULL;
    }

    /* Optional enrichment: fetch the top results' pages and attach excerpts. */
    if (fetch_content && results != NULL)
    {
        guint  budget = ai_search_options_get_fetch_count (options);
        guint  done   = 0;
        GList *l;

        for (l = results; l != NULL && done < budget; l = l->next)
        {
            AiSearchResult *r   = l->data;
            const gchar    *url = ai_search_result_get_url (r);
            gchar          *excerpt;

            if (url == NULL || *url == '\0')
                continue;

            excerpt = web_search_fetch_excerpt (url, cancellable);
            if (excerpt != NULL)
            {
                ai_search_result_set_content (r, excerpt);
                g_free (excerpt);
                done++;
            }
        }
    }

    formatted = ai_search_results_format (results, query, fetch_content);
    g_list_free_full (results, g_object_unref);

    if (formatted != NULL)
        web_search_cache_store (cache_key, formatted);

    return g_steal_pointer (&formatted);
}

/* ================================================================
 * Helpers shared by the harness-aware tools
 * ================================================================ */

/* The names of everything of one kind, for an error a model can act on. */
static gchar *
executor_list_resource_names (
    AiToolExecutor *self,
    AiResourceKind  kind
){
    g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
    GList               *resources;
    GList               *iter;

    if (self->registry == NULL)
        return NULL;

    resources = ai_resource_registry_list (self->registry, kind);

    for (iter = resources; iter != NULL; iter = iter->next)
    {
        const gchar *name = ai_resource_get_name (iter->data);

        if (name != NULL)
            g_ptr_array_add (names, g_strdup (name));
    }

    g_list_free (resources);

    if (names->len == 0)
        return NULL;

    g_ptr_array_add (names, NULL);

    return g_strjoinv (", ", (gchar **)names->pdata);
}

/* The ids the brigade knows, for the same reason. */
static gchar *
executor_list_agent_ids (AiToolExecutor *self)
{
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func (g_free);
    g_autoptr(GList)     agents = NULL;
    GList               *iter;

    if (self->brigade == NULL)
        return NULL;

    agents = ai_brigade_list (self->brigade);

    for (iter = agents; iter != NULL; iter = iter->next)
    {
        const gchar *id = ai_agent_get_id (iter->data);

        if (id != NULL)
            g_ptr_array_add (ids, g_strdup (id));
    }

    if (ids->len == 0)
        return NULL;

    g_ptr_array_add (ids, NULL);

    return g_strjoinv (", ", (gchar **)ids->pdata);
}

/*
 * The first line of TEXT, at most MAX_CHARS of it.
 *
 * For status listings, where one agent gets one line. Truncation is
 * marked, because a description silently cut mid-word reads as a
 * description that was written that way.
 *
 * MAX_CHARS counts characters, not bytes: cutting a UTF-8 sequence in
 * half would produce a string nothing downstream can render.
 */
static gchar *
executor_first_line (
    const gchar *text,
    glong        max_chars
){
    const gchar *newline;
    g_autofree gchar *line = NULL;

    if (text == NULL)
        return NULL;

    newline = strchr (text, '\n');
    line = newline != NULL ? g_strndup (text, (gsize)(newline - text))
                           : g_strdup (text);
    g_strstrip (line);

    if (!g_utf8_validate (line, -1, NULL))
        return g_strdup ("(not valid UTF-8)");

    if (g_utf8_strlen (line, -1) <= max_chars)
        return g_steal_pointer (&line);

    {
        const gchar      *end = g_utf8_offset_to_pointer (line, max_chars);
        g_autofree gchar *cut = g_strndup (line, (gsize)(end - line));

        return g_strconcat (cut, "\342\200\246", NULL);   /* … */
    }
}

/* ================================================================
 * todo_write
 * ================================================================ */

/*
 * Replace the whole list, never patch it.
 *
 * The model resends every item on every call, which is what keeps this
 * honest: there is no way for the executor's idea of the list and the
 * model's to drift apart, and no partial-update protocol to get wrong.
 */
static gchar *
tool_todo_write (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    JsonNode   *input;
    JsonObject *object;
    JsonArray  *array;
    guint       n;
    guint       i;
    guint       in_progress = 0;
    GString    *summary;

    (void)cancellable;

    input = ai_tool_use_get_input (tool_use);

    if (input == NULL || !JSON_NODE_HOLDS_OBJECT (input))
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "todo_write: expected an object of parameters");
        return NULL;
    }

    object = json_node_get_object (input);

    if (!json_object_has_member (object, "todos"))
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "todo_write: missing required parameter 'todos'");
        return NULL;
    }

    {
        JsonNode *todos_node = json_object_get_member (object, "todos");

        if (todos_node == NULL || !JSON_NODE_HOLDS_ARRAY (todos_node))
        {
            g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                 "todo_write: 'todos' must be an array");
            return NULL;
        }

        array = json_node_get_array (todos_node);
    }

    g_ptr_array_set_size (self->todos, 0);

    n = json_array_get_length (array);

    for (i = 0; i < n; i++)
    {
        JsonNode    *element = json_array_get_element (array, i);
        JsonObject  *item;
        const gchar *content;
        const gchar *active_form = NULL;
        const gchar *state_name = NULL;
        AiTodoState  state;

        if (element == NULL || !JSON_NODE_HOLDS_OBJECT (element))
        {
            /* One malformed entry costs itself, not the list. */
            g_debug ("todo_write: entry %u is not an object; skipping", i);
            continue;
        }

        item = json_node_get_object (element);

        /* Read every member through a type check: this is model output,
         * and json-glib's convenience accessors emit a critical on a
         * mismatch --- fatal under G_DEBUG=fatal-warnings. */
        content = ai_json_get_string (item, "content", NULL);

        if (content == NULL)
        {
            g_debug ("todo_write: entry %u has no content; skipping", i);
            continue;
        }

        active_form = ai_json_get_string (item, "active_form", NULL);

        if (active_form == NULL)
        {
            active_form = ai_json_get_string (item, "activeForm", NULL);
        }

        state_name = ai_json_get_string (item, "status", NULL);

        if (state_name == NULL)
        {
            state_name = ai_json_get_string (item, "state", NULL);
        }

        state = ai_todo_state_from_string (state_name);

        if (state == AI_TODO_IN_PROGRESS)
        {
            in_progress++;
        }

        g_ptr_array_add (self->todos,
                         ai_todo_new (content, active_form, state));
    }

    if (in_progress > 1)
    {
        /*
         * g_message, not g_warning: a model doing two things at once is
         * a model being imprecise, not this program being broken, and it
         * must not abort a run under fatal warnings.
         */
        g_message ("todo_write: %u items marked in progress at once",
                   in_progress);
    }

    g_signal_emit (self, signals[SIGNAL_TODOS_CHANGED], 0);

    summary = g_string_new (NULL);
    g_string_append_printf (summary, "Todo list updated (%u item%s).\n",
                            self->todos->len,
                            self->todos->len == 1 ? "" : "s");

    for (i = 0; i < self->todos->len; i++)
    {
        const AiTodo *todo = g_ptr_array_index (self->todos, i);

        g_string_append_printf (summary, "  [%s] %s\n",
                                ai_todo_state_to_string (todo->state),
                                ai_todo_get_label (todo));
    }

    return g_string_free (summary, FALSE);
}

/* ================================================================
 * multi_edit
 * ================================================================ */

/*
 * Several edits to one file, all or nothing.
 *
 * Every replacement is applied to an in-memory copy and the file is
 * written once, at the end. A failure on the third of four edits
 * therefore leaves the file byte-for-byte as it was, rather than
 * half-converted in a way that is worse than either state.
 *
 * Deliberately stricter than `edit`: an old_string matching more than
 * once is refused rather than taking the first. In a batch the wrong
 * match is buried among the right ones, and the model cannot see what it
 * did.
 */
static gchar *
tool_multi_edit (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar      *path;
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr(GString) working = NULL;
    JsonNode         *input;
    JsonObject       *object;
    JsonArray        *array;
    gsize             length;
    guint             n;
    guint             i;

    (void)cancellable;

    path = ai_tool_use_get_input_string (tool_use, "path");
    input = ai_tool_use_get_input (tool_use);

    if (path == NULL || input == NULL || !JSON_NODE_HOLDS_OBJECT (input))
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "multi_edit: missing required parameter(s): "
                             "path, edits");
        return NULL;
    }

    object = json_node_get_object (input);

    {
        JsonNode *edits_node = json_object_has_member (object, "edits")
                                   ? json_object_get_member (object, "edits")
                                   : NULL;

        if (edits_node == NULL || !JSON_NODE_HOLDS_ARRAY (edits_node))
        {
            g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                 "multi_edit: 'edits' must be an array");
            return NULL;
        }

        array = json_node_get_array (edits_node);
    }

    n = json_array_get_length (array);

    if (n == 0)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                             "multi_edit: 'edits' is empty");
        return NULL;
    }

    resolved = executor_resolve_path (self, path);

    if (!g_file_get_contents (resolved, &contents, &length, error))
        return NULL;

    working = g_string_new_len (contents, (gssize)length);

    for (i = 0; i < n; i++)
    {
        JsonNode    *element = json_array_get_element (array, i);
        JsonObject  *edit;
        const gchar *old_string;
        const gchar *new_string;
        const gchar *found;
        gsize        prefix;

        if (element == NULL || !JSON_NODE_HOLDS_OBJECT (element))
        {
            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "multi_edit: edit %u is not an object", i + 1);
            return NULL;
        }

        edit = json_node_get_object (element);
        old_string = ai_json_get_string (edit, "old_string", NULL);
        new_string = ai_json_get_string (edit, "new_string", NULL);

        if (old_string == NULL || new_string == NULL)
        {
            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "multi_edit: edit %u needs old_string and new_string",
                         i + 1);
            return NULL;
        }

        if (old_string[0] == '\0')
        {
            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "multi_edit: edit %u has an empty old_string; use "
                         "write to create a file", i + 1);
            return NULL;
        }

        found = strstr (working->str, old_string);

        if (found == NULL)
        {
            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "multi_edit: edit %u: old_string not found in '%s'; "
                         "no edits were applied", i + 1, resolved);
            return NULL;
        }

        if (strstr (found + 1, old_string) != NULL)
        {
            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "multi_edit: edit %u: old_string matches more than "
                         "once in '%s'; make it unique. No edits were applied",
                         i + 1, resolved);
            return NULL;
        }

        prefix = (gsize)(found - working->str);
        g_string_erase (working, (gssize)prefix, (gssize)strlen (old_string));
        g_string_insert (working, (gssize)prefix, new_string);
    }

    if (!g_file_set_contents (resolved, working->str, (gssize)working->len,
                              error))
        return NULL;

    return g_strdup_printf ("Applied %u edit%s to %s", n,
                            n == 1 ? "" : "s", resolved);
}

/* ================================================================
 * skill
 * ================================================================ */

static gchar *
tool_skill (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *name;
    AiResource  *resource;

    (void)cancellable;

    name = ai_tool_use_get_input_string (tool_use, "name");

    if (name == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "skill: missing required parameter 'name'");
        return NULL;
    }

    if (self->registry == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "skill: no resource registry is configured");
        return NULL;
    }

    resource = ai_resource_registry_lookup (self->registry, AI_RESOURCE_SKILL,
                                            name);

    if (resource == NULL)
    {
        g_autofree gchar *available = executor_list_resource_names (
            self, AI_RESOURCE_SKILL);

        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "skill: no skill named '%s'. Available: %s", name,
                     available != NULL ? available : "(none)");
        return NULL;
    }

    return g_strdup (ai_resource_get_body (resource));
}

/* ================================================================
 * task
 * ================================================================ */

/*
 * Map a tool name as a harness spells it onto the one ai-glib uses.
 *
 * The agent files on disk were written for claude-code, which
 * capitalises: "Bash", "WebFetch", "MultiEdit". Matching them by
 * lowercasing alone would silently drop half an allowlist and leave the
 * agent unable to do its job, with nothing to say why.
 */
static gchar *
normalise_tool_name (const gchar *declared)
{
    static const struct
    {
        const gchar *harness;
        const gchar *ours;
    } aliases[] = {
        { "webfetch",    "web_fetch"  },
        { "websearch",   "web_search" },
        { "multiedit",   "multi_edit" },
        { "todowrite",   "todo_write" },
        { "notebookedit", "edit"      },
        { NULL, NULL }
    };
    g_autofree gchar *lower = NULL;
    gsize             i;

    if (declared == NULL)
        return NULL;

    lower = g_ascii_strdown (declared, -1);
    g_strstrip (lower);

    for (i = 0; aliases[i].harness != NULL; i++)
    {
        if (g_strcmp0 (lower, aliases[i].harness) == 0)
            return g_strdup (aliases[i].ours);
    }

    return g_steal_pointer (&lower);
}

/*
 * Build the child executor an agent runs inside.
 *
 * The allowlist is applied by *removing* tools, not by refusing calls to
 * them: an agent whose `tools:` omits bash has no bash to call, so
 * calling it is not denied, it is unrepresentable. That is the same
 * argument ai_agent_new() already makes about owning an executor apiece.
 *
 * An agent that declares no tools inherits everything, which is what
 * claude-code does and what the files on disk assume.
 */
static AiToolExecutor *
build_agent_executor (
    AiToolExecutor *self,
    AiResource     *agent
){
    AiToolExecutor *child = ai_tool_executor_new ();
    g_auto(GStrv)   declared = ai_resource_get_meta_list (agent, "tools");

    ai_tool_executor_set_working_directory (child, self->working_directory);
    ai_tool_executor_set_resource_registry (child, self->registry);
    ai_tool_executor_set_approval_policy (child, self->approval_policy);
    child->task_depth = self->task_depth + 1;

    if (self->search_provider != NULL)
        ai_tool_executor_set_search_provider (child, self->search_provider);

    if (declared == NULL || declared[0] == NULL)
        return child;

    {
        g_autoptr(GHashTable) allowed =
            g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        g_autoptr(GPtrArray)  doomed = g_ptr_array_new_with_free_func (g_free);
        GList                *iter;
        guint                 i;

        for (i = 0; declared[i] != NULL; i++)
        {
            gchar *mapped = normalise_tool_name (declared[i]);

            if (mapped != NULL)
                g_hash_table_add (allowed, mapped);
        }

        for (iter = ai_tool_executor_get_tools (child); iter != NULL;
             iter = iter->next)
        {
            const gchar *name = ai_tool_get_name (iter->data);

            if (name != NULL && !g_hash_table_contains (allowed, name))
                g_ptr_array_add (doomed, g_strdup (name));
        }

        for (i = 0; i < doomed->len; i++)
            ai_tool_executor_unregister (child,
                                         g_ptr_array_index (doomed, i));
    }

    return child;
}

/* Republish a subagent's events on the parent's stream, stamped with
 * which agent produced them. */
static void
on_subagent_event (
    GObject  *source,
    AiEvent  *event,
    gpointer  user_data
){
    AiToolExecutor *parent = user_data;

    (void)source;

    if (event == NULL)
        return;

    ai_event_source_emit (AI_EVENT_SOURCE (parent), event);
}

static gchar *
tool_task (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar               *agent_name;
    const gchar               *prompt;
    const gchar               *provider_name;
    const gchar               *model;
    AiResource                *agent;
    g_autoptr(AiProvider)      provider = NULL;
    g_autoptr(AiToolExecutor)  child = NULL;
    g_autoptr(AiMessage)       message = NULL;
    GList                     *messages = NULL;
    gchar                     *result;
    gulong                     event_id;

    agent_name = ai_tool_use_get_input_string (tool_use, "agent");
    prompt = ai_tool_use_get_input_string (tool_use, "prompt");
    provider_name = ai_tool_use_get_input_string (tool_use, "provider");
    model = ai_tool_use_get_input_string (tool_use, "model");

    if (agent_name == NULL || prompt == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "task: missing required parameter(s): "
                             "agent, prompt");
        return NULL;
    }

    if (self->registry == NULL)
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                             "task: no resource registry is configured");
        return NULL;
    }

    /*
     * A subagent that spawns a subagent that spawns a subagent is a
     * runaway, not a plan. Refusing is reported to the model rather than
     * silently truncated, so it can do the work itself instead of
     * wondering why nothing happened.
     */
    if (self->task_depth >= AI_TOOL_EXECUTOR_MAX_TASK_DEPTH)
    {
        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "task: already %u agents deep (limit %d); do this work "
                     "yourself rather than delegating further",
                     self->task_depth, AI_TOOL_EXECUTOR_MAX_TASK_DEPTH);
        return NULL;
    }

    agent = ai_resource_registry_lookup (self->registry, AI_RESOURCE_AGENT,
                                         agent_name);

    if (agent == NULL)
    {
        g_autofree gchar *available =
            executor_list_resource_names (self, AI_RESOURCE_AGENT);

        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "task: no agent named '%s'. Available: %s", agent_name,
                     available != NULL ? available : "(none)");
        return NULL;
    }

    /*
     * The environment is checked last, after everything the model could
     * have got wrong. "No agent named X" is a mistake it can correct;
     * "no provider" is one it can do nothing about, and reporting that
     * first would bury the useful message.
     */
    provider = executor_resolve_agent_provider (self, provider_name, model,
                                                error);

    if (provider == NULL)
        return NULL;

    child = build_agent_executor (self, agent);

    /* One stream: a frontend watching the parent sees the child's tool
     * calls too, tagged with whose they are. */
    event_id = g_signal_connect (child, "event",
                                 G_CALLBACK (on_subagent_event), self);

    message = ai_message_new_user (prompt);
    messages = g_list_append (NULL, message);

    result = ai_tool_executor_run (child, provider, messages,
                                   ai_resource_get_body (agent),
                                   AI_TOOL_EXECUTOR_AGENT_MAX_TOKENS,
                                   cancellable, error);

    g_signal_handler_disconnect (child, event_id);
    g_list_free (messages);

    if (result == NULL)
        return NULL;

    return result;
}

/* ================================================================
 * Background agents
 * ================================================================ */

/*
 * The provider one background agent will run on.
 *
 * Named provider wins; otherwise the agent inherits whatever this
 * conversation is using. That inheritance is a borrowed reference to a
 * client somebody else owns, so it is reffed like any other -- and a
 * named one is built fresh, because two agents sharing one HTTP client
 * would share its per-request state.
 *
 * A named provider is constructed from the environment through the same
 * factory `ai` and `ai-tui` use, which is what makes "run this on
 * claude-code while I am talking to Grok" a matter of naming it.
 */
static AiProvider *
executor_resolve_agent_provider (
    AiToolExecutor  *self,
    const gchar     *provider_name,
    const gchar     *model,
    GError         **error
){
    g_autoptr(GObject) built = NULL;

    if (provider_name == NULL || provider_name[0] == '\0')
    {
        if (self->active_provider == NULL)
        {
            g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                                 "no provider is available to run an agent");
            return NULL;
        }

        if (model != NULL)
        {
            g_set_error_literal (
                error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                "naming a model needs a provider too: say which provider "
                "that model belongs to");
            return NULL;
        }

        return g_object_ref (self->active_provider);
    }

    built = ai_provider_factory_new_from_string (provider_name, NULL, error);

    if (built == NULL)
        return NULL;

    if (!AI_IS_PROVIDER (built))
    {
        g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                     "provider '%s' cannot hold a conversation", provider_name);
        return NULL;
    }

    /*
     * The two client base classes share no ancestor beyond GObject, so
     * setting the model means asking which one this is -- the same test
     * ai_provider_factory_new() documents.
     */
    if (model != NULL && model[0] != '\0')
    {
        if (AI_IS_CLIENT (built))
            ai_client_set_model (AI_CLIENT (built), model);
        else if (AI_IS_CLI_CLIENT (built))
            ai_cli_client_set_model (AI_CLI_CLIENT (built), model);
        else
            g_set_error (error, AI_ERROR, AI_ERROR_NOT_SUPPORTED,
                         "provider '%s' has no model to set", provider_name);
    }

    if (error != NULL && *error != NULL)
        return NULL;

    return AI_PROVIDER (g_steal_pointer (&built));
}

static gchar *
tool_agent_spawn (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar             *prompt;
    const gchar             *description;
    const gchar             *agent_name;
    const gchar             *provider_name;
    const gchar             *model;
    AiResource              *definition = NULL;
    g_autoptr(AiProvider)    provider   = NULL;
    g_autoptr(AiAgent)       agent      = NULL;
    g_autoptr(AiToolExecutor) child     = NULL;
    g_autofree gchar        *id         = NULL;

    (void)cancellable;

    prompt        = ai_tool_use_get_input_string (tool_use, "prompt");
    description   = ai_tool_use_get_input_string (tool_use, "description");
    agent_name    = ai_tool_use_get_input_string (tool_use, "agent");
    provider_name = ai_tool_use_get_input_string (tool_use, "provider");
    model         = ai_tool_use_get_input_string (tool_use, "model");

    if (prompt == NULL || prompt[0] == '\0')
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                             "agent_spawn: missing required parameter: prompt");
        return NULL;
    }

    /*
     * Everything the model could have got wrong is checked before
     * anything about the environment, so "no agent named 'reviewr'"
     * reaches it instead of being buried under a configuration problem
     * it can do nothing about. Same ordering as `task`.
     */
    if (agent_name != NULL && agent_name[0] != '\0')
    {
        if (self->registry == NULL)
        {
            g_set_error_literal (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                                 "agent_spawn: no agent definitions are "
                                 "available; omit 'agent' for a "
                                 "general-purpose one");
            return NULL;
        }

        definition = ai_resource_registry_lookup (self->registry,
                                                  AI_RESOURCE_AGENT,
                                                  agent_name);

        if (definition == NULL)
        {
            g_autofree gchar *available =
                executor_list_resource_names (self, AI_RESOURCE_AGENT);

            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "agent_spawn: no agent named '%s'. Available: %s",
                         agent_name,
                         available != NULL ? available : "(none)");
            return NULL;
        }
    }

    provider = executor_resolve_agent_provider (self, provider_name, model,
                                                error);

    if (provider == NULL)
        return NULL;

    id = ai_brigade_generate_id (self->brigade,
                                 agent_name != NULL ? agent_name : "agent");

    agent = ai_agent_new (id, provider);
    ai_agent_set_description (agent, description != NULL ? description
                                                         : prompt);
    ai_agent_set_max_tokens (agent, AI_TOOL_EXECUTOR_AGENT_MAX_TOKENS);

    /*
     * A background agent gets the same executor a foreground `task` would
     * -- the agent file's tool allowlist applied structurally -- with one
     * subtraction: it may not start further background agents. One level
     * of fan-out is delegation; agents spawning agents unattended is a
     * fork bomb that bills.
     */
    if (definition != NULL)
    {
        child = build_agent_executor (self, definition);
        ai_agent_set_system_prompt (agent, ai_resource_get_body (definition));
    }
    else
    {
        child = ai_tool_executor_new ();
        ai_tool_executor_set_working_directory (child,
                                                self->working_directory);
        ai_tool_executor_set_resource_registry (child, self->registry);
        ai_tool_executor_set_approval_policy (child, self->approval_policy);

        if (self->search_provider != NULL)
            ai_tool_executor_set_search_provider (child,
                                                  self->search_provider);
    }

    ai_tool_executor_set_features (
        child, ai_tool_executor_get_features (child) &
                   ~(AiToolFeatures) AI_TOOL_FEATURE_BACKGROUND);
    ai_agent_set_executor (agent, child);

    if (!ai_brigade_start (self->brigade, agent, prompt, error))
        return NULL;

    return g_strdup_printf (
        "Started agent '%s' (%s). It is running in the background; you will "
        "be told when it finishes. Use agent_status to check on it, "
        "agent_result to collect its answer.",
        id, ai_agent_state_to_string (ai_agent_get_state (agent)));
}

/* One agent's line in a status listing. */
static void
executor_append_agent_status (
    AiToolExecutor *self,
    GString        *out,
    AiAgent        *agent
){
    AiBudget         *budget  = ai_agent_get_budget (agent);
    gint64            ms      = ai_agent_get_elapsed_ms (agent);
    const gchar      *desc    = ai_agent_get_description (agent);
    g_autofree gchar *peek    = NULL;

    (void)self;

    g_string_append_printf (out, "%s  %s  %" G_GINT64_FORMAT "s",
                            ai_agent_get_id (agent),
                            ai_agent_state_to_string (ai_agent_get_state (agent)),
                            ms / 1000);

    if (budget != NULL && ai_budget_get_turns (budget) > 0)
        g_string_append_printf (out, "  %u turns, %" G_GUINT64_FORMAT " in / "
                                "%" G_GUINT64_FORMAT " out",
                                ai_budget_get_turns (budget),
                                ai_budget_get_input_tokens (budget),
                                ai_budget_get_output_tokens (budget));

    if (desc != NULL && desc[0] != '\0')
    {
        g_autofree gchar *one_line = executor_first_line (desc, 60);

        g_string_append_printf (out, "  %s", one_line);
    }

    g_string_append_c (out, '\n');

    /*
     * A peek at the work so far, not the whole thing. The point of
     * asking for status is to decide whether to keep waiting, and a
     * running agent's half-written answer pasted in full would crowd out
     * every other agent on the list.
     */
    if (self->brigade != NULL && ai_brigade_get_worker (self->brigade) != NULL)
    {
        peek = ai_agent_worker_read_output (
            ai_brigade_get_worker (self->brigade), agent, NULL);
    }

    if (peek != NULL && peek[0] != '\0')
    {
        g_autofree gchar *snippet = executor_first_line (peek, 100);

        g_string_append_printf (out, "    so far: %s\n", snippet);
    }

    if (ai_agent_get_error (agent) != NULL)
        g_string_append_printf (out, "    error: %s\n",
                                ai_agent_get_error (agent)->message);
}

static gchar *
tool_agent_status (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar        *wanted;
    g_autoptr(GString)  out = g_string_new (NULL);

    (void)cancellable;

    wanted = ai_tool_use_get_input_string (tool_use, "agent_id");

    if (wanted != NULL && wanted[0] != '\0')
    {
        AiAgent *agent = ai_brigade_get (self->brigade, wanted);

        if (agent == NULL)
        {
            g_autofree gchar *known = executor_list_agent_ids (self);

            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "agent_status: no agent named '%s'. Known: %s",
                         wanted, known != NULL ? known : "(none)");
            return NULL;
        }

        executor_append_agent_status (self, out, agent);
        return g_string_free (g_steal_pointer (&out), FALSE);
    }

    {
        g_autoptr(GList) agents = ai_brigade_list (self->brigade);
        GList           *iter;

        if (agents == NULL)
            return g_strdup ("No background agents.");

        for (iter = agents; iter != NULL; iter = iter->next)
            executor_append_agent_status (self, out, iter->data);
    }

    return g_string_free (g_steal_pointer (&out), FALSE);
}

static gchar *
tool_agent_result (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *wanted;

    (void)cancellable;

    wanted = ai_tool_use_get_input_string (tool_use, "agent_id");

    if (wanted == NULL || wanted[0] == '\0')
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                             "agent_result: missing required parameter: "
                             "agent_id");
        return NULL;
    }

    if (ai_brigade_get (self->brigade, wanted) == NULL)
    {
        g_autofree gchar *known = executor_list_agent_ids (self);

        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "agent_result: no agent named '%s'. Known: %s", wanted,
                     known != NULL ? known : "(none)");
        return NULL;
    }

    /* Reaps on success: collecting the answer is what finishes an agent's
     * life, and leaving the record behind would make every later status
     * listing longer for no reason. */
    return ai_brigade_reap (self->brigade, wanted, error);
}

/* What a waiting tool call is waiting for. */
typedef struct
{
    AiBrigade  *brigade;
    gchar      *wanted;      /* NULL means "whichever finishes first" */
    gchar      *finished;    /* the id that satisfied the wait */
    GMainLoop  *loop;
    guint       timeout_id;
    gboolean    timed_out;
} AgentWait;

static void
on_wait_agent_finished (
    AiBrigade   *brigade,
    const gchar *agent_id,
    gint         state,
    gpointer     user_data
){
    AgentWait *wait = user_data;

    (void)brigade;
    (void)state;

    if (wait->wanted != NULL && g_strcmp0 (wait->wanted, agent_id) != 0)
        return;

    wait->finished = g_strdup (agent_id);
    g_main_loop_quit (wait->loop);
}

static gboolean
on_wait_timeout (gpointer user_data)
{
    AgentWait *wait = user_data;

    wait->timed_out = TRUE;
    wait->timeout_id = 0;
    g_main_loop_quit (wait->loop);

    return G_SOURCE_REMOVE;
}

static void
on_wait_cancelled (GCancellable *cancellable, gpointer user_data)
{
    AgentWait *wait = user_data;

    (void)cancellable;

    g_main_loop_quit (wait->loop);
}

static gchar *
tool_agent_wait (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar      *wanted;
    gint64            seconds;
    AgentWait         wait = { 0 };
    gulong            finished_id;
    gulong            cancel_id = 0;
    g_autoptr(GMainContext) context = NULL;

    wanted  = ai_tool_use_get_input_string (tool_use, "agent_id");
    seconds = ai_tool_use_get_input_int (tool_use, "timeout_seconds", 0);

    if (seconds <= 0)
        seconds = AI_TOOL_EXECUTOR_AGENT_WAIT_MAX_SECONDS;
    if (seconds > AI_TOOL_EXECUTOR_AGENT_WAIT_MAX_SECONDS)
        seconds = AI_TOOL_EXECUTOR_AGENT_WAIT_MAX_SECONDS;

    if (wanted != NULL && wanted[0] != '\0')
    {
        AiAgent *agent = ai_brigade_get (self->brigade, wanted);

        if (agent == NULL)
        {
            g_autofree gchar *known = executor_list_agent_ids (self);

            g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                         "agent_wait: no agent named '%s'. Known: %s", wanted,
                         known != NULL ? known : "(none)");
            return NULL;
        }

        /* Already done: answer without spinning a loop at all. */
        if (!ai_agent_state_is_live (ai_agent_get_state (agent)))
            return ai_brigade_reap (self->brigade, wanted, error);
    }
    else
    {
        g_autoptr(GList) agents = ai_brigade_list (self->brigade);
        GList           *iter;

        wanted = NULL;

        /*
         * "Whichever finishes first" includes one that already has.
         *
         * Without this the wait is for a *future* ::agent-finished, and
         * an agent that completed while the model was composing its
         * request is one the signal has already been emitted for --- so
         * the wait would run to its timeout with the answer sitting
         * right there.
         */
        for (iter = agents; iter != NULL; iter = iter->next)
        {
            AiAgent *candidate = iter->data;

            if (!ai_agent_state_is_live (ai_agent_get_state (candidate)))
            {
                g_autofree gchar *id =
                    g_strdup (ai_agent_get_id (candidate));
                g_autofree gchar *text =
                    ai_brigade_reap (self->brigade, id, error);

                if (text == NULL)
                    return NULL;

                return g_strdup_printf ("Agent '%s' finished.\n\n%s", id,
                                        text);
            }
        }

        /* Nothing running and nothing to collect: say so rather than
         * waiting ten minutes for an agent that does not exist. */
        if (agents == NULL)
        {
            g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                                 "agent_wait: there are no background agents");
            return NULL;
        }
    }

    /*
     * The nested loop runs on the *thread-default* context, not the
     * global default. A synchronous caller may already be driving a
     * private context -- ai_tool_executor_run() does exactly that -- and
     * a loop attached to the global default would never dispatch the
     * sources this is waiting on.
     */
    context = g_main_context_ref_thread_default ();

    wait.brigade = self->brigade;
    wait.wanted  = g_strdup (wanted);
    wait.loop    = g_main_loop_new (context, FALSE);

    finished_id = g_signal_connect (self->brigade, "agent-finished",
                                    G_CALLBACK (on_wait_agent_finished),
                                    &wait);

    {
        GSource *source = g_timeout_source_new_seconds ((guint) seconds);

        g_source_set_callback (source, on_wait_timeout, &wait, NULL);
        wait.timeout_id = g_source_attach (source, context);
        g_source_unref (source);
    }

    if (cancellable != NULL)
        cancel_id = g_cancellable_connect (cancellable,
                                           G_CALLBACK (on_wait_cancelled),
                                           &wait, NULL);

    g_main_loop_run (wait.loop);

    if (cancel_id != 0)
        g_cancellable_disconnect (cancellable, cancel_id);

    g_signal_handler_disconnect (self->brigade, finished_id);

    if (wait.timeout_id != 0)
    {
        GSource *source = g_main_context_find_source_by_id (context,
                                                            wait.timeout_id);

        if (source != NULL)
            g_source_destroy (source);
    }

    g_main_loop_unref (wait.loop);
    g_free (wait.wanted);

    if (g_cancellable_set_error_if_cancelled (cancellable, error))
    {
        g_free (wait.finished);
        return NULL;
    }

    if (wait.finished != NULL)
    {
        g_autofree gchar *id = g_steal_pointer (&wait.finished);
        g_autofree gchar *text = ai_brigade_reap (self->brigade, id, error);

        if (text == NULL)
            return NULL;

        return g_strdup_printf ("Agent '%s' finished.\n\n%s", id, text);
    }

    /* A timeout is not a failure: the agent is still working, and saying
     * so is more useful than an error the model has to interpret. */
    return g_strdup_printf (
        "Still running after %" G_GINT64_FORMAT "s. The agent has not been "
        "stopped; check agent_status or wait again.", seconds);
}

static gchar *
tool_agent_cancel (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar *wanted;
    AiAgent     *agent;

    (void)cancellable;

    wanted = ai_tool_use_get_input_string (tool_use, "agent_id");

    if (wanted == NULL || wanted[0] == '\0')
    {
        g_set_error_literal (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                             "agent_cancel: missing required parameter: "
                             "agent_id");
        return NULL;
    }

    if (g_strcmp0 (wanted, "all") == 0)
    {
        guint n = ai_brigade_cancel_all (self->brigade);

        return g_strdup_printf ("Stopped %u agent%s.", n, n == 1 ? "" : "s");
    }

    agent = ai_brigade_get (self->brigade, wanted);

    if (agent == NULL)
    {
        g_autofree gchar *known = executor_list_agent_ids (self);

        g_set_error (error, AI_ERROR, AI_ERROR_INVALID_REQUEST,
                     "agent_cancel: no agent named '%s'. Known: %s", wanted,
                     known != NULL ? known : "(none)");
        return NULL;
    }

    if (!ai_agent_state_is_live (ai_agent_get_state (agent)))
        return g_strdup_printf ("Agent '%s' had already %s.", wanted,
                                ai_agent_state_to_string (
                                    ai_agent_get_state (agent)));

    ai_agent_cancel (agent);

    return g_strdup_printf ("Stopped agent '%s'. Its output so far is still "
                            "available through agent_result.", wanted);
}

/* ================================================================
 * Tool dispatch table
 * ================================================================ */

typedef gchar * (*ToolFn) (AiToolExecutor *, AiToolUse *,
                            GCancellable *, GError **);

typedef struct
{
    const gchar *name;
    ToolFn       fn;
} ToolEntry;

static const ToolEntry BUILTIN_TOOLS[] = {
    { "bash",       tool_bash       },
    { "read",       tool_read       },
    { "write",      tool_write      },
    { "edit",       tool_edit       },
    { "glob",       tool_glob       },
    { "grep",       tool_grep       },
    { "ls",         tool_ls         },
    { "web_fetch",  tool_web_fetch  },
    { "web_search", tool_web_search },
    { "todo_write", tool_todo_write },
    { "multi_edit", tool_multi_edit },
    { "task",       tool_task       },
    { "skill",      tool_skill      },
    { "agent_spawn",  tool_agent_spawn  },
    { "agent_status", tool_agent_status },
    { "agent_result", tool_agent_result },
    { "agent_wait",   tool_agent_wait   },
    { "agent_cancel", tool_agent_cancel },
    { NULL, NULL }
};

/* ================================================================
 * GObject plumbing
 * ================================================================ */

static void
ai_tool_executor_finalize (GObject *object)
{
    AiToolExecutor *self = AI_TOOL_EXECUTOR (object);

    g_list_free_full (self->tools, g_object_unref);
    self->tools = NULL;
    g_clear_object (&self->search_provider);
    g_clear_pointer (&self->callbacks, g_hash_table_unref);
    g_clear_pointer (&self->working_directory, g_free);
    g_clear_pointer (&self->always_allowed, g_hash_table_unref);
    g_clear_pointer (&self->todos, g_ptr_array_unref);
    g_clear_object (&self->registry);
    g_clear_object (&self->brigade);
    executor_unwatch_provider (self);

    G_OBJECT_CLASS (ai_tool_executor_parent_class)->finalize (object);
}

static void
ai_tool_executor_class_init (AiToolExecutorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = ai_tool_executor_finalize;
    object_class->get_property = ai_tool_executor_get_property;
    object_class->set_property = ai_tool_executor_set_property;

    /**
     * AiToolExecutor:approval-policy:
     *
     * What an %AI_TOOL_APPROVAL_DEFAULT answer resolves to.
     *
     * Defaults to %AI_TOOL_APPROVAL_ALLOW, which with no
     * #AiToolExecutor::approval-requested handler connected is the
     * behaviour this class has always had. Set it to
     * %AI_TOOL_APPROVAL_DENY to make an unanswered call a refusal --- the
     * safe default for an unattended agent, where nobody is there to say
     * yes.
     */
    properties[PROP_APPROVAL_POLICY] =
        g_param_spec_int ("approval-policy",
                          "Approval Policy",
                          "What a DEFAULT approval answer resolves to",
                          AI_TOOL_APPROVAL_DEFAULT, AI_TOOL_APPROVAL_DENY_ALL,
                          AI_TOOL_APPROVAL_ALLOW,
                          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiToolExecutor:stream:
     *
     * Whether to stream each turn when the provider supports it.
     *
     * The loop has always used ai_provider_chat_async(), so a caller could
     * have live tokens or tool execution and never both. With this set and
     * an #AiStreamable provider, the turn goes through
     * ai_streamable_chat_stream_async() instead and the provider's events
     * are republished on this executor's own stream.
     *
     * Defaults to %FALSE so existing callers take the path they always did.
     */
    properties[PROP_STREAM] =
        g_param_spec_boolean ("stream",
                              "Stream",
                              "Stream each turn when the provider supports it",
                              FALSE,
                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiToolExecutor:working-directory:
     *
     * Directory the built-in tools resolve relative paths against.
     */
    properties[PROP_WORKING_DIRECTORY] =
        g_param_spec_string ("working-directory",
                             "Working Directory",
                             "Directory the built-in tools run in",
                             NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiToolExecutor:resource-registry:
     *
     * Where the `task` and `skill` tools find what they run.
     *
     * %NULL by default, and while it is %NULL neither tool is offered at
     * all --- an executor built by ai_tool_executor_new() advertises
     * exactly the tools it always has. Setting a registry is what adds
     * them, so nothing changes for a caller who does not want subagents.
     */
    properties[PROP_RESOURCE_REGISTRY] =
        g_param_spec_object ("resource-registry", NULL, NULL,
                             AI_TYPE_RESOURCE_REGISTRY,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiToolExecutor:brigade:
     *
     * Where background agents run, and the record of the ones that have.
     *
     * %NULL by default, and while it is %NULL the `agent_*` tools are not
     * offered --- the same arrangement as #AiToolExecutor:resource-registry
     * and `task`. Handing over an #AiBrigade is how an application says
     * the model may start work that outlives the turn it was asked in.
     *
     * The brigade needs a worker to be able to run anything; see
     * ai_local_worker_new().
     */
    properties[PROP_BRIGADE] =
        g_param_spec_object ("brigade", NULL, NULL,
                             AI_TYPE_BRIGADE,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /**
     * AiToolExecutor:features:
     *
     * Which optional tool groups this executor is willing to offer, as
     * #AiToolFeatures.
     *
     * Defaults to %AI_TOOL_FEATURE_ALL. Clearing a bit removes that
     * group's tools immediately, and setting it back adds them again if
     * the thing they run on is present.
     */
    properties[PROP_FEATURES] =
        g_param_spec_uint ("features",
                           "Features",
                           "Optional tool groups this executor may offer",
                           AI_TOOL_FEATURE_NONE, AI_TOOL_FEATURE_ALL,
                           AI_TOOL_FEATURE_ALL,
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, properties);

    /**
     * AiToolExecutor::approval-requested:
     * @self: the #AiToolExecutor
     * @tool_use: the call the model wants to make
     *
     * Emitted before each tool call, so a host can ask a human.
     *
     * Return an #AiToolApproval. %AI_TOOL_APPROVAL_DEFAULT means "no
     * opinion" and lets the next handler decide, falling through to
     * #AiToolExecutor:approval-policy when none does. With no handler
     * connected the emission accumulates to DEFAULT, so an unwatched
     * executor behaves exactly as it did before this signal existed.
     *
     * A handler that asks a human must spin a nested #GMainLoop on
     * g_main_context_get_thread_default() --- *not* the global default,
     * which a caller driving the run from a private context would never
     * dispatch. The loop is synchronous at this point by design: the tool
     * has not run, and nothing else about the turn may proceed until the
     * answer arrives.
     *
     * This never fires for the CLI wrapper providers. They run their own
     * tools inside their own process, so approval there is
     * #AiClaudeCodeClient:permission-mode and its siblings, not this.
     */
    signals[SIGNAL_APPROVAL_REQUESTED] =
        g_signal_new ("approval-requested",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0,
                      ai_tool_approval_accumulator, NULL,
                      NULL,
                      G_TYPE_INT, 1,
                      AI_TYPE_TOOL_USE);

    /**
     * AiToolExecutor::todos-changed:
     * @self: the executor
     *
     * The todo list has been replaced.
     *
     * Emitted once per `todo_write` call, not once per item: the model
     * resends the whole list every time, and a frontend redrawing it
     * wants one notification per call.
     */
    signals[SIGNAL_TODOS_CHANGED] =
        g_signal_new ("todos-changed",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}

static void
ai_tool_executor_get_property (
    GObject    *object,
    guint       prop_id,
    GValue     *value,
    GParamSpec *pspec
){
    AiToolExecutor *self = AI_TOOL_EXECUTOR (object);

    switch (prop_id)
    {
        case PROP_APPROVAL_POLICY:
            g_value_set_int (value, self->approval_policy);
            break;
        case PROP_STREAM:
            g_value_set_boolean (value, self->stream);
            break;
        case PROP_WORKING_DIRECTORY:
            g_value_set_string (value, self->working_directory);
            break;
        case PROP_RESOURCE_REGISTRY:
            g_value_set_object (value, self->registry);
            break;
        case PROP_BRIGADE:
            g_value_set_object (value, self->brigade);
            break;
        case PROP_FEATURES:
            g_value_set_uint (value, (guint) self->features);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

static void
ai_tool_executor_set_property (
    GObject      *object,
    guint         prop_id,
    const GValue *value,
    GParamSpec   *pspec
){
    AiToolExecutor *self = AI_TOOL_EXECUTOR (object);

    switch (prop_id)
    {
        case PROP_APPROVAL_POLICY:
            self->approval_policy = (AiToolApproval) g_value_get_int (value);
            break;
        case PROP_STREAM:
            self->stream = g_value_get_boolean (value);
            break;
        case PROP_WORKING_DIRECTORY:
            ai_tool_executor_set_working_directory (self,
                                                    g_value_get_string (value));
            break;
        case PROP_RESOURCE_REGISTRY:
            ai_tool_executor_set_resource_registry (self,
                                                    g_value_get_object (value));
            break;
        case PROP_BRIGADE:
            ai_tool_executor_set_brigade (self, g_value_get_object (value));
            break;
        case PROP_FEATURES:
            ai_tool_executor_set_features (
                self, (AiToolFeatures) g_value_get_uint (value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
            break;
    }
}

/**
 * ai_tool_executor_get_approval_policy:
 * @self: an #AiToolExecutor
 *
 * Returns: what a %AI_TOOL_APPROVAL_DEFAULT answer resolves to
 */
AiToolApproval
ai_tool_executor_get_approval_policy (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), AI_TOOL_APPROVAL_ALLOW);

    return self->approval_policy;
}

/**
 * ai_tool_executor_set_approval_policy:
 * @self: an #AiToolExecutor
 * @policy: the fallback decision
 *
 * Sets what a %AI_TOOL_APPROVAL_DEFAULT answer resolves to.
 */
void
ai_tool_executor_set_approval_policy (
    AiToolExecutor *self,
    AiToolApproval  policy
){
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));

    if (self->approval_policy == policy)
        return;

    self->approval_policy = policy;
    g_object_notify_by_pspec (G_OBJECT (self),
                              properties[PROP_APPROVAL_POLICY]);
}

/**
 * ai_tool_executor_get_stream:
 * @self: an #AiToolExecutor
 *
 * Returns: whether turns are streamed when the provider supports it
 */
gboolean
ai_tool_executor_get_stream (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), FALSE);

    return self->stream;
}

/**
 * ai_tool_executor_set_stream:
 * @self: an #AiToolExecutor
 * @stream: %TRUE to stream each turn
 *
 * Sets whether to stream each turn when the provider supports it.
 */
void
ai_tool_executor_set_stream (
    AiToolExecutor *self,
    gboolean        stream
){
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));

    stream = !!stream;

    if (self->stream == stream)
        return;

    self->stream = stream;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_STREAM]);
}

static void
ai_tool_executor_init (AiToolExecutor *self)
{
    self->tools           = NULL;
    self->search_provider = NULL;
    self->approval_policy = AI_TOOL_APPROVAL_ALLOW;
    self->stream          = FALSE;
    self->always_allowed  = NULL;
    self->denied_all      = FALSE;
    self->registry        = NULL;
    self->brigade         = NULL;
    self->features        = AI_TOOL_FEATURE_ALL;
    self->task_depth      = 0;
    self->todos           = g_ptr_array_new_with_free_func (
                                (GDestroyNotify)ai_todo_free);
    self->callbacks       = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free, callback_entry_free);
}

/* ================================================================
 * Public API
 * ================================================================ */

AiToolExecutor *
ai_tool_executor_new_empty (void)
{
    return g_object_new (AI_TYPE_TOOL_EXECUTOR, NULL);
}

AiToolExecutor *
ai_tool_executor_new (void)
{
    AiToolExecutor *self = g_object_new (AI_TYPE_TOOL_EXECUTOR, NULL);
    AiTool         *tool;

    /* bash */
    tool = ai_tool_new ("bash",
                        "Run a shell command and return its stdout and stderr "
                        "combined. Nonzero exit codes are reported in the output.");
    ai_tool_add_parameter (tool, "command", "string",
                           "The shell command to execute.", TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* read */
    tool = ai_tool_new ("read",
                        "Read the contents of a file from disk.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Absolute or relative path to the file.", TRUE);
    ai_tool_add_parameter (tool, "offset", "number",
                           "Byte offset to start reading from (default: 0).", FALSE);
    ai_tool_add_parameter (tool, "limit", "number",
                           "Maximum number of bytes to read (default: entire file).", FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* write */
    tool = ai_tool_new ("write",
                        "Write content to a file, creating or overwriting it.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Absolute or relative path to the file.", TRUE);
    ai_tool_add_parameter (tool, "content", "string",
                           "The content to write to the file.", TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* edit */
    tool = ai_tool_new ("edit",
                        "Replace the first occurrence of old_string with "
                        "new_string in a file. The file must exist and "
                        "old_string must be found exactly once.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Absolute or relative path to the file.", TRUE);
    ai_tool_add_parameter (tool, "old_string", "string",
                           "The exact string to find and replace.", TRUE);
    ai_tool_add_parameter (tool, "new_string", "string",
                           "The replacement string.", TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* glob */
    tool = ai_tool_new ("glob",
                        "Find files whose names match a glob pattern, "
                        "searched recursively under a directory.");
    ai_tool_add_parameter (tool, "pattern", "string",
                           "Glob pattern to match filenames (e.g. '*.c', '*.h').",
                           TRUE);
    ai_tool_add_parameter (tool, "path", "string",
                           "Directory to search in (default: current directory).",
                           FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* grep */
    tool = ai_tool_new ("grep",
                        "Search file contents for a regular expression pattern. "
                        "Returns matching lines with file name and line number.");
    ai_tool_add_parameter (tool, "pattern", "string",
                           "Regular expression pattern to search for.", TRUE);
    ai_tool_add_parameter (tool, "path", "string",
                           "File or directory to search (default: current directory).",
                           FALSE);
    ai_tool_add_parameter (tool, "glob", "string",
                           "Glob pattern to filter files when path is a directory "
                           "(e.g. '*.c' to search only C source files).", FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* ls */
    tool = ai_tool_new ("ls",
                        "List the contents of a directory with type and size.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Directory to list (default: current directory).", FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* web_fetch */
    tool = ai_tool_new ("web_fetch",
                        "Fetch a URL over HTTP or HTTPS. HTML is converted to "
                        "readable markdown-style text (scripts/styles stripped, "
                        "headings and links preserved; images become "
                        "![alt](absolute-url) so you can reuse the URLs); JSON "
                        "and plain text are returned as-is. http:// is upgraded "
                        "to https://. "
                        "Responses are cached for 15 minutes. Returns up to "
                        "100 KB. If 'prompt' is given, a model reads the page "
                        "and returns only the requested information.");
    ai_tool_add_parameter (tool, "url", "string",
                           "The URL to fetch (must start with http:// or https://).",
                           TRUE);
    ai_tool_add_parameter (tool, "prompt", "string",
                           "Optional: what to extract from the page. When set, "
                           "a model summarises the fetched content and only the "
                           "result is returned instead of the full page.",
                           FALSE);
    self->tools = g_list_append (self->tools, tool);

    /* multi_edit */
    tool = ai_tool_new ("multi_edit",
                        "Apply several edits to one file atomically. Every "
                        "old_string must appear exactly once, and they are "
                        "applied in order; if any of them fails, the file is "
                        "left completely unchanged. Prefer this over several "
                        "edit calls on the same file.");
    ai_tool_add_parameter (tool, "path", "string",
                           "Absolute or relative path to the file.", TRUE);
    ai_tool_add_array_parameter (
        tool, "edits",
        "The edits to apply, in order.",
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"old_string\":{\"type\":\"string\","
        "\"description\":\"The exact text to replace. Must occur exactly "
        "once.\"},"
        "\"new_string\":{\"type\":\"string\","
        "\"description\":\"What to replace it with.\"}},"
        "\"required\":[\"old_string\",\"new_string\"]}",
        TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* todo_write */
    tool = ai_tool_new ("todo_write",
                        "Record the plan for a multi-step task, and keep it "
                        "current as you go. Send the whole list every time: "
                        "it replaces the previous one. Mark exactly one item "
                        "in_progress at a time, and mark it completed as soon "
                        "as it is done rather than in a batch at the end.");
    ai_tool_add_array_parameter (
        tool, "todos",
        "The complete list, in order.",
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"content\":{\"type\":\"string\","
        "\"description\":\"The task, imperative: \\\"Add the parser\\\".\"},"
        "\"active_form\":{\"type\":\"string\","
        "\"description\":\"The same task in progress: \\\"Adding the "
        "parser\\\".\"},"
        "\"status\":{\"type\":\"string\","
        "\"enum\":[\"pending\",\"in_progress\",\"completed\"]}},"
        "\"required\":[\"content\",\"status\"]}",
        TRUE);
    self->tools = g_list_append (self->tools, tool);

    /* web_search is registered on demand by set_search_provider(), and
     * task/skill by set_resource_registry(). */

    return self;
}

/*
 * Add or remove the `task` and `skill` tools to match the current state.
 *
 * Two things decide whether they are offered -- a registry to find what
 * they run, and the feature bit -- and either can change at any time, so
 * both paths converge here rather than each trying to work out what the
 * other left behind.
 */
static void
executor_sync_subagent_tools (AiToolExecutor *self)
{
    gboolean wanted = (self->registry != NULL) &&
                      (self->features & AI_TOOL_FEATURE_SUBAGENTS) != 0;
    gboolean present = executor_offers_tool (self, "task");

    if (wanted == present)
        return;

    if (!wanted)
    {
        ai_tool_executor_unregister (self, "task");
        ai_tool_executor_unregister (self, "skill");
        return;
    }

    {
        AiTool *tool;

        tool = ai_tool_new ("task",
                            "Delegate a self-contained piece of work to a "
                            "subagent, which runs with its own tools and "
                            "returns only its final answer. Use it when the "
                            "work is separable and would otherwise fill this "
                            "conversation with detail you do not need.");
        ai_tool_add_parameter (tool, "agent", "string",
                               "The agent to run. Use the skill tool or ask "
                               "for a listing if you are unsure which exist.",
                               TRUE);
        ai_tool_add_parameter (tool, "prompt", "string",
                               "What the agent should do. It sees nothing of "
                               "this conversation, so say everything it needs.",
                               TRUE);
        ai_tool_add_parameter (tool, "provider", "string",
                               "Which provider to run it on -- claude, openai, "
                               "gemini, grok, ollama, claude-code, opencode, "
                               "grok-build. Omit to use this conversation's.",
                               FALSE);
        ai_tool_add_parameter (tool, "model", "string",
                               "Which model, as that provider names it. Omit "
                               "for the provider's default.",
                               FALSE);
        self->tools = g_list_append (self->tools, tool);

        tool = ai_tool_new ("skill",
                            "Load a skill's instructions into the "
                            "conversation. A skill is a written procedure for "
                            "a kind of task; read one before doing work it "
                            "covers.");
        ai_tool_add_parameter (tool, "name", "string",
                               "The skill to load.", TRUE);
        self->tools = g_list_append (self->tools, tool);
    }
}

/*
 * Add or remove the `agent_*` tools to match the current state.
 *
 * The counterpart of executor_sync_subagent_tools(), and gated the same
 * way: a brigade to run agents in, and the feature bit.
 */
static void
executor_sync_background_tools (AiToolExecutor *self)
{
    gboolean wanted = (self->brigade != NULL) &&
                      (self->features & AI_TOOL_FEATURE_BACKGROUND) != 0;
    gboolean present = executor_offers_tool (self, "agent_spawn");

    if (wanted == present)
        return;

    if (!wanted)
    {
        ai_tool_executor_unregister (self, "agent_spawn");
        ai_tool_executor_unregister (self, "agent_status");
        ai_tool_executor_unregister (self, "agent_result");
        ai_tool_executor_unregister (self, "agent_wait");
        ai_tool_executor_unregister (self, "agent_cancel");
        return;
    }

    {
        AiTool *tool;

        tool = ai_tool_new ("agent_spawn",
                            "Start an agent working in the background and "
                            "return immediately with its id. Use it for work "
                            "that can proceed while you carry on -- a long "
                            "search, a second opinion, a build. You are told "
                            "when it finishes; check on it with agent_status "
                            "and collect its answer with agent_result.");
        ai_tool_add_parameter (tool, "prompt", "string",
                               "What the agent should do. It sees nothing of "
                               "this conversation, so say everything it needs.",
                               TRUE);
        ai_tool_add_parameter (tool, "description", "string",
                               "Three or four words naming the work, for the "
                               "status listing. E.g. 'audit the search code'.",
                               FALSE);
        ai_tool_add_parameter (tool, "agent", "string",
                               "An agent definition to run it as, which sets "
                               "its instructions and limits its tools. Omit "
                               "for a general-purpose agent.",
                               FALSE);
        ai_tool_add_parameter (tool, "provider", "string",
                               "Which provider to run it on -- claude, openai, "
                               "gemini, grok, ollama, claude-code, opencode, "
                               "grok-build. Omit to use this conversation's.",
                               FALSE);
        ai_tool_add_parameter (tool, "model", "string",
                               "Which model, as that provider names it. Omit "
                               "for the provider's default.",
                               FALSE);
        self->tools = g_list_append (self->tools, tool);

        tool = ai_tool_new ("agent_status",
                            "Report what background agents are doing: their "
                            "state, how long they have been at it, what they "
                            "have spent, and a peek at what they have produced "
                            "so far.");
        ai_tool_add_parameter (tool, "agent_id", "string",
                               "One agent to report on. Omit for all of them.",
                               FALSE);
        self->tools = g_list_append (self->tools, tool);

        tool = ai_tool_new ("agent_result",
                            "Collect a finished agent's answer and forget the "
                            "agent. Fails if it is still running -- use "
                            "agent_wait for that.");
        ai_tool_add_parameter (tool, "agent_id", "string",
                               "The agent to collect.", TRUE);
        self->tools = g_list_append (self->tools, tool);

        tool = ai_tool_new ("agent_wait",
                            "Wait until a background agent finishes, then "
                            "return its answer. Use it when you have nothing "
                            "useful to do until the work is done; otherwise "
                            "carry on and you will be told when it finishes.");
        ai_tool_add_parameter (tool, "agent_id", "string",
                               "The agent to wait for. Omit to wait for "
                               "whichever finishes first.",
                               FALSE);
        ai_tool_add_parameter (tool, "timeout_seconds", "integer",
                               "How long to wait before giving up and "
                               "returning. The agent keeps running.",
                               FALSE);
        self->tools = g_list_append (self->tools, tool);

        tool = ai_tool_new ("agent_cancel",
                            "Stop a background agent. Anything it had already "
                            "done stands; anything in flight is abandoned.");
        ai_tool_add_parameter (tool, "agent_id", "string",
                               "The agent to stop, or \"all\" for every one "
                               "that is running.",
                               TRUE);
        self->tools = g_list_append (self->tools, tool);
    }
}

/**
 * ai_tool_executor_set_resource_registry:
 * @self: an #AiToolExecutor
 * @registry: (nullable) (transfer none): where commands, skills and
 *   agents come from
 *
 * Gives the executor access to the harness resources on disk.
 *
 * Setting one registers the `task` and `skill` tools; clearing it
 * removes them again. That is deliberate: an executor with no registry
 * offers exactly the tools it always did, so adding subagent support to
 * the library changed nothing for callers who do not want it.
 */
void
ai_tool_executor_set_resource_registry (
    AiToolExecutor     *self,
    AiResourceRegistry *registry
){
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));

    if (!g_set_object (&self->registry, registry))
        return;

    executor_sync_subagent_tools (self);

    g_object_notify_by_pspec (G_OBJECT (self),
                              properties[PROP_RESOURCE_REGISTRY]);
}

/**
 * ai_tool_executor_set_brigade:
 * @self: an #AiToolExecutor
 * @brigade: (nullable) (transfer none): where background agents run
 *
 * Lets the model start work that outlives the turn it was asked in.
 *
 * Setting a brigade registers the `agent_*` tools; clearing it removes
 * them. Same arrangement as ai_tool_executor_set_resource_registry(),
 * and for the same reason --- an application that does not hand one over
 * is entirely unaffected.
 *
 * The brigade must have a worker to run anything. ai_local_worker_new()
 * is the one that runs agents in this process.
 */
void
ai_tool_executor_set_brigade (
    AiToolExecutor *self,
    AiBrigade      *brigade
){
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (brigade == NULL || AI_IS_BRIGADE (brigade));

    if (!g_set_object (&self->brigade, brigade))
        return;

    executor_sync_background_tools (self);

    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_BRIGADE]);
}

/**
 * ai_tool_executor_get_brigade:
 * @self: an #AiToolExecutor
 *
 * Returns: (transfer none) (nullable): the brigade, or %NULL
 */
AiBrigade *
ai_tool_executor_get_brigade (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    return self->brigade;
}

/**
 * ai_tool_executor_set_features:
 * @self: an #AiToolExecutor
 * @features: which optional tool groups to offer
 *
 * Turns the optional tool groups on or off.
 *
 * Takes effect immediately: clearing %AI_TOOL_FEATURE_BACKGROUND during a
 * run removes the `agent_*` tools before the next turn advertises them.
 * Agents already running are left alone --- withdrawing permission to
 * start work is not the same as killing work in progress, and stopping
 * something is ai_brigade_cancel_all().
 */
void
ai_tool_executor_set_features (
    AiToolExecutor *self,
    AiToolFeatures  features
){
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));

    if (self->features == features)
        return;

    self->features = features;

    executor_sync_subagent_tools (self);
    executor_sync_background_tools (self);

    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_FEATURES]);
}

/**
 * ai_tool_executor_get_features:
 * @self: an #AiToolExecutor
 *
 * Returns: which optional tool groups this executor may offer
 */
AiToolFeatures
ai_tool_executor_get_features (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), AI_TOOL_FEATURE_NONE);
    return self->features;
}

/**
 * ai_tool_executor_get_resource_registry:
 * @self: an #AiToolExecutor
 *
 * Returns: (transfer none) (nullable): the registry, or %NULL
 */
AiResourceRegistry *
ai_tool_executor_get_resource_registry (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    return self->registry;
}

/**
 * ai_tool_executor_get_n_todos:
 * @self: an #AiToolExecutor
 *
 * Returns: how many items are on the current todo list
 */
guint
ai_tool_executor_get_n_todos (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), 0);

    return self->todos->len;
}

/**
 * ai_tool_executor_get_todo:
 * @self: an #AiToolExecutor
 * @index: which item
 *
 * Returns: (transfer none) (nullable): the item, or %NULL if @index is
 *   out of range
 */
const AiTodo *
ai_tool_executor_get_todo (
    AiToolExecutor *self,
    guint           index
){
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    if (index >= self->todos->len)
        return NULL;

    return g_ptr_array_index (self->todos, index);
}

/**
 * ai_tool_executor_get_todo_fields:
 * @self: an #AiToolExecutor
 * @index: which item
 * @out_label: (out) (optional) (transfer none): what to show for it
 * @out_state: (out) (optional): where it stands
 *
 * Reads one item through out-parameters.
 *
 * The shape bindings use, for the same reason
 * ai_rendered_text_get_span() exists --- and @out_label already applies
 * the active-phrasing rule, so a renderer does not reimplement it.
 *
 * Returns: %FALSE if @index is out of range, leaving the outputs alone
 */
gboolean
ai_tool_executor_get_todo_fields (
    AiToolExecutor  *self,
    guint            index,
    const gchar    **out_label,
    AiTodoState     *out_state
){
    const AiTodo *todo;

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), FALSE);

    if (index >= self->todos->len)
        return FALSE;

    todo = g_ptr_array_index (self->todos, index);

    if (out_label != NULL)
        *out_label = ai_todo_get_label (todo);

    if (out_state != NULL)
        *out_state = todo->state;

    return TRUE;
}

/**
 * ai_tool_executor_get_todos:
 * @self: an #AiToolExecutor
 *
 * Returns: (transfer none) (element-type AiTodo): the current list,
 *   owned by the executor and replaced by the next `todo_write`
 */
GPtrArray *
ai_tool_executor_get_todos (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    return self->todos;
}

/**
 * ai_tool_executor_clear_todos:
 * @self: an #AiToolExecutor
 *
 * Empties the todo list and emits #AiToolExecutor::todos-changed.
 *
 * For a frontend's `/clear`: the transcript and the list belong to the
 * same conversation, and leaving one behind would be confusing.
 */
void
ai_tool_executor_clear_todos (AiToolExecutor *self)
{
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));

    if (self->todos->len == 0)
        return;

    g_ptr_array_set_size (self->todos, 0);
    g_signal_emit (self, signals[SIGNAL_TODOS_CHANGED], 0);
}

void
ai_tool_executor_set_search_provider (
    AiToolExecutor   *self,
    AiSearchProvider *provider
){
    GList       *iter;
    gboolean     already_registered = FALSE;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (AI_IS_SEARCH_PROVIDER (provider));

    g_set_object (&self->search_provider, provider);

    /* Register the web_search tool if not already present */
    for (iter = self->tools; iter != NULL; iter = iter->next)
    {
        if (g_strcmp0 (ai_tool_get_name (iter->data), "web_search") == 0)
        {
            already_registered = TRUE;
            break;
        }
    }

    if (!already_registered)
    {
        static const gchar *freshness_values[] =
            { "any", "day", "week", "month", "year", NULL };
        static const gchar *safesearch_values[] =
            { "off", "moderate", "strict", NULL };
        AiTool *tool = ai_tool_new ("web_search",
                                    "Search the web and return ranked results "
                                    "(title, URL, source, snippet). Optionally "
                                    "fetch the top results' page text.");

        ai_tool_add_parameter (tool, "query", "string",
                               "The search query string.", TRUE);
        ai_tool_add_parameter (tool, "count", "number",
                               "Maximum number of results to return "
                               "(default 10).", FALSE);
        ai_tool_add_enum_parameter (tool, "freshness",
                                    "Restrict results by recency.",
                                    (const gchar **) freshness_values, FALSE);
        ai_tool_add_enum_parameter (tool, "safesearch",
                                    "Safe-search filtering level.",
                                    (const gchar **) safesearch_values, FALSE);
        ai_tool_add_parameter (tool, "country", "string",
                               "Two-letter region/country code, e.g. \"US\".",
                               FALSE);
        ai_tool_add_parameter (tool, "language", "string",
                               "Two-letter language code, e.g. \"en\".", FALSE);
        ai_tool_add_parameter (tool, "site", "string",
                               "Restrict results to a single domain, e.g. "
                               "\"example.com\".", FALSE);
        ai_tool_add_parameter (tool, "fetch_content", "boolean",
                               "When true, fetch the top results' pages and "
                               "include their extracted text.", FALSE);
        self->tools = g_list_append (self->tools, tool);
    }
}

GList *
ai_tool_executor_get_tools (AiToolExecutor *self)
{
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    return self->tools;
}

/* Is @name in this executor's advertised tool list? */
static gboolean
executor_offers_tool (
    AiToolExecutor *self,
    const gchar    *name
){
    GList *iter;

    for (iter = self->tools; iter != NULL; iter = iter->next)
    {
        if (g_strcmp0 (ai_tool_get_name (iter->data), name) == 0)
            return TRUE;
    }

    return FALSE;
}

gchar *
ai_tool_executor_execute (
    AiToolExecutor  *self,
    AiToolUse       *tool_use,
    GCancellable    *cancellable,
    GError         **error
){
    const gchar     *name;
    const ToolEntry *entry;
    CallbackEntry   *user_entry;

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (tool_use != NULL, NULL);

    name = ai_tool_use_get_name (tool_use);

    /* User-registered callbacks win over built-ins so callers can override
     * a built-in tool by registering their own version with the same name. */
    user_entry = (name != NULL) ? g_hash_table_lookup (self->callbacks, name) : NULL;
    if (user_entry != NULL)
        return user_entry->fn (tool_use, cancellable, error, user_entry->user_data);

    /*
     * A built-in runs only if this executor advertises it.
     *
     * That is what makes an allowlist structural rather than a matter of
     * refusing calls: an executor built without bash has no bash, so
     * calling it is not denied, it is unrepresentable. ai_tool_executor_new_empty()
     * and the per-agent executors that `task` builds both depend on it.
     */
    for (entry = BUILTIN_TOOLS; entry->name != NULL; entry++)
    {
        if (g_strcmp0 (entry->name, name) != 0)
            continue;

        if (!executor_offers_tool (self, name))
            break;

        return entry->fn (self, tool_use, cancellable, error);
    }

    g_set_error (error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                 "ai_tool_executor_execute: unknown tool '%s'", name);
    return NULL;
}

void
ai_tool_executor_register_callback (
    AiToolExecutor  *self,
    AiTool          *tool,
    AiToolCallback   callback,
    gpointer         user_data,
    GDestroyNotify   user_data_free
){
    const gchar   *name;
    GList         *iter;
    CallbackEntry *entry;
    gboolean       tool_already_listed = FALSE;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (AI_IS_TOOL (tool));
    g_return_if_fail (callback != NULL);

    name = ai_tool_get_name (tool);
    g_return_if_fail (name != NULL);

    /* Register the callback (replaces any existing entry for this name). */
    entry = g_slice_new0 (CallbackEntry);
    entry->fn = callback;
    entry->user_data = user_data;
    entry->user_data_free = user_data_free;
    g_hash_table_replace (self->callbacks, g_strdup (name), entry);

    /* Ensure the tool definition appears in self->tools so the model sees
     * it in the request. Replace an existing entry with the same name. */
    for (iter = self->tools; iter != NULL; iter = iter->next)
    {
        AiTool *existing = iter->data;
        const gchar *existing_name = ai_tool_get_name (existing);

        if (g_strcmp0 (existing_name, name) == 0)
        {
            g_object_unref (existing);
            iter->data = g_object_ref (tool);
            tool_already_listed = TRUE;
            break;
        }
    }

    if (!tool_already_listed)
    {
        self->tools = g_list_append (self->tools, g_object_ref (tool));
    }
}

void
ai_tool_executor_unregister (
    AiToolExecutor *self,
    const gchar    *tool_name
){
    GList *iter;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (tool_name != NULL);

    g_hash_table_remove (self->callbacks, tool_name);

    for (iter = self->tools; iter != NULL; iter = iter->next)
    {
        AiTool *tool = iter->data;

        if (g_strcmp0 (ai_tool_get_name (tool), tool_name) == 0)
        {
            g_object_unref (tool);
            self->tools = g_list_delete_link (self->tools, iter);
            break;
        }
    }
}

/**
 * ai_tool_executor_set_working_directory:
 * @self: an #AiToolExecutor
 * @path: (nullable): directory the built-in tools work in, or %NULL
 *
 * Set the directory the built-in tools resolve relative paths against
 * and run commands in.
 *
 * An agent is given work to do somewhere -- a project, a checkout, a
 * notes tree -- and refers to "src/main.c", not to an absolute path.
 * Without this, every relative path and every `bash` command resolved
 * against the host process's own working directory, which for an editor
 * is wherever the user last visited a file.
 *
 * %NULL restores that behaviour.  An absolute path from the model is
 * always used as given.
 */
void
ai_tool_executor_set_working_directory (
    AiToolExecutor  *self,
    const gchar     *path
){
    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));

    g_free (self->working_directory);
    self->working_directory = g_strdup (path);
}

/**
 * ai_tool_executor_get_working_directory:
 * @self: an #AiToolExecutor
 *
 * Returns: (nullable) (transfer none): the directory set by
 *   ai_tool_executor_set_working_directory(), or %NULL.
 */
const gchar *
ai_tool_executor_get_working_directory (
    AiToolExecutor  *self
){
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);

    return self->working_directory;
}

/* g_clear_pointer needs a one-argument free; g_list_free_full takes two. */
static void
ai_tool_executor__free_messages (
    GList   *messages
){
    g_list_free_full (messages, g_object_unref);
}

/**
 * ai_tool_executor_run_full:
 * @self: an #AiToolExecutor
 * @provider: the #AiProvider to send requests to
 * @messages: (element-type AiMessage): initial conversation messages
 * @system_prompt: (nullable): optional system prompt
 * @max_tokens: maximum tokens for each response (0 for default 4096)
 * @cancellable: (nullable): a #GCancellable
 * @out_new_messages: (out) (optional) (nullable) (element-type AiMessage)
 *   (transfer full): return location for the messages the run produced --
 *   the assistant turns and tool results appended during the loop, and
 *   NOT the caller's originals. Free with
 *   g_list_free_full(list, g_object_unref).
 * @error: (out) (optional): return location for a #GError
 *
 * Like ai_tool_executor_run(), but also hands back the conversation the
 * run generated.
 *
 * The plain ai_tool_executor_run() drops it. It works on a private copy
 * of @messages, appends each assistant turn and tool result to that
 * copy, and frees the lot on the way out -- so a caller who wants to
 * continue the conversation afterwards has the final text and no record
 * of how the model got there. Sending a follow-up on top of that means
 * the model cannot see its own previous turn, which is not a
 * continuation at all.
 *
 * Pass @out_new_messages to get those messages back and append them to
 * whatever holds the conversation. Passing %NULL is exactly
 * ai_tool_executor_run().
 *
 * Returns: (transfer full) (nullable): the final response text, or %NULL on
 *   error. Free with g_free().
 */
gchar *
ai_tool_executor_run_full (
    AiToolExecutor  *self,
    AiProvider      *provider,
    GList           *messages,
    const gchar     *system_prompt,
    gint             max_tokens,
    GCancellable    *cancellable,
    GList          **out_new_messages,
    GError         **error
){
    RunContext  ctx;
    GList      *iter;
    gchar      *result;

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (AI_IS_PROVIDER (provider), NULL);
    g_return_val_if_fail (messages != NULL, NULL);

    if (out_new_messages != NULL)
        *out_new_messages = NULL;

    /* Bind the loop to the caller's thread-default context (NULL when
     * none is pushed, i.e. the process-global default -- unchanged from
     * the historical behaviour).  A synchronous caller can push a
     * private GMainContext to isolate this loop (and the provider's
     * thread-default-bound async I/O) from the caller's own default
     * context. */
    ctx.loop          = g_main_loop_new (g_main_context_get_thread_default (), FALSE);
    ctx.task          = NULL;
    ctx.executor      = self;
    ctx.provider      = provider;
    ctx.messages      = NULL;
    /* Borrowed in synchronous mode: the caller outlives the run. */
    ctx.system_prompt = (gchar *) system_prompt;
    ctx.max_tokens    = (max_tokens > 0) ? max_tokens : DEFAULT_MAX_TOKENS;
    ctx.max_turns     = MAX_TURNS;
    ctx.cancellable   = cancellable;
    ctx.turn_count    = 0;
    ctx.n_input       = 0;
    ctx.return_messages = FALSE;
    ctx.result        = NULL;
    ctx.error         = NULL;

    /* Expose the active provider to built-in tools (e.g. web_fetch's optional
     * prompt-based extraction) for the duration of this run only. */
    self->active_provider = provider;

    /*
     * Per-run state. ALLOW_ALWAYS is remembered for one run only: an
     * answer given about this task is not consent for the next one.
     */
    self->denied_all = FALSE;
    g_clear_pointer (&self->always_allowed, g_hash_table_unref);
    executor_watch_provider (self, provider);

    /* Shallow-copy the caller's messages so we can extend the list */
    for (iter = messages; iter != NULL; iter = iter->next)
    {
        ctx.messages = g_list_append (ctx.messages, g_object_ref (iter->data));
        ctx.n_input++;
    }

    run_context_send (&ctx);
    g_main_loop_run (ctx.loop);
    g_main_loop_unref (ctx.loop);

    self->active_provider = NULL;
    executor_unwatch_provider (self);
    g_clear_pointer (&self->always_allowed, g_hash_table_unref);

    /* Split our list at the caller's originals.  Everything past the
     * first n_input entries was appended during the loop -- the
     * assistant turns and the tool results -- and is what a caller
     * continuing the conversation needs.  Our ref on each is handed over
     * rather than dropped and re-taken. */
    if (out_new_messages != NULL)
    {
        *out_new_messages = run_context_take_new_messages (&ctx);
    }
    else
    {
        /* Free our message list (caller keeps ownership of their originals) */
        g_list_free_full (ctx.messages, g_object_unref);
    }

    if (ctx.error != NULL)
    {
        /* Nothing on the error path: a caller who got NULL should not
         * have to remember to free an out parameter as well, and a
         * half-finished exchange is not something to graft onto a
         * conversation that is about to be abandoned anyway. */
        if (out_new_messages != NULL)
            g_clear_pointer (out_new_messages, ai_tool_executor__free_messages);
        g_propagate_error (error, ctx.error);
        return NULL;
    }

    result = ctx.result;
    return result;
}

gchar *
ai_tool_executor_run (
    AiToolExecutor  *self,
    AiProvider      *provider,
    GList           *messages,
    const gchar     *system_prompt,
    gint             max_tokens,
    GCancellable    *cancellable,
    GError         **error
){
    return ai_tool_executor_run_full (self, provider, messages, system_prompt,
                                      max_tokens, cancellable, NULL, error);
}

static void
ai_tool_executor_run_async_internal (
    AiToolExecutor      *self,
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    gint                 max_turns,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data,
    gboolean             return_messages
){
    RunContext *ctx;
    GList      *iter;

    g_return_if_fail (AI_IS_TOOL_EXECUTOR (self));
    g_return_if_fail (AI_IS_PROVIDER (provider));

    ctx = g_new0 (RunContext, 1);
    ctx->loop          = NULL;
    ctx->task          = g_task_new (self, cancellable, callback, user_data);
    g_task_set_source_tag (
        ctx->task,
        return_messages
            ? ai_tool_executor_run_full_async
            : ai_tool_executor_run_async);
    ctx->executor      = g_object_ref (self);
    ctx->provider      = g_object_ref (provider);
    ctx->messages      = NULL;
    /* Owned here: the caller's string may not outlive an async run. */
    ctx->system_prompt = g_strdup (system_prompt);
    ctx->max_tokens    = (max_tokens > 0) ? max_tokens : DEFAULT_MAX_TOKENS;
    ctx->max_turns     = (max_turns  > 0) ? max_turns  : MAX_TURNS;
    ctx->cancellable   = cancellable ? g_object_ref (cancellable) : NULL;
    ctx->return_messages = return_messages;

    self->active_provider = provider;

    /*
     * Per-run state. ALLOW_ALWAYS is remembered for one run only: an
     * answer given about this task is not consent for the next one.
     */
    self->denied_all = FALSE;
    g_clear_pointer (&self->always_allowed, g_hash_table_unref);
    executor_watch_provider (self, provider);

    for (iter = messages; iter != NULL; iter = iter->next)
    {
        ctx->messages = g_list_append (ctx->messages, g_object_ref (iter->data));
        ctx->n_input++;
    }

    run_context_send (ctx);
}

/**
 * ai_tool_executor_run_async:
 * @self: an #AiToolExecutor
 * @provider: the #AiProvider to send requests to
 * @messages: (element-type AiMessage): initial conversation messages
 * @system_prompt: (nullable): optional system prompt
 * @max_tokens: maximum tokens per response, or 0 for the default
 * @max_turns: maximum tool-use turns, or 0 for the default of 20
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the loop finishes
 * @user_data: data for @callback
 *
 * Runs the tool-use conversation loop without blocking.
 *
 * This is what lets several agents run at once.  The synchronous
 * ai_tool_executor_run() drives its own nested #GMainLoop, so a caller
 * wanting two conversations in flight had to find two threads; this
 * version simply leaves callbacks pending on the caller's context, and N
 * concurrent runs are N sets of pending callbacks on one thread.
 *
 * @max_turns is a parameter rather than a constant because an
 * orchestrator budgets turns per agent, and a limit the caller cannot
 * set is a limit they have to work around.
 *
 * One executor supports one run at a time: it holds the tool list and
 * the active provider for the duration.  That is why an agent owns its
 * own executor rather than sharing one.
 */
void
ai_tool_executor_run_async (
    AiToolExecutor      *self,
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    gint                 max_turns,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    ai_tool_executor_run_async_internal (
        self, provider, messages, system_prompt, max_tokens, max_turns,
        cancellable, callback, user_data, FALSE);
}

/**
 * ai_tool_executor_run_full_async:
 * @self: an #AiToolExecutor
 * @provider: the #AiProvider to send requests to
 * @messages: (element-type AiMessage): initial conversation messages
 * @system_prompt: (nullable): optional system prompt
 * @max_tokens: maximum tokens per response, or 0 for the default
 * @max_turns: maximum tool-use turns, or 0 for the default of 20
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when the loop finishes
 * @user_data: data for @callback
 *
 * Asynchronously runs the tool loop while retaining the messages produced
 * by it. Complete with ai_tool_executor_run_full_finish().
 */
void
ai_tool_executor_run_full_async (
    AiToolExecutor      *self,
    AiProvider          *provider,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    gint                 max_turns,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
){
    ai_tool_executor_run_async_internal (
        self, provider, messages, system_prompt, max_tokens, max_turns,
        cancellable, callback, user_data, TRUE);
}

/**
 * ai_tool_executor_run_finish:
 * @self: an #AiToolExecutor
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the final response text, or %NULL
 *   on error.
 */
gchar *
ai_tool_executor_run_finish (
    AiToolExecutor  *self,
    GAsyncResult    *result,
    GError         **error
){
    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (g_task_is_valid (result, self), NULL);
    g_return_val_if_fail (
        g_async_result_is_tagged (result, ai_tool_executor_run_async), NULL);

    self->active_provider = NULL;
    executor_unwatch_provider (self);
    g_clear_pointer (&self->always_allowed, g_hash_table_unref);
    return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * ai_tool_executor_run_full_finish:
 * @self: an #AiToolExecutor
 * @result: the #GAsyncResult
 * @out_new_messages: (out) (optional) (nullable) (element-type AiMessage)
 *   (transfer full): return location for messages produced during the run
 * @error: (out) (optional): return location for a #GError
 *
 * Finishes ai_tool_executor_run_full_async().
 *
 * No messages are returned when the run fails, because a partial tool
 * exchange cannot safely be grafted onto a continuing conversation.
 *
 * Returns: (transfer full) (nullable): the final response text, or %NULL
 *   on error.
 */
gchar *
ai_tool_executor_run_full_finish (
    AiToolExecutor  *self,
    GAsyncResult    *result,
    GList          **out_new_messages,
    GError         **error
){
    RunResult *run_result;
    gchar     *text;

    g_return_val_if_fail (AI_IS_TOOL_EXECUTOR (self), NULL);
    g_return_val_if_fail (g_task_is_valid (result, self), NULL);
    g_return_val_if_fail (
        g_async_result_is_tagged (result, ai_tool_executor_run_full_async),
        NULL);

    if (out_new_messages != NULL)
        *out_new_messages = NULL;

    self->active_provider = NULL;
    executor_unwatch_provider (self);
    g_clear_pointer (&self->always_allowed, g_hash_table_unref);

    run_result = g_task_propagate_pointer (G_TASK (result), error);
    if (run_result == NULL)
        return NULL;

    text = g_steal_pointer (&run_result->text);
    if (out_new_messages != NULL)
        *out_new_messages = g_steal_pointer (&run_result->messages);

    run_result_free (run_result);
    return text;
}
