# Simple Chat Example

A basic example demonstrating chat completion with ai-glib.

## Overview

This example shows how to:
- Create a provider client
- Build a message
- Send an async chat request
- Handle the response

## Prerequisites

Set your API key:

```bash
export ANTHROPIC_API_KEY="your-key"
# Or for other providers:
# export OPENAI_API_KEY="your-key"
# export GEMINI_API_KEY="your-key"
# export XAI_API_KEY="your-key"
```

## Code

```c
/*
 * simple-chat.c - Basic chat completion example
 *
 * Build: gcc -o simple-chat simple-chat.c $(pkg-config --cflags --libs ai-glib-1.0)
 * Run:   ./simple-chat
 */

#include <ai-glib.h>

static void
on_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    /* Finish the async operation */
    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        g_main_loop_quit(loop);
        return;
    }

    /* Extract and print the response text */
    g_autofree gchar *text = ai_response_get_text(response);
    g_print("Assistant: %s\n", text);

    /* Print usage statistics */
    const AiUsage *usage = ai_response_get_usage(response);
    if (usage != NULL)
    {
        g_print("\nTokens: %d input, %d output\n",
                ai_usage_get_input_tokens(usage),
                ai_usage_get_output_tokens(usage));
    }

    g_main_loop_quit(loop);
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

    /* Create the client - uses ANTHROPIC_API_KEY from environment */
    client = ai_claude_client_new();

    /* Optionally set a specific model */
    ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_SONNET);

    /* Create a user message */
    msg = ai_message_new_user("What is the capital of France?");
    messages = g_list_append(NULL, msg);

    /* Create main loop for async operation */
    loop = g_main_loop_new(NULL, FALSE);

    /* Send the request */
    g_print("User: What is the capital of France?\n\n");

    ai_provider_chat_async(
        AI_PROVIDER(client),
        messages,           /* conversation history */
        NULL,               /* system prompt (optional) */
        4096,               /* max tokens */
        NULL,               /* tools (optional) */
        NULL,               /* cancellable (optional) */
        on_response,        /* callback */
        loop                /* user data */
    );

    /* Run until response received */
    g_main_loop_run(loop);

    /* Clean up */
    g_list_free(messages);

    return 0;
}
```

## Step-by-Step Explanation

### 1. Create the Client

```c
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
```

This creates a Claude client that reads `ANTHROPIC_API_KEY` from the environment. For other providers:

```c
g_autoptr(AiOpenAIClient) client = ai_openai_client_new();
g_autoptr(AiGeminiClient) client = ai_gemini_client_new();
g_autoptr(AiGrokClient) client = ai_grok_client_new();
g_autoptr(AiOllamaClient) client = ai_ollama_client_new();
```

### 2. Configure the Model (Optional)

```c
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_SONNET);
```

Each provider has model defines. See [Provider Documentation](../providers/index.md) for available models.

### 3. Create Messages

```c
g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
GList *messages = g_list_append(NULL, msg);
```

Messages are passed as a `GList`. For multi-turn conversations, append more messages:

```c
messages = g_list_append(messages, ai_message_new_user("First question"));
messages = g_list_append(messages, ai_message_new_assistant("First answer"));
messages = g_list_append(messages, ai_message_new_user("Follow-up"));
```

### 4. Send the Request

```c
ai_provider_chat_async(
    AI_PROVIDER(client),
    messages,           /* conversation history */
    "You are helpful.", /* system prompt */
    4096,               /* max tokens */
    NULL,               /* tools */
    NULL,               /* cancellable */
    on_response,        /* callback */
    user_data           /* callback data */
);
```

### 5. Handle the Response

```c
static void
on_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return;
    }

    g_autofree gchar *text = ai_response_get_text(response);
    g_print("%s\n", text);
}
```

## With System Prompt

```c
ai_provider_chat_async(
    AI_PROVIDER(client),
    messages,
    "You are a helpful assistant that speaks like a pirate.",
    4096,
    NULL, NULL,
    on_response,
    loop
);
```

## Multi-Turn Conversation

```c
/* First turn */
GList *messages = NULL;
g_autoptr(AiMessage) user1 = ai_message_new_user("My name is Alice.");
messages = g_list_append(messages, user1);

/* ... get response ... */

/* Second turn - include history */
g_autoptr(AiMessage) assistant1 = ai_message_new_assistant("Hello Alice!");
g_autoptr(AiMessage) user2 = ai_message_new_user("What's my name?");
messages = g_list_append(messages, assistant1);
messages = g_list_append(messages, user2);

ai_provider_chat_async(AI_PROVIDER(client), messages, ...);
```

## Building

```bash
# Using pkg-config
gcc -o simple-chat simple-chat.c $(pkg-config --cflags --libs ai-glib-1.0)

# Run
./simple-chat
```

## Output

```
User: What is the capital of France?