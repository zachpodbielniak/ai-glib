# AiError

Error domain and codes for ai-glib.

## Description

`AI_ERROR` is the error domain for all errors in ai-glib. Errors are returned via `GError` following standard GLib conventions.

## Error Domain

```c
#define AI_ERROR (ai_error_quark())

GQuark ai_error_quark(void);
```

## Error Codes

```c
typedef enum {
    AI_ERROR_INVALID_API_KEY,
    AI_ERROR_RATE_LIMITED,
    AI_ERROR_NETWORK_ERROR,
    AI_ERROR_INVALID_REQUEST,
    AI_ERROR_INVALID_RESPONSE,
    AI_ERROR_MODEL_NOT_FOUND,
    AI_ERROR_CONTEXT_LENGTH_EXCEEDED,
    AI_ERROR_CONTENT_FILTERED,
    AI_ERROR_SERVER_ERROR,
    AI_ERROR_TIMEOUT,
    AI_ERROR_CANCELLED,
    AI_ERROR_CONFIGURATION_ERROR
} AiErrorCode;
```

### AI_ERROR_INVALID_API_KEY

Authentication failed. The API key is missing, invalid, or expired.

**HTTP Status:** 401, 403

**Resolution:** Check your API key and ensure it's correctly set in environment variables or config.

---

### AI_ERROR_RATE_LIMITED

Request was rate limited by the provider.

**HTTP Status:** 429

**Resolution:** Wait and retry with exponential backoff. Consider reducing request frequency.

---

### AI_ERROR_NETWORK_ERROR

Network connection failed.

**HTTP Status:** 0 (no response)

**Resolution:** Check network connectivity. Verify the endpoint URL is correct.

---

### AI_ERROR_INVALID_REQUEST

The request was malformed or contained invalid parameters.

**HTTP Status:** 400

**Resolution:** Check the request parameters, message format, and tool definitions.

---

### AI_ERROR_INVALID_RESPONSE

The response from the provider could not be parsed.

**Resolution:** This usually indicates a bug or API change. Check provider status.

---

### AI_ERROR_MODEL_NOT_FOUND

The requested model does not exist or is not accessible.

**HTTP Status:** 404

**Resolution:** Check the model name. Use the provider's model listing to see available models.

---

### AI_ERROR_CONTEXT_LENGTH_EXCEEDED

The request exceeded the model's context window.

**HTTP Status:** 400

**Resolution:** Reduce message history or use a model with a larger context window.

---

### AI_ERROR_CONTENT_FILTERED

Content was filtered due to safety policies.

**HTTP Status:** 400

**Resolution:** Modify the request to comply with the provider's content policies.

---

### AI_ERROR_SERVER_ERROR

The provider's server encountered an error.

**HTTP Status:** 500, 502, 503

**Resolution:** Retry the request. Check provider status page.

---

### AI_ERROR_TIMEOUT

The request timed out.

**Resolution:** Increase timeout via `ai_config_set_timeout()` or reduce request complexity.

---

### AI_ERROR_CANCELLED

The operation was cancelled via GCancellable.

**Resolution:** None needed - this is expected when cancellation is requested.

---

### AI_ERROR_CONFIGURATION_ERROR

Configuration is invalid or incomplete.

**Resolution:** Ensure required configuration (API key, etc.) is provided.

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
        /* Check the error domain */
        if (error->domain == AI_ERROR)
        {
            switch (error->code)
            {
                case AI_ERROR_INVALID_API_KEY:
                    g_printerr("Authentication failed. Check your API key.\n");
                    break;

                case AI_ERROR_RATE_LIMITED:
                    g_printerr("Rate limited. Waiting before retry...\n");
                    /* Implement retry logic */
                    break;

                case AI_ERROR_NETWORK_ERROR:
                    g_printerr("Network error. Check connectivity.\n");
                    break;

                case AI_ERROR_MODEL_NOT_FOUND:
                    g_printerr("Model not found. Check model name.\n");
                    break;

                case AI_ERROR_CONTEXT_LENGTH_EXCEEDED:
                    g_printerr("Message too long. Reduce history.\n");
                    break;

                case AI_ERROR_TIMEOUT:
                    g_printerr("Request timed out.\n");
                    break;

                case AI_ERROR_CANCELLED:
                    g_print("Request was cancelled.\n");
                    break;

                default:
                    g_printerr("Error: %s\n", error->message);
                    break;
            }
        }
        else
        {
            /* Handle non-AI errors (e.g., GIO errors) */
            g_printerr("Unexpected error: %s\n", error->message);
        }

        g_main_loop_quit(loop);
        return;
    }

    /* Success - process response */
    g_autofree gchar *text = ai_response_get_text(response);
    g_print("%s\n", text);

    g_main_loop_quit(loop);
}
```

## Error Handling Best Practices

1. **Always check for errors** in async callbacks and sync function returns

2. **Use g_autoptr(GError)** for automatic cleanup:
   ```c
   g_autoptr(GError) error = NULL;
   ```

3. **Check the domain** before interpreting the code:
   ```c
   if (error->domain == AI_ERROR) {
       /* Handle AI-specific errors */
   }
   ```

4. **Log error details** for debugging:
   ```c
   g_warning("AI error (code %d): %s", error->code, error->message);
   ```

5. **Implement retry logic** for transient errors:
   - `AI_ERROR_RATE_LIMITED` - use exponential backoff
   - `AI_ERROR_SERVER_ERROR` - retry with delay
   - `AI_ERROR_TIMEOUT` - retry or increase timeout
   - `AI_ERROR_NETWORK_ERROR` - retry after checking connectivity

## See Also

- [GError Documentation](https://docs.gtk.org/glib/struct.Error.html) - GLib error handling
- [AiConfig](ai-config.md) - Configuration (for timeout settings)
