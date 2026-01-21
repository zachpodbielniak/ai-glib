# CLAUDE.md - ai-glib

Instructions for Claude Code when working on this project.

## Project Overview

ai-glib is a GLib/GObject-based C library for interacting with AI providers
(Claude, OpenAI, Gemini, Grok, Ollama). It follows GObject conventions and
provides both sync and async APIs.

## Build Commands

```bash
make                    # Build the library
make all                # Build library, tests, and examples
make DEBUG=1            # Debug build
make test               # Run all tests
make test-verbose       # Run tests with verbose output
make clean              # Clean build artifacts
make install            # Install to PREFIX
make gir                # Generate GObject introspection data
```

## Code Style

### C Standard
- Use `gnu89` exclusively
- Compile with `gcc`

### Formatting
- Indentation: TAB (4 spaces width)
- Naming:
  - Types: `AiTypeName` (PascalCase)
  - Functions: `ai_type_name_method()` (lowercase_snake_case)
  - Macros/defines: `AI_MACRO_NAME` (UPPERCASE_SNAKE_CASE)
  - Variables: `lowercase_snake_case`
- Comments: Always use `/* comment */`, never `//`

### Function Style

```c
/* function declaration */
AiResponse *
ai_client_chat_sync(
    AiClient    *self,
    GList       *messages,
    GCancellable *cancellable,
    GError      **error
);

/* function definition */
AiResponse *
ai_client_chat_sync(
    AiClient    *self,
    GList       *messages,
    GCancellable *cancellable,
    GError      **error
){
    AiResponse *response;
    g_autoptr(GTask) task = NULL;

    g_return_val_if_fail(AI_IS_CLIENT(self), NULL);

    /* implementation */
    return response;
}
```

### GObject Patterns

- Use `G_DECLARE_FINAL_TYPE` for final types
- Use `G_DECLARE_DERIVABLE_TYPE` for derivable types
- Use `G_DECLARE_INTERFACE` for interfaces
- Always include `gpointer _reserved[8]` in class/interface structs

### Memory Management (CRITICAL)

**Always use `g_autoptr()` and `g_autofree` for automatic cleanup:**
```c
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
g_autoptr(AiMessage) msg = ai_message_new_user("Hello");
g_autoptr(GError) error = NULL;
g_autofree gchar *text = ai_response_get_text(response);
```

**Always use `g_steal_pointer()` for explicit ownership transfer:**
```c
/* Returning ownership from a function */
AiResponse *
ai_client_parse_response(AiClient *self, JsonNode *json, GError **error)
{
    g_autoptr(AiResponse) response = ai_response_new(id, model);

    /* ... populate response ... */

    /* Transfer ownership to caller */
    return (AiResponse *)g_steal_pointer(&response);
}

/* Transferring ownership to a container */
g_autoptr(AiTextContent) content = ai_text_content_new(text);
ai_message_add_content_block(msg, (AiContentBlock *)g_steal_pointer(&content));
```

**Pattern for all `_new()` functions:**
```c
AiMessage *
ai_message_new(AiRole role, const gchar *content)
{
    g_autoptr(AiMessage) self = g_object_new(AI_TYPE_MESSAGE,
                                               "role", role,
                                               NULL);

    /* Add text content */
    g_autoptr(AiTextContent) text_content = ai_text_content_new(content);
    ai_message_add_content_block(self, (AiContentBlock *)g_steal_pointer(&text_content));

    return (AiMessage *)g_steal_pointer(&self);
}
```

### Header Guards

```c
#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif
```

### GObject Introspection

All public APIs must include GIR annotations:
- `(transfer none)` / `(transfer full)` for ownership
- `(nullable)` for nullable parameters/returns
- `(out)` for output parameters
- `(element-type TypeName)` for container types

### Ownership Rules

Functions returning `(transfer full)`:
- `*_new()` - always transfer full
- `*_get_text()` - string copies
- `*_copy()` - copies
- `*_dup_*()` - duplicates

Functions returning `(transfer none)`:
- `*_get_*()` on objects - borrowed reference
- Properties accessed via getter - borrowed

## Directory Structure

```
ai-glib/
├── src/
│   ├── ai-glib.h              # Main umbrella header
│   ├── core/                  # Core infrastructure
│   │   ├── ai-client.h/.c     # Base client class
│   │   ├── ai-config.h/.c     # Configuration management
│   │   ├── ai-error.h/.c      # Error domain and codes
│   │   ├── ai-enums.h/.c      # Enumerations
│   │   ├── ai-provider.h/.c   # Provider interface
│   │   └── ai-streamable.h/.c # Streaming interface
│   ├── model/                 # Data model classes
│   │   ├── ai-message.h/.c    # Conversation message
│   │   ├── ai-response.h/.c   # API response
│   │   ├── ai-tool.h/.c       # Tool definition
│   │   └── ...                # Content block types
│   └── providers/             # Provider implementations
│       ├── ai-claude-client.h/.c
│       ├── ai-openai-client.h/.c
│       ├── ai-gemini-client.h/.c
│       ├── ai-grok-client.h/.c
│       └── ai-ollama-client.h/.c
├── tests/                     # GTest unit tests
├── examples/                  # Example programs
│   ├── simple-chat-claude.c
│   ├── simple-chat-openai.c
│   ├── simple-chat-gemini.c
│   ├── simple-chat-grok.c
│   └── simple-chat-ollama.c
├── docs/                      # Documentation
│   ├── index.md
│   ├── contributing.md
│   ├── providers/             # Provider-specific docs
│   ├── api-reference/         # API documentation
│   └── examples/              # Example walkthroughs
└── Makefile
```

