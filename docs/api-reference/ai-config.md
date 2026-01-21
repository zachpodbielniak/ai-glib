# AiConfig

Configuration management for AI providers.

## Hierarchy

```
GObject
└── AiConfig
```

## Description

`AiConfig` manages configuration for all AI providers, including API keys, base URLs, timeouts, and retry settings. It automatically reads environment variables and allows programmatic overrides.

## Environment Variables

| Provider | Environment Variables (in order of precedence) |
|----------|-----------------------------------------------|
| Claude   | `ANTHROPIC_API_KEY`, `CLAUDE_API_KEY` |
| OpenAI   | `OPENAI_API_KEY` |
| Gemini   | `GEMINI_API_KEY` |
| Grok     | `XAI_API_KEY`, `GROK_API_KEY` |
| Ollama   | `OLLAMA_API_KEY` (optional), `OLLAMA_HOST` |

Additional variables:
- `OPENAI_BASE_URL` - Custom base URL for OpenAI-compatible APIs
- `OLLAMA_HOST` - Ollama server URL (default: `http://localhost:11434`)

## Functions

### ai_config_new

```c
AiConfig *
ai_config_new(void);
```

Creates a new configuration object.

**Returns:** `(transfer full)`: a new AiConfig

---

### ai_config_get_default

```c
AiConfig *
ai_config_get_default(void);
```

Gets the default shared configuration instance.

**Returns:** `(transfer none)`: the default AiConfig singleton

---

### ai_config_get_api_key

```c
const gchar *
ai_config_get_api_key(AiConfig *self, AiProviderType provider);
```

Gets the API key for a provider.

**Parameters:**
- `self`: an AiConfig
- `provider`: the provider type

**Returns:** `(transfer none) (nullable)`: the API key, or NULL if not set

---

### ai_config_set_api_key

```c
void
ai_config_set_api_key(
    AiConfig      *self,
    AiProviderType provider,
    const gchar   *api_key
);
```

Sets the API key for a provider (overrides environment variable).

**Parameters:**
- `self`: an AiConfig
- `provider`: the provider type
- `api_key`: the API key

---

### ai_config_get_base_url

```c
const gchar *
ai_config_get_base_url(AiConfig *self, AiProviderType provider);
```

Gets the base URL for a provider.

**Parameters:**
- `self`: an AiConfig
- `provider`: the provider type

**Returns:** `(transfer none) (nullable)`: the base URL, or NULL for default

---

### ai_config_set_base_url

```c
void
ai_config_set_base_url(
    AiConfig      *self,
    AiProviderType provider,
    const gchar   *base_url
);
```

Sets a custom base URL for a provider.

**Parameters:**
- `self`: an AiConfig
- `provider`: the provider type
- `base_url`: the base URL

---

### ai_config_get_timeout

```c
guint
ai_config_get_timeout(AiConfig *self);
```

Gets the request timeout in seconds.

**Parameters:**
- `self`: an AiConfig

**Returns:** the timeout in seconds

---

### ai_config_set_timeout

```c
void
ai_config_set_timeout(AiConfig *self, guint timeout_seconds);
```

Sets the request timeout.

**Parameters:**
- `self`: an AiConfig
- `timeout_seconds`: timeout in seconds

---

### ai_config_get_max_retries

```c
guint
ai_config_get_max_retries(AiConfig *self);
```

Gets the maximum number of retries.

**Parameters:**
- `self`: an AiConfig

**Returns:** the max retry count

---

### ai_config_set_max_retries

```c
void
ai_config_set_max_retries(AiConfig *self, guint max_retries);
```

Sets the maximum number of retries for failed requests.

**Parameters:**
- `self`: an AiConfig
- `max_retries`: maximum retry count

---

### ai_config_validate

```c
gboolean
ai_config_validate(
    AiConfig      *self,
    AiProviderType provider,
    GError       **error
);
```

Validates configuration for a specific provider.

**Parameters:**
- `self`: an AiConfig
- `provider`: the provider type to validate
- `error`: `(out) (optional)`: return location for error

**Returns:** TRUE if valid, FALSE if configuration is incomplete

## Example

```c
#include <ai-glib.h>

/* Using default configuration (reads from environment) */
g_autoptr(AiClaudeClient) client1 = ai_claude_client_new();

/* Using custom configuration */
g_autoptr(AiConfig) config = ai_config_new();

/* Override API key */
ai_config_set_api_key(config, AI_PROVIDER_CLAUDE, "sk-ant-...");

/* Set custom timeout */
ai_config_set_timeout(config, 120);  /* 2 minutes */

/* Set retry count */
ai_config_set_max_retries(config, 3);

/* Validate before use */
g_autoptr(GError) error = NULL;
if (!ai_config_validate(config, AI_PROVIDER_CLAUDE, &error))
{
    g_printerr("Configuration error: %s\n", error->message);
    return 1;
}

/* Create client with custom config */
g_autoptr(AiClaudeClient) client2 = ai_claude_client_new_with_config(config);

/* Custom base URL for OpenAI-compatible API */
ai_config_set_base_url(config, AI_PROVIDER_OPENAI, "https://api.custom.com/v1");
g_autoptr(AiOpenAIClient) custom_client = ai_openai_client_new_with_config(config);

/* Ollama with custom host */
ai_config_set_base_url(config, AI_PROVIDER_OLLAMA, "http://192.168.1.100:11434");
g_autoptr(AiOllamaClient) remote_ollama = ai_ollama_client_new_with_config(config);
```

## Configuration Priority

When looking up configuration values, the following order is used:

1. **Programmatic override** (highest priority)
   - Values set via `ai_config_set_*()` functions

2. **Environment variables**
   - Read automatically from the environment

3. **Default values** (lowest priority)
   - Built-in defaults (e.g., Ollama host = localhost:11434)

## See Also

- [AiClient](ai-client.md) - Base client class
- [Configuration Guide](../configuration.md) - Detailed configuration reference
- [AiError](ai-error.md) - Error codes for validation failures
