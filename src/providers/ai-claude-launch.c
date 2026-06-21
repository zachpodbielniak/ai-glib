/*
 * ai-claude-launch.c - Ollama-as-transport launcher helpers for the
 *                      Claude Code CLI providers
 *
 * Copyright (C) 2025
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of ai-glib.
 *
 * See ai-claude-launch.h for the design rationale.
 */

#include "providers/ai-claude-launch.h"

/* The transport-selecting model prefix and its length (sans NUL). */
#define AI_CLAUDE_LAUNCH_OLLAMA_PREFIX     "ollama/"
#define AI_CLAUDE_LAUNCH_OLLAMA_PREFIX_LEN 7

gboolean
ai_claude_launch_model_is_ollama(const gchar *model)
{
    if (model == NULL)
    {
        return FALSE;
    }

    if (!g_str_has_prefix(model, AI_CLAUDE_LAUNCH_OLLAMA_PREFIX))
    {
        return FALSE;
    }

    /* Require a non-empty suffix after "ollama/". */
    return model[AI_CLAUDE_LAUNCH_OLLAMA_PREFIX_LEN] != '\0';
}

const gchar *
ai_claude_launch_ollama_model(const gchar *model)
{
    if (!ai_claude_launch_model_is_ollama(model))
    {
        return NULL;
    }

    return model + AI_CLAUDE_LAUNCH_OLLAMA_PREFIX_LEN;
}

gchar *
ai_claude_launch_executable_name(const gchar *model)
{
    const gchar *env_var;
    const gchar *env_path;
    const gchar *fallback;

    if (ai_claude_launch_model_is_ollama(model))
    {
        env_var  = "OLLAMA_PATH";
        fallback = "ollama";
    }
    else
    {
        env_var  = "CLAUDE_CODE_PATH";
        fallback = "claude";
    }

    env_path = g_getenv(env_var);
    if (env_path != NULL && env_path[0] != '\0')
    {
        return g_strdup(env_path);
    }

    return g_strdup(fallback);
}

gboolean
ai_claude_launch_should_emit_claude_model(const gchar *model)
{
    return !ai_claude_launch_model_is_ollama(model);
}

void
ai_claude_launch_emit_tokens(
    GPtrArray   *argv,
    const gchar *model,
    const gchar *program
){
    g_return_if_fail(argv != NULL);
    g_return_if_fail(program != NULL);

    /* Program position (exec'd binary, or placeholder for the base CLI
     * pipeline which overwrites argv[0] with the resolved path). */
    g_ptr_array_add(argv, g_strdup(program));

    if (ai_claude_launch_model_is_ollama(model))
    {
        /*
         * ollama launch claude --model <suffix> --
         *
         * "claude" is ollama's integration name (literal); the claude
         * args the caller appends next ride after the "--".
         */
        g_ptr_array_add(argv, g_strdup("launch"));
        g_ptr_array_add(argv, g_strdup("claude"));
        g_ptr_array_add(argv, g_strdup("--model"));
        g_ptr_array_add(argv, g_strdup(ai_claude_launch_ollama_model(model)));
        g_ptr_array_add(argv, g_strdup("--"));
    }
}
