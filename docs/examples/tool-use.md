# Tool Use Example

Function calling with AI providers.

## Overview

This example demonstrates:
- Defining tools with parameters
- Sending tools with chat requests
- Detecting and extracting tool use from responses
- Executing tools and sending results back
- Multi-turn tool use conversations

## Prerequisites

Set your API key:

```bash
export ANTHROPIC_API_KEY="your-key"
```

## Code

```c
/*
 * tool-use.c - Function calling example
 *
 * Build: gcc -o tool-use tool-use.c $(pkg-config --cflags --libs ai-glib-1.0)
 */

#include <ai-glib.h>
#include <math.h>

/* Tool implementation: get weather */
static gchar *
execute_get_weather(JsonNode *input)
{
    JsonObject *obj = json_node_get_object(input);
    const gchar *location = json_object_get_string_member(obj, "location");
    const gchar *unit = "celsius";

    if (json_object_has_member(obj, "unit"))
    {
        unit = json_object_get_string_member(obj, "unit");
    }

    /* Simulated weather data */
    return g_strdup_printf(
        "{\"location\": \"%s\", \"temperature\": 22, \"unit\": \"%s\", "
        "\"conditions\": \"sunny\"}",
        location, unit);
}

/* Tool implementation: calculate */
static gchar *
execute_calculate(JsonNode *input)
{
    JsonObject *obj = json_node_get_object(input);
    const gchar *operation = json_object_get_string_member(obj, "operation");
    gdouble a = json_object_get_double_member(obj, "a");
    gdouble b = json_object_get_double_member(obj, "b");
    gdouble result = 0;

    if (g_str_equal(operation, "add"))
        result = a + b;
    else if (g_str_equal(operation, "subtract"))
        result = a - b;
    else if (g_str_equal(operation, "multiply"))
        result = a * b;
    else if (g_str_equal(operation, "divide"))
        result = b != 0 ? a / b : NAN;
    else
        return g_strdup("{\"error\": \"unknown operation\"}");

    return g_strdup_printf("{\"result\": %g}", result);
}

/* Execute a tool by name */
static gchar *
execute_tool(const gchar *name, JsonNode *input)
{
    if (g_str_equal(name, "get_weather"))
        return execute_get_weather(input);
    if (g_str_equal(name, "calculate"))
        return execute_calculate(input);

    return g_strdup_printf("{\"error\": \"unknown tool: %s\"}", name);
}

typedef struct {
    GMainLoop  *loop;
    AiProvider *provider;
    GList      *messages;
    GList      *tools;
} ChatContext;

static void on_response(GObject *source, GAsyncResult *result, gpointer user_data);

static void
continue_conversation(ChatContext *ctx)
{
    ai_provider_chat_async(
        ctx->provider,
        ctx->messages,
        "You are a helpful assistant with access to tools.",
        4096,
        ctx->tools,
        NULL,
        on_response,
        ctx
    );
}

static void
on_response(
    GObject      *source,
    GAsyncResult *result,
    gpointer      user_data
){
    ChatContext *ctx = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiResponse) response = NULL;

    response = ai_provider_chat_finish(AI_PROVIDER(source), result, &error);

    if (error != NULL)
    {
        g_printerr("Error: %s\n", error->message);
        g_main_loop_quit(ctx->loop);
        return;
    }

    /* Check for tool use */
    if (ai_response_has_tool_use(response))
    {
        g_print("\n[Model requested tool use]\n");

        /* Add assistant message with tool use to history */
        g_autoptr(AiMessage) assistant_msg =
            ai_message_new_with_blocks(
                AI_ROLE_ASSISTANT,
                ai_response_get_content_blocks(response));
        ctx->messages = g_list_append(ctx->messages, g_steal_pointer(&assistant_msg));

        /* Process each tool use */
        GList *tool_uses = ai_response_get_tool_uses(response);
        for (GList *l = tool_uses; l != NULL; l = l->next)
        {
            AiToolUse *tu = AI_TOOL_USE(l->data);
            const gchar *name = ai_tool_use_get_name(tu);
            const gchar *id = ai_tool_use_get_id(tu);
            JsonNode *input = ai_tool_use_get_input(tu);

            g_print("  Tool: %s\n", name);

            /* Execute the tool */
            g_autofree gchar *result_str = execute_tool(name, input);
            g_print("  Result: %s\n", result_str);

            /* Add tool result to messages */
            g_autoptr(AiMessage) result_msg =
                ai_message_new_tool_result(id, result_str, FALSE);
            ctx->messages = g_list_append(ctx->messages, g_steal_pointer(&result_msg));
        }
        g_list_free(tool_uses);

        /* Continue the conversation */
        g_print("\n[Continuing conversation...]\n\n");
        continue_conversation(ctx);
        return;
    }

    /* No tool use - print final response */
    g_autofree gchar *text = ai_response_get_text(response);
    g_print("Assistant: %s\n", text);

    /* Print usage */
    const AiUsage *usage = ai_response_get_usage(response);
    if (usage != NULL)
    {
        g_print("\n[Tokens: %d input, %d output]\n",
                ai_usage_get_input_tokens(usage),
                ai_usage_get_output_tokens(usage));
    }

    g_main_loop_quit(ctx->loop);
}

int
main(
    int   argc,
    char *argv[]
){
    g_autoptr(AiClaudeClient) client = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    ChatContext ctx = { 0 };

    /* Create client */
    client = ai_claude_client_new();
    ai_client_set_model(AI_CLIENT(client), AI_CLAUDE_MODEL_SONNET);

    /* Define tools */
    g_autoptr(AiTool) weather_tool = ai_tool_new(
        "get_weather",
        "Get the current weather for a location");
    ai_tool_add_parameter(weather_tool, "location", "string",
                          "City and country, e.g., 'Paris, France'", TRUE);
    ai_tool_add_parameter(weather_tool, "unit", "string",
                          "Temperature unit: celsius or fahrenheit", FALSE);

    g_autoptr(AiTool) calc_tool = ai_tool_new(
        "calculate",
        "Perform a mathematical calculation");
    const gchar *operations[] = { "add", "subtract", "multiply", "divide" };
    ai_tool_add_enum_parameter(calc_tool, "operation",
                               "The operation to perform",
                               operations, 4, TRUE);
    ai_tool_add_parameter(calc_tool, "a", "number", "First number", TRUE);
    ai_tool_add_parameter(calc_tool, "b", "number", "Second number", TRUE);

    /* Build tool list */
    GList *tools = NULL;
    tools = g_list_append(tools, weather_tool);
    tools = g_list_append(tools, calc_tool);

    /* Create initial message */
    g_autoptr(AiMessage) user_msg = ai_message_new_user(
        "What's the weather in Tokyo? Also, what's 47 multiplied by 23?");
    GList *messages = g_list_append(NULL, user_msg);

    g_print("User: What's the weather in Tokyo? Also, what's 47 multiplied by 23?\n\n");

    /* Set up context */
    loop = g_main_loop_new(NULL, FALSE);
    ctx.loop = loop;
    ctx.provider = AI_PROVIDER(client);
    ctx.messages = messages;
    ctx.tools = tools;

    /* Start conversation */
    continue_conversation(&ctx);

    g_main_loop_run(loop);

    /* Clean up */
    g_list_free(tools);
    g_list_free_full(ctx.messages, g_object_unref);

    return 0;
}
```

