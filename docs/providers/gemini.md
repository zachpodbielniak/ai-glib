# Gemini Provider

Google's Gemini models, offering strong multimodal capabilities.

## Configuration

### Environment Variables

| Variable | Description |
|----------|-------------|
| `GEMINI_API_KEY` | Google AI API key (required) |

### Creating a Client

```c
/* Using environment variable (recommended) */
g_autoptr(AiGeminiClient) client = ai_gemini_client_new();

/* Using explicit API key */
g_autoptr(AiGeminiClient) client = ai_gemini_client_new_with_key("AI...");

/* Using configuration object */
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_api_key(config, AI_PROVIDER_GEMINI, "AI...");
g_autoptr(AiGeminiClient) client = ai_gemini_client_new_with_config(config);
```

## Available Models

### Gemini 3.5 Models (Latest Stable Flash)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_3_5_FLASH` | gemini-3.5-flash | Latest stable Flash (default) |

### Gemini 3.1 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_3_1_PRO_PREVIEW` | gemini-3.1-pro-preview | Latest Pro (preview) |
| `AI_GEMINI_MODEL_3_1_FLASH_LITE` | gemini-3.1-flash-lite | Stable Flash-Lite |
| `AI_GEMINI_MODEL_3_1_FLASH_LITE_PREVIEW` | gemini-3.1-flash-lite-preview | Flash-Lite preview |

### Gemini 3 Models (Preview)

`gemini-3-pro-preview` was retired upstream on 2026-03-09; migrate to
`AI_GEMINI_MODEL_3_1_PRO_PREVIEW`.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_3_FLASH_PREVIEW` | gemini-3-flash-preview | Fast text generation |
| `AI_GEMINI_MODEL_3_PRO_PREVIEW` | gemini-3-pro-preview | Retired upstream — use 3.1 Pro |

### Gemini 2.5 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_2_5_FLASH` | gemini-2.5-flash | Fast |
| `AI_GEMINI_MODEL_2_5_FLASH_LITE` | gemini-2.5-flash-lite | Lightweight |
| `AI_GEMINI_MODEL_2_5_FLASH_LITE_PREVIEW` | gemini-2.5-flash-lite-preview-09-2025 | Lite preview |
| `AI_GEMINI_MODEL_2_5_FLASH_PREVIEW` | gemini-2.5-flash-preview-09-2025 | Flash preview |
| `AI_GEMINI_MODEL_2_5_PRO` | gemini-2.5-pro | Professional |

### Gemini 2.0 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_2_0_FLASH` | gemini-2.0-flash | Fast and capable |
| `AI_GEMINI_MODEL_2_0_FLASH_001` | gemini-2.0-flash-001 | Versioned |
| `AI_GEMINI_MODEL_2_0_FLASH_EXP` | gemini-2.0-flash-exp | Experimental |
| `AI_GEMINI_MODEL_2_0_FLASH_LITE` | gemini-2.0-flash-lite | Lightweight |
| `AI_GEMINI_MODEL_2_0_FLASH_LITE_001` | gemini-2.0-flash-lite-001 | Lite versioned |
| `AI_GEMINI_MODEL_2_0_FLASH_LITE_PREVIEW` | gemini-2.0-flash-lite-preview | Lite preview |

### Latest Aliases

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_FLASH_LATEST` | gemini-flash-latest | Latest flash |
| `AI_GEMINI_MODEL_FLASH_LITE_LATEST` | gemini-flash-lite-latest | Latest lite |
| `AI_GEMINI_MODEL_PRO_LATEST` | gemini-pro-latest | Latest pro |

### Experimental & Special Purpose

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_EXP_1206` | gemini-exp-1206 | Experimental |
| `AI_GEMINI_MODEL_DEEP_RESEARCH` | deep-research-pro-preview-12-2025 | Deep research |

