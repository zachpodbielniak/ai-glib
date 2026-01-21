# Examples

Code examples and tutorials for ai-glib.

## Quick Start Examples

| Example | Description |
|---------|-------------|
| [Simple Chat](simple-chat.md) | Basic chat completion with any provider |
| [Multi-Provider](multi-provider.md) | Using multiple providers in one application |
| [Streaming](streaming.md) | Real-time streaming responses |
| [Tool Use](tool-use.md) | Function calling / tool use |

## Building Examples

Examples are built automatically with the library:

```bash
make
```

Run an example:

```bash
./build/examples/simple-chat-claude
./build/examples/simple-chat-openai
./build/examples/simple-chat-ollama
```

## Provider-Specific Examples

The library includes simple chat examples for each provider:

| File | Provider | Requires |
|------|----------|----------|
| `simple-chat-claude.c` | Claude | `ANTHROPIC_API_KEY` |
| `simple-chat-openai.c` | OpenAI | `OPENAI_API_KEY` |
| `simple-chat-gemini.c` | Gemini | `GEMINI_API_KEY` |
| `simple-chat-grok.c` | Grok | `XAI_API_KEY` |
| `simple-chat-ollama.c` | Ollama | Ollama server running |

## Common Patterns

### Async Chat Request

```c
#include <ai-glib.h>

static void
on_response(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GMainLoop *loop = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        g_main_loop_quit(loop);
        return;
    }

    g_autofree gchar *text = ai_response_get_text(response);
    g_print("%s\n", text);

    g_main_loop_quit(loop);
}

int main(void)
{
    g_autoptr(AiClaudeClient) client = ai_claude_client_new();
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello!");
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    GList *messages = g_list_append(NULL, msg);

    ai_provider_chat_async(AI_PROVIDER(client), messages,
                           NULL, 4096, NULL, NULL, on_response, loop);

    g_main_loop_run(loop);
    g_list_free(messages);
    return 0;
}
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
                g_printerr("Check your API key\n");
                break;
            case AI_ERROR_RATE_LIMITED:
                g_printerr("Rate limited - try again later\n");
                break;
            default:
                g_printerr("Error: %s\n", error->message);
                break;
        }
    }
}
```

### Memory Management

```c
/* Use g_autoptr for automatic cleanup */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
g_autoptr(AiMessage) msg = ai_message_new_user("Hello");
g_autoptr(AiResponse) response = NULL;
g_autoptr(GError) error = NULL;

/* Use g_autofree for strings */
g_autofree gchar *text = ai_response_get_text(response);

/* Use g_steal_pointer for ownership transfer */
g_autoptr(AiMessage) msg = ai_message_new_user("Hello");
messages = g_list_append(messages, g_steal_pointer(&msg));
```

## Compiling Your Own Examples

```bash
# Compile with pkg-config
gcc -o myapp myapp.c $(pkg-config --cflags --libs ai-glib-1.0)

# Or manually specify flags
gcc -o myapp myapp.c \
    -I/usr/local/include/ai-glib-1.0 \
    -L/usr/local/lib \
    -lai-glib-1.0 \
    $(pkg-config --cflags --libs glib-2.0 gobject-2.0 gio-2.0 libsoup-3.0 json-glib-1.0)
```

## See Also

- [API Reference](../api-reference/index.md) - Complete API documentation
- [Providers](../providers/index.md) - Provider-specific information
- [Configuration](../configuration.md) - Configuration reference
