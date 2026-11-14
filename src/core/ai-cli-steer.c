/*
 * ai-cli-steer.c - The open stdin of a steerable CLI stream
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * See ai-cli-steer-private.h for the shape.  The rule every path below
 * keeps: a steer is REFUSED only while not one byte of it has gone into
 * the pipe.  Once any of it has, the answer is DELIVERED (the engine
 * said so) or INDETERMINATE (it did not, and the run is gone or the wait
 * ran out).
 */

#include "config.h"

#include <signal.h>
#include <string.h>

#include "core/ai-cli-steer-private.h"
#include "core/ai-steerable-private.h"

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

/* One thing to put into stdin: the prompt, a protocol line, or a steer. */
typedef struct
{
    gchar  *data;
    gsize   length;
    gsize   offset;      /* bytes already in the pipe */
    GTask  *task;        /* the steer's task; NULL for the parser's lines */
    gchar  *ack_key;     /* what the engine will echo, or NULL */
    guint   ack_timeout_ms;
} SteerWrite;

/* A steer that is in the pipe and waiting for the engine to say so. */
typedef struct
{
    AiCliSteerChannel *channel;   /* borrowed: the channel owns the wait */
    GTask             *task;
    gchar             *key;
    GSource           *timeout;   /* owned; NULL when none is armed */
} SteerWait;

struct _AiCliSteerChannel
{
    gint            ref_count;     /* atomic */

    /* Immutable after creation; safe to read from any thread. */
    AiCliClient    *client;        /* strong; dropped at end */
    gchar          *turn_id;
    GMainContext   *context;

    /* Everything below is touched only on @context. */
    GOutputStream  *input;         /* NULL once closed */
    GCancellable   *cancellable;
    GQueue          writes;        /* SteerWrite*, head in flight while writing */
    gboolean        writing;
    GHashTable     *awaiting;      /* ack key -> SteerWait* */
    gboolean        settled;
    gboolean        ended;

    gpointer        data;
    GDestroyNotify  data_free;

    AiCliSteerInputFailed input_failed;
    gpointer              input_failed_data;
};

/* The client's live channels, newest last.  Kept as object data so the
 * registry costs AiCliClient nothing until a steerable run exists. */
typedef struct
{
    GMutex     lock;
    GPtrArray *channels;           /* AiCliSteerChannel*, ref'd */
} SteerRegistry;

static GPrivate current_channel = G_PRIVATE_INIT(NULL);
static GMutex   registry_create_lock;

#define STEER_REGISTRY_KEY "ai-cli-steer-registry"

static void channel_pump(AiCliSteerChannel *channel);
static void channel_maybe_close(AiCliSteerChannel *channel);

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

static void
steer_registry_free(gpointer data)
{
    SteerRegistry *registry = data;

    g_ptr_array_unref(registry->channels);
    g_mutex_clear(&registry->lock);
    g_free(registry);
}

static SteerRegistry *
steer_registry_get(AiCliClient *client, gboolean create)
{
    SteerRegistry *registry;

    g_mutex_lock(&registry_create_lock);
    registry = g_object_get_data(G_OBJECT(client), STEER_REGISTRY_KEY);
    if (registry == NULL && create)
    {
        registry = g_new0(SteerRegistry, 1);
        g_mutex_init(&registry->lock);
        registry->channels = g_ptr_array_new_with_free_func(
            (GDestroyNotify)ai_cli_steer_channel_unref);
        g_object_set_data_full(G_OBJECT(client), STEER_REGISTRY_KEY,
                               registry, steer_registry_free);
    }
    g_mutex_unlock(&registry_create_lock);

    return registry;
}

static AiCliSteerChannel *
channel_ref(AiCliSteerChannel *channel)
{
    g_atomic_int_inc(&channel->ref_count);
    return channel;
}

/* ------------------------------------------------------------------ */
/* Channel lifetime                                                    */
/* ------------------------------------------------------------------ */

