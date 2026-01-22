# OpenCode Provider

The OpenCode provider (`AiOpenCodeClient`) is a wrapper around the `opencode` CLI tool. It provides access to multiple AI providers (Anthropic, OpenAI, Google) through a unified command-line interface.

## Overview

| Property | Value |
|----------|-------|
| Type | `AI_PROVIDER_OPENCODE` |
| Name | "OpenCode" |
| Default Model | `anthropic/claude-sonnet-4-20250514` |
| Interface | CLI (subprocess) |

## Requirements

- The `opencode` CLI must be installed and configured
- The CLI must be available in `$PATH` or specified via environment variable
- API keys for the desired providers must be configured in OpenCode

## Installation

Install the OpenCode CLI following its official documentation.

## Environment Variables

| Variable | Description |
|----------|-------------|
| `OPENCODE_PATH` | Override the path to the `opencode` executable |

## Available Models

OpenCode supports models from multiple providers:

### Anthropic Models

| Define | Model ID |
|--------|----------|
| `AI_OPENCODE_MODEL_CLAUDE_SONNET_4` | `"anthropic/claude-sonnet-4-20250514"` |
| `AI_OPENCODE_MODEL_CLAUDE_OPUS_4` | `"anthropic/claude-opus-4-20250514"` |
| `AI_OPENCODE_MODEL_CLAUDE_OPUS_4_5` | `"anthropic/claude-opus-4-5-20251101"` |
| `AI_OPENCODE_MODEL_CLAUDE_HAIKU` | `"anthropic/claude-3-5-haiku-20241022"` |

### OpenAI Models

| Define | Model ID |
|--------|----------|
| `AI_OPENCODE_MODEL_GPT_4O` | `"openai/gpt-4o"` |
| `AI_OPENCODE_MODEL_GPT_4O_MINI` | `"openai/gpt-4o-mini"` |
| `AI_OPENCODE_MODEL_O3` | `"openai/o3"` |
| `AI_OPENCODE_MODEL_O3_MINI` | `"openai/o3-mini"` |

### Google Models

| Define | Model ID |
|--------|----------|
| `AI_OPENCODE_MODEL_GEMINI_2_FLASH` | `"google/gemini-2.0-flash"` |
| `AI_OPENCODE_MODEL_GEMINI_2_5_PRO` | `"google/gemini-2.5-pro-preview-05-06"` |

## Usage

### Basic Usage

```c
#include <ai-glib.h>

int main(void)
{
    g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
    GList *messages = g_list_append(NULL, msg);

    /* Async chat request */
    ai_provider_chat_async(
        AI_PROVIDER(client),
        messages,
        NULL,  /* system prompt */
        4096,  /* max tokens */
        NULL,  /* tools */
        NULL,  /* cancellable */
        on_chat_complete,
        user_data
    );

    g_list_free(messages);
    return 0;
}
```

### Switching Providers

One of the key features of OpenCode is the ability to switch between providers easily:

```c
g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();

/* Use Anthropic Claude */
ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_CLAUDE_SONNET_4);

/* Switch to OpenAI GPT-4o */
ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_GPT_4O);

/* Switch to Google Gemini */
ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_OPENCODE_MODEL_GEMINI_2_FLASH);
```

### Streaming

OpenCode supports streaming responses:

```c
g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();

/* Connect to streaming signals */
g_signal_connect(client, "stream-start", G_CALLBACK(on_stream_start), NULL);
g_signal_connect(client, "delta", G_CALLBACK(on_delta), NULL);
g_signal_connect(client, "stream-end", G_CALLBACK(on_stream_end), NULL);

/* Start streaming chat */
ai_streamable_chat_stream_async(
    AI_STREAMABLE(client),
    messages,
    system_prompt,
    max_tokens,
    NULL,  /* tools */
    NULL,  /* cancellable */
    on_stream_complete,
    user_data
);
```

### Custom Executable Path

```c
g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();

/* Set custom executable path */
ai_cli_client_set_executable_path(AI_CLI_CLIENT(client), "/opt/opencode/bin/opencode");
```

## CLI Invocation Details

### Non-Streaming

```bash
opencode run --format json --model anthropic/claude-sonnet-4 "prompt"
```

### Streaming

```bash
opencode run --format stream-json --model anthropic/claude-sonnet-4 "prompt"
```

## Response Format

The OpenCode CLI returns JSON responses. The exact format may vary by provider, but generally includes:

```json
{
    "type": "result",
    "content": "response text",
    "usage": {
        "input_tokens": 100,
        "output_tokens": 50
    }
}
```

## Multi-Provider Comparison

Use OpenCode to easily compare responses from different providers:

```c
static void
compare_providers(const gchar *prompt)
{
    const gchar *models[] = {
        AI_OPENCODE_MODEL_CLAUDE_SONNET_4,
        AI_OPENCODE_MODEL_GPT_4O,
        AI_OPENCODE_MODEL_GEMINI_2_FLASH,
        NULL
    };

    for (int i = 0; models[i] != NULL; i++)
    {
        g_autoptr(AiOpenCodeClient) client = ai_opencode_client_new();
        ai_cli_client_set_model(AI_CLI_CLIENT(client), models[i]);

        g_print("Testing model: %s\n", models[i]);
        /* Run chat and compare responses... */
    }
}
```

## Error Handling

```c
g_autoptr(GError) error = NULL;
g_autoptr(AiResponse) response = ai_cli_client_chat_sync(
    AI_CLI_CLIENT(client), messages, NULL, &error);

if (error != NULL)
{
    switch (error->code)
    {
        case AI_ERROR_CLI_NOT_FOUND:
            g_printerr("OpenCode CLI not found. Install it or set OPENCODE_PATH.\n");
            break;
        case AI_ERROR_CLI_EXECUTION:
            g_printerr("CLI execution failed: %s\n", error->message);
            break;
        case AI_ERROR_CLI_PARSE_ERROR:
            g_printerr("Failed to parse CLI output: %s\n", error->message);
            break;
        default:
            g_printerr("Error: %s\n", error->message);
            break;
    }
}
```

## Comparison with HTTP API Providers

| Feature | HTTP Providers | OpenCode CLI |
|---------|----------------|--------------|
| Multiple providers | Separate clients | Single client |
| Authentication | Per-provider API keys | Centralized config |
| Performance | Direct HTTP | Subprocess overhead |
| Offline capability | No | Depends on CLI |
| Feature parity | Full | Limited to CLI features |

## When to Use OpenCode

- **Multi-provider testing**: Compare responses across different AI providers
- **Centralized configuration**: Manage API keys in one place (the OpenCode config)
- **CLI-first workflows**: When integrating with shell scripts or CLI tools
- **Provider abstraction**: Switch providers without code changes

## See Also

- [AiClaudeClient](claude.md) - Direct HTTP API client for Claude
- [AiOpenAIClient](openai.md) - Direct HTTP API client for OpenAI
- [AiGeminiClient](gemini.md) - Direct HTTP API client for Gemini
- [AiCliClient](../api-reference/ai-cli-client.md) - Base class for CLI providers
