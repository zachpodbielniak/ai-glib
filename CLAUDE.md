# CLAUDE.md - ai-glib

Instructions for Claude Code when working on this project.

## Project Overview

ai-glib is a GLib/GObject-based C library for interacting with AI providers
(Claude, OpenAI, Gemini, Grok, Ollama). It follows GObject conventions and
provides both sync and async APIs.

## Build Commands

```bash
make                    # Release build -> build/release/
make DEBUG=1            # Debug build   -> build/debug/   (coexists)
make GIR=1              # Also build GObject introspection (.gir/.typelib)
make test               # Run tests for the current build type
make test-verbose       # Run tests with verbose output
make examples           # Build example binaries
make clean              # Remove current build type (build/release/ or build/debug/)
make clean-all          # Remove the entire build/ tree
make install            # Install to PREFIX
make install GIR=1      # Also install .gir/.typelib into standard GI paths
```

Build artifacts land under `build/release/` by default and `build/debug/`
when `DEBUG=1` — the two trees coexist so you can flip between them with
no rebuild. Tests live under `build/<type>/tests/`, examples under
`build/<type>/examples/`, the shared library at `build/<type>/libai-glib-1.0.so.*`.
`ASAN=1` and `UBSAN=1` add the corresponding sanitizers.

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

The library is fully introspectable. `make test` runs the `test-gir-clean`
gate which fails on any `g-ir-scanner` warning. `make test-gi` exercises
the bindings via PyGObject.

All public APIs must include GIR annotations:
- `(transfer none)` / `(transfer full)` / `(transfer container)` for ownership
- `(nullable)` for nullable parameters/returns
- `(out)` / `(out) (optional)` for output parameters
- `(element-type T)` for `GList` / `GPtrArray` / `GHashTable`
- For custom callback registration, put all three on the callback param
  itself (HarfBuzz convention):
  `(scope notified) (closure user_data) (destroy user_data_free)` —
  do NOT put `(closure)` on user_data when it directly follows the
  callback; the scanner auto-detects.
- Docs above the implementation in `.c` only (avoid duplicating in `.h`,
  the scanner warns on duplicates).

When adding a new client config knob, prefer
`g_object_class_install_property` over plain getter/setters so bindings
get native property syntax (`obj.props.thing = ...`).

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
├── bin/                       # Installable CLI binaries
│   └── ai.c                   # `ai` command-line front-end
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
| Claude Code | `CLAUDE_CODE_PATH` (override `claude` path), `OLLAMA_PATH` (override `ollama` launcher for `ollama/` models) |
| Claude Code (tmux) | `CLAUDE_CODE_PATH`, `TMUX_PATH`, `OLLAMA_PATH` |
| OpenCode | `OPENCODE_PATH` (override `opencode` path) |
| Grok Build | `GROK_PATH` (override `grok` path) |

## CLI provider options

Every option a wrapped CLI accepts is a GObject property, never a bespoke
setter-only field: that is what makes it reachable from bindings and from
`ai --set` with no extra plumbing. When one of these CLIs gains a flag, add
a property and emit it — nothing else needs to change.

Two conventions worth keeping:

- **Flags that only apply in a mode are gated on that mode**, not left to
  the caller to remember. `--include-partial-messages` is emitted only when
  streaming and `--fork-session` only alongside `--resume`, because claude
  rejects them otherwise.
- **Shared "how to run" flags live in one helper** (`emit_session_args` for
  claude-code, `emit_execution_args` for opencode and grok-build) called by
  both `build_argv` and the no-text re-prompt path. A retry that silently
  dropped `--auto` would sit waiting for an approval nobody is there to
  give.

Per-CLI equivalents of the same idea:

| Concept | claude-code | opencode | grok-build |
|---|---|---|---|
| Bypass approval | `--dangerously-skip-permissions` | `--auto` | `--permission-mode bypassPermissions` |
| Reasoning effort | `--effort` | `--variant` | `--reasoning-effort` |
| Agent profile | `--agent` | `--agent` | `--agent` |

## Ollama-as-transport (`ollama/` models)

The `claude-code` and `claude-tmux` CLI providers detect a model name that
begins with `ollama/`. Such models are routed through Ollama as the transport:
instead of spawning `claude` directly they spawn

```
ollama launch claude --model <model-after-prefix> -- <the claude args>
```

