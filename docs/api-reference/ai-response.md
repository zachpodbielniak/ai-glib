# AiResponse

Represents an API response from a provider.

## Hierarchy

```
GObject
└── AiResponse
```

## Description

`AiResponse` contains the result of a chat completion request. It includes the response ID, model used, content blocks, stop reason, and token usage information.

## Functions

### ai_response_new

```c
AiResponse *
ai_response_new(const gchar *id, const gchar *model);
```

Creates a new response.

**Parameters:**
- `id`: the response ID
- `model`: the model that generated the response

**Returns:** `(transfer full)`: a new AiResponse

---

### ai_response_get_id

```c
const gchar *
ai_response_get_id(AiResponse *self);
```

Gets the response ID.

**Parameters:**
- `self`: an AiResponse

**Returns:** `(transfer none)`: the response ID

---

### ai_response_get_model

```c
const gchar *
ai_response_get_model(AiResponse *self);
```

Gets the model that generated the response.

**Parameters:**
- `self`: an AiResponse

**Returns:** `(transfer none)`: the model name

---

### ai_response_get_stop_reason

```c
AiStopReason
ai_response_get_stop_reason(AiResponse *self);
```

Gets the reason the response stopped generating.

**Parameters:**
- `self`: an AiResponse

**Returns:** the AiStopReason

---

### ai_response_set_stop_reason

```c
void
ai_response_set_stop_reason(
    AiResponse   *self,
    AiStopReason  reason
);
```

Sets the stop reason.

**Parameters:**
- `self`: an AiResponse
- `reason`: the stop reason

---

### ai_response_get_text

```c
gchar *
ai_response_get_text(AiResponse *self);
```

Gets the concatenated text from all text content blocks.

**Parameters:**
- `self`: an AiResponse

**Returns:** `(transfer full) (nullable)`: the text, free with g_free()

---

### ai_response_get_content_blocks

```c
GList *
ai_response_get_content_blocks(AiResponse *self);
```

Gets all content blocks in the response.

**Parameters:**
- `self`: an AiResponse

**Returns:** `(element-type AiContentBlock) (transfer none)`: the content blocks

---

### ai_response_add_content_block

```c
void
ai_response_add_content_block(
    AiResponse     *self,
    AiContentBlock *block
);
```

Adds a content block to the response.

**Parameters:**
- `self`: an AiResponse
- `block`: `(transfer full)`: the content block to add

---

### ai_response_has_tool_use

```c
gboolean
ai_response_has_tool_use(AiResponse *self);
```

Checks if the response contains tool use requests.

**Parameters:**
- `self`: an AiResponse

**Returns:** TRUE if tool use is present

---

### ai_response_get_tool_uses

```c
GList *
ai_response_get_tool_uses(AiResponse *self);
```

Gets all tool use blocks from the response.

**Parameters:**
- `self`: an AiResponse

**Returns:** `(element-type AiToolUse) (transfer container)`: list of tool uses

---

### ai_response_get_usage

```c
const AiUsage *
ai_response_get_usage(AiResponse *self);
```

Gets the token usage information.

**Parameters:**
- `self`: an AiResponse

**Returns:** `(transfer none) (nullable)`: the usage info, or NULL if not available

---

### ai_response_set_usage

```c
void
ai_response_set_usage(
    AiResponse    *self,
    const AiUsage *usage
);
```

Sets the token usage information.

**Parameters:**
- `self`: an AiResponse
- `usage`: the usage info

## Example

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

    /* Get basic info */
    g_print("Response ID: %s\n", ai_response_get_id(response));
    g_print("Model: %s\n", ai_response_get_model(response));

    /* Get the text content */
    g_autofree gchar *text = ai_response_get_text(response);
    g_print("Response: %s\n", text);

    /* Check stop reason */
    AiStopReason reason = ai_response_get_stop_reason(response);
    switch (reason)
    {
        case AI_STOP_REASON_END_TURN:
            g_print("Stopped: end of turn\n");
            break;
        case AI_STOP_REASON_MAX_TOKENS:
            g_print("Stopped: max tokens reached\n");
            break;
        case AI_STOP_REASON_TOOL_USE:
            g_print("Stopped: tool use requested\n");
            break;
        default:
            break;
    }

    /* Check for tool use */
    if (ai_response_has_tool_use(response))
    {
        GList *tool_uses = ai_response_get_tool_uses(response);
        for (GList *l = tool_uses; l != NULL; l = l->next)
        {
            AiToolUse *tu = AI_TOOL_USE(l->data);
            g_print("Tool: %s (id: %s)\n",
                    ai_tool_use_get_name(tu),
                    ai_tool_use_get_id(tu));
        }
        g_list_free(tool_uses);
    }

    /* Get usage stats */
    const AiUsage *usage = ai_response_get_usage(response);
    if (usage != NULL)
    {
        g_print("Tokens: %d input, %d output, %d total\n",
                ai_usage_get_input_tokens(usage),
                ai_usage_get_output_tokens(usage),
                ai_usage_get_total_tokens(usage));
    }

    g_main_loop_quit(loop);
}
```

## See Also

- [AiMessage](ai-message.md) - Message class
- [AiUsage](ai-usage.md) - Token usage
- [AiToolUse](ai-tool-use.md) - Tool use content
- [AiStopReason](index.md#aistopreason) - Stop reason enumeration
