# Tool Executor

`AiToolExecutor` provides built-in tool implementations and manages the
multi-turn tool-use loop with any `AiProvider` that supports tools (HTTP
providers: Claude, OpenAI, Gemini, Grok, Ollama).

## Built-in Tools

| Tool | Description |
|------|-------------|
| `bash` | Run a shell command; stdout + stderr returned |
| `read` | Read a file; supports `offset` and `limit` parameters |
| `write` | Write a file (create or overwrite) |
| `edit` | Replace the first occurrence of `old_string` with `new_string` |
| `glob` | Find files matching a glob pattern (recursive) |
| `grep` | Search file contents with a regex; returns `file:line: match` |
| `ls` | List a directory with type and size |
| `web_fetch` | Fetch a URL via HTTP/HTTPS (max 100 KB) |
| `web_search` | Search the web (requires a search provider — see below) |

## Quick Start

```c
#include <ai-glib.h>

/* Create executor with all built-in tools */
g_autoptr(AiToolExecutor) exec = ai_tool_executor_new();

/* Create a provider (any HTTP provider) */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();

/* Build the message list */
g_autoptr(AiMessage) msg = ai_message_new_user(
    "List all .c files in /tmp and show the first 10 lines of each.");
GList *messages = g_list_append(NULL, msg);

/* Run the full tool-use loop */
g_autoptr(GError) err = NULL;
g_autofree gchar *reply = ai_tool_executor_run(
    exec,
    AI_PROVIDER(client),
    messages,
    NULL,   /* system prompt */
    4096,   /* max tokens */
    NULL,   /* cancellable */
    &err
);
g_list_free(messages);

if (err != NULL) {
    g_printerr("Error: %s\n", err->message);
} else {
    g_print("%s\n", reply);
}
```

## web_search

The `web_search` tool is disabled by default. Enable it by setting a search
provider:

```c
/* Using Bing Web Search API */
g_autoptr(AiBingSearch) bing = ai_bing_search_new("YOUR_BING_API_KEY");
ai_tool_executor_set_search_provider(exec, AI_SEARCH_PROVIDER(bing));

/* Using Brave Search API */
g_autoptr(AiBraveSearch) brave = ai_brave_search_new("YOUR_BRAVE_API_KEY");
ai_tool_executor_set_search_provider(exec, AI_SEARCH_PROVIDER(brave));
```

### Bing

Requires an Azure Cognitive Services key (`Ocp-Apim-Subscription-Key`).
Create one at https://portal.azure.com/ → Bing Search v7.

### Brave

Requires a Brave Search API key (`X-Subscription-Token`).
Create one at https://brave.com/search/api/.

### Adding a New Search Provider

Implement the `AiSearchProvider` interface:

```c
/* my-duckduckgo-search.h */
#include <ai-glib.h>

G_DECLARE_FINAL_TYPE(MyDuckDuckGoSearch, my_duckduckgo_search,
                     MY, DUCKDUCKGO_SEARCH, GObject)

MyDuckDuckGoSearch *my_duckduckgo_search_new(void);
```

```c
/* my-duckduckgo-search.c */
static void
my_duckduckgo_iface_init(AiSearchProviderInterface *iface)
{
    iface->search = my_duckduckgo_do_search;
}

G_DEFINE_TYPE_WITH_CODE(
    MyDuckDuckGoSearch, my_duckduckgo_search, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(AI_TYPE_SEARCH_PROVIDER, my_duckduckgo_iface_init)
)

static gchar *
my_duckduckgo_do_search(AiSearchProvider *provider,
                         const gchar *query,
                         GCancellable *cancellable,
                         GError **error)
{
    /* Fetch from DDG API, format as "Title\nURL\nSnippet\n---\n" per result */
    ...
}
```

Then register it:

```c
g_autoptr(MyDuckDuckGoSearch) ddg = my_duckduckgo_search_new();
ai_tool_executor_set_search_provider(exec, AI_SEARCH_PROVIDER(ddg));
```

## Low-level API

For cases where you manage the provider loop yourself:

```c
/* Get the tool list to pass to ai_provider_chat_async() */
GList *tools = ai_tool_executor_get_tools(exec);   /* transfer none */

ai_provider_chat_async(provider, messages, system, max_tokens, tools,
                       NULL, on_response, user_data);

/* In on_response: execute tool calls */
GList *uses = ai_response_get_tool_uses(response);
for (GList *i = uses; i; i = i->next) {
    g_autofree gchar *result = ai_tool_executor_execute(
        exec, i->data, NULL, NULL);
    /* build tool result message and continue conversation */
}
g_list_free(uses);
```

## Limits

- `ai_tool_executor_run()` caps at **20 turns** to prevent infinite loops.
- `web_fetch` returns at most **100 KB** of response body.
- Tools run on the calling thread; `bash` commands block until completion.