/*
 * A CLI that dies while a steer is being written turns the write into
 * SIGPIPE, and SIGPIPE's default action kills the *caller* -- a daemon
 * steering an agent whose CLI just exited.  The steer path writes into
 * pipes at moments the child chooses to exit, which the one-shot prompt
 * write never did, so it is ignored here, once, and only when nobody
 * has installed a handler of their own.  ai-mcp-host.c does the same.
 */
static void
steer_ignore_sigpipe(void)
{
    static gsize once = 0;

    if (g_once_init_enter(&once))
    {
        struct sigaction current;

        if (sigaction(SIGPIPE, NULL, &current) == 0 &&
            current.sa_handler == SIG_DFL)
            signal(SIGPIPE, SIG_IGN);

        g_once_init_leave(&once, 1);
    }
}

#define STEER_OPEN_INPUT_KEY "ai-cli-steer-open-input"
#define STEER_PROTOCOL_INPUT_KEY "ai-cli-steer-protocol-input"

gboolean
ai_cli_steer_wants_open_input(AiCliClient *client)
{
    g_return_val_if_fail(AI_IS_CLI_CLIENT(client), FALSE);

    /* A request/response protocol on stdin needs it open, steerable or not. */
    if (g_object_get_data(G_OBJECT(client), STEER_PROTOCOL_INPUT_KEY) != NULL)
        return TRUE;

    return AI_IS_STEERABLE(client) &&
           ai_steerable_can_steer(AI_STEERABLE(client)) &&
           g_object_get_data(G_OBJECT(client), STEER_OPEN_INPUT_KEY) != NULL;
}

void
ai_cli_steer_set_open_input(AiCliClient *client, gboolean open_input)
{
    g_return_if_fail(AI_IS_CLI_CLIENT(client));

    g_object_set_data(G_OBJECT(client), STEER_OPEN_INPUT_KEY,
                      open_input ? GINT_TO_POINTER(1) : NULL);
}

void
ai_cli_steer_set_protocol_input(AiCliClient *client, gboolean protocol)
{
    g_return_if_fail(AI_IS_CLI_CLIENT(client));

    g_object_set_data(G_OBJECT(client), STEER_PROTOCOL_INPUT_KEY,
                      protocol ? GINT_TO_POINTER(1) : NULL);
}

static void
steer_write_free(SteerWrite *write)
{
    g_free(write->data);
    g_free(write->ack_key);
    g_clear_object(&write->task);
    g_free(write);
}

static SteerWrite *
steer_write_new(const gchar *line, GTask *task, gchar *ack_key,
                guint ack_timeout_ms)
{
    SteerWrite *write = g_new0(SteerWrite, 1);

    write->data = g_strdup(line);
    write->length = strlen(line);
    write->task = task != NULL ? g_object_ref(task) : NULL;
    write->ack_key = ack_key;
    write->ack_timeout_ms = ack_timeout_ms;

    return write;
}

static void
steer_wait_free(gpointer data)
{
    SteerWait *wait = data;

    if (wait->timeout != NULL)
    {
        g_source_destroy(wait->timeout);
        g_clear_pointer(&wait->timeout, g_source_unref);
    }
    g_clear_object(&wait->task);
    g_free(wait->key);
    g_free(wait);
}

AiCliSteerChannel *
ai_cli_steer_channel_new(
    AiCliClient           *client,
    GOutputStream         *input,
    const gchar           *initial_data,
    AiCliSteerInputFailed  input_failed,
    gpointer               user_data
){
    AiCliSteerChannel *channel;
    SteerRegistry *registry;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(client), NULL);
    g_return_val_if_fail(G_IS_OUTPUT_STREAM(input), NULL);

    steer_ignore_sigpipe();

    channel = g_new0(AiCliSteerChannel, 1);
    channel->ref_count = 1;
    channel->client = g_object_ref(client);
    channel->turn_id = g_uuid_string_random();
    channel->context = g_main_context_ref_thread_default();
    channel->input = g_object_ref(input);
    channel->cancellable = g_cancellable_new();
    g_queue_init(&channel->writes);
    channel->awaiting = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, steer_wait_free);
    channel->input_failed = input_failed;
    channel->input_failed_data = user_data;

    registry = steer_registry_get(client, TRUE);
    g_mutex_lock(&registry->lock);
    g_ptr_array_add(registry->channels, channel_ref(channel));
    g_mutex_unlock(&registry->lock);

    /* The prompt is the first thing in the pipe, ahead of any steer. */
    if (initial_data != NULL && initial_data[0] != '\0')
        g_queue_push_tail(&channel->writes,
                          steer_write_new(initial_data, NULL, NULL, 0));
    channel_pump(channel);

    return channel;
}

