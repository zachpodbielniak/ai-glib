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
 * ai_provider_factory_resolve_defaults:
 * @config: an #AiConfig
 * @app: (nullable): `ai` or `ai-tui`, or %NULL for library defaults
 * @provider_name: (nullable): provider name, or %NULL to consult defaults
 * @model: (nullable): explicit model, or %NULL or `default` to consult defaults
 * @out_provider: (out): resolved provider
 * @out_model: (out) (transfer full) (nullable): resolved model, or %NULL for native defaults
 * @error: (out) (optional): return location for a #GError
 *
 * A missing or empty provider name consults legacy `AI_PROVIDER` first, then
 * the scoped default provider. Explicit `default` bypasses `AI_PROVIDER`.
 * App scopes never consult library defaults or `AI_GLIB_DEFAULT_*`; missing
 * app settings mean Claude with its native model. Library scope retains its
 * programmatic and `AI_GLIB_DEFAULT_*` environment precedence.
 * An explicit model wins; otherwise the configured model is used only when
 * the selected provider matches the configured provider. An empty configured
 * model or literal `default` means the provider's native default (%NULL).
 * Invalid provider names, including an effective `AI_GLIB_DEFAULT_PROVIDER`,
 * return a configuration error rather than silently selecting Claude.
 * Programmatic config overrides retain their priority over environment values.
 * Outputs are initialized to -1 and %NULL on failure.
 *
 * Returns: %TRUE if selection succeeded, %FALSE on invalid configuration
 */
gboolean
ai_provider_factory_resolve_defaults(
	AiConfig       *config,
	const gchar    *app,
	const gchar    *provider_name,
	const gchar    *model,
	AiProviderType *out_provider,
	gchar         **out_model,
	GError        **error
){
	AiProviderType configured;
	AiProviderType selected;
	const gchar *resolved_model = model;

	g_return_val_if_fail(AI_IS_CONFIG(config), FALSE);
	g_return_val_if_fail(out_provider != NULL, FALSE);
	g_return_val_if_fail(out_model != NULL, FALSE);
	*out_provider = (AiProviderType)-1;
	*out_model = NULL;
	if (app != NULL && !g_str_equal(app, "ai") && !g_str_equal(app, "ai-tui"))
	{
		g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR, "Unknown app '%s'", app);
		return FALSE;
	}
	configured = app != NULL ? ai_config_get_app_provider(config, app) :
	                          ai_config_get_default_provider(config);
	if (g_str_equal(ai_provider_type_to_string(configured), "unknown"))
	{
		g_set_error_literal(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
		                    "Invalid configured default provider (check AI_GLIB_DEFAULT_PROVIDER)");
		return FALSE;
	}
	if (provider_name == NULL || provider_name[0] == '\0')
		provider_name = g_getenv("AI_PROVIDER");
	selected = configured;
	if (provider_name != NULL && provider_name[0] != '\0' &&
	    !g_str_equal(provider_name, "default"))
	{
		selected = ai_provider_type_from_string(provider_name);
		if (selected == AI_PROVIDER_CLAUDE &&
		    g_ascii_strcasecmp(provider_name, "claude") != 0 &&
		    g_ascii_strcasecmp(provider_name, "anthropic") != 0)
		{
			g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
			            "unknown provider '%s'", provider_name);
			return FALSE;
		}
	}
	if (model == NULL || g_str_equal(model, "default"))
	{
		resolved_model = NULL;
		if (selected == configured)
			resolved_model = app != NULL ? ai_config_get_app_model(config, app) :
			                              ai_config_get_default_model(config);
		if (resolved_model != NULL &&
		    (resolved_model[0] == '\0' || g_str_equal(resolved_model, "default")))
			resolved_model = NULL;
	}
	*out_provider = selected;
	*out_model = g_strdup(resolved_model);
	return TRUE;
}

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
        g_ascii_strcasecmp(name, "claude") != 0 &&
        g_ascii_strcasecmp(name, "anthropic") != 0)
    {
        g_set_error(error, AI_ERROR, AI_ERROR_CONFIGURATION_ERROR,
                    "unknown provider '%s'", name);
        return NULL;
    }

    return ai_provider_factory_new(type, config, error);
}
