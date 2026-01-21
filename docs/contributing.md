# Contributing to ai-glib

Thank you for your interest in contributing to ai-glib!

## Development Setup

### Prerequisites

- GCC with gnu89 support
- GLib >= 2.56
- libsoup >= 3.0
- json-glib >= 1.6
- GObject Introspection (optional, for bindings)

### Fedora

```bash
dnf install gcc make glib2-devel libsoup3-devel json-glib-devel \
            gobject-introspection-devel
```

### Ubuntu/Debian

```bash
apt install build-essential libglib2.0-dev libsoup-3.0-dev \
            libjson-glib-dev gobject-introspection
```

### Building

```bash
git clone https://gitlab.com/your-username/ai-glib.git
cd ai-glib
make
make test
```

## Code Style

### C Standard

- Use `gnu89` exclusively
- Compile with `gcc`

### Formatting

- **Indentation**: TAB (4 spaces width)
- **Naming conventions**:
  - Types: `AiTypeName` (PascalCase)
  - Functions: `ai_type_name_method()` (lowercase_snake_case)
  - Macros/defines: `AI_MACRO_NAME` (UPPERCASE_SNAKE_CASE)
  - Variables: `lowercase_snake_case`
- **Comments**: Always use `/* comment */`, never `//`

### Function Style

```c
/* Function declaration */
AiResponse *
ai_client_chat_sync(
    AiClient     *self,
    GList        *messages,
    GCancellable *cancellable,
    GError      **error
);

/* Function definition */
AiResponse *
ai_client_chat_sync(
    AiClient     *self,
    GList        *messages,
    GCancellable *cancellable,
    GError      **error
){
    g_autoptr(AiResponse) response = NULL;

    g_return_val_if_fail(AI_IS_CLIENT(self), NULL);

    /* implementation */

    return (AiResponse *)g_steal_pointer(&response);
}
```

### Memory Management

Always use GLib's automatic cleanup:

```c
/* Use g_autoptr for GObjects */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
g_autoptr(AiMessage) msg = ai_message_new_user("Hello");
g_autoptr(GError) error = NULL;

/* Use g_autofree for strings */
g_autofree gchar *text = ai_response_get_text(response);

/* Use g_steal_pointer for ownership transfer */
return (AiResponse *)g_steal_pointer(&response);
```

### GObject Introspection

All public APIs must include GIR annotations:

```c
/**
 * ai_response_get_text:
 * @self: an #AiResponse
 *
 * Gets the concatenated text content from all text blocks.
 *
 * Returns: (transfer full) (nullable): the text content, free with g_free()
 */
gchar *
ai_response_get_text(AiResponse *self);
```

## Adding a New Provider

1. Create header and source files:
   - `src/providers/ai-<name>-client.h`
   - `src/providers/ai-<name>-client.c`

2. Extend `AiClient` and implement interfaces:
   - `AiProvider` interface
   - `AiStreamable` interface (for streaming support)

3. Update build system:
   - Add to `PUBLIC_HEADERS` in Makefile
   - Add to `LIB_SOURCES` in Makefile

4. Add tests:
   - Create `tests/test-<name>-client.c`

5. Add documentation:
   - Create `docs/providers/<name>.md`

6. Update umbrella header:
   - Add `#include "providers/ai-<name>-client.h"` to `src/ai-glib.h`

## Testing

### Running Tests

```bash
make test              # Run all tests
make test-verbose      # Run with verbose output
```

### Running Specific Tests

```bash
./build/tests/test-config
G_TEST_VERBOSE=1 ./build/tests/test-message
```

### Writing Tests

Tests use GLib's GTest framework:

```c
static void
test_message_new_user(void)
{
    g_autoptr(AiMessage) msg = ai_message_new_user("Hello");

    g_assert_nonnull(msg);
    g_assert_cmpint(ai_message_get_role(msg), ==, AI_ROLE_USER);

    g_autofree gchar *text = ai_message_get_text(msg);
    g_assert_cmpstr(text, ==, "Hello");
}

int main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ai-glib/message/new-user", test_message_new_user);

    return g_test_run();
}
```

## Submitting Changes

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Make your changes following the code style
4. Add tests for new functionality
5. Ensure all tests pass: `make test`
6. Commit with clear messages
7. Push and create a merge request

## Commit Messages

- Use present tense: "Add feature" not "Added feature"
- Use imperative mood: "Fix bug" not "Fixes bug"
- Keep first line under 72 characters
- Reference issues when applicable

Example:

```
Add streaming support for Gemini provider

Implement the AiStreamable interface for AiGeminiClient to enable
real-time streaming of responses.

Closes #42
```

## License

By contributing to ai-glib, you agree that your contributions will be
licensed under the AGPL-3.0 license.
