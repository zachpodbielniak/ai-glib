# AiTool

Defines a tool/function that can be called by the AI model.

## Hierarchy

```
GObject
└── AiTool
```

## Description

`AiTool` defines a tool (also known as a function) that the AI model can choose to call. Tools have a name, description, and a JSON schema defining their parameters.

## Functions

### ai_tool_new

```c
AiTool *
ai_tool_new(const gchar *name, const gchar *description);
```

Creates a new tool definition.

**Parameters:**
- `name`: the tool name (should be a valid identifier)
- `description`: human-readable description of what the tool does

**Returns:** `(transfer full)`: a new AiTool

---

### ai_tool_get_name

```c
const gchar *
ai_tool_get_name(AiTool *self);
```

Gets the tool name.

**Parameters:**
- `self`: an AiTool

**Returns:** `(transfer none)`: the tool name

---

### ai_tool_get_description

```c
const gchar *
ai_tool_get_description(AiTool *self);
```

Gets the tool description.

**Parameters:**
- `self`: an AiTool

**Returns:** `(transfer none)`: the description

---

### ai_tool_add_parameter

```c
void
ai_tool_add_parameter(
    AiTool      *self,
    const gchar *name,
    const gchar *type,
    const gchar *description,
    gboolean     required
);
```

Adds a parameter to the tool's input schema.

**Parameters:**
- `self`: an AiTool
- `name`: parameter name
- `type`: JSON schema type (string, number, integer, boolean, array, object)
- `description`: parameter description
- `required`: whether the parameter is required

---

### ai_tool_add_enum_parameter

```c
void
ai_tool_add_enum_parameter(
    AiTool       *self,
    const gchar  *name,
    const gchar  *description,
    const gchar **values,
    gsize         n_values,
    gboolean      required
);
```

Adds an enum parameter with allowed values.

**Parameters:**
- `self`: an AiTool
- `name`: parameter name
- `description`: parameter description
- `values`: array of allowed string values
- `n_values`: number of values
- `required`: whether the parameter is required

---

### ai_tool_get_parameters

```c
JsonNode *
ai_tool_get_parameters(AiTool *self);
```

Gets the parameter schema as JSON.

**Parameters:**
- `self`: an AiTool

**Returns:** `(transfer none)`: the JSON schema node

---

### ai_tool_to_json

```c
JsonNode *
ai_tool_to_json(AiTool *self, AiProviderType provider);
```

Serializes the tool to provider-specific JSON format.

**Parameters:**
- `self`: an AiTool
- `provider`: the target provider type

**Returns:** `(transfer full)`: the JSON node

## Example

```c
#include <ai-glib.h>

/* Create a simple tool */
g_autoptr(AiTool) get_weather = ai_tool_new(
    "get_weather",
    "Get the current weather for a location"
);

/* Add parameters */
ai_tool_add_parameter(get_weather, "location", "string",
                      "The city and state, e.g. San Francisco, CA",
                      TRUE);  /* required */

ai_tool_add_parameter(get_weather, "unit", "string",
                      "Temperature unit (celsius or fahrenheit)",
                      FALSE);  /* optional */

/* Or use enum for constrained values */
const gchar *units[] = { "celsius", "fahrenheit" };
ai_tool_add_enum_parameter(get_weather, "unit",
                           "Temperature unit",
                           units, 2, FALSE);

/* Create a list of tools */
GList *tools = NULL;
tools = g_list_append(tools, g_steal_pointer(&get_weather));

/* Use tools in a chat request */
g_autoptr(AiClaudeClient) client = ai_claude_client_new();
g_autoptr(AiMessage) msg = ai_message_new_user("What's the weather in Paris?");
GList *messages = g_list_append(NULL, msg);

ai_provider_chat_async(
    AI_PROVIDER(client),
    messages,
    NULL,      /* system prompt */
    4096,      /* max tokens */
    tools,     /* tools list */
    NULL,      /* cancellable */
    on_response,
    loop
);

/* Clean up */
g_list_free_full(tools, g_object_unref);
g_list_free(messages);
```

## Tool Use Flow

1. Define tools with `ai_tool_new()` and `ai_tool_add_parameter()`
2. Pass tools to `ai_provider_chat_async()`
3. Check response for tool use with `ai_response_has_tool_use()`
4. Extract tool uses with `ai_response_get_tool_uses()`
5. Execute the tools and gather results
6. Send results back with `ai_message_new_tool_result()`
7. Continue the conversation

```c
/* Handling tool use */
if (ai_response_has_tool_use(response))
{
    GList *tool_uses = ai_response_get_tool_uses(response);

    for (GList *l = tool_uses; l != NULL; l = l->next)
    {
        AiToolUse *tu = AI_TOOL_USE(l->data);

        const gchar *name = ai_tool_use_get_name(tu);
        const gchar *id = ai_tool_use_get_id(tu);
        JsonNode *input = ai_tool_use_get_input(tu);

        /* Execute the tool... */
        gchar *result = execute_tool(name, input);

        /* Create result message */
        g_autoptr(AiMessage) result_msg = ai_message_new_tool_result(
            id, result, FALSE);

        /* Add to conversation and continue */
        messages = g_list_append(messages, g_steal_pointer(&result_msg));
    }

    g_list_free(tool_uses);
}
```

## See Also

- [AiToolUse](ai-tool-use.md) - Tool use content block
- [AiToolResult](ai-tool-result.md) - Tool result content block
- [Tool Use Example](../examples/tool-use.md) - Complete tool use example
