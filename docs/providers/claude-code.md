# Claude Code Provider

The Claude Code provider (`AiClaudeCodeClient`) is a wrapper around the `claude` CLI tool from Anthropic. It allows you to use Claude through the command-line interface instead of direct HTTP API calls.

## Overview

| Property | Value |
|----------|-------|
| Type | `AI_PROVIDER_CLAUDE_CODE` |
| Name | "Claude Code" |
| Default Model | `sonnet` |
| Interface | CLI (subprocess) |

## Requirements

- The `claude` CLI must be installed and configured
- The CLI must be available in `$PATH` or specified via environment variable

## Installation

Install the Claude CLI following the official instructions from Anthropic. The CLI is part of the Claude Code product.

## Environment Variables

| Variable | Description |
|----------|-------------|
| `CLAUDE_CODE_PATH` | Override the path to the `claude` executable |

## Available Models

The Claude Code CLI uses simplified model aliases:

| Define | Model Alias |
|--------|-------------|
| `AI_CLAUDE_CODE_MODEL_OPUS` | `"opus"` |
| `AI_CLAUDE_CODE_MODEL_SONNET` | `"sonnet"` |
| `AI_CLAUDE_CODE_MODEL_HAIKU` | `"haiku"` |
| `AI_CLAUDE_CODE_DEFAULT_MODEL` | `"sonnet"` |

## Usage

### Basic Usage

```c
#include <ai-glib.h>

int main(void)
{
    g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello, Claude!");
    GList *messages = g_list_append(NULL, msg);

    /* Async chat request */
    ai_provider_chat_async(
        AI_PROVIDER(client),
        messages,
        "You are a helpful assistant.",  /* system prompt */
        4096,                             /* max tokens */
        NULL,                             /* tools */
        NULL,                             /* cancellable */
        on_chat_complete,
        user_data
    );

    g_list_free(messages);
    return 0;
}
```

### Setting the Model

```c
g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();

/* Use Opus model */
ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_CLAUDE_CODE_MODEL_OPUS);

/* Or use Haiku for faster responses */
ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_CLAUDE_CODE_MODEL_HAIKU);
```

### Session Management

Claude Code supports session persistence for multi-turn conversations:

```c
g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();

/* Enable session persistence */
ai_cli_client_set_session_persistence(AI_CLI_CLIENT(client), TRUE);

/* After first request, a session ID will be set automatically */
/* You can retrieve it for debugging or logging */
const gchar *session_id = ai_cli_client_get_session_id(AI_CLI_CLIENT(client));

/* Or set a specific session ID to resume a conversation */
ai_cli_client_set_session_id(AI_CLI_CLIENT(client), "previous-session-id");
```

### Getting Cost Information

Claude Code provides cost information in its responses:

```c
static void
on_chat_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(AiResponse) response = ai_provider_chat_finish(
        AI_PROVIDER(source), result, NULL);

    /* Get the cost from the last request */
    gdouble cost = ai_claude_code_client_get_total_cost(
        AI_CLAUDE_CODE_CLIENT(source));
    g_print("Cost: $%.6f USD\n", cost);
}
```

### Streaming

Claude Code supports streaming responses:

```c
g_autoptr(AiClaudeCodeClient) client = ai_claude_code_client_new();

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

## CLI Invocation Details

### Non-Streaming

```bash
claude --print --output-format json \
    --model sonnet \
    --system-prompt "..." \
    --no-session-persistence \
    "user prompt"
```

### Streaming

```bash
claude --print --output-format stream-json --verbose \
    --model sonnet \
    "user prompt"
```

## Response Format

The Claude Code CLI returns JSON in the following format:

```json
{
    "type": "result",
    "result": "response text",
    "session_id": "uuid",
    "usage": {
        "input_tokens": 100,
        "output_tokens": 50
    },
    "total_cost_usd": 0.001
}
```

## Comparison with AiClaudeClient

| Feature | AiClaudeClient | AiClaudeCodeClient |
|---------|----------------|-------------------|
| Interface | HTTP API | CLI subprocess |
| Authentication | API key | CLI config |
| Session management | Manual | Built-in |
| Cost tracking | Via usage | Direct `total_cost_usd` |
| Tool use | Full support | Limited |
| Streaming | SSE | NDJSON |

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
            g_printerr("Claude CLI not found. Install it or set CLAUDE_CODE_PATH.\n");
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

## See Also

- [AiClaudeClient](claude.md) - HTTP API client for Claude
- [AiCliClient](../api-reference/ai-cli-client.md) - Base class for CLI providers
