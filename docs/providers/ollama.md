# Ollama Provider

Run AI models locally using Ollama. No API key required, models run on your own hardware.

## Prerequisites

1. Install Ollama: https://ollama.ai/
2. Start the Ollama server: `ollama serve`
3. Pull a model: `ollama pull llama3.2`

## Configuration

### Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `OLLAMA_HOST` | Server URL | `http://localhost:11434` |
| `OLLAMA_API_KEY` | API key (optional, for remote servers) | None |

### Creating a Client

```c
/* Using default localhost (recommended) */
g_autoptr(AiOllamaClient) client = ai_ollama_client_new();

/* Using custom host */
g_autoptr(AiOllamaClient) client = ai_ollama_client_new_with_host(
    "http://192.168.1.100:11434");

/* Using configuration object */
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, "http://remote:11434");
g_autoptr(AiOllamaClient) client = ai_ollama_client_new_with_config(config);
```

## Available Models

Models depend on what you have pulled locally. Common models:

### DeepSeek Models

| Define | Model ID | Size |
|--------|----------|------|
| `AI_OLLAMA_MODEL_DEEPSEEK_R1_32B` | deepseek-r1:32b | 19 GB |
| `AI_OLLAMA_MODEL_DEEPSEEK_R1_14B` | deepseek-r1:14b | 9 GB |
| `AI_OLLAMA_MODEL_DEEPSEEK_R1_8B` | deepseek-r1:8b | 5.2 GB |
| `AI_OLLAMA_MODEL_DEEPSEEK_R1_1_5B` | deepseek-r1:1.5b | 1.1 GB |

### Llama Models

| Define | Model ID | Size |
|--------|----------|------|
| `AI_OLLAMA_MODEL_LLAMA3_1_8B` | llama3.1:8b | 4.9 GB |
| `AI_OLLAMA_MODEL_LLAMA3_2` | llama3.2 | ~2 GB |

### Gemma Models

| Define | Model ID | Size |
|--------|----------|------|
| `AI_OLLAMA_MODEL_GEMMA3_27B` | gemma3:27b | 17 GB |
| `AI_OLLAMA_MODEL_GEMMA3_12B` | gemma3:12b | 8.1 GB |
| `AI_OLLAMA_MODEL_GEMMA3_4B` | gemma3:4b | 3.3 GB |

### Other Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OLLAMA_MODEL_DOLPHIN_MIXTRAL` | dolphin-mixtral:8x7b | Uncensored Mixtral |
| `AI_OLLAMA_MODEL_DOLPHIN3_8B` | dolphin3:8b | Dolphin 3 |
| `AI_OLLAMA_MODEL_FALCON3_10B` | falcon3:10b | Falcon 3 |
| `AI_OLLAMA_MODEL_TINYLLAMA` | tinyllama:1.1b | Tiny, fast |
| `AI_OLLAMA_MODEL_GPT_OSS_20B` | gpt-oss:20b | Custom model (default) |

### Embedding Models

| Define | Model ID | Description |
|--------|----------|-------------|
| `AI_OLLAMA_MODEL_NOMIC_EMBED` | nomic-embed-text:v1.5 | Text embeddings |

### Setting the Model

```c
g_autoptr(AiOllamaClient) client = ai_ollama_client_new();

/* Use DeepSeek for reasoning tasks */
ai_client_set_model(AI_CLIENT(client), AI_OLLAMA_MODEL_DEEPSEEK_R1_14B);

/* Or use any model you have pulled */
ai_client_set_model(AI_CLIENT(client), "codellama:13b");
```

## Listing Available Models

Check what models you have:

```bash
ollama list
```

Pull a new model:

```bash
ollama pull llama3.2
ollama pull deepseek-r1:14b
ollama pull gemma3:12b
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
    g_print("Ollama: %s\n", text);

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
    g_autoptr(AiOllamaClient) client = ai_ollama_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Write a simple Python function.");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    /* Use a coding-focused model */
    ai_client_set_model(AI_CLIENT(client), "deepseek-r1:14b");

    ai_provider_chat_async(AI_PROVIDER(client), messages,
                           NULL, 4096, NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## API Format

Ollama uses its own API format:

- **Endpoint**: `http://localhost:11434/api/chat`
- **Authentication**: None required locally
- **Request Format**: Ollama-specific messages format
- **Response Format**: Ollama-specific format with `message` object

## Performance Tips

1. **GPU Acceleration**: Ollama automatically uses GPU if available
2. **Memory**: Larger models need more RAM/VRAM
3. **Quantization**: Use quantized models (Q4, Q5) for less memory
4. **Context Length**: Some models support longer contexts than others

## Troubleshooting

### Connection Refused

```
Error: Request failed (HTTP 0)
```

Make sure Ollama is running:

```bash
ollama serve
```

### Model Not Found

```
Error: model 'xyz' not found
```

Pull the model first:

```bash
ollama pull xyz
```

### Out of Memory

Use a smaller model or quantized version:

```bash
ollama pull llama3.2:1b    # Smaller
ollama pull llama3.2:q4    # Quantized
```

## Features

- **Chat Completion**: Full support
- **Streaming**: Full support via `AiStreamable` interface
- **Tool Use**: Model-dependent
- **Vision**: Supported on multimodal models (llava, etc.)
- **System Prompts**: Full support
- **No API Key**: Runs locally, completely private

## Links

- [Ollama Website](https://ollama.ai/)
- [Ollama GitHub](https://github.com/ollama/ollama)
- [Ollama Model Library](https://ollama.ai/library)
