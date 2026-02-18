/*
 * ai-simple.h - Simple convenience API for ai-glib
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * AiSimple provides a minimal-ceremony wrapper around ai-glib's provider
 * system. It reads defaults from config files and environment variables,
 * instantiates the appropriate provider, and lets you prompt an LLM with
 * just a few lines of C:
 *
 *   g_autoptr(AiSimple) ai = ai_simple_new();
 *   g_autofree gchar *answer = ai_simple_prompt(ai, "What is 2+2?", NULL, NULL);
 *   g_print("%s\n", answer);
 */

#pragma once

#if !defined(AI_GLIB_INSIDE) && !defined(AI_GLIB_COMPILATION)
#error "Only <ai-glib.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "core/ai-enums.h"
#include "core/ai-config.h"

G_BEGIN_DECLS

#define AI_TYPE_SIMPLE (ai_simple_get_type())

G_DECLARE_FINAL_TYPE(AiSimple, ai_simple, AI, SIMPLE, GObject)

/**
 * ai_simple_new:
 *
 * Creates a new #AiSimple using the default configuration.
 * The provider and model are determined from config files
 * (~/.config/ai-glib/config.yaml) and environment variables.
 * If no default provider is configured, falls back to %AI_PROVIDER_OLLAMA.
 *
 * Returns: (transfer full): a new #AiSimple
 */
AiSimple *
ai_simple_new(void);

/**
 * ai_simple_new_with_provider:
 * @provider: the provider type to use
 * @model: (nullable): the model name, or %NULL for the provider default
 *
 * Creates a new #AiSimple with an explicit provider and optional model.
 * API keys and base URLs are still read from the default configuration.
 *
 * Returns: (transfer full): a new #AiSimple
 */
AiSimple *
ai_simple_new_with_provider(
    AiProviderType  provider,
    const gchar    *model
);

/**
 * ai_simple_new_with_config:
 * @config: an #AiConfig
 *
 * Creates a new #AiSimple with the specified configuration.
 * The provider and model are read from @config.
 *
 * Returns: (transfer full): a new #AiSimple
 */
AiSimple *
ai_simple_new_with_config(AiConfig *config);

/**
 * ai_simple_prompt:
 * @self: an #AiSimple
 * @prompt: the user prompt text
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Sends a single-shot prompt to the LLM and returns the response text.
 * This is stateless — no conversation history is maintained.
 * If a system prompt has been set, it is included in the request.
 *
 * Returns: (transfer full) (nullable): the response text, or %NULL on error.
 *   Free with g_free().
 */
gchar *
ai_simple_prompt(
    AiSimple      *self,
    const gchar   *prompt,
    GCancellable  *cancellable,
    GError       **error
);

/**
 * ai_simple_chat:
 * @self: an #AiSimple
 * @prompt: the user prompt text
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Sends a prompt and maintains conversation history.
 * Each call appends the user message and assistant response to the
 * internal history, enabling multi-turn conversations.
 * Use ai_simple_clear_history() to reset.
 *
 * Returns: (transfer full) (nullable): the response text, or %NULL on error.
 *   Free with g_free().
 */
gchar *
ai_simple_chat(
    AiSimple      *self,
    const gchar   *prompt,
    GCancellable  *cancellable,
    GError       **error
);

/**
 * ai_simple_set_system_prompt:
 * @self: an #AiSimple
 * @system_prompt: (nullable): the system prompt, or %NULL to clear
 *
 * Sets the system prompt used for all subsequent prompt and chat calls.
 */
void
ai_simple_set_system_prompt(
    AiSimple    *self,
    const gchar *system_prompt
);

/**
 * ai_simple_get_system_prompt:
 * @self: an #AiSimple
 *
 * Gets the current system prompt.
 *
 * Returns: (transfer none) (nullable): the system prompt
 */
const gchar *
ai_simple_get_system_prompt(AiSimple *self);

/**
 * ai_simple_clear_history:
 * @self: an #AiSimple
 *
 * Clears the conversation history used by ai_simple_chat().
 */
void
ai_simple_clear_history(AiSimple *self);

/**
 * ai_simple_get_provider:
 * @self: an #AiSimple
 *
 * Gets the underlying provider instance as an #AiProvider interface.
 * This is an escape hatch for advanced usage — you can use the full
 * ai-glib API on the returned provider (async calls, streaming, etc.).
 *
 * Returns: (transfer none): the #AiProvider
 */
AiProvider *
ai_simple_get_provider(AiSimple *self);

G_END_DECLS
