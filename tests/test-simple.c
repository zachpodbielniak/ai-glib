/*
 * test-simple.c - Unit tests for AiSimple convenience API
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <glib.h>

#define AI_GLIB_INSIDE
#include "ai-types.h"
#include "core/ai-enums.h"
#include "core/ai-config.h"
#include "core/ai-provider.h"
#include "core/ai-client.h"
#include "core/ai-cli-client.h"
#include "convenience/ai-simple.h"
#undef AI_GLIB_INSIDE

/*
 * test_simple_new:
 *
 * Tests that ai_simple_new() creates a valid AiSimple object
 * using default configuration (falls back to Ollama).
 */
static void
test_simple_new(void)
{
    g_autoptr(AiSimple) simple = NULL;

    simple = ai_simple_new();
    g_assert_nonnull(simple);
    g_assert_true(AI_IS_SIMPLE(simple));
}

/*
 * test_simple_new_with_provider:
 *
 * Tests that ai_simple_new_with_provider() creates a valid AiSimple
 * with the specified provider type and model.
 */
static void
test_simple_new_with_provider(void)
{
    g_autoptr(AiSimple) simple = NULL;
    AiProvider *provider;

    simple = ai_simple_new_with_provider(AI_PROVIDER_OLLAMA, "tinyllama:1.1b");
    g_assert_nonnull(simple);
    g_assert_true(AI_IS_SIMPLE(simple));

    /* Verify the underlying provider is valid */
    provider = ai_simple_get_provider(simple);
    g_assert_nonnull(provider);
    g_assert_true(AI_IS_PROVIDER(provider));
}

/*
 * test_simple_new_with_config:
 *
 * Tests that ai_simple_new_with_config() creates a valid AiSimple
 * using a custom AiConfig object.
 */
static void
test_simple_new_with_config(void)
{
    g_autoptr(AiConfig) config = NULL;
    g_autoptr(AiSimple) simple = NULL;
    AiProvider *provider;

    config = ai_config_new();
    ai_config_set_default_provider(config, AI_PROVIDER_OLLAMA);
    ai_config_set_default_model(config, "tinyllama:1.1b");

    simple = ai_simple_new_with_config(config);
    g_assert_nonnull(simple);
    g_assert_true(AI_IS_SIMPLE(simple));

    provider = ai_simple_get_provider(simple);
    g_assert_nonnull(provider);
    g_assert_true(AI_IS_PROVIDER(provider));
}

/*
 * test_simple_system_prompt:
 *
 * Tests setting and getting the system prompt.
 */
static void
test_simple_system_prompt(void)
{
    g_autoptr(AiSimple) simple = NULL;
    const gchar *prompt;

    simple = ai_simple_new();

    /* Initially NULL */
    prompt = ai_simple_get_system_prompt(simple);
    g_assert_null(prompt);

    /* Set and verify */
    ai_simple_set_system_prompt(simple, "You are a helpful assistant.");
    prompt = ai_simple_get_system_prompt(simple);
    g_assert_cmpstr(prompt, ==, "You are a helpful assistant.");

    /* Clear by setting NULL */
    ai_simple_set_system_prompt(simple, NULL);
    prompt = ai_simple_get_system_prompt(simple);
    g_assert_null(prompt);
}

/*
 * test_simple_clear_history:
 *
 * Tests that clear_history() doesn't crash and can be called
 * on a fresh instance with no history.
 */
static void
test_simple_clear_history(void)
{
    g_autoptr(AiSimple) simple = NULL;

    simple = ai_simple_new();

    /* Should be safe to call with no history */
    ai_simple_clear_history(simple);

    /* Should also be safe to call multiple times */
    ai_simple_clear_history(simple);
    ai_simple_clear_history(simple);
}

/*
 * test_simple_provider_types:
 *
 * Tests that each HTTP API provider type creates successfully.
 * CLI providers may fail if the executable isn't available, so we
 * only test the HTTP providers here.
 */
static void
test_simple_provider_types(void)
{
    AiProviderType types[] = {
        AI_PROVIDER_CLAUDE,
        AI_PROVIDER_OPENAI,
        AI_PROVIDER_GEMINI,
        AI_PROVIDER_GROK,
        AI_PROVIDER_OLLAMA
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(types); i++)
    {
        g_autoptr(AiSimple) simple = NULL;
        AiProvider *provider;

        simple = ai_simple_new_with_provider(types[i], NULL);
        g_assert_nonnull(simple);

        provider = ai_simple_get_provider(simple);
        g_assert_nonnull(provider);
        g_assert_true(AI_IS_PROVIDER(provider));
    }
}

/*
 * test_simple_get_provider_is_client:
 *
 * Verifies that HTTP API providers return an AiClient subclass
 * when accessed through get_provider().
 */
static void
test_simple_get_provider_is_client(void)
{
    g_autoptr(AiSimple) simple = NULL;
    AiProvider *provider;

    simple = ai_simple_new_with_provider(AI_PROVIDER_OLLAMA, NULL);
    provider = ai_simple_get_provider(simple);

    /* Ollama should be an AiClient subclass */
    g_assert_true(AI_IS_CLIENT(provider));
    g_assert_true(AI_IS_PROVIDER(provider));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/simple/new", test_simple_new);
    g_test_add_func("/simple/new-with-provider", test_simple_new_with_provider);
    g_test_add_func("/simple/new-with-config", test_simple_new_with_config);
    g_test_add_func("/simple/system-prompt", test_simple_system_prompt);
    g_test_add_func("/simple/clear-history", test_simple_clear_history);
    g_test_add_func("/simple/provider-types", test_simple_provider_types);
    g_test_add_func("/simple/get-provider-is-client", test_simple_get_provider_is_client);

    return g_test_run();
}
