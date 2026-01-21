# Configuration

This document describes configuration options in ai-glib.

## AiConfig

The `AiConfig` object manages configuration for all providers:

```c
g_autoptr(AiConfig) config = ai_config_new();
```

## API Keys

### From Environment Variables

By default, `ai_config_new()` reads API keys from environment variables.
Multiple environment variable names are supported for some providers,
checked in order of precedence:

| Provider | Environment Variables (in order of precedence) |
|----------|-----------------------------------------------|
| Claude   | `ANTHROPIC_API_KEY`, `CLAUDE_API_KEY` |
| OpenAI   | `OPENAI_API_KEY` |
| Gemini   | `GEMINI_API_KEY` |
| Grok     | `XAI_API_KEY`, `GROK_API_KEY` |
| Ollama   | `OLLAMA_API_KEY` (optional) |

### Programmatic Configuration

```c
ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "your-api-key");
ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "your-openai-key");
```

## Base URLs

Default base URLs are set for each provider:

| Provider | Default URL |
|----------|-------------|
| Claude   | https://api.anthropic.com |
| OpenAI   | https://api.openai.com |
| Gemini   | https://generativelanguage.googleapis.com |
| Grok     | https://api.x.ai |
| Ollama   | http://localhost:11434 |

### Custom Base URLs

For OpenAI-compatible APIs or custom deployments:

```c
/* Use Azure OpenAI */
ai_config_set_base_url(config, AI_PROVIDER_OPENAI,
                       "https://your-resource.openai.azure.com");

/* Use local Ollama on different port */
ai_config_set_base_url(config, AI_PROVIDER_OLLAMA,
                       "http://localhost:12345");
```

Or via environment variable (OpenAI only):

```bash
export OPENAI_BASE_URL="https://your-api.example.com"
```

## Timeout and Retries

```c
/* Set timeout (default: 120 seconds) */
ai_config_set_timeout(config, 60);

/* Set max retries (default: 3) */
ai_config_set_max_retries(config, 5);
```

## Validation

Validate configuration before making requests:

```c
g_autoptr(GError) error = NULL;

if (!ai_config_validate(config, AI_PROVIDER_CLAUDE, &error))
{
    g_printerr("Configuration error: %s\n", error->message);
    return;
}
```

## Using Configuration with Clients

### Default Configuration

```c
/* Uses default configuration (reads from environment) */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
```

### Custom Configuration

```c
g_autoptr(AiConfig) config = ai_config_new();
ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "custom-key");

g_autoptr(AiClaudeClient) client = ai_claude_client_new_with_config(config);
```

### Direct API Key

```c
/* Shorthand for single provider */
g_autoptr(AiClaudeClient) client = ai_claude_client_new_with_key("your-key");
```

## Shared Configuration

The default configuration is a singleton:

```c
AiConfig *config = ai_config_get_default();
/* Configure once, use everywhere */
ai_config_set_timeout(config, 30);
```

All clients created with `ai_xxx_client_new()` share this configuration.

## Model Selection

Set the model on the client:

```c
g_autoptr(AiClaudeClient) client = ai_claude_client_new();

/* Change from default model */
ai_client_set_model(AI_CLIENT(client), "claude-opus-4-20250514");
```

Available models vary by provider. Use `ai_provider_list_models_async()` to query.

## Temperature

Control response randomness:

```c
/* Lower = more deterministic, higher = more creative */
ai_client_set_temperature(AI_CLIENT(client), 0.7);
```

Range is typically 0.0 to 2.0 (default: 1.0).
