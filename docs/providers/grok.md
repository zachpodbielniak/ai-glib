# Grok Provider

xAI's Grok models, with real-time information access and strong reasoning capabilities.

## Configuration

### Environment Variables

| Variable | Description |
|----------|-------------|
| `XAI_API_KEY` | Primary API key (recommended) |
| `GROK_API_KEY` | Alternative API key |

### Creating a Client

```c
/* Using environment variable (recommended) */
g_autoptr(AiGrokClient) client = ai_grok_client_new();

/* Using explicit API key */
g_autoptr(AiGrokClient) client = ai_grok_client_new_with_key("xai-...");

/* Using configuration object */
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_api_key(config, AI_PROVIDER_GROK, "xai-...");
g_autoptr(AiGrokClient) client = ai_grok_client_new_with_config(config);
```

## Available Models

### Grok 4.1 Models (Latest)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_4_1_FAST_REASONING` | grok-4-1-fast-reasoning | Fast with reasoning (default) |
| `AI_GROK_MODEL_4_1_FAST_NON_REASONING` | grok-4-1-fast-non-reasoning | Fast without reasoning |

### Grok 4 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_4_0709` | grok-4-0709 | Grok 4 versioned |
| `AI_GROK_MODEL_4_FAST_REASONING` | grok-4-fast-reasoning | Fast with reasoning |
| `AI_GROK_MODEL_4_FAST_NON_REASONING` | grok-4-fast-non-reasoning | Fast without reasoning |

### Grok 3 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_3` | grok-3 | Grok 3 base |
| `AI_GROK_MODEL_3_MINI` | grok-3-mini | Compact Grok 3 |

### Grok 2 Models (Vision)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_2_VISION_1212` | grok-2-vision-1212 | Vision capabilities |
| `AI_GROK_MODEL_2_IMAGE_1212` | grok-2-image-1212 | Image understanding |

### Grok Code Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_CODE_FAST_1` | grok-code-fast-1 | Fast code generation |

### Convenience Aliases

| Define | Points To | Description |
|--------|-----------|-------------|
| `AI_GROK_MODEL_LATEST` | `AI_GROK_MODEL_4_1_FAST_REASONING` | Latest model |
| `AI_GROK_MODEL_FAST` | `AI_GROK_MODEL_4_1_FAST_NON_REASONING` | Fastest model |
| `AI_GROK_MODEL_CODE` | `AI_GROK_MODEL_CODE_FAST_1` | Code model |

### Setting the Model

```c
g_autoptr(AiGrokClient) client = ai_grok_client_new();

/* Use Grok 4.1 for best performance */
ai_client_set_model(AI_CLIENT(client), AI_GROK_MODEL_4_1_FAST_REASONING);

/* Or use the fast non-reasoning model */
ai_client_set_model(AI_CLIENT(client), AI_GROK_MODEL_FAST);

/* Or use the code model */
ai_client_set_model(AI_CLIENT(client), AI_GROK_MODEL_CODE);
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
    g_print("Grok: %s\n", text);

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
    g_autoptr(AiGrokClient) client = ai_grok_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("What's happening in tech today?");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    ai_provider_chat_async(AI_PROVIDER(client), messages,
                           NULL, 4096, NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## API Compatibility

Grok uses an OpenAI-compatible API:

- **Endpoint**: `https://api.x.ai/v1/chat/completions`
- **Authentication**: Bearer token
- **Request Format**: OpenAI-compatible messages format
- **Response Format**: OpenAI-compatible choices format

## Context Windows

| Model Family | Context Window | Max Output |
|--------------|----------------|------------|
| Grok 4.1 | 256K tokens | 4K tokens |
| Grok 4 | 256K tokens | 4K tokens |
| Grok 3 | 1M tokens | 4K tokens |
| Grok 2 | 131K tokens | 4K tokens |

## Features

- **Chat Completion**: Full support
- **Streaming**: Full support via `AiStreamable` interface
- **Tool Use**: Full support for function calling
- **Vision**: Supported on vision models
- **Real-time Info**: Access to X (Twitter) data
- **System Prompts**: Full support

## Links

- [xAI API Documentation](https://docs.x.ai/)
- [xAI Console](https://console.x.ai/)
