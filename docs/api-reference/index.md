# API Reference

Complete API documentation for ai-glib.

## Convenience API

| Class | Description |
|-------|-------------|
| [AiSimple](ai-simple.md) | Simple convenience wrapper — call an LLM in 3 lines of C |

## Core Classes

| Class | Description |
|-------|-------------|
| [AiClient](ai-client.md) | Base class for all provider clients |
| [AiConfig](ai-config.md) | Configuration management |
| [AiError](ai-error.md) | Error codes and handling |

## Interfaces

| Interface | Description |
|-----------|-------------|
| [AiProvider](ai-provider.md) | Core provider interface |
| AiStreamable | Streaming response interface |
| [AiImageGenerator](ai-image-generator.md) | Image generation interface |

## Model Classes

| Class | Description |
|-------|-------------|
| [AiMessage](ai-message.md) | Conversation message |
| [AiResponse](ai-response.md) | API response |
| [AiTool](ai-tool.md) | Tool/function definition |
| AiUsage | Token usage (boxed type) |
| AiImageRequest | Image generation request (boxed type) |
| AiImageResponse | Image generation response (boxed type) |
| AiGeneratedImage | Generated image data (boxed type) |

## Content Block Classes

| Class | Description |
|-------|-------------|
| AiContentBlock | Base class for content |
| AiTextContent | Text content block |
| AiToolUse | Tool use content block |
| AiToolResult | Tool result content block |

## Provider Clients (HTTP API)

| Class | Provider |
|-------|----------|
| AiClaudeClient | Anthropic Claude |
| AiOpenAIClient | OpenAI GPT |
| AiGeminiClient | Google Gemini |
| AiGrokClient | xAI Grok |
| AiOllamaClient | Ollama (local) |

## Provider Clients (CLI Wrappers)

| Class | Provider |
|-------|----------|
| AiClaudeCodeClient | Claude Code CLI |
| AiOpenCodeClient | OpenCode CLI |

## Enumerations

### AiProviderType

Provider identification:

```c
typedef enum {
    AI_PROVIDER_CLAUDE,
    AI_PROVIDER_OPENAI,
    AI_PROVIDER_GEMINI,
    AI_PROVIDER_GROK,
    AI_PROVIDER_OLLAMA,
    AI_PROVIDER_CLAUDE_CODE,
    AI_PROVIDER_OPENCODE
} AiProviderType;
```

### AiRole

Message roles:

```c
typedef enum {
    AI_ROLE_USER,
    AI_ROLE_ASSISTANT,
    AI_ROLE_SYSTEM
} AiRole;
```

### AiStopReason

Response stop reasons:

```c
typedef enum {
    AI_STOP_REASON_END_TURN,
    AI_STOP_REASON_MAX_TOKENS,
    AI_STOP_REASON_STOP_SEQUENCE,
    AI_STOP_REASON_TOOL_USE
} AiStopReason;
```

### AiContentType

Content block types:

```c
typedef enum {
    AI_CONTENT_TYPE_TEXT,
    AI_CONTENT_TYPE_TOOL_USE,
    AI_CONTENT_TYPE_TOOL_RESULT
} AiContentType;
```

### AiImageSize

Image generation sizes:

```c
typedef enum {
    AI_IMAGE_SIZE_AUTO,       /* Provider default */
    AI_IMAGE_SIZE_256,        /* 256x256 */
    AI_IMAGE_SIZE_512,        /* 512x512 */
    AI_IMAGE_SIZE_1024,       /* 1024x1024 */
    AI_IMAGE_SIZE_1024_1792,  /* 1024x1792 (portrait) */
    AI_IMAGE_SIZE_1792_1024,  /* 1792x1024 (landscape) */
    AI_IMAGE_SIZE_CUSTOM      /* Custom size string */
} AiImageSize;
```

### AiImageQuality

Image quality levels:

```c
typedef enum {
    AI_IMAGE_QUALITY_AUTO,     /* Provider default */
    AI_IMAGE_QUALITY_STANDARD, /* Standard quality */
    AI_IMAGE_QUALITY_HD        /* High definition */
} AiImageQuality;
```

### AiImageStyle

Image generation styles:

```c
typedef enum {
    AI_IMAGE_STYLE_AUTO,    /* Provider default */
    AI_IMAGE_STYLE_VIVID,   /* Vivid, dramatic */
    AI_IMAGE_STYLE_NATURAL  /* Natural, realistic */
} AiImageStyle;
```

### AiImageResponseFormat

Image response formats:

```c
typedef enum {
    AI_IMAGE_RESPONSE_URL,    /* Return URL */
    AI_IMAGE_RESPONSE_BASE64  /* Return base64 data */
} AiImageResponseFormat;
```

## Type Macros

Each GObject type has standard type macros:

```c
/* Type checking */
AI_IS_CLIENT(obj)
AI_IS_MESSAGE(obj)
AI_IS_RESPONSE(obj)

/* Type casting */
AI_CLIENT(obj)
AI_MESSAGE(obj)
AI_RESPONSE(obj)

/* Type retrieval */
AI_TYPE_CLIENT
AI_TYPE_MESSAGE
AI_TYPE_RESPONSE
```

## Memory Management

### Automatic Cleanup

Use `g_autoptr()` for automatic cleanup:

```c
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
g_autoptr(AiMessage) msg = ai_message_new_user("Hello");
g_autoptr(AiResponse) response = NULL;
g_autoptr(GError) error = NULL;
```

Use `g_autofree` for strings:

```c
g_autofree gchar *text = ai_response_get_text(response);
```

### Ownership Transfer

Functions documented as `(transfer full)` return ownership:

```c
/* Caller owns the returned object */
AiMessage *ai_message_new_user(const gchar *content);  /* (transfer full) */

/* Caller owns the returned string */
gchar *ai_response_get_text(AiResponse *self);  /* (transfer full) */
```

Functions documented as `(transfer none)` return borrowed references:

```c
/* Caller does NOT own - do not free */
const gchar *ai_client_get_model(AiClient *self);  /* (transfer none) */
AiConfig *ai_client_get_config(AiClient *self);    /* (transfer none) */
```

## Error Handling

All async operations use `GError`:

```c
g_autoptr(GError) error = NULL;
response = ai_provider_chat_finish(provider, result, &error);

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
                /* Handle other errors */
                break;
        }
    }
    g_printerr("Error: %s\n", error->message);
}
```

## Thread Safety

- Client objects should be used from a single thread
- GMainLoop integration is required for async operations
- Use `GTask` for thread-safe async patterns
