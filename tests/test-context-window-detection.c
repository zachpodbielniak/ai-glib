/*
 * test-context-window-detection.c - Unit tests for context window full detection
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tests the stderr pattern matching for context-window-full error detection.
 * When claude-code exits with a non-zero status, we check stderr for specific
 * keywords to distinguish context-full errors from generic CLI failures.
 */

#include <glib.h>
#include <string.h>

/*
 * Helper function to test if stderr matches context-window-full patterns.
 * This mirrors the logic in ai-claude-code-client.c on_chat_finish().
 */
static gboolean
stderr_indicates_context_full(const gchar *stderr_data)
{
    if (stderr_data == NULL)
        return FALSE;

    if (strstr(stderr_data, "context") != NULL ||
        strstr(stderr_data, "window") != NULL ||
        strstr(stderr_data, "tokens") != NULL ||
        strstr(stderr_data, "max_tokens") != NULL ||
        strstr(stderr_data, "maximum tokens") != NULL)
    {
        return TRUE;
    }

    return FALSE;
}

/*
 * Test that "context" in stderr is detected.
 */
static void
test_context_window_detection_context(void)
{
    gboolean result = stderr_indicates_context_full(
        "Error: context window is full");
    g_assert_true(result);
}

/*
 * Test that "window" in stderr is detected.
 */
static void
test_context_window_detection_window(void)
{
    gboolean result = stderr_indicates_context_full(
        "Error: exceeded context window size");
    g_assert_true(result);
}

/*
 * Test that "tokens" in stderr is detected.
 */
static void
test_context_window_detection_tokens(void)
{
    gboolean result = stderr_indicates_context_full(
        "Error: request exceeds maximum tokens");
    g_assert_true(result);
}

/*
 * Test that "max_tokens" in stderr is detected.
 */
static void
test_context_window_detection_max_tokens(void)
{
    gboolean result = stderr_indicates_context_full(
        "Error: max_tokens limit reached");
    g_assert_true(result);
}

/*
 * Test that "maximum tokens" in stderr is detected.
 */
static void
test_context_window_detection_maximum_tokens(void)
{
    gboolean result = stderr_indicates_context_full(
        "Error: maximum tokens exceeded");
    g_assert_true(result);
}

/*
 * Test that multiple keywords in one message are handled.
 */
static void
test_context_window_detection_multiple_keywords(void)
{
    gboolean result = stderr_indicates_context_full(
        "Error: context window exceeded: tokens=8192 max_tokens=8000");
    g_assert_true(result);
}

/*
 * Test that case-sensitive matching works (lowercase only).
 */
static void
test_context_window_detection_case_sensitive(void)
{
    /* Uppercase should NOT match */
    gboolean result = stderr_indicates_context_full(
        "Error: CONTEXT WINDOW FULL");
    g_assert_false(result);
}

/*
 * Test that empty stderr does not trigger detection.
 */
static void
test_context_window_detection_empty(void)
{
    gboolean result = stderr_indicates_context_full("");
    g_assert_false(result);
}

/*
 * Test that NULL stderr does not trigger detection.
 */
static void
test_context_window_detection_null(void)
{
    gboolean result = stderr_indicates_context_full(NULL);
    g_assert_false(result);
}

/*
 * Test that generic errors are not misdetected.
 */
static void
test_context_window_detection_generic_error(void)
{
    gboolean result = stderr_indicates_context_full(
        "Error: network timeout");
    g_assert_false(result);
}

/*
 * Test that partial keyword matches work (e.g., "context" in other words).
 */
static void
test_context_window_detection_partial_match(void)
{
    /* "context" appears in "context" */
    gboolean result = stderr_indicates_context_full(
        "Error: contextual error occurred");
    g_assert_true(result);  /* This will match because "context" is present */
}

/*
 * Test with real-world-ish error message from claude-code.
 */
static void
test_context_window_detection_realistic(void)
{
    const gchar *realistic_error =
        "Error: The context window (20000 tokens) has been exceeded. "
        "The request uses 21500 tokens, which is 1500 tokens over the limit.";

    gboolean result = stderr_indicates_context_full(realistic_error);
    g_assert_true(result);
}

/*
 * Test error message with newlines and formatting.
 */
static void
test_context_window_detection_multiline(void)
{
    const gchar *multiline_error =
        "Error:\n"
        "  Message: Request exceeded context window\n"
        "  Window:  8192 tokens\n"
        "  Used:    9000 tokens\n";

    gboolean result = stderr_indicates_context_full(multiline_error);
    g_assert_true(result);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* Basic keyword detection */
    g_test_add_func("/ai-glib/context-window-detection/context",
                    test_context_window_detection_context);
    g_test_add_func("/ai-glib/context-window-detection/window",
                    test_context_window_detection_window);
    g_test_add_func("/ai-glib/context-window-detection/tokens",
                    test_context_window_detection_tokens);
    g_test_add_func("/ai-glib/context-window-detection/max-tokens",
                    test_context_window_detection_max_tokens);
    g_test_add_func("/ai-glib/context-window-detection/maximum-tokens",
                    test_context_window_detection_maximum_tokens);

    /* Edge cases */
    g_test_add_func("/ai-glib/context-window-detection/multiple-keywords",
                    test_context_window_detection_multiple_keywords);
    g_test_add_func("/ai-glib/context-window-detection/case-sensitive",
                    test_context_window_detection_case_sensitive);
    g_test_add_func("/ai-glib/context-window-detection/empty",
                    test_context_window_detection_empty);
    g_test_add_func("/ai-glib/context-window-detection/null",
                    test_context_window_detection_null);
    g_test_add_func("/ai-glib/context-window-detection/generic-error",
                    test_context_window_detection_generic_error);
    g_test_add_func("/ai-glib/context-window-detection/partial-match",
                    test_context_window_detection_partial_match);

    /* Real-world scenarios */
    g_test_add_func("/ai-glib/context-window-detection/realistic",
                    test_context_window_detection_realistic);
    g_test_add_func("/ai-glib/context-window-detection/multiline",
                    test_context_window_detection_multiline);

    return g_test_run();
}