void
ai_cli_steer_channel_unref(AiCliSteerChannel *channel)
{
    if (channel == NULL || !g_atomic_int_dec_and_test(&channel->ref_count))
        return;

    g_queue_clear_full(&channel->writes, (GDestroyNotify)steer_write_free);
    g_clear_pointer(&channel->awaiting, g_hash_table_unref);
    if (channel->data_free != NULL && channel->data != NULL)
        channel->data_free(channel->data);
    g_clear_object(&channel->input);
    g_clear_object(&channel->cancellable);
    g_clear_object(&channel->client);
    g_main_context_unref(channel->context);
    g_free(channel->turn_id);
    g_free(channel);
}

static void
channel_close_input(AiCliSteerChannel *channel)
{
    if (channel->input == NULL)
        return;

    /* EOF is what tells the CLI the conversation is over. */
    g_output_stream_close(channel->input, NULL, NULL);
    g_clear_object(&channel->input);
}

/* Refuse every steer still queued behind the one in flight. */
static void
channel_refuse_queued(AiCliSteerChannel *channel, const gchar *why)
{
    GList *link = channel->writes.head;

    /* The head is on the wire while writing; its callback answers it. */
    if (channel->writing && link != NULL)
        link = link->next;

    while (link != NULL)
    {
        GList *next = link->next;
        SteerWrite *write = link->data;

        if (write->task != NULL)
            ai_steer_task_return(write->task, AI_STEER_REFUSED,
                g_error_new(AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN,
                            "%s before the words were sent", why));
        steer_write_free(write);
        g_queue_delete_link(&channel->writes, link);
        link = next;
    }
}

void
ai_cli_steer_channel_end(AiCliSteerChannel *channel)
{
    SteerRegistry *registry;
    GHashTableIter iter;
    gpointer value;

    g_return_if_fail(channel != NULL);

    if (channel->ended)
        return;
    channel->ended = TRUE;

    /* Off the registry first, so no new steer finds a dead run. */
    registry = steer_registry_get(channel->client, FALSE);
    if (registry != NULL)
    {
        g_mutex_lock(&registry->lock);
        g_ptr_array_remove(registry->channels, channel);
        g_mutex_unlock(&registry->lock);
    }

    channel_refuse_queued(channel, "the turn ended");

    /*
     * In the pipe, never answered: the engine may have read them.
     * INDETERMINATE, which the caller counts as delivered.
     */
    g_hash_table_iter_init(&iter, channel->awaiting);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        SteerWait *wait = value;

        ai_steer_task_return(wait->task, AI_STEER_INDETERMINATE, NULL);
        g_clear_object(&wait->task);
    }
    g_hash_table_remove_all(channel->awaiting);

    /* A write still in flight is cancelled; its callback answers it. */
    g_cancellable_cancel(channel->cancellable);
    if (!channel->writing)
        channel_close_input(channel);
}

/* ------------------------------------------------------------------ */
/* Writing                                                             */
/* ------------------------------------------------------------------ */

static gboolean
on_wait_timeout(gpointer user_data)
{
    SteerWait *wait = user_data;
    AiCliSteerChannel *channel = wait->channel;

    /* The source is finishing; the wait must not destroy it again. */
    g_clear_pointer(&wait->timeout, g_source_unref);

    ai_steer_task_return(wait->task, AI_STEER_INDETERMINATE, NULL);
    g_clear_object(&wait->task);
    g_hash_table_remove(channel->awaiting, wait->key);
    channel_maybe_close(channel);

    return G_SOURCE_REMOVE;
}

