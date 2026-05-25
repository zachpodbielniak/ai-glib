# Using ai-glib from Python (PyGObject)

ai-glib ships full GObject Introspection metadata, so any language with
a GI binding can drive the library natively. The most common consumer is
Python via PyGObject; this page walks through a complete example.

## Prerequisites

```bash
# Fedora
sudo dnf install python3-gobject gobject-introspection-devel

# Debian / Ubuntu
sudo apt install python3-gi gobject-introspection libgirepository1.0-dev
```

Build and install ai-glib with GIR generation enabled:

```bash
make GIR=1
sudo make install GIR=1
sudo ldconfig
```

The `.gir` lands in `/usr/share/gir-1.0/AiGlib-1.0.gir` and the
`.typelib` in `/usr/lib64/girepository-1.0/AiGlib-1.0.typelib` (or
`/usr/lib/...` on multilib distros). Both directories are on the standard
`GI_TYPELIB_PATH` and `XDG_DATA_DIRS` search paths, so consumers should
find them automatically.

## Hello world

```python
import gi
gi.require_version("AiGlib", "1.0")
from gi.repository import AiGlib

client = AiGlib.ClaudeClient.new()
client.props.model = "claude-sonnet-4-5"

msg = AiGlib.Message.new_user("Say hello.")
reply = client.chat_sync([msg], None)
print(reply.get_text())
```

`client.props.model = "..."` works because the C library exposes `model`
as a GObject property (see [AiClient](../api-reference/ai-client.md#properties)).
The `chat_sync()` method is the synchronous version of
`ai_provider_chat_async()` / `ai_provider_chat_finish()`.

## Tool calling end-to-end

```python
import gi
gi.require_version("AiGlib", "1.0")
from gi.repository import AiGlib


def on_get_weather(tool_use, cancellable, user_data):
    """Tool callback. Return the result as a string."""
    location = tool_use.get_input_string("location")
    return f'{{"location":"{location}","temp":72,"conditions":"sunny"}}'


# Wire up the executor with the built-in tools plus our custom one
exec_ = AiGlib.ToolExecutor.new()
weather = AiGlib.Tool.new("get_weather", "Look up the weather")
weather.add_parameter("location", "string", "City name", True)
exec_.register_callback(weather, on_get_weather, None)

# Drive the full multi-turn loop
client = AiGlib.ClaudeClient.new()
msg = AiGlib.Message.new_user("What's the weather in Miami?")
answer = exec_.run(client, [msg],
                   "You are a helpful assistant. Use tools when needed.",
                   4096, None)
print(answer)
```

## Async usage

PyGObject can drive `_async` / `_finish` pairs from a `GLib.MainLoop`:

```python
import gi
gi.require_version("AiGlib", "1.0")
from gi.repository import AiGlib, GLib

loop = GLib.MainLoop()
client = AiGlib.ClaudeClient.new()
client.props.model = "claude-sonnet-4-5"


def on_done(provider, result, user_data):
    response, err = provider.chat_finish(result)
    if err is not None:
        print("error:", err.message)
    else:
        print(response.get_text())
    loop.quit()


msg = AiGlib.Message.new_user("Hi")
AiGlib.Provider.chat_async(client, [msg], None, 4096, None, None, on_done, None)
loop.run()
```

## Connecting to streaming signals

```python
client = AiGlib.ClaudeClient.new()

def on_delta(streamable, chunk):
    print(chunk, end="", flush=True)

client.connect("delta", on_delta)
```

## Smoke test

`tests/test-gi-bindings.py` in the repo exercises the same surface area
end-to-end. Run it locally with:

```bash
make test-gi
```

If PyGObject isn't installed, the target skips cleanly. The
`test-gir-clean` target also runs as part of `make test` and fails CI
on any new `g-ir-scanner` warning, so the bindings keep working as the
library evolves.

## See Also

- [AiClient](../api-reference/ai-client.md) - properties + methods
- [AiToolExecutor](../tool-executor.md) - multi-turn tool loop
- [Tool Use Example](tool-use.md) - manual loop, C version
