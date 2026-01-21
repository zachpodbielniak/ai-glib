# Multi-Provider Example

Using multiple AI providers in a single application.

## Overview

This example demonstrates:
- Creating clients for multiple providers
- Using the common `AiProvider` interface
- Comparing responses across providers
- Abstracting provider selection

## Prerequisites

Set API keys for the providers you want to use:

```bash
export ANTHROPIC_API_KEY="your-key"
export OPENAI_API_KEY="your-key"
export GEMINI_API_KEY="your-key"
export XAI_API_KEY="your-key"
# Ollama: just ensure the server is running
```

## Code

```c
/*
 * multi-provider.c - Using multiple AI providers
 *
 * Build: gcc -o multi-provider multi-provider.c $(pkg-config --cflags --libs ai-glib-1.0)
 */

#include <ai-glib.h>

typedef struct {
    GMainLoop     *loop;
    const gchar   *provider_name;
    gint          *pending;
} CallbackData;

static void
on_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    CallbackData *data = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

    g_print("\n=== %s ===\n", data->provider_name);

    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
    }
    else
    {
        g_autofree gchar *text = ai_response_get_text(response);
        g_print("%s\n", text);

        const AiUsage *usage = ai_response_get_usage(response);
        if (usage != NULL)
        {
            g_print("[Tokens: %d in, %d out]\n",
                    ai_usage_get_input_tokens(usage),
                    ai_usage_get_output_tokens(usage));
        }
    }

    /* Check if all providers have responded */
    (*data->pending)--;
    if (*data->pending == 0)
    {
        g_main_loop_quit(data->loop);
    }

    g_free(data);
}

static void
send_to_provider(
    AiProvider  *provider,
    const gchar *name,
    GList       *messages,
    GMainLoop   *loop,
    gint        *pending
){
    CallbackData *data = g_new0(CallbackData, 1);
    data->loop = loop;
    data->provider_name = name;
    data->pending = pending;

    ai_provider_chat_async(
        provider,
        messages,
        "You are a helpful assistant. Be concise.",
        1024,       /* max tokens */
        NULL,       /* tools */
        NULL,       /* cancellable */
        on_response,
        data
    );
}

int
main(
    int   argc,
    char *argv[]
){
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(AiMessage) msg = NULL;
    GList *messages = NULL;
    gint pending = 0;

    /* Create the question */
    msg = ai_message_new_user("Explain recursion in one sentence.");
    messages = g_list_append(NULL, msg);

    g_print("Question: Explain recursion in one sentence.\n");

    loop = g_main_loop_new(NULL, FALSE);

    /* Try each provider that has a key configured */

    /* Claude */
    if (g_getenv("ANTHROPIC_API_KEY") != NULL ||
        g_getenv("CLAUDE_API_KEY") != NULL)
    {
        g_autoptr(AiClaudeClient) claude = ai_claude_client_new();
        ai_client_set_model(AI_CLIENT(claude), AI_CLAUDE_MODEL_HAIKU);
        pending++;
        send_to_provider(AI_PROVIDER(claude), "Claude (Haiku)",
                         messages, loop, &pending);
    }

    /* OpenAI */
    if (g_getenv("OPENAI_API_KEY") != NULL)
    {
        g_autoptr(AiOpenAIClient) openai = ai_openai_client_new();
        ai_client_set_model(AI_CLIENT(openai), AI_OPENAI_MODEL_GPT4O_MINI);
        pending++;
        send_to_provider(AI_PROVIDER(openai), "OpenAI (GPT-4o-mini)",
                         messages, loop, &pending);
    }

    /* Gemini */
    if (g_getenv("GEMINI_API_KEY") != NULL)
    {
        g_autoptr(AiGeminiClient) gemini = ai_gemini_client_new();
        ai_client_set_model(AI_CLIENT(gemini), AI_GEMINI_MODEL_2_0_FLASH);
        pending++;
        send_to_provider(AI_PROVIDER(gemini), "Gemini (2.0 Flash)",
                         messages, loop, &pending);
    }

    /* Grok */
    if (g_getenv("XAI_API_KEY") != NULL ||
        g_getenv("GROK_API_KEY") != NULL)
    {
        g_autoptr(AiGrokClient) grok = ai_grok_client_new();
        ai_client_set_model(AI_CLIENT(grok), AI_GROK_MODEL_3_MINI_FAST_BETA);
        pending++;
        send_to_provider(AI_PROVIDER(grok), "Grok (3 Mini Fast)",
                         messages, loop, &pending);
    }

    /* Ollama (always try - it's local) */
    {
        g_autoptr(AiOllamaClient) ollama = ai_ollama_client_new();
        ai_client_set_model(AI_CLIENT(ollama), AI_OLLAMA_MODEL_LLAMA3_2);
        pending++;
        send_to_provider(AI_PROVIDER(ollama), "Ollama (Llama 3.2)",
                         messages, loop, &pending);
    }

    if (pending == 0)
    {
        g_printerr("No providers configured!\n");
        g_list_free(messages);
        return 1;
    }

    g_print("\nWaiting for %d providers...\n", pending);
    g_main_loop_run(loop);

    g_list_free(messages);
    return 0;
}
```

