# Getting Started with ai-glib

This guide will help you get started using ai-glib for interacting with AI providers.

## Prerequisites

- GLib >= 2.56
- GObject >= 2.56
- GIO >= 2.56
- libsoup >= 3.0
- json-glib >= 1.6
- GCC with gnu89 support

### Fedora

```bash
dnf install glib2-devel libsoup3-devel json-glib-devel
```

### Ubuntu/Debian

```bash
apt install libglib2.0-dev libsoup-3.0-dev libjson-glib-dev
```

## Building

```bash
git clone https://gitlab.com/your-username/ai-glib.git
cd ai-glib
make
make test
```

## Quick Start (Simple API)

The fastest way to call an LLM from C:

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

`ai_simple_new()` reads the default provider and model from
`~/.config/ai-glib/config.yaml` and environment variables. See
[AiSimple](api-reference/ai-simple.md) for multi-turn conversations
and more options.

## Full API Usage

For async operations, streaming, and tool use, use the full provider API.

### 1. Include the header

```c
#include <ai-glib.h>
```

### 2. Create a client

```c
/* Uses ANTHROPIC_API_KEY from environment */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();

/* Or with explicit API key */
g_autoptr(AiClaudeClient) client = ai_claude_client_new_with_key("your-api-key");
```

### 3. Create a message

```c
g_autoptr(AiMessage) msg = ai_message_new_user("Hello, Claude!");
```

### 4. Send a chat request

```c
GList *messages = g_list_append(NULL, msg);

ai_provider_chat_async(
    AI_PROVIDER(client),
    messages,
    NULL,  /* system prompt */
    4096,  /* max tokens */
    NULL,  /* tools */
    NULL,  /* cancellable */
    on_chat_complete,
    user_data
);
```

### 5. Handle the response

```c
static void
on_chat_complete(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    g_autoptr(AiResponse) response = NULL;
    g_autoptr(GError) error = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);
    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        return;
    }

    g_autofree gchar *text = ai_response_get_text(response);
    g_print("Response: %s\n", text);
}
```

## Compilation

Compile your program with pkg-config:

```bash
gcc -o myprogram myprogram.c $(pkg-config --cflags --libs ai-glib-1.0)
```

Or use the library directly:

```bash
gcc -o myprogram myprogram.c \
    -I/usr/local/include/ai-glib-1.0 \
    -L/usr/local/lib \
    -lai-glib-1.0 \
    $(pkg-config --cflags --libs glib-2.0 gobject-2.0 gio-2.0 libsoup-3.0 json-glib-1.0)
```

## Environment Variables

Set these environment variables to configure API keys. Some providers support
alternative environment variable names (listed in order of precedence):

### HTTP API Providers

| Provider | Environment Variables | Notes |
|----------|----------------------|-------|
| Claude   | `ANTHROPIC_API_KEY`, `CLAUDE_API_KEY` | |
| OpenAI   | `OPENAI_API_KEY` | Also supports `OPENAI_BASE_URL` |
| Gemini   | `GEMINI_API_KEY` | |
| Grok     | `XAI_API_KEY`, `GROK_API_KEY` | |
| Ollama   | `OLLAMA_API_KEY` | Optional - also supports `OLLAMA_HOST` (default: http://localhost:11434) |

### CLI Wrapper Providers

CLI providers use their own tool's authentication and don't require
API key environment variables in ai-glib. You can optionally override
the executable path:

| Provider | Environment Variables | Notes |
|----------|----------------------|-------|
| Claude Code | `CLAUDE_CODE_PATH` | Optional path override for `claude` CLI |
| OpenCode | `OPENCODE_PATH` | Optional path override for `opencode` CLI |

Additionally, the default provider and model can be set via environment variables:

| Variable | Description |
|----------|-------------|
| `AI_GLIB_DEFAULT_PROVIDER` | Default provider name (e.g., `ollama`, `claude`) |
| `AI_GLIB_DEFAULT_MODEL` | Default model name (e.g., `qwen2.5:7b`) |

These override config file values but are overridden by programmatic `set_*()` calls.
See [Configuration](configuration.md) for the full priority chain.

## Next Steps

- See the [Architecture](architecture.md) document for design details
- Read the [Configuration](configuration.md) guide for advanced setup
- Check out the `examples/` directory for complete working examples
