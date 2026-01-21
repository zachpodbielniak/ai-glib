# AiProvider

Interface for AI provider operations.

## Description

`AiProvider` is the core interface that all provider clients implement. It defines the common operations for interacting with AI models, including chat completion and model listing.

## Implemented By

- AiClaudeClient
- AiOpenAIClient
- AiGeminiClient
- AiGrokClient
- AiOllamaClient

## Functions

### ai_provider_get_provider_type

```c
AiProviderType
ai_provider_get_provider_type(AiProvider *provider);
```

Gets the provider type identifier.

**Parameters:**
- `provider`: an AiProvider

**Returns:** the AiProviderType enum value

---

### ai_provider_get_name

```c
const gchar *
ai_provider_get_name(AiProvider *provider);
```

Gets the human-readable provider name.

**Parameters:**
- `provider`: an AiProvider

**Returns:** `(transfer none)`: the provider name (e.g., "Claude", "OpenAI")

---

### ai_provider_get_default_model

```c
const gchar *
ai_provider_get_default_model(AiProvider *provider);
```

Gets the default model for this provider.

**Parameters:**
- `provider`: an AiProvider

**Returns:** `(transfer none)`: the default model name

---

### ai_provider_chat_async

```c
void
ai_provider_chat_async(
    AiProvider         *provider,
    GList              *messages,
    const gchar        *system_prompt,
    gint                max_tokens,
    GList              *tools,
    GCancellable       *cancellable,
    GAsyncReadyCallback callback,
    gpointer            user_data
);
```

Sends a chat completion request asynchronously.

**Parameters:**
- `provider`: an AiProvider
- `messages`: `(element-type AiMessage)`: list of messages
- `system_prompt`: `(nullable)`: system prompt to use
- `max_tokens`: maximum tokens to generate
- `tools`: `(element-type AiTool) (nullable)`: list of tools
- `cancellable`: `(nullable)`: a GCancellable
- `callback`: callback when complete
- `user_data`: data for callback

---

### ai_provider_chat_finish

```c
AiResponse *
ai_provider_chat_finish(
    AiProvider   *provider,
    GAsyncResult *result,
    GError      **error
);
```

Finishes an async chat completion request.

**Parameters:**
- `provider`: an AiProvider
- `result`: the GAsyncResult
- `error`: `(out) (optional)`: return location for error

**Returns:** `(transfer full) (nullable)`: the AiResponse, or NULL on error

## Example

```c
#include <ai-glib.h>

static void
on_chat_complete(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
    }
    else
    {
        g_autofree gchar *text = ai_response_get_text(response);
        g_print("Response: %s\n", text);
    }

    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiClaudeClient) client = ai_claude_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    /* Get provider info */
    AiProvider *provider = AI_PROVIDER(client);
    g_print("Provider: %s\n", ai_provider_get_name(provider));
    g_print("Default model: %s\n", ai_provider_get_default_model(provider));

    /* Send chat request */
    ai_provider_chat_async(
        provider,
        messages,
        "You are helpful.",  /* system prompt */
        4096,                /* max tokens */
        NULL,                /* tools */
        NULL,                /* cancellable */
        on_chat_complete,
        loop
    );

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
```

## See Also

- [AiClient](ai-client.md) - Base client class
- [AiMessage](ai-message.md) - Message class
- [AiResponse](ai-response.md) - Response class
