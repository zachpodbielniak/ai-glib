# ai-glib Documentation

A GLib/GObject-based C library for interacting with AI providers.

## Overview

ai-glib provides a unified interface to multiple AI providers through a GObject-based design. It supports both synchronous and asynchronous operations, streaming responses, and tool/function calling.

## Supported Providers

| Provider | Description | Default Model |
|----------|-------------|---------------|
| [Claude](providers/claude.md) | Anthropic's Claude models | claude-sonnet-4-20250514 |
| [OpenAI](providers/openai.md) | OpenAI GPT models | gpt-4o |
| [Gemini](providers/gemini.md) | Google's Gemini models | gemini-2.0-flash |
| [Grok](providers/grok.md) | xAI's Grok models | grok-4-1-fast-reasoning |
| [Ollama](providers/ollama.md) | Local models via Ollama | gpt-oss:20b |

## Documentation

### Getting Started

- [Getting Started](getting-started.md) - Quick start guide and installation
- [Configuration](configuration.md) - Environment variables and configuration options
- [Architecture](architecture.md) - Design overview and class hierarchy

### Providers

- [Providers Overview](providers/index.md) - Common provider interface
- [Claude](providers/claude.md) - Anthropic Claude specifics
- [OpenAI](providers/openai.md) - OpenAI GPT specifics
- [Gemini](providers/gemini.md) - Google Gemini specifics
- [Grok](providers/grok.md) - xAI Grok specifics
- [Ollama](providers/ollama.md) - Local Ollama specifics

### API Reference

- [API Reference Index](api-reference/index.md) - Complete API documentation
- [AiSimple](api-reference/ai-simple.md) - Simple convenience API (start here)
- [AiClient](api-reference/ai-client.md) - Base client class
- [AiProvider](api-reference/ai-provider.md) - Provider interface
- [AiImageGenerator](api-reference/ai-image-generator.md) - Image generation interface
- [AiMessage](api-reference/ai-message.md) - Message class
- [AiResponse](api-reference/ai-response.md) - Response class
- [AiTool](api-reference/ai-tool.md) - Tool definition
- [AiConfig](api-reference/ai-config.md) - Configuration
- [AiError](api-reference/ai-error.md) - Error codes

### Examples

- [Examples Index](examples/index.md) - All examples
- [Simple Chat](examples/simple-chat.md) - Basic chat completion
- [Multi-Provider](examples/multi-provider.md) - Using multiple providers
- [Streaming](examples/streaming.md) - Streaming responses
- [Tool Use](examples/tool-use.md) - Function calling
- [Image Generation](examples/image-generation.md) - Generate images from text

### Contributing

- [Contributing](contributing.md) - How to contribute to ai-glib

## Quick Example (Simple API)

The simplest way to call an LLM — just 3 lines:

```c
#include <ai-glib.h>

int main(void)
{
    g_autoptr(AiSimple) ai = ai_simple_new();
    g_autofree gchar *answer = ai_simple_prompt(ai, "Hello!", NULL, NULL);
    g_print("Response: %s\n", answer);
    return 0;
}
```

## Full API Example

For async operations, streaming, and tool use, use the full provider API:

```c
#include <ai-glib.h>

static void
on_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;
    g_autofree gchar *text = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        g_main_loop_quit(loop);
        return;
    }

    text = ai_response_get_text(response);
    g_print("Response: %s\n", text);
    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiClaudeClient) client = ai_claude_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    ai_provider_chat_async(AI_PROVIDER(client), messages, NULL, 4096,
                           NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## License

ai-glib is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0).
