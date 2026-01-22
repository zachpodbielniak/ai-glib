# OpenAI Provider

OpenAI's GPT models, including GPT-4o and the O-series reasoning models.

## Configuration

### Environment Variables

| Variable | Description |
|----------|-------------|
| `OPENAI_API_KEY` | API key (required) |
| `OPENAI_BASE_URL` | Custom base URL (optional, for Azure or proxies) |

### Creating a Client

```c
/* Using environment variable (recommended) */
g_autoptr(AiOpenAIClient) client = ai_openai_client_new();

/* Using explicit API key */
g_autoptr(AiOpenAIClient) client = ai_openai_client_new_with_key("sk-...");

/* Using configuration object */
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "sk-...");
g_autoptr(AiOpenAIClient) client = ai_openai_client_new_with_config(config);
```

## Available Models

### GPT-5.2 Models (Latest)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_5_2` | gpt-5.2 | Latest flagship |
| `AI_OPENAI_MODEL_GPT_5_2_PRO` | gpt-5.2-pro | Professional tier |
| `AI_OPENAI_MODEL_GPT_5_2_CODEX` | gpt-5.2-codex | Code generation |

### GPT-5.1 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_5_1` | gpt-5.1 | GPT-5.1 base |
| `AI_OPENAI_MODEL_GPT_5_1_CODEX` | gpt-5.1-codex | Code generation |
| `AI_OPENAI_MODEL_GPT_5_1_CODEX_MAX` | gpt-5.1-codex-max | Max code generation |
| `AI_OPENAI_MODEL_GPT_5_1_CODEX_MINI` | gpt-5.1-codex-mini | Mini code generation |

### GPT-5 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_5` | gpt-5 | GPT-5 base |
| `AI_OPENAI_MODEL_GPT_5_MINI` | gpt-5-mini | Smaller GPT-5 |
| `AI_OPENAI_MODEL_GPT_5_NANO` | gpt-5-nano | Smallest GPT-5 |
| `AI_OPENAI_MODEL_GPT_5_PRO` | gpt-5-pro | Professional tier |
| `AI_OPENAI_MODEL_GPT_5_CODEX` | gpt-5-codex | Code generation |

### GPT-4.1 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_4_1` | gpt-4.1 | GPT-4.1 base |
| `AI_OPENAI_MODEL_GPT_4_1_MINI` | gpt-4.1-mini | Mini version |
| `AI_OPENAI_MODEL_GPT_4_1_NANO` | gpt-4.1-nano | Nano version |

### GPT-4o Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_4O` | gpt-4o | Multimodal flagship (default) |
| `AI_OPENAI_MODEL_GPT_4O_MINI` | gpt-4o-mini | Fast and affordable |
| `AI_OPENAI_MODEL_CHATGPT_4O_LATEST` | chatgpt-4o-latest | Latest ChatGPT |

### GPT-4 Turbo Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_4_TURBO` | gpt-4-turbo | GPT-4 Turbo |
| `AI_OPENAI_MODEL_GPT_4_TURBO_PREVIEW` | gpt-4-turbo-preview | Turbo preview |

### GPT-4 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_4` | gpt-4 | Original GPT-4 |
| `AI_OPENAI_MODEL_GPT_4_0613` | gpt-4-0613 | Versioned |

### GPT-3.5 Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_GPT_3_5_TURBO` | gpt-3.5-turbo | Fast and affordable |
| `AI_OPENAI_MODEL_GPT_3_5_TURBO_16K` | gpt-3.5-turbo-16k | Extended context |
| `AI_OPENAI_MODEL_GPT_3_5_INSTRUCT` | gpt-3.5-turbo-instruct | Instruction following |

### O4 Series (Latest Reasoning)

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_O4_MINI` | o4-mini | O4 mini reasoning |
| `AI_OPENAI_MODEL_O4_MINI_DEEP_RESEARCH` | o4-mini-deep-research | Deep research |

### O3 Series

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_O3` | o3 | O3 reasoning |
| `AI_OPENAI_MODEL_O3_MINI` | o3-mini | O3 mini |