/* A steer is entirely in the pipe: answer it, or wait for the engine. */
static void
channel_sent(AiCliSteerChannel *channel, SteerWrite *write)
{
    SteerWait *wait;

    if (write->task == NULL)
        return;

    if (write->ack_key == NULL || channel->ended)
    {
        ai_steer_task_return(write->task,
                             channel->ended ? AI_STEER_INDETERMINATE
                                            : AI_STEER_DELIVERED,
                             NULL);
        return;
    }

    wait = g_new0(SteerWait, 1);
    wait->channel = channel;
    wait->task = g_steal_pointer(&write->task);
    wait->key = g_strdup(write->ack_key);

    if (write->ack_timeout_ms > 0)
    {
        wait->timeout = g_timeout_source_new(write->ack_timeout_ms);
        g_source_set_callback(wait->timeout, on_wait_timeout, wait, NULL);
        g_source_attach(wait->timeout, channel->context);
    }

    g_hash_table_replace(channel->awaiting, wait->key, wait);
}

static void
on_channel_write(GObject *source, GAsyncResult *result, gpointer user_data)
{
    AiCliSteerChannel *channel = user_data;
    SteerWrite *write = g_queue_peek_head(&channel->writes);
    g_autoptr(GError) error = NULL;
    gsize written = 0;
    gboolean ok;

    ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result,
                                          &written, &error);
    channel->writing = FALSE;
    write->offset += written;

    if (!ok)
    {
        g_queue_pop_head(&channel->writes);

        /*
         * Nothing of it in the pipe: provably not delivered.  Part of it:
         * the engine may have read a torn line, or may yet -- not ours
         * to call refused.
         */
        if (write->task != NULL)
        {
            if (write->offset == 0)
                ai_steer_task_return(write->task, AI_STEER_REFUSED,
                    g_error_new(AI_STEER_ERROR, AI_STEER_ERROR_SEND_FAILED,
                                "could not write to the CLI: %s",
                                error->message));
            else
                ai_steer_task_return(write->task, AI_STEER_INDETERMINATE,
                                     NULL);
        }
        g_debug("steer: stdin write failed: %s", error->message);

        /* The prompt did not go in: that is the run's failure. */
        if (write->task == NULL && !channel->ended &&
            channel->input_failed != NULL)
        {
            AiCliSteerInputFailed input_failed = channel->input_failed;

            channel->input_failed = NULL;
            input_failed(g_error_copy(error), channel->input_failed_data);
        }
        steer_write_free(write);

        /* A pipe that refused one write will refuse the rest. */
        channel_refuse_queued(channel, "the CLI stopped reading its input");
        channel_close_input(channel);
        ai_cli_steer_channel_unref(channel);
        return;
    }

    if (write->offset < write->length)
    {
        channel_pump(channel);
        ai_cli_steer_channel_unref(channel);
        return;
    }

    g_queue_pop_head(&channel->writes);
    channel_sent(channel, write);
    steer_write_free(write);

    if (channel->ended)
    {
        channel_refuse_queued(channel, "the turn ended");
        channel_close_input(channel);
    }
    else
    {
        channel_pump(channel);
        channel_maybe_close(channel);
    }
    ai_cli_steer_channel_unref(channel);
}

static void
channel_pump(AiCliSteerChannel *channel)
{
    SteerWrite *write;

    if (channel->writing || channel->input == NULL)
        return;

    write = g_queue_peek_head(&channel->writes);
    if (write == NULL)
        return;

    channel->writing = TRUE;

    /*
     * Pipes may accept only PIPE_BUF bytes without blocking, and a larger
     * write can stall the main context despite the async API -- the same
     * reason the one-shot prompt writer goes 512 bytes at a time.  The
     * callback holds a reference so a channel ended meanwhile survives.
     */
    g_output_stream_write_all_async(channel->input,
                                    write->data + write->offset,
                                    MIN(write->length - write->offset, 512),
                                    G_PRIORITY_DEFAULT, channel->cancellable,
                                    on_channel_write, channel_ref(channel));
}

static void
channel_maybe_close(AiCliSteerChannel *channel)
{
    if (!channel->settled || channel->writing ||
        !g_queue_is_empty(&channel->writes) ||
        g_hash_table_size(channel->awaiting) > 0)
        return;

    channel_close_input(channel);
}