### Gemma 3 Models (via Gemini API)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_MODEL_GEMMA_3_27B` | gemma-3-27b-it | 27B parameters |
| `AI_GEMINI_MODEL_GEMMA_3_12B` | gemma-3-12b-it | 12B parameters |
| `AI_GEMINI_MODEL_GEMMA_3_4B` | gemma-3-4b-it | 4B parameters |
| `AI_GEMINI_MODEL_GEMMA_3_1B` | gemma-3-1b-it | 1B parameters |

### Convenience Aliases

| Define | Points To | Description |
|--------|-----------|-------------|
| `AI_GEMINI_MODEL_FLASH` | `AI_GEMINI_MODEL_3_5_FLASH` | Latest stable flash |
| `AI_GEMINI_MODEL_PRO` | `AI_GEMINI_MODEL_3_1_PRO_PREVIEW` | Latest pro (preview) |

### Live / Audio / TTS Models

Identifiers only. `AiGeminiClient` does not yet expose Live,
native-audio, or TTS endpoints; these constants exist so callers can
reference upstream IDs by symbolic name once support lands.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_LIVE_MODEL_3_1_FLASH` | gemini-3.1-flash-live-preview | Live (3.1 Flash) |
| `AI_GEMINI_LIVE_MODEL_2_5_FLASH_NATIVE_AUDIO` | gemini-2.5-flash-native-audio-preview-12-2025 | Live (2.5 native audio) |
| `AI_GEMINI_TTS_MODEL_3_1_FLASH` | gemini-3.1-flash-tts-preview | TTS (3.1 Flash) |
| `AI_GEMINI_TTS_MODEL_2_5_FLASH` | gemini-2.5-flash-preview-tts | TTS (2.5 Flash) |
| `AI_GEMINI_TTS_MODEL_2_5_PRO` | gemini-2.5-pro-preview-tts | TTS (2.5 Pro) |

### Video Generation Models (Veo)

Identifiers only. `AiGeminiClient` does not yet expose Veo endpoints.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_VIDEO_MODEL_VEO_3_1` | veo-3.1-generate-preview | Veo 3.1 |
| `AI_GEMINI_VIDEO_MODEL_VEO_3_1_LITE` | veo-3.1-lite-generate-preview | Veo 3.1 Lite |

### Embedding Models

Identifiers only. `AiGeminiClient` does not yet expose embeddings.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_EMBEDDING_MODEL_2` | gemini-embedding-2 | Gemini Embedding 2 |
| `AI_GEMINI_EMBEDDING_MODEL_001` | gemini-embedding-001 | Gemini Embedding (001) |

### Setting the Model

```c
g_autoptr(AiGeminiClient) client = ai_gemini_client_new();

/* Use the most capable model */
ai_client_set_model(AI_CLIENT(client), AI_GEMINI_MODEL_3_1_PRO_PREVIEW);

/* Or use the latest stable flash for speed */
ai_client_set_model(AI_CLIENT(client), AI_GEMINI_MODEL_3_5_FLASH);

/* Or try the 3.1 Flash-Lite preview */
ai_client_set_model(AI_CLIENT(client), AI_GEMINI_MODEL_3_1_FLASH_LITE);
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
    g_print("Gemini: %s\n", text);

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
    g_autoptr(AiGeminiClient) client = ai_gemini_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Explain machine learning.");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    ai_provider_chat_async(AI_PROVIDER(client), messages,
                           NULL, 4096, NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## API Differences

Gemini uses a different API format than OpenAI-compatible providers:

- **Endpoint**: Model is included in the URL path
- **Authentication**: API key passed as query parameter
- **Request Format**: Uses `contents` array instead of `messages`
- **Response Format**: Uses `candidates` array

The ai-glib library handles these differences transparently.

## Features

- **Chat Completion**: Full support
- **Streaming**: Full support via `AiStreamable` interface
- **Tool Use**: Full support (function declarations, function calls, function
  responses, including streaming)
- **Vision**: Full multimodal support
- **System Prompts**: Supported via system instruction

## Tool Calling

Gemini uses a wire format distinct from OpenAI / Anthropic:

