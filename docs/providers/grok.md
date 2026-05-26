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

xAI changed its naming convention from dash-separated (`grok-4-1`) to
dot-separated (`grok-4.3`) starting with the 4.20 generation.

### Grok 4.3 Models (Latest)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_4_3` | grok-4.3 | Latest flagship (default) |

### Grok 4.20 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_4_20_REASONING` | grok-4.20-0309-reasoning | Reasoning tier |
| `AI_GROK_MODEL_4_20_NON_REASONING` | grok-4.20-0309-non-reasoning | Non-reasoning (fast alias) |
| `AI_GROK_MODEL_4_20_MULTI_AGENT` | grok-4.20-multi-agent-0309 | Multi-agent |

### Grok Build Models (Coding Agent CLI)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_BUILD_0_1` | grok-build-0.1 | Coding agent (code alias) |

### Grok 4.1 Models

Retired upstream 2026-05-15 — server redirects to `grok-4.3`.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_4_1_FAST_REASONING` | grok-4-1-fast-reasoning | Retired — redirects to grok-4.3 |
| `AI_GROK_MODEL_4_1_FAST_NON_REASONING` | grok-4-1-fast-non-reasoning | Retired — redirects to grok-4.3 |

### Grok 4 Models

Retired upstream 2026-05-15 — server redirects to `grok-4.3`.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_4_0709` | grok-4-0709 | Retired — redirects to grok-4.3 |
| `AI_GROK_MODEL_4_FAST_REASONING` | grok-4-fast-reasoning | Retired — redirects to grok-4.3 |
| `AI_GROK_MODEL_4_FAST_NON_REASONING` | grok-4-fast-non-reasoning | Retired — redirects to grok-4.3 |

### Grok 3 Models

Retired upstream 2026-05-15 — server redirects to `grok-4.3`.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_3` | grok-3 | Retired — redirects to grok-4.3 |
| `AI_GROK_MODEL_3_MINI` | grok-3-mini | Retired — redirects to grok-4.3 |

### Grok 2 Models (Vision)

Retired upstream.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_2_VISION_1212` | grok-2-vision-1212 | Retired upstream |
| `AI_GROK_MODEL_2_IMAGE_1212` | grok-2-image-1212 | Retired upstream |

### Grok Code Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_MODEL_CODE_FAST_1` | grok-code-fast-1 | Retired — use `AI_GROK_MODEL_BUILD_0_1` |

### Convenience Aliases

| Define | Points To | Description |
|--------|-----------|-------------|
| `AI_GROK_MODEL_LATEST` | `AI_GROK_MODEL_4_3` | Latest model |
| `AI_GROK_MODEL_FAST` | `AI_GROK_MODEL_4_20_NON_REASONING` | Fastest model |
| `AI_GROK_MODEL_CODE` | `AI_GROK_MODEL_BUILD_0_1` | Code model |

### Setting the Model

```c
g_autoptr(AiGrokClient) client = ai_grok_client_new();

/* Use Grok 4.3 for best performance */
ai_client_set_model(AI_CLIENT(client), AI_GROK_MODEL_4_3);

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
- **Image Generation**: Basic support via `AiImageGenerator` interface

## Tool Calling

Grok's API is OpenAI-compatible, so ai-glib uses the same wire-format
serializer as the OpenAI client. See the
[OpenAI Tool Calling section](openai.md#tool-calling) for the details — the
behavior is identical: tool definitions wrapped in `{type:"function", ...}`,
prior assistant tool calls round-tripped as `tool_calls`, and tool results
emitted as separate `role:"tool"` messages.

## Image Generation

Grok provides basic image generation through an OpenAI-compatible API.

### Image / Video Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GROK_IMAGE_MODEL_GROK_IMAGINE` | grok-imagine-image | Grok Imagine image (default) |
| `AI_GROK_IMAGE_MODEL_GROK_IMAGINE_QUALITY` | grok-imagine-image-quality | High-quality image |
| `AI_GROK_VIDEO_MODEL_GROK_IMAGINE` | grok-imagine-video | Grok Imagine video |
| `AI_GROK_IMAGE_MODEL_GROK_2_IMAGE` | grok-2-image | Retired upstream |

### Default Model

```c
#define AI_GROK_IMAGE_DEFAULT_MODEL  AI_GROK_IMAGE_MODEL_GROK_IMAGINE
```

### Supported Parameters

Grok's image generation is basic and ignores most parameters:

| Parameter | Support | Notes |
|-----------|---------|-------|
| prompt | Yes | Required |
| model | Yes | Only grok-2-image |
| size | No | Ignored |
| quality | No | Ignored |
| style | No | Ignored |
| count | Yes | Number of images |
| response_format | Yes | url or b64_json |

### Image Generation Example

```c
#include <ai-glib.h>

static void
on_image_complete(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_autoptr(AiImageResponse) response = NULL;
    g_autoptr(GError) error = NULL;

    response = ai_image_generator_generate_image_finish(
        AI_IMAGE_GENERATOR(source), result, &error);

    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        g_main_loop_quit(loop);
        return;
    }

    AiGeneratedImage *image = ai_image_response_get_image(response, 0);
    ai_generated_image_save_to_file(image, "grok-output.png", NULL);
    g_print("Image saved!\n");

    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiGrokClient) client = ai_grok_client_new();
    g_autoptr(AiImageRequest) request = ai_image_request_new(
        "a robot playing chess in a park"
    );
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    /* Model is optional - defaults to grok-imagine-image */
    ai_image_request_set_model(request, AI_GROK_IMAGE_MODEL_GROK_IMAGINE);

    ai_image_generator_generate_image_async(
        AI_IMAGE_GENERATOR(client),
        request,
        NULL,
        on_image_complete,
        loop
    );

    g_main_loop_run(loop);
    return 0;
}
```

### Notes

- Grok image generation uses an OpenAI-compatible endpoint
- Size, quality, and style parameters are accepted but ignored
- Images are typically returned as URLs

## Links

- [xAI API Documentation](https://docs.x.ai/)
- [xAI Console](https://console.x.ai/)
