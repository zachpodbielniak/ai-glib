# Tool Use Executor Example

Custom user-registered tools driven by `AiToolExecutor`'s built-in
multi-turn loop. Runnable against every HTTP provider.

Source: [`examples/tool-use-executor.c`](https://github.com/anthropics/ai-glib/blob/master/examples/tool-use-executor.c)

## What it shows

- Defining tools with `ai_tool_new()` + `ai_tool_add_parameter()`
- Registering them with `ai_tool_executor_register_callback()` — your
  callback runs whenever the model invokes the tool
- Letting `ai_tool_executor_run()` handle the whole multi-turn loop
  (request → tool dispatch → result message → next turn)
- The same code works across all HTTP providers; the per-provider wire
  format is handled inside ai-glib

Compared to the hand-rolled [Tool Use](tool-use.md) example, this version
trades the explicit loop for a single `ai_tool_executor_run()` call.

## Prerequisites

```bash
export ANTHROPIC_API_KEY="..."  # or OPENAI_API_KEY / GEMINI_API_KEY / XAI_API_KEY
# Ollama: `ollama serve` running with a tool-capable model.
```

## Running

```bash
make examples

./build/examples/tool-use-executor claude   "What's the weather in Miami in Fahrenheit, and what's 13 * 7?"
./build/examples/tool-use-executor openai
./build/examples/tool-use-executor gemini
./build/examples/tool-use-executor grok
./build/examples/tool-use-executor ollama
```

## Highlights from the source

```c
static gchar *
on_get_weather (AiToolUse    *tool_use,
                GCancellable *cancellable,
                GError      **error,
                gpointer      user_data)
{
    const gchar *location = ai_tool_use_get_input_string (tool_use, "location");
    /* ...look it up... */
    return g_strdup_printf ("{\"location\":\"%s\",\"temp\":22}", location);
}

g_autoptr(AiToolExecutor) exec    = ai_tool_executor_new ();
g_autoptr(AiTool)         weather = ai_tool_new ("get_weather",
                                                  "Get the current weather.");
ai_tool_add_parameter (weather, "location", "string", "City name", TRUE);

ai_tool_executor_register_callback (exec, weather, on_get_weather, NULL, NULL);

/* One call. Runs the full loop. */
g_autofree gchar *answer = ai_tool_executor_run (
    exec, AI_PROVIDER (provider), messages,
    /* system_prompt */ "You are a helpful assistant. Use tools when helpful.",
    /* max_tokens   */  4096,
    /* cancellable  */  NULL,
    &err);
```

The model sees the union of your registered tools and any built-ins
(`bash`, `read`, `write`, `edit`, `glob`, `grep`, `ls`, `web_fetch` and
optionally `web_search`). Built-ins remain available; register a tool with
the same name to override one of them for this executor.

## See also

- [tool-executor.md](../tool-executor.md) — `AiToolExecutor` reference
- [Tool Use](tool-use.md) — manual loop without `AiToolExecutor`
- [api-reference/ai-tool.md](../api-reference/ai-tool.md) — `AiTool` API
