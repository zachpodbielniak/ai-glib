# Tool Use Example

Hand-rolled multi-turn tool-use loop, runnable against every HTTP provider.

Source: [`examples/tool-use.c`](https://github.com/anthropics/ai-glib/blob/master/examples/tool-use.c)

## What it shows

- Defining tools with `ai_tool_new()` + `ai_tool_add_parameter()` /
  `ai_tool_add_enum_parameter()`
- Passing tools to `ai_provider_chat_async()`
- Detecting tool use with `ai_response_has_tool_use()` and extracting
  `AiToolUse` blocks from the response
- Replaying the assistant's tool-use blocks back into the next request so
  the provider can match them with results
- Sending results back with `ai_message_new_tool_result_with_name()` —
  preferred over `ai_message_new_tool_result()` because Gemini's wire format
  keys results by tool name (other providers ignore the extra field)
- Running the same loop against any provider (the wire-format conversion
  happens inside each provider's `build_request`)

## Prerequisites

Set the API key(s) you want to test:

```bash
export ANTHROPIC_API_KEY="..."
export OPENAI_API_KEY="..."
export XAI_API_KEY="..."
export GEMINI_API_KEY="..."
# Ollama: `ollama serve` running, with a tool-capable model
# (e.g. `ollama pull llama3.1:8b`)
```

## Running

```bash
make examples

./build/examples/tool-use claude  "What's the weather in Miami and 13 * 7?"
./build/examples/tool-use openai
./build/examples/tool-use grok
./build/examples/tool-use gemini
./build/examples/tool-use ollama
```

The first argument selects the provider (`claude` by default); the second
is the prompt.

## Example output

```
Provider: openai
User: What's the weather in San Francisco and New York? Also, calculate 15 * 7 + 23 for me.

[Turn 1: Model requested tool use]
  [Executing get_weather: location="San Francisco, CA", unit="(null)"]
  [Result: {"location": "San Francisco, CA", "temperature": 22.0, ...}]
  [Executing get_weather: location="New York, NY", unit="(null)"]
  [Result: {"location": "New York, NY", "temperature": 8.0, ...}]
  [Executing calculate: multiply(15, 7)]
  [Result: {"operation": "multiply", ..., "result": 105}]

[Turn 2: Model requested tool use]
  [Executing calculate: add(105, 23)]
  [Result: {"operation": "add", ..., "result": 128}]
```

## See also

- [Tool Use Executor](tool-use-executor.md) — easier path using `AiToolExecutor`
- [tool-executor.md](../tool-executor.md) — `AiToolExecutor` reference
- [api-reference/ai-tool.md](../api-reference/ai-tool.md) — `AiTool` API
