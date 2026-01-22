# Image Generation

Generate images from text prompts using AI providers.

## Overview

ai-glib supports image generation through the `AiImageGenerator` interface. Currently supported providers:

| Provider | Models | Notes |
|----------|--------|-------|
| OpenAI | DALL-E 2, DALL-E 3, GPT Image 1 | Full parameter support |
| Grok | grok-2-image | Basic generation only |
| Gemini | Nano Banana, Nano Banana Pro, Imagen 4 | Aspect ratio mapping |

## Quick Start

```c
#include <ai-glib.h>

/* Create client and request */
g_autoptr(AiOpenAIClient) client = ai_openai_client_new();
g_autoptr(AiImageRequest) request = ai_image_request_new("a sunset over mountains");

/* Generate image */
ai_image_generator_generate_image_async(
    AI_IMAGE_GENERATOR(client),
    request,
    NULL,
    on_complete,
    user_data
);
```

## Provider Examples

### OpenAI (DALL-E)

OpenAI supports the most comprehensive set of image generation parameters.

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

    /* Get the generated image */
    AiGeneratedImage *image = ai_image_response_get_image(response, 0);

    /* Check for revised prompt (DALL-E 3 may modify prompts) */
    const gchar *revised = ai_generated_image_get_revised_prompt(image);
    if (revised != NULL)
    {
        g_print("Revised prompt: %s\n", revised);
    }

    /* Save to file */
    if (!ai_generated_image_save_to_file(image, "dalle-output.png", &error))
    {
        g_printerr("Save failed: %s\n", error->message);
    }
    else
    {
        g_print("Image saved to dalle-output.png\n");
    }

    g_main_loop_quit(loop);
}

int main(int argc, char *argv[])
{
    g_autoptr(AiOpenAIClient) client = ai_openai_client_new();
    g_autoptr(AiImageRequest) request = ai_image_request_new(
        "a futuristic city at night with neon lights"
    );
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    /* Configure for best quality */
    ai_image_request_set_model(request, AI_OPENAI_IMAGE_MODEL_DALL_E_3);
    ai_image_request_set_size(request, AI_IMAGE_SIZE_1024);
    ai_image_request_set_quality(request, AI_IMAGE_QUALITY_HD);
    ai_image_request_set_style(request, AI_IMAGE_STYLE_VIVID);

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

**Environment:** `OPENAI_API_KEY`

### Grok

Grok provides basic image generation through an OpenAI-compatible API.

```c
#include <ai-glib.h>

int main(int argc, char *argv[])
{
    g_autoptr(AiGrokClient) client = ai_grok_client_new();
    g_autoptr(AiImageRequest) request = ai_image_request_new(
        "a robot playing chess"
    );
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    /* Grok ignores size/quality/style parameters */
    ai_image_request_set_model(request, AI_GROK_IMAGE_MODEL_GROK_2_IMAGE);

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

**Environment:** `XAI_API_KEY` or `GROK_API_KEY`

### Gemini (Nano Banana)

Gemini offers native image generation through "Nano Banana" (gemini-*-image models) and the legacy Imagen API.

**Nano Banana** is the recommended approach - it's Gemini's native image generation:

```c
#include <ai-glib.h>

int main(int argc, char *argv[])
{
    g_autoptr(AiGeminiClient) client = ai_gemini_client_new();
    g_autoptr(AiImageRequest) request = ai_image_request_new(
        "a cat wearing a space helmet floating in space"
    );
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    /* Use Nano Banana (native Gemini image generation) */
    ai_image_request_set_model(request, AI_GEMINI_IMAGE_MODEL_NANO_BANANA);

    /* Size maps to aspect ratio: 1024 = 1:1 square */
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

**Available Gemini image models:**

| Model | Define | Description |
|-------|--------|-------------|
| gemini-2.5-flash-image | `AI_GEMINI_IMAGE_MODEL_NANO_BANANA` | Fast, up to 1K (default) |
| gemini-3-pro-image-preview | `AI_GEMINI_IMAGE_MODEL_NANO_BANANA_PRO` | Pro, up to 4K |
| imagen-4.0-generate-001 | `AI_GEMINI_IMAGE_MODEL_IMAGEN_4` | Legacy Imagen |
| imagen-3.0-generate-001 | `AI_GEMINI_IMAGE_MODEL_IMAGEN_3` | Legacy Imagen |

**Environment:** `GEMINI_API_KEY`

## Parameter Support by Provider

| Parameter | OpenAI | Grok | Gemini |
|-----------|--------|------|--------|
| prompt | Yes | Yes | Yes |
| model | Yes | Yes | Yes |
| size | Yes (pixels) | No | Yes (aspect ratio) |
| quality | Yes (standard/hd) | No | No |
| style | Yes (vivid/natural) | No | No |
| count | Yes (1-10) | Yes | Yes |
| response_format | Yes (url/b64) | Yes | b64 only |

## Working with Generated Images

### Saving to File

```c
AiGeneratedImage *image = ai_image_response_get_image(response, 0);
g_autoptr(GError) error = NULL;

if (!ai_generated_image_save_to_file(image, "output.png", &error))
{
    g_printerr("Failed to save: %s\n", error->message);
}
```

### Getting Raw Bytes

```c
g_autoptr(GBytes) bytes = ai_generated_image_get_bytes(image, &error);
if (bytes != NULL)
{
    gsize size;
    gconstpointer data = g_bytes_get_data(bytes, &size);
    /* process raw image data */
}
```

### Checking Image Type

```c
if (ai_generated_image_is_url(image))
{
    const gchar *url = ai_generated_image_get_url(image);
    g_print("Image URL: %s\n", url);
}
else if (ai_generated_image_is_base64(image))
{
    const gchar *mime = ai_generated_image_get_mime_type(image);
    g_print("Got base64 image (%s)\n", mime);
}
```

### Multiple Images

```c
ai_image_request_set_count(request, 4);

/* ... generate ... */

guint count = ai_image_response_get_image_count(response);
for (guint i = 0; i < count; i++)
{
    AiGeneratedImage *img = ai_image_response_get_image(response, i);
    g_autofree gchar *filename = g_strdup_printf("output-%u.png", i);
    ai_generated_image_save_to_file(img, filename, NULL);
}
```

## Building the Examples

The library includes ready-to-run examples:

```bash
make

# Run with appropriate API key
OPENAI_API_KEY=sk-... ./build/examples/image-gen-openai "a mountain landscape"
XAI_API_KEY=xai-... ./build/examples/image-gen-grok "a robot"
GEMINI_API_KEY=AI... ./build/examples/image-gen-gemini "a cat in space"
```

## Error Handling

```c
response = ai_image_generator_generate_image_finish(
    AI_IMAGE_GENERATOR(source), result, &error);

if (error != NULL)
{
    if (error->domain == AI_ERROR)
    {
        switch (error->code)
        {
            case AI_ERROR_INVALID_API_KEY:
                g_printerr("Invalid API key\n");
                break;
            case AI_ERROR_RATE_LIMITED:
                g_printerr("Rate limited - try again later\n");
                break;
            case AI_ERROR_CONTENT_FILTER:
                g_printerr("Content was filtered\n");
                break;
            default:
                g_printerr("Error: %s\n", error->message);
                break;
        }
    }
}
```

## See Also

- [AiImageGenerator API Reference](../api-reference/ai-image-generator.md)
- [OpenAI Provider](../providers/openai.md)
- [Grok Provider](../providers/grok.md)
- [Gemini Provider](../providers/gemini.md)