(claude's own `--model` is omitted; `claude` must still be installed). Models
without the prefix are unchanged. The prefix logic lives in one place,
`src/providers/ai-claude-launch.{h,c}` (private), and is shared by both
providers and the retry path. Inspect the exact command with
`ai -p claude-code -m ollama/<id> --dry-run "hi"`.

## Grok Build (`grok-build`)

`AiGrokBuildClient` wraps xAI's `grok` CLI headlessly. Three things about it
are load-bearing and easy to get wrong:

1. **The prompt is piped, not passed.** argv carries `--prompt-file
   /dev/stdin`; `build_stdin()` produces the prompt and the caller writes it
   to the child's stdin. That is the C equivalent of `--prompt-file
   <(echo ...)` and it is what keeps long conversations under `ARG_MAX`.
2. **Exit status is not a reliable error signal.** A rejected
   `--reasoning-effort` prints `{"type":"error","message":...}` on stdout and
   exits **0**; a rejected `--model` exits 1. Always parse stdout first and
   check for `type == "error"` before trusting the status.
3. **`--output-format json` is grok's own camelCase shape** (`text`,
   `stopReason`, `sessionId`) — *not* Claude Code's `result` envelope.
   `--output-format streaming-messages-json` *is* Anthropic-shaped, so the
   streaming parser mirrors claude-code's, minus the whole-message
   `assistant` line (deltas already carried that text).

Effort levels are `low|medium|high|xhigh`; `AI_EFFORT_MAX` is folded onto
`xhigh` because grok rejects `max`. Inspect the command with
`ai -p grok-build --dry-run "hi"`.

Every JSON member is read through the type-checked `grok_get_*()` helpers
rather than json-glib's `*_member_with_default()`, which emit criticals on a
type mismatch — subprocess stdout is untrusted input and must not be able to
abort a fatal-warnings test run. Keep new fields on those helpers.

Tests live in two files and both are load-bearing:

- `tests/test-grok-build-client.c` — argv/stdin/parsers in isolation.
- `tests/test-grok-build-subprocess.c` — a stub `grok` script recording its
  argv, cwd and stdin. This is what proves the prompt reaches the child at
  all; a regression there would leave every run "succeeding" against an
  empty prompt. Configure the stub by staging files in its directory
  (`stdout`, `stdout.<n>` for the Nth call, `stderr`, `exit`, `sleep`).

## CLI binary (`ai`)

`bin/ai.c` builds a small `ai` front-end (provider/model/system/stream/
interactive/`--dry-run`/`--list-providers`). It is built by `make` and
installed by `make install`. See `docs/cli.org`.

`--set PROP=VALUE` (repeatable) sets any GObject property on the resolved
provider, so a new provider knob needs no new flag here — add the property
and it is reachable. A bare `--set NAME` sets a boolean true. Unknown names
print the provider's writable properties; read-only ones and unparseable
values are errors, not silent no-ops.

`--dry-run` calls `AiCliClientClass.build_argv` through the vtable, so it
covers every CLI provider (claude-tmux is the exception: it drives claude
through tmux rather than the argv pipeline, so it keeps a dedicated branch
and its own internal header). Do not add per-provider dry-run branches.

`tests/test-ai-cli.c` spawns the built binary (hence `test:` depends on
`$(BIN_BINARIES)`) and drives it against a stub `grok` via `GROK_PATH`, so
the CLI wiring is covered without network access.

## Model Defines

Each provider header defines model constants. Use these instead of hardcoding strings:

```c
/* Claude */
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_SONNET_5);
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_OPUS_5);
ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_FABLE_5);
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

/* Grok Build (CLI) — note the AI_GROK_BUILD_ prefix; the AI_GROK_MODEL_*
   defines above are xAI API ids and are NOT valid `grok --model` values */
ai_cli_client_set_model(AI_CLI_CLIENT(client), AI_GROK_BUILD_MODEL_GROK_4_6);
```

See `src/providers/ai-*-client.h` for complete model lists.

## Testing

Tests use GLib's GTest framework. Each component has its own test file:

    tests/test-<component>.c

Run specific test (substitute `debug` for `release` when built with `DEBUG=1`):

```bash
./build/release/tests/test-config
```

Run with verbose output:

```bash
G_TEST_VERBOSE=1 ./build/release/tests/test-config
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

## Web Search Subsystem

The `web_search` tool is backed by the `AiSearchProvider` interface
(`src/convenience/ai-search-provider.{h,c}`). A search returns a
`GList<AiSearchResult>` (transfer full) and is shaped by an `AiSearchOptions`
GObject (count, freshness, safesearch, country, language, site, fetch_content).
Backends:

- `ai-bing-search.c` / `ai-brave-search.c` — API providers; `api-key` +
  `endpoint` construct properties; share the robust JSON GET helper
  `ai-search-http.c` (retries/backoff/429/Retry-After/status→`AI_ERROR`).
- `ai-duckduckgo-search.c` — keyless, scrapes the DDG "lite" HTML endpoint via
  libxml2; best-effort (degrades to an empty list, never an error).

`ai_search_provider_new_default()` picks Brave/Bing from the environment, else
DuckDuckGo. The executor (`tool_web_search` in `ai-tool-executor.c`) builds the
options, caches the formatted result (5 min), and — when `fetch_content` is set
— enriches the top results via the existing `web_fetch_get` + `html_to_text`.

`ai-search-http.{c,h}` is **private** (the `AI_GLIB_COMPILATION` guard, like
`ai-openai-shared`): in `LIB_SOURCES` but NOT `PUBLIC_HEADERS`/`ai-glib.h`.