## Step-by-Step Explanation

### 1. Define Tools

```c
g_autoptr(AiTool) tool = ai_tool_new("tool_name", "Description of what it does");

/* Add string parameter */
ai_tool_add_parameter(tool, "param_name", "string", "Description", required);

/* Add number parameter */
ai_tool_add_parameter(tool, "value", "number", "A numeric value", TRUE);

/* Add enum parameter */
const gchar *options[] = { "option1", "option2", "option3" };
ai_tool_add_enum_parameter(tool, "choice", "Description", options, 3, TRUE);
```

### 2. Send Request with Tools

```c
GList *tools = NULL;
tools = g_list_append(tools, tool1);
tools = g_list_append(tools, tool2);

ai_provider_chat_async(
    provider,
    messages,
    system_prompt,
    max_tokens,
    tools,          /* <-- pass tools here */
    NULL,
    callback,
    user_data
);
```

### 3. Check for Tool Use

```c
if (ai_response_has_tool_use(response))
{
    /* Model wants to use tools */
    GList *tool_uses = ai_response_get_tool_uses(response);
    /* ... */
}
```

### 4. Execute Tools

```c
for (GList *l = tool_uses; l != NULL; l = l->next)
{
    AiToolUse *tu = AI_TOOL_USE(l->data);

    const gchar *name = ai_tool_use_get_name(tu);
    const gchar *id = ai_tool_use_get_id(tu);
    JsonNode *input = ai_tool_use_get_input(tu);

    /* Execute your tool implementation */
    gchar *result = my_tool_handler(name, input);

    /* ... */
}
```

### 5. Send Results Back

```c
/* Add assistant message with tool use to history */
g_autoptr(AiMessage) assistant_msg =
    ai_message_new_with_blocks(AI_ROLE_ASSISTANT,
                               ai_response_get_content_blocks(response));
messages = g_list_append(messages, g_steal_pointer(&assistant_msg));

/* Add tool result */
g_autoptr(AiMessage) result_msg =
    ai_message_new_tool_result(tool_use_id, result_json, FALSE);
messages = g_list_append(messages, g_steal_pointer(&result_msg));

/* Continue conversation */
ai_provider_chat_async(provider, messages, ...);
```

## JSON Parameter Handling

```c
static gchar *
my_tool_handler(const gchar *name, JsonNode *input)
{
    JsonObject *obj = json_node_get_object(input);

    /* Get string parameter */
    const gchar *str_val = json_object_get_string_member(obj, "param");

    /* Get number parameter */
    gdouble num_val = json_object_get_double_member(obj, "number");
    gint64 int_val = json_object_get_int_member(obj, "integer");

    /* Get boolean parameter */
    gboolean bool_val = json_object_get_boolean_member(obj, "flag");

    /* Check if parameter exists */
    if (json_object_has_member(obj, "optional_param"))
    {
        /* ... */
    }

    /* Return JSON result */
    return g_strdup_printf("{\"result\": \"value\"}");
}
```

## Error Handling in Tools

```c
g_autoptr(AiMessage) result_msg = ai_message_new_tool_result(
    tool_use_id,
    "{\"error\": \"Division by zero\"}",
    TRUE    /* is_error = TRUE */
);
```

## Building

```bash
gcc -o tool-use tool-use.c $(pkg-config --cflags --libs ai-glib-1.0) -lm
./tool-use
```

## Output

```
User: What's the weather in Tokyo? Also, what's 47 multiplied by 23?

[Model requested tool use]
  Tool: get_weather
  Result: {"location": "Tokyo, Japan", "temperature": 22, "unit": "celsius", "conditions": "sunny"}
  Tool: calculate
  Result: {"result": 1081}

[Continuing conversation...]