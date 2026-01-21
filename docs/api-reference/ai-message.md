# AiMessage

Represents a conversation message with role and content.

## Hierarchy

```
GObject
└── AiMessage
```

## Description

`AiMessage` represents a single message in a conversation. Messages have a role (user, assistant, or system) and contain one or more content blocks (text, tool use, or tool results).

## Functions

### ai_message_new

```c
AiMessage *
ai_message_new(AiRole role, const gchar *content);
```

Creates a new message with the specified role and text content.

**Parameters:**
- `role`: the message role
- `content`: the text content

**Returns:** `(transfer full)`: a new AiMessage

---

### ai_message_new_user

```c
AiMessage *
ai_message_new_user(const gchar *content);
```

Creates a new user message.

**Parameters:**
- `content`: the text content

**Returns:** `(transfer full)`: a new AiMessage with AI_ROLE_USER

---

### ai_message_new_assistant

```c
AiMessage *
ai_message_new_assistant(const gchar *content);
```

Creates a new assistant message.

**Parameters:**
- `content`: the text content

**Returns:** `(transfer full)`: a new AiMessage with AI_ROLE_ASSISTANT

---

### ai_message_new_with_blocks

```c
AiMessage *
ai_message_new_with_blocks(AiRole role, GList *blocks);
```

Creates a new message with multiple content blocks.

**Parameters:**
- `role`: the message role
- `blocks`: `(element-type AiContentBlock) (transfer none)`: list of content blocks

**Returns:** `(transfer full)`: a new AiMessage

---

### ai_message_new_tool_result

```c
AiMessage *
ai_message_new_tool_result(
    const gchar *tool_use_id,
    const gchar *content,
    gboolean     is_error
);
```

Creates a new message containing a tool result.

**Parameters:**
- `tool_use_id`: the ID of the tool use this is responding to
- `content`: the result content
- `is_error`: whether the result is an error

**Returns:** `(transfer full)`: a new AiMessage with AI_ROLE_USER

---

### ai_message_get_role

```c
AiRole
ai_message_get_role(AiMessage *self);
```

Gets the message role.

**Parameters:**
- `self`: an AiMessage

**Returns:** the AiRole

---

### ai_message_get_text

```c
gchar *
ai_message_get_text(AiMessage *self);
```

Gets the concatenated text from all text content blocks.

**Parameters:**
- `self`: an AiMessage

**Returns:** `(transfer full) (nullable)`: the text content, free with g_free()

---

### ai_message_get_content_blocks

```c
GList *
ai_message_get_content_blocks(AiMessage *self);
```

Gets the list of content blocks.

**Parameters:**
- `self`: an AiMessage

**Returns:** `(element-type AiContentBlock) (transfer none)`: the content blocks

---

### ai_message_add_content_block

```c
void
ai_message_add_content_block(
    AiMessage      *self,
    AiContentBlock *block
);
```

Adds a content block to the message.

**Parameters:**
- `self`: an AiMessage
- `block`: `(transfer full)`: the content block to add

---

### ai_message_to_json

```c
JsonNode *
ai_message_to_json(AiMessage *self);
```

Serializes the message to JSON.

**Parameters:**
- `self`: an AiMessage

**Returns:** `(transfer full)`: a JSON node

---

### ai_message_new_from_json

```c
AiMessage *
ai_message_new_from_json(JsonNode *json, GError **error);
```

Creates a message from JSON.

**Parameters:**
- `json`: a JSON node
- `error`: `(out) (optional)`: return location for error

**Returns:** `(transfer full) (nullable)`: a new AiMessage, or NULL on error

## Example

```c
#include <ai-glib.h>

/* Simple user message */
g_autoptr(AiMessage) user_msg = ai_message_new_user("Hello, how are you?");

/* Get the text */
g_autofree gchar *text = ai_message_get_text(user_msg);
g_print("User said: %s\n", text);

/* Check the role */
AiRole role = ai_message_get_role(user_msg);
if (role == AI_ROLE_USER)
    g_print("This is a user message\n");

/* Multi-block message */
g_autoptr(AiTextContent) text1 = ai_text_content_new("First paragraph.");
g_autoptr(AiTextContent) text2 = ai_text_content_new("Second paragraph.");
GList *blocks = NULL;
blocks = g_list_append(blocks, g_steal_pointer(&text1));
blocks = g_list_append(blocks, g_steal_pointer(&text2));

g_autoptr(AiMessage) multi_msg = ai_message_new_with_blocks(AI_ROLE_USER, blocks);
g_list_free(blocks);

/* Tool result message */
g_autoptr(AiMessage) tool_result = ai_message_new_tool_result(
    "tool_use_123",
    "{\"result\": 42}",
    FALSE  /* not an error */
);
```

## See Also

- [AiContentBlock](ai-content-block.md) - Content block base class
- [AiResponse](ai-response.md) - Response class
- [AiRole](index.md#airole) - Role enumeration