/* ------------------------------------------------------------------ */
/* Parser-facing                                                       */
/* ------------------------------------------------------------------ */

void
ai_cli_steer_set_current(AiCliSteerChannel *channel)
{
    g_private_set(&current_channel, channel);
}

AiCliSteerChannel *
ai_cli_steer_current(void)
{
    return g_private_get(&current_channel);
}

void
ai_cli_steer_channel_settle(AiCliSteerChannel *channel)
{
    if (channel == NULL || channel->ended)
        return;

    channel->settled = TRUE;
    channel_refuse_queued(channel, "the turn ended");
    channel_maybe_close(channel);
}

void
ai_cli_steer_channel_resolve(
    AiCliSteerChannel *channel,
    const gchar       *ack_key,
    AiSteerResult      result,
    GError            *error
){
    SteerWait *wait;

    if (channel == NULL || ack_key == NULL)
    {
        g_clear_error(&error);
        return;
    }

    /* Not ours (the prompt's own echo, say), or already answered. */
    wait = g_hash_table_lookup(channel->awaiting, ack_key);
    if (wait == NULL)
    {
        g_clear_error(&error);
        return;
    }

    ai_steer_task_return(wait->task, result, error);
    g_clear_object(&wait->task);
    g_hash_table_remove(channel->awaiting, ack_key);
    channel_maybe_close(channel);
}

void
ai_cli_steer_channel_write(
    AiCliSteerChannel *channel,
    const gchar       *line
){
    if (channel == NULL || channel->ended || line == NULL)
        return;

    g_queue_push_tail(&channel->writes, steer_write_new(line, NULL, NULL, 0));
    channel_pump(channel);
}

gpointer
ai_cli_steer_channel_get_data(AiCliSteerChannel *channel)
{
    return channel != NULL ? channel->data : NULL;
}

void
ai_cli_steer_channel_set_data(
    AiCliSteerChannel *channel,
    gpointer           data,
    GDestroyNotify     destroy
){
    g_return_if_fail(channel != NULL);

    if (channel->data_free != NULL && channel->data != NULL)
        channel->data_free(channel->data);
    channel->data = data;
    channel->data_free = destroy;
}

const gchar *
ai_cli_steer_channel_get_turn_id(AiCliSteerChannel *channel)
{
    return channel != NULL ? channel->turn_id : NULL;
}

/* ------------------------------------------------------------------ */
/* AiSteerable-facing                                                  */
/* ------------------------------------------------------------------ */

gchar *
ai_cli_steer_dup_turn_id(AiCliClient *client)
{
    SteerRegistry *registry;
    gchar *turn_id = NULL;

    g_return_val_if_fail(AI_IS_CLI_CLIENT(client), NULL);

    registry = steer_registry_get(client, FALSE);
    if (registry == NULL)
        return NULL;

    g_mutex_lock(&registry->lock);
    if (registry->channels->len > 0)
    {
        AiCliSteerChannel *newest = g_ptr_array_index(
            registry->channels, registry->channels->len - 1);

        turn_id = g_strdup(newest->turn_id);
    }
    g_mutex_unlock(&registry->lock);

    return turn_id;
}

typedef struct
{
    AiCliSteerChannel   *channel;   /* ref'd */
    GTask               *task;      /* ref'd */
    gchar               *text;
    AiCliSteerBuildFunc  build;
    guint                ack_timeout_ms;
} SteerRequest;

static void
steer_request_free(gpointer data)
{
    SteerRequest *request = data;

    ai_cli_steer_channel_unref(request->channel);
    g_clear_object(&request->task);
    g_free(request->text);
    g_free(request);
}

