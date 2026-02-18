# AiSimple Usage

The simplest way to call an LLM from C using ai-glib.

## Overview

`AiSimple` wraps the full provider lifecycle into a few function calls.
No message lists, no async callbacks, no response parsing. Just prompt
and get text back.

## Prerequisites

Configure your default provider in `~/.config/ai-glib/config.yaml`:

```yaml
default_provider: ollama
default_model: qwen2.5:7b
```

Or set environment variables for your provider (e.g., `ANTHROPIC_API_KEY`).

## One-Shot Prompt

```c
/*
 * simple-prompt.c - Simplest possible LLM call
 *
 * Build: gcc -o simple-prompt simple-prompt.c $(pkg-config --cflags --libs ai-glib-1.0)
 * Run:   ./simple-prompt
 */

#include <ai-glib.h>

int
main(void)
{
    g_autoptr(AiSimple) ai = ai_simple_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *answer = NULL;

    answer = ai_simple_prompt(ai, "What is the capital of France?", NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }

    g_print("%s\n", answer);
    return 0;
}
```

## With System Prompt

```c
#include <ai-glib.h>

int
main(void)
{
    g_autoptr(AiSimple) ai = ai_simple_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *answer = NULL;

    ai_simple_set_system_prompt(ai,
        "You are a helpful assistant. Be concise.");

    answer = ai_simple_prompt(ai,
        "Explain what GObject is in one sentence.", NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }

    g_print("%s\n", answer);
    return 0;
}
```

## Multi-Turn Conversation

Use `ai_simple_chat()` instead of `ai_simple_prompt()` to maintain
conversation history:

```c
#include <ai-glib.h>

int
main(void)
{
    g_autoptr(AiSimple) ai = ai_simple_new();
    g_autoptr(GError) error = NULL;

    /* First turn */
    g_autofree gchar *r1 = ai_simple_chat(ai,
        "My name is Zach. Remember it.", NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }
    g_print("AI: %s\n\n", r1);

    /* Second turn - history is maintained */
    g_autofree gchar *r2 = ai_simple_chat(ai,
        "What's my name?", NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }
    g_print("AI: %s\n\n", r2);

    /* Clear history and start fresh */
    ai_simple_clear_history(ai);

    g_autofree gchar *r3 = ai_simple_chat(ai,
        "What's my name?", NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }
    g_print("AI (after clear): %s\n", r3);

    return 0;
}
```

## Explicit Provider

Skip the config file and specify a provider directly:

```c
#include <ai-glib.h>

int
main(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *answer = NULL;

    /* Use Claude with the default model */
    g_autoptr(AiSimple) ai = ai_simple_new_with_provider(
        AI_PROVIDER_CLAUDE, NULL);

    answer = ai_simple_prompt(ai, "Hello from Claude!", NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }

    g_print("%s\n", answer);
    return 0;
}
```

## Escape Hatch to Full API

When you need streaming, tool use, or async operations, get the
underlying provider and use the full ai-glib API:

```c
#include <ai-glib.h>

int
main(void)
{
    g_autoptr(AiSimple) ai = ai_simple_new_with_provider(
        AI_PROVIDER_OLLAMA, "tinyllama:1.1b");

    /* Get the underlying provider for advanced operations */
    AiProvider *provider = ai_simple_get_provider(ai);

    /* Now use the full async/streaming API */
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
    GList *messages = g_list_append(NULL, msg);
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    ai_provider_chat_async(provider, messages, NULL, 4096,
                           NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## prompt() vs chat()

| Method | History | Use Case |
|--------|---------|----------|
| `ai_simple_prompt()` | None (stateless) | One-off questions, scripts |
| `ai_simple_chat()` | Maintained | Multi-turn conversations |

## Building

```bash
gcc -o myapp myapp.c $(pkg-config --cflags --libs ai-glib-1.0)
```

## See Also

- [AiSimple API Reference](../api-reference/ai-simple.md) - Full API docs
- [Configuration](../configuration.md) - YAML config files and env vars
- [Simple Chat](simple-chat.md) - Full async chat example
