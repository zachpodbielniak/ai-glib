# Claude Provider

Anthropic's Claude models, known for their strong reasoning and instruction-following capabilities.

## Configuration

### Environment Variables

| Variable | Description |
|----------|-------------|
| `ANTHROPIC_API_KEY` | Primary API key (recommended) |
| `CLAUDE_API_KEY` | Alternative API key |

### Creating a Client

```c
/* Using environment variable (recommended) */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();

/* Using explicit API key */
g_autoptr(AiClaudeClient) client = ai_claude_client_new_with_key("sk-ant-...");

/* Using configuration object */
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "sk-ant-...");
g_autoptr(AiClaudeClient) client = ai_claude_client_new_with_config(config);
```

## Available Models

### Claude 4.5 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_CLAUDE_MODEL_OPUS_4_5` | claude-opus-4-5-20251101 | High intelligence |
| `AI_CLAUDE_MODEL_SONNET_4_5` | claude-sonnet-4-5-20250929 | Balanced intelligence & speed |
| `AI_CLAUDE_MODEL_HAIKU_4_5` | claude-haiku-4-5-20251001 | Fast responses |

### Claude 4.1 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_CLAUDE_MODEL_OPUS_4_1` | claude-opus-4-1-20250805 | High intelligence |

### Claude 4 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_CLAUDE_MODEL_OPUS_4` | claude-opus-4-20250514 | High intelligence |
| `AI_CLAUDE_MODEL_SONNET_4` | claude-sonnet-4-20250514 | Balanced (default) |

### Claude 3.7 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_CLAUDE_MODEL_SONNET_3_7` | claude-3-7-sonnet-20250219 | Improved reasoning |

### Claude 3.5 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_CLAUDE_MODEL_HAIKU_3_5` | claude-3-5-haiku-20241022 | Fast responses |

### Claude 3 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_CLAUDE_MODEL_HAIKU_3` | claude-3-haiku-20240307 | Fast responses |

### Convenience Aliases

| Define | Points To | Description |
|--------|-----------|-------------|
| `AI_CLAUDE_MODEL_OPUS` | `AI_CLAUDE_MODEL_OPUS_4_5` | Latest Opus |
| `AI_CLAUDE_MODEL_SONNET` | `AI_CLAUDE_MODEL_SONNET_4` | Default Sonnet |
| `AI_CLAUDE_MODEL_HAIKU` | `AI_CLAUDE_MODEL_HAIKU_4_5` | Latest Haiku |

### Setting the Model

```c
g_autoptr(AiClaudeClient) client = ai_claude_client_new();

/* Use the most capable model */
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_OPUS_4);

/* Or use a string */
ai_client_set_model(AI_CLIENT(client), "claude-3-5-haiku-20241022");
```

## API Version

Claude uses a versioned API. The default version is `2023-06-01`.

```c
/* Get current API version */
const gchar *version = ai_claude_client_get_api_version(client);

/* Set a different API version */
ai_claude_client_set_api_version(client, "2024-01-01");
```

## Example

```c
#include <ai-glib.h>

static void
on_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        g_main_loop_quit(loop);
        return;
    }

    g_autofree gchar *text = ai_response_get_text(response);
    g_print("Claude: %s\n", text);

    const AiUsage *usage = ai_response_get_usage(response);
    if (usage != NULL)
    {
        g_print("Tokens: %d in, %d out\n",
                ai_usage_get_input_tokens(usage),
                ai_usage_get_output_tokens(usage));
    }

    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiClaudeClient) client = ai_claude_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Explain quantum computing briefly.");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    /* Use Opus for best results */
    ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_OPUS_4);

    ai_provider_chat_async(AI_PROVIDER(client), messages,
                           "You are a helpful assistant.",
                           4096, NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## Features

- **Chat Completion**: Full support
- **Streaming**: Full support via `AiStreamable` interface
- **Tool Use**: Full support for function calling
- **Vision**: Supported on all Claude 3+ models
- **System Prompts**: Full support

## Rate Limits

Claude API has rate limits based on your plan tier. The library will return `AI_ERROR_RATE_LIMITED` when limits are exceeded.

## Links

- [Anthropic API Documentation](https://docs.anthropic.com/)
- [Claude Models](https://docs.anthropic.com/en/docs/about-claude/models)
