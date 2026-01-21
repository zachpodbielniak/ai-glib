# ai-glib

A GLib/GObject-based C library for interacting with AI providers.

## Features

- **Multiple Providers**: Claude, OpenAI, Gemini, Grok, Ollama
- **GObject-based**: Full GObject type system integration
- **Async Support**: Non-blocking operations via GAsyncResult/GTask
- **Streaming**: Real-time response streaming with signals
- **Tool Use**: Function calling / tool use support
- **GObject Introspection**: Use from Python, JavaScript, etc.
- **Environment Variables**: Standard env var configuration
- **AGPLv3 Licensed**: Free and open source

## Requirements

- GLib >= 2.56
- GObject >= 2.56
- GIO >= 2.56
- libsoup >= 3.0
- json-glib >= 1.6
- GCC with gnu89 support

### Fedora

```bash
dnf install glib2-devel libsoup3-devel json-glib-devel \
            gobject-introspection-devel
```

### Ubuntu/Debian

```bash
apt install libglib2.0-dev libsoup-3.0-dev libjson-glib-dev \
            gobject-introspection
```

## Building

```bash
make
make test
sudo make install
```

### Build Options

```bash
make DEBUG=1        # Debug build with symbols
make ASAN=1         # Address sanitizer
make UBSAN=1        # Undefined behavior sanitizer
```

## Quick Start

```c
#include <ai-glib.h>

int main(void)
{
    g_autoptr(AiClaudeClient) client = NULL;
    g_autoptr(AiMessage) msg = NULL;
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;
    GList *messages = NULL;

    /* Uses ANTHROPIC_API_KEY from environment */
    client = ai_claude_client_new();
    ai_client_set_model(AI_CLIENT(client), "claude-sonnet-4-20250514");

    msg = ai_message_new_user("What is 2 + 2?");
    messages = g_list_append(NULL, msg);

    /* Synchronous call for simplicity */
    response = ai_client_chat_sync(AI_CLIENT(client), messages, NULL, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return 1;
    }

    g_autofree gchar *text = ai_response_get_text(response);
    g_print("Response: %s\n", text);

    g_list_free(messages);
    return 0;
}
```

## Environment Variables

| Variable | Provider | Description |
|----------|----------|-------------|
| `ANTHROPIC_API_KEY` | Claude | Anthropic API key |
| `OPENAI_API_KEY` | OpenAI | OpenAI API key |
| `OPENAI_BASE_URL` | OpenAI | Custom base URL (optional) |
| `GEMINI_API_KEY` | Gemini | Google AI API key |
| `XAI_API_KEY` | Grok | xAI API key |
| `OLLAMA_HOST` | Ollama | Ollama server URL (default: http://localhost:11434) |

## Documentation

See the [docs/](docs/) directory for full documentation:

- [Getting Started](docs/getting-started.md)
- [Architecture](docs/architecture.md)
- [Configuration](docs/configuration.md)
- [API Reference](docs/api-reference/)
- [Examples](docs/examples/)

## License

AGPLv3 - See [LICENSE](LICENSE) for details.