### O1 Series

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_MODEL_O1` | o1 | O1 reasoning |
| `AI_OPENAI_MODEL_O1_PRO` | o1-pro | O1 professional |

### Convenience Aliases

| Define | Points To | Description |
|--------|-----------|-------------|
| `AI_OPENAI_MODEL_LATEST` | `AI_OPENAI_MODEL_GPT_5_2` | Latest model |
| `AI_OPENAI_MODEL_FAST` | `AI_OPENAI_MODEL_GPT_4O_MINI` | Fast model |
| `AI_OPENAI_MODEL_REASONING` | `AI_OPENAI_MODEL_O3` | Reasoning model |

### Setting the Model

```c
g_autoptr(AiOpenAIClient) client = ai_openai_client_new();

/* Use GPT-4o mini for cost efficiency */
ai_client_set_model(AI_CLIENT(client), AI_OPENAI_MODEL_GPT_4O_MINI);

/* Or use GPT-5.2 for best performance */
ai_client_set_model(AI_CLIENT(client), AI_OPENAI_MODEL_GPT_5_2);

/* Or use a reasoning model */
ai_client_set_model(AI_CLIENT(client), AI_OPENAI_MODEL_O3);
```

## Custom Base URL

For Azure OpenAI or custom endpoints:

```c
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "your-key");
ai_config_set_base_url(config, AI_PROVIDER_OPENAI,
                       "https://your-resource.openai.azure.com");

g_autoptr(AiOpenAIClient) client = ai_openai_client_new_with_config(config);
```

Or via environment variable:

```bash
export OPENAI_BASE_URL="https://your-resource.openai.azure.com"
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
    g_print("GPT: %s\n", text);

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
    g_autoptr(AiOpenAIClient) client = ai_openai_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Write a haiku about programming.");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    ai_provider_chat_async(AI_PROVIDER(client), messages,
                           NULL, 4096, NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## Features

- **Chat Completion**: Full support
- **Streaming**: Full support via `AiStreamable` interface
- **Tool Use**: Full support for function calling
- **Vision**: Supported on GPT-4o and GPT-4-turbo
- **System Prompts**: Full support
- **Image Generation**: Full support via `AiImageGenerator` interface

## Image Generation

OpenAI provides comprehensive image generation through DALL-E models.

### Image Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OPENAI_IMAGE_MODEL_DALL_E_3` | dall-e-3 | Best quality (default) |
| `AI_OPENAI_IMAGE_MODEL_DALL_E_2` | dall-e-2 | Faster, lower cost |
| `AI_OPENAI_IMAGE_MODEL_GPT_IMAGE_1` | gpt-image-1 | GPT-based image model |

### Default Model

```c
#define AI_OPENAI_IMAGE_DEFAULT_MODEL  AI_OPENAI_IMAGE_MODEL_DALL_E_3
```

### Supported Parameters

OpenAI supports all image generation parameters:

| Parameter | Support | Notes |
|-----------|---------|-------|
| size | Yes | 256, 512, 1024, 1024x1792, 1792x1024 |
| quality | Yes | standard, hd |
| style | Yes | vivid, natural |
| count | Yes | 1-10 images |
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

    /* DALL-E 3 may revise the prompt for better results */
    const gchar *revised = ai_generated_image_get_revised_prompt(image);
    if (revised != NULL)
    {
        g_print("Revised prompt: %s\n", revised);
    }

    ai_generated_image_save_to_file(image, "dalle-output.png", NULL);
    g_print("Image saved!\n");

    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiOpenAIClient) client = ai_openai_client_new();
    g_autoptr(AiImageRequest) request = ai_image_request_new(
        "a futuristic city with flying cars at sunset"
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

### Notes

- DALL-E 3 automatically revises prompts for better results; the revised prompt is available via `ai_generated_image_get_revised_prompt()`
- DALL-E 3 only supports generating 1 image at a time; use DALL-E 2 for multiple images
- HD quality is only available with DALL-E 3

## Links

- [OpenAI API Documentation](https://platform.openai.com/docs/)
- [OpenAI Models](https://platform.openai.com/docs/models)
