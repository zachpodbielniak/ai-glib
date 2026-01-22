# AiImageGenerator

Interface for AI image generation providers.

## Overview

`AiImageGenerator` is a GObject interface that providers implement to support image generation. It provides an async API for generating images from text prompts.

Currently implemented by:
- `AiOpenAIClient` - OpenAI DALL-E models
- `AiGrokClient` - xAI Grok image models
- `AiGeminiClient` - Google Gemini Nano Banana and Imagen models

## Interface Methods

### generate_image_async

```c
void
ai_image_generator_generate_image_async(
    AiImageGenerator    *self,
    AiImageRequest      *request,
    GCancellable        *cancellable,
    GAsyncReadyCallback  callback,
    gpointer             user_data
);
```

Starts an asynchronous image generation request.

**Parameters:**
- `self` - The image generator instance
- `request` - An `AiImageRequest` with generation parameters
- `cancellable` - Optional `GCancellable` to cancel the operation
- `callback` - Callback function called when complete
- `user_data` - User data passed to the callback

### generate_image_finish

```c
AiImageResponse *
ai_image_generator_generate_image_finish(
    AiImageGenerator  *self,
    GAsyncResult      *result,
    GError           **error
);
```

Finishes an asynchronous image generation request.

**Parameters:**
- `self` - The image generator instance
- `result` - The `GAsyncResult` from the callback
- `error` - Return location for errors

**Returns:** `(transfer full)` An `AiImageResponse`, or `NULL` on error

### get_supported_sizes

```c
GList *
ai_image_generator_get_supported_sizes(AiImageGenerator *self);
```

Gets the list of supported image sizes for this provider.

**Returns:** `(transfer full) (element-type utf8)` List of size strings

### get_default_model

```c
const gchar *
ai_image_generator_get_default_model(AiImageGenerator *self);
```

Gets the default model for image generation.

**Returns:** `(transfer none)` The default model name

## Related Types

### AiImageRequest (Boxed Type)

Request parameters for image generation.

```c
/* Create a request */
g_autoptr(AiImageRequest) request = ai_image_request_new("a cat in space");

/* Configure options */
ai_image_request_set_model(request, "dall-e-3");
ai_image_request_set_size(request, AI_IMAGE_SIZE_1024);
ai_image_request_set_quality(request, AI_IMAGE_QUALITY_HD);
ai_image_request_set_style(request, AI_IMAGE_STYLE_VIVID);
ai_image_request_set_count(request, 1);
ai_image_request_set_response_format(request, AI_IMAGE_RESPONSE_BASE64);
```

**Properties:**
| Property | Type | Default | Description |
|----------|------|---------|-------------|
| prompt | string | (required) | Text description of the image |
| model | string | NULL | Model to use (NULL = provider default) |
| size | AiImageSize | AUTO | Image dimensions |
| custom_size | string | NULL | Custom size string |
| quality | AiImageQuality | AUTO | Quality level |
| style | AiImageStyle | AUTO | Generation style |
| count | int | 1 | Number of images to generate |
| response_format | AiImageResponseFormat | URL | How to return the image |
| user | string | NULL | User ID for abuse tracking |

### AiImageResponse (Boxed Type)

Response from an image generation request.

```c
/* Get response metadata */
const gchar *id = ai_image_response_get_id(response);
const gchar *model = ai_image_response_get_model(response);
gint64 created = ai_image_response_get_created(response);

/* Get images */
guint count = ai_image_response_get_image_count(response);
AiGeneratedImage *image = ai_image_response_get_image(response, 0);

/* Or iterate */
GList *images = ai_image_response_get_images(response);
for (GList *l = images; l != NULL; l = l->next)
{
    AiGeneratedImage *img = (AiGeneratedImage *)l->data;
    /* process image */
}
```

### AiGeneratedImage (Boxed Type)

A single generated image.

```c
/* Check image type */
if (ai_generated_image_is_url(image))
{
    const gchar *url = ai_generated_image_get_url(image);
}
else if (ai_generated_image_is_base64(image))
{
    const gchar *b64 = ai_generated_image_get_base64(image);
}

/* Get metadata */
const gchar *mime = ai_generated_image_get_mime_type(image);
const gchar *revised = ai_generated_image_get_revised_prompt(image);

/* Get raw bytes */
g_autoptr(GError) error = NULL;
g_autoptr(GBytes) bytes = ai_generated_image_get_bytes(image, &error);

/* Save to file */
ai_generated_image_save_to_file(image, "output.png", &error);
```

## Enumerations

### AiImageSize

```c
typedef enum {
    AI_IMAGE_SIZE_AUTO = 0,      /* Provider default */
    AI_IMAGE_SIZE_256,           /* 256x256 */
    AI_IMAGE_SIZE_512,           /* 512x512 */
    AI_IMAGE_SIZE_1024,          /* 1024x1024 */
    AI_IMAGE_SIZE_1024_1792,     /* 1024x1792 (portrait) */
    AI_IMAGE_SIZE_1792_1024,     /* 1792x1024 (landscape) */
    AI_IMAGE_SIZE_CUSTOM         /* Custom size string */
} AiImageSize;
```

### AiImageQuality

```c
typedef enum {
    AI_IMAGE_QUALITY_AUTO = 0,   /* Provider default */
    AI_IMAGE_QUALITY_STANDARD,   /* Standard quality */
    AI_IMAGE_QUALITY_HD          /* High definition */
} AiImageQuality;
```

### AiImageStyle

```c
typedef enum {
    AI_IMAGE_STYLE_AUTO = 0,     /* Provider default */
    AI_IMAGE_STYLE_VIVID,        /* Vivid, dramatic */
    AI_IMAGE_STYLE_NATURAL       /* Natural, realistic */
} AiImageStyle;
```

### AiImageResponseFormat

```c
typedef enum {
    AI_IMAGE_RESPONSE_URL = 0,   /* Return URL */
    AI_IMAGE_RESPONSE_BASE64     /* Return base64 data */
} AiImageResponseFormat;
```

## Provider Support Matrix

| Feature | OpenAI | Grok | Gemini (Nano Banana) | Gemini (Imagen) |
|---------|--------|------|----------------------|-----------------|
| Size | Yes | No | Aspect ratio | Aspect ratio |
| Quality | Yes | No | No | No |
| Style | Yes | No | No | No |
| Count | Yes | Yes | Yes | Yes |
| Revised Prompt | Yes | No | No | No |

## Example

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

    /* Get first image and save */
    AiGeneratedImage *image = ai_image_response_get_image(response, 0);
    if (!ai_generated_image_save_to_file(image, "output.png", &error))
    {
        g_printerr("Save failed: %s\n", error->message);
    }
    else
    {
        g_print("Image saved to output.png\n");
    }

    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiOpenAIClient) client = ai_openai_client_new();
    g_autoptr(AiImageRequest) request = ai_image_request_new("a cat in space");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);

    ai_image_request_set_model(request, AI_OPENAI_IMAGE_MODEL_DALL_E_3);
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

## See Also

- [Image Generation Example](../examples/image-generation.md)
- [OpenAI Provider](../providers/openai.md)
- [Grok Provider](../providers/grok.md)
- [Gemini Provider](../providers/gemini.md)
