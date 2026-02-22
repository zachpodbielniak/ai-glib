# Providers Overview

ai-glib provides a unified interface to multiple AI providers through the `AiProvider` interface. All provider clients extend `AiClient` and implement `AiProvider` and `AiStreamable` interfaces.

## Supported Providers

### HTTP API Providers

| Provider | Client Class | Environment Variable | Default Model |
|----------|--------------|---------------------|---------------|
| [Claude](claude.md) | `AiClaudeClient` | `ANTHROPIC_API_KEY` or `CLAUDE_API_KEY` | claude-sonnet-4-20250514 |
| [OpenAI](openai.md) | `AiOpenAIClient` | `OPENAI_API_KEY` | gpt-4o |
| [Gemini](gemini.md) | `AiGeminiClient` | `GEMINI_API_KEY` | gemini-2.0-flash |
| [Grok](grok.md) | `AiGrokClient` | `XAI_API_KEY` or `GROK_API_KEY` | grok-4-1-fast-reasoning |
| [Ollama](ollama.md) | `AiOllamaClient` | `OLLAMA_HOST` (optional) | gpt-oss:20b |

### CLI Wrapper Providers

| Provider | Client Class | Environment Variable | Default Model |
|----------|--------------|---------------------|---------------|
| [Claude Code](claude-code.md) | `AiClaudeCodeClient` | `CLAUDE_CODE_PATH` (optional) | sonnet |
| [OpenCode](opencode.md) | `AiOpenCodeClient` | `OPENCODE_PATH` (optional) | anthropic/claude-sonnet-4-20250514 |

## Common Interface

All providers implement the `AiProvider` interface:

```c
/* Get provider information */
AiProviderType ai_provider_get_provider_type(AiProvider *provider);
const gchar *ai_provider_get_name(AiProvider *provider);
const gchar *ai_provider_get_default_model(AiProvider *provider);

/* Chat completion */
void ai_provider_chat_async(AiProvider *provider,
                            GList *messages,
                            const gchar *system_prompt,
                            gint max_tokens,
                            GList *tools,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data);

AiResponse *ai_provider_chat_finish(AiProvider *provider,
                                    GAsyncResult *result,
                                    GError **error);
```

## Creating a Client

Each provider has multiple constructors:

```c
/* Using environment variables (recommended) */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();

/* Using explicit configuration */
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "sk-...");
g_autoptr(AiClaudeClient) client = ai_claude_client_new_with_config(config);

/* Using API key directly */
g_autoptr(AiClaudeClient) client = ai_claude_client_new_with_key("sk-...");
```

## Setting the Model

After creating a client, you can change the model:

```c
g_autoptr(AiClaudeClient) client = ai_claude_client_new();

/* Set a different model */
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_OPUS_4);

/* Or use the string directly */
ai_client_set_model(AI_CLIENT(client), "claude-3-5-haiku-20241022");
```

## Provider Comparison

| Feature | Claude | OpenAI | Gemini | Grok | Ollama | Claude Code | OpenCode |
|---------|--------|--------|--------|------|--------|-------------|----------|
| Chat Completion | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| Streaming | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| Tool Use | Yes | Yes | Partial | Yes | Partial | Yes | Model-dependent |
| Vision | Yes | Yes | Yes | Yes | Model-dependent | Yes | Model-dependent |
| Local | No | No | No | No | Yes | No | No |
| API Key Required | Yes | Yes | Yes | Yes | No | No (uses CLI auth) | No (uses CLI auth) |
| Multi-Provider | No | No | No | No | No | No | Yes |

## Error Handling

All providers use the same error domain (`AI_ERROR`):

```c
g_autoptr(GError) error = NULL;
g_autoptr(AiResponse) response = NULL;

response = ai_provider_chat_finish(AI_PROVIDER(client), result, &error);
if (error != NULL)
{
    switch (error->code)
    {
        case AI_ERROR_INVALID_API_KEY:
            g_printerr("Invalid API key\n");
            break;
        case AI_ERROR_RATE_LIMITED:
            g_printerr("Rate limited, retry later\n");
            break;
        case AI_ERROR_NETWORK_ERROR:
            g_printerr("Network error: %s\n", error->message);
            break;
        default:
            g_printerr("Error: %s\n", error->message);
    }
}
```
