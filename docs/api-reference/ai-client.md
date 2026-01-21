# AiClient

Base class for all provider clients.

## Hierarchy

```
GObject
└── AiClient
    ├── AiClaudeClient
    ├── AiOpenAIClient
    ├── AiGeminiClient
    ├── AiGrokClient
    └── AiOllamaClient
```

## Description

`AiClient` is the abstract base class for all AI provider clients. It provides common functionality for configuration, model selection, and HTTP communication via libsoup.

Subclasses must implement:
- `build_request()` - Build provider-specific JSON request
- `parse_response()` - Parse provider-specific JSON response
- `get_endpoint_url()` - Return the API endpoint URL
- `add_auth_headers()` - Add authentication headers

## Functions

### ai_client_get_config

```c
AiConfig *
ai_client_get_config(AiClient *self);
```

Gets the configuration object.

**Parameters:**
- `self`: an AiClient

**Returns:** `(transfer none)`: the AiConfig, do not free

---

### ai_client_get_model

```c
const gchar *
ai_client_get_model(AiClient *self);
```

Gets the current model name.

**Parameters:**
- `self`: an AiClient

**Returns:** `(transfer none) (nullable)`: the model name

---

### ai_client_set_model

```c
void
ai_client_set_model(
    AiClient    *self,
    const gchar *model
);
```

Sets the model to use for requests.

**Parameters:**
- `self`: an AiClient
- `model`: the model name

---

### ai_client_get_max_tokens

```c
gint
ai_client_get_max_tokens(AiClient *self);
```

Gets the default max tokens setting.

**Parameters:**
- `self`: an AiClient

**Returns:** the max tokens value

---

### ai_client_set_max_tokens

```c
void
ai_client_set_max_tokens(
    AiClient *self,
    gint      max_tokens
);
```

Sets the default max tokens for responses.

**Parameters:**
- `self`: an AiClient
- `max_tokens`: maximum tokens to generate

---

### ai_client_get_temperature

```c
gdouble
ai_client_get_temperature(AiClient *self);
```

Gets the sampling temperature.

**Parameters:**
- `self`: an AiClient

**Returns:** the temperature (0.0 to 2.0)

---

### ai_client_set_temperature

```c
void
ai_client_set_temperature(
    AiClient *self,
    gdouble   temperature
);
```

Sets the sampling temperature.

**Parameters:**
- `self`: an AiClient
- `temperature`: temperature value (0.0 to 2.0)

---

### ai_client_get_system_prompt

```c
const gchar *
ai_client_get_system_prompt(AiClient *self);
```

Gets the default system prompt.

**Parameters:**
- `self`: an AiClient

**Returns:** `(transfer none) (nullable)`: the system prompt

---

### ai_client_set_system_prompt

```c
void
ai_client_set_system_prompt(
    AiClient    *self,
    const gchar *prompt
);
```

Sets the default system prompt.

**Parameters:**
- `self`: an AiClient
- `prompt`: `(nullable)`: the system prompt

## Example

```c
/* Get a client (from any provider) */
g_autoptr(AiClaudeClient) claude = ai_claude_client_new();
AiClient *client = AI_CLIENT(claude);

/* Configure the client */
ai_client_set_model(client, "claude-sonnet-4-20250514");
ai_client_set_max_tokens(client, 4096);
ai_client_set_temperature(client, 0.7);
ai_client_set_system_prompt(client, "You are a helpful assistant.");

/* Check current settings */
g_print("Model: %s\n", ai_client_get_model(client));
g_print("Max tokens: %d\n", ai_client_get_max_tokens(client));
g_print("Temperature: %.1f\n", ai_client_get_temperature(client));
```

## See Also

- [AiProvider](ai-provider.md) - Provider interface
- [AiConfig](ai-config.md) - Configuration