## Provider Abstraction

The key insight is that all clients implement `AiProvider`:

```c
/* All of these work identically through the interface */
AiProvider *provider;

provider = AI_PROVIDER(ai_claude_client_new());
provider = AI_PROVIDER(ai_openai_client_new());
provider = AI_PROVIDER(ai_gemini_client_new());
provider = AI_PROVIDER(ai_grok_client_new());
provider = AI_PROVIDER(ai_ollama_client_new());

/* Same API for all */
ai_provider_chat_async(provider, messages, system, max_tokens,
                       tools, cancellable, callback, user_data);
```

## Factory Pattern

Create providers dynamically:

```c
AiClient *
create_provider(AiProviderType type)
{
    switch (type)
    {
        case AI_PROVIDER_CLAUDE:
            return AI_CLIENT(ai_claude_client_new());
        case AI_PROVIDER_OPENAI:
            return AI_CLIENT(ai_openai_client_new());
        case AI_PROVIDER_GEMINI:
            return AI_CLIENT(ai_gemini_client_new());
        case AI_PROVIDER_GROK:
            return AI_CLIENT(ai_grok_client_new());
        case AI_PROVIDER_OLLAMA:
            return AI_CLIENT(ai_ollama_client_new());
        default:
            return NULL;
    }
}

/* Usage */
g_autoptr(AiClient) client = create_provider(AI_PROVIDER_CLAUDE);
ai_client_set_model(client, "claude-sonnet-4-20250514");

ai_provider_chat_async(AI_PROVIDER(client), messages, ...);
```

## Provider Selection by Name

```c
AiClient *
create_provider_by_name(const gchar *name)
{
    if (g_str_equal(name, "claude"))
        return AI_CLIENT(ai_claude_client_new());
    if (g_str_equal(name, "openai"))
        return AI_CLIENT(ai_openai_client_new());
    if (g_str_equal(name, "gemini"))
        return AI_CLIENT(ai_gemini_client_new());
    if (g_str_equal(name, "grok"))
        return AI_CLIENT(ai_grok_client_new());
    if (g_str_equal(name, "ollama"))
        return AI_CLIENT(ai_ollama_client_new());

    return NULL;
}

/* Usage from command line */
int main(int argc, char *argv[])
{
    const gchar *provider_name = argc > 1 ? argv[1] : "claude";

    g_autoptr(AiClient) client = create_provider_by_name(provider_name);
    if (client == NULL)
    {
        g_printerr("Unknown provider: %s\n", provider_name);
        return 1;
    }

    /* Use the client... */
}
```

## Fallback Pattern

Try multiple providers in order:

```c
static AiProvider *current_provider = NULL;
static GList *fallback_providers = NULL;

static void
try_next_provider(GList *messages, GMainLoop *loop)
{
    if (fallback_providers == NULL)
    {
        g_printerr("All providers failed!\n");
        g_main_loop_quit(loop);
        return;
    }

    current_provider = AI_PROVIDER(fallback_providers->data);
    fallback_providers = g_list_delete_link(fallback_providers, fallback_providers);

    g_print("Trying %s...\n", ai_provider_get_name(current_provider));

    ai_provider_chat_async(current_provider, messages, NULL, 4096,
                           NULL, NULL, on_fallback_response, loop);
}

static void
on_fallback_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

    if (error != NULL)
    {
        g_printerr("%s failed: %s\n",
                   ai_provider_get_name(AI_PROVIDER(source)),
                   error->message);
        try_next_provider(/* messages */, loop);
        return;
    }

    /* Success! */
    g_autofree gchar *text = ai_response_get_text(response);
    g_print("Response from %s:\n%s\n",
            ai_provider_get_name(AI_PROVIDER(source)), text);
    g_main_loop_quit(loop);
}
```

## Building

```bash
gcc -o multi-provider multi-provider.c $(pkg-config --cflags --libs ai-glib-1.0)
./multi-provider
```

## Output

```
Question: Explain recursion in one sentence.

Waiting for 5 providers...

=== Claude (Haiku) ===
Recursion is when a function calls itself to solve a problem by breaking it into smaller instances of the same problem.
[Tokens: 23 in, 28 out]

=== OpenAI (GPT-4o-mini) ===
Recursion is a programming technique where a function calls itself to solve smaller instances of a problem until reaching a base case.
[Tokens: 20 in, 30 out]

=== Gemini (2.0 Flash) ===
Recursion is when a function calls itself to solve a problem.
[Tokens: 18 in, 15 out]

=== Grok (3 Mini Fast) ===
Recursion is a function calling itself until it reaches a stopping condition.
[Tokens: 22 in, 16 out]

=== Ollama (Llama 3.2) ===
Recursion is a programming concept where a function calls itself repeatedly until it reaches a base case that stops the recursion.
[Tokens: 21 in, 29 out]
```

## See Also

- [Simple Chat](simple-chat.md) - Basic single-provider example
- [Providers](../providers/index.md) - Provider documentation
- [AiProvider](../api-reference/ai-provider.md) - Provider interface
