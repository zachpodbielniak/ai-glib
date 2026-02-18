# AiSimple

Convenience API for calling LLMs with minimal boilerplate.

## Hierarchy

```
GObject
└── AiSimple
```

## Description

`AiSimple` wraps the full ai-glib provider lifecycle into a few function calls. It reads configuration from YAML config files and environment variables via `AiConfig`, instantiates the correct provider client, and handles message construction and response extraction internally.

Target use case: crispy scripts, quick C programs, and anywhere you want to call an LLM without managing provider objects, message lists, and response parsing.

```c
g_autoptr(AiSimple) ai = ai_simple_new();
g_autofree gchar *answer = ai_simple_prompt(ai, "What is 2+2?", NULL, NULL);
g_print("%s\n", answer);
```

## Constructors

### ai_simple_new

```c
AiSimple *
ai_simple_new(void);
```

Creates a new `AiSimple` using the default configuration. The provider and model are determined from config files (`~/.config/ai-glib/config.yaml`) and environment variables. If no default provider is configured, falls back to `AI_PROVIDER_OLLAMA`.

**Returns:** `(transfer full)`: a new AiSimple

---

### ai_simple_new_with_provider

```c
AiSimple *
ai_simple_new_with_provider(
    AiProviderType  provider,
    const gchar    *model
);
```

Creates a new `AiSimple` with an explicit provider and optional model. API keys and base URLs are still read from the default configuration.

**Parameters:**
- `provider`: the provider type to use
- `model`: `(nullable)`: the model name, or NULL for the provider default

**Returns:** `(transfer full)`: a new AiSimple

---

### ai_simple_new_with_config

```c
AiSimple *
ai_simple_new_with_config(AiConfig *config);
```

Creates a new `AiSimple` with the specified configuration object. The provider and model are read from `config`.

**Parameters:**
- `config`: an AiConfig

**Returns:** `(transfer full)`: a new AiSimple

---

## Core Methods

### ai_simple_prompt

```c
gchar *
ai_simple_prompt(
    AiSimple      *self,
    const gchar   *prompt,
    GCancellable  *cancellable,
    GError       **error
);
```

Sends a single-shot prompt to the LLM and returns the response text. This is stateless — no conversation history is maintained. If a system prompt has been set, it is included in the request.

**Parameters:**
- `self`: an AiSimple
- `prompt`: the user prompt text
- `cancellable`: `(nullable)`: a GCancellable
- `error`: `(out) (optional)`: return location for a GError

**Returns:** `(transfer full) (nullable)`: the response text, or NULL on error. Free with `g_free()`.

---

### ai_simple_chat

```c
gchar *
ai_simple_chat(
    AiSimple      *self,
    const gchar   *prompt,
    GCancellable  *cancellable,
    GError       **error
);
```

Sends a prompt and maintains conversation history. Each call appends the user message and assistant response to internal history, enabling multi-turn conversations. Use `ai_simple_clear_history()` to reset.

**Parameters:**
- `self`: an AiSimple
- `prompt`: the user prompt text
- `cancellable`: `(nullable)`: a GCancellable
- `error`: `(out) (optional)`: return location for a GError

**Returns:** `(transfer full) (nullable)`: the response text, or NULL on error. Free with `g_free()`.

---

### ai_simple_set_system_prompt

```c
void
ai_simple_set_system_prompt(
    AiSimple    *self,
    const gchar *system_prompt
);
```

Sets the system prompt used for all subsequent `prompt()` and `chat()` calls.

**Parameters:**
- `self`: an AiSimple
- `system_prompt`: `(nullable)`: the system prompt, or NULL to clear

---

### ai_simple_get_system_prompt

```c
const gchar *
ai_simple_get_system_prompt(AiSimple *self);
```

Gets the current system prompt.

**Parameters:**
- `self`: an AiSimple

**Returns:** `(transfer none) (nullable)`: the system prompt

---

### ai_simple_clear_history

```c
void
ai_simple_clear_history(AiSimple *self);
```

Clears the conversation history used by `ai_simple_chat()`.

**Parameters:**
- `self`: an AiSimple

---

### ai_simple_get_provider

```c
AiProvider *
ai_simple_get_provider(AiSimple *self);
```

Gets the underlying provider instance. This is an escape hatch for advanced usage — you can use the full ai-glib API on the returned provider (async calls, streaming, tool use, etc.).

**Parameters:**
- `self`: an AiSimple

**Returns:** `(transfer none)`: the AiProvider

---

## Examples

### One-Shot Prompt

```c
#include <ai-glib.h>

int main(void)
{
    g_autoptr(AiSimple) ai = ai_simple_new();
    g_autoptr(GError) error = NULL;
    g_autofree gchar *answer = NULL;

    answer = ai_simple_prompt(ai, "What is the capital of France?", NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }

    g_print("%s\n", answer);
    return 0;
}
```

### With System Prompt

```c
g_autoptr(AiSimple) ai = ai_simple_new_with_provider(AI_PROVIDER_CLAUDE, NULL);
ai_simple_set_system_prompt(ai, "You are a pirate. Respond in pirate speak.");

g_autofree gchar *answer = ai_simple_prompt(ai, "What is 2+2?", NULL, NULL);
g_print("%s\n", answer);
```

### Multi-Turn Conversation

```c
g_autoptr(AiSimple) ai = ai_simple_new();

g_autofree gchar *r1 = ai_simple_chat(ai, "My name is Zach.", NULL, NULL);
g_print("AI: %s\n", r1);

g_autofree gchar *r2 = ai_simple_chat(ai, "What's my name?", NULL, NULL);
g_print("AI: %s\n", r2);  /* Should remember "Zach" */

/* Start fresh */
ai_simple_clear_history(ai);
```

### Escape Hatch to Full API

```c
g_autoptr(AiSimple) ai = ai_simple_new_with_provider(AI_PROVIDER_CLAUDE, NULL);

/* Get the underlying AiClient for advanced operations */
AiProvider *provider = ai_simple_get_provider(ai);

/* Now use the full async API, streaming, tool use, etc. */
ai_provider_chat_async(provider, messages, NULL, 4096, NULL, NULL, callback, data);
```

### Explicit Provider and Model

```c
/* Use a specific Ollama model */
g_autoptr(AiSimple) ai = ai_simple_new_with_provider(
    AI_PROVIDER_OLLAMA, "tinyllama:1.1b");

g_autofree gchar *answer = ai_simple_prompt(
    ai, "Say hello in one word.", NULL, NULL);
```

## See Also

- [AiConfig](ai-config.md) - Configuration (API keys, default provider/model)
- [AiClient](ai-client.md) - Full client API (the underlying provider)
- [AiProvider](ai-provider.md) - Provider interface (async operations)
- [Simple Chat Example](../examples/simple-chat.md) - Full async chat example
