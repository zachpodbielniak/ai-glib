# Configuration

This document describes configuration options in ai-glib.

## AiConfig

The `AiConfig` object manages configuration for all providers:

```c
g_autoptr(AiConfig) config = ai_config_new();
```

## YAML Config Files

`ai_config_new()` automatically loads YAML configuration files from a
3-path fallback chain (lowest to highest priority):

1. `/usr/share/ai-glib/config.yaml` — distro/image defaults
2. `/etc/ai-glib/config.yaml` — system admin overrides
3. `~/.config/ai-glib/config.yaml` — user overrides

Each file is optional. Later files override earlier ones. Environment
variables override all file values. Programmatic `set_*()` calls override
everything.

### Config File Format

```yaml
# Default provider and model used when no explicit provider is requested
default_provider: ollama
default_model: qwen2.5:7b

# Per-provider configuration
providers:
  claude:
    api_key: sk-ant-...
  openai:
    api_key: sk-...
    base_url: https://api.openai.com
  gemini:
    api_key: AIza...
  grok:
    api_key: xai-...
  ollama:
    base_url: http://localhost:11434

# Global request settings
timeout: 120
max_retries: 3
```

All keys are optional. Missing keys are skipped (fall through to env vars / defaults).

### Loading a Custom Config File

```c
g_autoptr(AiConfig) config = ai_config_new();
g_autoptr(GError) error = NULL;

if (!ai_config_load_from_file(config, "/path/to/config.yaml", &error))
{
    g_printerr("Config error: %s\n", error->message);
}
```

### Default Provider and Model

```c
/* Read from config file or set programmatically */
AiProviderType provider = ai_config_get_default_provider(config);
const gchar *model = ai_config_get_default_model(config);

/* Override */
ai_config_set_default_provider(config, AI_PROVIDER_CLAUDE);
ai_config_set_default_model(config, "claude-sonnet-4-20250514");
```

## API Keys

### From Environment Variables

By default, `ai_config_new()` reads API keys from environment variables.
Multiple environment variable names are supported for some providers,
checked in order of precedence:

#### HTTP API Providers

| Provider | Environment Variables (in order of precedence) |
|----------|-----------------------------------------------|
| Claude   | `ANTHROPIC_API_KEY`, `CLAUDE_API_KEY` |
| OpenAI   | `OPENAI_API_KEY` |
| Gemini   | `GEMINI_API_KEY` |
| Grok     | `XAI_API_KEY`, `GROK_API_KEY` |
| Ollama   | `OLLAMA_API_KEY` (optional) |

#### CLI Wrapper Providers

CLI providers (`AiClaudeCodeClient`, `AiOpenCodeClient`) do not use
API keys directly — they delegate authentication to their respective
CLI tools. Optional environment variables:

| Provider | Environment Variable | Description |
|----------|---------------------|-------------|
| Claude Code | `CLAUDE_CODE_PATH` | Override path to `claude` executable |
| OpenCode | `OPENCODE_PATH` | Override path to `opencode` executable |

### Programmatic Configuration

```c
ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "your-api-key");
ai_config_set_api_key(config, AI_PROVIDER_OPENAI, "your-openai-key");
```

## Default Provider and Model Environment Variables

The default provider and model can be set via environment variables, which
override config file values but are overridden by programmatic `set_*()` calls:

| Variable | Description | Example |
|----------|-------------|---------|
| `AI_GLIB_DEFAULT_PROVIDER` | Default provider name | `ollama`, `claude`, `openai`, `gemini`, `grok`, `claude-code`, `opencode` |
| `AI_GLIB_DEFAULT_MODEL` | Default model name | `qwen2.5:7b`, `claude-sonnet-4-20250514` |

```bash
export AI_GLIB_DEFAULT_PROVIDER=ollama
export AI_GLIB_DEFAULT_MODEL=qwen2.5:7b
```

These are read at runtime by `ai_config_get_default_provider()` and
`ai_config_get_default_model()`. See [Configuration Priority](#configuration-priority)
for the full precedence chain.

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

## Configuration Priority

When looking up values, the following order is used (highest to lowest):

1. **Programmatic override** — values set via `ai_config_set_*()` functions
2. **Environment variables** — read automatically from the environment
3. **Config files** — YAML files loaded in the 3-path fallback chain
4. **Built-in defaults** — hardcoded defaults (e.g., Ollama host = `localhost:11434`)

## Using AiSimple (Convenience API)

For the simplest possible setup, use `AiSimple` which reads all configuration
automatically:

```c
/* Reads default_provider and default_model from config files + env vars */
g_autoptr(AiSimple) ai = ai_simple_new();
g_autofree gchar *answer = ai_simple_prompt(ai, "Hello!", NULL, NULL);
```

See [AiSimple](api-reference/ai-simple.md) for the full API.
