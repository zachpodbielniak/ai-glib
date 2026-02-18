# AiConfig

Configuration management for AI providers.

## Hierarchy

```
GObject
└── AiConfig
```

## Description

`AiConfig` manages configuration for all AI providers, including API keys, base URLs, timeouts, retry settings, and default provider/model selection. It automatically loads YAML configuration files from a 3-path fallback chain (`/usr/share/ai-glib/`, `/etc/ai-glib/`, `~/.config/ai-glib/`), reads environment variables, and allows programmatic overrides.

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
- `AI_GLIB_DEFAULT_PROVIDER` - Default provider name (e.g., `ollama`, `claude`, `openai`)
- `AI_GLIB_DEFAULT_MODEL` - Default model name (e.g., `qwen2.5:7b`, `claude-sonnet-4-20250514`)

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

---

### ai_config_load_from_file

```c
gboolean
ai_config_load_from_file(
    AiConfig     *self,
    const gchar  *path,
    GError      **error
);
```

Loads configuration from a YAML file. Values in the file overlay onto existing
configuration (later calls override earlier ones).

**Parameters:**
- `self`: an AiConfig
- `path`: path to the YAML config file
- `error`: `(out) (optional)`: return location for error

**Returns:** TRUE on success, FALSE on parse error or missing file

---

### ai_config_get_default_provider

```c
AiProviderType
ai_config_get_default_provider(AiConfig *self);
```

Gets the default provider type. Checks the following sources in priority order:

1. Programmatic value set via `ai_config_set_default_provider()`
2. `AI_GLIB_DEFAULT_PROVIDER` environment variable
3. `default_provider` key from YAML config files
4. Built-in default (`AI_PROVIDER_CLAUDE`)

**Parameters:**
- `self`: an AiConfig

**Returns:** the default AiProviderType

---

### ai_config_set_default_provider

```c
void
ai_config_set_default_provider(
    AiConfig       *self,
    AiProviderType  provider
);
```

Sets the default provider type programmatically. This takes the highest
priority, overriding both `AI_GLIB_DEFAULT_PROVIDER` environment variable
and config file values.

**Parameters:**
- `self`: an AiConfig
- `provider`: the provider type

---

### ai_config_get_default_model

```c
const gchar *
ai_config_get_default_model(AiConfig *self);
```

Gets the default model name. Checks the following sources in priority order:

1. Programmatic value set via `ai_config_set_default_model()`
2. `AI_GLIB_DEFAULT_MODEL` environment variable
3. `default_model` key from YAML config files
4. Built-in default (NULL)

**Parameters:**
- `self`: an AiConfig

**Returns:** `(transfer none) (nullable)`: the default model, or NULL

---

### ai_config_set_default_model

```c
void
ai_config_set_default_model(
    AiConfig    *self,
    const gchar *model
);
```

Sets the default model name programmatically. This takes the highest
priority, overriding both `AI_GLIB_DEFAULT_MODEL` environment variable
and config file values.

**Parameters:**
- `self`: an AiConfig
- `model`: the model name

---

## YAML Config File Format

`ai_config_new()` automatically loads YAML config files from a 3-path
fallback chain:

1. `/usr/share/ai-glib/config.yaml` — distro defaults (lowest priority)
2. `/etc/ai-glib/config.yaml` — system admin
3. `~/.config/ai-glib/config.yaml` — user overrides (highest file priority)

```yaml
default_provider: ollama
default_model: qwen2.5:7b

providers:
  claude:
    api_key: sk-ant-...
  openai:
    api_key: sk-...
    base_url: https://api.openai.com
  ollama:
    base_url: http://localhost:11434

timeout: 120
max_retries: 3
```

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

3. **Config files**
   - YAML files loaded from the 3-path fallback chain

4. **Built-in defaults** (lowest priority)
   - Hardcoded defaults (e.g., Ollama host = localhost:11434)

## See Also

- [AiSimple](ai-simple.md) - Convenience API that uses config automatically
- [AiClient](ai-client.md) - Base client class
- [Configuration Guide](../configuration.md) - Detailed configuration reference
- [AiError](ai-error.md) - Error codes for validation failures