## Environment Variables

| Provider | Environment Variables (in order of precedence) |
|----------|-----------------------------------------------|
| Claude   | `ANTHROPIC_API_KEY`, `CLAUDE_API_KEY` |
| OpenAI   | `OPENAI_API_KEY`, `OPENAI_BASE_URL` (optional) |
| Gemini   | `GEMINI_API_KEY` |
| Grok     | `XAI_API_KEY`, `GROK_API_KEY` |
| Ollama   | `OLLAMA_HOST` (default: `http://localhost:11434`), `OLLAMA_API_KEY` (optional) |

## Model Defines

Each provider header defines model constants. Use these instead of hardcoding strings:

```c
/* Claude */
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_SONNET_4);
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_OPUS_4_5);
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_HAIKU);  /* alias */

/* OpenAI */
ai_client_set_model(AI_CLIENT(client), AI_OPENAI_MODEL_GPT_4O);
ai_client_set_model(AI_CLIENT(client), AI_OPENAI_MODEL_GPT_5_2);
ai_client_set_model(AI_CLIENT(client), AI_OPENAI_MODEL_O3);

/* Gemini */
ai_client_set_model(AI_CLIENT(client), AI_GEMINI_MODEL_2_5_FLASH);
ai_client_set_model(AI_CLIENT(client), AI_GEMINI_MODEL_3_PRO_PREVIEW);

/* Grok */
ai_client_set_model(AI_CLIENT(client), AI_GROK_MODEL_4_1_FAST_REASONING);
ai_client_set_model(AI_CLIENT(client), AI_GROK_MODEL_CODE_FAST_1);

/* Ollama */
ai_client_set_model(AI_CLIENT(client), AI_OLLAMA_MODEL_LLAMA3_2);
ai_client_set_model(AI_CLIENT(client), AI_OLLAMA_MODEL_DEEPSEEK_R1_14B);
```

See `src/providers/ai-*-client.h` for complete model lists.

## Testing

Tests use GLib's GTest framework. Each component has its own test file:

    tests/test-<component>.c

Run specific test:

```bash
./build/tests/test-config
```

Run with verbose output:

```bash
G_TEST_VERBOSE=1 ./build/tests/test-config
```

## Key Files

- `src/ai-glib.h` - Main umbrella header
- `src/core/ai-client.h` - Base client class
- `src/core/ai-provider.h` - Provider interface
- `src/core/ai-streamable.h` - Streaming interface
- `src/core/ai-config.h` - Configuration management
- `Makefile` - Build system

## Adding a New Provider

1. Create `src/providers/ai-<name>-client.h/.c`
2. Extend `AiClient`, implement `AiProvider` and `AiStreamable`
3. Define model constants in the header (e.g., `AI_<NAME>_MODEL_*`)
4. Add to `PUBLIC_HEADERS` and `LIB_SOURCES` in Makefile
5. Add test file `tests/test-<name>-client.c`
6. Add example `examples/simple-chat-<name>.c`
7. Add documentation `docs/providers/<name>.md`
8. Update `src/ai-glib.h` to include new header

## Adding New Models to a Provider

1. Add define to `src/providers/ai-<provider>-client.h`:
   ```c
   #define AI_<PROVIDER>_MODEL_<NAME> "model-id-string"
   ```
2. Update documentation in `docs/providers/<provider>.md`
3. Consider adding convenience aliases if appropriate

## Common Patterns

### Basic Chat Request

```c
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
GList *messages = g_list_append(NULL, msg);

ai_provider_chat_async(
    AI_PROVIDER(client),
    messages,
    "You are helpful.",  /* system prompt */
    4096,                /* max tokens */
    NULL,                /* tools */
    NULL,                /* cancellable */
    on_response,         /* callback */
    loop                 /* user data */
);

g_main_loop_run(loop);
g_list_free(messages);
```

### Error Handling

```c
if (error != NULL)
{
    if (error->domain == AI_ERROR)
    {
        switch (error->code)
        {
            case AI_ERROR_INVALID_API_KEY:
                /* Handle auth error */
                break;
            case AI_ERROR_RATE_LIMITED:
                /* Handle rate limit */
                break;
            default:
                g_printerr("Error: %s\n", error->message);
                break;
        }
    }
}
```
