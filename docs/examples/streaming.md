# Streaming Example

Real-time streaming responses from AI providers.

## Overview

This example demonstrates:
- Using the `AiStreamable` interface
- Connecting to streaming signals
- Handling deltas as they arrive
- Processing tool use during streaming

## Prerequisites

Set your API key:

```bash
export ANTHROPIC_API_KEY="your-key"
```

## Code

```c
/*
 * streaming-chat.c - Streaming response example
 *
 * Build: gcc -o streaming-chat streaming-chat.c $(pkg-config --cflags --libs ai-glib-1.0)
 */

#include <ai-glib.h>

typedef struct {
    GMainLoop *loop;
    gboolean   first_delta;
} StreamData;

static void
on_stream_start(
    AiStreamable *streamable,
    gpointer      user_data
){
    StreamData *data = user_data;
    data->first_delta = TRUE;
    g_print("Assistant: ");
    fflush(stdout);
}

static void
on_delta(
    AiStreamable *streamable,
    const gchar  *text,
    gpointer      user_data
){
    /* Print each text chunk as it arrives */
    g_print("%s", text);
    fflush(stdout);
}

static void
on_tool_use(
    AiStreamable *streamable,
    AiToolUse    *tool_use,
    gpointer      user_data
){
    g_print("\n[Tool use: %s (id: %s)]\n",
            ai_tool_use_get_name(tool_use),
            ai_tool_use_get_id(tool_use));
}

static void
on_stream_end(
    AiStreamable *streamable,
    gpointer      user_data
){
    g_print("\n\n[Stream complete]\n");
}

static void
on_stream_complete(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    StreamData *data = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_streamable_chat_stream_finish(
        AI_STREAMABLE(source), result, &error);

    if (error != NULL)
    {
        g_printerr("\nError: %s\n", error->message);
    }
    else
    {
        /* Print final usage stats */
        const AiUsage *usage = ai_response_get_usage(response);
        if (usage != NULL)
        {
            g_print("Tokens: %d input, %d output\n",
                    ai_usage_get_input_tokens(usage),
                    ai_usage_get_output_tokens(usage));
        }
    }

    g_main_loop_quit(data->loop);
}

int
main(
    int   argc,
    char *argv[]
){
    g_autoptr(AiClaudeClient) client = NULL;
    g_autoptr(AiMessage) msg = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    GList *messages = NULL;
    StreamData data = { 0 };

    /* Create client */
    client = ai_claude_client_new();
    ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_SONNET);

    /* Create message */
    msg = ai_message_new_user(
        "Write a short poem about programming in C.");
    messages = g_list_append(NULL, msg);

    /* Set up main loop */
    loop = g_main_loop_new(NULL, FALSE);
    data.loop = loop;

    /* Connect streaming signals */
    g_signal_connect(client, "stream-start",
                     G_CALLBACK(on_stream_start), &data);
    g_signal_connect(client, "delta",
                     G_CALLBACK(on_delta), &data);
    g_signal_connect(client, "tool-use",
                     G_CALLBACK(on_tool_use), &data);
    g_signal_connect(client, "stream-end",
                     G_CALLBACK(on_stream_end), &data);

    g_print("User: Write a short poem about programming in C.\n\n");

    /* Start streaming */
    ai_streamable_chat_stream_async(
        AI_STREAMABLE(client),
        messages,
        NULL,           /* system prompt */
        4096,           /* max tokens */
        NULL,           /* tools */
        NULL,           /* cancellable */
        on_stream_complete,
        &data
    );

    g_main_loop_run(loop);

    g_list_free(messages);
    return 0;
}
```

## AiStreamable Interface

All provider clients implement `AiStreamable`:

```c
/* Start a streaming request */
void
ai_streamable_chat_stream_async(
    AiStreamable        *streamable,
    GList               *messages,
    const gchar         *system_prompt,
    gint                 max_tokens,
    GList               *tools,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);

/* Finish the streaming request */
AiResponse *
ai_streamable_chat_stream_finish(
    AiStreamable  *streamable,
    GAsyncResult  *result,
    GError       **error
);
```

## Signals

### stream-start

Emitted when the stream begins:

```c
void
on_stream_start(AiStreamable *streamable, gpointer user_data);
```

### delta

Emitted for each text chunk:

```c
void
on_delta(AiStreamable *streamable, const gchar *text, gpointer user_data);
```

### tool-use

Emitted when tool use is detected (streaming with tools):

```c
void
on_tool_use(AiStreamable *streamable, AiToolUse *tool_use, gpointer user_data);
```

### stream-end

Emitted when the stream completes:

```c
void
on_stream_end(AiStreamable *streamable, gpointer user_data);
```

## Streaming with Tools

```c
static void
on_tool_use(AiStreamable *streamable, AiToolUse *tool_use, gpointer user_data)
{
    const gchar *name = ai_tool_use_get_name(tool_use);
    const gchar *id = ai_tool_use_get_id(tool_use);
    JsonNode *input = ai_tool_use_get_input(tool_use);

    g_print("\n[Tool: %s]\n", name);

    /* Execute tool and prepare result for next turn */
    g_autofree gchar *result = execute_my_tool(name, input);

    /* Store for sending back after stream completes */
    store_tool_result(id, result);
}

static void
on_stream_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_streamable_chat_stream_finish(
        AI_STREAMABLE(source), result, &error);

    if (ai_response_has_tool_use(response))
    {
        /* Send tool results back */
        send_tool_results_and_continue();
    }
}
```

## Cancellation

Cancel a stream in progress:

```c
g_autoptr(GCancellable) cancellable = g_cancellable_new();

ai_streamable_chat_stream_async(
    AI_STREAMABLE(client),
    messages,
    NULL,
    4096,
    NULL,
    cancellable,    /* pass cancellable */
    on_stream_complete,
    &data
);

/* Later, to cancel: */
g_cancellable_cancel(cancellable);
```

## Building a Simple Terminal Chat

```c
static gchar *accumulated_text = NULL;

static void
on_delta(AiStreamable *streamable, const gchar *text, gpointer user_data)
{
    g_print("%s", text);
    fflush(stdout);

    /* Accumulate for history */
    if (accumulated_text == NULL)
    {
        accumulated_text = g_strdup(text);
    }
    else
    {
        gchar *new_text = g_strconcat(accumulated_text, text, NULL);
        g_free(accumulated_text);
        accumulated_text = new_text;
    }
}

static void
on_stream_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
    /* Add assistant response to history */
    g_autoptr(AiMessage) assistant_msg =
        ai_message_new_assistant(accumulated_text);
    messages = g_list_append(messages, g_steal_pointer(&assistant_msg));

    g_clear_pointer(&accumulated_text, g_free);

    /* Prompt for next input */
    prompt_user();
}
```

## Building

```bash
gcc -o streaming-chat streaming-chat.c $(pkg-config --cflags --libs ai-glib-1.0)
./streaming-chat
```

## Output

```
User: Write a short poem about programming in C.