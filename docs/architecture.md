# ai-glib Architecture

This document describes the architecture and design of ai-glib.

## Overview

ai-glib is built on GLib/GObject principles, providing:
- Type-safe GObject classes
- Asynchronous operations via GTask
- GObject Introspection support for bindings
- Memory management via g_autoptr and g_steal_pointer

## Directory Structure

```
ai-glib/
  src/
    ai-glib.h          # Main umbrella header
    ai-types.h         # Forward type declarations
    core/              # Core infrastructure
      ai-error.h/.c    # Error domain and codes
      ai-enums.h/.c    # Enumerations
      ai-config.h/.c   # Configuration management
      ai-provider.h/.c # Provider interface
      ai-streamable.h/.c # Streaming interface
      ai-client.h/.c   # Base client class
    model/             # Data model classes
      ai-usage.h/.c    # Token usage (boxed type)
      ai-content-block.h/.c # Base content block
      ai-text-content.h/.c  # Text content
      ai-tool.h/.c     # Tool definition
      ai-tool-use.h/.c # Tool use request
      ai-tool-result.h/.c # Tool result
      ai-message.h/.c  # Conversation message
      ai-response.h/.c # API response
    providers/         # Provider implementations
      ai-claude-client.h/.c  # Anthropic Claude
      ai-openai-client.h/.c  # OpenAI
      ai-grok-client.h/.c    # xAI Grok
      ai-gemini-client.h/.c  # Google Gemini
      ai-ollama-client.h/.c  # Ollama (local)
      ai-claude-code-client.h/.c  # Claude Code CLI
      ai-opencode-client.h/.c     # OpenCode CLI
    convenience/       # High-level convenience wrappers
      ai-simple.h/.c   # Simple LLM interface (3 lines of C)
  tests/               # Unit tests
  examples/            # Example programs
  docs/                # Documentation
  deps/                # Bundled dependencies
    yaml-glib/         # YAML parser (built as static lib)
```

## Type Hierarchy

```
GObject
  AiConfig         (final)
  AiSimple         (final) — convenience wrapper
  AiTool           (final)
  AiMessage        (final)
  AiResponse       (final)
  AiContentBlock   (derivable)
    AiTextContent  (final)
    AiToolUse      (final)
    AiToolResult   (final)
  AiClient         (derivable, implements AiProvider, AiStreamable)
    AiClaudeClient (final)
    AiOpenAIClient (final)
    AiGrokClient   (final)
    AiGeminiClient (final)
    AiOllamaClient (final)
  AiCliClient      (derivable, implements AiProvider, AiStreamable)
    AiClaudeCodeClient (final)
    AiOpenCodeClient   (final)

GBoxed
  AiUsage

GInterface
  AiProvider
  AiStreamable
  AiImageGenerator
```

## Interfaces

### AiProvider

The `AiProvider` interface defines the contract for AI providers:

```c
struct _AiProviderInterface
{
    GTypeInterface parent_iface;

    AiProviderType (*get_provider_type)(AiProvider *self);
    const gchar *  (*get_name)(AiProvider *self);
    const gchar *  (*get_default_model)(AiProvider *self);

    void           (*chat_async)(AiProvider *self, ...);
    AiResponse *   (*chat_finish)(AiProvider *self, ...);

    void           (*list_models_async)(AiProvider *self, ...);
    GList *        (*list_models_finish)(AiProvider *self, ...);
};
```

### AiStreamable

The `AiStreamable` interface adds streaming support with signals:

- `delta` - Emitted for each text chunk during streaming
- `stream-start` - Emitted when streaming begins
- `stream-end` - Emitted when streaming completes
- `tool-use` - Emitted when a tool use is detected

## Memory Management

ai-glib follows GLib conventions:

1. **g_autoptr()** - Automatic cleanup with scope-based destruction
2. **g_steal_pointer()** - Explicit ownership transfer
3. **(transfer full)** - Caller owns the returned object
4. **(transfer none)** - Caller borrows the reference

### Example Pattern

```c
AiMessage *
ai_message_new_user(const gchar *text)
{
    g_autoptr(AiMessage) self = g_object_new(AI_TYPE_MESSAGE,
                                               "role", AI_ROLE_USER,
                                               NULL);

    g_autoptr(AiTextContent) content = ai_text_content_new(text);
    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&content));

    return (AiMessage *)g_steal_pointer(&self);
}
```

## Async Operations

All network operations are asynchronous using GTask:

```c
/* Start async operation */
ai_provider_chat_async(provider, messages, system, max_tokens, tools,
                       cancellable, callback, user_data);

/* In callback, get result */
AiResponse *response = ai_provider_chat_finish(provider, result, &error);
```

## Provider Implementation

Each provider:
1. Extends `AiClient`
2. Implements `AiProvider` interface
3. Implements `AiStreamable` interface
4. Overrides virtual methods for request building and response parsing

### Virtual Methods

```c
struct _AiClientClass
{
    GObjectClass parent_class;

    JsonNode * (*build_request)(AiClient *self, GList *messages, ...);
    AiResponse * (*parse_response)(AiClient *self, JsonNode *json, ...);
    gchar * (*get_endpoint_url)(AiClient *self);
    void (*add_auth_headers)(AiClient *self, SoupMessage *msg);
};
```

## Error Handling

Errors use the GLib error domain system:

```c
#define AI_ERROR (ai_error_quark())

typedef enum
{
    AI_ERROR_INVALID_API_KEY = 1,
    AI_ERROR_RATE_LIMITED,
    AI_ERROR_NETWORK_ERROR,
    /* ... */
} AiError;
```

Check errors in callbacks:

```c
if (error != NULL)
{
    if (g_error_matches(error, AI_ERROR, AI_ERROR_RATE_LIMITED))
    {
        /* Handle rate limiting */
    }
    return;
}
```
