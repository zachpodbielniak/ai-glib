/*
 * ai-provider-factory.c - Construct a provider by type
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 */

#include "config.h"

#include "convenience/ai-provider-factory.h"
#include "core/ai-error.h"
#include "providers/ai-claude-client.h"
#include "providers/ai-openai-client.h"
#include "providers/ai-openai-compatible-client.h"
#include "providers/ai-gemini-client.h"
#include "providers/ai-grok-client.h"
#include "providers/ai-ollama-client.h"
#include "providers/ai-claude-code-client.h"
#include "providers/ai-claude-tmux-client.h"
#include "providers/ai-opencode-client.h"
#include "providers/ai-grok-build-client.h"
#include "providers/ai-antigravity-client.h"
#include "providers/ai-cursor-client.h"
#include "providers/ai-codex-cli-client.h"

/**
 * ai_provider_factory_new:
 * @type: which provider to build
 * @config: (nullable): configuration, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * Builds a provider from an #AiProviderType.
 *
 * Returns a #GObject rather than an #AiProvider because the two client base
 * classes -- #AiClient for the HTTP providers, #AiCliClient for the CLI
 * wrappers -- share no ancestor beyond #GObject. A caller applies the knobs
 * it cares about by testing which one it got, exactly as `ai` and `ai-tui`
 * do:
 *
 * |[<!-- language="C" -->
 * if (AI_IS_CLIENT (provider))
 *     ai_client_set_model (AI_CLIENT (provider), model);
 * else if (AI_IS_CLI_CLIENT (provider))
 *     ai_cli_client_set_model (AI_CLI_CLIENT (provider), model);
 * ]|
 *
 * This lived as a static function inside bin/ai.c until a second front-end
 * needed it. It belongs in the library for the same reason: anything
 * driving ai-glib through bindings wants to name a provider and get one.
 *
 * Returns: (transfer full) (nullable): the provider, or %NULL on error
 */
GObject *
ai_provider_factory_new(
    AiProviderType   type,
    AiConfig        *config,
    GError         **error
){
    g_autoptr(AiConfig) owned_config = NULL;

    if (config == NULL)
    {
        owned_config = ai_config_new();
        config = owned_config;
    }

    switch (type)
    {
        case AI_PROVIDER_CLAUDE:
            return G_OBJECT(ai_claude_client_new_with_config(config));
        case AI_PROVIDER_OPENAI_COMPATIBLE:
            return G_OBJECT(ai_openai_compatible_client_new_with_config(config));
        case AI_PROVIDER_OPENAI:
            return G_OBJECT(ai_openai_client_new_with_config(config));
        case AI_PROVIDER_GEMINI:
            return G_OBJECT(ai_gemini_client_new_with_config(config));
        case AI_PROVIDER_GROK:
            return G_OBJECT(ai_grok_client_new_with_config(config));
        case AI_PROVIDER_OLLAMA:
            return G_OBJECT(ai_ollama_client_new_with_config(config));
        case AI_PROVIDER_CLAUDE_CODE:
            return G_OBJECT(ai_claude_code_client_new_with_config(config));
        case AI_PROVIDER_CLAUDE_TMUX:
            return G_OBJECT(ai_claude_tmux_client_new_with_config(config));
        case AI_PROVIDER_OPENCODE:
            return G_OBJECT(ai_opencode_client_new_with_config(config));
        case AI_PROVIDER_GROK_BUILD:
            return G_OBJECT(ai_grok_build_client_new_with_config(config));
        case AI_PROVIDER_ANTIGRAVITY:
            return G_OBJECT(ai_antigravity_client_new_with_config(config));
        case AI_PROVIDER_CODEX_CLI:
            return G_OBJECT(ai_codex_cli_client_new_with_config(config));
        case AI_PROVIDER_CURSOR:
            return G_OBJECT(ai_cursor_client_new_with_config(config));
        default:
            break;
    }

    g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                "unknown provider type %d", (gint)type);

    return NULL;
}

/**
 * ai_provider_factory_new_from_string:
 * @name: (nullable): a provider name, as ai_provider_type_from_string() spells it
 * @config: (nullable): configuration, or %NULL for the default
 * @error: (out) (optional): return location for a #GError
 *
 * Builds a provider from its name.
 *
 * Returns: (transfer full) (nullable): the provider, or %NULL on error
 */
GObject *
ai_provider_factory_new_from_string(
    const gchar  *name,
    AiConfig     *config,
    GError      **error
){
    AiProviderType type;

    if (name == NULL || name[0] == '\0')
    {
        g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                            "no provider named");
        return NULL;
    }

    type = ai_provider_type_from_string(name);

    /*
     * ai_provider_type_from_string() answers CLAUDE for anything it does
     * not recognise, so an unknown name would silently become Claude. A
     * caller who asked for "gpt5" deserves to be told, not quietly
     * redirected.
     */
    if (type == AI_PROVIDER_CLAUDE &&
        g_strcmp0(name, "claude") != 0 &&
        g_strcmp0(name, "anthropic") != 0)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                    "unknown provider '%s'", name);
        return NULL;
    }

    return ai_provider_factory_new(type, config, error);
}