* Tool definitions are wrapped in `tools:[{functionDeclarations:[...]}]`.
* Assistant tool calls arrive as `parts:[{functionCall:{name, args}}]` inside
  a `role:"model"` content item. `args` is a parsed JSON object, not a
  string.
* Tool results must be sent back as
  `parts:[{functionResponse:{name, response:{output|error}}}]` in a
  `role:"user"` content item. Crucially, Gemini keys the response by `name`,
  not by a call id.

Two things to know when calling tools through Gemini:

1. Use `ai_message_new_tool_result_with_name(id, tool_name, content, is_error)`
   when sending results back. The tool name is required on the wire. If you
   pass `NULL` for the name, ai-glib falls back to scanning earlier assistant
   messages for a matching `tool_use_id` — works for simple loops but is
   slower and less robust than just supplying it.
2. Gemini's API does not return tool-call IDs. ai-glib synthesizes a UUID
   per call so the multi-turn loop can match results back to calls
   internally. The synthetic ID is only used in-memory; on the wire only
   the tool name matters.

The `AiToolExecutor` convenience class handles both of these automatically.

## Context Windows

Gemini models offer large context windows:

| Model | Context Window |
|-------|----------------|
| Gemini 2.0 Flash | 1M tokens |
| Gemini 1.5 Pro | 1M tokens (2M available) |
| Gemini 1.5 Flash | 1M tokens |

## Image Generation

Gemini supports image generation through the `AiImageGenerator` interface. Two APIs are available:

### Nano Banana (Recommended)

Nano Banana is Google's native image generation built into Gemini models. It's faster and more integrated than Imagen.

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_IMAGE_MODEL_NANO_BANANA` | gemini-2.5-flash-image | Stable, up to 1K resolution (default) |
| `AI_GEMINI_IMAGE_MODEL_NANO_BANANA_2` | gemini-3.1-flash-image-preview | Preview, newer fast tier |
| `AI_GEMINI_IMAGE_MODEL_NANO_BANANA_PRO` | gemini-3-pro-image-preview | Pro, up to 4K resolution |

### Imagen (Legacy)

The original dedicated image generation models:

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_GEMINI_IMAGE_MODEL_IMAGEN_4` | imagen-4.0-generate-001 | Imagen 4 |
| `AI_GEMINI_IMAGE_MODEL_IMAGEN_3` | imagen-3.0-generate-001 | Imagen 3 |

### Default Model

```c
#define AI_GEMINI_IMAGE_DEFAULT_MODEL  AI_GEMINI_IMAGE_MODEL_NANO_BANANA
```

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
    ai_generated_image_save_to_file(image, "gemini-output.png", NULL);
    g_print("Image saved!\n");

    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiGeminiClient) client = ai_gemini_client_new();
    g_autoptr(AiImageRequest) request = ai_image_request_new(
        "a cat wearing a space helmet floating in space"
    );
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    /* Use Nano Banana for native Gemini image generation */
    ai_image_request_set_model(request, AI_GEMINI_IMAGE_MODEL_NANO_BANANA);

    /* Size maps to aspect ratio (1024 = 1:1 square) */
    ai_image_request_set_size(request, AI_IMAGE_SIZE_1024);

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

### Size to Aspect Ratio Mapping

Gemini uses aspect ratios instead of pixel dimensions:

| AiImageSize | Aspect Ratio |
|-------------|--------------|
| `AI_IMAGE_SIZE_1024` | 1:1 (square) |
| `AI_IMAGE_SIZE_1024_1792` | 9:16 (portrait) |
| `AI_IMAGE_SIZE_1792_1024` | 16:9 (landscape) |

### Notes

- Gemini image generation always returns base64-encoded images
- Quality and style parameters are ignored (not supported)
- The library automatically detects whether to use Nano Banana or Imagen API based on model name

## Links

- [Google AI Studio](https://aistudio.google.com/)
- [Gemini API Documentation](https://ai.google.dev/docs)
- [Gemini Models](https://ai.google.dev/models/gemini)