/* On the channel's context: refuse, or put the words in the queue. */
static gboolean
steer_dispatch(gpointer user_data)
{
    SteerRequest *request = user_data;
    AiCliSteerChannel *channel = request->channel;
    GCancellable *cancellable = g_task_get_cancellable(request->task);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *line = NULL;
    gchar *ack_key = NULL;

    if (channel->ended || channel->settled || channel->input == NULL)
    {
        ai_steer_task_return(request->task, AI_STEER_REFUSED,
            g_error_new_literal(AI_STEER_ERROR, AI_STEER_ERROR_NO_ACTIVE_TURN,
                                "the turn has already ended"));
        return G_SOURCE_REMOVE;
    }

    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable))
    {
        ai_steer_task_return(request->task, AI_STEER_REFUSED,
            g_error_new_literal(AI_STEER_ERROR, AI_STEER_ERROR_CANCELLED,
                                "cancelled before the words were sent"));
        return G_SOURCE_REMOVE;
    }

    line = request->build(channel->client, channel, request->text,
                          &ack_key, &error);
    if (line == NULL)
    {
        ai_steer_task_return(request->task, AI_STEER_REFUSED,
            error != NULL ? g_steal_pointer(&error)
                          : g_error_new_literal(AI_STEER_ERROR,
                                                AI_STEER_ERROR_NOT_STEERABLE,
                                                "the running turn cannot take input"));
        g_free(ack_key);
        return G_SOURCE_REMOVE;
    }

    g_queue_push_tail(&channel->writes,
                      steer_write_new(line, request->task, ack_key,
                                      request->ack_timeout_ms));
    channel_pump(channel);

    return G_SOURCE_REMOVE;
}

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
){
    g_autoptr(GTask) task = NULL;
    SteerRegistry *registry;
    AiCliSteerChannel *channel = NULL;
    SteerRequest *request;
    GError *error = NULL;

    g_return_if_fail(AI_IS_CLI_CLIENT(client));
    g_return_if_fail(text != NULL);
    g_return_if_fail(build != NULL);

    task = ai_steer_task_new(client, cancellable, callback, user_data);
    g_task_set_source_tag(task, ai_cli_steer_async);

    /* Pick the run under the lock, then let go of it before any I/O. */
    registry = steer_registry_get(client, FALSE);
    if (registry != NULL)
    {
        guint i;

        g_mutex_lock(&registry->lock);
        if (turn_id != NULL)
        {
            for (i = 0; i < registry->channels->len; i++)
            {
                AiCliSteerChannel *candidate =
                    g_ptr_array_index(registry->channels, i);

                if (g_strcmp0(candidate->turn_id, turn_id) == 0)
                {
                    channel = channel_ref(candidate);
                    break;
                }
            }

            if (channel == NULL && registry->channels->len > 0)
                error = g_error_new(AI_STEER_ERROR,
                                    AI_STEER_ERROR_TURN_MISMATCH,
                                    "turn %s is not the one running", turn_id);
        }
        else if (registry->channels->len == 1)
        {
            channel = channel_ref(g_ptr_array_index(registry->channels, 0));
        }
        else if (registry->channels->len > 1)
        {
            error = g_error_new(AI_STEER_ERROR, AI_STEER_ERROR_AMBIGUOUS,
                                "%u turns are running; name one",
                                registry->channels->len);
        }
        g_mutex_unlock(&registry->lock);
    }

    if (channel == NULL)
    {
        if (error == NULL)
            error = g_error_new_literal(AI_STEER_ERROR,
                                        AI_STEER_ERROR_NO_ACTIVE_TURN,
                                        "no turn is running");
        ai_steer_task_return(task, AI_STEER_REFUSED, error);
        return;
    }

    request = g_new0(SteerRequest, 1);
    request->channel = channel;
    request->task = g_object_ref(task);
    request->text = g_strdup(text);
    request->build = build;
    request->ack_timeout_ms = ack_timeout_ms;

    g_main_context_invoke_full(channel->context, G_PRIORITY_DEFAULT,
                               steer_dispatch, request, steer_request_free);
}

AiSteerResult
ai_cli_steer_finish(
    AiCliClient   *client,
    GAsyncResult  *result,
    GError       **error
){
    g_return_val_if_fail(AI_IS_CLI_CLIENT(client), AI_STEER_REFUSED);
    g_return_val_if_fail(g_task_is_valid(result, client), AI_STEER_REFUSED);

    return ai_steer_task_propagate(G_TASK(result), error);
}