### Adding a search provider

1. Create `src/convenience/ai-<name>-search.{h,c}`
2. Implement `AiSearchProvider` (the single `search` vfunc returning a
   `GList<AiSearchResult>`)
3. Add the `.c` to `LIB_SOURCES` and the public `.h` to `PUBLIC_HEADERS`
4. Add it to `src/ai-glib.h`
5. Cover it in `tests/test-search-providers.c` (use the `TServer` harness +
   the `endpoint` property to test against loopback)
6. Document it in `docs/web-search.org` + `docs/api-reference/ai-search-provider.org`

## Image Generation Subsystem

Image generation hangs off the `AiImageGenerator` interface
(`src/core/ai-image-generator.{h,c}`), implemented by the OpenAI, Gemini and
Grok clients. The request type (`src/model/ai-image-request.c`) is deliberately a
**superset** of every parameter the supported APIs accept — roughly thirty of
them — because those APIs overlap only partly and reject anything they do not
recognise.

The rule that makes that workable: each provider projects the request through
the chosen model's `AiImageModelInfo` and calls `ai_image_request_validate()`
before serialising. Unsupported parameters are dropped with a `g_debug` by
default, or raise `AI_ERROR_INVALID_REQUEST` under `AI_IMAGE_VALIDATE_STRICT`.
**Never add a parameter to a provider's builder without a capability gate** —
that is exactly how the `response_format`-to-GPT-Image bug got in.

Pieces:

- `src/model/ai-image.{h,c}` — `AiImage`, the input-side payload (bytes, MIME,
  optional role label). Roles are folded into the prompt text because no wire
  format has a field for them.
- `src/core/ai-image-capabilities.{h,c}` — capability flags, `AiImageModelInfo`,
  and the validator.
- `src/providers/ai-image-shared.{h,c}` — **private**. OpenAI-compatible JSON and
  multipart builders, the Gemini parts builder, status→error mapping, and an
  async send with retry/backoff.

Three things to know before touching the send path:

1. Retries must build a **fresh** `SoupMessage`; a message whose body stream has
   been consumed cannot be resent. That is why the body is passed separately —
   it cannot be read back off a `SoupMessage`.
2. Timeout sources must attach to the **thread-default** context, not the global
   default `g_timeout_add()` uses. A synchronous caller drives a nested loop on a
   private context that a global-default timer never reaches.
3. `ai_image_generator_generate_image()` (sync) runs that nested loop on a
   private context deliberately. A mock server on the default context will never
   be dispatched while it is in flight — run test servers on their own thread.

### Adding an image-generation provider

1. Implement `AiImageGenerator` on the client: `generate_image_async`,
   `generate_image_finish`, `get_default_model`, `list_image_models`.
2. Declare a static table of `AiImageModelInfo` in `list_image_models` — id,
   capability flags, `max_count`, `max_reference_images`, sizes or aspect
   ratios, quality vocabulary, notes. **That table is the registration.**
3. In `generate_image_async`: resolve the model, `get_model_info`, call
   `ai_image_request_validate()`, build the body, then
   `ai_image_shared_send_async()`.
4. For an OpenAI-compatible API, `ai_image_shared_build_openai_json()` and
   `ai_image_shared_parse_openai_response()` do the work — see the Grok client,
   which is the minimal example at roughly 80 lines.
5. Emit `image-progress` when the response is parsed.
6. Cover the wire format in `tests/test-image-serialize.c` and the round trip in
   `tests/test-image-generator.c` (loopback server + `ai_config_set_base_url()`).
7. Document it in `docs/providers/<name>.org` with a capability matrix.

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

### Tool Calling (HTTP providers)

Tool definition + multi-turn loop using built-in and user-supplied tools:

```c
g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();
g_autoptr(AiTool) tool = ai_tool_new("get_weather", "Look up weather");
ai_tool_add_parameter(tool, "location", "string", "City", TRUE);

ai_tool_executor_register_callback(
    exec, tool, on_weather, /* user_data */ NULL, /* free */ NULL);

g_autofree gchar *answer = ai_tool_executor_run(
    exec, AI_PROVIDER(client), messages, NULL, 4096, NULL, &err);
```

For manual loops (no AiToolExecutor), construct tool-result messages with
the tool name so Gemini's wire format round-trips:

```c
result_msg = ai_message_new_tool_result_with_name(
    ai_tool_use_get_id(tu),
    ai_tool_use_get_name(tu),
    result_text,
    /* is_error */ FALSE);
```

`ai_message_new_tool_result()` (no name) still works for everything except
Gemini; if in doubt, use `_with_name`.

Per-provider wire formats are handled inside each provider's
`build_request`. The shared OpenAI-style serializer lives at
`src/providers/ai-openai-shared.{c,h}` (private; consumed by OpenAI, Grok,
and Ollama).

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
